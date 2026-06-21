#!/bin/bash
# Full CLI-driven integration test against a RUNNING fileengine server.
# Drives build_2/cli/fileengine_cli over gRPC (default localhost:50051) and
# exercises every CLI command group: filesystem (mkdir/touch/upload/get/stat/
# exists/ls/lsd/rename/copy/move/del/undelete/rm, incl. recursive dir copy and
# the copy/move-into-own-subtree guard), root-UUID aliasing (""/all-zeros),
# versioning (versions/getversion/download/restore/purge), metadata (set/get/
# all/del + versioned reads), permissions/ACL (r/w/x/d/m, ALLOW+DENY precedence,
# role principals), role management, and diagnostics. Per-step PASS/FAIL summary.
#
# Assumes the server is already up (e.g. the Docker compose stack).
# Usage: ./cli_full_integration.sh [server_address]

CLI="${CLI:-$(dirname "$0")/../build_2/cli/fileengine_cli}"
SERVER="${1:-localhost:50051}"
ADMIN=(-u root -r system_admin --server "$SERVER")
SUF="it_$$_$RANDOM"
WORK=/tmp/fe_cli_it_$$
mkdir -p "$WORK"

PASS=0; FAIL=0; XFAIL=0; FAILED_STEPS=(); XFAIL_STEPS=()
UID_RE='[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}'

