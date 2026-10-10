#!/bin/bash
# usage: tcp.sh '{"cmd":"ping"}' [timeout_s]   - one JSON request to the OriginMonitor control port, keeps the connection open until the reply
exec 3<>/dev/tcp/127.0.0.1/8765 || exit 1
printf '%s\n' "$1" >&3
IFS= read -r -t "${2:-90}" line <&3 && printf '%s\n' "$line"
exec 3<&-
