#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../dsp/TriggerEngine.h"
#include "Theme.h"

#include <deque>

namespace latrigger
{
class LATriggerProcessor;

/** Real-time scrolling view: input, output, detection envelope, thresholds, gate and hits. */
class WaveformDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    void paintOverChildren (juce::Graphics& g) override { Hardware::drawGlass (g, getLocalBounds().toFloat()); }
    explicit WaveformDisplay (LATriggerProcessor&);

    void addPoint (const DisplayPoint& p);
    void addHit (const HitEvent& h);
    void setTimeSpan (double seconds) { spanSeconds = seconds; repaint(); }
    void setFrozen (bool f);
    /** Forgets the marks of one kind shown on hits (after that learned profile was cleared). */
    void clearMarks (HitMark kind);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseExit (const juce::MouseEvent&) override;

    std::function<void (double)> onSpanChanged;
    /** Called after the user marked a hit (to refresh read-outs and show a confirmation). */
    std::function<void (const HitEvent&)> onHitMarked;

private:
    juce::Rectangle<float> graphArea() const;
    juce::Rectangle<float> gateLane() const;
    float dbToY (float db, juce::Rectangle<float> area) const;
    float yToDb (float y, juce::Rectangle<float> area) const;
    bool nearThreshold (juce::Point<float> p) const;
    float sampleToX (double samplePos) const;
    /** The hit under the mouse in the frozen view, or nullptr. */
    HitEvent* hitAt (juce::Point<float> p);
    void showHitMenu (int64_t samplePos);
    void updateTooltip();

    LATriggerProcessor& processor;
    juce::RangedAudioParameter* thresholdParam = nullptr;
    std::atomic<float>* thresholdValue = nullptr;
    std::atomic<float>* gateEnabled = nullptr;

    static constexpr int ringSize = 1 << 15;
    std::vector<DisplayPoint> ring;
    int64_t latest = -1;
    std::deque<HitEvent> hits;
    double spanSeconds = 3.0;
    bool frozen = false;
    bool draggingThreshold = false, hoverThreshold = false;
    int64_t hoverHit = -1; // samplePos of the hit under the mouse (frozen view only)

    static constexpr float minDb = -60.0f, maxDb = 0.0f;
};

/** Shows the learned target / bleed fingerprints and the spectrum of the last hit. */
class MatchDisplay : public juce::Component
{
public:
    void paintOverChildren (juce::Graphics& g) override { Hardware::drawGlass (g, getLocalBounds().toFloat(), 4.0f); }
    explicit MatchDisplay (LATriggerProcessor& p) : processor (p) {}
    void setLastHit (const HitEvent& h) { lastHit = h; hasHit = h.status != HitStatus::manual; repaint(); }
    void paint (juce::Graphics&) override;

private:
    LATriggerProcessor& processor;
    HitEvent lastHit;
    bool hasHit = false;
};

/** Input level -> MIDI velocity transfer curve with the last hit marked on it. */
class VelocityCurveDisplay : public juce::Component
{
public:
    void paintOverChildren (juce::Graphics& g) override { Hardware::drawGlass (g, getLocalBounds().toFloat(), 4.0f); }
    explicit VelocityCurveDisplay (juce::AudioProcessorValueTreeState& s);
    void setLastHit (float peakDb, int velocity) { lastDb = peakDb; lastVel = velocity; hitAge = 0; repaint(); }
    void tick();
    void paint (juce::Graphics&) override;

private:
    juce::AudioProcessorValueTreeState& state;
    float lastDb = -100.0f;
    int lastVel = 0, hitAge = 1000;
};

/** Big read-out of the most recent trigger. */
class HitInfoPanel : public juce::Component
{
public:
    void paintOverChildren (juce::Graphics& g) override { Hardware::drawGlass (g, getLocalBounds().toFloat(), 4.0f); }
    explicit HitInfoPanel (juce::AudioProcessorValueTreeState& s) : state (s) {}
    void setHit (const HitEvent& h);
    void tick();
    void paint (juce::Graphics&) override;
    void resetCounters() { accepted = rejected = 0; repaint(); }

private:
    juce::AudioProcessorValueTreeState& state;
    HitEvent last;
    bool hasHit = false;
    int accepted = 0, rejected = 0;
    float flash = 0.0f;
};
} // namespace latrigger
