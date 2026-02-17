// CppMonoSynth — single-file C++ monosynth for Critter & Guitari Organelle
// PolyBLEP multi-waveform → Cytomic SVF LPF → AR envelope → ALSA hw:0
// OSC control via UDP port 4000

#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr int    SAMPLE_RATE         = 44100;
static constexpr int    PERIOD_FRAMES       = 128;
static constexpr int    CHANNELS            = 2;
static constexpr int    OSC_PORT            = 4000;
static constexpr int    MOTHER_PORT         = 4001;
static constexpr int    NOTE_STACK_SZ       = 16;
static constexpr float  TWO_PI              = 6.283185307f;
static constexpr float  PI                  = 3.141592654f;
static constexpr float  INV_SR              = 1.0f / SAMPLE_RATE;
static constexpr float  ATTACK_MS           = 5.0f;
static constexpr int    NUM_WAVEFORMS       = 4;
static constexpr int    OLED_INTERVAL       = 2205;  // ~50ms at 44100Hz
static constexpr float  PARAM_SMOOTH_COEFF = 0.002f;

static const char* WAVE_NAMES[] = {"Saw", "PWM", "Tri", "Sine"};
static const int   LED_COLORS[] = {1, 2, 3, 4};  // Red, Yellow, Green, Cyan

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int) { g_running = 0; }

// ─── PolyBLEP residual ──────────────────────────────────────────────────────

