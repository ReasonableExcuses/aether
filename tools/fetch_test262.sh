#!/usr/bin/env bash
# Fetches official tc39/test262 repository pinned to the commit in tools/test262.lock
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOCK_FILE="${SCRIPT_DIR}/test262.lock"
TARGET_DIR="${ROOT_DIR}/test/official-test262"

if [ ! -f "${LOCK_FILE}" ]; then
    echo "Error: ${LOCK_FILE} not found."
    exit 1
fi

COMMIT_SHA="$(cat "${LOCK_FILE}" | tr -d '[:space:]')"

if [ ! -d "${TARGET_DIR}/.git" ]; then
    echo "Cloning tc39/test262 into ${TARGET_DIR}..."
    git clone https://github.com/tc39/test262.git "${TARGET_DIR}"
fi

echo "Checking out pinned test262 commit ${COMMIT_SHA}..."
cd "${TARGET_DIR}"
git fetch --depth 1 origin "${COMMIT_SHA}" 2>/dev/null || git fetch origin
git checkout "${COMMIT_SHA}"
echo "Test262 is ready at ${TARGET_DIR} (${COMMIT_SHA})"
