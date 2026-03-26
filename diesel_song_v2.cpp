// Diesel Engine Synth v2 — improved song render
// Better timing, reverb, delay, clearer pitch, atmospheric mix
//
// Build: clang++ -std=c++17 -O2 -o diesel_song_v2 diesel_song_v2.cpp -lm

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <vector>

static constexpr double PI = 3.14159265358979323846;
static constexpr double TWO_PI = 2.0 * PI;
static constexpr int SAMPLE_RATE = 44100;
static constexpr int NUM_CYLINDERS = 6;

// ============================================================================
// Engine model
// ============================================================================

struct EngineParams {
    double V_d = 0.002, V_c = 0.000125, R_r = 4.0, L_pm = 0.142;
    double gamma = 1.35;
    double firing_offsets[NUM_CYLINDERS] = {0, 240, 120, 300, 60, 180};
    double r_c() const { return (V_d + V_c) / V_c; }
};

class OnePole {
    double y1 = 0.0;
public:
    void reset() { y1 = 0.0; }
    double process(double x, double cutoff_hz, double sr) {
        double w = TWO_PI * cutoff_hz / sr;
        double a = w / (1.0 + w);
        y1 = a * x + (1.0 - a) * y1;
        return y1;
    }
};

class DCBlocker {
    double x1 = 0.0, y1 = 0.0;
public:
    void reset() { x1 = y1 = 0.0; }
    double process(double x) {
        double y = x - x1 + 0.995 * y1;
        x1 = x; y1 = y;
        return y;
    }
};

class NoiseGen {
    uint32_t state = 12345;
public:
    float next() {
        state = state * 1664525u + 1013904223u;
        return (float)(int32_t)state / (float)INT32_MAX;
    }
};

class CylinderModel {
public:
    EngineParams params;
    double firing_offset_deg = 0.0;

    double cylinderVolume(double theta_rad) const {
        double cos_t = std::cos(theta_rad), sin_t = std::sin(theta_rad);
        double R_r = params.R_r, r_c = params.r_c();
        return params.V_c + (params.V_c / 2.0) * (r_c - 1.0) *
               (R_r + 1.0 - cos_t - std::sqrt(std::max(0.0, R_r * R_r - sin_t * sin_t)));
    }

    double computePressure(double global_crank_deg, double fuel_rate) const {
        double local_deg = std::fmod(global_crank_deg - firing_offset_deg, 720.0);
        if (local_deg < 0.0) local_deg += 720.0;
        double r_c = params.r_c(), gamma = params.gamma, pressure = 0.0;

        if (local_deg < 180.0) {
            pressure = -0.05 * std::sin(PI * local_deg / 180.0);
        } else if (local_deg < 360.0) {
            double comp_frac = (local_deg - 180.0) / 180.0;
            double theta = PI * (1.0 - comp_frac);
            double V = cylinderVolume(theta), V_bdc = cylinderVolume(PI);
            double p_ratio = std::pow(V_bdc / V, gamma);
            pressure = (p_ratio - 1.0) / (std::pow(r_c, gamma) - 1.0);
        } else if (local_deg < 540.0) {
            double exp_frac = (local_deg - 360.0) / 180.0;
            double theta = PI * exp_frac;
            double V = cylinderVolume(theta), V_tdc = cylinderVolume(0.0);
            double p_peak = std::pow(r_c, gamma);
            double comb_boost = fuel_rate * 0.6;
            double delay_frac = (5.0 + (1.0 - fuel_rate) * 10.0) / 180.0;
            double envelope = (exp_frac < delay_frac)
                ? std::pow(exp_frac / delay_frac, 2.0)
                : std::exp(-3.0 * (exp_frac - delay_frac) / (1.0 - delay_frac));
            double p_exp = (p_peak + comb_boost * p_peak * envelope) * std::pow(V_tdc / V, gamma);
            double p_max = p_peak * (1.0 + comb_boost);
            pressure = std::clamp((p_exp - 1.0) / (p_max - 1.0), 0.0, 1.0);
        } else {
            double exh_frac = (local_deg - 540.0) / 180.0;
            pressure = std::exp(-8.0 * exh_frac) * fuel_rate * 0.4;
        }
        return pressure;
    }
};

