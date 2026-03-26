#pragma once

#include <JuceHeader.h>
#include "DieselEngine.h"

class DieselEngineSynthProcessor : public juce::AudioProcessor
{
public:
    DieselEngineSynthProcessor();
    ~DieselEngineSynthProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Engine voices (polyphonic — multiple engine notes)
    static constexpr int MAX_VOICES = 8;
    std::array<DieselEngineVoice, MAX_VOICES> voices;
    int voiceNoteMap[MAX_VOICES] = {}; // MIDI note for each voice

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Parameter pointers
    std::atomic<float>* rpmParam = nullptr;
    std::atomic<float>* fuelParam = nullptr;
    std::atomic<float>* turboMixParam = nullptr;
    std::atomic<float>* driveParam = nullptr;
    std::atomic<float>* exhaustCutoffParam = nullptr;
    std::atomic<float>* masterGainParam = nullptr;

    // MIDI note → RPM mapping
    double noteToRPM(int midiNote) const;

    // Find free voice or steal oldest
    int findFreeVoice() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DieselEngineSynthProcessor)
};
