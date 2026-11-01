#!/usr/bin/env bash
# Run clang-tidy (ESPHome's .clang-tidy) on every component. The compilation database
# comes from the host builds of tests/components/<name>/test*.host.yaml (the native host
# toolchain writes it while compiling), so each write-back variant is analysed with its
# own defines.
set -euo pipefail
cd "$(dirname "$0")/.."

extra_args=()
if [[ "$(uname)" == "Darwin" ]]; then
  sdk=$(xcrun --show-sdk-path)
  extra_args+=("--extra-arg=-isysroot$sdk" "--extra-arg=-I$sdk/usr/include/c++/v1")
fi

status=0
for config in tests/components/*/test*.host.yaml; do
  # Host I2C only validates on Linux; such configs start with a "# linux-only" comment.
  if [[ "$(uname)" != "Linux" ]] && head -1 "$config" | grep -q "^# linux-only"; then
    echo "=== $config (skipped: linux-only)"
    continue
  fi
  component=$(basename "$(dirname "$config")")
  device=$(esphome config "$config" 2>/dev/null | awk '/^esphome:/{f=1} f && /^  name:/{print $2; exit}')
  build="$(dirname "$config")/.esphome/build/$device"

  echo "=== $config"
  # Without the precompiled header: clang-tidy cannot read the compiler's .gch
  ESPHOME_PCH_ENABLE=0 esphome compile "$config" > /dev/null
  clang-tidy -p "$build/.pioenvs/$device" --quiet --header-filter="components/$component/" "${extra_args[@]}" \
    "$build"/src/esphome/components/"$component"/*.cpp || status=1
done
exit $status
