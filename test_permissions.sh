#!/usr/bin/env bash
#
# Permission enforcement over the gRPC surface, driven through the CLI against a
# running core.
#
# WHAT THIS ASSERTS ON, AND WHY IT MATTERS: the CLI exits 0 whether an operation
# SUCCEEDED or was REFUSED — a refused mkdir, a denied read and a clean write all
# return 0. The previous version of this script tested `$?`, so it passed
# unconditionally and reported nothing; it also "expected" an unauthorized user to
# be refused a read, which is the opposite of how this system works. Every check
# below reads the ✓/✗ marker the CLI prints, which is the only place the outcome
# actually appears.
#
# The model it encodes (see the ACL design docs):
#   * READ is allowed BY DEFAULT. A user with no ACL row can read and list.
#   * A DENY row is what withholds a read, and it beats an ALLOW.
#   * WRITE is not default — it takes a grant.
#   * A grant to `role:<name>` reaches any caller presenting that role.
#
# Usage: ./test_permissions.sh          (core must be listening; defaults to :50051)
#   FE_CLI=./build/cli/fileengine_cli   the CLI to drive (NOT build_2 — that is stale)
#   FE_SERVER=localhost:50051           the core to drive it against
#   FE_TENANT=default                   the tenant to work in
#   FILEENGINE_CLI_TOKEN=…              operator credential, when the core requires
#                                       service auth (it does by default):
#                                         FILEENGINE_SERVICE_TOKEN_PEPPER=<core's> \
#                                         $FE_CLI service-token issue cli:<you>
set -u

BIN="${FE_CLI:-./build/cli/fileengine_cli}"
SRV="${FE_SERVER:-localhost:50051}"
TENANT="${FE_TENANT:-default}"

pass=0; fail=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS  $1"; pass=$((pass+1));
       else echo "  FAIL  $1 (want '$2' got '$3')"; fail=$((fail+1)); fi; }

# Run the CLI as `user` with optional roles. `as <user> <roles|-> <args...>`
as() {
    local user="$1" roles="$2"; shift 2
    if [ "$roles" = "-" ]; then
        "$BIN" -u "$user" -t "$TENANT" --server "$SRV" "$@" 2>&1
    else
        "$BIN" -u "$user" -t "$TENANT" -r "$roles" --server "$SRV" "$@" 2>&1
    fi
}

# "allowed"/"refused" from the marker on the last non-empty output line, so an
# assertion says which of the two happened rather than merely that the process
# exited.
verdict() { grep -E '^[✓✗]' | tail -1 | grep -q '^✓' && echo allowed || echo refused; }
uid_of()  { grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1; }

echo "== permission enforcement (tenant=$TENANT, server=$SRV) =="

# ---- setup -----------------------------------------------------------------
# Root-level creation is admin-only, so the fixture is built as system_admin.
# This is also the step that fails first when the core wants a service token, so
# it reports that specifically: every later assertion would otherwise fail with
# an unrelated-looking message.
ts=$(date +%s)
setup_out=$(as root system_admin mkdir "" "perm_test_$ts")
DIR=$(printf '%s' "$setup_out" | uid_of)
if [ -z "$DIR" ]; then
    echo "  ABORT  could not create the fixture directory:"
    printf '         %s\n' "$setup_out" | tail -2
    case "$setup_out" in
      *"no service token presented"*|*UNAUTHENTICATED*)
        echo "         The core requires service auth. Issue an operator credential:"
        echo "           FILEENGINE_SERVICE_TOKEN_PEPPER=<the core's pepper> \\"
        echo "             $BIN service-token issue cli:\$USER"
        echo "         then re-run with FILEENGINE_CLI_TOKEN=<the printed secret>." ;;
    esac
    exit 1
fi

printf 'content under test\n' > "/tmp/perm_body.$$"
FILE=$(as root system_admin upload "$DIR" body.txt "/tmp/perm_body.$$" | uid_of)
ck "fixture: a directory and a file exist" "yes" "$([ -n "$FILE" ] && echo yes || echo no)"
[ -n "$FILE" ] || { echo "  ABORT  no file to test against"; exit 1; }

# ---- read is allowed by default --------------------------------------------
echo "-- READ is allowed by default (no ACL row) --"
ck "a stranger has READ" "allowed" "$(as stranger - check "$FILE" stranger r | verdict)"
ck "and can list the directory" "allowed" \
   "$(as stranger - ls "$DIR" | grep -q 'Contents of directory' && echo allowed || echo refused)"

# ---- write takes a grant ---------------------------------------------------
echo "-- WRITE is not default --"
ck "a stranger has no WRITE" "refused" "$(as stranger - check "$FILE" stranger w | verdict)"
ck "granting WRITE to alice" "allowed" "$(as root system_admin grant "$FILE" alice w | verdict)"
ck "alice now has WRITE" "allowed" "$(as alice - check "$FILE" alice w | verdict)"
ck "and can write the bytes" "allowed" \
   "$(as alice - put "$FILE" "/tmp/perm_body.$$" | verdict)"

# ---- deny withholds a read, and beats allow --------------------------------
echo "-- a DENY row is what withholds a read --"
ck "granting alice an explicit ALLOW READ" "allowed" \
   "$(as root system_admin grant "$FILE" alice r | verdict)"
ck "adding a DENY READ for alice" "allowed" \
   "$(as root system_admin -e deny grant "$FILE" alice r | verdict)"
ck "DENY beats the ALLOW she also holds" "refused" "$(as alice - check "$FILE" alice r | verdict)"
ck "and the read itself is refused" "refused" \
   "$(as alice - get "$FILE" "/tmp/perm_dl.$$" | verdict)"
ck "removing the DENY restores the read" "allowed" \
   "$(as root system_admin -e deny revoke "$FILE" alice r >/dev/null; as alice - check "$FILE" alice r | verdict)"

# ---- a grant to a role reaches whoever presents it -------------------------
echo "-- role grants --"
ck "granting WRITE to role:editor" "allowed" \
   "$(as root system_admin grant "$FILE" "role:editor" w | verdict)"
ck "a caller presenting 'editor' has WRITE" "allowed" \
   "$(as edna editor check "$FILE" edna w | verdict)"
ck "the same caller WITHOUT the role does not" "refused" \
   "$(as edna - check "$FILE" edna w | verdict)"

# ---- revocation ------------------------------------------------------------
echo "-- revocation --"
ck "revoking alice's WRITE" "allowed" "$(as root system_admin revoke "$FILE" alice w | verdict)"
ck "alice has no WRITE" "refused" "$(as alice - check "$FILE" alice w | verdict)"
ck "while her READ is untouched" "allowed" "$(as alice - check "$FILE" alice r | verdict)"

# ---- cleanup ---------------------------------------------------------------
as root system_admin rm "$FILE" >/dev/null
as root system_admin rm "$DIR"  >/dev/null
rm -f "/tmp/perm_body.$$" "/tmp/perm_dl.$$"

echo "== results: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
