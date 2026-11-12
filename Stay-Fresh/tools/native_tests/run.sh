#!/bin/sh
# Compile and run the host-side logic tests. Uses -std=gnu++11 to match the
# ESP32 Arduino 2.x toolchain so we don't accidentally use newer C++.
set -e
cd "$(dirname "$0")/../.."
mkdir -p tools/native_tests/build
${CXX:-c++} -std=gnu++11 -Wall -Wextra -Werror -Iinclude \
  tools/native_tests/test_door_logic.cpp -o tools/native_tests/build/test_door_logic -lm
./tools/native_tests/build/test_door_logic
