#!/usr/bin/env bash
#
# Role administration and role-borne permissions over the gRPC surface, driven
# through the CLI against a running core.
#
# Three things the previous version of this script got wrong, all of which made it
# report success regardless of what the core did:
#
#   * It tested `$?`. The CLI exits 0 whether an operation succeeded or was
#     REFUSED, so every check passed unconditionally. Assertions here read the
#     ✓/✗ marker the CLI prints instead.
#   * It started its own `fileengine_server` in the background and killed the pid
#     afterwards. With a core already on :50051 the new one cannot bind, the
#     script silently drove the EXISTING core, and the teardown printed
#     "kill: No such process". It now requires a running core and says so.
#   * It created resources as a plain user at the root, which the core no longer
#     permits — root-level creation is admin-only.
#
# Role membership note: the `assign_role`/`list_roles` RPCs write the core's own
# `user_roles` table. In a deployed system roles arrive with the REQUEST from the
# directory, so that table is normally empty and this is the only surface that
# exercises it — worth keeping, and worth not mistaking for how production
# resolves a caller's roles.
#
# Usage: ./test_acl_roles.sh            (core must be listening; defaults to :50051)
#   FE_CLI / FE_SERVER / FE_TENANT / FILEENGINE_CLI_TOKEN — as in test_permissions.sh
set -u

BIN="${FE_CLI:-./build/cli/fileengine_cli}"
SRV="${FE_SERVER:-localhost:50051}"
TENANT="${FE_TENANT:-default}"

pass=0; fail=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS  $1"; pass=$((pass+1));
       else echo "  FAIL  $1 (want '$2' got '$3')"; fail=$((fail+1)); fi; }

as() {
    local user="$1" roles="$2"; shift 2
    if [ "$roles" = "-" ]; then
        "$BIN" -u "$user" -t "$TENANT" --server "$SRV" "$@" 2>&1
    else
        "$BIN" -u "$user" -t "$TENANT" -r "$roles" --server "$SRV" "$@" 2>&1
    fi
}
verdict() { grep -E '^[✓✗]' | tail -1 | grep -q '^✓' && echo allowed || echo refused; }
uid_of()  { grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1; }

echo "== ACL + role administration (tenant=$TENANT, server=$SRV) =="

ts=$(date +%s)
ROLE="editors_$ts"
setup_out=$(as admin_user system_admin mkdir "" "aclrole_test_$ts")
DIR=$(printf '%s' "$setup_out" | uid_of)
if [ -z "$DIR" ]; then
    echo "  ABORT  could not create the fixture directory:"
    printf '         %s\n' "$setup_out" | tail -2
    case "$setup_out" in
      *"no service token presented"*|*UNAUTHENTICATED*)
        echo "         The core requires service auth — issue an operator credential:"
        echo "           FILEENGINE_SERVICE_TOKEN_PEPPER=<the core's pepper> \\"
        echo "             $BIN service-token issue cli:\$USER"
        echo "         then re-run with FILEENGINE_CLI_TOKEN=<the printed secret>." ;;
      *"Connection refused"*|*UNAVAILABLE*)
        echo "         No core is listening on $SRV. Start one (this script no longer"
        echo "         starts its own) and re-run." ;;
    esac
    exit 1
fi

# ---- role lifecycle --------------------------------------------------------
echo "-- role lifecycle --"
ck "an admin creates a role" "allowed" "$(as admin_user system_admin create_role "$ROLE" | verdict)"
# Creating an existing role SUCCEEDS — create_role is idempotent, which is what
# lets a provisioning step run twice without a special case. What must not happen
# is a second catalog entry.
ck "creating it again succeeds (idempotent)" "allowed" \
   "$(as admin_user system_admin create_role "$ROLE" | verdict)"
ck "and the catalog holds it exactly once" "1" \
   "$(as admin_user system_admin list_all_roles | grep -c -- "  - $ROLE\$")"

echo "-- membership (core user_roles, not the directory) --"
ck "assigning a user to the role" "allowed" \
   "$(as admin_user system_admin assign_role member_user "$ROLE" | verdict)"
ck "the assignment is readable back" "yes" \
   "$(as admin_user system_admin list_roles member_user | grep -q -- "$ROLE" && echo yes || echo no)"
ck "removing the membership" "allowed" \
   "$(as admin_user system_admin remove_role member_user "$ROLE" | verdict)"
ck "and it is gone" "no" \
   "$(as admin_user system_admin list_roles member_user | grep -q -- "$ROLE" && echo yes || echo no)"

# ---- a role-borne grant ----------------------------------------------------
# WRITE, not READ: read is allowed by default here, so granting READ to a role
# and finding the caller can read proves nothing about the role.
echo "-- a grant to a role is honoured for whoever presents it --"
ck "granting WRITE to the role on the directory" "allowed" \
   "$(as admin_user system_admin grant "$DIR" "role:$ROLE" w | verdict)"
ck "a caller presenting the role has WRITE" "allowed" \
   "$(as rolled "$ROLE" check "$DIR" rolled w | verdict)"
ck "the same caller without it does not" "refused" \
   "$(as rolled - check "$DIR" rolled w | verdict)"
ck "a caller presenting an UNRELATED role does not" "refused" \
   "$(as rolled "not_$ROLE" check "$DIR" rolled w | verdict)"
ck "revoking the role's grant withdraws it" "refused" \
   "$(as admin_user system_admin revoke "$DIR" "role:$ROLE" w >/dev/null; as rolled "$ROLE" check "$DIR" rolled w | verdict)"

# ---- who may administer roles, at THIS surface -----------------------------
# The core does NOT gate role administration on the caller's roles, and that is
# the architecture rather than an oversight: the gRPC surface is trusted-upstream,
# reachable only by services holding a credential, and each DOOR (http_bridge,
# WebDAV, MCP) is what admits or refuses a person. Asserting a refusal here would
# be asserting a gate that deliberately lives elsewhere — and would fail, which is
# how this script came to claim a gate that does not exist.
#
# The gate that MUST exist is the bridge's, and it is a known gap:
# http_bridge/tests/test_e2e_security.sh carries it as finding C1
# (EXPECT-FAIL-UNTIL-FIX) for /v1/roles. Keep the two in step — when C1 is fixed,
# the bridge test flips to a pass and this one still describes the core.
echo "-- role administration at the gRPC surface is trusted-upstream --"
ck "a caller presenting no roles may create one (the door is the gate, not the core)" "allowed" \
   "$(as plain_user - create_role "plain_$ts" | verdict)"
# Clean it up. Roles are catalog state: the earlier version of this script left
# every role it created behind, which is why dev catalogs accumulate names like
# "pwn" from the bridge's security suite.
as admin_user system_admin delete_role "plain_$ts" >/dev/null
ck "and the test removes what it created" "no" \
   "$(as admin_user system_admin list_all_roles | grep -q -- "plain_$ts" && echo yes || echo no)"

# ---- cleanup ---------------------------------------------------------------
as admin_user system_admin delete_role "$ROLE" >/dev/null
ck "the role is deleted on the way out" "no" \
   "$(as admin_user system_admin list_all_roles | grep -q -- "$ROLE" && echo yes || echo no)"
as admin_user system_admin rm "$DIR" >/dev/null

echo "== results: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
