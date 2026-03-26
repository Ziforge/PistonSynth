// Diesel Engine Synth — Godflesh-style industrial drums
// Crushing machine-like repetition, relentless mechanical precision
// No melody — pure rhythm and weight
//
// Build: clang++ -std=c++17 -O2 -o diesel_godflesh diesel_godflesh.cpp -lm

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
// Engine core
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
    double x1=0,x2=0,y1=0,y2=0;
public:
    void reset() { x1=x2=y1=y2=0; }
    double process(double x, double cutoff_hz, double sr) {
        double w = std::tan(PI * cutoff_hz / sr);
        double n = 1.0/(1.0+std::sqrt(2.0)*w+w*w);
        double a0=n,a1=-2.0*n,a2=n;
        double b1=2.0*(w*w-1.0)*n, b2=(1.0-std::sqrt(2.0)*w+w*w)*n;
        double y=a0*x+a1*x1+a2*x2-b1*y1-b2*y2;
        x2=x1; x1=x; y2=y1; y1=y;
        return y;
    }
};

class DCBlocker {
    double x1=0,y1=0;
public:
    void reset() { x1=y1=0; }
    double process(double x) { double y=x-x1+0.995*y1; x1=x; y1=y; return y; }
};

class NoiseGen {
    uint32_t state = 12345;
public:
    void seed(uint32_t s) { state = s ? s : 1; }
    float next() { state=state*1664525u+1013904223u; return (float)(int32_t)state/(float)INT32_MAX; }
};

class CylinderModel {
public:
    EngineParams params;
    double firing_offset_deg = 0.0;
    double cylinderVolume(double theta_rad) const {
        double c=std::cos(theta_rad),s=std::sin(theta_rad);
        double R=params.R_r,rc=params.r_c();
        return params.V_c+(params.V_c/2.0)*(rc-1.0)*(R+1.0-c-std::sqrt(std::max(0.0,R*R-s*s)));
    }
    double computePressure(double global_deg, double fuel) const {
        double ld = std::fmod(global_deg - firing_offset_deg, 720.0);
        if (ld<0) ld+=720.0;
        double rc=params.r_c(), g=params.gamma, p=0.0;
        if (ld<180.0) {
            p = -0.05*std::sin(PI*ld/180.0);
        } else if (ld<360.0) {
            double cf=(ld-180.0)/180.0, th=PI*(1.0-cf);
            double V=cylinderVolume(th), Vb=cylinderVolume(PI);
            p = (std::pow(Vb/V,g)-1.0)/(std::pow(rc,g)-1.0);
        } else if (ld<540.0) {
            double ef=(ld-360.0)/180.0, th=PI*ef;
            double V=cylinderVolume(th), Vt=cylinderVolume(0.0);
            double pp=std::pow(rc,g), cb=fuel*0.6;
            double df=(5.0+(1.0-fuel)*10.0)/180.0;
            double env=(ef<df)?std::pow(ef/df,2.0):std::exp(-3.0*(ef-df)/(1.0-df));
            double pe=(pp+cb*pp*env)*std::pow(Vt/V,g);
            double pm=pp*(1.0+cb);
            p=std::clamp((pe-1.0)/(pm-1.0),0.0,1.0);
        } else {
            p = std::exp(-8.0*(ld-540.0)/180.0)*fuel*0.4;
        }
        return p;
    }
};

// ============================================================================
// Drum voice — engine burst with distortion/decimation options
// ============================================================================

struct DrumVoice {
    EngineParams params;
    std::array<CylinderModel, NUM_CYLINDERS> cylinders;
    NoiseGen noise;
    OnePole lpf1, lpf2;
    TwoPoleHP hpf;
    DCBlocker dc_block;
    double crank_angle_deg = 0.0;
    double rpm_start=0, rpm_end=0, fuel=0, env=0;
    double attack=0.001, decay=0.08;
    double lpf_cutoff=500, hpf_cutoff=20;
    double noise_mix=0, noise_cutoff=4000;
    double pitch_decay=0.05;
    bool active=false;
    double time_since_trigger=0;
    double gain=1.0;
    double distortion=1.0;       // Pre-clip drive
    int decimation=1;            // Sample rate reduction (1=none)
    double bitcrush_bits=16.0;   // Bit depth reduction
    int decim_counter=0;
    float decim_hold=0;

