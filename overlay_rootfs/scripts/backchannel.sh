#!/bin/sh
# go2rtc backchannel handler
# Receives PCMA (a-law 8kHz mono) on stdin from go2rtc,
# writes raw data to FIFO for astream to decode and play.

FIFO=/tmp/audio_in.fifo

[ ! -p "$FIFO" ] && exit 1

# Close stdout (fd 1) so go2rtc's pipe capture doesn't interfere.
# Use dd to write stdin directly to FIFO via fd redirection.
exec 1>/dev/null
exec dd bs=320 <&0 > "$FIFO" 2>/dev/null
