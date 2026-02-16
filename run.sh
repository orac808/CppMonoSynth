#!/bin/sh
# CppMonoSynth launcher for Organelle
USB_LOG=/usbdrive/Patches/CppMonoSynth/crash.log

oscsend localhost 4001 /oled/line/1 s "CppMonoSynth"
oscsend localhost 4001 /oled/line/2 s "Starting..."

# Kill JACK — we use ALSA directly
killall -9 jackd 2>/dev/null
sleep 3
oscsend localhost 4001 /oled/line/2 s "JACK killed"

# Ensure execute permission (FAT32 USB strips +x)
chmod +x /tmp/patch/monosynth

oscsend localhost 4001 /oled/line/2 s "Launching..."

# Check for missing shared libs before exec
if ! ldd /tmp/patch/monosynth >/dev/null 2>/tmp/monosynth_ldd.log; then
    oscsend localhost 4001 /oled/line/2 s "Lib missing!"
    oscsend localhost 4001 /oled/line/3 s "See ldd log"
    cat /tmp/monosynth_ldd.log >> /tmp/monosynth_diag.log 2>&1
    sleep 10
fi

# Diagnostics dump
{
  echo "=== date ===" ; date
  echo "=== uname ===" ; uname -a
  echo "=== ldd ===" ; ldd /tmp/patch/monosynth 2>&1
  echo "=== file ===" ; file /tmp/patch/monosynth
  echo "=== ls -la ===" ; ls -la /tmp/patch/monosynth
  echo "=== aplay -l ===" ; aplay -l 2>&1
  echo "=== port check ===" ; ss -tulpn 2>/dev/null || netstat -tulpn 2>/dev/null
  echo "=== proc check ===" ; ps aux 2>/dev/null | grep -i 'jack\|mono\|mother' || true
} > /tmp/monosynth_diag.log 2>&1

# Forward SIGTERM to child so Organelle can stop the patch
trap 'kill -TERM $CHILD 2>/dev/null' TERM INT

# Run binary (not exec) so we can capture exit code for OLED diagnostics
/tmp/patch/monosynth 2>/tmp/monosynth.log &
CHILD=$!
wait $CHILD
EXIT_CODE=$?

# Exit code key: 0=clean, 2=socket, 3=bind, 4=ALSA open, 5=hw_params
# If still 1, crash happened before our code (dynamic linker, segfault, etc.)
if [ "$EXIT_CODE" -ne 0 ]; then
    oscsend localhost 4001 /oled/line/2 s "Exit code: $EXIT_CODE"
    oscsend localhost 4001 /oled/line/3 s "Logs on USB"

    # Collect all logs and write to USB so Mac can read them
    {
      echo "========================================"
      echo "CppMonoSynth crash report"
      echo "Exit code: $EXIT_CODE"
      echo "========================================"
      echo ""
      echo "=== stderr (monosynth.log) ==="
      cat /tmp/monosynth.log 2>/dev/null
      echo ""
      echo "=== diagnostics ==="
      cat /tmp/monosynth_diag.log 2>/dev/null
      echo ""
      echo "=== ldd log ==="
      cat /tmp/monosynth_ldd.log 2>/dev/null
    } > "$USB_LOG" 2>&1
    sync

    sleep 30
fi
