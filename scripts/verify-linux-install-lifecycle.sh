#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <deb|rpm> <package> <diagnostic-report>" >&2
  exit 2
fi
if [ "${CI:-}" != "true" ] || [ "${BLACKBOX_ALLOW_SYSTEM_PACKAGE_TEST:-}" != "1" ]; then
  echo "refusing a system package lifecycle test outside an explicitly enabled CI container" >&2
  exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
  echo "system package lifecycle test requires the disposable container root user" >&2
  exit 2
fi

kind="$1"
package="$2"
report="$3"
test -s "$package"
case "$kind" in
  deb) package_name="$(dpkg-deb --field "$package" Package)" ;;
  rpm) package_name="$(rpm --query --package --queryformat '%{NAME}' "$package")" ;;
  *) echo "unsupported package kind: $kind" >&2; exit 2 ;;
esac
if [ "$package_name" != "blackbox" ]; then
  echo "unexpected native package name: $package_name" >&2
  exit 1
fi

executable="/usr/bin/blackbox"
desktop="/usr/share/applications/io.github.Haanuwaa.BlackBox.desktop"
icon="/usr/share/icons/hicolor/scalable/apps/io.github.Haanuwaa.BlackBox.svg"
if [ -e "$executable" ] || [ -e "$desktop" ] || [ -e "$icon" ]; then
  echo "refusing to overwrite a pre-existing BlackBox installation" >&2
  exit 1
fi

installed=0
cleanup() {
  if [ "$installed" -eq 1 ]; then
    if [ "$kind" = deb ]; then
      dpkg --purge "$package_name" >/dev/null 2>&1 || true
    else
      rpm --erase "$package_name" >/dev/null 2>&1 || true
    fi
  fi
}
trap cleanup EXIT

if [ "$kind" = deb ]; then
  dpkg --install "$package"
else
  rpm --install "$package"
fi
installed=1
test -x "$executable"
test -s "$desktop"
test -s "$icon"
desktop-file-validate "$desktop"

xvfb-run -a env SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software \
  "$executable" --background-diagnostic-seconds=2 --diagnostic-report="$report"
grep -Fxq "platform=Linux" "$report"
grep -Fxq "source_revision=${GITHUB_SHA:?}" "$report"
grep -Fxq "completed=1" "$report"
grep -Fxq "failed_samples=0" "$report"

if [ "$kind" = deb ]; then
  dpkg --purge "$package_name"
  if dpkg-query --show "$package_name" >/dev/null 2>&1; then
    echo "Debian package database still reports BlackBox after purge" >&2
    exit 1
  fi
else
  rpm --erase "$package_name"
  if rpm --query "$package_name" >/dev/null 2>&1; then
    echo "RPM database still reports BlackBox after erase" >&2
    exit 1
  fi
fi
installed=0
if [ -e "$executable" ] || [ -e "$desktop" ] || [ -e "$icon" ]; then
  echo "native package uninstall left owned lifecycle files behind" >&2
  exit 1
fi