    void init() {
        for (int i=0;i<NUM_CYLINDERS;i++) {
            cylinders[i].params=params;
            cylinders[i].firing_offset_deg=params.firing_offsets[i];
        }
    }
    void trigger(double vel) {
        active=true; time_since_trigger=0; env=0;
        fuel=vel; crank_angle_deg=0; init();
    }
    float process(double dt) {
        if (!active) return 0.0f;
        time_since_trigger += dt;
        if (time_since_trigger<attack) env=time_since_trigger/attack;
        else env *= 1.0 - dt/decay;
        if (env<0.0003) { active=false; return 0.0f; }

        double t_norm = std::min(time_since_trigger/pitch_decay, 1.0);
        double rpm = rpm_start + (rpm_end-rpm_start)*t_norm;
        crank_angle_deg += rpm*6.0*dt;
        if (crank_angle_deg>=720.0) crank_angle_deg-=720.0;

        double total=0;
        for (auto& c:cylinders) total+=c.computePressure(crank_angle_deg, fuel);
        total /= NUM_CYLINDERS;

        double body = lpf1.process(total, lpf_cutoff, SAMPLE_RATE);
        body = hpf.process(body, hpf_cutoff, SAMPLE_RATE);

        double n = noise.next();
        double nf = lpf2.process(n, noise_cutoff, SAMPLE_RATE);

        double mix = body*(1.0-noise_mix) + nf*noise_mix;
        mix = dc_block.process(mix);

        // Distortion
        mix *= distortion;
        mix = std::tanh(mix);

        // Bitcrush
        if (bitcrush_bits < 16.0) {
            double levels = std::pow(2.0, bitcrush_bits);
            mix = std::round(mix * levels) / levels;
        }

        // Decimation
        decim_counter++;
        if (decim_counter >= decimation) {
            decim_counter = 0;
            decim_hold = (float)mix;
        }

        return decim_hold * (float)(env * gain);
    }
};

// ============================================================================
// Drone voice — continuous low engine rumble
// ============================================================================

struct DroneVoice {
    EngineParams params;
    std::array<CylinderModel, NUM_CYLINDERS> cylinders;
    NoiseGen noise;
    OnePole lpf;
    DCBlocker dc_block;
    double crank_angle_deg=0, rpm=0, fuel=0;
    double env=0;
    bool active=false;

    void init() {
        for (int i=0;i<NUM_CYLINDERS;i++) {
            cylinders[i].params=params;
            cylinders[i].firing_offset_deg=params.firing_offsets[i];
        }
    }
    void start(double r, double f) { rpm=r; fuel=f; active=true; init(); }
    void stop() { active=false; }

    float process(double dt) {
        if (active) env += (1.0-env)*(1.0-std::exp(-dt/0.5));
        else env += (0.0-env)*(1.0-std::exp(-dt/1.0));
        if (env<0.0001 && !active) return 0.0f;

        crank_angle_deg += rpm*6.0*dt;
        if (crank_angle_deg>=720.0) crank_angle_deg-=720.0;

        double total=0;
        for (auto& c:cylinders) total+=c.computePressure(crank_angle_deg, fuel);
        total /= NUM_CYLINDERS;

        double filtered = lpf.process(total, 200.0, SAMPLE_RATE);
        double mech = noise.next()*0.02;
        double mix = filtered + mech;
        mix = dc_block.process(mix);
        mix = std::tanh(mix * 3.0); // Heavy saturation on drone
        return (float)(mix * env);
    }
};

// ============================================================================
// Godflesh drum kit — heavier, more distorted, decimated
// ============================================================================

DrumVoice makeGodfleshKick() {
    DrumVoice d;
    d.rpm_start = 600.0;
    d.rpm_end = 120.0;      // Drops to sub-bass
    d.attack = 0.0005;
    d.decay = 0.25;          // Long, heavy
    d.pitch_decay = 0.03;
    d.lpf_cutoff = 180.0;   // Very dark
    d.hpf_cutoff = 20.0;
    d.noise_mix = 0.08;
    d.noise_cutoff = 400.0;
    d.gain = 2.0;
    d.distortion = 3.0;     // Crushed
    d.decimation = 2;        // Lo-fi
    d.bitcrush_bits = 10.0;
    d.params.gamma = 1.25;   // Softer combustion = rounder
    return d;
}

