// Diesel Engine Physical Model Synthesizer
// Based on: "Physical modeling of a heavy-duty engine for test-cycle simulations"
// Peter Jonsson, Lund University, 2018 (TFRT-6054)
//
// Maps the thermodynamic equations of a 6-cylinder compression-ignition diesel engine
// to audio synthesis. Cylinder pressure pulses become the waveform; RPM controls pitch;
// fuel injection controls timbre; turbocharger adds a secondary harmonic tone.
//
// Build: clang++ -std=c++17 -O2 -o diesel_synth diesel_synth.cpp -lm
// Usage: ./diesel_synth [output.wav]

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
// Engine geometry and thermodynamic parameters (from thesis Chapter 3)
// ============================================================================

struct EngineParams {
    // Cylinder geometry
    double V_d = 0.002;       // Displacement volume per cylinder [m^3] (~2L heavy-duty)
    double V_c = 0.000125;    // Clearance volume [m^3]
    double R_r = 4.0;         // Connecting rod / crank radius ratio (eq 3.2, Heywood p.45)
    double L_pm = 0.142;      // Piston stroke [m] (eq 3.11)

    // Thermodynamic
    double gamma = 1.35;      // Ratio of specific heats (cp/cv for air-fuel mix)
    double C_eta_vol = 0.95;  // Volumetric efficiency calibration
    double Q_LHV = 43e6;      // Lower heating value of diesel [J/kg]
    double c_p = 1005.0;      // Specific heat at constant pressure [J/(kg·K)]
    double x_e = 0.0;         // Evaporation fraction (simplified)
    double h_fg = 270000.0;   // Vaporization enthalpy [J/kg]

    // Combustion timing (eq 3.6 - Arrhenius ignition delay)
    double E_a = 50000.0;     // Activation energy [J/mol]
    double R_gas = 8.314;     // Universal gas constant [J/(mol·K)]
    double A_id = 0.01;       // Arrhenius pre-exponential
    double n_id = 1.0;        // Pressure exponent in ignition delay

    // Ignition timing efficiency (eq 3.8)
    double C_ign2 = 4.316;    // Ignition timing efficiency constant
    double theta_opt = 10.0;  // Optimal combustion angle [deg after TDC]
    double eta_ig_ch = 0.6894; // Chemical efficiency factor

    // Friction (eq 3.11)
    double C1_friction = 75000.0;

    // Firing order offsets for 6-cylinder (degrees)
    // Typical firing order: 1-5-3-6-2-4, evenly spaced at 120°
    double firing_offsets[NUM_CYLINDERS] = {0, 240, 120, 300, 60, 180};

    // Derived
    double r_c() const { return (V_d + V_c) / V_c; } // Compression ratio (~17:1)
};

// ============================================================================
// Single cylinder model
// ============================================================================

struct CylinderState {
    double crank_angle_deg = 0.0;  // Current crank angle [degrees]
    double pressure = 1e5;         // In-cylinder pressure [Pa]
    double temperature = 300.0;    // In-cylinder temperature [K]
    bool combustion_active = false;
    double combustion_progress = 0.0;
};

class CylinderModel {
public:
    EngineParams params;
    CylinderState state;
    double firing_offset_deg = 0.0;

    // Cylinder volume as function of crank angle (eq 3.2)
    // V = V_c + (V_c/2)(r_c - 1)(R_r + 1 - cos(θ) - sqrt(R_r² - sin²(θ)))
    double cylinderVolume(double theta_rad) const {
        double cos_t = std::cos(theta_rad);
        double sin_t = std::sin(theta_rad);
        double R_r = params.R_r;
        double r_c = params.r_c();
        return params.V_c + (params.V_c / 2.0) * (r_c - 1.0) *
               (R_r + 1.0 - cos_t - std::sqrt(R_r * R_r - sin_t * sin_t));
    }

    // Rate of volume change (for pressure waveform shaping)
    double dVdTheta(double theta_rad) const {
        double sin_t = std::sin(theta_rad);
        double cos_t = std::cos(theta_rad);
        double R_r = params.R_r;
        double r_c = params.r_c();
        double denom = std::sqrt(R_r * R_r - sin_t * sin_t);
        if (denom < 1e-10) denom = 1e-10;
        return (params.V_c / 2.0) * (r_c - 1.0) *
               (sin_t + sin_t * cos_t / denom);
    }

    // Adiabatic pressure from compression (eq 3.3: pV^γ = C)
    // p = p0 * (V0 / V)^γ
    double compressionPressure(double theta_rad, double p_inlet, double T_inlet) const {
        double V_max = cylinderVolume(PI); // Volume at BDC
        double V = cylinderVolume(theta_rad);
        return p_inlet * std::pow(V_max / V, params.gamma);
    }

