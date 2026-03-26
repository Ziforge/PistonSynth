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
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    static constexpr int MAX_VOICES = 8;
    std::array<DieselEngineVoice, MAX_VOICES> voices;

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Parameter pointers
    std::atomic<float>* rpmParam = nullptr;
    std::atomic<float>* fuelParam = nullptr;
    std::atomic<float>* turboMixParam = nullptr;
    std::atomic<float>* driveParam = nullptr;
    std::atomic<float>* exhaustCutoffParam = nullptr;
    std::atomic<float>* masterGainParam = nullptr;
    std::atomic<float>* attackParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;

    // MIDI state
    double pitchBendSemitones = 0.0;
    double modWheelValue = 0.0;
    double pitchBendRange = 2.0; // ±2 semitones default

    // MIDI note → RPM
    double noteToRPM(int midiNote) const;
    int findFreeVoice() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DieselEngineSynthProcessor)
};
