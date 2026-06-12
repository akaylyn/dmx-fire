#!/usr/bin/env bash
# Local web UI preview server — background daemon with live reload.
#
#   scripts/web-debug.sh                # start in background (default)
#   scripts/web-debug.sh start [--open] # start + optionally open browser
#   scripts/web-debug.sh stop           # stop the running server
#   scripts/web-debug.sh restart        # stop then start
#   scripts/web-debug.sh status         # show running PID + URL
#   scripts/web-debug.sh logs           # tail -f logs/web-preview.log
#   scripts/web-debug.sh foreground     # run attached (Ctrl-C to stop)
#
# Edit tools/web-preview/index.html — it is the canonical source of truth.
# The browser auto-reloads whenever the file changes on disk (a small polling
# script is injected into every served page). When the design is ready, ask
# Claude to run /web-sync to port the changes into Test_Button_DMX/web.cpp.

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
PORT=8123
URL="http://localhost:${PORT}/"
PIDFILE="/tmp/dmx-fire-web-preview.pid"
LOGDIR="$REPO/logs"
LOGFILE="$LOGDIR/web-preview.log"
SERVER="$REPO/tools/web-preview/server.py"

is_running() {
  [[ -f "$PIDFILE" ]] || return 1
  local pid
  pid="$(cat "$PIDFILE" 2>/dev/null || true)"
  [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null
}

start() {
  local open_browser=0
  for arg in "$@"; do
    [[ "$arg" == "--open" ]] && open_browser=1
  done

  if is_running; then
    local pid; pid="$(cat "$PIDFILE")"
    echo "==> already running (PID $pid) at $URL"
  else
    mkdir -p "$LOGDIR"
    nohup python3 "$SERVER" >>"$LOGFILE" 2>&1 &
    echo $! >"$PIDFILE"
    sleep 0.3
    if is_running; then
      echo "==> started (PID $(cat "$PIDFILE")) at $URL"
      echo "    logs:  $LOGFILE"
      echo "    edit:  tools/web-preview/index.html  (browser auto-reloads)"
      echo "    sync:  ask Claude to run /web-sync when ready"
    else
      echo "==> failed to start — check $LOGFILE"
      rm -f "$PIDFILE"
      exit 1
    fi
  fi

  if (( open_browser )); then
    ( sleep 0.3 && open "$URL" ) &
  fi
}

stop() {
  if is_running; then
    local pid; pid="$(cat "$PIDFILE")"
    kill "$pid" 2>/dev/null || true
    # Wait briefly for graceful exit.
    for _ in 1 2 3 4 5; do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
    rm -f "$PIDFILE"
    echo "==> stopped (was PID $pid)"
  else
    echo "==> not running"
    rm -f "$PIDFILE"
  fi
}

status() {
  if is_running; then
    echo "==> running (PID $(cat "$PIDFILE")) at $URL"
    echo "    logs:  $LOGFILE"
  else
    echo "==> not running"
  fi
}

logs() {
  mkdir -p "$LOGDIR"
  touch "$LOGFILE"
  exec tail -f "$LOGFILE"
}

foreground() {
  if is_running; then
    echo "==> background server already running (PID $(cat "$PIDFILE")); stop it first" >&2
    exit 1
  fi
  echo "==> DMX Fire web UI preview (foreground, Ctrl-C to stop)"
  echo "    edit:  tools/web-preview/index.html  (browser auto-reloads)"
  echo "    url:   $URL"
  echo ""
  exec python3 "$SERVER"
}

cmd="${1:-start}"
case "$cmd" in
  start|"")    shift || true; start "$@" ;;
  stop)        stop ;;
  restart)     stop; start ;;
  status)      status ;;
  logs)        logs ;;
  foreground)  foreground ;;
  --open)      start --open ;;
  *)
    echo "usage: $0 [start [--open] | stop | restart | status | logs | foreground]" >&2
    exit 2
    ;;
esac
