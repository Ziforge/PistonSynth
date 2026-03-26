#include "PluginEditor.h"

DieselEngineSynthEditor::DieselEngineSynthEditor(DieselEngineSynthProcessor& p)
    : AudioProcessorEditor(&p), proc(p)
{
    setSize(660, 280);

    setupSlider(rpmSlider,     rpmLabel,     "RPM",       "rpm",           rpmAtt);
    setupSlider(fuelSlider,    fuelLabel,    "Fuel",      "fuel",          fuelAtt);
    setupSlider(turboSlider,   turboLabel,   "Turbo",     "turboMix",      turboAtt);
    setupSlider(driveSlider,   driveLabel,   "Drive",     "drive",         driveAtt);
    setupSlider(exhaustSlider, exhaustLabel, "Exhaust",   "exhaustCutoff", exhaustAtt);
    setupSlider(gainSlider,    gainLabel,    "Gain",      "masterGain",    gainAtt);
    setupSlider(attackSlider,  attackLabel,  "Attack",    "attack",        attackAtt);
    setupSlider(releaseSlider, releaseLabel, "Release",   "release",       releaseAtt);
}

void DieselEngineSynthEditor::setupSlider(
    juce::Slider& slider, juce::Label& label, const juce::String& text,
    const juce::String& paramId,
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att)
{
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 65, 18);
    slider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFFFF6600));
    slider.setColour(juce::Slider::thumbColourId, juce::Colour(0xFFFFCC00));
    addAndMakeVisible(slider);

    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, juce::Colours::white);
    label.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(label);

    att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        proc.apvts, paramId, slider);
}

void DieselEngineSynthEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF1A1A2E));

    // Title
    g.setColour(juce::Colour(0xFF16213E));
    g.fillRect(0, 0, getWidth(), 50);

    g.setColour(juce::Colour(0xFFFF6600));
    g.setFont(juce::FontOptions(24.0f));
    g.drawText("PISTON SYNTH", getLocalBounds().removeFromTop(38),
               juce::Justification::centred);

    g.setColour(juce::Colour(0xFF888888));
    g.setFont(juce::FontOptions(10.0f));
    g.drawText("6-Cylinder Diesel Physical Model  |  Jonsson 2018, Lund University",
               getLocalBounds().removeFromTop(50).removeFromBottom(14),
               juce::Justification::centred);

    g.setColour(juce::Colour(0xFF333355));
    g.drawLine(15.0f, 52.0f, (float)getWidth() - 15.0f, 52.0f, 1.0f);

    // Section labels
    g.setColour(juce::Colour(0xFF666688));
    g.setFont(juce::FontOptions(9.0f));
    g.drawText("ENGINE", 10, 56, 120, 12, juce::Justification::left);
    g.drawText("TONE", 255, 56, 120, 12, juce::Justification::left);
    g.drawText("ENVELOPE", 490, 56, 120, 12, juce::Justification::left);

    // MIDI info
    g.setColour(juce::Colour(0xFF555577));
    g.setFont(juce::FontOptions(9.0f));
    g.drawText("MIDI: Note=RPM | Velocity=Fuel | PitchBend | ModWheel=Vibrato | CC11=Expression",
               0, getHeight() - 18, getWidth(), 16, juce::Justification::centred);
}

void DieselEngineSynthEditor::resized()
{
    int knobW = 75;
    int knobH = 75;
    int labelH = 14;
    int startY = 70;
    int gap = 6;

    int totalW = 8 * knobW + 7 * gap;
    int startX = (getWidth() - totalW) / 2;

    auto place = [&](juce::Slider& s, juce::Label& l, int col) {
        int x = startX + col * (knobW + gap);
        l.setBounds(x, startY, knobW, labelH);
        s.setBounds(x, startY + labelH, knobW, knobH);
    };

    place(rpmSlider,     rpmLabel,     0);
    place(fuelSlider,    fuelLabel,    1);
    place(turboSlider,   turboLabel,   2);
    place(exhaustSlider, exhaustLabel, 3);
    place(driveSlider,   driveLabel,   4);
    place(gainSlider,    gainLabel,    5);
    place(attackSlider,  attackLabel,  6);
    place(releaseSlider, releaseLabel, 7);
}
