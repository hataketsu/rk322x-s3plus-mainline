#!/bin/bash
# UART serial console -> logfile, for the RK322x S3 Plus box.
# Run on the machine that has the PL2303 USB-serial adapter (m416).
#
#   bash tools/uart-log.sh                 # /dev/ttyUSB0 @115200 -> uart.log
#   bash tools/uart-log.sh /dev/ttyUSB0 115200 uart.log
#   tail -f uart.log                       # watch live
#
# Robust: sets raw mode, appends, auto-reconnects if the adapter drops.
set -u
DEV="${1:-/dev/ttyUSB0}"
BAUD="${2:-115200}"
OUT="${3:-$(cd "$(dirname "$0")/.." && pwd)/uart.log}"

echo "uart-log: $DEV @${BAUD} -> $OUT   (Ctrl-C to stop; tail -f to watch)"
while true; do
  if [ -e "$DEV" ]; then
    stty -F "$DEV" "$BAUD" raw -echo 2>/dev/null
    printf '\n===== uart-log connect %s %s @%s =====\n' "$(date '+%F %T')" "$DEV" "$BAUD" >> "$OUT"
    cat "$DEV" >> "$OUT" 2>/dev/null
    printf '\n===== uart-log disconnect %s =====\n' "$(date '+%F %T')" >> "$OUT"
  fi
  sleep 1
done
