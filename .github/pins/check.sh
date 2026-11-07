#!/usr/bin/env bash
# Checks that every pinned repository is pinned to a release.
#
# The "Pinned releases" table in README.md (between the pinned-releases
# markers) is the list people read. For each row this checks that the release
# named there exists in that repository and points to exactly the commit the
# folder is pinned to. A row can instead say "commit `abc1234` (exception:
# reason)" when no usable release exists; the reason is required. Every
# submodule of this repo must have a row. Rows for folders inside a submodule
# (what a pinned repo itself pins) are checked the same way, through it.
#
#   .github/pins/check.sh            (from the repo root; submodules whose own
#                                     pins are listed must be checked out)
set -uo pipefail

readme="README.md"
table=$(sed -n '/<!-- pinned-releases:start -->/,/<!-- pinned-releases:end -->/p' "$readme" \
        | grep '^| `' || true)
[ -n "$table" ] || { echo "::error::No Pinned releases table in $readme"; exit 1; }

# The commit a folder is pinned to, also for folders inside a submodule
pinned() {
  local path="$1" parent="." rest="$1" p
  while :; do
    p=$(git -C "$parent" ls-tree HEAD -- "$rest" 2>/dev/null | awk '$2 == "commit" { print $3 }')
    [ -n "$p" ] && { echo "$p"; return 0; }
    local sub
    sub=$(git -C "$parent" config -f .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null \
          | awk '{ print $2 }' | while read -r s; do case "$rest" in "$s"/*) echo "$s";; esac; done | head -1)
    [ -n "$sub" ] || return 1
    [ -e "$parent/$sub/.git" ] || { echo "not-checked-out:$parent/$sub"; return 1; }
    parent="$parent/$sub"; rest="${rest#"$sub"/}"
  done
}

failed=0
listed=""
while IFS= read -r row; do
  folder=$(echo "$row" | awk -F'|' '{ print $2 }' | tr -d ' `')
  repo=$(echo "$row" | awk -F'|' '{ print $3 }' | grep -o 'https://[^ )]*' | head -1)
  release=$(echo "$row" | awk -F'|' '{ print $4 }' | sed 's/^ *//; s/ *$//')
  listed="$listed $folder"
  have=$(pinned "$folder") || {
    case "$have" in
      not-checked-out:*) echo "::error::$folder: ${have#not-checked-out:} is not checked out, so its pin can't be read" ;;
      *) echo "::error::$folder is in the table but is not a pinned folder" ;;
    esac
    failed=1; continue
  }
  if [[ "$release" == commit* ]]; then
    short=$(echo "$release" | grep -o '`[0-9a-f]\{7,40\}`' | tr -d '`' | head -1)
    reason=$(echo "$release" | sed -n 's/.*(exception: *\(.*\)).*/\1/p')
    if [ -z "$short" ] || [ -z "$reason" ]; then
      echo "::error::$folder: an exception must read: commit \`<id>\` (exception: <reason>)"; failed=1
    elif [[ "$have" != "$short"* ]]; then
      echo "::error::$folder is pinned to ${have:0:12}, but the table says commit $short"; failed=1
    else
      echo "ok   $folder  commit ${have:0:12}  (exception: $reason)"
    fi
    continue
  fi
  tag=$(echo "$release" | grep -o '`[^`]*`' | tr -d '`' | head -1)
  [ -n "$tag" ] || { echo "::error::$folder: no release named in the table"; failed=1; continue; }
  [ -n "$repo" ] || { echo "::error::$folder: no repository link in the table"; failed=1; continue; }
  refs=$(git ls-remote "$repo" "refs/tags/$tag" "refs/tags/$tag^{}" 2>/dev/null)
  sha=$(echo "$refs" | awk '/\^\{\}$/ { print $1 }')
  [ -n "$sha" ] || sha=$(echo "$refs" | awk '{ print $1 }' | head -1)
  if [ -z "$sha" ]; then
    echo "::error::$folder: $repo has no release $tag (not published yet?)"; failed=1
  elif [ "$sha" != "$have" ]; then
    echo "::error::$folder is pinned to ${have:0:12}, but release $tag is ${sha:0:12}"; failed=1
  else
    echo "ok   $folder  $tag  (${have:0:12})"
  fi
done <<< "$table"

# Every submodule of this repo has a row
for s in $(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{ print $2 }'); do
  case " $listed " in *" $s "*) ;; *) echo "::error::$s is pinned but missing from the Pinned releases table in $readme"; failed=1;; esac
done

[ "$failed" -eq 0 ] && echo "All pins are releases (or listed exceptions)"
exit "$failed"
