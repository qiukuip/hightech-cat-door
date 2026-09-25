#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../src/server"

exec go run . --detector=stub "$@"