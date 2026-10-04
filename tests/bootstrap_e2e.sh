#!/bin/bash
# End-to-end: the one-shot enrolment socket opens ONLY on a system that has
# never had a credential, against the real fileengine_server + fileengine_cli.
#
# Why this exists: production ran with twelve credentials and no bootstrap
# marker (credentials are issued with `fileengine_cli service-token issue`,
# which never wrote it), so every core start tried to open the unauthenticated
# enrolment socket. It failed to bind only because /run/fileengine is absent in
# the image — the error was the only symptom.
#
# Scenarios, each a fresh server start against a THROWAWAY database:
#   1. fresh system            -> the socket opens and enrolment works once
#   2. production shape        -> creds, marker deleted: backfilled at start,
#                                 recorded, socket never attempted
#   3. second start            -> no second backfill
#   4. credential map emptied  -> the marker keeps the socket shut
#   5. issue on a fresh system -> a direct issue closes bootstrap by itself
#
# The database is DROPPED and recreated, so it must not be one anything uses.
#
# Usage: tests/bootstrap_e2e.sh [build_dir]     (default: build_bootstrap)
#   FE_TEST_PG_HOST/PORT/USER/PASSWORD   default localhost:5434 postgres/postgres
#   FE_BOOTSTRAP_E2E_DB                  default fe_bootstrap_e2e
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$(cd "${1:-$HERE/../build_bootstrap}" && pwd)"
SERVER="$BUILD/core/fileengine_server"
CLI="$BUILD/cli/fileengine_cli"
PGH="${FE_TEST_PG_HOST:-localhost}"; PGP="${FE_TEST_PG_PORT:-5434}"
PGU="${FE_TEST_PG_USER:-postgres}"; PGW="${FE_TEST_PG_PASSWORD:-postgres}"
DB="${FE_BOOTSTRAP_E2E_DB:-fe_bootstrap_e2e}"
GRPC_PORT="${FE_BOOTSTRAP_E2E_PORT:-50161}"
WORK="$(mktemp -d /tmp/fe_bootstrap_e2e.XXXXXX)"
SOCK="$WORK/run/bootstrap.sock"
PASS=0; FAIL=0; FAILED=()
ok()  { PASS=$((PASS+1)); printf '  \033[32m✓\033[0m %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); FAILED+=("$1"); printf '  \033[31m✗\033[0m %s\n     %s\n' "$1" "${2:-}"; }
check() { if eval "$2"; then ok "$1"; else bad "$1" "${3:-condition: $2}"; fi; }

[ "$DB" = "fileengine" ] && { echo "SKIP: refusing the shared 'fileengine' database"; exit 77; }
for b in "$SERVER" "$CLI"; do [ -x "$b" ] || { echo "SKIP: $b not built"; exit 77; }; done
export PGPASSWORD="$PGW"
psql -h "$PGH" -p "$PGP" -U "$PGU" -d postgres -Atc 'select 1' >/dev/null 2>&1 \
    || { echo "SKIP: no Postgres at $PGH:$PGP"; exit 77; }
q() { psql -h "$PGH" -p "$PGP" -U "$PGU" -d "$DB" -Atc "$1" 2>/dev/null; }

# Everything the core and CLI read comes from here — the server runs from an
# empty directory so no .env can leak in.
export FILEENGINE_PG_HOST="$PGH" FILEENGINE_PG_PORT="$PGP" FILEENGINE_PG_DATABASE="$DB"
export FILEENGINE_PG_USER="$PGU" FILEENGINE_PG_PASSWORD="$PGW"
export FILEENGINE_SERVICE_TOKEN_PEPPER="bootstrap-e2e-pepper"
export FILEENGINE_BOOTSTRAP_SOCKET="$SOCK"
export FILEENGINE_STORAGE_BASE="$WORK/storage"
export FILEENGINE_GRPC_HOST=127.0.0.1 FILEENGINE_GRPC_PORT="$GRPC_PORT"
export FILEENGINE_HTTP_METRICS_ENABLED=false
export FILEENGINE_EVENTS_ENABLED=false FILEENGINE_AUDIT_ENABLED=false
export FILEENGINE_LOG_TO_CONSOLE=true FILEENGINE_LOG_LEVEL=INFO
export FILEENGINE_LOG_FILE_PATH="$WORK/core.log"
export FILEENGINE_CLI_IDENTITY="cli:bootstrap-e2e"
mkdir -p "$WORK/run" "$WORK/storage" "$WORK/cwd"

SPID=""
LOG=""
start_core() {   # $1 = label; waits for gRPC or for the process to die
    LOG="$WORK/server-$1.log"
    rm -f "$SOCK"
    ( cd "$WORK/cwd" && exec "$SERVER" ) > "$LOG" 2>&1 &
    SPID=$!
    for _ in $(seq 1 60); do
        grep -q "gRPC Server listening" "$LOG" && return 0
        kill -0 "$SPID" 2>/dev/null || { echo "server exited:"; tail -20 "$LOG"; return 1; }
        sleep 0.5
    done
    echo "server did not come up:"; tail -20 "$LOG"; return 1
}
stop_core() { [ -n "$SPID" ] && kill "$SPID" 2>/dev/null && wait "$SPID" 2>/dev/null; SPID=""; }
cleanup() { stop_core; rm -rf "$WORK"; }
trap cleanup EXIT

reset_db() {
    psql -h "$PGH" -p "$PGP" -U "$PGU" -d postgres -qc "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1
    psql -h "$PGH" -p "$PGP" -U "$PGU" -d postgres -qc "CREATE DATABASE $DB" >/dev/null
}
closures() { q "SELECT count(*) FROM accountability_record_global WHERE action='service_token.bootstrap_closed'"; }
attempted_bind() { grep -q "enrolment socket at\|could not bind the enrolment socket" "$LOG"; }

echo "=========================================================="
echo " bootstrap enrolment E2E   server=$SERVER   db=$DB"
echo "=========================================================="

echo "[1] fresh system: enrolment is available exactly once"
reset_db
start_core fresh || exit 1
check "the enrolment socket exists" "[ -S '$SOCK' ]" "no socket at $SOCK"
out=$("$CLI" bootstrap enrol cli:e2e-operator 2>&1)
check "enrolment mints the first credential" "[ \"\$(q \"SELECT count(*) FROM service_auth_credential WHERE service_id='cli:e2e-operator'\")\" = 1 ]" "$out"
check "and writes the marker" "[ \"\$(q 'SELECT enrolled_id FROM service_auth_bootstrap')\" = cli:e2e-operator ]"
out2=$("$CLI" bootstrap enrol cli:second 2>&1)
check "a second enrolment is refused" "[ \"\$(q \"SELECT count(*) FROM service_auth_credential WHERE service_id='cli:second'\")\" = 0 ]" "$out2"
stop_core

echo "[2] production shape: credentials, no marker"
reset_db
start_core seed || exit 1; stop_core            # creates the schema
"$CLI" service-token issue http_bridge > "$WORK/issue.out" 2>&1
"$CLI" service-token issue csai >> "$WORK/issue.out" 2>&1
check "two credentials issued directly (the production path)" "[ \"\$(q 'SELECT count(*) FROM service_auth_credential')\" = 2 ]" "$(tail -3 "$WORK/issue.out")"
q "DELETE FROM service_auth_bootstrap" >/dev/null   # what every pre-fix deployment looks like
before=$(closures)
start_core prod || exit 1
check "the core backfills the missing marker" "[ \"\$(q 'SELECT enrolled_id FROM service_auth_bootstrap')\" = '*pre-existing*' ]"
check "it says so in its log" "grep -q 'marked it complete now' '$LOG'" "$(grep -i bootstrap "$LOG" | head -3)"
check "the closure is on the accountability record" "[ \"\$(closures)\" = $((before + 1)) ]"
check "no enrolment socket is attempted" "! attempted_bind" "$(grep -i 'enrolment' "$LOG" | head -2)"
check "and none exists" "[ ! -e '$SOCK' ]"
stop_core

echo "[3] second start: nothing more to do"
before=$(closures)
start_core again || exit 1
check "no second backfill message" "! grep -q 'marked it complete now' '$LOG'"
check "no second closure record" "[ \"\$(closures)\" = $before ]"
check "still no socket" "[ ! -e '$SOCK' ] && ! attempted_bind"
stop_core

echo "[4] credential map emptied: the marker keeps enrolment shut"
q "DELETE FROM service_auth_capability; DELETE FROM service_auth_credential" >/dev/null
start_core emptied || exit 1
check "no socket after the map is emptied" "[ ! -e '$SOCK' ] && ! attempted_bind" "$(grep -i 'enrolment' "$LOG" | head -2)"
out=$("$CLI" bootstrap enrol cli:intruder 2>&1)
check "and enrolment cannot be reached" "[ \"\$(q \"SELECT count(*) FROM service_auth_credential WHERE service_id='cli:intruder'\")\" = 0 ]" "$out"
stop_core

echo "[5] a direct issue on a fresh system closes bootstrap itself"
reset_db
start_core seed2 || exit 1; stop_core
"$CLI" service-token issue share > "$WORK/issue2.out" 2>&1
check "the issue wrote the marker, naming the credential" "[ \"\$(q 'SELECT enrolled_id FROM service_auth_bootstrap')\" = share ]" "$(tail -3 "$WORK/issue2.out")"
check "and recorded the closure" "[ \"\$(closures)\" = 1 ]"
start_core after-issue || exit 1
check "the next start opens no socket" "[ ! -e '$SOCK' ] && ! attempted_bind"
check "and has nothing to backfill" "! grep -q 'marked it complete now' '$LOG'"
stop_core

psql -h "$PGH" -p "$PGP" -U "$PGU" -d postgres -qc "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1
echo "=========================================================="
echo " $PASS passed, $FAIL failed"
for f in "${FAILED[@]+"${FAILED[@]}"}"; do echo "   - $f"; done
[ "$FAIL" -eq 0 ]