DrumVoice makeGodfleshSnare() {
    DrumVoice d;
    d.rpm_start = 1800.0;
    d.rpm_end = 600.0;
    d.attack = 0.0005;
    d.decay = 0.1;
    d.pitch_decay = 0.015;
    d.lpf_cutoff = 3000.0;
    d.hpf_cutoff = 200.0;
    d.noise_mix = 0.6;
    d.noise_cutoff = 5000.0;
    d.gain = 1.5;
    d.distortion = 4.0;      // Nasty clipped snare
    d.decimation = 3;
    d.bitcrush_bits = 8.0;   // Crunchy 8-bit
    return d;
}

DrumVoice makeGodfleshHat() {
    DrumVoice d;
    d.rpm_start = 4000.0;
    d.rpm_end = 3000.0;
    d.attack = 0.0003;
    d.decay = 0.04;
    d.pitch_decay = 0.01;
    d.lpf_cutoff = 7000.0;
    d.hpf_cutoff = 2000.0;
    d.noise_mix = 0.75;
    d.noise_cutoff = 10000.0;
    d.gain = 0.6;
    d.distortion = 2.0;
    d.decimation = 4;         // Very crunchy
    d.bitcrush_bits = 6.0;
    return d;
}

DrumVoice makeGodfleshOpenHat() {
    DrumVoice d;
    d.rpm_start = 4500.0;
    d.rpm_end = 2500.0;
    d.attack = 0.0003;
    d.decay = 0.3;
    d.pitch_decay = 0.08;
    d.lpf_cutoff = 8000.0;
    d.hpf_cutoff = 1500.0;
    d.noise_mix = 0.7;
    d.noise_cutoff = 11000.0;
    d.gain = 0.5;
    d.distortion = 2.5;
    d.decimation = 3;
    d.bitcrush_bits = 7.0;
    return d;
}

DrumVoice makeGodfleshTom() {
    DrumVoice d;
    d.rpm_start = 1000.0;
    d.rpm_end = 300.0;
    d.attack = 0.001;
    d.decay = 0.2;
    d.pitch_decay = 0.08;
    d.lpf_cutoff = 500.0;
    d.hpf_cutoff = 30.0;
    d.noise_mix = 0.1;
    d.noise_cutoff = 1200.0;
    d.gain = 1.3;
    d.distortion = 2.5;
    d.decimation = 2;
    d.bitcrush_bits = 9.0;
    return d;
}

DrumVoice makeGodfleshCrash() {
    DrumVoice d;
    d.rpm_start = 5000.0;
    d.rpm_end = 1500.0;
    d.attack = 0.001;
    d.decay = 1.2;
    d.pitch_decay = 0.4;
    d.lpf_cutoff = 9000.0;
    d.hpf_cutoff = 600.0;
    d.noise_mix = 0.85;
    d.noise_cutoff = 13000.0;
    d.gain = 0.5;
    d.distortion = 2.0;
    d.decimation = 2;
    d.bitcrush_bits = 10.0;
    return d;
}

// Metallic clang — very short, bright, distorted engine burst
DrumVoice makeClang() {
    DrumVoice d;
    d.rpm_start = 3000.0;
    d.rpm_end = 2000.0;
    d.attack = 0.0002;
    d.decay = 0.06;
    d.pitch_decay = 0.02;
    d.lpf_cutoff = 6000.0;
    d.hpf_cutoff = 800.0;
    d.noise_mix = 0.3;
    d.noise_cutoff = 8000.0;
    d.gain = 0.8;
    d.distortion = 5.0;
    d.decimation = 5;
    d.bitcrush_bits = 5.0;  // Extremely crushed
    return d;
}

// ============================================================================
// Simple delay/reverb
// ============================================================================

class DelayLine {
    std::vector<float> buf; int wp=0;
public:
    void init(int n) { buf.assign(n,0); wp=0; }
    void write(float s) { buf[wp]=s; wp=(wp+1)%(int)buf.size(); }
    float read(int d) const { int i=wp-d; if(i<0) i+=(int)buf.size(); return buf[i]; }
};