    // Temperature at combustion time (eq 3.5)
    double combustionTemperature(double T_inlet, double V_ct_rad) const {
        double V_ct = cylinderVolume(V_ct_rad);
        double V_max = cylinderVolume(PI); // BDC volume = V_c + V_d
        return T_inlet * std::pow(V_max / V_ct, params.gamma - 1.0);
    }

    // Compute cylinder pressure waveform for audio
    // The 4-stroke cycle spans 720° (two crankshaft revolutions)
    // 0-180°: compression, 180°(TDC): combustion, 180-360°: expansion
    // 360-540°: exhaust, 540-720°: intake
    double computePressureSample(double global_crank_deg,
                                 double p_inlet,    // Inlet manifold pressure [Pa]
                                 double T_inlet,    // Inlet temperature [K]
                                 double fuel_rate,   // Fuel injection rate [0..1]
                                 double theta_SOI)   // Start of injection [deg before TDC]
    {
        // Local crank angle relative to this cylinder's TDC
        double local_deg = std::fmod(global_crank_deg - firing_offset_deg, 720.0);
        if (local_deg < 0) local_deg += 720.0;

        double theta_rad = local_deg * PI / 180.0;

        // Four-stroke phases mapped to 720°
        // Phase 1: Intake (0° - 180°) - valve open, low pressure
        // Phase 2: Compression (180° - 360°) - rising pressure, adiabatic
        // Phase 3: Combustion + Expansion (360° - 540°) - TDC at 360°, peak then fall
        // Phase 4: Exhaust (540° - 720°) - blowdown pulse then low

        // Remap: TDC compression is at 360° in our 720° cycle
        double pressure_normalized = 0.0;

        if (local_deg < 180.0) {
            // INTAKE: slightly below atmospheric
            double t = local_deg / 180.0;
            pressure_normalized = -0.05 * std::sin(PI * t);

        } else if (local_deg < 360.0) {
            // COMPRESSION: adiabatic pressure rise (eq 3.3)
            double comp_angle = (local_deg - 180.0) / 180.0; // 0..1
            double theta_comp = PI * (1.0 - comp_angle); // PI -> 0 (BDC -> TDC)
            double V = cylinderVolume(theta_comp);
            double V_bdc = cylinderVolume(PI);
            double p_ratio = std::pow(V_bdc / V, params.gamma);
            pressure_normalized = (p_ratio - 1.0) / (std::pow(params.r_c(), params.gamma) - 1.0);

        } else if (local_deg < 540.0) {
            // COMBUSTION + EXPANSION
            double exp_angle = (local_deg - 360.0) / 180.0; // 0..1
            double theta_exp = PI * exp_angle; // 0 -> PI (TDC -> BDC)
            double V = cylinderVolume(theta_exp);
            double V_tdc = cylinderVolume(0.0); // = V_c

            // Compression pressure at TDC
            double p_comp_peak = std::pow(params.r_c(), params.gamma);

            // Combustion adds energy (eq 3.10: W_ig = m_f * Q_LHV * eta_ig)
            // Model as pressure spike at TDC proportional to fuel_rate
            double combustion_boost = fuel_rate * 0.6; // Extra pressure from combustion

            // Ignition delay shifts the peak slightly after TDC (eq 3.6, 3.7)
            double delay_deg = 5.0 + (1.0 - fuel_rate) * 10.0;
            double peak_angle = delay_deg / 180.0; // Normalized

            // Combustion pressure envelope: fast rise, slower fall
            double comb_envelope;
            if (exp_angle < peak_angle) {
                comb_envelope = std::pow(exp_angle / peak_angle, 2.0);
            } else {
                double decay = (exp_angle - peak_angle) / (1.0 - peak_angle);
                comb_envelope = std::exp(-3.0 * decay);
            }

            // Expansion (adiabatic fall from peak)
            double p_expansion = (p_comp_peak + combustion_boost * p_comp_peak * comb_envelope) *
                                 std::pow(V_tdc / V, params.gamma);
            double p_max = p_comp_peak * (1.0 + combustion_boost);
            pressure_normalized = (p_expansion - 1.0) / (p_max - 1.0);
            pressure_normalized = std::clamp(pressure_normalized, 0.0, 1.0);

        } else {
            // EXHAUST BLOWDOWN: sharp pulse as exhaust valve opens, then low
            double exh_angle = (local_deg - 540.0) / 180.0; // 0..1
            // Blowdown pulse (the characteristic diesel exhaust "bark")
            double blowdown = std::exp(-8.0 * exh_angle) * fuel_rate * 0.4;
            pressure_normalized = blowdown;
        }

        return pressure_normalized;
    }
};

