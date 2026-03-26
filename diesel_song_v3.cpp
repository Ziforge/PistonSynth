// Diesel Engine Synth v3 — full drum kit from engine physics
// Kick = very low RPM burst, Snare = mid RPM + noise, HiHat = high RPM short burst
// Tom = mid-low RPM slides, Crash = turbo whine burst
//
// Build: clang++ -std=c++17 -O2 -o diesel_song_v3 diesel_song_v3.cpp -lm

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
// Engine model (same core)
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

class TwoPoleHP {
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
public:
    void reset() { x1 = x2 = y1 = y2 = 0; }
    double process(double x, double cutoff_hz, double sr) {
        double w = std::tan(PI * cutoff_hz / sr);
        double n = 1.0 / (1.0 + std::sqrt(2.0) * w + w * w);
        double a0 = n, a1 = -2.0 * n, a2 = n;
        double b1 = 2.0 * (w * w - 1.0) * n;
        double b2 = (1.0 - std::sqrt(2.0) * w + w * w) * n;
        double y = a0 * x + a1 * x1 + a2 * x2 - b1 * y1 - b2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
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
    void seed(uint32_t s) { state = s; }
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
        return (std::sin(TWO_PI * phase) * 0.7
              + std::sin(TWO_PI * phase * 2.0) * 0.2
              + std::sin(TWO_PI * phase * 3.0) * 0.1) * speed * 0.12;
    }
};

// ============================================================================
// Melodic voice (same as v2)
// ============================================================================

struct MelodicVoice {
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
    double exhaust_cutoff = 900.0;

    void init() {
        for (int i = 0; i < NUM_CYLINDERS; i++) {
            cylinders[i].params = params;
            cylinders[i].firing_offset_deg = params.firing_offsets[i];
        }
    }
    void noteOn(double r, double f) { rpm = r; fuel = f; active = true; init(); }
    void noteOff() { active = false; }

    float process(double dt) {
        double sm = 1.0 - std::exp(-dt / 0.025);
        rpm_smooth += (rpm - rpm_smooth) * sm;
        fuel_smooth += (fuel - fuel_smooth) * sm;
        if (active) env += (1.0 - env) * (1.0 - std::exp(-dt / 0.06));
        else {
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
        return (float)(std::tanh(mix * 1.3) * env);
    }
};

// ============================================================================
// Drum voices — each is a specially-configured engine burst
// ============================================================================

struct DrumVoice {
    EngineParams params;
    std::array<CylinderModel, NUM_CYLINDERS> cylinders;
    NoiseGen noise;
    OnePole lpf1, lpf2;
    TwoPoleHP hpf;
    DCBlocker dc_block;
    double crank_angle_deg = 0.0;
    double rpm_start = 0.0, rpm_end = 0.0;
    double fuel = 0.0;
    double env = 0.0;
    double attack = 0.002, decay = 0.08;
    double lpf_cutoff = 500.0;
    double hpf_cutoff = 20.0;
    double noise_mix = 0.0;    // Noise layer amount (for snare)
    double noise_cutoff = 4000.0;
    double pitch_decay = 0.05; // How fast RPM drops from start to end
    bool active = false;
    double time_since_trigger = 0.0;
    double gain = 1.0;

    void init() {
        for (int i = 0; i < NUM_CYLINDERS; i++) {
            cylinders[i].params = params;
            cylinders[i].firing_offset_deg = params.firing_offsets[i];
        }
    }

    void trigger(double vel) {
        active = true;
        time_since_trigger = 0.0;
        env = 0.0;
        fuel = vel;
        crank_angle_deg = 0.0;
        init();
    }