class SimpleReverb {
    DelayLine combs[4]; float combFb[4]; OnePole combLpf[4];
    DelayLine ap1,ap2; int cD[4],a1d,a2d;
public:
    void init(double sr) {
        cD[0]=(int)(0.0297*sr); cD[1]=(int)(0.0371*sr);
        cD[2]=(int)(0.0411*sr); cD[3]=(int)(0.0437*sr);
        for(int i=0;i<4;i++){combs[i].init(cD[i]+2);combFb[i]=0.88f;combLpf[i].reset();}
        a1d=(int)(0.005*sr); a2d=(int)(0.0017*sr);
        ap1.init(a1d+2); ap2.init(a2d+2);
    }
    float process(float in) {
        float sum=0;
        for(int i=0;i<4;i++){
            float del=combs[i].read(cD[i]);
            float f=(float)combLpf[i].process(del,2500,SAMPLE_RATE);
            combs[i].write(in+f*combFb[i]); sum+=del;
        }
        sum*=0.25f;
        float a1d_=ap1.read(a1d); float a1o=-sum*0.5f+a1d_; ap1.write(sum+a1d_*0.5f);
        float a2d_=ap2.read(a2d); float a2o=-a1o*0.5f+a2d_; ap2.write(a1o+a2d_*0.5f);
        return a2o;
    }
};

// ============================================================================
// WAV stereo
// ============================================================================

void writeWavStereo(const char* fn, const std::vector<float>& L,
                    const std::vector<float>& R, int sr) {
    FILE* f=fopen(fn,"wb"); if(!f) return;
    uint32_t n=(uint32_t)L.size(),ds=n*4,fs=36+ds;
    fwrite("RIFF",1,4,f); fwrite(&fs,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f);
    uint32_t fz=16; uint16_t fmt=1,ch=2; uint32_t s=sr,br=sr*4;
    uint16_t ba=4,bits=16;
    fwrite(&fz,4,1,f);fwrite(&fmt,2,1,f);fwrite(&ch,2,1,f);
    fwrite(&s,4,1,f);fwrite(&br,4,1,f);fwrite(&ba,2,1,f);fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&ds,4,1,f);
    for(uint32_t i=0;i<n;i++){
        int16_t vl=(int16_t)std::clamp((int)(L[i]*32000.f),-32767,32767);
        int16_t vr=(int16_t)std::clamp((int)(R[i]*32000.f),-32767,32767);
        fwrite(&vl,2,1,f); fwrite(&vr,2,1,f);
    }
    fclose(f);
    printf("Wrote %s (%.1f seconds, stereo)\n", fn, (double)n/sr);
}

// ============================================================================
// Event
// ============================================================================

enum DrumType { KICK=0, SNARE, HAT, OHAT, TOM, CRASH, CLANG, NUM_DRUMS };

struct Event {
    double time;
    int drum; // DrumType, or -1 for drone on, -2 for drone off
    double velocity;
};

// ============================================================================
// Godflesh-style patterns
// ============================================================================

