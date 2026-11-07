#!/usr/bin/env bash
# Ends a sanitizer run: prints every report the sanitizers wrote to <folder>
# (one file per process) and fails if there was any, or if the run itself
# failed or timed out.
#
#   report.sh <folder> <exit code of the run>
set -uo pipefail
reports="${1:?usage: report.sh <folder> <exit code>}"
code="${2:?exit code}"
count=0
for f in "$reports"/*; do
  [ -s "$f" ] || continue
  count=$((count + 1))
  echo "== $(basename "$f")"
  cat "$f"
done
findings=$(cat "$reports"/* 2>/dev/null | grep -c 'runtime error:\|ERROR: AddressSanitizer' || true)
[ "$code" -eq 124 ] && echo "::error::the run timed out"
[ "$code" -ne 0 ] && [ "$code" -ne 124 ] && echo "::error::the run failed ($code)"
[ "$findings" -gt 0 ] && echo "::error::the sanitizers reported $findings finding(s), listed above"
[ "$count" -eq 0 ] && [ "$code" -eq 0 ] && echo "No sanitizer findings"
if [ "$code" -ne 0 ] || [ "$findings" -gt 0 ]; then exit 1; fi
exit 0