# pass <name> if last output indicates success
ok()   { PASS=$((PASS+1)); printf '  \033[32m✓\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); FAILED_STEPS+=("$1"); printf '  \033[31m✗\033[0m %s\n     %s\n' "$1" "${2:-}"; }
# known unimplemented server feature — reported, but not a regression
xfail() { XFAIL=$((XFAIL+1)); XFAIL_STEPS+=("$1: $2"); printf '  \033[33m⚠\033[0m %s (XFAIL: %s)\n' "$1" "$2"; }
# expect a "not implemented/supported" response, else treat as pass/fail normally
expect_unimpl() { if grep -qiE 'not (fully )?(implemented|supported)' <<<"$2"; then xfail "$1" "server reports unimplemented"; else assert_ok "$1" "$2"; fi; }

# assert_contains <name> <expected-substring> <actual>
assert_contains() { if grep -qF "$2" <<<"$3"; then ok "$1"; else bad "$1" "expected '$2' in: $(head -3 <<<"$3" | tr '\n' '|')"; fi; }
# assert_ok <name> <output>  -> success if no failure markers
assert_ok() { if grep -qE '✗|Failed|Error|error' <<<"$2"; then bad "$1" "$(grep -E '✗|Failed|Error|error' <<<"$2" | head -1)"; else ok "$1"; fi; }
extract_uid() { grep -oE "$UID_RE" | head -1; }
# permission-check helpers. CLI 'check' prints:
#   "✓ User 'x' has READ permission ..."  or  "✗ User 'x' does not have READ ..."
# NOTE: the 'check' command evaluates the ACTING identity (-u/-r); the positional
# <user> argument is cosmetic, so callers must authenticate as the principal.
assert_perm()   { if grep -q "has $2 permission" <<<"$3"; then ok "$1"; else bad "$1" "$(tail -1 <<<"$3")"; fi; }
assert_noperm() { if grep -q "does not have" <<<"$2"; then ok "$1"; else bad "$1" "unexpected grant: $(tail -1 <<<"$2")"; fi; }

echo "=========================================================="
echo " FileEngine CLI full integration test"
echo " server=$SERVER  cli=$CLI"
echo "=========================================================="

# ---- 0. connectivity ----
echo "[0] Connectivity"
out=$("$CLI" "${ADMIN[@]}" usage 2>&1);            assert_contains "usage/storage stats" "Storage Usage" "$out"
out=$("$CLI" "${ADMIN[@]}" ls "" 2>&1);            assert_ok "ls root (empty string)" "$out"
# root may be referenced as the empty string OR the all-zeros UUID
zout=$("$CLI" "${ADMIN[@]}" ls "00000000-0000-0000-0000-000000000000" 2>&1)
assert_ok "ls root (all-zeros UUID)" "$zout"
if [ "$(grep -c '\[' <<<"$out")" = "$(grep -c '\[' <<<"$zout")" ]; then ok "root aliases list identically"; else bad "root aliases list identically" "entry counts differ"; fi
# creating under the all-zeros root must land in the same root as the empty string
out=$("$CLI" "${ADMIN[@]}" mkdir "00000000-0000-0000-0000-000000000000" "rootalias_$SUF" 2>&1)
ZDIR=$(extract_uid <<<"$out")
out=$("$CLI" "${ADMIN[@]}" ls "" 2>&1)
if grep -qF "rootalias_$SUF" <<<"$out"; then ok "mkdir via all-zeros root visible at root"; else bad "mkdir via all-zeros root visible at root" "$out"; fi
[ -n "$ZDIR" ] && "$CLI" "${ADMIN[@]}" rm "$ZDIR" >/dev/null 2>&1

# ---- 1. filesystem ops ----
echo "[1] Filesystem operations"
out=$("$CLI" "${ADMIN[@]}" mkdir "" "work_$SUF" 2>&1)
DIR=$(extract_uid <<<"$out"); [ -n "$DIR" ] && ok "mkdir workspace ($DIR)" || bad "mkdir workspace" "$out"

out=$("$CLI" "${ADMIN[@]}" mkdir "$DIR" sub 2>&1)
SUB=$(extract_uid <<<"$out"); [ -n "$SUB" ] && ok "mkdir subdir" || bad "mkdir subdir" "$out"
out=$("$CLI" "${ADMIN[@]}" mkdir "$DIR" sub2 2>&1)
SUB2=$(extract_uid <<<"$out"); [ -n "$SUB2" ] && ok "mkdir subdir2" || bad "mkdir subdir2" "$out"

echo "hello world content $SUF" > "$WORK/src.txt"
out=$("$CLI" "${ADMIN[@]}" upload "$DIR" file.txt "$WORK/src.txt" 2>&1)
# upload prints the PARENT uid first ("to parent 'X'") then the new file uid
# ("Created file with UID: Y") — parse the file line specifically.
FILE=$(grep -i 'Created file with UID' <<<"$out" | extract_uid)
[ -n "$FILE" ] && ok "upload file ($FILE)" || bad "upload file" "$out"

out=$("$CLI" "${ADMIN[@]}" exists "$FILE" 2>&1);   assert_ok "exists file" "$out"
out=$("$CLI" "${ADMIN[@]}" stat "$FILE" 2>&1);     assert_ok "stat file" "$out"
out=$("$CLI" "${ADMIN[@]}" stat "$DIR" 2>&1);      assert_ok "stat directory" "$out"

# touch: create an empty file directly (distinct from upload)
out=$("$CLI" "${ADMIN[@]}" touch "$DIR" empty.txt 2>&1)
EMPTY=$(extract_uid <<<"$out"); [ -n "$EMPTY" ] && ok "touch empty file" || bad "touch empty file" "$out"
out=$("$CLI" "${ADMIN[@]}" exists "$EMPTY" 2>&1);  assert_ok "exists touched file" "$out"
# exists must report absence (not error out) for a bogus UID
out=$("$CLI" "${ADMIN[@]}" exists "deadbeef-0000-0000-0000-000000000000" 2>&1)
if grep -qiF "does not exist" <<<"$out"; then ok "exists reports absent uid"; else bad "exists reports absent uid" "$out"; fi

"$CLI" "${ADMIN[@]}" get "$FILE" "$WORK/out.txt" >/dev/null 2>&1
if diff -q "$WORK/src.txt" "$WORK/out.txt" >/dev/null 2>&1; then ok "get roundtrip (content matches)"; else bad "get roundtrip" "content mismatch"; fi

out=$("$CLI" "${ADMIN[@]}" ls "$DIR" 2>&1);        assert_contains "ls shows file" "file.txt" "$out"
out=$("$CLI" "${ADMIN[@]}" rename "$FILE" renamed.txt 2>&1); assert_ok "rename file" "$out"
out=$("$CLI" "${ADMIN[@]}" ls "$DIR" 2>&1);        assert_contains "ls shows renamed" "renamed.txt" "$out"
out=$("$CLI" "${ADMIN[@]}" copy "$FILE" "$SUB" 2>&1);        assert_ok "copy file to subdir" "$out"
out=$("$CLI" "${ADMIN[@]}" ls "$SUB" 2>&1);        assert_contains "ls subdir after copy" "renamed.txt" "$out"
out=$("$CLI" "${ADMIN[@]}" move "$FILE" "$SUB2" 2>&1);       assert_ok "move file to subdir2" "$out"
out=$("$CLI" "${ADMIN[@]}" ls "$SUB2" 2>&1);       assert_contains "ls subdir2 after move" "renamed.txt" "$out"

# regression: copying/moving a directory into its own subtree must not crash
# the server (previously SIGSEGV via unbounded recursive copy).
out=$("$CLI" "${ADMIN[@]}" copy "$DIR" "$SUB" 2>&1)
assert_contains "copy dir into own subtree rejected" "itself or its own subtree" "$out"
out=$("$CLI" "${ADMIN[@]}" move "$DIR" "$SUB" 2>&1)
assert_contains "move dir into own subtree rejected" "itself or its own subtree" "$out"

# valid recursive directory copy — the legitimate form of the operation that
# crashed when the destination was inside the source. Copy a populated dir into
# an unrelated dir and confirm contents are copied recursively.
RCSRC=$("$CLI" "${ADMIN[@]}" mkdir "$DIR" rcsrc 2>&1 | extract_uid)
"$CLI" "${ADMIN[@]}" upload "$RCSRC" inner.txt "$WORK/src.txt" >/dev/null 2>&1
out=$("$CLI" "${ADMIN[@]}" copy "$RCSRC" "$SUB" 2>&1);  assert_ok "recursive dir copy" "$out"
out=$("$CLI" "${ADMIN[@]}" ls "$SUB" 2>&1);             assert_contains "copied dir appears in dest" "rcsrc" "$out"
RCCOPY=$("$CLI" "${ADMIN[@]}" ls "$SUB" 2>&1 | grep -F rcsrc | extract_uid)
out=$("$CLI" "${ADMIN[@]}" ls "$RCCOPY" 2>&1);          assert_contains "recursive copy preserved contents" "inner.txt" "$out"

# soft delete / undelete  (file currently lives in SUB2 after the move above)
out=$("$CLI" "${ADMIN[@]}" del "$FILE" 2>&1);      assert_ok "soft delete" "$out"
out=$("$CLI" "${ADMIN[@]}" ls "$SUB2" 2>&1)
if grep -qF "renamed.txt" <<<"$out"; then bad "ls hides deleted" "still listed"; else ok "ls hides deleted"; fi
out=$("$CLI" "${ADMIN[@]}" lsd "$SUB2" 2>&1);      assert_contains "lsd shows deleted" "renamed.txt" "$out"
out=$("$CLI" "${ADMIN[@]}" undelete "$FILE" 2>&1); assert_ok "undelete" "$out"
out=$("$CLI" "${ADMIN[@]}" ls "$SUB2" 2>&1);       assert_contains "ls shows undeleted" "renamed.txt" "$out"

# ---- 2. versioning ----
echo "[2] Versioning"
echo "version-2 body $SUF" > "$WORK/v2.txt"
echo "version-3 body $SUF" > "$WORK/v3.txt"
"$CLI" "${ADMIN[@]}" put "$FILE" "$WORK/v2.txt" >/dev/null 2>&1
"$CLI" "${ADMIN[@]}" put "$FILE" "$WORK/v3.txt" >/dev/null 2>&1
out=$("$CLI" "${ADMIN[@]}" versions "$FILE" 2>&1)
# version timestamps are formatted YYYYMMDD_HHMMSS.mmm
vcount=$(grep -coE '[0-9]{8}_[0-9]{6}' <<<"$out")
if [ "$vcount" -ge 1 ]; then ok "versions lists history ($vcount entries)"; else bad "versions list" "$out"; fi
TS=$(grep -oE '[0-9]{8}_[0-9]{6}(\.[0-9]+)?' <<<"$out" | head -1)
if [ -n "$TS" ]; then
  out=$("$CLI" "${ADMIN[@]}" getversion "$FILE" "$TS" "$WORK/oldver.txt" 2>&1); assert_ok "getversion" "$out"
  out=$("$CLI" "${ADMIN[@]}" download "$FILE" "$WORK/dl_cur.txt" 2>&1);         assert_ok "download (current)" "$out"
  out=$("$CLI" "${ADMIN[@]}" download "$FILE" "$WORK/dl_ver.txt" "$TS" 2>&1);   assert_ok "download (specific version)" "$out"
  out=$("$CLI" "${ADMIN[@]}" restore "$FILE" "$TS" 2>&1);                       assert_ok "restore to version" "$out"
else
  bad "getversion/restore" "no numeric timestamp parsed from: $(head -3 <<<"$out" | tr '\n' '|')"
fi

# ---- 3. metadata ----
echo "[3] Metadata"
out=$("$CLI" "${ADMIN[@]}" setmeta "$FILE" color blue 2>&1);  assert_ok "setmeta" "$out"
out=$("$CLI" "${ADMIN[@]}" getmeta "$FILE" color 2>&1);       assert_contains "getmeta value" "blue" "$out"
"$CLI" "${ADMIN[@]}" setmeta "$FILE" size large >/dev/null 2>&1
out=$("$CLI" "${ADMIN[@]}" allmeta "$FILE" 2>&1);             assert_contains "allmeta lists keys" "color" "$out"
# versioned metadata reads (metadata is stored under the "current" version)
out=$("$CLI" "${ADMIN[@]}" allmetaversion "$FILE" current 2>&1);   assert_contains "allmetaversion (current)" "color" "$out"
out=$("$CLI" "${ADMIN[@]}" getmetaversion "$FILE" current color 2>&1); assert_contains "getmetaversion (current)" "blue" "$out"

out=$("$CLI" "${ADMIN[@]}" delmeta "$FILE" color 2>&1);       assert_ok "delmeta" "$out"
out=$("$CLI" "${ADMIN[@]}" getmeta "$FILE" color 2>&1)
if grep -qE 'blue' <<<"$out"; then bad "delmeta removed key" "still returns blue"; else ok "delmeta removed key"; fi

# ---- 4. permissions / ACL ----
echo "[4] Permissions / ACL"
out=$("$CLI" "${ADMIN[@]}" grant "$FILE" alice r 2>&1);       assert_ok "grant READ to alice" "$out"
out=$("$CLI" "${ADMIN[@]}" grant "$FILE" alice w 2>&1);       assert_ok "grant WRITE to alice" "$out"
out=$("$CLI" -u alice --server "$SERVER" check "$FILE" alice r 2>&1);  assert_perm "alice has READ" READ "$out"
out=$("$CLI" "${ADMIN[@]}" revoke "$FILE" alice w 2>&1);      assert_ok "revoke WRITE from alice" "$out"
out=$("$CLI" -u alice --server "$SERVER" check "$FILE" alice w 2>&1);  assert_noperm "alice WRITE revoked" "$out"
# DENY rule
out=$("$CLI" "${ADMIN[@]}" -e deny grant "$FILE" bob r 2>&1); assert_ok "grant DENY READ to bob" "$out"
out=$("$CLI" -u bob --server "$SERVER" check "$FILE" bob r 2>&1);      assert_noperm "bob denied READ" "$out"
# unauthorized user cannot read content
"$CLI" -u mallory --server "$SERVER" get "$FILE" "$WORK/mallory.txt" >/dev/null 2>&1
out=$("$CLI" -u mallory --server "$SERVER" check "$FILE" mallory r 2>&1); assert_noperm "mallory has no access" "$out"

# additional permission letters beyond r/w
out=$("$CLI" "${ADMIN[@]}" grant "$FILE" dave d 2>&1);                    assert_ok "grant DELETE(d) to dave" "$out"
out=$("$CLI" -u dave --server "$SERVER" check "$FILE" dave d 2>&1);       assert_perm "dave has DELETE" DELETE "$out"
out=$("$CLI" "${ADMIN[@]}" grant "$FILE" dave x 2>&1);                    assert_ok "grant EXECUTE(x) to dave" "$out"
out=$("$CLI" -u dave --server "$SERVER" check "$FILE" dave x 2>&1);       assert_perm "dave has EXECUTE" EXECUTE "$out"
out=$("$CLI" "${ADMIN[@]}" grant "$FILE" dave m 2>&1);                    assert_ok "grant MANAGE_ACL(m) to dave" "$out"
out=$("$CLI" -u dave --server "$SERVER" check "$FILE" dave m 2>&1);       assert_perm "dave has MANAGE_ACL" MANAGE_ACL "$out"

# DENY precedence then deny-revoke restores access: a DENY beats an ALLOW, and
# removing the DENY (via -e deny revoke) lets the ALLOW take effect again.
"$CLI" "${ADMIN[@]}" grant "$FILE" erin r >/dev/null 2>&1            # ALLOW read
"$CLI" "${ADMIN[@]}" -e deny grant "$FILE" erin r >/dev/null 2>&1   # DENY read
out=$("$CLI" -u erin --server "$SERVER" check "$FILE" erin r 2>&1);       assert_noperm "DENY overrides ALLOW" "$out"
"$CLI" "${ADMIN[@]}" -e deny revoke "$FILE" erin r >/dev/null 2>&1  # remove DENY
out=$("$CLI" -u erin --server "$SERVER" check "$FILE" erin r 2>&1);       assert_perm "access restored after deny-revoke" READ "$out"

# ---- 5. roles ----
echo "[5] Role management"
ROLE="editors_$SUF"
out=$("$CLI" "${ADMIN[@]}" create_role "$ROLE" 2>&1);        assert_ok "create_role" "$out"
out=$("$CLI" "${ADMIN[@]}" list_all_roles 2>&1);            assert_contains "list_all_roles shows new role" "$ROLE" "$out"
out=$("$CLI" "${ADMIN[@]}" assign_role carol "$ROLE" 2>&1); assert_ok "assign_role carol" "$out"
out=$("$CLI" "${ADMIN[@]}" list_roles carol 2>&1);          assert_contains "list_roles shows role" "$ROLE" "$out"
out=$("$CLI" "${ADMIN[@]}" list_users "$ROLE" 2>&1);        assert_contains "list_users shows carol" "carol" "$out"
# role-based grant + check
out=$("$CLI" "${ADMIN[@]}" grant "$FILE" "role:$ROLE" r 2>&1); assert_ok "grant READ to role" "$out"
out=$("$CLI" -u carol -r "$ROLE" --server "$SERVER" check "$FILE" carol r 2>&1); assert_perm "carol(role) has READ" READ "$out"
out=$("$CLI" "${ADMIN[@]}" remove_role carol "$ROLE" 2>&1); assert_ok "remove_role carol" "$out"
out=$("$CLI" "${ADMIN[@]}" list_roles carol 2>&1)
if grep -qF "$ROLE" <<<"$out"; then bad "remove_role took effect" "role still listed"; else ok "remove_role took effect"; fi
out=$("$CLI" "${ADMIN[@]}" delete_role "$ROLE" 2>&1);       assert_ok "delete_role" "$out"

# ---- 6. diagnostics ----
echo "[6] Diagnostics"
out=$("$CLI" "${ADMIN[@]}" sync 2>&1);                      assert_ok "sync trigger" "$out"
out=$("$CLI" "${ADMIN[@]}" purge "$FILE" 1 2>&1);           assert_ok "purge old versions (keep 1)" "$out"
out=$("$CLI" "${ADMIN[@]}" usage 2>&1);                     assert_contains "usage after ops" "Storage Usage" "$out"

# ---- cleanup ----
echo "[cleanup] removing test workspace"
for u in "$FILE" "$EMPTY" "$RCSRC" "$SUB" "$SUB2" "$DIR"; do
  [ -n "$u" ] && "$CLI" "${ADMIN[@]}" rm "$u" >/dev/null 2>&1
done
rm -rf "$WORK"

echo "=========================================================="
echo " RESULTS:  PASS=$PASS  FAIL=$FAIL  XFAIL=$XFAIL"
if [ "$XFAIL" -gt 0 ]; then
  echo " Known-unimplemented (XFAIL):"; printf '   - %s\n' "${XFAIL_STEPS[@]}"
fi
if [ "$FAIL" -gt 0 ]; then
  echo " Failed steps:"; printf '   - %s\n' "${FAILED_STEPS[@]}"
fi
echo "=========================================================="
[ "$FAIL" -eq 0 ]
