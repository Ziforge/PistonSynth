#include "PluginEditor.h"

DieselEngineSynthEditor::DieselEngineSynthEditor(DieselEngineSynthProcessor& p)
    : AudioProcessorEditor(&p), proc(p)
{
    setSize(600, 400);

    setupSlider(rpmSlider,     rpmLabel,     "RPM",             "rpm",           rpmAtt);
    setupSlider(fuelSlider,    fuelLabel,    "Fuel",            "fuel",          fuelAtt);
    setupSlider(turboSlider,   turboLabel,   "Turbo",           "turboMix",      turboAtt);
    setupSlider(driveSlider,   driveLabel,   "Drive",           "drive",         driveAtt);
    setupSlider(exhaustSlider, exhaustLabel, "Exhaust",         "exhaustCutoff", exhaustAtt);
    setupSlider(gainSlider,    gainLabel,    "Gain",            "masterGain",    gainAtt);
}

void DieselEngineSynthEditor::setupSlider(
    juce::Slider& slider, juce::Label& label, const juce::String& text,
    const juce::String& paramId,
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att)
{
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 20);
    slider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFFFF6600));
    slider.setColour(juce::Slider::thumbColourId, juce::Colour(0xFFFFCC00));
    addAndMakeVisible(slider);

    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(label);

    att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        proc.apvts, paramId, slider);
}

void DieselEngineSynthEditor::paint(juce::Graphics& g)
{
    // Dark metallic background
    g.fillAll(juce::Colour(0xFF1A1A2E));

    // Title bar
    g.setColour(juce::Colour(0xFF16213E));
    g.fillRect(0, 0, getWidth(), 60);

    g.setColour(juce::Colour(0xFFFF6600));
    g.setFont(juce::FontOptions(28.0f));
    g.drawText("DIESEL ENGINE SYNTH", getLocalBounds().removeFromTop(45),
               juce::Justification::centred);

    g.setColour(juce::Colour(0xFF888888));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("6-Cylinder Compression-Ignition Physical Model  |  Jonsson 2018",
               getLocalBounds().removeFromTop(60).removeFromBottom(18),
               juce::Justification::centred);

    // Divider
    g.setColour(juce::Colour(0xFF333355));
    g.drawLine(20.0f, 62.0f, (float)getWidth() - 20.0f, 62.0f, 1.0f);

    // Section labels
    g.setColour(juce::Colour(0xFF666688));
    g.setFont(juce::FontOptions(10.0f));
    g.drawText("ENGINE", 15, 68, 200, 15, juce::Justification::left);
    g.drawText("CHARACTER", 310, 68, 200, 15, juce::Justification::left);

    // MIDI info
    g.setColour(juce::Colour(0xFF555577));
    g.setFont(juce::FontOptions(10.0f));
    g.drawText("MIDI: C2=300rpm  C4=1200rpm  C6=4800rpm  |  Velocity = Fuel",
               0, getHeight() - 22, getWidth(), 20, juce::Justification::centred);
}

void DieselEngineSynthEditor::resized()
{
    int knobW = 90;
    int knobH = 90;
    int labelH = 18;
    int startY = 85;
    int gap = 8;

    // Row of 6 knobs
    int totalW = 6 * knobW + 5 * gap;
    int startX = (getWidth() - totalW) / 2;

    auto placeKnob = [&](juce::Slider& s, juce::Label& l, int col) {
        int x = startX + col * (knobW + gap);
        l.setBounds(x, startY, knobW, labelH);
        s.setBounds(x, startY + labelH, knobW, knobH);
    };

    placeKnob(rpmSlider,     rpmLabel,     0);
    placeKnob(fuelSlider,    fuelLabel,    1);
    placeKnob(turboSlider,   turboLabel,   2);
    placeKnob(driveSlider,   driveLabel,   3);
    placeKnob(exhaustSlider, exhaustLabel, 4);
    placeKnob(gainSlider,    gainLabel,    5);
}
