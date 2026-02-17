# CppMonoSynth

A monophonic synthesizer for the [Critter & Guitari Organelle](https://www.critterandguitari.com/organelle), written as a single-file C++ program with direct ALSA audio output and OSC control. No Pure Data, no SuperCollider — just C++ talking to hardware.

## Features

- **4 waveforms** — Saw, Pulse (PWM), Triangle, Sine with PolyBLEP anti-aliasing
- **Waveform morphing** — smooth crossfade between adjacent waveforms via AUX button
- **LED color per waveform** — Saw=Red, Pulse=Yellow, Tri=Green, Sine=Cyan
- **Cytomic SVF filter** — trapezoidal-integration low-pass, unconditionally stable at all cutoff/resonance settings
- **Portamento** — one-pole glide in log2-frequency domain with legato note priority
- **PWM LFO** — triangle LFO modulates pulse width, rate tied to portamento time (K1)
- **AR envelope** — fast attack, knob-controlled release (10ms–2s)
- **OLED UI** — real-time parameter display + VU meter bar, rate-limited to ~20 fps with dirty checking
- **Last-note-priority** note stack with legato behavior

## Controls

| Knob | Parameter | Range |
|------|-----------|-------|
| K1 | Portamento + PWM LFO rate | 0–500 ms (0 = no glide, static PW) |
| K2 | Filter cutoff | 20 Hz – 18 kHz (exponential) |
| K3 | Filter resonance | 0–0.95 |
| K4 | Amp release | 10 ms – 2 s (exponential) |
| AUX | Cycle waveform | Saw → Pulse → Tri → Sine (morphs), LED changes color |
| Keys | Play notes | Organelle 1 keys (indices 1–24, MIDI 60–83) |

## Building

Cross-compile for the Organelle's ARM Cortex-A9 (armv7l) using Docker:

```bash
# Build the Docker image (one-time)
docker build --platform linux/arm/v7 -t monosynth-builder CppMonoSynth/

# Compile the binary
docker run --platform linux/arm/v7 --rm -v $(pwd)/CppMonoSynth:/build monosynth-builder make
```

The output is a `monosynth` ARM binary (~25KB). See the [Cross-Compilation](#cross-compilation) section below for why this specific setup is required.

## Deploying

1. Build via Docker (above)
2. Mount the Organelle's USB drive on your Mac
3. Copy files to the patch folder:
   ```bash
   cp monosynth run.sh /Volumes/ORGANELLE/Patches/CppMonoSynth/
   sync
   ```
4. Safely eject the USB drive
5. Boot the Organelle and select **CppMonoSynth** from the patch menu

## File Structure

```
CppMonoSynth/
├── monosynth.cpp   # Complete synth — oscillator, filter, envelope, OSC, ALSA, OLED
├── Makefile        # Build config (g++, -std=c++14, static libstdc++)
├── Dockerfile      # arm32v7/debian:stretch cross-compilation environment
├── run.sh          # Organelle launcher (kills JACK, chmod, ldd check, crash logging)
└── .gitignore      # Excludes compiled binary, crash.log, .DS_Store
```

---

## Organelle C++ Development Guide

Everything learned while getting a native C++ synth running on the Organelle.

### Cross-Compilation

The Organelle runs an ancient kernel (`Linux 3.14.14+`, 2015-era) with Arch Linux ARM and an **old glibc** (< 2.27).

**You MUST cross-compile using `arm32v7/debian:stretch`** (glibc 2.24). Using Debian Buster or newer produces binaries that fail at runtime with:

```
./monosynth: /lib/libc.so.6: version `GLIBC_2.27' not found
```

Key compiler flags:

| Flag | Why |
|------|-----|
| `-std=c++14` | Stretch ships GCC 6.3; C++17 not fully supported |
| `-static-libgcc -static-libstdc++` | Avoids C++ runtime mismatches — the Organelle's libstdc++ is old |
| `-lasound` | Links ALSA dynamically (acceptable — Organelle has libasound) |

The Dockerfile handles the Stretch archive migration (repos moved to `archive.debian.org`) and installs `g++`, `make`, and `libasound2-dev`.

Verify your binary only needs GLIBC_2.4:
```bash
objdump -T monosynth | grep GLIBC | sed 's/.*GLIBC_/GLIBC_/' | sort -uV
```

### ALSA Audio

The Organelle uses JACK by default. A C++ patch that talks to ALSA directly must kill JACK first:

```bash
killall -9 jackd 2>/dev/null
sleep 3   # ALSA device release is not instantaneous
```

ALSA device names:
- `hw:0` — direct hardware access (preferred, lowest latency)
- `plughw:0,0` — ALSA plugin layer (fallback, handles format conversion)

**Always retry `snd_pcm_open()`** with delays — JACK may not have released the device yet:

```cpp
for (int attempt = 0; attempt < 10; attempt++) {
    err = snd_pcm_open(&pcm, "hw:0", SND_PCM_STREAM_PLAYBACK, 0);
    if (err == 0) break;
    err = snd_pcm_open(&pcm, "plughw:0,0", SND_PCM_STREAM_PLAYBACK, 0);
    if (err == 0) break;
    usleep(500000);
}
```

Typical hw_params:
- Format: `S16_LE` (16-bit signed little-endian)
- Rate: 44100 Hz
- Period: 128 frames (~2.9 ms latency)
- Buffer: 512 frames (4 periods)
- Channels: 2 (stereo, both channels get the same mono signal)

### OSC Protocol

The Organelle's **mother** process manages the hardware UI and communicates with patches over UDP/OSC.

| Direction | Port | Description |
|-----------|------|-------------|
| Mother → Patch | 4000 | Knobs, keys, aux button, quit signal |
| Patch → Mother | 4001 | OLED display updates |

Messages received on port 4000:

| Address | Args | Description |
|---------|------|-------------|
| `/key` | `ii` (index, velocity) | Key press/release. **Index 0 = AUX button**, indices 1–24 = piano keys. Piano keys map to MIDI note `index + 59` (key 1 = middle C). Velocity 0 = note off. |
| `/knobs` | `iiiiii` (K1–K6) | All 6 knob values, range 0–1023 each. Sent on any knob change (~100 Hz when turning). |
| `/aux` | `i` (state) | AUX button. Value > 0 = pressed. **Unreliable** — mother may not send this; always handle AUX via `/key` index 0 instead. |
| `/quit` | (none) | Organelle is shutting down the patch. Set `g_running = 0`. |

Messages sent to port 4001:

| Address | Args | Description |
|---------|------|-------------|
| `/oled/line/N` | `s` (text) | Set text on OLED line N (1–5). Max ~21 characters. |
| `/oled/gBox` | `iiiii` (x1, y1, x2, y2, fill) | Draw filled/unfilled rectangle. OLED is 128x64 pixels. fill=1 for white, 0 for black. |
| `/led` | `i` (color) | Set the LED color. Values: 0=off, 1=red, 2=yellow, 3=green, 4=cyan, 5=blue, 6=purple, 7=white. |

The OSC socket should be **non-blocking** (`O_NONBLOCK`) so the audio loop never stalls waiting for messages. Use `SO_REUSEADDR` and retry `bind()` with delays in case the port is in `TIME_WAIT` from a previous patch.

### AUX Button

**The AUX button sends `/key` with index 0** — the same message format as piano keys. The mother _may_ also send a separate `/aux` message, but this is unreliable across firmware versions. Always handle AUX in your `/key` handler:

```cpp
if (index > 0 && index < 25) {
    // Piano keys (1–24)
    handleNote(index + 59, vel);
} else if (index == 0 && vel > 0) {
    // AUX button pressed
    handleAux();
}
```

Do NOT rely on the `/aux` OSC address as your primary handler. You can keep it as a fallback, but `/key 0` is the reliable path.

### LED

Control the Organelle's LED color by sending `/led` with an int to port 4001:

```cpp
osc_send_1i(mother_sock, &mother_addr, "/led", color);
```

| Value | Color |
|-------|-------|
| 0 | Off |
| 1 | Red |
| 2 | Yellow |
| 3 | Green |
| 4 | Cyan |
| 5 | Blue |
| 6 | Purple |
| 7 | White |

The LED persists until changed — no need to re-send. Useful for indicating mode (e.g., one color per waveform).

### OLED Display

The Organelle has a 128x64 monochrome OLED. The mother process provides 5 text lines and graphics primitives via OSC.

Best practices:
- **Rate-limit updates to ~50ms** (every ~2205 samples at 44100 Hz). Faster updates flood the mother process and cause lag.
- **Never force OLED redraws from the knobs handler.** Knob messages arrive at ~100 Hz. If you reset your OLED timer on every knob message, you'll send ~700 OSC packets/sec during knob turns, overwhelming the mother. Let the regular 50ms cycle handle all screen updates.
- **Dirty-check everything** before sending — text lines AND graphical elements (e.g., VU bar width). Only send when a value actually changes.
- **VU meter**: use `/oled/gBox` to draw a filled bar. Clear the area first (fill=0), then draw the bar (fill=1). Track the previous width and skip sends when unchanged.
- The OLED auto-clears on patch launch — no need to send blank lines at startup.

### run.sh Patterns

The `run.sh` script is the entry point called by the Organelle when a patch is selected. Key patterns:

**FAT32 strips execute permission:**
```bash
chmod +x /tmp/patch/monosynth
```

**ldd check before launch** — catch missing `.so` files before they cause a cryptic crash:
```bash
if ! ldd /tmp/patch/monosynth >/dev/null 2>&1; then
    oscsend localhost 4001 /oled/line/3 s "Missing libs!"
    exit 1
fi
```

**trap + wait (not exec)** — allows capturing the exit code for diagnostics:
```bash
trap 'kill -TERM $CHILD 2>/dev/null' TERM INT
/tmp/patch/monosynth 2>/tmp/monosynth.log &
CHILD=$!
wait $CHILD
EXIT_CODE=$?
```

**Write crash logs to USB**, not `/tmp/` (which is tmpfs and invisible from a Mac):
```bash
USB_LOG=/usbdrive/Patches/CppMonoSynth/crash.log
```

### Debugging Without SSH

The Organelle has SSH on port 22 (user `root`), but when you don't have network access:

1. **OLED messages** — show progress at each init stage. If the screen goes blank or freezes, the last message tells you where it failed.
2. **Distinct exit codes** — the C++ binary uses different codes per failure: 2=socket, 3=bind, 4=ALSA open, 5=hw_params. `run.sh` displays the code on the OLED.
3. **crash.log on USB** — `run.sh` writes stderr, `ldd` output, and system diagnostics to a file on the USB drive. Plug the USB into your Mac to read it.

### C++ Architecture Patterns

**Create the mother socket first** — before anything else in `main()`. This gives you OLED diagnostic output for all subsequent failures:

```cpp
int mother_sock = socket(AF_INET, SOCK_DGRAM, 0);
// ... setup mother_addr for port 4001 ...
osc_send_str(mother_sock, &mother_addr, "/oled/line/2", "Init...");
// Now if ALSA fails, you can display the error on screen
```

**Retry with delays** — both `bind()` and `snd_pcm_open()` can fail transiently. Retry up to 10 times with 500ms delays. Display the attempt count on OLED so you can see it's not frozen.

**Smooth ALL signal-path parameters** — knob values from OSC arrive at irregular intervals (~100 Hz). Any parameter that directly multiplies or shapes the audio signal (volume, cutoff, resonance) will produce audible zipper noise if applied as raw step changes. Apply one-pole smoothing per sample in the audio loop:

```cpp
static constexpr float PARAM_SMOOTH_COEFF = 0.002f;

// In audio loop (per sample):
cutoffSmooth += PARAM_SMOOTH_COEFF * (cutoffTarget - cutoffSmooth);
resoSmooth   += PARAM_SMOOTH_COEFF * (resoTarget - resoSmooth);
volSmooth    += PARAM_SMOOTH_COEFF * (volTarget - volSmooth);
```

Parameters that set time constants (portamento rate, envelope release) don't need audio-rate smoothing — they control the speed of change, not the signal amplitude directly.

**Soft clip the output** — prevents harsh digital distortion if the synth output exceeds ±1.0:

```cpp
if (s > 1.0f) s = 1.0f;
else if (s < -1.0f) s = -1.0f;
```