int main() {
    const char* outfile = "diesel_godflesh.wav";

    // Slow, crushing tempo — Streetcleaner / Pure / Selfless era
    double bpm = 72.0;
    double beat = 60.0 / bpm;
    double q = beat, h = beat*0.5, e = beat*0.25;

    std::vector<Event> events;

    auto hit = [&](DrumType d, double vel, double t) {
        events.push_back({t, (int)d, vel});
    };

    // ---- Pattern A: Streetcleaner stomp ----
    // Relentless kick-snare on every beat, mechanical precision
    auto patternA = [&](double t0, int bars) {
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * q * 4;
            // Every beat: KICK
            for (int b = 0; b < 4; b++) {
                hit(KICK, 0.95, bt + b*q);
            }
            // Snare on 2 and 4
            hit(SNARE, 0.9, bt + q);
            hit(SNARE, 0.85, bt + q*3);
            // Hat on every eighth, accenting downbeats
            for (int i = 0; i < 8; i++) {
                hit(HAT, (i%2==0) ? 0.5 : 0.3, bt + i*h);
            }
        }
    };

    // ---- Pattern B: Crushing half-time ----
    // Like Avalanche or Like Rats — spacious but heavy
    auto patternB = [&](double t0, int bars) {
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * q * 4;
            hit(KICK, 1.0, bt);             // Beat 1 — massive
            hit(SNARE, 0.9, bt + q*2);      // Beat 3 — snare
            hit(HAT, 0.4, bt);
            hit(HAT, 0.3, bt + q);
            hit(OHAT, 0.4, bt + q*2);
            hit(HAT, 0.3, bt + q*3);
            // Ghost kick
            hit(KICK, 0.5, bt + q*3 + h);
        }
    };

    // ---- Pattern C: Double-time industrial pound ----
    // Merciless / Crush My Soul
    auto patternC = [&](double t0, int bars) {
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * q * 4;
            // Kick on every eighth note — relentless
            for (int i = 0; i < 8; i++) {
                hit(KICK, 0.85 + (i%4==0 ? 0.15 : 0.0), bt + i*h);
            }
            hit(SNARE, 0.95, bt + q);
            hit(SNARE, 0.9,  bt + q*3);
            // Clang accents
            hit(CLANG, 0.6, bt + q*2);
            hit(CLANG, 0.5, bt + q*2 + e);
        }
    };

    // ---- Pattern D: Broken / syncopated ----
    // Slavestate / Mothra
    auto patternD = [&](double t0, int bars) {
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * q * 4;
            hit(KICK, 1.0, bt);
            hit(KICK, 0.7, bt + q + h);     // Syncopated
            hit(KICK, 0.9, bt + q*2 + e);   // Off-grid
            hit(SNARE, 0.85, bt + q);
            hit(SNARE, 0.9, bt + q*3);
            hit(HAT, 0.45, bt);
            hit(CLANG, 0.5, bt + q*2);
            hit(HAT, 0.35, bt + q*2 + h);
            hit(OHAT, 0.4, bt + q*3 + h);
        }
    };

    // Fill: tom rolls
    auto fillA = [&](double t0) {
        for (int i = 0; i < 8; i++) {
            hit(TOM, 0.6 + i*0.05, t0 + i*e);
        }
        hit(CRASH, 0.9, t0 + q*2);
        hit(KICK, 1.0, t0 + q*2);
    };

    auto fillB = [&](double t0) {
        hit(SNARE, 0.7, t0);
        hit(SNARE, 0.8, t0+e);
        hit(SNARE, 0.85, t0+e*2);
        hit(SNARE, 0.9, t0+e*3);
        hit(KICK, 1.0, t0+q);
        hit(CRASH, 0.95, t0+q);
    };

    // ============================================================
    // ARRANGEMENT — 60+ seconds of industrial punishment
    // ============================================================

    double t = 0.0;

    // Drone on throughout
    events.push_back({t, -1, 0.4}); // Drone start

    // Intro: 2 bars of just kick + hat (machine starting up)
    for (int i = 0; i < 8; i++) {
        hit(KICK, 0.6 + i*0.05, t + i*q);
        hit(HAT, 0.3, t + i*q);
    }
    t += q*8;

    // Crash in
    hit(CRASH, 1.0, t);

    // Section 1: Pattern A — Streetcleaner stomp (8 bars)
    patternA(t, 7);
    fillA(t + q*28);
    t += q*32;

    // Section 2: Pattern B — half-time crushing (8 bars)
    hit(CRASH, 0.9, t);
    patternB(t, 7);
    fillB(t + q*30);
    t += q*32;

    // Section 3: Pattern C — double-time assault (8 bars)
    hit(CRASH, 1.0, t);
    patternC(t, 7);
    fillA(t + q*28);
    t += q*32;

    // Section 4: Pattern D — broken/syncopated (4 bars)
    hit(CRASH, 0.85, t);
    patternD(t, 4);
    t += q*16;

    // Section 5: Back to A but heavier (4 bars)
    hit(CRASH, 1.0, t);
    patternA(t, 3);
    // Final bar: all kicks
    for (int i = 0; i < 16; i++)
        hit(KICK, 0.9, t + q*12 + i*e);
    hit(CRASH, 1.0, t + q*12 + 16*e);
    t += q*16;

    // Outro: sparse kicks dying out
    hit(KICK, 1.0, t);
    hit(KICK, 0.8, t + q*2);
    hit(KICK, 0.6, t + q*5);
    hit(KICK, 0.4, t + q*9);
    t += q*12;

    events.push_back({t, -2, 0}); // Drone stop
    t += 3.0;

    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) { return a.time < b.time; });

    double totalDuration = t;
    int totalSamples = (int)(totalDuration*SAMPLE_RATE);

    printf("DIESEL GODFLESH DRUMS\n");
    printf("72 BPM — Streetcleaner-era industrial\n");
    printf("Bitcrushed, decimated, distorted engine bursts\n");
    printf("Duration: %.1f seconds, %d events\n\n", totalDuration, (int)events.size());

    // Create drums
    std::array<DrumVoice, NUM_DRUMS> drums;
    drums[KICK]  = makeGodfleshKick();
    drums[SNARE] = makeGodfleshSnare();
    drums[HAT]   = makeGodfleshHat();
    drums[OHAT]  = makeGodfleshOpenHat();
    drums[TOM]   = makeGodfleshTom();
    drums[CRASH] = makeGodfleshCrash();
    drums[CLANG] = makeClang();
    for (int i=0; i<NUM_DRUMS; i++) { drums[i].init(); drums[i].noise.seed(i*7777+1); }

    DroneVoice drone;
    drone.init();

    // Effects — darker, longer reverb for industrial space
    SimpleReverb reverb;
    reverb.init(SAMPLE_RATE);

    // Feedback delay — longer, darker
    DelayLine delayL, delayR;
    OnePole delLpfL, delLpfR;
    int delTimeL = (int)(beat * 1.0 * SAMPLE_RATE); // Full beat delay
    int delTimeR = (int)(beat * 0.75 * SAMPLE_RATE);
    delayL.init(delTimeL + 2);
    delayR.init(delTimeR + 2);
    delLpfL.reset(); delLpfR.reset();

    // Render
    std::vector<float> audioL(totalSamples, 0), audioR(totalSamples, 0);
    int evIdx = 0;
    double dt = 1.0 / SAMPLE_RATE;

    for (int i = 0; i < totalSamples; i++) {
        double t = (double)i / SAMPLE_RATE;

        while (evIdx < (int)events.size() && events[evIdx].time <= t) {
            auto& ev = events[evIdx];
            if (ev.drum == -1) drone.start(150.0, ev.velocity); // Very low drone
            else if (ev.drum == -2) drone.stop();
            else if (ev.drum >= 0 && ev.drum < NUM_DRUMS)
                drums[ev.drum].trigger(ev.velocity);
            evIdx++;
        }

        // Process all drums
        float kick  = drums[KICK].process(dt);
        float snare = drums[SNARE].process(dt);
        float hat   = drums[HAT].process(dt);
        float ohat  = drums[OHAT].process(dt);
        float tom   = drums[TOM].process(dt);
        float crash = drums[CRASH].process(dt);
        float clang = drums[CLANG].process(dt);
        float dr    = drone.process(dt);

        // Drum bus
        float drumMono = kick + snare + hat + ohat + tom + crash + clang;

        // Panning — mostly center for industrial weight
        float dL = kick*0.5f + snare*0.5f + hat*0.4f + ohat*0.35f
                 + tom*0.55f + crash*0.45f + clang*0.6f;
        float dR = kick*0.5f + snare*0.5f + hat*0.6f + ohat*0.65f
                 + tom*0.45f + crash*0.55f + clang*0.4f;

        // Drone wide
        dL += dr * 0.55f;
        dR += dr * 0.45f;

        // Delay (on snare + clang for industrial echo)
        float delIn = snare*0.3f + clang*0.4f + crash*0.15f;
        float delOutL = delayL.read(delTimeL);
        float delOutR = delayR.read(delTimeR);
        float fbL = (float)delLpfL.process(delOutL, 1500, SAMPLE_RATE);
        float fbR = (float)delLpfR.process(delOutR, 1500, SAMPLE_RATE);
        delayL.write(delIn + fbR * 0.4f);
        delayR.write(delIn + fbL * 0.4f);

        // Dark reverb
        float rev = reverb.process(drumMono * 0.2f + dr * 0.1f);

        // Final mix
        float mixL = dL + delOutL*0.25f + rev*0.35f;
        float mixR = dR + delOutR*0.25f + rev*0.35f;

        // Heavy master distortion — the Godflesh way
        mixL = std::tanh(mixL * 1.8f);
        mixR = std::tanh(mixR * 1.8f);

        audioL[i] = mixL;
        audioR[i] = mixR;
    }

    // Normalize loud
    float peak = 0;
    for (int i=0;i<totalSamples;i++)
        peak = std::max(peak, std::max(std::abs(audioL[i]), std::abs(audioR[i])));
    if (peak > 0) {
        float g = 0.95f / peak;
        for (int i=0;i<totalSamples;i++) { audioL[i]*=g; audioR[i]*=g; }
    }

    writeWavStereo(outfile, audioL, audioR, SAMPLE_RATE);
    return 0;
}