static inline float polyblep(float phase, float dt) {
    if (phase < dt) {
        float t = phase / dt;
        return t + t - t * t - 1.0f;
    }
    if (phase > 1.0f - dt) {
        float t = (phase - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// ─── Oscillator ──────────────────────────────────────────────────────────────

struct Oscillator {
    float phase = 0.0f;
    float freq  = 440.0f;
    float pulseWidth = 0.5f;

    void advance() {
        phase += freq * INV_SR;
        if (phase >= 1.0f) phase -= 1.0f;
    }

    float saw() const {
        float dt = freq * INV_SR;
        float s = 2.0f * phase - 1.0f;
        s -= polyblep(phase, dt);
        return s;
    }

    float pulse() const {
        float dt = freq * INV_SR;
        float s = (phase < pulseWidth) ? 1.0f : -1.0f;
        s += polyblep(phase, dt);          // rising edge at phase=0
        float shifted = phase - pulseWidth;
        if (shifted < 0.0f) shifted += 1.0f;
        s -= polyblep(shifted, dt);        // falling edge at phase=pw
        return s;
    }

    float triangle() const {
        return (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
    }

    float sine() const {
        return sinf(TWO_PI * phase);
    }

    float waveform(int idx) const {
        switch (idx) {
        case 0: return saw();
        case 1: return pulse();
        case 2: return triangle();
        case 3: return sine();
        default: return saw();
        }
    }
};

// ─── Portamento (one-pole in log2-freq domain) ───────────────────────────────

struct Portamento {
    float target   = 0.0f;   // log2(freq)
    float current  = 0.0f;   // log2(freq)
    float coeff    = 1.0f;   // 1.0 = instant

    void setTime(float ms) {
        if (ms < 1.0f) { coeff = 1.0f; return; }
        float samples = ms * 0.001f * SAMPLE_RATE;
        coeff = 1.0f - expf(-1.0f / samples);
    }

    void setTarget(float freqHz) {
        target = log2f(freqHz);
    }

    void snap(float freqHz) {
        target  = log2f(freqHz);
        current = target;
    }

    float tick() {
        current += coeff * (target - current);
        return exp2f(current);
    }
};

// ─── Triangle LFO (for PWM modulation, tied to portamento time) ─────────────

struct TriLFO {
    float phase = 0.0f;
    float freq  = 0.0f;

    void setPeriodMs(float ms) {
        freq = (ms < 1.0f) ? 0.0f : 1000.0f / ms;
    }

    float tick() {
        if (freq <= 0.0f) return 0.0f;
        phase += freq * INV_SR;
        if (phase >= 1.0f) phase -= 1.0f;
        return (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
    }
};

// ─── Cytomic SVF (trapezoidal integration, unconditionally stable) ───────────

struct SVFilter {
    float ic1eq = 0.0f;
    float ic2eq = 0.0f;
    float g     = 0.0f;
    float k     = 2.0f;   // k = 2 - 2*reso
    float a1    = 0.0f;
    float a2    = 0.0f;
    float a3    = 0.0f;

    void setParams(float cutoffHz, float reso) {
        float fc = cutoffHz;
        if (fc < 20.0f)    fc = 20.0f;
        if (fc > 20000.0f) fc = 20000.0f;
        g  = tanf(PI * fc * INV_SR);
        k  = 2.0f - 2.0f * reso;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    // Returns low-pass output
    float tick(float v0) {
        float v3 = v0 - ic2eq;
        float v1 = a1 * ic1eq + a2 * v3;
        float v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = 2.0f * v1 - ic1eq;
        ic2eq = 2.0f * v2 - ic2eq;
        return v2;
    }
};

// ─── AR Envelope ─────────────────────────────────────────────────────────────

struct Envelope {
    enum Stage { OFF, ATTACK, RELEASE };
    Stage stage       = OFF;
    float value       = 0.0f;
    float attackCoeff = 0.0f;
    float releaseCoeff= 0.0f;

    Envelope() {
        setAttack(ATTACK_MS);
        setRelease(200.0f);
    }

    void setAttack(float ms) {
        float samples = ms * 0.001f * SAMPLE_RATE;
        attackCoeff = 1.0f - expf(-1.0f / samples);
    }

    void setRelease(float ms) {
        if (ms < 1.0f) ms = 1.0f;
        float samples = ms * 0.001f * SAMPLE_RATE;
        releaseCoeff = expf(-1.0f / samples);
    }

    void gate(bool on) {
        if (on) stage = ATTACK;
        else if (stage == ATTACK) stage = RELEASE;
    }

    float tick() {
        switch (stage) {
        case ATTACK:
            value += attackCoeff * (1.0f - value);
            if (value > 0.999f) { value = 1.0f; }
            break;
        case RELEASE:
            value *= releaseCoeff;
            if (value < 0.0001f) { value = 0.0f; stage = OFF; }
            break;
        case OFF:
            break;
        }
        return value;
    }
};

// ─── Note Stack (last-note priority) ─────────────────────────────────────────

struct NoteStack {
    int notes[NOTE_STACK_SZ];
    int size = 0;

    void push(int note) {
        // Remove if already present
        remove(note);
        if (size < NOTE_STACK_SZ) {
            notes[size++] = note;
        }
    }

    void remove(int note) {
        for (int i = 0; i < size; i++) {
            if (notes[i] == note) {
                for (int j = i; j < size - 1; j++)
                    notes[j] = notes[j + 1];
                size--;
                return;
            }
        }
    }

    int top() const { return size > 0 ? notes[size - 1] : -1; }
    bool empty() const { return size == 0; }
};

// ─── MIDI note → frequency ──────────────────────────────────────────────────

static inline float mtof(int note) {
    return 440.0f * exp2f((note - 69) / 12.0f);
}

// ─── Voice ───────────────────────────────────────────────────────────────────

struct Voice {
    NoteStack  stack;
    Oscillator osc;
    Portamento porta;
    SVFilter   filt;
    Envelope   env;
    bool       gateOn = false;
    int        targetWaveform = 0;
    float      morphPos = 0.0f;

    void noteOn(int note) {
        bool legato = gateOn;
        stack.push(note);
        float freq = mtof(note);
        if (legato) {
            porta.setTarget(freq);
        } else {
            porta.snap(freq);
            env.gate(true);
        }
        gateOn = true;
    }

    void noteOff(int note) {
        stack.remove(note);
        if (stack.empty()) {
            env.gate(false);
            gateOn = false;
        } else {
            // Glide to the new top note (legato)
            porta.setTarget(mtof(stack.top()));
        }
    }

    float tick() {
        osc.freq = porta.tick();
        osc.advance();

        // Smooth morphPos toward targetWaveform (reuses portamento speed)
        float target = (float)targetWaveform;
        morphPos += porta.coeff * (target - morphPos);
        if (fabsf(morphPos - target) < 0.001f) morphPos = target;

        // Crossfade between adjacent waveforms during morph
        float s;
        int lo = (int)floorf(morphPos);
        float frac = morphPos - (float)lo;
        int loIdx = ((lo % NUM_WAVEFORMS) + NUM_WAVEFORMS) % NUM_WAVEFORMS;

        if (frac < 0.001f) {
            s = osc.waveform(loIdx);
        } else {
            int hiIdx = (loIdx + 1) % NUM_WAVEFORMS;
            s = osc.waveform(loIdx) * (1.0f - frac) + osc.waveform(hiIdx) * frac;
        }

        s = filt.tick(s);
        s *= env.tick();
        return s;
    }
};

// ─── OSC helpers ─────────────────────────────────────────────────────────────

// Round up to next multiple of 4
static inline int osc_pad(int n) { return (n + 3) & ~3; }

// Parse an int32 from OSC data (big-endian)
static inline int32_t osc_int(const uint8_t* p) {
    return (int32_t)ntohl(*(const uint32_t*)p);
}

// ─── OSC display helper (send string to mother on port 4001) ─────────────────

static void osc_send_str(int sock, struct sockaddr_in* addr,
                          const char* path, const char* text) {
    uint8_t buf[256];
    int plen = osc_pad((int)strlen(path) + 1);
    int tlen = osc_pad((int)strlen(text) + 1);
    int total = plen + 4 + tlen;  // path + ",s\0\0" + text
    if (total > (int)sizeof(buf)) return;
    memset(buf, 0, total);
    memcpy(buf, path, strlen(path));
    int off = plen;
    buf[off++] = ','; buf[off++] = 's'; buf[off++] = 0; buf[off++] = 0;
    memcpy(buf + plen + 4, text, strlen(text));
    sendto(sock, buf, total, 0,
           (struct sockaddr*)addr, sizeof(*addr));
}

// ─── OSC helper: send 5 ints (for /oled/gBox x1 y1 x2 y2 fill) ─────────────

static void osc_send_5i(int sock, struct sockaddr_in* addr,
                         const char* path, int v0, int v1, int v2, int v3, int v4) {
    uint8_t buf[256];
    int plen = osc_pad((int)strlen(path) + 1);
    // Type tag: ,iiiii + pad to 4-byte boundary = 8 bytes
    int total = plen + 8 + 5 * 4;
    if (total > (int)sizeof(buf)) return;
    memset(buf, 0, total);
    memcpy(buf, path, strlen(path));
    int off = plen;
    buf[off++] = ','; buf[off++] = 'i'; buf[off++] = 'i'; buf[off++] = 'i';
    buf[off++] = 'i'; buf[off++] = 'i'; off += 2;  // null + pad
    int32_t vals[] = {v0, v1, v2, v3, v4};
    for (int j = 0; j < 5; j++) {
        uint32_t nv = htonl((uint32_t)vals[j]);
        memcpy(buf + off, &nv, 4);
        off += 4;
    }
    sendto(sock, buf, total, 0,
           (struct sockaddr*)addr, sizeof(*addr));
}

// ─── OSC helper: send 1 int (for /led) ──────────────────────────────────────

static void osc_send_1i(int sock, struct sockaddr_in* addr,
                         const char* path, int v0) {
    uint8_t buf[64];
    int plen = osc_pad((int)strlen(path) + 1);
    int total = plen + 4 + 4;  // path + ",i\0\0" + int
    if (total > (int)sizeof(buf)) return;
    memset(buf, 0, total);
    memcpy(buf, path, strlen(path));
    int off = plen;
    buf[off++] = ','; buf[off++] = 'i'; buf[off++] = 0; buf[off++] = 0;
    uint32_t nv = htonl((uint32_t)v0);
    memcpy(buf + off, &nv, 4);
    sendto(sock, buf, total, 0,
           (struct sockaddr*)addr, sizeof(*addr));
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    // Signal handling
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    // ── UDP socket for sending to mother (port 4001) — create FIRST for OLED diag ──
    int mother_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in mother_addr{};
    mother_addr.sin_family      = AF_INET;
    mother_addr.sin_port        = htons(MOTHER_PORT);
    mother_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    osc_send_str(mother_sock, &mother_addr, "/oled/line/2", "Init sockets...");

    // ── UDP socket for OSC receive (port 4000) ──
    int osc_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (osc_sock < 0) {
        perror("socket");
        osc_send_str(mother_sock, &mother_addr, "/oled/line/2", "socket() FAIL");
        sleep(5); close(mother_sock); return 2;
    }
    int reuse = 1;
    setsockopt(osc_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // Non-blocking
    fcntl(osc_sock, F_SETFL, O_NONBLOCK);

    struct sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(OSC_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Retry bind in case port 4000 is still in TIME_WAIT
    int bind_err = -1;
    for (int attempt = 0; attempt < 10; attempt++) {
        bind_err = bind(osc_sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr));
        if (bind_err == 0) break;
        fprintf(stderr, "bind attempt %d failed: %s\n", attempt + 1, strerror(errno));
        char msg[32];
        snprintf(msg, sizeof(msg), "bind retry %d/10", attempt + 1);
        osc_send_str(mother_sock, &mother_addr, "/oled/line/2", msg);
        usleep(500000);
    }
    if (bind_err < 0) {
        perror("bind");
        osc_send_str(mother_sock, &mother_addr, "/oled/line/2", "bind:4000 FAIL");
        sleep(5); close(osc_sock); close(mother_sock); return 3;
    }

    osc_send_str(mother_sock, &mother_addr, "/oled/line/2", "Sockets OK");

    // ── ALSA setup (retry in case JACK hasn't released the device yet) ──
    snd_pcm_t* pcm = nullptr;
    int err = -1;
    for (int attempt = 0; attempt < 10; attempt++) {
        err = snd_pcm_open(&pcm, "hw:0", SND_PCM_STREAM_PLAYBACK, 0);
        if (err == 0) break;
        err = snd_pcm_open(&pcm, "plughw:0,0", SND_PCM_STREAM_PLAYBACK, 0);
        if (err == 0) break;
        fprintf(stderr, "ALSA open attempt %d failed: %s\n", attempt + 1, snd_strerror(err));
        char msg[32];
        snprintf(msg, sizeof(msg), "ALSA retry %d/10", attempt + 1);
        osc_send_str(mother_sock, &mother_addr, "/oled/line/2", msg);
        usleep(500000);
    }
    if (err < 0) {
        fprintf(stderr, "ALSA open: all attempts failed: %s\n", snd_strerror(err));
        osc_send_str(mother_sock, &mother_addr, "/oled/line/2", "ALSA open FAIL");
        sleep(5);
        close(osc_sock); close(mother_sock);
        return 4;
    }
    osc_send_str(mother_sock, &mother_addr, "/oled/line/2", "ALSA opened");

    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm, hw_params);
    snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw_params, CHANNELS);
    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, nullptr);
    snd_pcm_uframes_t period = PERIOD_FRAMES;
    snd_pcm_hw_params_set_period_size_near(pcm, hw_params, &period, nullptr);
    snd_pcm_uframes_t bufsize = PERIOD_FRAMES * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw_params, &bufsize);

    err = snd_pcm_hw_params(pcm, hw_params);
    if (err < 0) {
        fprintf(stderr, "ALSA hw_params: %s\n", snd_strerror(err));
        osc_send_str(mother_sock, &mother_addr, "/oled/line/2", "hw_params FAIL");
        sleep(5);
        snd_pcm_close(pcm); close(osc_sock); close(mother_sock);
        return 5;
    }
    snd_pcm_prepare(pcm);
    osc_send_str(mother_sock, &mother_addr, "/oled/line/2", "Audio ready");
    osc_send_1i(mother_sock, &mother_addr, "/led", LED_COLORS[0]);

    // ── Voice + params ──
    Voice voice;
    TriLFO pwmLfo;
    float volTarget    = 0.5f;
    float volSmooth    = 0.5f;
    float cutoffTarget = 8000.0f;
    float cutoffSmooth = 8000.0f;
    float resoTarget   = 0.0f;
    float resoSmooth   = 0.0f;
    int   oledCounter  = OLED_INTERVAL;  // trigger immediate OLED draw
    float peakLevel    = 0.0f;

    // Display values for OLED formatting
    float dispPortoMs   = 0.0f;
    float dispCutoffHz  = 8000.0f;
    float dispReso      = 0.0f;
    float dispReleaseMs = 200.0f;

    // Previous OLED strings / VU for dirty checking
    char prevLine1[32] = "";
    char prevLine2[32] = "";
    char prevLine3[32] = "";
    char prevLine4[32] = "";
    int  prevVuWidth    = -1;
    char prevLine5[32] = "";

    voice.filt.setParams(cutoffTarget, resoTarget);
    voice.porta.setTime(0.0f);

    // Audio buffer
    int16_t buf[PERIOD_FRAMES * CHANNELS];
    uint8_t osc_buf[512];

    // ── Main audio loop ──
    while (g_running) {
        // Poll OSC messages (non-blocking)
        for (;;) {
            ssize_t n = recv(osc_sock, osc_buf, sizeof(osc_buf), 0);
            if (n <= 0) break;

            // Parse OSC address
            const char* addr = (const char*)osc_buf;
            int addr_len = osc_pad((int)strnlen(addr, n) + 1);
            if (addr_len >= n) continue;

            // Compute args offset from actual type tag length
            const char* typetag = (const char*)(osc_buf + addr_len);
            int args_off = addr_len + osc_pad((int)strnlen(typetag, n - addr_len) + 1);

            if (strcmp(addr, "/key") == 0 && n >= args_off + 8) {
                // /key <index:i> <vel:i>
                int32_t index = osc_int(osc_buf + args_off);
                int32_t vel   = osc_int(osc_buf + args_off + 4);
                if (index > 0 && index < 25) {  // keys 1-24
                    int note = index + 59;
                    if (vel > 0) {
                        voice.noteOn(note);
                    } else {
                        voice.noteOff(note);
                    }
                } else if (index == 0 && vel > 0) {  // AUX button
                    voice.targetWaveform = (voice.targetWaveform + 1) % NUM_WAVEFORMS;
                    osc_send_1i(mother_sock, &mother_addr, "/led",
                                LED_COLORS[voice.targetWaveform]);
                }
            }
            else if (strcmp(addr, "/knobs") == 0 && n >= args_off + 20) {
                // /knobs <k1> <k2> <k3> <k4> <k5> (K6 ignored if present)
                int32_t k1 = osc_int(osc_buf + args_off);
                int32_t k2 = osc_int(osc_buf + args_off + 4);
                int32_t k3 = osc_int(osc_buf + args_off + 8);
                int32_t k4 = osc_int(osc_buf + args_off + 12);
                int32_t k5 = osc_int(osc_buf + args_off + 16);

                // K1: Portamento 0–500ms linear (also sets PWM LFO rate)
                float portoMs = k1 * (500.0f / 1023.0f);
                voice.porta.setTime(portoMs);
                pwmLfo.setPeriodMs(portoMs);
                dispPortoMs = portoMs;

                // K2: Filter cutoff 20–18kHz exponential (target only, smoothed in audio loop)
                cutoffTarget = 20.0f * powf(900.0f, k2 / 1023.0f);
                dispCutoffHz = cutoffTarget;

                // K3: Filter resonance 0–0.95 (target only)
                resoTarget = k3 * (0.95f / 1023.0f);
                dispReso = resoTarget;

                // K4: Amp release 10–2000ms exponential
                float releaseMs = 10.0f * powf(200.0f, k4 / 1023.0f);
                voice.env.setRelease(releaseMs);
                dispReleaseMs = releaseMs;

                // K5: Master volume 0–1
                volTarget = k5 / 1023.0f;

                // OLED updates on regular 50ms cycle (no forced redraw)
            }
            else if (strcmp(addr, "/aux") == 0 && n >= args_off + 4) {
                int32_t auxVal = osc_int(osc_buf + args_off);
                if (auxVal > 0) {
                    voice.targetWaveform = (voice.targetWaveform + 1) % NUM_WAVEFORMS;
                    osc_send_1i(mother_sock, &mother_addr, "/led",
                                LED_COLORS[voice.targetWaveform]);
                }
            }
            else if (strcmp(addr, "/quit") == 0) {
                g_running = 0;
            }
        }

        // Fill audio buffer
        for (int i = 0; i < PERIOD_FRAMES; i++) {
            // Smooth control parameters (one-pole)
            cutoffSmooth += PARAM_SMOOTH_COEFF * (cutoffTarget - cutoffSmooth);
            resoSmooth   += PARAM_SMOOTH_COEFF * (resoTarget - resoSmooth);
            volSmooth    += PARAM_SMOOTH_COEFF * (volTarget - volSmooth);

            voice.osc.pulseWidth = 0.5f + 0.4f * pwmLfo.tick();
            voice.filt.setParams(cutoffSmooth, resoSmooth);

            float s = voice.tick() * volSmooth;
            // Soft clip
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;

            // Track peak level for VU
            float absS = fabsf(s);
            if (absS > peakLevel) peakLevel = absS;

            int16_t sample = (int16_t)(s * 32767.0f);
            buf[i * 2]     = sample;  // L
            buf[i * 2 + 1] = sample;  // R
        }

        // Write to ALSA
        snd_pcm_sframes_t frames = snd_pcm_writei(pcm, buf, PERIOD_FRAMES);
        if (frames < 0) {
            // EPIPE = underrun; try to recover
            frames = snd_pcm_recover(pcm, (int)frames, 0);
            if (frames < 0) {
                fprintf(stderr, "ALSA write error: %s\n", snd_strerror((int)frames));
                osc_send_str(mother_sock, &mother_addr, "/oled/line/2", "ALSA write ERR");
                sleep(3);
                break;
            }
        }

        // ── OLED update (every ~50ms) ──
        oledCounter += PERIOD_FRAMES;
        if (oledCounter >= OLED_INTERVAL) {
            oledCounter -= OLED_INTERVAL;

            char line[32];

            // Line 1: Portamento
            snprintf(line, sizeof(line), "Porto: %dms", (int)dispPortoMs);
            if (strcmp(line, prevLine1) != 0) {
                osc_send_str(mother_sock, &mother_addr, "/oled/line/1", line);
                strcpy(prevLine1, line);
            }

            // Line 2: Cutoff (Hz or kHz)
            if (dispCutoffHz >= 1000.0f)
                snprintf(line, sizeof(line), "Cutoff: %.1fkHz", dispCutoffHz / 1000.0f);
            else
                snprintf(line, sizeof(line), "Cutoff: %dHz", (int)dispCutoffHz);
            if (strcmp(line, prevLine2) != 0) {
                osc_send_str(mother_sock, &mother_addr, "/oled/line/2", line);
                strcpy(prevLine2, line);
            }

            // Line 3: Resonance
            snprintf(line, sizeof(line), "Reso: %.2f", dispReso);
            if (strcmp(line, prevLine3) != 0) {
                osc_send_str(mother_sock, &mother_addr, "/oled/line/3", line);
                strcpy(prevLine3, line);
            }

            // Line 4: Release
            if (dispReleaseMs >= 1000.0f)
                snprintf(line, sizeof(line), "Release: %.1fs", dispReleaseMs / 1000.0f);
            else
                snprintf(line, sizeof(line), "Release: %dms", (int)dispReleaseMs);
            if (strcmp(line, prevLine4) != 0) {
                osc_send_str(mother_sock, &mother_addr, "/oled/line/4", line);
                strcpy(prevLine4, line);
            }

            // Line 5: Waveform name (with morph indicator)
            float morphFrac = voice.morphPos - floorf(voice.morphPos);
            int morphLo = ((int)floorf(voice.morphPos) % NUM_WAVEFORMS + NUM_WAVEFORMS) % NUM_WAVEFORMS;
            if (morphFrac > 0.001f) {
                int morphHi = (morphLo + 1) % NUM_WAVEFORMS;
                snprintf(line, sizeof(line), "%s > %s", WAVE_NAMES[morphLo], WAVE_NAMES[morphHi]);
            } else {
                snprintf(line, sizeof(line), "%s", WAVE_NAMES[morphLo]);
            }
            if (strcmp(line, prevLine5) != 0) {
                osc_send_str(mother_sock, &mother_addr, "/oled/line/5", line);
                strcpy(prevLine5, line);
            }

            // VU bar: graphical filled rectangle at bottom of OLED (128x64)
            int vuWidth = (int)(peakLevel * 122.0f);
            if (vuWidth > 122) vuWidth = 122;
            if (vuWidth != prevVuWidth) {
                osc_send_5i(mother_sock, &mother_addr, "/oled/gBox", 3, 55, 125, 62, 0);
                if (vuWidth > 0)
                    osc_send_5i(mother_sock, &mother_addr, "/oled/gBox", 3, 55, 3 + vuWidth, 62, 1);
                prevVuWidth = vuWidth;
            }

            // Peak decay
            peakLevel *= 0.95f;
        }
    }

    // Cleanup
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    close(osc_sock);
    close(mother_sock);

    return 0;
}
