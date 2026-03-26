// Diesel Engine Synth — plays a song and renders to WAV
// Uses the same physical model from the JUCE plugin
//
// Build: clang++ -std=c++17 -O2 -o diesel_song diesel_song.cpp -lm

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
// Engine model (same as DieselEngine.h)
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
        return whine * speed * 0.15;
    }
};

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
    double env = 0.0; // Amplitude envelope

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
        double smoothing = 1.0 - std::exp(-dt / 0.03);
        rpm_smooth += (rpm - rpm_smooth) * smoothing;
        fuel_smooth += (fuel - fuel_smooth) * smoothing;

        // Envelope
        if (active) {
            env += (1.0 - env) * (1.0 - std::exp(-dt / 0.08));
        } else {
            env += (0.0 - env) * (1.0 - std::exp(-dt / 0.15));
            rpm_smooth += (0.0 - rpm_smooth) * (1.0 - std::exp(-dt / 0.2));
        }

        if (env < 0.001 && !active) return 0.0f;

        crank_angle_deg += rpm_smooth * 6.0 * dt;
        if (crank_angle_deg >= 720.0) crank_angle_deg -= 720.0;

        double total = 0.0;
        for (auto& cyl : cylinders)
            total += cyl.computePressure(crank_angle_deg, fuel_smooth);
        total /= NUM_CYLINDERS;

        double cutoff = 200.0 + fuel_smooth * 600.0;
        double filtered = exhaust_lpf.process(total, cutoff, SAMPLE_RATE);
        double tw = turbo.process(rpm_smooth, fuel_smooth, dt);
        double mech = noise.next() * 0.02 * (rpm_smooth / 2000.0);
        double mix = filtered * 0.8 + tw + mech;
        mix = dc_block.process(mix);
        mix = std::tanh(mix * 1.5);
        return (float)(mix * env);
    }
};

// ============================================================================
// WAV writer
// ============================================================================

void writeWav(const char* filename, const std::vector<float>& samples, int sr) {
    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return; }
    uint32_t dataSize = (uint32_t)(samples.size() * sizeof(int16_t));
    uint32_t fileSize = 36 + dataSize;
    fwrite("RIFF", 1, 4, f); fwrite(&fileSize, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtSize = 16; uint16_t fmt = 1, ch = 1;
    uint32_t srate = sr, byteRate = sr * 2;
    uint16_t blockAlign = 2, bits = 16;
    fwrite(&fmtSize, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&srate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataSize, 4, 1, f);
    for (float s : samples) {
        int16_t val = (int16_t)std::clamp((int)(s * 32000.0f), -32767, 32767);
        fwrite(&val, 2, 1, f);
    }
    fclose(f);
    printf("Wrote %s (%.1f seconds)\n", filename, (double)samples.size() / sr);
}

// ============================================================================
// MIDI note → RPM (same mapping as plugin)
// ============================================================================

double noteToRPM(int note) {
    return 300.0 * std::pow(2.0, (note - 36) / 12.0);
}

// ============================================================================
// Song sequencer
// ============================================================================

struct NoteEvent {
    double time;    // seconds
    int note;       // MIDI note (0 = note off for that voice)
    int voiceIdx;
    double velocity; // 0-1
};

