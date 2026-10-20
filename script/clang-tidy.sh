#!/usr/bin/env bash
# Run clang-tidy (ESPHome's .clang-tidy) on every component. The compilation database
# comes from the host builds of tests/components/<name>/test*.host.yaml, so each
# write-back variant is analysed with its own defines.
set -euo pipefail
cd "$(dirname "$0")/.."

extra_args=()
if [[ "$(uname)" == "Darwin" ]]; then
  sdk=$(xcrun --show-sdk-path)
  extra_args+=("--extra-arg=-isysroot$sdk" "--extra-arg=-I$sdk/usr/include/c++/v1")
fi

status=0
for config in tests/components/*/test*.host.yaml; do
  component=$(basename "$(dirname "$config")")
  device=$(esphome config "$config" 2>/dev/null | awk '/^esphome:/{f=1} f && /^  name:/{print $2; exit}')
  build="$(dirname "$config")/.esphome/build/$device"

  echo "=== $config"
  esphome compile --only-generate "$config" > /dev/null
  pio run -d "$build" -t compiledb > /dev/null
  clang-tidy -p "$build" --quiet --header-filter="components/$component/" "${extra_args[@]}" \
    "$build"/src/esphome/components/"$component"/*.cpp || status=1
done
exit $status
