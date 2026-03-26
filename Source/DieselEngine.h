#pragma once

#include <cmath>
#include <array>
#include <algorithm>

// ============================================================================
// Diesel Engine Physical Model Synthesizer
// Based on: "Physical modeling of a heavy-duty engine for test-cycle simulations"
// Peter Jonsson, Lund University, 2018 (TFRT-6054)
//
// Maps the thermodynamic equations of a 6-cylinder compression-ignition diesel
// engine to audio synthesis. Cylinder pressure pulses become the waveform.
// ============================================================================

static constexpr double PI = 3.14159265358979323846;
static constexpr double TWO_PI = 2.0 * PI;
static constexpr int NUM_CYLINDERS = 6;

// ============================================================================
// Engine geometry and thermodynamic parameters (from thesis Chapter 3)
// ============================================================================

struct EngineParams {
    // Cylinder geometry
    double V_d = 0.002;       // Displacement volume per cylinder [m^3] (~2L heavy-duty)
    double V_c = 0.000125;    // Clearance volume [m^3]
    double R_r = 4.0;         // Connecting rod / crank radius ratio (eq 3.2)
    double L_pm = 0.142;      // Piston stroke [m] (eq 3.11)

    // Thermodynamic
    double gamma = 1.35;      // Ratio of specific heats cp/cv

    // Firing order offsets for 6-cylinder [degrees]
    // 1-5-3-6-2-4, evenly spaced at 120°
    double firing_offsets[NUM_CYLINDERS] = {0, 240, 120, 300, 60, 180};

    double r_c() const { return (V_d + V_c) / V_c; } // Compression ratio (~17:1)
};

// ============================================================================
// One-pole lowpass filter
// ============================================================================

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

// ============================================================================
// DC blocker
// ============================================================================

class DCBlocker {
    double x1 = 0.0, y1 = 0.0;
public:
    void reset() { x1 = y1 = 0.0; }