    float process(double dt) {
        if (!active) return 0.0f;
        time_since_trigger += dt;

        // Envelope: fast attack, exponential decay
        if (time_since_trigger < attack)
            env = time_since_trigger / attack;
        else
            env *= 1.0 - dt / decay;

        if (env < 0.0005) { active = false; return 0.0f; }

        // RPM pitch sweep (kick drops, snare is steady, etc)
        double t_norm = std::min(time_since_trigger / pitch_decay, 1.0);
        double current_rpm = rpm_start + (rpm_end - rpm_start) * t_norm;

        crank_angle_deg += current_rpm * 6.0 * dt;
        if (crank_angle_deg >= 720.0) crank_angle_deg -= 720.0;

        // Engine body
        double total = 0.0;
        for (auto& cyl : cylinders)
            total += cyl.computePressure(crank_angle_deg, fuel);
        total /= NUM_CYLINDERS;

        double body = lpf1.process(total, lpf_cutoff, SAMPLE_RATE);
        body = hpf.process(body, hpf_cutoff, SAMPLE_RATE);

        // Noise layer (for snare rattle / hihat sizzle)
        double n = noise.next();
        double noise_filtered = lpf2.process(n, noise_cutoff, SAMPLE_RATE);

        double mix = body * (1.0 - noise_mix) + noise_filtered * noise_mix;
        mix = dc_block.process(mix);
        mix = std::tanh(mix * 2.0);

        return (float)(mix * env * gain);
    }
};

// ============================================================================
// Create specific drum sounds
// ============================================================================

DrumVoice makeKick() {
    DrumVoice d;
    d.rpm_start = 800.0;   // Start high for click
    d.rpm_end = 180.0;     // Drop to deep thump
    d.attack = 0.001;
    d.decay = 0.18;
    d.pitch_decay = 0.04;
    d.lpf_cutoff = 250.0;
    d.hpf_cutoff = 25.0;
    d.noise_mix = 0.05;
    d.noise_cutoff = 800.0;
    d.gain = 1.4;
    d.params.gamma = 1.3; // Softer combustion for rounder tone
    return d;
}

DrumVoice makeSnare() {
    DrumVoice d;
    d.rpm_start = 1400.0;
    d.rpm_end = 900.0;
    d.attack = 0.001;
    d.decay = 0.12;
    d.pitch_decay = 0.02;
    d.lpf_cutoff = 2500.0;
    d.hpf_cutoff = 120.0;
    d.noise_mix = 0.55;       // Lots of noise = snare rattle
    d.noise_cutoff = 6000.0;
    d.gain = 1.0;
    return d;
}

DrumVoice makeHiHatClosed() {
    DrumVoice d;
    d.rpm_start = 5000.0;  // Very high RPM = buzzy
    d.rpm_end = 4000.0;
    d.attack = 0.0005;
    d.decay = 0.03;         // Very short
    d.pitch_decay = 0.01;
    d.lpf_cutoff = 8000.0;
    d.hpf_cutoff = 3000.0;  // Highpassed — just the sizzle
    d.noise_mix = 0.7;
    d.noise_cutoff = 12000.0;
    d.gain = 0.5;
    return d;
}

DrumVoice makeHiHatOpen() {
    DrumVoice d;
    d.rpm_start = 5000.0;
    d.rpm_end = 3500.0;
    d.attack = 0.0005;
    d.decay = 0.2;          // Longer ring
    d.pitch_decay = 0.05;
    d.lpf_cutoff = 9000.0;
    d.hpf_cutoff = 2500.0;
    d.noise_mix = 0.65;
    d.noise_cutoff = 11000.0;
    d.gain = 0.45;
    return d;
}

DrumVoice makeTomLow() {
    DrumVoice d;
    d.rpm_start = 1200.0;
    d.rpm_end = 400.0;
    d.attack = 0.001;
    d.decay = 0.15;
    d.pitch_decay = 0.06;
    d.lpf_cutoff = 600.0;
    d.hpf_cutoff = 40.0;
    d.noise_mix = 0.08;
    d.noise_cutoff = 1500.0;
    d.gain = 1.1;
    return d;
}

DrumVoice makeTomHigh() {
    DrumVoice d;
    d.rpm_start = 2000.0;
    d.rpm_end = 800.0;
    d.attack = 0.001;
    d.decay = 0.1;
    d.pitch_decay = 0.04;
    d.lpf_cutoff = 1200.0;
    d.hpf_cutoff = 80.0;
    d.noise_mix = 0.1;
    d.noise_cutoff = 2000.0;
    d.gain = 0.9;
    return d;
}

DrumVoice makeCrash() {
    DrumVoice d;
    d.rpm_start = 6000.0;
    d.rpm_end = 2000.0;
    d.attack = 0.001;
    d.decay = 0.8;        // Long sustain
    d.pitch_decay = 0.3;
    d.lpf_cutoff = 10000.0;
    d.hpf_cutoff = 800.0;
    d.noise_mix = 0.8;
    d.noise_cutoff = 14000.0;
    d.gain = 0.4;
    return d;
}

// ============================================================================
// Effects (reverb + delay, same as v2)
// ============================================================================

class DelayLine {
    std::vector<float> buf;
    int writePos = 0;
public:
    void init(int maxSamples) { buf.assign(maxSamples, 0.0f); writePos = 0; }
    void write(float s) { buf[writePos] = s; writePos = (writePos + 1) % (int)buf.size(); }
    float read(int d) const { int i = writePos - d; if (i < 0) i += (int)buf.size(); return buf[i]; }
};

class SimpleReverb {
    DelayLine combs[4];
    float combFb[4];
    OnePole combLpf[4];
    DelayLine ap1, ap2;
    int combD[4];
    int ap1d, ap2d;
public:
    void init(double sr) {
        combD[0] = (int)(0.0297*sr); combD[1] = (int)(0.0371*sr);
        combD[2] = (int)(0.0411*sr); combD[3] = (int)(0.0437*sr);
        for (int i = 0; i < 4; i++) {
            combs[i].init(combD[i]+2); combFb[i] = 0.84f; combLpf[i].reset();
        }
        ap1d = (int)(0.005*sr); ap2d = (int)(0.0017*sr);
        ap1.init(ap1d+2); ap2.init(ap2d+2);
    }
    float process(float input) {
        float sum = 0.0f;
        for (int i = 0; i < 4; i++) {
            float del = combs[i].read(combD[i]);
            float filt = (float)combLpf[i].process(del, 3500.0, SAMPLE_RATE);
            combs[i].write(input + filt * combFb[i]);
            sum += del;
        }
        sum *= 0.25f;
        float a1d = ap1.read(ap1d); float a1o = -sum*0.5f + a1d; ap1.write(sum + a1d*0.5f);
        float a2d = ap2.read(ap2d); float a2o = -a1o*0.5f + a2d; ap2.write(a1o + a2d*0.5f);
        return a2o;
    }
};

class StereoDelay {
    DelayLine left, right;
    OnePole lpfL, lpfR;
    int dL, dR;
public:
    float outL = 0, outR = 0;
    void init(double sr, double bpm) {
        double beat = 60.0/bpm;
        dL = (int)(beat*0.75*sr); dR = (int)(beat*0.5*sr);
        left.init(dL+2); right.init(dR+2);
        lpfL.reset(); lpfR.reset();
    }
    void process(float input, float fb = 0.3f) {
        float delL = left.read(dL), delR = right.read(dR);
        float fbL = (float)lpfL.process(delL, 3000, SAMPLE_RATE);
        float fbR = (float)lpfR.process(delR, 3000, SAMPLE_RATE);
        left.write(input + fbR*fb); right.write(input + fbL*fb);
        outL = delL; outR = delR;
    }
};

// ============================================================================
// WAV writer (stereo)
// ============================================================================

void writeWavStereo(const char* fn, const std::vector<float>& L,
                    const std::vector<float>& R, int sr) {
    FILE* f = fopen(fn, "wb");
    if (!f) return;
    uint32_t n = (uint32_t)L.size(), dataSize = n*4, fileSize = 36+dataSize;
    fwrite("RIFF",1,4,f); fwrite(&fileSize,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f);
    uint32_t fmtSz=16; uint16_t fmt=1,ch=2; uint32_t s=sr,br=sr*4;
    uint16_t ba=4,bits=16;
    fwrite(&fmtSz,4,1,f); fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f);
    fwrite(&s,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&dataSize,4,1,f);
    for (uint32_t i = 0; i < n; i++) {
        int16_t vl=(int16_t)std::clamp((int)(L[i]*32000.f),-32767,32767);
        int16_t vr=(int16_t)std::clamp((int)(R[i]*32000.f),-32767,32767);
        fwrite(&vl,2,1,f); fwrite(&vr,2,1,f);
    }
    fclose(f);
    printf("Wrote %s (%.1f seconds, stereo)\n", fn, (double)n/sr);
}

