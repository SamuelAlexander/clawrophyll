#!/usr/bin/env bash
# Full redeploy loop for Clawrophyll: edit on Mac, ship to UNO Q, rebuild sketch
# + restart App Lab container, and -- critically -- restart nanobot.
#
# That last step is necessary because nanobot's MCP SSE client doesn't reconnect
# when the App Lab container restarts. The SSE stream stays dead, nanobot keeps
# retrying on it, and tool calls return ClosedResourceError until nanobot itself
# is restarted. This script makes "redeploy" mean the full chain, every time.
#
# Usage: scripts/redeploy.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [[ -f .env ]]; then
  set -a; source .env; set +a
fi

: "${UNOQ_HOST:?Set UNOQ_HOST in .env (see .env.example)}"
: "${UNOQ_REMOTE_DIR:?Set UNOQ_REMOTE_DIR in .env (see .env.example)}"

echo "[redeploy] 1/4  rsync Mac -> board"
scripts/sync.sh

echo
echo "[redeploy] 2/4  deploy nanobot workspace to the live agent (~/.nanobot/workspace)"
scripts/sync-workspace.sh

echo
echo "[redeploy] 3/4  rebuild sketch + restart container (stop -> start)"
# `app restart` proved fragile: it can report "App Is Running" when the CLI's
# state tracking disagrees with the actual container, aborting the deploy.
# An explicit stop (ignore error if already stopped) then start is deterministic.
ssh "$UNOQ_HOST" "arduino-app-cli app stop $UNOQ_REMOTE_DIR || true"
ssh "$UNOQ_HOST" "arduino-app-cli app start $UNOQ_REMOTE_DIR"

echo
echo "[redeploy] 4/4  restart nanobot so its MCP SSE session reconnects"
ssh "$UNOQ_HOST" 'export XDG_RUNTIME_DIR=/run/user/$(id -u); systemctl --user restart nanobot.service'

echo
echo "[redeploy] waiting for nanobot to settle (~10s)..."
sleep 10
ssh "$UNOQ_HOST" 'export XDG_RUNTIME_DIR=/run/user/$(id -u); systemctl --user is-active nanobot.service' \
  && echo "[redeploy] nanobot active, MCP link should be alive" \
  || { echo "[redeploy] nanobot did not come up; check journalctl --user -u nanobot"; exit 1; }
