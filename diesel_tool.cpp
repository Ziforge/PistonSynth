// Diesel Engine Synth — Tool-esque industrial prog
// Polyrhythmic drums, droning bass, dissonant melodic textures
// Odd time signatures, heavy but musical
//
// Build: clang++ -std=c++17 -O2 -o diesel_tool diesel_tool.cpp -lm

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
    double V_d=0.002, V_c=0.000125, R_r=4.0, L_pm=0.142, gamma=1.35;
    double firing_offsets[NUM_CYLINDERS] = {0,240,120,300,60,180};
    double r_c() const { return (V_d+V_c)/V_c; }
};

class OnePole {
    double y1=0;
public:
    void reset() { y1=0; }
    double process(double x, double c, double sr) {
        double w=TWO_PI*c/sr, a=w/(1.0+w);
        y1=a*x+(1.0-a)*y1; return y1;
    }
};

class TwoPoleHP {
    double x1=0,x2=0,y1=0,y2=0;
public:
    void reset(){x1=x2=y1=y2=0;}
    double process(double x, double c, double sr) {
        double w=std::tan(PI*c/sr), n=1.0/(1.0+std::sqrt(2.0)*w+w*w);
        double y=n*x-2.0*n*x1+n*x2-2.0*(w*w-1.0)*n*y1-(1.0-std::sqrt(2.0)*w+w*w)*n*y2;
        x2=x1;x1=x;y2=y1;y1=y; return y;
    }
};

class TwoPoleLP {
    double x1=0,x2=0,y1=0,y2=0;
public:
    void reset(){x1=x2=y1=y2=0;}
    double process(double x, double c, double sr, double q=0.707) {
        double w=TWO_PI*c/sr;
        double cosw=std::cos(w), sinw=std::sin(w);
        double alpha=sinw/(2.0*q);
        double b0=(1.0-cosw)/2.0, b1=1.0-cosw, b2=(1.0-cosw)/2.0;
        double a0=1.0+alpha, a1=-2.0*cosw, a2=1.0-alpha;
        double y=(b0/a0)*x+(b1/a0)*x1+(b2/a0)*x2-(a1/a0)*y1-(a2/a0)*y2;
        x2=x1;x1=x;y2=y1;y1=y; return y;
    }
};

class DCBlocker {
    double x1=0,y1=0;
public:
    void reset(){x1=y1=0;}
    double process(double x){double y=x-x1+0.995*y1;x1=x;y1=y;return y;}
};

class NoiseGen {
    uint32_t state=12345;
public:
    void seed(uint32_t s){state=s?s:1;}
    float next(){state=state*1664525u+1013904223u;return(float)(int32_t)state/(float)INT32_MAX;}
};

class CylinderModel {
public:
    EngineParams params;
    double firing_offset_deg=0;
    double cylinderVolume(double t) const {
        double c=std::cos(t),s=std::sin(t),R=params.R_r,rc=params.r_c();
        return params.V_c+(params.V_c/2.0)*(rc-1.0)*(R+1.0-c-std::sqrt(std::max(0.0,R*R-s*s)));
    }
    double computePressure(double gd, double fuel) const {
        double ld=std::fmod(gd-firing_offset_deg,720.0);
        if(ld<0)ld+=720.0;
        double rc=params.r_c(),g=params.gamma,p=0;
        if(ld<180) p=-0.05*std::sin(PI*ld/180.0);
        else if(ld<360){
            double cf=(ld-180)/180.0,th=PI*(1-cf);
            double V=cylinderVolume(th),Vb=cylinderVolume(PI);
            p=(std::pow(Vb/V,g)-1)/(std::pow(rc,g)-1);
        } else if(ld<540){
            double ef=(ld-360)/180.0,th=PI*ef;
            double V=cylinderVolume(th),Vt=cylinderVolume(0);
            double pp=std::pow(rc,g),cb=fuel*0.6;
            double df=(5+(1-fuel)*10)/180.0;
            double env=(ef<df)?std::pow(ef/df,2):std::exp(-3*(ef-df)/(1-df));
            double pe=(pp+cb*pp*env)*std::pow(Vt/V,g),pm=pp*(1+cb);
            p=std::clamp((pe-1)/(pm-1),0.0,1.0);
        } else p=std::exp(-8*(ld-540)/180.0)*fuel*0.4;
        return p;
    }
};

