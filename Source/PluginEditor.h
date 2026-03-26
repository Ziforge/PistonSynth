#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class DieselEngineSynthEditor : public juce::AudioProcessorEditor
{
public:
    explicit DieselEngineSynthEditor(DieselEngineSynthProcessor&);
    ~DieselEngineSynthEditor() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    DieselEngineSynthProcessor& proc;

    // Knobs
    juce::Slider rpmSlider, fuelSlider, turboSlider, driveSlider, exhaustSlider, gainSlider;
    juce::Label rpmLabel, fuelLabel, turboLabel, driveLabel, exhaustLabel, gainLabel;

    // Attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        rpmAtt, fuelAtt, turboAtt, driveAtt, exhaustAtt, gainAtt;

    // RPM tachometer display
    juce::Label tachLabel;

    void setupSlider(juce::Slider& slider, juce::Label& label, const juce::String& text,
                     const juce::String& paramId,
                     std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DieselEngineSynthEditor)
};
