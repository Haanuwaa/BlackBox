#!/usr/bin/env bash
set -euo pipefail
source_root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/blackbox-wayland-test.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/tools" "$work/runtime"
cat > "$work/tools/mutter" <<'EOF'
#!/usr/bin/env bash
if [[ "$CASE" == startup_failure ]]; then echo 'fixture compositor failed'; exit 3; fi
echo $$ > "$TEST_PID_PATH"
exec sleep 60
EOF
cat > "$work/tools/find" <<'EOF'
#!/usr/bin/env bash
[[ "$CASE" != startup_failure ]] || exit 0
echo wayland-fixture
EOF
cat > "$work/tools/timeout" <<'EOF'
#!/usr/bin/env bash
# The fixture child exits immediately. GNU timeout belongs to the Linux runtime,
# not the macOS host running these control-flow tests; CTest bounds this suite.
[[ "$1" == 30s ]] || exit 2
shift
exec "$@"
EOF
cat > "$work/application" <<'EOF'
#!/usr/bin/env bash
echo 'fixture application launched'
[[ "$CASE" != application_failure ]] || exit 42
for argument; do
  if [[ "$argument" == --diagnostic-report=* ]]; then
    report="${argument#*=}"
  fi
done
completed=1
[[ "$CASE" != incomplete_report ]] || completed=0
printf 'platform=Linux\nvideo_driver=wayland\nsource_revision=%s\ncompleted=%s\nfailed_samples=0\ntray_available=0\nwindow_visible=1\n' \
  "$REVISION" "$completed" > "$report"
EOF
chmod +x "$work/tools/"* "$work/application"
export PATH="$work/tools:$PATH" RUNNER_TEMP="$work/runtime"
export REVISION=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
count=0
for scenario in success startup_failure application_failure incomplete_report; do
  export CASE="$scenario" TEST_PID_PATH="$work/$scenario.pid"
  result=0
  evidence="$work/$scenario"
  bash "$source_root/scripts/qualify-wayland-compositor.sh" mutter \
    "$work/application" "$evidence" "$REVISION" > "$work/$scenario.log" 2>&1 || result=$?
  expected=1
  stage=verify_report
  case "$scenario" in
    success) expected=0; stage=complete ;;
    startup_failure) stage=wait_for_socket ;;
    application_failure) expected=42; stage=launch_application ;;
  esac
  if [[ "$result" != "$expected" ]]; then cat "$work/$scenario.log"; exit 1; fi
  grep -Fxq "stage=$stage" "$evidence/mutter-status.ini"
  grep -Fxq "exit_code=$expected" "$evidence/mutter-status.ini"
  if [[ "$expected" != 0 ]]; then
    grep -Fxq completed=0 "$evidence/mutter-status.ini"
    grep -q 'Wayland qualification failed:' "$work/$scenario.log"
    [[ ! -f "$evidence/mutter-summary.ini" ]]
  fi
  if [[ -f "$TEST_PID_PATH" ]] && kill -0 "$(cat "$TEST_PID_PATH")" 2>/dev/null; then
    echo 'Fixture compositor survived cleanup' >&2
    exit 1
  fi
  count=$((count + 1))
done
# Existing evidence is refused without replacing the successful result.
if bash "$source_root/scripts/qualify-wayland-compositor.sh" mutter \
  "$work/application" "$work/success" "$REVISION" > "$work/repeated.log" 2>&1; then exit 1; fi
grep -Fxq completed=1 "$work/success/mutter-status.ini"
printf 'Wayland qualification: %s fixture scenarios and stale-evidence rejection passed\n' "$count"
