#!/usr/bin/env bash

set -e

echo "🔍 Finding C++ files..."

find . \
    \( -path "./.git" -o -path "./build" -o -path "./out" \) -prune -o \
    -type f \( \
        -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" \
        -o -name "*.h" -o -name "*.hpp" -o -name "*.hh" \
    \) -print0 | \
xargs -0 -P $(nproc) clang-format -i -style=file

echo "✅ Format complete."