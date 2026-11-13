#!/bin/sh
# Build and run the PC unit tests for the portable dash logic.
set -e
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Wextra -O1 -o /tmp/dash_core_test test_core.cpp ../main/dash_core.cpp
/tmp/dash_core_test
