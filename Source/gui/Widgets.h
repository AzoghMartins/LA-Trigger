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

/** A ComboBox whose menu reports the chosen item straight to onItemChosen, synchronously.

    A plain ComboBox stores the chosen id and fires onChange asynchronously; the editor timer that
    keeps the box text in sync with the processor could run in between and put the old selection
    back, so the click loaded the old preset again (or nothing). Here the choice is delivered
    directly, and choosing the item that is already shown runs it again (e.g. to reload a preset
    after tweaking it). The displayed text is owned by whoever calls setText. */
class MenuBox : public juce::ComboBox
{
public:
    std::function<void (int itemId)> onItemChosen;

    void showPopup() override
    {
        auto menu = *getRootMenu();
        const auto shown = getText();
        bool ticked = false;
        for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
        {
            auto& item = it.getItem();
            if (item.itemID != 0)
            {
                item.isTicked = ! ticked && item.text == shown;
                ticked = ticked || item.isTicked;
            }
        }

        menu.setLookAndFeel (&getLookAndFeel());
        juce::Component::SafePointer<MenuBox> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (this)
                                .withMinimumWidth (getWidth())
                                .withMaximumNumColumns (1)
                                .withStandardItemHeight (juce::jlimit (12, 24, getHeight())),
                            [safe] (int result)
                            {
                                if (safe == nullptr)
                                    return;
                                safe->hidePopup();
                                if (result != 0 && safe->onItemChosen)
                                    safe->onItemChosen (result);
                            });
    }
};
} // namespace latrigger