double noteToRPM(int note) { return 300.0 * std::pow(2.0, (note-36)/12.0); }

// ============================================================================
// Event types
// ============================================================================

enum EventType { NOTE_ON, NOTE_OFF, DRUM_HIT };
enum DrumType { KICK=0, SNARE, HH_CLOSED, HH_OPEN, TOM_LO, TOM_HI, CRASH, NUM_DRUMS };

struct Event {
    double time;
    EventType type;
    int voiceIdx;    // For melodic
    int note;        // MIDI note or DrumType
    double velocity;
};

// ============================================================================
// Main
// ============================================================================

int main() {
    const char* outfile = "diesel_song_v3.wav";
    double bpm = 112.0;
    double beat = 60.0 / bpm;
    double q = beat, h = beat*0.5, dq = beat*1.5;

    std::vector<Event> events;

    // Helper: melodic note
    auto addNote = [&](int voice, int note, double vel, double start, double dur) {
        events.push_back({start, NOTE_ON, voice, note, vel});
        events.push_back({start + dur - 0.01, NOTE_OFF, voice, note, 0});
    };

    // Helper: drum hit
    auto hit = [&](DrumType drum, double vel, double time) {
        events.push_back({time, DRUM_HIT, 0, (int)drum, vel});
    };

    // ---- Drum pattern: rock beat ----
    auto rockBeat = [&](double t0, int bars) {
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * q * 4;
            // Kick on 1 and 3
            hit(KICK, 0.9, bt);
            hit(KICK, 0.8, bt + q*2);
            // Snare on 2 and 4
            hit(SNARE, 0.85, bt + q);
            hit(SNARE, 0.8, bt + q*3);
            // Hi-hat eighths
            for (int i = 0; i < 8; i++) {
                double ht = bt + i * h;
                if (i == 6) // Open hat on the "and of 4"
                    hit(HH_OPEN, 0.4, ht);
                else
                    hit(HH_CLOSED, 0.35 + (i%2==0 ? 0.1 : 0.0), ht);
            }
        }
    };

    // Drum fill
    auto fill = [&](double t0) {
        hit(SNARE, 0.7, t0);
        hit(SNARE, 0.75, t0 + h);
        hit(TOM_HI, 0.8, t0 + q);
        hit(TOM_HI, 0.75, t0 + q + h);
        hit(TOM_LO, 0.85, t0 + q*2);
        hit(TOM_LO, 0.8, t0 + q*2 + h);
        hit(KICK, 0.9, t0 + q*3);
        hit(CRASH, 0.7, t0 + q*3);
    };

    // ---- Smoke on the Water riff ----
    auto riff = [&](double t0, int octaveShift, double vel) {
        int G = 55+octaveShift, Bb = 58+octaveShift;
        int C = 60+octaveShift, Db = 61+octaveShift;

        addNote(0, G,  vel,     t0,             q-0.02);
        addNote(0, Bb, vel,     t0+q,           q-0.02);
        addNote(0, C,  vel+0.1, t0+q*2,         q*1.8);

        addNote(0, G,  vel,     t0+q*4,         q-0.02);
        addNote(0, Bb, vel,     t0+q*5,         q*0.8);
        addNote(0, Db, vel+0.1, t0+q*6,         h-0.02);
        addNote(0, C,  vel+0.1, t0+q*6+h,       q*1.3);

        addNote(0, G,  vel,     t0+q*8,         q-0.02);
        addNote(0, Bb, vel,     t0+q*9,         q-0.02);
        addNote(0, C,  vel+0.1, t0+q*10,        q*1.8);

        addNote(0, Bb, vel*0.9, t0+q*12,        dq-0.02);
        addNote(0, G,  vel,     t0+q*12+dq,     q*2.3);
    };

    auto bassLine = [&](double t0, double vel) {
        int G2 = 43, Bb2 = 46;
        addNote(1, G2,  vel, t0,         q*3.8);
        addNote(1, G2,  vel, t0+q*4,     q*3.8);
        addNote(1, G2,  vel, t0+q*8,     q*3.8);
        addNote(1, Bb2, vel*0.8, t0+q*12, q*1.3);
        addNote(1, G2,  vel, t0+q*13.5,  q*2.3);
    };

    double riffLen = q * 16;

    // ---- ARRANGEMENT ----

    // Intro: 2-bar drum intro with crash
    double t = 0.0;
    hit(CRASH, 0.6, t);
    rockBeat(t, 2);
    addNote(1, 31, 0.2, t, q*8); // Low idle rumble
    t += q * 8;

    // Verse 1: riff + bass + drums
    hit(CRASH, 0.7, t);
    riff(t, 0, 0.75);
    bassLine(t, 0.55);
    rockBeat(t, 3);
    fill(t + q*12); // Fill in bar 4
    t += riffLen + q*2;

    // Verse 2: up an octave
    hit(CRASH, 0.8, t);
    riff(t, 12, 0.85);
    bassLine(t, 0.6);
    rockBeat(t, 3);
    fill(t + q*12);
    t += riffLen + q*2;

    // Verse 3: back down, heavy
    hit(CRASH, 0.85, t);
    riff(t, 0, 0.9);
    bassLine(t, 0.65);
    rockBeat(t, 4);
    t += riffLen + q;

    // Outro: ascending engine rev over half-time drums
    hit(CRASH, 0.9, t);
    // Half-time feel
    for (int i = 0; i < 4; i++) {
        double bt = t + i*q*4;
        hit(KICK, 0.9, bt);
        hit(SNARE, 0.8, bt + q*2);
        hit(HH_CLOSED, 0.3, bt);
        hit(HH_CLOSED, 0.3, bt + q);
        hit(HH_OPEN, 0.35, bt + q*2);
        hit(HH_CLOSED, 0.3, bt + q*3);
    }
    // Ascending rev
    addNote(0, 48, 0.6, t,       0.4);
    addNote(0, 55, 0.7, t+0.5,   0.4);
    addNote(0, 60, 0.8, t+1.0,   0.4);
    addNote(0, 67, 0.9, t+1.5,   0.4);
    addNote(0, 72, 0.95, t+2.0,  0.4);
    addNote(0, 79, 1.0, t+2.5,   2.0);
    t += q*16;

    // Final crash + idle
    hit(CRASH, 1.0, t);
    hit(KICK, 1.0, t);
    addNote(1, 36, 0.3, t+0.5, 3.0);
    t += 4.0;

    // Sort events
    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) { return a.time < b.time; });

    double totalDuration = t + 2.0;
    int totalSamples = (int)(totalDuration * SAMPLE_RATE);

    printf("Diesel Engine Synth v3 — with ENGINE DRUMS\n");
    printf("Kick=low RPM burst, Snare=mid RPM+noise, HiHat=high RPM sizzle\n");
    printf("Duration: %.1f seconds, %d events\n\n", totalDuration, (int)events.size());

    // Voices
    constexpr int MAX_MEL = 3;
    std::array<MelodicVoice, MAX_MEL> melVoices;
    for (auto& v : melVoices) v.init();
    melVoices[0].exhaust_cutoff = 900.0;
    melVoices[1].exhaust_cutoff = 400.0;

    // Drum voices
    std::array<DrumVoice, NUM_DRUMS> drums;
    drums[KICK] = makeKick();
    drums[SNARE] = makeSnare();
    drums[HH_CLOSED] = makeHiHatClosed();
    drums[HH_OPEN] = makeHiHatOpen();
    drums[TOM_LO] = makeTomLow();
    drums[TOM_HI] = makeTomHigh();
    drums[CRASH] = makeCrash();
    for (auto& d : drums) { d.init(); d.noise.seed(rand()); }

    // Effects
    SimpleReverb reverb; reverb.init(SAMPLE_RATE);
    StereoDelay delay; delay.init(SAMPLE_RATE, bpm);

    // Render
    std::vector<float> audioL(totalSamples, 0.0f), audioR(totalSamples, 0.0f);
    int evIdx = 0;
    double dt = 1.0 / SAMPLE_RATE;

    for (int i = 0; i < totalSamples; i++) {
        double t = (double)i / SAMPLE_RATE;

        while (evIdx < (int)events.size() && events[evIdx].time <= t) {
            auto& ev = events[evIdx];
            if (ev.type == NOTE_ON && ev.voiceIdx < MAX_MEL) {
                melVoices[ev.voiceIdx].noteOn(noteToRPM(ev.note), ev.velocity);
            } else if (ev.type == NOTE_OFF && ev.voiceIdx < MAX_MEL) {
                melVoices[ev.voiceIdx].noteOff();
            } else if (ev.type == DRUM_HIT && ev.note < NUM_DRUMS) {
                drums[ev.note].trigger(ev.velocity);
            }
            evIdx++;
        }

        // Melodic
        float lead = melVoices[0].process(dt);
        float bass = melVoices[1].process(dt);
        float mel2 = melVoices[2].process(dt);

        // Drums
        float kick   = drums[KICK].process(dt);
        float snare  = drums[SNARE].process(dt);
        float hhc    = drums[HH_CLOSED].process(dt);
        float hho    = drums[HH_OPEN].process(dt);
        float tomLo  = drums[TOM_LO].process(dt);
        float tomHi  = drums[TOM_HI].process(dt);
        float crash  = drums[CRASH].process(dt);

        float drumMix = kick + snare*0.9f + hhc + hho + tomLo + tomHi + crash;

        // Panning
        float melL = lead*0.45f + bass*0.5f + mel2*0.5f;
        float melR = lead*0.55f + bass*0.5f + mel2*0.5f;

        // Drums: kick/snare center, hats slightly right, toms panned
        float drumL = kick*0.5f + snare*0.5f + hhc*0.35f + hho*0.35f
                    + tomHi*0.6f + tomLo*0.7f + crash*0.45f;
        float drumR = kick*0.5f + snare*0.5f + hhc*0.65f + hho*0.65f
                    + tomHi*0.4f + tomLo*0.3f + crash*0.55f;

        float dryL = melL + drumL;
        float dryR = melR + drumR;

        // Delay on lead only
        delay.process(lead * 0.35f, 0.28f);

        // Reverb on mix (light on drums, more on melody)
        float revIn = lead*0.25f + snare*0.1f + crash*0.15f;
        float rev = reverb.process(revIn);

        float mixL = dryL + delay.outL*0.22f + rev*0.3f;
        float mixR = dryR + delay.outR*0.22f + rev*0.3f;

        audioL[i] = std::tanh(mixL * 1.05f);
        audioR[i] = std::tanh(mixR * 1.05f);
    }

    // Normalize
    float peak = 0.0f;
    for (int i = 0; i < totalSamples; i++)
        peak = std::max(peak, std::max(std::abs(audioL[i]), std::abs(audioR[i])));
    if (peak > 0.0f) {
        float g = 0.92f / peak;
        for (int i = 0; i < totalSamples; i++) { audioL[i]*=g; audioR[i]*=g; }
    }

    writeWavStereo(outfile, audioL, audioR, SAMPLE_RATE);
    return 0;
}
