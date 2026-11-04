#!/bin/sh
# Checks that every sketch has the same timeline.h, then runs its unit tests.
set -e
cd "$(dirname "$0")/.."

for sketch in esp-ssd1306 esp-7219max; do
  for header in timeline.h netlink.h; do
    if ! cmp -s "esp-st7735/$header" "$sketch/$header"; then
      echo "$sketch/$header differs from esp-st7735/$header; copy it over:"
      echo "  cp sketch/esp-st7735/$header sketch/$sketch/$header"
      exit 1
    fi
  done
done

out="$(mktemp -d)/test_timeline"
c++ -std=c++11 -Wall -Wextra -o "$out" tests/test_timeline.cpp
"$out"
