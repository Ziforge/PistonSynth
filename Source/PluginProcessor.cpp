#include "PluginProcessor.h"
#include "PluginEditor.h"

DieselEngineSynthProcessor::DieselEngineSynthProcessor()
    : AudioProcessor(BusesProperties()
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    rpmParam         = apvts.getRawParameterValue("rpm");
    fuelParam        = apvts.getRawParameterValue("fuel");
    turboMixParam    = apvts.getRawParameterValue("turboMix");
    driveParam       = apvts.getRawParameterValue("drive");
    exhaustCutoffParam = apvts.getRawParameterValue("exhaustCutoff");
    masterGainParam  = apvts.getRawParameterValue("masterGain");

    for (auto& v : voices)
        v.active = false;
    std::fill(std::begin(voiceNoteMap), std::end(voiceNoteMap), -1);
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
        juce::NormalisableRange<float>(0.0f, 1.0f),
        0.5f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("turboMix", 1), "Turbo Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f),
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

    return { params.begin(), params.end() };
}

void DieselEngineSynthProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    for (auto& v : voices)
        v.prepare(sampleRate);
}

double DieselEngineSynthProcessor::noteToRPM(int midiNote) const
{
    // Map MIDI notes to RPM range:
    // C2 (36) = 300 RPM idle chug
    // C4 (60) = 1200 RPM
    // C6 (84) = 4800 RPM screaming
    // Exponential mapping so pitch intervals feel musical
    double baseRPM = 300.0;
    double semitones = (double)(midiNote - 36);
    return baseRPM * std::pow(2.0, semitones / 12.0);
}

int DieselEngineSynthProcessor::findFreeVoice() const
{
    // Find inactive voice
    for (int i = 0; i < MAX_VOICES; i++)
        if (!voices[i].active)
            return i;
    // Steal voice 0 if all busy
    return 0;
}

void DieselEngineSynthProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                               juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    float rpmKnob   = rpmParam->load();
    float fuelKnob  = fuelParam->load();
    float turboMix  = turboMixParam->load();
    float drive     = driveParam->load();
    float exhCutoff = exhaustCutoffParam->load();
    float gainDb    = masterGainParam->load();
    float gainLin   = juce::Decibels::decibelsToGain(gainDb);

    // Process MIDI
    for (const auto metadata : midiMessages) {
        auto msg = metadata.getMessage();

        if (msg.isNoteOn()) {
            int vi = findFreeVoice();
            double rpm = noteToRPM(msg.getNoteNumber());
            double fuel = fuelKnob * (msg.getFloatVelocity());
            voices[vi].noteOn(rpm, std::max(fuel, 0.1));
            voiceNoteMap[vi] = msg.getNoteNumber();

        } else if (msg.isNoteOff()) {
            for (int i = 0; i < MAX_VOICES; i++) {
                if (voiceNoteMap[i] == msg.getNoteNumber()) {
                    voices[i].noteOff();
                    voiceNoteMap[i] = -1;
                }
            }
        } else if (msg.isAllNotesOff() || msg.isAllSoundOff()) {
            for (int i = 0; i < MAX_VOICES; i++) {
                voices[i].noteOff();
                voiceNoteMap[i] = -1;
            }
        }
    }

    // Update active voice parameters from knobs (for non-MIDI use)
    // MIDI overrides RPM via note, but fuel/turbo/drive apply globally
    for (auto& v : voices) {
        if (v.active) {
            v.fuel = fuelKnob * (v.fuel / std::max(v.fuel, 0.01)); // Scale, keep velocity
        }
    }

    // Render audio
    int numSamples = buffer.getNumSamples();
    auto* outL = buffer.getWritePointer(0);
    auto* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

    for (int i = 0; i < numSamples; i++) {
        float sample = 0.0f;

        for (auto& v : voices)
            sample += v.processSample();

        // Apply drive (soft clip)
        sample = std::tanh(sample * drive);

        // Apply gain
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
