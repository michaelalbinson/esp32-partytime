#!/bin/sh
# Checks that every sketch has the same timeline.h, then runs its unit tests.
set -e
cd "$(dirname "$0")/.."

for sketch in esp-ssd1306 esp-7219max; do
  if ! cmp -s esp-st7735/timeline.h "$sketch/timeline.h"; then
    echo "$sketch/timeline.h differs from esp-st7735/timeline.h; copy it over:"
    echo "  cp sketch/esp-st7735/timeline.h sketch/$sketch/timeline.h"
    exit 1
  fi
done

out="$(mktemp -d)/test_timeline"
c++ -std=c++11 -Wall -Wextra -o "$out" tests/test_timeline.cpp
"$out"
