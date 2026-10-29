#!/usr/bin/env bash
# Deploy the kids-demo app to the UNO Q and switch the board between modes.
#
#   kids-demo/deploy.sh          sync + (re)start the kids app, make it the boot app
#   kids-demo/deploy.sh agent    switch back to full Clawrophyll (AI agent) on boot
#   kids-demo/deploy.sh logs     follow the kids app's status log
#
# Kids mode stops and disables the agent's services (clawrophyll-app, nanobot) so
# the board boots straight into the offline demo with no network dependency.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -f "$REPO_ROOT/.env" ]] && { set -a; source "$REPO_ROOT/.env"; set +a; }
: "${UNOQ_HOST:?Set UNOQ_HOST in .env}"
KIDS_DIR=/home/arduino/ArduinoApps/clawrophyll-kids
AGENT_DIR=/home/arduino/ArduinoApps/clawrophyll
SYSD='export XDG_RUNTIME_DIR=/run/user/$(id -u); systemctl --user'

case "${1:-kids}" in
  kids)
    echo "[kids] sync -> $UNOQ_HOST:$KIDS_DIR"
    rsync -az --delete --exclude='.DS_Store' --exclude='__pycache__/' --exclude='.cache/' \
      "$REPO_ROOT/kids-demo/app.yaml" "$REPO_ROOT/kids-demo/sketch" \
      "$REPO_ROOT/kids-demo/python" "$REPO_ROOT/kids-demo/README.md" "$UNOQ_HOST:$KIDS_DIR/"
    echo "[kids] stop agent services, disable them at boot"
    ssh "$UNOQ_HOST" "$SYSD stop nanobot.service clawrophyll-app.service || true; \
      $SYSD disable nanobot.service clawrophyll-app.service || true; \
      arduino-app-cli app stop $AGENT_DIR >/dev/null 2>&1 || true"
    echo "[kids] build + start (first build takes a few minutes)"
    ssh "$UNOQ_HOST" "arduino-app-cli app stop $KIDS_DIR >/dev/null 2>&1 || true; \
      arduino-app-cli app start $KIDS_DIR && arduino-app-cli properties set default $KIDS_DIR"
    ssh "$UNOQ_HOST" "arduino-app-cli properties get default"
    ;;
  agent)
    echo "[agent] stop kids app, restore the agent on boot"
    ssh "$UNOQ_HOST" "arduino-app-cli app stop $KIDS_DIR >/dev/null 2>&1 || true; \
      arduino-app-cli properties set default none; \
      $SYSD enable clawrophyll-app.service nanobot.service; \
      $SYSD start clawrophyll-app.service && $SYSD restart nanobot.service"
    ;;
  logs)
    ssh "$UNOQ_HOST" "arduino-app-cli app logs $KIDS_DIR --follow"
    ;;
  *) echo "usage: $0 [kids|agent|logs]"; exit 1 ;;
esac