// ============================================================================
// Turbocharger model (eq 3.16 - mass flow, mapped to a whine tone)
// ============================================================================

class TurboModel {
public:
    double phase = 0.0;
    double speed = 0.0; // Normalized turbo speed [0..1]

    // Turbo whine: frequency tracks engine RPM but at much higher ratio
    // Real turbos spin 50,000-150,000 RPM, producing a characteristic whine
    double process(double rpm, double fuel_rate, double dt) {
        // Turbo speed lags behind engine demand (spool-up)
        double target_speed = fuel_rate * std::sqrt(rpm / 2000.0);
        double tau = 0.5; // Spool time constant [seconds]
        speed += (target_speed - speed) * dt / tau;

        // Turbo fundamental frequency: ~2000-8000 Hz depending on speed
        double turbo_freq = 2000.0 + speed * 6000.0;
        phase += turbo_freq * dt;
        if (phase > 1.0) phase -= 1.0;

        // Turbo whine is mostly sinusoidal with some harmonics
        double whine = std::sin(TWO_PI * phase) * 0.7 +
                       std::sin(TWO_PI * phase * 2.0) * 0.2 +
                       std::sin(TWO_PI * phase * 3.0) * 0.1;

        return whine * speed * 0.15; // Scale down - turbo is background
    }
};

// ============================================================================
// Mechanical noise model (valve clatter, injector tick)
// ============================================================================

class MechanicalNoise {
    uint32_t rng_state = 12345;

    float noise() {
        rng_state = rng_state * 1664525u + 1013904223u;
        return (float)(int32_t)rng_state / (float)INT32_MAX;
    }

public:
    double process(double rpm, double fuel_rate) {
        // Filtered noise proportional to RPM
        double n = noise() * 0.03 * (rpm / 2000.0);
        return n;
    }
};

// ============================================================================
// Simple one-pole lowpass filter
// ============================================================================

class OnePole {
    double y1 = 0.0;
public:
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
    double process(double x) {
        double y = x - x1 + 0.995 * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

// ============================================================================
// WAV writer
// ============================================================================

void writeWav(const char* filename, const std::vector<float>& samples, int sampleRate) {
    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return; }

    uint32_t dataSize = (uint32_t)(samples.size() * sizeof(int16_t));
    uint32_t fileSize = 36 + dataSize;

    // RIFF header
    fwrite("RIFF", 1, 4, f);
    fwrite(&fileSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt chunk
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1; // PCM
    uint16_t numChannels = 1;
    uint32_t sr = sampleRate;
    uint32_t byteRate = sr * 2;
    uint16_t blockAlign = 2;
    uint16_t bitsPerSample = 16;
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&audioFormat, 2, 1, f);
    fwrite(&numChannels, 2, 1, f);
    fwrite(&sr, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    fwrite(&bitsPerSample, 2, 1, f);

    // data chunk
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);
    for (float s : samples) {
        int16_t val = (int16_t)std::clamp((int)(s * 32000.0f), -32767, 32767);
        fwrite(&val, 2, 1, f);
    }
    fclose(f);
    printf("Wrote %s (%zu samples, %.1f seconds)\n", filename, samples.size(),
           (double)samples.size() / sampleRate);
}

// ============================================================================
// Main synthesizer
// ============================================================================

class DieselEngineSynth {
public:
    EngineParams params;
    std::array<CylinderModel, NUM_CYLINDERS> cylinders;
    TurboModel turbo;
    MechanicalNoise mech_noise;
    OnePole exhaust_filter;  // Exhaust pipe resonance (lowpass)
    OnePole intake_filter;
    DCBlocker dc_block;

    double crank_angle_deg = 0.0; // Global crankshaft angle [degrees]

    DieselEngineSynth() {
        for (int i = 0; i < NUM_CYLINDERS; i++) {
            cylinders[i].params = params;
            cylinders[i].firing_offset_deg = params.firing_offsets[i];
        }
    }

