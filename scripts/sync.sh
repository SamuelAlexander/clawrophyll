#!/usr/bin/env bash
# Deploy the repo from the Mac to the Arduino UNO Q over SSH.
#
# The board's RAM is too tight to run an editor; we edit on the Mac and ship.
# This rsyncs the source trees only -- secrets and runtime state stay on the board.
#
# Usage:
#   scripts/sync.sh            # push the App Lab project + nanobot/ + scripts/ to the board
#   scripts/sync.sh --dry-run  # show what would change, transfer nothing
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Load deployment vars from .env if present.
if [[ -f .env ]]; then
  set -a; source .env; set +a
fi

: "${UNOQ_HOST:?Set UNOQ_HOST in .env (see .env.example)}"
: "${UNOQ_REMOTE_DIR:?Set UNOQ_REMOTE_DIR in .env (see .env.example)}"

DRY_RUN=()
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=(--dry-run --itemize-changes)
  echo "[sync] dry run -- no files will be transferred"
fi

echo "[sync] $REPO_ROOT  ->  $UNOQ_HOST:$UNOQ_REMOTE_DIR"

# --delete keeps the board in sync with the Mac. The excludes protect
# anything created/owned on the board (secrets, runtime state).
# Sources land as subdirs under $UNOQ_REMOTE_DIR; app.yaml lands at the root
# so `arduino-app-cli app start $UNOQ_REMOTE_DIR` finds the App Lab project.
rsync -az --delete ${DRY_RUN[@]+"${DRY_RUN[@]}"} \
  --exclude='.git/' \
  --exclude='.env' \
  --exclude='.phil_secrets' \
  --exclude='*.onnx' \
  --exclude='*.onnx.json' \
  --exclude='.DS_Store' \
  --exclude='__pycache__/' \
  --exclude='.venv/' \
  --exclude='.pytest_cache/' \
  --exclude='build/' \
  --exclude='config.json' \
  --exclude='journal.json' \
  --exclude='HISTORY.md' \
  --exclude='sessions/' \
  app.yaml sketch python nanobot scripts docs README.md \
  "$UNOQ_HOST:$UNOQ_REMOTE_DIR/"

echo "[sync] done"
