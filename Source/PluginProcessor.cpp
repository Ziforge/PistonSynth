#include "PluginProcessor.h"
#include "PluginEditor.h"

DieselEngineSynthProcessor::DieselEngineSynthProcessor()
    : AudioProcessor(BusesProperties()
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    rpmParam           = apvts.getRawParameterValue("rpm");
    fuelParam          = apvts.getRawParameterValue("fuel");
    turboMixParam      = apvts.getRawParameterValue("turboMix");
    driveParam         = apvts.getRawParameterValue("drive");
    exhaustCutoffParam = apvts.getRawParameterValue("exhaustCutoff");
    masterGainParam    = apvts.getRawParameterValue("masterGain");
    attackParam        = apvts.getRawParameterValue("attack");
    releaseParam       = apvts.getRawParameterValue("release");

    for (auto& v : voices)
        v.active = false;
}

juce::AudioProcessorValueTreeState::ParameterLayout
DieselEngineSynthProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("rpm", 1), "RPM",
        juce::NormalisableRange<float>(200.0f, 6000.0f, 1.0f, 0.5f),
        1200.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("fuel", 1), "Fuel Injection",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f),
        0.5f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("turboMix", 1), "Turbo Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f),
        0.3f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("drive", 1), "Drive",
        juce::NormalisableRange<float>(0.5f, 5.0f, 0.01f),
        1.5f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("exhaustCutoff", 1), "Exhaust Cutoff",
        juce::NormalisableRange<float>(80.0f, 2000.0f, 1.0f, 0.4f),
        400.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("masterGain", 1), "Master Gain",
        juce::NormalisableRange<float>(-40.0f, 6.0f, 0.1f),
        -6.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("attack", 1), "Attack",
        juce::NormalisableRange<float>(0.001f, 0.5f, 0.001f, 0.4f),
        0.05f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("release", 1), "Release",
        juce::NormalisableRange<float>(0.01f, 2.0f, 0.01f, 0.4f),
        0.3f));

    return { params.begin(), params.end() };
}

void DieselEngineSynthProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    for (auto& v : voices)
        v.prepare(sampleRate);
}

double DieselEngineSynthProcessor::noteToRPM(int midiNote) const
{
    // C2 (36) = 300 RPM, C4 (60) = 1200 RPM, C6 (84) = 4800 RPM
    return 300.0 * std::pow(2.0, (midiNote - 36) / 12.0);
}

int DieselEngineSynthProcessor::findFreeVoice() const
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (!voices[i].active)
            return i;
    return 0; // Steal oldest
}

void DieselEngineSynthProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                               juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // Read all parameters
    float rpmKnob    = rpmParam->load();
    float fuelKnob   = fuelParam->load();
    float turboMix   = turboMixParam->load();
    float drive      = driveParam->load();
    float exhCutoff  = exhaustCutoffParam->load();
    float gainDb     = masterGainParam->load();
    float gainLin    = juce::Decibels::decibelsToGain(gainDb);
    float attackTime = attackParam->load();
    float releaseTime = releaseParam->load();

    // Process MIDI events
    for (const auto metadata : midiMessages) {
        auto msg = metadata.getMessage();

        if (msg.isNoteOn()) {
            int note = msg.getNoteNumber();
            double rpm = noteToRPM(note);
            double vel = (double)msg.getFloatVelocity();
            double fuel = fuelKnob * vel;

            int vi = findFreeVoice();
            voices[vi].noteOn(rpm, std::max(fuel, 0.05), vel, note);

        } else if (msg.isNoteOff()) {
            int note = msg.getNoteNumber();
            for (int i = 0; i < MAX_VOICES; i++) {
                if (voices[i].assigned_note == note && voices[i].active)
                    voices[i].noteOff();
            }

        } else if (msg.isPitchWheel()) {
            // 0-16383, center = 8192
            double raw = (msg.getPitchWheelValue() - 8192) / 8192.0;
            pitchBendSemitones = raw * pitchBendRange;

        } else if (msg.isController()) {
            int cc = msg.getControllerNumber();
            float val = msg.getControllerValue() / 127.0f;

            if (cc == 1) {
                // CC1 = Mod wheel → vibrato
                modWheelValue = val;
            } else if (cc == 11) {
                // CC11 = Expression → fuel injection
                for (auto& v : voices) {
                    if (v.active)
                        v.fuel = fuelKnob * val * v.velocity;
                }
            }
            // CC-to-parameter mappings (standard CC numbers)
            // CC16 = RPM, CC17 = Fuel, CC18 = Turbo, CC19 = Drive
            // CC20 = Exhaust Cutoff, CC21 = Gain, CC22 = Attack, CC23 = Release
            // CC74 = Exhaust Cutoff (MPE Brightness — alternative)
            else if (cc == 16) {
                auto* p = apvts.getParameter("rpm");
                p->setValueNotifyingHost(val);
            } else if (cc == 17) {
                auto* p = apvts.getParameter("fuel");
                p->setValueNotifyingHost(val);
            } else if (cc == 18) {
                auto* p = apvts.getParameter("turboMix");
                p->setValueNotifyingHost(val);
            } else if (cc == 19) {
                auto* p = apvts.getParameter("drive");
                p->setValueNotifyingHost(val);
            } else if (cc == 20 || cc == 74) {
                auto* p = apvts.getParameter("exhaustCutoff");
                p->setValueNotifyingHost(val);
            } else if (cc == 21) {
                auto* p = apvts.getParameter("masterGain");
                p->setValueNotifyingHost(val);
            } else if (cc == 22) {
                auto* p = apvts.getParameter("attack");
                p->setValueNotifyingHost(val);
            } else if (cc == 23) {
                auto* p = apvts.getParameter("release");
                p->setValueNotifyingHost(val);
            }

        } else if (msg.isAllNotesOff() || msg.isAllSoundOff()) {
            for (auto& v : voices) v.noteOff();
            pitchBendSemitones = 0.0;
            modWheelValue = 0.0;
        }
    }

    // Update all voices with current knob values
    for (auto& v : voices) {
        v.exhaust_cutoff_ext = exhCutoff;
        v.turbo_mix_ext = turboMix;
        v.pitch_bend = pitchBendSemitones;
        v.mod_wheel = modWheelValue;
    }

    // Render audio
    int numSamples = buffer.getNumSamples();
    auto* outL = buffer.getWritePointer(0);
    auto* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

    for (int i = 0; i < numSamples; i++) {
        float sample = 0.0f;

        for (auto& v : voices)
            sample += v.processSample();

        // Drive (soft clip)
        sample = std::tanh(sample * drive);

        // Master gain
        sample *= gainLin;

        outL[i] = sample;
        if (outR) outR[i] = sample;
    }
}

void DieselEngineSynthProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void DieselEngineSynthProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* DieselEngineSynthProcessor::createEditor()
{
    return new DieselEngineSynthEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DieselEngineSynthProcessor();
}
