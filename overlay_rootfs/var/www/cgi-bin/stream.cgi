#!/bin/sh

echo "Content-Type: text/plain"
echo ""

FIFO="/tmp/audio_in.fifo"

if [ ! -p "$FIFO" ]; then
  echo "error: fifo not found"
  exit 1
fi

cat > "$FIFO"
echo "ok"
