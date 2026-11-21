#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Theme.h"

namespace latrigger
{
/** Rotary knob with name above and value read-out below, bound to a parameter. */
class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& name,
          juce::Colour colour = Colours::accent, bool bipolar = false);

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::Slider slider;

private:
    juce::String name;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

/** Small round on/off switch drawn with a power icon. */
class PowerButton : public juce::ToggleButton
{
public:
    explicit PowerButton (juce::Colour onColour = Colours::accent) : colour (onColour) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    juce::Colour colour;
};

/** Titled panel that groups controls. Optionally hosts a power switch bound to a bool parameter. */
class Section : public juce::Component
{
public:
    Section (const juce::String& title, juce::AudioProcessorValueTreeState* state = nullptr,
             const juce::String& enableParamID = {}, juce::Colour colour = Colours::accent);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Area below the title where children go (in this component's coordinates). */
    juce::Rectangle<int> getContentArea() const;
    bool isActive() const { return power == nullptr || power->getToggleState(); }

    std::unique_ptr<PowerButton> power;
    std::function<void()> onActiveChanged;

private:
    juce::String title;
    juce::Colour colour;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

/** Vertical peak meters: input, output and gain reduction. */
class LevelMeters : public juce::Component
{
public:
    void setLevels (float inPeak, float outPeak, float grDb);
    void paint (juce::Graphics&) override;

private:
    float in = 0.0f, out = 0.0f, gr = 0.0f;
    float inHold = -100.0f, outHold = -100.0f;
    int inHoldAge = 0, outHoldAge = 0;
};

/** Pill toggle/text button. */
class PillButton : public juce::TextButton
{
public:
    PillButton (const juce::String& buttonText, juce::Colour onColour = Colours::accent)
        : juce::TextButton (buttonText)
    {
        setColour (juce::TextButton::buttonOnColourId, onColour);
    }
};
} // namespace latrigger
