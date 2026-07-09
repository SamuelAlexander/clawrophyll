#!/usr/bin/env bash
# Deploy the curated nanobot workspace CONFIG from the repo to the live agent on
# the UNO Q -- i.e. to ~/.nanobot/workspace/, which is where nanobot actually
# reads at runtime. (That is NOT the same as the $UNOQ_REMOTE_DIR/nanobot copy
# that scripts/sync.sh maintains; that one is just a backup of the source tree.)
#
# Pushes only the files we author and Phil does not edit at runtime:
#   SOUL.md, USER.md, skills/
# It deliberately:
#   - does NOT push MEMORY.md or HEARTBEAT.md. Those are *seeds*: Sam edits
#     standing instructions / periodic tasks and Phil writes notes into the live
#     copies, so the running files are authoritative. Seed them by hand on first
#     setup; never let a deploy clobber them.
#   - does NOT use --delete, so board-only files survive: the nanobot boilerplate
#     (AGENTS.md, TOOLS.md) and runtime state (cron/, memory/, sessions/, .git/).
#
# nanobot re-reads the workspace live, so changes take effect with no restart.
#
# Usage:
#   scripts/sync-workspace.sh            # push the curated config to the board
#   scripts/sync-workspace.sh --dry-run  # show what would change, transfer nothing
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [[ -f .env ]]; then
  set -a; source .env; set +a
fi

: "${UNOQ_HOST:?Set UNOQ_HOST in .env (see .env.example)}"
# Path to the live nanobot workspace, relative to the board user's home dir.
WS_REMOTE="${UNOQ_WORKSPACE_DIR:-.nanobot/workspace}"

DRY_RUN=()
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=(--dry-run --itemize-changes)
  echo "[sync-workspace] dry run -- no files will be transferred"
fi

echo "[sync-workspace] repo nanobot/workspace -> $UNOQ_HOST:~/$WS_REMOTE"
echo "[sync-workspace]   (config only; MEMORY.md + HEARTBEAT.md seeds and board-only state are left untouched)"

# No --delete: only add/update the authored config; never remove board-only files.
rsync -az ${DRY_RUN[@]+"${DRY_RUN[@]}"} \
  --exclude='MEMORY.md' \
  --exclude='HEARTBEAT.md' \
  --exclude='.DS_Store' \
  nanobot/workspace/ \
  "$UNOQ_HOST:$WS_REMOTE/"

echo "[sync-workspace] done. nanobot reads the workspace live -- no restart needed."
