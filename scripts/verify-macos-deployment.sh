#!/usr/bin/env bash
set -euo pipefail
fail() { echo "macOS deployment verification failed: $*" >&2; exit 1; }
if [ "$#" -ne 2 ]; then
  echo "usage: $0 <BlackBox.app> <minimum-macos-version>" >&2
  exit 2
fi
bundle="$1"
minimum="$2"
[[ "$minimum" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || fail "invalid minimum version"
test -d "$bundle" || fail "bundle missing: $bundle"
test -x "$bundle/Contents/MacOS/BlackBox" || fail "main executable missing"
test "$(plutil -extract LSMinimumSystemVersion raw "$bundle/Contents/Info.plist")" = "$minimum" \
  || fail "Info.plist does not advertise $minimum"
main_kind="$(file -b "$bundle/Contents/MacOS/BlackBox")"
[[ "$main_kind" == *Mach-O* ]] || fail "main executable is not Mach-O"
found=0
slices=0
file_list="$(mktemp "${TMPDIR:-/tmp}/blackbox-macho-files.XXXXXX")"
trap 'rm -f -- "$file_list"' EXIT
# Process substitution would hide a failed traversal from set -e/pipefail.
find "$bundle" -type f -print0 > "$file_list"
while IFS= read -r -d '' binary; do
  kind="$(file -b "$binary")"
  if [[ "$kind" == *Mach-O* ]]; then
    found=$((found + 1))
    # otool defaults to the host slice. Enumerate explicitly so a valid host
    # slice cannot conceal a newer target or missing metadata in another slice.
    architectures="$(lipo -archs "$binary")"
    [[ -n "$architectures" ]] || fail "no architectures: $binary"
    for architecture in $architectures; do
      [[ "$architecture" =~ ^[a-zA-Z0-9_]+$ ]] || fail "invalid architecture"
      version="$(otool -arch "$architecture" -l "$binary" | awk '
        $1 == "cmd" { command = $2 }
        command == "LC_BUILD_VERSION" && $1 == "platform" {
          if ($2 != "1" && $2 != "macos" && $2 != "MACOS") invalid = 1;
          platforms++;
        }
        command == "LC_BUILD_VERSION" && $1 == "minos" { modern++; value = $2 }
        command == "LC_VERSION_MIN_MACOSX" && $1 == "version" { legacy++; value = $2 }
        END {
          if (invalid || modern + legacy != 1 || platforms != modern) exit 1;
          print value;
        }
      ')" || fail "missing or invalid macOS target: $binary ($architecture)"
      [[ "$version" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || fail "invalid target: $version"
      if ! awk -v actual="$version" -v maximum="$minimum" 'BEGIN {
        split(actual, a, "."); split(maximum, b, ".");
        for (i = 1; i <= 3; ++i) {
          if (a[i] + 0 > b[i] + 0) exit 1;
          if (a[i] + 0 < b[i] + 0) exit 0;
        }
      }'; then
        fail "$binary ($architecture) requires $version, advertised $minimum"
      fi
      slices=$((slices + 1))
    done
  fi
done < "$file_list"
test "$found" -ge 1 || fail "no Mach-O files"
printf 'Verified %s Mach-O files, %s architecture slices against macOS %s\n' "$found" "$slices" "$minimum"