class TurboModel {
    double phase = 0.0, speed = 0.0;
public:
    void reset() { phase = speed = 0.0; }
    double process(double rpm, double fuel, double dt) {
        double target = fuel * std::sqrt(std::max(rpm, 1.0) / 2000.0);
        speed += (target - speed) * dt / 0.5;
        double freq = 2000.0 + speed * 6000.0;
        phase += freq * dt;
        if (phase > 1.0) phase -= 1.0;
        double whine = std::sin(TWO_PI * phase) * 0.7
                     + std::sin(TWO_PI * phase * 2.0) * 0.2
                     + std::sin(TWO_PI * phase * 3.0) * 0.1;
        return whine * speed * 0.1;
    }
};

// ============================================================================
// Voice with higher exhaust cutoff for clearer pitch
// ============================================================================

struct Voice {
    EngineParams params;
    std::array<CylinderModel, NUM_CYLINDERS> cylinders;
    TurboModel turbo;
    NoiseGen noise;
    OnePole exhaust_lpf;
    DCBlocker dc_block;
    double crank_angle_deg = 0.0;
    double rpm = 0.0, fuel = 0.0;
    double rpm_smooth = 0.0, fuel_smooth = 0.0;
    bool active = false;
    double env = 0.0;
    double exhaust_cutoff = 800.0; // Higher default for clearer pitch

    void init() {
        for (int i = 0; i < NUM_CYLINDERS; i++) {
            cylinders[i].params = params;
            cylinders[i].firing_offset_deg = params.firing_offsets[i];
        }
    }

    void noteOn(double r, double f) {
        rpm = r; fuel = f; active = true;
        init();
    }
    void noteOff() { active = false; }

    float process(double dt) {
        double smoothing = 1.0 - std::exp(-dt / 0.025);
        rpm_smooth += (rpm - rpm_smooth) * smoothing;
        fuel_smooth += (fuel - fuel_smooth) * smoothing;

        if (active) {
            env += (1.0 - env) * (1.0 - std::exp(-dt / 0.06));
        } else {
            env += (0.0 - env) * (1.0 - std::exp(-dt / 0.12));
            rpm_smooth += (0.0 - rpm_smooth) * (1.0 - std::exp(-dt / 0.15));
        }

        if (env < 0.001 && !active) return 0.0f;

        crank_angle_deg += rpm_smooth * 6.0 * dt;
        if (crank_angle_deg >= 720.0) crank_angle_deg -= 720.0;

        double total = 0.0;
        for (auto& cyl : cylinders)
            total += cyl.computePressure(crank_angle_deg, fuel_smooth);
        total /= NUM_CYLINDERS;

        double cutoff = exhaust_cutoff + fuel_smooth * 800.0;
        double filtered = exhaust_lpf.process(total, cutoff, SAMPLE_RATE);
        double tw = turbo.process(rpm_smooth, fuel_smooth, dt);
        double mech = noise.next() * 0.015 * (rpm_smooth / 2000.0);
        double mix = filtered * 0.85 + tw + mech;
        mix = dc_block.process(mix);
        mix = std::tanh(mix * 1.3);
        return (float)(mix * env);
    }
};

// ============================================================================
// Simple delay line
// ============================================================================

class DelayLine {
    std::vector<float> buf;
    int writePos = 0;
public:
    void init(int maxSamples) {
        buf.assign(maxSamples, 0.0f);
        writePos = 0;
    }

    void write(float sample) {
        buf[writePos] = sample;
        writePos = (writePos + 1) % (int)buf.size();
    }

    float read(int delaySamples) const {
        int idx = writePos - delaySamples;
        if (idx < 0) idx += (int)buf.size();
        return buf[idx];
    }

    float readInterp(float delaySamples) const {
        int d = (int)delaySamples;
        float frac = delaySamples - d;
        return read(d) * (1.0f - frac) + read(d + 1) * frac;
    }
};

// ============================================================================
// Simple reverb (Schroeder-style: 4 combs + 2 allpass)
// ============================================================================

class SimpleReverb {
    struct Comb {
        DelayLine dl;
        float feedback = 0.0f;
        OnePole lpf;

        void init(int delaySamples, float fb, float damping) {
            dl.init(delaySamples + 1);
            feedback = fb;
        }

        float process(float input) {
            float delayed = dl.read(0); // Will be set properly
            return delayed; // placeholder
        }
    };

    DelayLine combs[4];
    float combFb[4];
    OnePole combLpf[4];

    DelayLine ap1, ap2;

