#!/usr/bin/env bash
# Runs the full test matrix (3 tiers x 3 GC modes = 9 configurations)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"
exec python3 "${SCRIPT_DIR}/matrix.py" "$@"
