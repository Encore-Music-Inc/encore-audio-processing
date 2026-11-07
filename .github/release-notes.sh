#!/usr/bin/env bash
# Prints the release notes for one version: its "## vX.Y.Z" section of
# CHANGELOG.md, without the heading. Fails if the section is missing or lacks
# the parts every ENCORE. release has (see "Release notes" in CLAUDE.md).
# CI runs it on every build, so a "Release vX.Y.Z" pull request can't merge
# without its notes; the release job puts its output on the GitHub release.
#
# Usage: .github/release-notes.sh <version> [CHANGELOG.md]
set -euo pipefail
version=${1:?usage: release-notes.sh <version> [CHANGELOG.md]}
file=${2:-CHANGELOG.md}

notes=$(awk -v h="## v$version" '
  $0 == h { found = 1; next }
  found && /^## / { exit }
  found { print }
' "$file" | sed -e '/./,$!d' | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}')

fail() { echo "::error file=$file::$1" >&2; exit 1; }
[[ -n "$notes" ]] || fail "No \"## v$version\" section in $file. Add this release's notes there, in the \"Release v$version\" pull request."
for part in '### Downloads' '### Built on' 'Questions or problems: support@encoremusic.studio'; do
  grep -qxF "$part" <<<"$notes" || fail "The \"## v$version\" section in $file has no \"$part\" line."
done
printf '%s\n' "$notes"