    float combOut[4] = {};

public:
    void init(double sampleRate) {
        // Comb delays (prime-ish numbers for diffusion)
        int combDelays[4] = {
            (int)(0.0297 * sampleRate),
            (int)(0.0371 * sampleRate),
            (int)(0.0411 * sampleRate),
            (int)(0.0437 * sampleRate)
        };
        for (int i = 0; i < 4; i++) {
            combs[i].init(combDelays[i] + 2);
            combFb[i] = 0.85f;
            combLpf[i].reset();
            combOut[i] = 0.0f;
        }

        ap1.init((int)(0.005 * sampleRate) + 2);
        ap2.init((int)(0.0017 * sampleRate) + 2);
    }

    float process(float input) {
        float sum = 0.0f;

        int combDelays[4] = {
            (int)(0.0297 * SAMPLE_RATE),
            (int)(0.0371 * SAMPLE_RATE),
            (int)(0.0411 * SAMPLE_RATE),
            (int)(0.0437 * SAMPLE_RATE)
        };

        for (int i = 0; i < 4; i++) {
            float delayed = combs[i].read(combDelays[i]);
            float filtered = (float)combLpf[i].process(delayed, 4000.0, SAMPLE_RATE);
            combs[i].write(input + filtered * combFb[i]);
            sum += delayed;
        }
        sum *= 0.25f;

        // Allpass 1
        int ap1d = (int)(0.005 * SAMPLE_RATE);
        float ap1del = ap1.read(ap1d);
        float ap1out = -sum * 0.5f + ap1del;
        ap1.write(sum + ap1del * 0.5f);

        // Allpass 2
        int ap2d = (int)(0.0017 * SAMPLE_RATE);
        float ap2del = ap2.read(ap2d);
        float ap2out = -ap1out * 0.5f + ap2del;
        ap2.write(ap1out + ap2del * 0.5f);

        return ap2out;
    }
};

// ============================================================================
// Stereo delay
// ============================================================================

class StereoDelay {
    DelayLine left, right;
    OnePole lpfL, lpfR;
public:
    float outL = 0.0f, outR = 0.0f;

    void init(double sampleRate) {
        // Dotted-eighth feel: L=3/16 beat, R=1/4 beat at ~112 BPM
        double beat = 60.0 / 112.0;
        left.init((int)(beat * 0.75 * sampleRate) + 2);
        right.init((int)(beat * 0.5 * sampleRate) + 2);
        lpfL.reset();
        lpfR.reset();
    }

    void process(float input, float feedback = 0.35f) {
        int dL = (int)(60.0 / 112.0 * 0.75 * SAMPLE_RATE);
        int dR = (int)(60.0 / 112.0 * 0.5 * SAMPLE_RATE);

        float delL = left.read(dL);
        float delR = right.read(dR);

        float fbL = (float)lpfL.process(delL, 3000.0, SAMPLE_RATE);
        float fbR = (float)lpfR.process(delR, 3000.0, SAMPLE_RATE);

        left.write(input + fbR * feedback);  // Cross-feed for ping-pong
        right.write(input + fbL * feedback);

        outL = delL;
        outR = delR;
    }
};

// ============================================================================
// WAV writer (stereo)
// ============================================================================

void writeWavStereo(const char* filename, const std::vector<float>& L,
                    const std::vector<float>& R, int sr) {
    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return; }
    uint32_t numSamples = (uint32_t)L.size();
    uint32_t dataSize = numSamples * 4; // 2 channels * 16-bit
    uint32_t fileSize = 36 + dataSize;
    fwrite("RIFF", 1, 4, f); fwrite(&fileSize, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtSize = 16; uint16_t fmt = 1, ch = 2;
    uint32_t srate = sr, byteRate = sr * 4;
    uint16_t blockAlign = 4, bits = 16;
    fwrite(&fmtSize, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&srate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataSize, 4, 1, f);
    for (uint32_t i = 0; i < numSamples; i++) {
        int16_t vl = (int16_t)std::clamp((int)(L[i] * 32000.0f), -32767, 32767);
        int16_t vr = (int16_t)std::clamp((int)(R[i] * 32000.0f), -32767, 32767);
        fwrite(&vl, 2, 1, f);
        fwrite(&vr, 2, 1, f);
    }
    fclose(f);
    printf("Wrote %s (%.1f seconds, stereo)\n", filename, (double)numSamples / sr);
}