class TurboModel {
    double phase=0,speed=0;
public:
    void reset(){phase=speed=0;}
    double process(double rpm, double fuel, double dt) {
        speed+=(fuel*std::sqrt(std::max(rpm,1.0)/2000.0)-speed)*dt/0.5;
        double freq=2000+speed*6000; phase+=freq*dt; if(phase>1)phase-=1;
        return (std::sin(TWO_PI*phase)*0.7+std::sin(TWO_PI*phase*2)*0.2
               +std::sin(TWO_PI*phase*3)*0.1)*speed*0.1;
    }
};

// ============================================================================
// Melodic voice — with resonant filter for Tool-like singing quality
// ============================================================================

struct MelodicVoice {
    EngineParams params;
    std::array<CylinderModel, NUM_CYLINDERS> cylinders;
    TurboModel turbo;
    NoiseGen noise;
    OnePole exhaust_lpf;
    TwoPoleLP resonant_lpf;  // Resonant filter for vowel-like quality
    TwoPoleHP body_hpf;
    DCBlocker dc_block;
    double crank_angle_deg=0, rpm=0, fuel=0;
    double rpm_smooth=0, fuel_smooth=0;
    bool active=false;
    double env=0;
    double exhaust_cutoff=1200.0;
    double resonance=2.0;    // Filter Q for singing quality
    double attack_time=0.08;
    double release_time=0.15;
    double vibrato_rate=5.0;  // Hz
    double vibrato_depth=0.01; // RPM fraction
    double vibrato_phase=0;
    double gain=1.0;

    void init() {
        for(int i=0;i<NUM_CYLINDERS;i++){
            cylinders[i].params=params; cylinders[i].firing_offset_deg=params.firing_offsets[i];
        }
    }
    void noteOn(double r, double f) {
        rpm=r; fuel=f; active=true;
        if(env<0.01) { init(); crank_angle_deg=0; }
    }
    void noteOff() { active=false; }

    float process(double dt) {
        double sm=1.0-std::exp(-dt/0.02);
        double target_rpm = rpm;

        // Vibrato (delayed onset — Tool-like expressive vibrato)
        if (active && env > 0.5) {
            vibrato_phase += vibrato_rate * dt;
            target_rpm *= 1.0 + std::sin(TWO_PI * vibrato_phase) * vibrato_depth * env;
        }

        rpm_smooth += (target_rpm - rpm_smooth) * sm;
        fuel_smooth += (fuel - fuel_smooth) * sm;

        if(active) env+=(1.0-env)*(1.0-std::exp(-dt/attack_time));
        else {
            env+=(0.0-env)*(1.0-std::exp(-dt/release_time));
            rpm_smooth+=(0.0-rpm_smooth)*(1.0-std::exp(-dt/0.2));
        }
        if(env<0.001 && !active) return 0.0f;

        crank_angle_deg += rpm_smooth*6.0*dt;
        if(crank_angle_deg>=720) crank_angle_deg-=720;

        double total=0;
        for(auto& c:cylinders) total+=c.computePressure(crank_angle_deg, fuel_smooth);
        total /= NUM_CYLINDERS;

        // Exhaust filter
        double filtered = exhaust_lpf.process(total, exhaust_cutoff + fuel_smooth*600, SAMPLE_RATE);

        // Resonant filter — gives it a singing, vowel-like quality
        double res = resonant_lpf.process(filtered, exhaust_cutoff * 0.8, SAMPLE_RATE, resonance);

        // Blend raw + resonant
        double mix = filtered * 0.4 + res * 0.6;

        // Turbo (subtle)
        mix += turbo.process(rpm_smooth, fuel_smooth, dt) * 0.5;

        mix = dc_block.process(mix);
        mix = std::tanh(mix * 1.5);
        return (float)(mix * env * gain);
    }
};

// ============================================================================
// Drum voice
// ============================================================================

struct DrumVoice {
    EngineParams params;
    std::array<CylinderModel, NUM_CYLINDERS> cylinders;
    NoiseGen noise;
    OnePole lpf1,lpf2;
    TwoPoleHP hpf;
    DCBlocker dc_block;
    double crank_angle_deg=0;
    double rpm_start=0,rpm_end=0,fuel=0,env=0;
    double attack=0.001,decay=0.08;
    double lpf_cutoff=500,hpf_cutoff=20;
    double noise_mix=0,noise_cutoff=4000;
    double pitch_decay=0.05;
    bool active=false;
    double time_since_trigger=0;
    double gain=1.0;
    double distortion=1.0;
    int decimation=1;
    double bitcrush_bits=16;
    int dc=0; float dh=0;

