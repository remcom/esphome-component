#pragma once

// Host-only test hook: sensor values driven from a control file so the integration tests can
// change inputs while the binary runs. The file named by $ESPHOME_TEST_CTL holds `key=value`
// lines; a value is a float, `nan`, or `skip` (publish nothing, so the sensor goes quiet).
// A missing file or key yields the default.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esphome/core/optional.h"

inline esphome::optional<float> test_ctl(const char *key, float default_value) {
  const char *path = std::getenv("ESPHOME_TEST_CTL");
  if (path == nullptr)
    return default_value;
  FILE *f = std::fopen(path, "r");
  if (f == nullptr)
    return default_value;

  esphome::optional<float> result = default_value;
  const size_t key_len = std::strlen(key);
  char line[64];
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    if (std::strncmp(line, key, key_len) != 0 || line[key_len] != '=')
      continue;
    const char *value = line + key_len + 1;
    if (std::strncmp(value, "skip", 4) == 0)
      result.reset();
    else
      result = std::strtof(value, nullptr);
  }
  std::fclose(f);
  return result;
}