    // Generate one audio sample
    // rpm: engine speed [RPM]
    // fuel: fuel injection amount [0..1] (throttle/timbre)
    // returns audio sample [-1..1]
    float process(double rpm, double fuel) {
        double dt = 1.0 / SAMPLE_RATE;

        // Advance crankshaft angle
        // RPM -> degrees per second = RPM * 360 / 60 = RPM * 6
        double deg_per_sec = rpm * 6.0;
        crank_angle_deg += deg_per_sec * dt;
        if (crank_angle_deg >= 720.0) crank_angle_deg -= 720.0;

        // Inlet conditions
        double p_inlet = 1.5e5;  // ~1.5 bar boost pressure
        double T_inlet = 320.0;  // ~47°C after intercooler

        // Sum pressure from all 6 cylinders
        double total_pressure = 0.0;
        for (auto& cyl : cylinders) {
            double p = cyl.computePressureSample(
                crank_angle_deg, p_inlet, T_inlet, fuel, 15.0);
            total_pressure += p;
        }
        total_pressure /= NUM_CYLINDERS;

        // Exhaust pipe acts as a lowpass filter (larger pipe = lower resonance)
        // Real exhaust resonance ~80-200 Hz
        double exhaust_cutoff = 200.0 + fuel * 600.0;
        double filtered = exhaust_filter.process(total_pressure, exhaust_cutoff, SAMPLE_RATE);

        // Add turbocharger whine
        double turbo_whine = turbo.process(rpm, fuel, dt);

        // Add mechanical noise (injectors, valve train)
        double mech = mech_noise.process(rpm, fuel);

        // Mix
        double mix = filtered * 0.8 + turbo_whine + mech;

        // Remove DC offset
        mix = dc_block.process(mix);

        // Soft clip for analog warmth
        mix = std::tanh(mix * 1.5);

        return (float)mix;
    }
};

// ============================================================================
// Demo: generate WAV with RPM and fuel sweeps
// ============================================================================

int main(int argc, char* argv[]) {
    const char* outfile = (argc > 1) ? argv[1] : "diesel_engine.wav";

    DieselEngineSynth synth;

    double duration = 15.0; // seconds
    int total_samples = (int)(duration * SAMPLE_RATE);
    std::vector<float> audio(total_samples);

    printf("Diesel Engine Physical Model Synthesizer\n");
    printf("Based on Jonsson 2018 (Lund University TFRT-6054)\n");
    printf("6-cylinder compression-ignition engine → audio\n\n");
    printf("Generating %.0f seconds of audio...\n", duration);

    for (int i = 0; i < total_samples; i++) {
        double t = (double)i / SAMPLE_RATE;
        double rpm, fuel;

        if (t < 3.0) {
            // Phase 1: Idle — low RPM, minimal fuel
            rpm = 600.0;
            fuel = 0.15;
        } else if (t < 5.0) {
            // Phase 2: Rev up smoothly
            double blend = (t - 3.0) / 2.0;
            rpm = 600.0 + blend * 1400.0;  // 600 -> 2000 RPM
            fuel = 0.15 + blend * 0.55;     // light -> moderate
        } else if (t < 7.0) {
            // Phase 3: Hold at cruising speed
            rpm = 2000.0;
            fuel = 0.7;
        } else if (t < 9.0) {
            // Phase 4: Full throttle acceleration
            double blend = (t - 7.0) / 2.0;
            rpm = 2000.0 + blend * 1000.0; // 2000 -> 3000 RPM
            fuel = 0.7 + blend * 0.3;       // moderate -> full
        } else if (t < 11.0) {
            // Phase 5: Hold at redline
            rpm = 3000.0;
            fuel = 1.0;
        } else if (t < 14.0) {
            // Phase 6: Deceleration / engine braking
            double blend = (t - 11.0) / 3.0;
            rpm = 3000.0 - blend * 2200.0;  // 3000 -> 800
            fuel = 1.0 - blend * 0.85;       // full -> idle
        } else {
            // Phase 7: Return to idle
            rpm = 800.0;
            fuel = 0.15;
        }

        audio[i] = synth.process(rpm, fuel);
    }

    // Normalize
    float peak = 0.0f;
    for (float s : audio) peak = std::max(peak, std::abs(s));
    if (peak > 0.0f) {
        float gain = 0.9f / peak;
        for (float& s : audio) s *= gain;
    }

    writeWav(outfile, audio, SAMPLE_RATE);

    printf("\nEngine parameters (from thesis):\n");
    printf("  Compression ratio: %.1f:1\n", synth.params.r_c());
    printf("  Displacement/cyl:  %.0f cc\n", synth.params.V_d * 1e6);
    printf("  Cylinders:         %d\n", NUM_CYLINDERS);
    printf("  Gamma:             %.2f\n", synth.params.gamma);
    printf("  Stroke:            %.0f mm\n", synth.params.L_pm * 1000);
    printf("  Rod ratio:         %.1f\n", synth.params.R_r);

    return 0;
}