    void init(){for(int i=0;i<NUM_CYLINDERS;i++){cylinders[i].params=params;cylinders[i].firing_offset_deg=params.firing_offsets[i];}}
    void trigger(double vel){active=true;time_since_trigger=0;env=0;fuel=vel;crank_angle_deg=0;init();}
    float process(double dt) {
        if(!active) return 0;
        time_since_trigger+=dt;
        if(time_since_trigger<attack) env=time_since_trigger/attack;
        else env*=1.0-dt/decay;
        if(env<0.0003){active=false;return 0;}
        double tn=std::min(time_since_trigger/pitch_decay,1.0);
        double rpm=rpm_start+(rpm_end-rpm_start)*tn;
        crank_angle_deg+=rpm*6.0*dt; if(crank_angle_deg>=720)crank_angle_deg-=720;
        double total=0; for(auto& c:cylinders) total+=c.computePressure(crank_angle_deg,fuel);
        total/=NUM_CYLINDERS;
        double body=lpf1.process(total,lpf_cutoff,SAMPLE_RATE);
        body=hpf.process(body,hpf_cutoff,SAMPLE_RATE);
        double nf=lpf2.process(noise.next(),noise_cutoff,SAMPLE_RATE);
        double mix=body*(1-noise_mix)+nf*noise_mix;
        mix=dc_block.process(mix);
        mix*=distortion; mix=std::tanh(mix);
        if(bitcrush_bits<16){double lv=std::pow(2.0,bitcrush_bits);mix=std::round(mix*lv)/lv;}
        dc++; if(dc>=decimation){dc=0;dh=(float)mix;}
        return dh*(float)(env*gain);
    }
};

// ============================================================================
// Tool-style drum kit — tight, precise, less lo-fi than Godflesh
// ============================================================================

DrumVoice makeToolKick() {
    DrumVoice d;
    d.rpm_start=700; d.rpm_end=150;
    d.attack=0.0008; d.decay=0.2;
    d.pitch_decay=0.035;
    d.lpf_cutoff=220; d.hpf_cutoff=25;
    d.noise_mix=0.06; d.noise_cutoff=600;
    d.gain=1.6; d.distortion=2.0;
    d.decimation=1; d.bitcrush_bits=14;
    d.params.gamma=1.28;
    return d;
}

DrumVoice makeToolSnare() {
    DrumVoice d;
    d.rpm_start=1600; d.rpm_end=700;
    d.attack=0.0005; d.decay=0.14;
    d.pitch_decay=0.02;
    d.lpf_cutoff=3500; d.hpf_cutoff=150;
    d.noise_mix=0.5; d.noise_cutoff=7000;
    d.gain=1.2; d.distortion=2.5;
    d.decimation=1; d.bitcrush_bits=12;
    return d;
}

DrumVoice makeToolHat() {
    DrumVoice d;
    d.rpm_start=4500; d.rpm_end=3500;
    d.attack=0.0003; d.decay=0.035;
    d.pitch_decay=0.01;
    d.lpf_cutoff=9000; d.hpf_cutoff=3000;
    d.noise_mix=0.7; d.noise_cutoff=12000;
    d.gain=0.45; d.distortion=1.5;
    d.decimation=2; d.bitcrush_bits=10;
    return d;
}

DrumVoice makeToolRide() {
    DrumVoice d;
    d.rpm_start=5000; d.rpm_end=3000;
    d.attack=0.0005; d.decay=0.5;
    d.pitch_decay=0.1;
    d.lpf_cutoff=7000; d.hpf_cutoff=1500;
    d.noise_mix=0.6; d.noise_cutoff=10000;
    d.gain=0.35; d.distortion=1.3;
    d.decimation=1; d.bitcrush_bits=13;
    return d;
}

DrumVoice makeToolTomHi() {
    DrumVoice d;
    d.rpm_start=1800; d.rpm_end=700;
    d.attack=0.001; d.decay=0.12;
    d.pitch_decay=0.05;
    d.lpf_cutoff=1000; d.hpf_cutoff=60;
    d.noise_mix=0.08; d.noise_cutoff=2000;
    d.gain=1.0; d.distortion=1.8;
    return d;
}

DrumVoice makeToolTomLo() {
    DrumVoice d;
    d.rpm_start=1200; d.rpm_end=350;
    d.attack=0.001; d.decay=0.18;
    d.pitch_decay=0.07;
    d.lpf_cutoff=500; d.hpf_cutoff=35;
    d.noise_mix=0.06; d.noise_cutoff=1500;
    d.gain=1.1; d.distortion=1.8;
    return d;
}

