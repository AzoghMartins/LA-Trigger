#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "Displays.h"
#include "Theme.h"
#include "Widgets.h"

namespace latrigger
{
/** All UI content, laid out at a fixed base size and scaled by the editor. */
class MainPanel : public juce::Component
{
public:
    static constexpr int baseWidth = 1280, baseHeight = 640;

    explicit MainPanel (LATriggerProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();

private:
    void layoutKnobGrid (Section& section, std::initializer_list<Knob*> knobs, int columns, juce::Rectangle<int> area);
    void updateSectionEnablement();
    void showClearMenu();

    LATriggerProcessor& processor;
    juce::AudioProcessorValueTreeState& state;

    // Header
    juce::ComboBox presetBox, spanBox;
    PillButton freezeButton { "FREEZE", Colours::learn };

    // Displays
    WaveformDisplay waveform;
    LevelMeters meters;
    Section matchSection, hitSection;
    MatchDisplay matchDisplay;
    HitInfoPanel hitInfo;
    PillButton learnHitButton { "LEARN HIT", Colours::learn }, learnBleedButton { "LEARN BLEED", Colours::danger };
    PillButton clearButton { "CLEAR" }, fitButton { "AUTO-FIT" }, testButton { "TEST" };
    PillButton scButton { "SC BLEED", Colours::threshold };
    juce::Slider matchSlider;
    juce::Label matchLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> matchAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> scAttachment;

    // Control sections
    Section detectionSection, velocitySection, midiSection, gateSection, compSection, outputSection;
    Knob threshold, sensitivity, retrigger, hpf, lpf, dynAmount, dynRelease, scMargin;
    VelocityCurveDisplay velocityCurve;
    Knob inLow, inHigh, velCurve, velMin, velMax;
    Knob note, channel, noteLength, midiOffset;
    PillButton thruButton { "THRU" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> thruAttachment;
    Knob gateAttack, gateHold, gateRelease, gateRange;
    Knob compThreshold, compRatio, compKnee, compAttack, compRelease, compMakeup;
    juce::ComboBox outputModeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> outputModeAttachment;
    Knob mix, outputGain;

    int blink = 0;
    juce::String infoText;
};

class LATriggerEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit LATriggerEditor (LATriggerProcessor&);
    ~LATriggerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Pulls new data from the audio thread and refreshes the displays (called at 60 Hz). */
    void updateDisplays() { panel.tick(); }

private:
    void timerCallback() override;

    LATriggerLookAndFeel lookAndFeel;
    MainPanel panel;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LATriggerEditor)
};
} // namespace latrigger
