#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: qualify-wayland-compositor.sh <weston|mutter|kwin|sway> <blackbox> <evidence-dir> <source-revision>" >&2
  exit 2
fi

compositor="$1"
executable="$2"
evidence_dir="$3"
source_revision="$4"

[[ -x "$executable" ]]
[[ "$source_revision" =~ ^[0-9a-f]{40}$ ]]
mkdir -p "$evidence_dir"
evidence_dir="$(cd "$evidence_dir" && pwd)"

runtime="$RUNNER_TEMP/blackbox-$compositor-runtime"
config="$RUNNER_TEMP/blackbox-$compositor-config"
rm -rf "$runtime" "$config"
mkdir -p "$runtime" "$config"
chmod 700 "$runtime"
export XDG_RUNTIME_DIR="$runtime"
export XDG_CONFIG_HOME="$config"
export XDG_SESSION_TYPE=wayland
export SDL_VIDEODRIVER=wayland
export SDL_RENDER_DRIVER=software
export LIBGL_ALWAYS_SOFTWARE=1

log="$evidence_dir/$compositor.log"
compositor_pid=""
cleanup() {
  if [[ -n "$compositor_pid" ]]; then
    kill "$compositor_pid" 2>/dev/null || true
    wait "$compositor_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

case "$compositor" in
  weston)
    weston --backend=headless-backend.so --socket=wayland-blackbox \
      --idle-time=0 --log="$log" &
    compositor_pid=$!
    ;;
  mutter)
    export XDG_CURRENT_DESKTOP=GNOME
    mutter --wayland --headless --no-x11 \
      --virtual-monitor 1280x720 >"$log" 2>&1 &
    compositor_pid=$!
    ;;
  kwin)
    export XDG_CURRENT_DESKTOP=KDE
    kwin_wayland --virtual --no-lockscreen \
      --no-global-shortcuts --width 1280 --height 720 >"$log" 2>&1 &
    compositor_pid=$!
    ;;
  sway)
    export XDG_CURRENT_DESKTOP=sway
    sway_config="$config/sway.config"
    printf '%s\n' 'xwayland disable' 'output * mode 1280x720' >"$sway_config"
    WLR_BACKENDS=headless WLR_RENDERER=pixman \
      WLR_LIBINPUT_NO_DEVICES=1 sway --config "$sway_config" \
      >"$log" 2>&1 &
    compositor_pid=$!
    ;;
  *)
    echo "unsupported compositor: $compositor" >&2
    exit 2
    ;;
esac

socket=""
for _ in $(seq 1 100); do
  kill -0 "$compositor_pid"
  socket="$(find "$runtime" -maxdepth 1 -type s -name 'wayland-*' -printf '%f\n' | sort | head -n 1)"
  [[ -n "$socket" ]] && break
  sleep 0.1
done
[[ -n "$socket" ]]
export WAYLAND_DISPLAY="$socket"

report="$evidence_dir/$compositor-smoke.ini"
timeout 30s "$executable" --background-diagnostic-seconds=2 \
  --diagnostic-report="$report"
grep -Fxq 'platform=Linux' "$report"
grep -Fxq 'video_driver=wayland' "$report"
grep -Fxq "source_revision=$source_revision" "$report"
grep -Fxq 'completed=1' "$report"
grep -Fxq 'failed_samples=0' "$report"
tray="$(awk -F= '$1 == "tray_available" { print $2 }' "$report")"
visible="$(awk -F= '$1 == "window_visible" { print $2 }' "$report")"
[[ "$tray" == 1 || "$visible" == 1 ]]

{
  printf 'format=1\n'
  printf 'compositor=%s\n' "$compositor"
  printf 'wayland_display=%s\n' "$socket"
  printf 'source_revision=%s\n' "$source_revision"
  printf 'video_driver=wayland\n'
  printf 'completed=1\n'
} >"$evidence_dir/$compositor-summary.ini"
