#!/usr/bin/env bash
# Exit 0 (and say why) when CONFIG needs a newer ESPHome than the installed one.
# A config declares that with an "# esphome>=X" comment line, e.g. "# esphome>=2026.11.0".
set -euo pipefail
config=$1
required=$(sed -n 's/^# esphome>=\([0-9][0-9.a-z]*\).*/\1/p' "$config" | head -1)
[[ -n "$required" ]] || exit 1
installed=$(esphome version | awk '{print $2}')
# sort -V orders 2026.11.0 before 2026.11.0b1, so a beta counts as that release
if [[ "$(printf '%s\n%s\n' "$required" "$installed" | sort -V | head -1)" != "$required" ]]; then
  echo "Skipping $config: needs ESPHome >= $required, installed $installed"
  exit 0
fi
exit 1
