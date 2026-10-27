#!/bin/bash
# Start, stop or check the OriginWatch phone view (headless OriginWatch with the web server). One instance, pidfile in ~/.cache.
#   tools/watch_web.sh start|stop|restart|status
# Default: plain HTTP on 127.0.0.1:8480 for nginx to proxy (contrib/nginx/origin-watch.conf; phone address https://origin.kimmitt.uk/?t=TOKEN, then a cookie).
# Direct HTTPS instead (no nginx): ORIGIN_WEB_TLS=1 serves https on all addresses at port $ORIGIN_WEB_PORT (default 8443) with ~/.config/origin/web/{fullchain,key}.pem.
# Access token: ~/.config/origin/web.token. ORIGIN_WEB_PIDFILE moves the pidfile (default ~/.cache/origin-watch-web.pid).
# For cron (starts it when it is not running):   */10 * * * * ~/OriginMonitor/tools/watch_web.sh start >/dev/null 2>&1
HERE="$(cd "$(dirname "$0")/.." && pwd)"; BIN="$HERE/build/watch/build/OriginWatch"
CFG="$HOME/.config/origin"; PIDF="${ORIGIN_WEB_PIDFILE:-$HOME/.cache/origin-watch-web.pid}"; LOG="$HOME/.cache/origin-watch-web.log"; PORT="${ORIGIN_WEB_PORT:-8480}"; [ -n "$ORIGIN_WEB_TLS" ] && PORT="${ORIGIN_WEB_PORT:-8443}"
alive() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }
case "$1" in
  start)
    alive && { echo "already running (pid $(cat "$PIDF"))"; exit 0; }
    [ -x "$BIN" ] || { echo "build OriginWatch first: $BIN"; exit 1; }
    args=(-platform offscreen --port "${ORIGIN_CONTROL_PORT:-8765}" --web-port "$PORT")
    [ -r "$CFG/web.token" ] && args+=(--web-token-file "$CFG/web.token")
    if [ -n "$ORIGIN_WEB_TLS" ]; then [ -r "$CFG/web/fullchain.pem" ] && [ -r "$CFG/web/key.pem" ] && args+=(--web-cert "$CFG/web/fullchain.pem" --web-key "$CFG/web/key.pem")
    else args+=(--web-bind 127.0.0.1); fi
    nohup "$BIN" "${args[@]}" >> "$LOG" 2>&1 &
    echo $! > "$PIDF"; sleep 2; alive && echo "started (pid $(cat "$PIDF")), log $LOG" || { echo "failed, see $LOG"; exit 1; } ;;
  stop)
    alive && { kill "$(cat "$PIDF")"; rm -f "$PIDF"; echo stopped; } || echo "not running" ;;
  restart) "$0" stop; sleep 1; "$0" start ;;
  status) alive && echo "running (pid $(cat "$PIDF"))" || { echo "not running"; exit 1; } ;;
  *) echo "usage: $0 start|stop|restart|status"; exit 2 ;;
esac
