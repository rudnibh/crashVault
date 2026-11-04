#!/usr/bin/env bash
# scripts/fetch_deps.sh — Fetch dependencies that cannot be fetched by CMake
# (e.g., in offline CI environments or when FetchContent is not available)
# This script is NOT needed if you have internet access during cmake configuration.

set -euo pipefail

VENDOR_DIR="$(dirname "$0")/../vendor"
mkdir -p "$VENDOR_DIR"

echo "Fetching doctest v2.4.11..."
curl -fsSL -o "$VENDOR_DIR/doctest.h" \
  "https://github.com/doctest/doctest/releases/download/v2.4.11/doctest.h"

echo "Done. vendor/doctest.h created."
