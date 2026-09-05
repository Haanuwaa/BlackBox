#!/usr/bin/env bash
set -euo pipefail
source_root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/blackbox-deployment-test.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/tools" "$work/BlackBox space.app/Contents/MacOS" \
  "$work/BlackBox space.app/Contents/Frameworks"
bundle="$work/BlackBox space.app"
printf '#!/usr/bin/env bash\nexit 0\n' > "$bundle/Contents/MacOS/BlackBox"
touch "$bundle/Contents/Frameworks/helper.dylib"
chmod +x "$bundle/Contents/MacOS/BlackBox"

# Fixtures replace Apple inspection tools only. The production verifier runs
# unchanged, including shell error propagation and per-architecture iteration.
cat > "$work/tools/plutil" <<'EOF'
#!/usr/bin/env bash
[[ "$CASE" != plist_failure ]] || exit 1
if [[ "$CASE" == plist_mismatch ]]; then echo 14.0; else echo 13.0; fi
EOF
cat > "$work/tools/file" <<'EOF'
#!/usr/bin/env bash
[[ "$CASE" != file_failure ]] || exit 1
if [[ "$CASE" == not_macho ]]; then echo text; else echo 'Mach-O universal binary'; fi
EOF
cat > "$work/tools/lipo" <<'EOF'
#!/usr/bin/env bash
[[ "$CASE" != lipo_failure ]] || exit 1
[[ "$CASE" != no_architectures ]] || exit 0
echo 'arm64 x86_64'
EOF
cat > "$work/tools/otool" <<'EOF'
#!/usr/bin/env bash
[[ "$1" == -arch && "$3" == -l ]] || exit 1
[[ "$CASE" != otool_failure ]] || exit 1
[[ "$CASE" != missing_slice || "$2" != x86_64 ]] || exit 0
version=13.0
case "$CASE" in
  older) version=12.6 ;;
  equivalent) version=13.0.0 ;;
  higher_minor) version=13.1 ;;
  higher_patch) version=13.0.1 ;;
  invalid_version) version=garbage ;;
  newer_foreign_slice) [[ "$2" != x86_64 ]] || version=14.0 ;;
  newer_dependency) [[ "$4" != *helper.dylib ]] || version=14.0 ;;
esac
if [[ "$CASE" == legacy ]]; then
  printf 'cmd LC_VERSION_MIN_MACOSX\nversion %s\nsdk 26.2\n' "$version"
else
  platform=1
  [[ "$CASE" != ios ]] || platform=2
  printf 'cmd LC_BUILD_VERSION\nplatform %s\nminos %s\nsdk 26.2\n' "$platform" "$version"
fi
if [[ "$CASE" == duplicate_target ]]; then
  printf 'cmd LC_VERSION_MIN_MACOSX\nversion 13.0\n'
fi
EOF
chmod +x "$work/tools/"*
real_find="$(command -v find)"
cat > "$work/tools/find" <<EOF
#!/usr/bin/env bash
"$real_find" "\$@"
[[ "\$CASE" != traversal_failure ]]
EOF
chmod +x "$work/tools/find"
export PATH="$work/tools:$PATH"
count=0
check() {
  local expected="$1"
  export CASE="$2"
  local result=0
  bash "$source_root/scripts/verify-macos-deployment.sh" "$bundle" "${3:-13.0}" \
    >"$work/result.log" 2>&1 || result=$?
  if [[ "$expected" == pass && "$result" != 0 ]] || \
     [[ "$expected" == fail && "$result" == 0 ]]; then
    echo "Unexpected $result for $CASE ($expected expected)" >&2
    cat "$work/result.log" >&2
    exit 1
  fi
  count=$((count + 1))
}
for scenario in valid older equivalent legacy; do check pass "$scenario"; done
for scenario in higher_minor higher_patch newer_foreign_slice newer_dependency \
  invalid_version ios duplicate_target missing_slice plist_failure plist_mismatch \
  file_failure not_macho lipo_failure no_architectures otool_failure traversal_failure; do
  check fail "$scenario"
done
check fail valid '13.bad'
mv "$bundle/Contents/MacOS/BlackBox" "$bundle/Contents/MacOS/renamed"
check fail missing_main
printf 'macOS deployment verifier: %s contract cases passed (fixture tools, not native validation)\n' "$count"
