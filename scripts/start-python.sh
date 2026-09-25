#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$REPO_ROOT/src/server"

SAVE_DIR="${SAVE_DIR:-/var/cat-door/snapshots}"
DB_DSN="${DB_DSN:-}"

ARGS=(
  --detector=python
  --model="$REPO_ROOT/model-training/best_int8.onnx"
  --script="$REPO_ROOT/model-training/detect.py"
  --save-dir="$SAVE_DIR"
)

if [[ -n "$DB_DSN" ]]; then
  ARGS+=(--db-dsn="$DB_DSN")
fi

exec go run . "${ARGS[@]}" "$@"