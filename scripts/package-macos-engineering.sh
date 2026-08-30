#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <BlackBox.app> <output-directory> <version>" >&2
  exit 2
fi

source_bundle="$1"
output_directory="$2"
version="$3"
test -x "$source_bundle/Contents/MacOS/BlackBox"
if ! [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "version must be a three-component release version" >&2
  exit 2
fi
mkdir -p -- "$output_directory"
staging="$(mktemp -d "${TMPDIR:-/tmp}/blackbox-macos-package.XXXXXX")"
cleanup() {
  case "$staging" in
    "${TMPDIR:-/tmp}"/blackbox-macos-package.*) rm -rf -- "$staging" ;;
    *) echo "refusing to remove unexpected staging path: $staging" >&2 ;;
  esac
}
trap cleanup EXIT

bundle="$staging/BlackBox.app"
ditto "$source_bundle" "$bundle"
application_identity="${BLACKBOX_MACOS_APPLICATION_IDENTITY:-}"
installer_identity="${BLACKBOX_MACOS_INSTALLER_IDENTITY:-}"
notary_profile="${BLACKBOX_MACOS_NOTARY_PROFILE:-}"
if [ -n "$application_identity" ]; then
  codesign --force --deep --options runtime --timestamp \
    --sign "$application_identity" "$bundle"
  codesign --verify --deep --strict --verbose=2 "$bundle"
fi

architecture="$(uname -m)"
base="$output_directory/BlackBox-$version-macos-$architecture-engineering-preview"
dmg="$base.dmg"
pkg="$base.pkg"
hdiutil create -quiet -volname "BlackBox $version" -srcfolder "$staging" \
  -format UDZO -ov "$dmg"

pkg_arguments=(--component "$bundle" --install-location /Applications \
  --identifier io.github.Haanuwaa.BlackBox --version "$version")
if [ -n "$installer_identity" ]; then
  pkg_arguments+=(--sign "$installer_identity" --timestamp)
fi
pkgbuild "${pkg_arguments[@]}" "$pkg"

hdiutil verify "$dmg"
pkgutil --payload-files "$pkg" | grep -Eq '(^|/)BlackBox\.app/Contents/MacOS/BlackBox$'

if [ -n "$notary_profile" ]; then
  if [ -z "$application_identity" ] || [ -z "$installer_identity" ]; then
    echo "notarization requires both application and installer signing identities" >&2
    exit 2
  fi
  xcrun notarytool submit "$dmg" --keychain-profile "$notary_profile" --wait
  xcrun stapler staple "$dmg"
  xcrun stapler validate "$dmg"
  xcrun notarytool submit "$pkg" --keychain-profile "$notary_profile" --wait
  xcrun stapler staple "$pkg"
  xcrun stapler validate "$pkg"
fi

printf '%s\n%s\n' "$dmg" "$pkg"
