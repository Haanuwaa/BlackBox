#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <deb|rpm> <package> <extract-directory>" >&2
  exit 2
fi

kind="$1"
package="$2"
extract="$3"
test -s "$package"
rm -rf -- "$extract"
mkdir -p -- "$extract"

case "$kind" in
  deb)
    dpkg-deb --info "$package" >/dev/null
    dpkg-deb --extract "$package" "$extract"
    ;;
  rpm)
    rpm --query --package --list "$package" >/dev/null
    (cd "$extract" && rpm2cpio "$OLDPWD/$package" | cpio --extract --make-directories --quiet)
    ;;
  *)
    echo "unsupported package kind: $kind" >&2
    exit 2
    ;;
esac

executable="$extract/usr/bin/blackbox"
desktop="$extract/usr/share/applications/io.github.Haanuwaa.BlackBox.desktop"
icon="$extract/usr/share/icons/hicolor/scalable/apps/io.github.Haanuwaa.BlackBox.svg"
test -x "$executable"
test -s "$desktop"
test -s "$icon"
desktop-file-validate "$desktop" >&2
dynamic_section="$(readelf -d "$executable")"
if ! grep -Fq '$ORIGIN/../lib/blackbox' <<<"$dynamic_section"; then
  echo "Native package executable is missing its private-library RPATH" >&2
  exit 1
fi
if [ -n "${GITHUB_WORKSPACE:-}" ] &&
    grep -Fq "$GITHUB_WORKSPACE" <<<"$dynamic_section"; then
  echo "Native package retains a build-workspace runtime path" >&2
  exit 1
fi
printf '%s\n' "$executable"