DrumVoice makeToolFloorTom() {
    DrumVoice d;
    d.rpm_start=800; d.rpm_end=200;
    d.attack=0.001; d.decay=0.25;
    d.pitch_decay=0.08;
    d.lpf_cutoff=350; d.hpf_cutoff=28;
    d.noise_mix=0.05; d.noise_cutoff=1000;
    d.gain=1.3; d.distortion=2.0;
    return d;
}

DrumVoice makeToolCrash() {
    DrumVoice d;
    d.rpm_start=5500; d.rpm_end=2000;
    d.attack=0.001; d.decay=1.0;
    d.pitch_decay=0.35;
    d.lpf_cutoff=10000; d.hpf_cutoff=700;
    d.noise_mix=0.8; d.noise_cutoff=14000;
    d.gain=0.4; d.distortion=1.5;
    return d;
}

DrumVoice makeToolChina() {
    DrumVoice d;
    d.rpm_start=4000; d.rpm_end=1800;
    d.attack=0.0005; d.decay=0.6;
    d.pitch_decay=0.15;
    d.lpf_cutoff=6000; d.hpf_cutoff=900;
    d.noise_mix=0.75; d.noise_cutoff=9000;
    d.gain=0.5; d.distortion=3.0; // Trashy
    d.decimation=2; d.bitcrush_bits=9;
    return d;
}

// ============================================================================
// Effects
// ============================================================================

class DelayLine {
    std::vector<float> buf; int wp=0;
public:
    void init(int n){buf.assign(n,0);wp=0;}
    void write(float s){buf[wp]=s;wp=(wp+1)%(int)buf.size();}
    float read(int d) const {int i=wp-d;if(i<0)i+=(int)buf.size();return buf[i];}
};

class SimpleReverb {
    DelayLine combs[4]; float combFb[4]; OnePole combLpf[4];
    DelayLine ap1,ap2; int cD[4],a1d,a2d;
public:
    void init(double sr){
        cD[0]=(int)(0.0297*sr);cD[1]=(int)(0.0371*sr);
        cD[2]=(int)(0.0411*sr);cD[3]=(int)(0.0437*sr);
        for(int i=0;i<4;i++){combs[i].init(cD[i]+2);combFb[i]=0.82f;combLpf[i].reset();}
        a1d=(int)(0.005*sr);a2d=(int)(0.0017*sr);
        ap1.init(a1d+2);ap2.init(a2d+2);
    }
    float process(float in){
        float sum=0;
        for(int i=0;i<4;i++){
            float del=combs[i].read(cD[i]);
            float f=(float)combLpf[i].process(del,3000,SAMPLE_RATE);
            combs[i].write(in+f*combFb[i]);sum+=del;
        }
        sum*=0.25f;
        float a1=ap1.read(a1d),a1o=-sum*0.5f+a1;ap1.write(sum+a1*0.5f);
        float a2=ap2.read(a2d),a2o=-a1o*0.5f+a2;ap2.write(a1o+a2*0.5f);
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
    fwrite("RIFF",1,4,f);fwrite(&fs,4,1,f);fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f);
    uint32_t fz=16;uint16_t fmt=1,ch=2;uint32_t s=sr,br=sr*4;
    uint16_t ba=4,bits=16;
    fwrite(&fz,4,1,f);fwrite(&fmt,2,1,f);fwrite(&ch,2,1,f);
    fwrite(&s,4,1,f);fwrite(&br,4,1,f);fwrite(&ba,2,1,f);fwrite(&bits,2,1,f);
    fwrite("data",1,4,f);fwrite(&ds,4,1,f);
    for(uint32_t i=0;i<n;i++){
        int16_t vl=(int16_t)std::clamp((int)(L[i]*32000.f),-32767,32767);
        int16_t vr=(int16_t)std::clamp((int)(R[i]*32000.f),-32767,32767);
        fwrite(&vl,2,1,f);fwrite(&vr,2,1,f);
    }
    fclose(f); printf("Wrote %s (%.1f seconds, stereo)\n",fn,(double)n/sr);
}

double noteToRPM(int note){return 300.0*std::pow(2.0,(note-36)/12.0);}

// ============================================================================
// Events
// ============================================================================