// ============================================================================
// Note → RPM
// ============================================================================

double noteToRPM(int note) {
    return 300.0 * std::pow(2.0, (note - 36) / 12.0);
}

// ============================================================================
// Note event
// ============================================================================

struct NoteEvent {
    double time;
    int note;
    int voiceIdx;
    double velocity;
};

// ============================================================================
// Main — "Smoke on the Water" v2: tighter timing, stereo, reverb, delay
// ============================================================================

int main() {
    const char* outfile = "diesel_song_v2.wav";

    std::vector<NoteEvent> events;
    double bpm = 112.0;
    double beat = 60.0 / bpm;

    auto addNote = [&](int voice, int note, double vel, double start, double dur) {
        events.push_back({start, note, voice, vel});
        events.push_back({start + dur - 0.01, 0, voice, 0.0});
    };

    // ---- Smoke on the Water — precise rhythm ----
    // Original riff: |G-Bb-|C~~~|G-Bb-|Db.C~~|G-Bb-|C~~~|Bb-G~~~|
    // Rhythm in 4/4: each group is 4 beats

    auto riff = [&](double t0, int octaveShift, double vel) {
        int G = 55 + octaveShift, Bb = 58 + octaveShift;
        int C = 60 + octaveShift, Db = 61 + octaveShift;
        double h = beat * 0.5; // eighth
        double q = beat;       // quarter
        double dq = beat * 1.5; // dotted quarter

        // Bar 1: G5--Bb5--C5~~~~
        // |x.x.x...|
        addNote(0, G,  vel,       t0,                 q - 0.02);
        addNote(0, Bb, vel,       t0 + q,             q - 0.02);
        addNote(0, C,  vel + 0.1, t0 + q*2,           q*1.8);

        // Bar 2: G5--Bb5--Db5.C5~~~~
        double t1 = t0 + q * 4;
        addNote(0, G,  vel,       t1,                 q - 0.02);
        addNote(0, Bb, vel,       t1 + q,             q*0.8);
        addNote(0, Db, vel + 0.1, t1 + q*2,           h - 0.02);
        addNote(0, C,  vel + 0.1, t1 + q*2 + h,       q*1.3);

        // Bar 3: G5--Bb5--C5~~~~
        double t2 = t0 + q * 8;
        addNote(0, G,  vel,       t2,                 q - 0.02);
        addNote(0, Bb, vel,       t2 + q,             q - 0.02);
        addNote(0, C,  vel + 0.1, t2 + q*2,           q*1.8);

        // Bar 4: Bb5--G5~~~~~~~~
        double t3 = t0 + q * 12;
        addNote(0, Bb, vel * 0.9, t3,                 dq - 0.02);
        addNote(0, G,  vel,       t3 + dq,            q*2.3);
    };

    // Bass: follows root
    auto bassLine = [&](double t0, double vel) {
        int G2 = 43, Bb2 = 46;
        double q = beat;
        addNote(1, G2,  vel, t0,          q*3.8);
        addNote(1, G2,  vel, t0 + q*4,    q*3.8);
        addNote(1, G2,  vel, t0 + q*8,    q*3.8);
        addNote(1, Bb2, vel * 0.8, t0 + q*12,  q*1.3);
        addNote(1, G2,  vel, t0 + q*13.5, q*2.3);
    };

    // Kick-like engine chug on beats
    auto kick = [&](double t0, int bars) {
        double q = beat;
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * q * 4;
            addNote(2, 28, 0.5, bt,            q*0.2);  // beat 1
            addNote(2, 28, 0.35, bt + q*2,     q*0.2);  // beat 3
            addNote(2, 30, 0.25, bt + q*2 + q, q*0.15); // beat 3-and (ghost)
        }
    };

    // ---- Arrangement ----
    double riffLen = beat * 16;

    // Intro idle
    addNote(1, 31, 0.25, 0.0, 1.8);
    addNote(2, 28, 0.3,  0.5, 0.15);
    addNote(2, 28, 0.2,  1.0, 0.15);

    double t0 = 2.0;

    // Verse 1: original register
    riff(t0, 0, 0.75);
    bassLine(t0, 0.55);
    kick(t0, 4);

    // Verse 2: up an octave — higher RPM scream
    double t1 = t0 + riffLen + beat * 2;
    riff(t1, 12, 0.85);
    bassLine(t1, 0.6);
    kick(t1, 4);

    // Verse 3: back down, heavier
    double t2 = t1 + riffLen + beat * 2;
    riff(t2, 0, 0.9);
    bassLine(t2, 0.65);
    kick(t2, 4);

    // Outro: ascending rev then long decay
    double t3 = t2 + riffLen + beat;
    addNote(0, 48, 0.6, t3,       0.4);
    addNote(0, 55, 0.7, t3 + 0.5, 0.4);
    addNote(0, 60, 0.8, t3 + 1.0, 0.4);
    addNote(0, 67, 0.9, t3 + 1.5, 0.4);
    addNote(0, 72, 0.95, t3 + 2.0, 0.4);
    addNote(0, 79, 1.0, t3 + 2.5, 2.0);
    addNote(1, 36, 0.3,  t3 + 4.5, 2.5);

    // Sort
    std::sort(events.begin(), events.end(),
              [](const NoteEvent& a, const NoteEvent& b) { return a.time < b.time; });

    double totalDuration = events.back().time + 4.0;
    int totalSamples = (int)(totalDuration * SAMPLE_RATE);

    printf("Diesel Engine Synth v2\n");
    printf("\"Smoke on the Water\" — stereo, reverb, delay\n");
    printf("Duration: %.1f seconds, %d events\n\n", totalDuration, (int)events.size());

    // Voices
    constexpr int MAX_VOICES = 4;
    std::array<Voice, MAX_VOICES> voices;
    for (auto& v : voices) {
        v.init();
        v.exhaust_cutoff = 900.0; // Higher for clearer pitch
    }
    voices[1].exhaust_cutoff = 400.0; // Bass stays darker
    voices[2].exhaust_cutoff = 300.0; // Kick stays very dark

    // Effects
    SimpleReverb reverb;
    reverb.init(SAMPLE_RATE);
    StereoDelay delay;
    delay.init(SAMPLE_RATE);

    // Render
    std::vector<float> audioL(totalSamples, 0.0f);
    std::vector<float> audioR(totalSamples, 0.0f);
    int eventIdx = 0;
    double dt = 1.0 / SAMPLE_RATE;

    for (int i = 0; i < totalSamples; i++) {
        double t = (double)i / SAMPLE_RATE;

        while (eventIdx < (int)events.size() && events[eventIdx].time <= t) {
            auto& ev = events[eventIdx];
            if (ev.voiceIdx < MAX_VOICES) {
                if (ev.note > 0)
                    voices[ev.voiceIdx].noteOn(noteToRPM(ev.note), ev.velocity);
                else
                    voices[ev.voiceIdx].noteOff();
            }
            eventIdx++;
        }

        // Mix dry voices with panning
        float dry = 0.0f;
        float dryL = 0.0f, dryR = 0.0f;

        float lead = voices[0].process(dt);
        float bass = voices[1].process(dt);
        float perc = voices[2].process(dt);
        float v3   = voices[3].process(dt);

        // Pan: lead center-slight-right, bass center, perc center-left
        dryL = lead * 0.45f + bass * 0.5f + perc * 0.55f + v3 * 0.5f;
        dryR = lead * 0.55f + bass * 0.5f + perc * 0.45f + v3 * 0.5f;
        dry = lead + bass + perc + v3;

        // Delay (mostly on lead)
        delay.process(lead * 0.4f, 0.3f);

        // Reverb (on everything, lighter)
        float rev = reverb.process(dry * 0.3f);

        // Final mix
        float mixL = dryL + delay.outL * 0.25f + rev * 0.35f;
        float mixR = dryR + delay.outR * 0.25f + rev * 0.35f;

        // Master soft clip
        mixL = std::tanh(mixL * 1.1f);
        mixR = std::tanh(mixR * 1.1f);

        audioL[i] = mixL;
        audioR[i] = mixR;
    }

    // Normalize
    float peak = 0.0f;
    for (int i = 0; i < totalSamples; i++)
        peak = std::max(peak, std::max(std::abs(audioL[i]), std::abs(audioR[i])));
    if (peak > 0.0f) {
        float gain = 0.92f / peak;
        for (int i = 0; i < totalSamples; i++) {
            audioL[i] *= gain;
            audioR[i] *= gain;
        }
    }

    writeWavStereo(outfile, audioL, audioR, SAMPLE_RATE);

    printf("Open with: open diesel_song_v2.wav\n");
    return 0;
}
