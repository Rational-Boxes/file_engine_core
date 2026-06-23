#!/usr/bin/env bash
#
# Core test for the file-as-container "hidden renditions" feature, driven via the
# CLI against a running gRPC core. Renditions are children of a file entity:
# hidden from the parent directory listing, revealed when the file UID is listed,
# carried by move, deep-copied by copy, and cascaded on delete.
#
# Usage: ./test_renditions.sh   (core must be listening; defaults to :50051)
set -u
BIN="${FE_CLI:-./build_2/cli/fileengine_cli}"
SRV="${FE_SERVER:-localhost:50051}"
TENANT="${FE_TENANT:-default}"
CLI="$BIN -u rendtester -t $TENANT -r system_admin --server $SRV"

pass=0; fail=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS  $1"; pass=$((pass+1)); else echo "  FAIL  $1 (want '$2' got '$3')"; fail=$((fail+1)); fi; }
uid_of() { grep -oiE 'UID: [0-9a-f-]{36}' | head -1 | awk '{print $2}'; }
has_uid() { $CLI ls "$1" 2>/dev/null | grep -q "$2" && echo yes || echo no; }
# Count only entry lines ("[FILE]"/"[DIR]"), not the "Contents of ... (UID:)" header.
child_count() { $CLI ls "$1" 2>/dev/null | grep -cE '\[(FILE|DIR)\]'; }

ts=$(date +%s)
printf 'parent content\n' > /tmp/rend_parent.$$
printf 'rendition content (alternate format)\n' > /tmp/rend_child.$$

echo "== rendition core test (tenant=$TENANT) =="
dir=$($CLI  mkdir "" "rendsrc_$ts"  | uid_of)
dir2=$($CLI mkdir "" "renddst_$ts" | uid_of)
file=$($CLI touch "$dir" "doc.txt"  | uid_of)
$CLI put "$file" /tmp/rend_parent.$$ >/dev/null 2>&1
rend=$($CLI touch "$file" "${ts}-pdf.pdf" | uid_of)   # rendition: child of the FILE
$CLI put "$rend" /tmp/rend_child.$$ >/dev/null 2>&1

ck "setup produced uids" "ok" "$([ -n "$dir" ] && [ -n "$file" ] && [ -n "$rend" ] && echo ok || echo missing)"

# Hiding: the rendition is a child of the file, so it must NOT appear in the dir.
ck "file visible in parent dir"        yes "$(has_uid "$dir" "$file")"
ck "rendition hidden in parent dir"    no  "$(has_uid "$dir" "$rend")"
# Targeted: listing the file UID reveals its renditions.
ck "rendition revealed by file-uid ls" yes "$(has_uid "$file" "$rend")"

# Copy deep-copies renditions to the new file.
$CLI copy "$file" "$dir2" >/dev/null 2>&1
copyfile=$($CLI ls "$dir2" 2>/dev/null | grep 'doc.txt' | uid_of)
ck "copy created the file"              ok  "$([ -n "$copyfile" ] && echo ok || echo missing)"
ck "copy deep-copied the rendition"    1   "$(child_count "$copyfile")"

# Move carries renditions (same file UID; children reference it).
$CLI move "$file" "$dir2" >/dev/null 2>&1
ck "rendition persists after move"     yes "$(has_uid "$file" "$rend")"

# Delete cascades to renditions.
$CLI rm "$file" >/dev/null 2>&1
ck "rendition cascaded on delete"      0   "$(child_count "$file")"

# Cleanup (best effort).
$CLI rm "$copyfile" >/dev/null 2>&1
$CLI rmdir "$dir" >/dev/null 2>&1; $CLI rmdir "$dir2" >/dev/null 2>&1
rm -f /tmp/rend_parent.$$ /tmp/rend_child.$$

echo "== results: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