enum EventType { NOTE_ON, NOTE_OFF, DRUM_HIT };
enum DrumType { KICK=0, SNARE, HAT, RIDE, TOM_HI, TOM_LO, FLOOR_TOM, CRASH, CHINA, NUM_DRUMS };

struct Event {
    double time;
    EventType type;
    int voice;
    int note; // MIDI or DrumType
    double vel;
};

int main() {
    const char* outfile = "diesel_tool.wav";

    // Tool tempos: 77-138 BPM, lots of odd time
    double bpm = 84.0;
    double beat = 60.0/bpm;
    double q=beat, h=beat*0.5, e=beat*0.25, s16=beat*0.25;
    double triplet = beat/3.0;

    std::vector<Event> events;

    auto addNote=[&](int v, int note, double vel, double start, double dur){
        events.push_back({start, NOTE_ON, v, note, vel});
        events.push_back({start+dur-0.01, NOTE_OFF, v, note, 0});
    };
    auto hit=[&](DrumType d, double vel, double t){
        events.push_back({t, DRUM_HIT, 0, (int)d, vel});
    };

    // ============================================================
    // DRUM PATTERNS — Danny Carey style
    // ============================================================

    // Pattern A: 7/8 groove (Schism-inspired)
    // 7 eighth notes per bar
    auto pattern7_8 = [&](double t0, int bars) {
        double barLen = h * 7;
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * barLen;
            hit(KICK, 0.95, bt);
            hit(HAT,  0.5,  bt);
            hit(HAT,  0.35, bt + h);
            hit(SNARE,0.85, bt + h*2);
            hit(HAT,  0.4,  bt + h*2);
            hit(HAT,  0.35, bt + h*3);
            hit(KICK, 0.7,  bt + h*4);
            hit(HAT,  0.45, bt + h*4);
            hit(HAT,  0.3,  bt + h*5);
            hit(SNARE,0.8,  bt + h*5);
            hit(RIDE, 0.35, bt + h*6);
        }
    };

    // Pattern B: 5/4 groove (Lateralus-inspired)
    auto pattern5_4 = [&](double t0, int bars) {
        double barLen = q * 5;
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * barLen;
            hit(KICK, 1.0,  bt);
            hit(RIDE, 0.4,  bt);
            hit(HAT,  0.35, bt + h);
            hit(RIDE, 0.35, bt + q);
            hit(SNARE,0.9,  bt + q);
            hit(RIDE, 0.35, bt + q + h);
            hit(KICK, 0.7,  bt + q*2);
            hit(RIDE, 0.4,  bt + q*2);
            hit(RIDE, 0.35, bt + q*2 + h);
            hit(SNARE,0.85, bt + q*3);
            hit(RIDE, 0.35, bt + q*3);
            hit(RIDE, 0.35, bt + q*3 + h);
            hit(KICK, 0.75, bt + q*4);
            hit(RIDE, 0.4,  bt + q*4);
            hit(HAT,  0.3,  bt + q*4 + h);
        }
    };

    // Pattern C: 4/4 but polyrhythmic — triplet kick over straight hat
    auto patternPoly = [&](double t0, int bars) {
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * q * 4;
            // Straight hats on eighths
            for (int i = 0; i < 8; i++)
                hit(HAT, (i%2==0)?0.45:0.3, bt + i*h);
            // Snare 2 and 4
            hit(SNARE, 0.9, bt + q);
            hit(SNARE, 0.85, bt + q*3);
            // Kick in triplet pattern over bar (6 triplets)
            for (int i = 0; i < 6; i++) {
                double vel = (i==0||i==3) ? 0.95 : 0.65;
                hit(KICK, vel, bt + i * (q*4.0/6.0));
            }
        }
    };

    // Pattern D: Heavy 4/4 with floor tom accents (Aenima-like)
    auto patternHeavy = [&](double t0, int bars) {
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * q * 4;
            hit(KICK, 1.0, bt);
            hit(CHINA, 0.6, bt);
            hit(HAT, 0.35, bt + h);
            hit(KICK, 0.7, bt + q + h);
            hit(SNARE, 0.95, bt + q);
            hit(HAT, 0.4, bt + q);
            hit(HAT, 0.3, bt + q + h);
            hit(KICK, 0.85, bt + q*2);
            hit(RIDE, 0.4, bt + q*2);
            hit(FLOOR_TOM, 0.7, bt + q*2 + h);
            hit(SNARE, 0.9, bt + q*3);
            hit(CHINA, 0.5, bt + q*3);
            hit(KICK, 0.6, bt + q*3 + h);
        }
    };

    // Tom fill — descending toms
    auto tomFill = [&](double t0) {
        hit(TOM_HI, 0.8, t0);
        hit(TOM_HI, 0.75, t0 + s16);
        hit(TOM_LO, 0.85, t0 + s16*2);
        hit(TOM_LO, 0.8, t0 + s16*3);
        hit(FLOOR_TOM, 0.9, t0 + s16*4);
        hit(FLOOR_TOM, 0.85, t0 + s16*5);
        hit(KICK, 1.0, t0 + s16*6);
        hit(CRASH, 0.9, t0 + s16*6);
    };

    // Triplet fill
    auto tripletFill = [&](double t0) {
        for (int i = 0; i < 6; i++) {
            DrumType d = (i<2) ? TOM_HI : (i<4) ? TOM_LO : FLOOR_TOM;
            hit(d, 0.7+i*0.05, t0 + i*triplet);
        }
        hit(CRASH, 1.0, t0 + 6*triplet);
        hit(KICK, 1.0, t0 + 6*triplet);
    };

    // ============================================================
    // MELODIC PARTS — D minor, dissonant, Tool intervals
    // ============================================================

    // Bass riff in D (voice 0) — droning, repetitive
    // D2=38, E2=40, F2=41, G2=43, A2=45, Bb2=46
    auto bassRiff7_8 = [&](double t0, int reps) {
        double barLen = h * 7;
        for (int r = 0; r < reps; r++) {
            double bt = t0 + r * barLen;
            addNote(0, 38, 0.8, bt,         h*2.8);  // D
            addNote(0, 41, 0.7, bt + h*3,   h*1.8);  // F
            addNote(0, 40, 0.75, bt + h*5,  h*1.8);  // E
        }
    };

    auto bassRiff5_4 = [&](double t0, int reps) {
        double barLen = q * 5;
        for (int r = 0; r < reps; r++) {
            double bt = t0 + r * barLen;
            addNote(0, 38, 0.85, bt,        q*1.8);   // D
            addNote(0, 43, 0.7,  bt + q*2,  q*0.8);   // G
            addNote(0, 41, 0.75, bt + q*3,  q*0.8);   // F
            addNote(0, 40, 0.7,  bt + q*4,  q*0.8);   // E
        }
    };

    auto bassHeavy = [&](double t0, int bars) {
        for (int bar = 0; bar < bars; bar++) {
            double bt = t0 + bar * q * 4;
            addNote(0, 38, 0.9, bt,         q*1.8);   // D powerchord feel
            addNote(0, 38, 0.85, bt + q*2,  q*0.8);
            addNote(0, 41, 0.8, bt + q*3,   q*0.8);   // F
        }
    };

    // Lead/texture voice (voice 1) — high, eerie, sparse
    // D4=62, E4=64, F4=65, G4=67, A4=69, Bb4=70, C#5=73
    auto leadTexture7_8 = [&](double t0, int reps) {
        double barLen = h * 7;
        for (int r = 0; r < reps; r += 2) {
            double bt = t0 + r * barLen;
            addNote(1, 69, 0.5, bt + h,       barLen*1.5);  // A4 — sustained
        }
    };

    auto leadTexture5_4 = [&](double t0, int reps) {
        double barLen = q * 5;
        for (int r = 0; r < reps; r += 2) {
            double bt = t0 + r * barLen;
            addNote(1, 65, 0.45, bt + q,      q*3);         // F4
            addNote(1, 73, 0.4, bt + barLen + q*2, q*2.5);  // C#5 — dissonant
        }
    };

    auto leadHeavy = [&](double t0, int bars) {
        for (int bar = 0; bar < bars; bar += 2) {
            double bt = t0 + bar * q * 4;
            addNote(1, 62, 0.6, bt,           q*3);   // D4
            addNote(1, 70, 0.5, bt + q*4,     q*3);   // Bb4 — tritone tension
        }
    };

    // Drone voice (voice 2) — sub D
    auto droneD = [&](double t0, double dur) {
        addNote(2, 26, 0.35, t0, dur);  // D1 — sub rumble
    };

    // ============================================================
    // ARRANGEMENT
    // ============================================================

    double t = 0.0;
    double barLen7 = h * 7;
    double barLen5 = q * 5;

    // --- Intro: sparse, building (8 bars of 7/8) ---
    droneD(t, barLen7 * 8 + 2.0);
    // First 4 bars: just bass
    bassRiff7_8(t, 4);
    t += barLen7 * 4;
    // Next 4: drums enter
    hit(CRASH, 0.7, t);
    pattern7_8(t, 4);
    bassRiff7_8(t, 4);
    leadTexture7_8(t, 4);
    t += barLen7 * 4;

    // --- Section A: 7/8 full groove (8 bars) ---
    hit(CRASH, 0.85, t);
    droneD(t, barLen7 * 8);
    pattern7_8(t, 7);
    tomFill(t + barLen7 * 7);
    bassRiff7_8(t, 8);
    leadTexture7_8(t, 8);
    t += barLen7 * 8;

    // --- Section B: 5/4 (8 bars) — Lateralus feel ---
    hit(CRASH, 0.9, t);
    droneD(t, barLen5 * 8);
    pattern5_4(t, 7);
    tripletFill(t + barLen5 * 7 + q*3);
    bassRiff5_4(t, 8);
    leadTexture5_4(t, 8);
    t += barLen5 * 8;

    // --- Section C: Polyrhythmic 4/4 (8 bars) ---
    hit(CRASH, 0.95, t);
    droneD(t, q * 32);
    patternPoly(t, 7);
    tomFill(t + q*28 + q*2);
    bassHeavy(t, 8);
    leadHeavy(t, 8);
    t += q * 32;

    // --- Section D: Heavy breakdown (8 bars) ---
    hit(CHINA, 1.0, t);
    hit(KICK, 1.0, t);
    droneD(t, q * 32);
    patternHeavy(t, 7);
    // Epic fill
    for (int i = 0; i < 12; i++)
        hit(i<4?TOM_HI:i<8?TOM_LO:FLOOR_TOM, 0.7+i*0.025, t + q*28 + i*s16);
    hit(CRASH, 1.0, t + q*31);
    hit(KICK, 1.0, t + q*31);
    bassHeavy(t, 8);
    leadHeavy(t, 8);
    t += q * 32;

    // --- Reprise: back to 7/8 (4 bars) then collapse ---
    hit(CRASH, 0.9, t);
    droneD(t, barLen7 * 4 + 3.0);
    pattern7_8(t, 4);
    bassRiff7_8(t, 4);
    leadTexture7_8(t, 4);
    t += barLen7 * 4;

    // Outro: decaying hits
    hit(CHINA, 1.0, t);
    hit(KICK, 1.0, t);
    addNote(0, 38, 0.7, t, 3.0); // Long D bass
    addNote(1, 62, 0.4, t, 4.0); // Long D lead
    hit(KICK, 0.7, t + q*3);
    hit(FLOOR_TOM, 0.6, t + q*5);
    hit(KICK, 0.5, t + q*8);
    t += 6.0;

    // Sort
    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b){return a.time<b.time;});

    double totalDuration = t + 2.0;
    int totalSamples = (int)(totalDuration * SAMPLE_RATE);

    printf("DIESEL TOOL — Industrial Prog\n");
    printf("84 BPM — 7/8, 5/4, polyrhythmic 4/4, heavy breakdown\n");
    printf("Duration: %.1f seconds, %d events\n\n", totalDuration, (int)events.size());

    // Create voices
    constexpr int MAX_MEL = 3;
    std::array<MelodicVoice, MAX_MEL> mel;
    for (auto& v : mel) v.init();

    // Voice 0: Bass — dark, low cutoff, heavy
    mel[0].exhaust_cutoff = 500.0;
    mel[0].resonance = 1.5;
    mel[0].gain = 1.2;
    mel[0].vibrato_depth = 0.005;

    // Voice 1: Lead — higher cutoff, more resonant, singing
    mel[1].exhaust_cutoff = 1400.0;
    mel[1].resonance = 3.5;  // Very resonant — almost singing
    mel[1].gain = 0.7;
    mel[1].vibrato_rate = 5.5;
    mel[1].vibrato_depth = 0.015;
    mel[1].attack_time = 0.15;   // Slow attack for swells
    mel[1].release_time = 0.4;

    // Voice 2: Sub drone — very low, dark
    mel[2].exhaust_cutoff = 200.0;
    mel[2].resonance = 1.0;
    mel[2].gain = 0.5;
    mel[2].attack_time = 0.5;
    mel[2].release_time = 1.0;

    // Drums
    std::array<DrumVoice, NUM_DRUMS> drums;
    drums[KICK]=makeToolKick(); drums[SNARE]=makeToolSnare();
    drums[HAT]=makeToolHat(); drums[RIDE]=makeToolRide();
    drums[TOM_HI]=makeToolTomHi(); drums[TOM_LO]=makeToolTomLo();
    drums[FLOOR_TOM]=makeToolFloorTom();
    drums[CRASH]=makeToolCrash(); drums[CHINA]=makeToolChina();
    for(int i=0;i<NUM_DRUMS;i++){drums[i].init();drums[i].noise.seed(i*3333+7);}

    // Effects
    SimpleReverb reverb; reverb.init(SAMPLE_RATE);
    DelayLine delayL, delayR;
    OnePole dlpfL, dlpfR;
    int dTimeL=(int)(beat*0.75*SAMPLE_RATE), dTimeR=(int)(beat*0.5*SAMPLE_RATE);
    delayL.init(dTimeL+2); delayR.init(dTimeR+2);
    dlpfL.reset(); dlpfR.reset();

    // Render
    std::vector<float> audioL(totalSamples,0), audioR(totalSamples,0);
    int evIdx=0;
    double dt=1.0/SAMPLE_RATE;

    for (int i=0; i<totalSamples; i++) {
        double t=(double)i/SAMPLE_RATE;

        while(evIdx<(int)events.size() && events[evIdx].time<=t){
            auto& ev=events[evIdx];
            if(ev.type==NOTE_ON && ev.voice<MAX_MEL)
                mel[ev.voice].noteOn(noteToRPM(ev.note), ev.vel);
            else if(ev.type==NOTE_OFF && ev.voice<MAX_MEL)
                mel[ev.voice].noteOff();
            else if(ev.type==DRUM_HIT && ev.note<NUM_DRUMS)
                drums[ev.note].trigger(ev.vel);
            evIdx++;
        }

        // Melodic voices
        float bass = mel[0].process(dt);
        float lead = mel[1].process(dt);
        float sub  = mel[2].process(dt);

        // Drums
        float k=drums[KICK].process(dt), sn=drums[SNARE].process(dt);
        float hh=drums[HAT].process(dt), rd=drums[RIDE].process(dt);
        float th=drums[TOM_HI].process(dt), tl=drums[TOM_LO].process(dt);
        float ft=drums[FLOOR_TOM].process(dt);
        float cr=drums[CRASH].process(dt), ch=drums[CHINA].process(dt);

        // Panning
        float mL = bass*0.5f + lead*0.4f + sub*0.5f;
        float mR = bass*0.5f + lead*0.6f + sub*0.5f;

        float dL = k*0.5f + sn*0.5f + hh*0.35f + rd*0.6f
                 + th*0.65f + tl*0.55f + ft*0.45f + cr*0.4f + ch*0.55f;
        float dR = k*0.5f + sn*0.5f + hh*0.65f + rd*0.4f
                 + th*0.35f + tl*0.45f + ft*0.55f + cr*0.6f + ch*0.45f;

        float mixL = mL + dL;
        float mixR = mR + dR;

        // Delay on lead + ride
        float delIn = lead*0.3f + rd*0.15f + cr*0.1f;
        float doL=delayL.read(dTimeL), doR=delayR.read(dTimeR);
        float fL=(float)dlpfL.process(doL,2000,SAMPLE_RATE);
        float fR=(float)dlpfR.process(doR,2000,SAMPLE_RATE);
        delayL.write(delIn+fR*0.35f);
        delayR.write(delIn+fL*0.35f);

        // Reverb
        float drumMono = k+sn+hh+rd+th+tl+ft+cr+ch;
        float rev = reverb.process(lead*0.2f + sn*0.08f + cr*0.1f + ft*0.05f);

        mixL += doL*0.2f + rev*0.3f;
        mixR += doR*0.2f + rev*0.3f;

        // Master — moderate saturation (Tool is tight, not sludgy)
        audioL[i] = std::tanh(mixL * 1.3f);
        audioR[i] = std::tanh(mixR * 1.3f);
    }

    // Normalize
    float peak=0;
    for(int i=0;i<totalSamples;i++)
        peak=std::max(peak,std::max(std::abs(audioL[i]),std::abs(audioR[i])));
    if(peak>0){float g=0.93f/peak;for(int i=0;i<totalSamples;i++){audioL[i]*=g;audioR[i]*=g;}}

    writeWavStereo(outfile, audioL, audioR, SAMPLE_RATE);
    return 0;
}