int main() {
    const char* outfile = "diesel_song.wav";

    // "Smoke on the Water" riff in diesel engine
    // G4=67, Bb4=70, C5=72, G4=67, Bb4=70, Db5=73, C5=72
    // Then repeat with bass notes underneath

    std::vector<NoteEvent> events;
    double bpm = 112.0;
    double beat = 60.0 / bpm;
    double t = 0.5; // Start after brief pause

    // Helper to add a note with duration
    auto addNote = [&](int voice, int note, double vel, double start, double dur) {
        events.push_back({start, note, voice, vel});
        events.push_back({start + dur, 0, voice, 0.0}); // note off
    };

    // ---- Smoke on the Water riff (voice 0 = lead) ----
    auto riff = [&](double startTime, double octaveShift) {
        int base = (int)octaveShift;
        int G  = 55 + base, Bb = 58 + base, C = 60 + base, Db = 61 + base;
        double q = beat;       // quarter
        double dq = beat * 1.5; // dotted quarter
        double e = beat * 0.5; // eighth

        // da da daa, da da da-daa, da da daa, da daa
        addNote(0, G,  0.8, startTime,              dq);
        addNote(0, Bb, 0.8, startTime + dq,         dq);
        addNote(0, C,  0.9, startTime + dq*2,       q*2);

        addNote(0, G,  0.8, startTime + q*5,        dq);
        addNote(0, Bb, 0.8, startTime + q*5 + dq,   q);
        addNote(0, Db, 0.9, startTime + q*7,        e);
        addNote(0, C,  0.9, startTime + q*7 + e,    q*2);

        addNote(0, G,  0.8, startTime + q*10,       dq);
        addNote(0, Bb, 0.8, startTime + q*10 + dq,  dq);
        addNote(0, C,  0.9, startTime + q*10 + dq*2, q*2);

        addNote(0, Bb, 0.7, startTime + q*15,       dq);
        addNote(0, G,  0.8, startTime + q*15 + dq,  q*2.5);
    };

    // ---- Bass line (voice 1 = low rumbling engine) ----
    auto bassLine = [&](double startTime) {
        int G2 = 43, F2 = 41, Bb2 = 46, C3 = 48;
        double q = beat;
        // Simple root notes following the harmony
        addNote(1, G2,  0.6, startTime,         q*4.5);
        addNote(1, G2,  0.6, startTime + q*5,   q*4.5);
        addNote(1, G2,  0.6, startTime + q*10,  q*4.5);
        addNote(1, Bb2, 0.5, startTime + q*15,  q*1.5);
        addNote(1, G2,  0.6, startTime + q*16.5, q*2);
    };

    // ---- Rhythmic engine hits (voice 2 = percussive low hits) ----
    auto percussion = [&](double startTime, int numBeats) {
        int E1 = 28; // Very low = slow RPM chug
        double q = beat;
        for (int i = 0; i < numBeats; i += 2) {
            addNote(2, E1, 0.4, startTime + i * q, q * 0.3);
        }
    };

    // ---- Arrange the song ----

    // Intro: engine idle rumble
    addNote(1, 31, 0.3, 0.0, 2.0); // Low idle

    // First verse of riff
    double riffLen = beat * 19;
    riff(2.0, 0);
    bassLine(2.0);
    percussion(2.0, 19);

    // Second verse (up an octave = higher RPM = screaming)
    riff(2.0 + riffLen + beat, 12);
    bassLine(2.0 + riffLen + beat);
    percussion(2.0 + riffLen + beat, 19);

    // Third verse — back to original with heavier fuel
    double t3 = 2.0 + (riffLen + beat) * 2;
    riff(t3, 0);
    bassLine(t3);
    percussion(t3, 19);

    // Outro: rev up and fade
    double outro = t3 + riffLen + beat;
    addNote(0, 48, 0.5, outro, 0.5);
    addNote(0, 55, 0.7, outro + 0.6, 0.5);
    addNote(0, 60, 0.8, outro + 1.2, 0.5);
    addNote(0, 67, 0.9, outro + 1.8, 0.5);
    addNote(0, 72, 1.0, outro + 2.4, 0.5);
    addNote(0, 79, 1.0, outro + 3.0, 1.5); // Hold high rev
    // Final idle
    addNote(1, 36, 0.3, outro + 4.5, 2.0);

    // Sort events by time
    std::sort(events.begin(), events.end(),
              [](const NoteEvent& a, const NoteEvent& b) { return a.time < b.time; });

    double totalDuration = events.back().time + 3.0;
    int totalSamples = (int)(totalDuration * SAMPLE_RATE);

    printf("Diesel Engine Synth — Playing \"Smoke on the Water\"\n");
    printf("Duration: %.1f seconds, %d note events\n\n", totalDuration, (int)events.size());

    // Render
    constexpr int MAX_VOICES = 4;
    std::array<Voice, MAX_VOICES> voices;
    for (auto& v : voices) v.init();

    std::vector<float> audio(totalSamples, 0.0f);
    int eventIdx = 0;
    double dt = 1.0 / SAMPLE_RATE;

    for (int i = 0; i < totalSamples; i++) {
        double t = (double)i / SAMPLE_RATE;

        // Process events
        while (eventIdx < (int)events.size() && events[eventIdx].time <= t) {
            auto& ev = events[eventIdx];
            int vi = ev.voiceIdx;
            if (vi < MAX_VOICES) {
                if (ev.note > 0) {
                    voices[vi].noteOn(noteToRPM(ev.note), ev.velocity);
                } else {
                    voices[vi].noteOff();
                }
            }
            eventIdx++;
        }

        // Mix all voices
        float sample = 0.0f;
        for (auto& v : voices)
            sample += v.process(dt);

        sample = std::tanh(sample * 1.2f);
        audio[i] = sample;
    }

    // Normalize
    float peak = 0.0f;
    for (float s : audio) peak = std::max(peak, std::abs(s));
    if (peak > 0.0f) {
        float gain = 0.9f / peak;
        for (float& s : audio) s *= gain;
    }

    writeWav(outfile, audio, SAMPLE_RATE);
    return 0;
}
