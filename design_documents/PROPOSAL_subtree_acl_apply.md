# Proposal: apply an ACL change to a subtree in the core

**Status:** Draft — for review, nothing implemented
**Scope (cross-repo):** `file_engine_core` (RPC + ACL manager + SQL);
`http_bridge` (drop the client-side walk); `frontend` (unchanged surface);
`python_interface` / `javascript_interface` (regenerated stubs)
**Related:** `PROPOSAL_read_path_performance.md` — the enumeration half of this
problem is the same folder-mtime cost described there.

---

## The problem

Applying an ACL to a large tree does not complete. It is not slow; it stops.

`http_bridge` implements "recursive" ACL application by walking the tree itself:

```cpp
collectDescendants(uid, id, nodes, 0);   // N listDirectory RPCs, recursive
for (const auto& n : nodes) {            // then one RPC per node
    grantOne(n);
}
// the existing comment: "not atomic — a mid-walk failure leaves a
// partial cascade, safe to re-run."
```

Two fan-outs multiply:

1. **Per node.** `GrantPermissionRequest` carries a single `resource_uid`. There
   is no subtree form.
2. **Per permission bit.** The request carries `Permission permission = 3` — one
   enum. The core's own `grant_permission` takes a *bitmask* and the creator
   grant sets eleven bits in one call, but that shape is not reachable over
   gRPC. Every caller therefore loops: the tenant provisioning play issues
   eleven grants for a single folder.

"Full control for a principal on a 50,000-node tree" is **50,000 × 11 ≈ 550,000
RPCs**, preceded by a recursive enumeration that is itself expensive — each
`listDirectory` pays the folder-mtime subtree walk for every child directory
(see the companion proposal). All inside one synchronous HTTP request, against
an edge with a 60-second timeout.

So it times out, and because the walk is not transactional, it leaves **a tree
in a half-applied state** with no record of where it stopped.

### Why this cannot be fixed in the client

A client can batch, parallelise, or resume, and all of it is working around the
absence of a set operation. The database can express "insert a row for every
descendant" in one statement; every RPC-level optimisation is an attempt to
approximate that from outside.

### Why inheritance does not already solve it

`ACL_INHERIT` cascades — but **at creation time**. `apply_inherited_acls` copies
the parent's inheritable rules onto each new child as it is created, preserving
the bit so it reaches grandchildren.

It says nothing about resources that already exist. Changing a folder's ACL
today does not reach a single existing descendant, which is precisely why the
SPA offers a recursive checkbox and why the bridge walks.

---

## Options

### A. A subtree form of the grant/revoke RPCs (recommended)

Add to `GrantPermissionRequest` / `RevokePermissionRequest`:

```protobuf
bool  recursive       = 6;   // apply to every descendant of resource_uid
int32 permission_mask = 7;   // OR of Permission bits; supersedes `permission`
```

Implemented in the ACL manager as one statement per tenant schema:

```sql
INSERT INTO acls (resource_uid, principal, principal_type, permissions, effect)
SELECT f.uid, $principal, $ptype, $mask, $effect
  FROM <recursive subtree CTE rooted at $1> f
ON CONFLICT (resource_uid, principal, principal_type)
DO UPDATE SET permissions = acls.permissions | EXCLUDED.permissions;
```

One round trip, one transaction, one index-driven pass. It also fixes the
per-bit fan-out for *non*-recursive callers, which is the cheaper half and
benefits every existing caller including tenant provisioning.

**Estimated effect:** 550,000 RPCs and an unbounded wall-clock become one RPC
and one statement whose cost is proportional to subtree size, executed inside
the database rather than across a network.

### B. Evaluate inheritance at check time instead of materialising it

Stop copying rules to children. Have `check_permission` consult ancestors'
inheritable rules directly — the traversal already exists in
`ancestors_readable`.

A later ACL change would then apply to the whole subtree instantly, with **no
write fan-out at all**, and the class of problem disappears rather than getting
faster.

The costs are real: every check gains an ancestor walk (O(depth), cheap, but on
the hottest path in the system); existing materialised rows must be reconciled
or a precedence rule defined; and "break the chain by revoking the bit on a
child" becomes "an explicit override on the child", which is a different mental
model for anyone already relying on the current one.

### C. Keep materialisation, add the set operation

A, plus B's evaluation-time model considered separately and later, on its own
evidence. This is what the recommendation below assumes.

---

## The part that needs deciding before anything is written

### Audit granularity

Every grant emits an audit event today (`emit_permission_audit`,
`grpc_service.cpp:1617`). A subtree apply over 50,000 nodes × 11 bits would emit
**550,000 audit events**, into a chain that is deliberately serialised by a
per-chain advisory lock. That is a second fan-out wearing the first one's
clothes, and it would be slower than the thing it replaced.

The proposal is **one event for the operation**: root uid, principal, mask,
effect, recursive=true, and the number of nodes affected. That is a more useful
record than 550,000 rows anyway — "who was granted what, where, and how widely"
is the auditable fact; the per-node expansion is derivable.

There is precedent for not spamming the record:
`AccountabilityMode::PartOfCreation` exists so that propagating an inherited
rule to a new child does not generate accountability noise.

**This needs explicit sign-off**, because it changes what a security review can
reconstruct: after this, "was node X granted to principal P" is answered by the
ACL table plus one subtree event, not by a per-node audit trail.

### Authorisation

Require `MANAGE_ACL` **on the root**, and document that it applies to every
descendant. Checking it per node would reintroduce the fan-out inside the core,
and a caller who may manage a folder's ACL can already reach its contents.

The sharp edge: a subtree may contain nodes the caller cannot currently manage
individually. Applying anyway is the intended behaviour — it is what "apply to
everything in this folder" means — but it should be stated rather than
discovered.

### Very large trees

One transaction over a million nodes is a long lock and a large WAL burst.
Chunk by a cursor over the subtree, committing per chunk, with the operation
resumable and idempotent (`ON CONFLICT DO UPDATE` already is).

That trades atomicity for progress — the same trade the current implementation
makes accidentally, but bounded, resumable, and reported.

### Revoke is not symmetric

Granting adds bits; revoking removes them, and a revoke that empties a rule
should delete the row rather than leave `permissions = 0`. Worth specifying,
because an empty ALLOW row and an absent row read differently in a system that
is read-by-default.

---

## Recommendation

1. **Add `permission_mask`** to grant and revoke. Small, no semantic change,
   and it removes a factor of eleven from every caller today — including the
   non-recursive ones.
2. **Add `recursive`**, implemented as one SQL statement, with a single summary
   audit event and `MANAGE_ACL` checked on the root.
3. **Delete the walk in `http_bridge`.** It becomes a passthrough; the SPA's
   checkbox keeps working unchanged.
4. **Chunked, resumable execution** once 2 is proven on a real tree — not
   before, since the simple form may be sufficient at the sizes this deployment
   actually sees.
5. Revisit **B** separately. It is the better model and it deserves its own
   evidence rather than being smuggled in behind a performance fix.

Items 1–3 are one core release. Landing them alongside the folder-mtime work in
`PROPOSAL_read_path_performance.md` means one RPM rebuild and one deploy window
rather than two.

---

## Appendix: how this was established

```bash
# the bridge's recursive apply
sed -n '1225,1300p' http_bridge/src/http_server.cpp        # collectDescendants + per-node grantOne

# the request shape
grep -A8 'message GrantPermissionRequest' file_engine_core/proto/fileservice.proto

# inheritance is at creation, not evaluation
grep -n 'ACL_INHERIT' file_engine_core/core/src/acl_manager.cpp   # apply_inherited_acls
```