    double process(double x) {
        double y = x - x1 + 0.995 * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

// ============================================================================
// Simple noise generator
// ============================================================================

class NoiseGen {
    uint32_t state = 12345;
public:
    void reset() { state = 12345; }

    float next() {
        state = state * 1664525u + 1013904223u;
        return (float)(int32_t)state / (float)INT32_MAX;
    }
};

// ============================================================================
// Single cylinder model (eq 3.2, 3.3, 3.4-3.9)
// ============================================================================

class CylinderModel {
public:
    EngineParams params;
    double firing_offset_deg = 0.0;

    // Cylinder volume V(θ) — eq 3.2
    double cylinderVolume(double theta_rad) const {
        double cos_t = std::cos(theta_rad);
        double sin_t = std::sin(theta_rad);
        double R_r = params.R_r;
        double r_c = params.r_c();
        return params.V_c + (params.V_c / 2.0) * (r_c - 1.0) *
               (R_r + 1.0 - cos_t - std::sqrt(std::max(0.0, R_r * R_r - sin_t * sin_t)));
    }

    // Compute normalised cylinder pressure for one sample
    // 4-stroke cycle = 720° crankshaft rotation
    double computePressure(double global_crank_deg, double fuel_rate) const {
        double local_deg = std::fmod(global_crank_deg - firing_offset_deg, 720.0);
        if (local_deg < 0.0) local_deg += 720.0;

        double pressure = 0.0;
        double r_c = params.r_c();
        double gamma = params.gamma;

        if (local_deg < 180.0) {
            // INTAKE: slight vacuum
            double t = local_deg / 180.0;
            pressure = -0.05 * std::sin(PI * t);

        } else if (local_deg < 360.0) {
            // COMPRESSION: adiabatic pV^γ = C  (eq 3.3)
            double comp_frac = (local_deg - 180.0) / 180.0; // 0→1
            double theta = PI * (1.0 - comp_frac);           // π→0 (BDC→TDC)
            double V = cylinderVolume(theta);
            double V_bdc = cylinderVolume(PI);
            double p_ratio = std::pow(V_bdc / V, gamma);
            double p_max = std::pow(r_c, gamma);
            pressure = (p_ratio - 1.0) / (p_max - 1.0);

        } else if (local_deg < 540.0) {
            // COMBUSTION + EXPANSION
            double exp_frac = (local_deg - 360.0) / 180.0; // 0→1
            double theta = PI * exp_frac;                    // 0→π (TDC→BDC)
            double V = cylinderVolume(theta);
            double V_tdc = cylinderVolume(0.0);

            double p_comp_peak = std::pow(r_c, gamma);

            // Combustion boost proportional to fuel (eq 3.10 mapped)
            double comb_boost = fuel_rate * 0.6;

            // Ignition delay shifts peak after TDC (eq 3.6, 3.7)
            double delay_frac = (5.0 + (1.0 - fuel_rate) * 10.0) / 180.0;

            double envelope;
            if (exp_frac < delay_frac) {
                envelope = std::pow(exp_frac / delay_frac, 2.0);
            } else {
                double decay = (exp_frac - delay_frac) / (1.0 - delay_frac);
                envelope = std::exp(-3.0 * decay);
            }

            double p_exp = (p_comp_peak + comb_boost * p_comp_peak * envelope) *
                           std::pow(V_tdc / V, gamma);
            double p_max = p_comp_peak * (1.0 + comb_boost);
            pressure = std::clamp((p_exp - 1.0) / (p_max - 1.0), 0.0, 1.0);

        } else {
            // EXHAUST BLOWDOWN
            double exh_frac = (local_deg - 540.0) / 180.0;
            pressure = std::exp(-8.0 * exh_frac) * fuel_rate * 0.4;
        }

        return pressure;
    }
};

// ============================================================================
// Turbocharger whine model (derived from eq 3.16 turbine mass flow)
// ============================================================================

class TurboModel {
    double phase = 0.0;
    double speed = 0.0;
public:
    void reset() { phase = speed = 0.0; }

    double process(double rpm, double fuel, double dt) {
        double target = fuel * std::sqrt(std::max(rpm, 1.0) / 2000.0);
        double tau = 0.5; // Spool-up time constant [s]
        speed += (target - speed) * dt / tau;

        double freq = 2000.0 + speed * 6000.0;
        phase += freq * dt;
        if (phase > 1.0) phase -= 1.0;

        double whine = std::sin(TWO_PI * phase) * 0.7
                     + std::sin(TWO_PI * phase * 2.0) * 0.2
                     + std::sin(TWO_PI * phase * 3.0) * 0.1;

        return whine * speed * 0.15;
    }
};

// ============================================================================
// Complete diesel engine synth voice
// ============================================================================

class DieselEngineVoice {
public:
    EngineParams params;
    std::array<CylinderModel, NUM_CYLINDERS> cylinders;
    TurboModel turbo;
    NoiseGen noise;
    OnePole exhaust_lpf;
    OnePole turbo_hpf;
    DCBlocker dc_block;

    double crank_angle_deg = 0.0;
    double sample_rate = 44100.0;

    // Per-voice state (set by noteOn)
    double rpm = 0.0;
    double fuel = 0.0;
    double velocity = 0.0;      // Original velocity, preserved
    bool active = false;
    int assigned_note = -1;     // MIDI note this voice is playing

    // Smoothing
    double rpm_smooth = 0.0;
    double fuel_smooth = 0.0;

    // External parameters (set per-block by processor)
    double exhaust_cutoff_ext = 400.0;   // From Exhaust Cutoff knob
    double turbo_mix_ext = 0.3;          // From Turbo Mix knob
    double pitch_bend = 0.0;             // Semitones from pitch bend wheel
    double mod_wheel = 0.0;              // 0-1 from mod wheel

    void prepare(double sr) {
        sample_rate = sr;
        reset();
    }

    void reset() {
        crank_angle_deg = 0.0;
        rpm_smooth = 0.0;
        fuel_smooth = 0.0;
        exhaust_lpf.reset();
        turbo_hpf.reset();
        dc_block.reset();
        turbo.reset();
        noise.reset();

        for (int i = 0; i < NUM_CYLINDERS; i++) {
            cylinders[i].params = params;
            cylinders[i].firing_offset_deg = params.firing_offsets[i];
        }
    }

    void noteOn(double rpm_target, double fuel_amount, double vel, int note) {
        rpm = rpm_target;
        fuel = fuel_amount;
        velocity = vel;
        assigned_note = note;
        active = true;
    }

    void noteOff() {
        active = false;
        assigned_note = -1;
    }

    float processSample() {
        if (!active && rpm_smooth < 1.0)
            return 0.0f;

        double dt = 1.0 / sample_rate;

        // Apply pitch bend to RPM (±2 semitones = ±bend_range)
        double bent_rpm = rpm * std::pow(2.0, pitch_bend / 12.0);

        // Mod wheel adds vibrato
        double vibrato = 0.0;
        if (mod_wheel > 0.01 && active) {
            static double vib_phase = 0.0;
            vib_phase += 5.5 * dt;
            if (vib_phase > 1.0) vib_phase -= 1.0;
            vibrato = std::sin(TWO_PI * vib_phase) * mod_wheel * 0.02;
            bent_rpm *= (1.0 + vibrato);
        }

        // Smooth parameter transitions
        double smoothing = 1.0 - std::exp(-dt / 0.05);
        rpm_smooth += (bent_rpm - rpm_smooth) * smoothing;
        if (!active) {
            fuel_smooth += (0.0 - fuel_smooth) * smoothing;
            rpm_smooth += (0.0 - rpm_smooth) * (1.0 - std::exp(-dt / 0.3));
        } else {
            fuel_smooth += (fuel - fuel_smooth) * smoothing;
        }

        // Advance crankshaft: RPM → deg/s = RPM × 6
        crank_angle_deg += rpm_smooth * 6.0 * dt;
        if (crank_angle_deg >= 720.0) crank_angle_deg -= 720.0;

        // Sum pressure from all 6 cylinders
        double total = 0.0;
        for (auto& cyl : cylinders)
            total += cyl.computePressure(crank_angle_deg, fuel_smooth);
        total /= NUM_CYLINDERS;

        // Exhaust pipe lowpass — uses external knob value
        double cutoff = exhaust_cutoff_ext + fuel_smooth * 400.0;
        double filtered = exhaust_lpf.process(total, cutoff, sample_rate);

        // Turbo whine — scaled by external mix knob
        double tw = turbo.process(rpm_smooth, fuel_smooth, dt) * turbo_mix_ext;

        // Mechanical clatter
        double mech = noise.next() * 0.03 * (rpm_smooth / 2000.0);

        double mix = filtered * 0.8 + tw + mech;
        mix = dc_block.process(mix);

        return (float)mix;
    }
};
