#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CARGO_TARGET_DIR="${TMPDIR:-/tmp}/lumie-adblock-tests" cargo test --locked --manifest-path "${root}/overlay/third_party/adblock_rust/Cargo.toml"
