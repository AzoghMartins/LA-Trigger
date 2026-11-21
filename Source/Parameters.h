#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/TriggerEngine.h"

namespace latrigger
{
namespace ParamID
{
    // Detection
    inline constexpr auto threshold   = "threshold";
    inline constexpr auto sensitivity = "sensitivity";
    inline constexpr auto retrigger   = "retrigger";
    inline constexpr auto dynAmount   = "dynAmount";
    inline constexpr auto dynRelease  = "dynRelease";
    inline constexpr auto hpf         = "hpf";
    inline constexpr auto lpf         = "lpf";
    // Sound match / bleed
    inline constexpr auto matchEnable = "matchEnable";
    inline constexpr auto matchAmount = "matchAmount";
    inline constexpr auto scEnable    = "scEnable";
    inline constexpr auto scMargin    = "scMargin";
    // Velocity
    inline constexpr auto inLow   = "inLow";
    inline constexpr auto inHigh  = "inHigh";
    inline constexpr auto velMin  = "velMin";
    inline constexpr auto velMax  = "velMax";
    inline constexpr auto velCurve = "velCurve";
    // MIDI
    inline constexpr auto note       = "note";
    inline constexpr auto channel    = "channel";
    inline constexpr auto noteLength = "noteLength";
    inline constexpr auto midiOffset = "midiOffset";
    inline constexpr auto midiThru   = "midiThru";
    // Gate
    inline constexpr auto gateEnable  = "gateEnable";
    inline constexpr auto gateAttack  = "gateAttack";
    inline constexpr auto gateHold    = "gateHold";
    inline constexpr auto gateRelease = "gateRelease";
    inline constexpr auto gateRange   = "gateRange";
    // Compressor
    inline constexpr auto compEnable    = "compEnable";
    inline constexpr auto compThreshold = "compThreshold";
    inline constexpr auto compRatio     = "compRatio";
    inline constexpr auto compAttack    = "compAttack";
    inline constexpr auto compRelease   = "compRelease";
    inline constexpr auto compKnee      = "compKnee";
    inline constexpr auto compMakeup    = "compMakeup";
    // Output
    inline constexpr auto outputMode = "outputMode";
    inline constexpr auto mix        = "mix";
    inline constexpr auto outputGain = "outputGain";
} // namespace ParamID

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Cached raw parameter pointers so the audio thread can build EngineParams cheaply. */
class ParameterCache
{
public:
    explicit ParameterCache (juce::AudioProcessorValueTreeState& state);
    EngineParams read() const noexcept;
    bool midiThru() const noexcept { return midiThruParam->load() > 0.5f; }

private:
    std::atomic<float>* get (juce::AudioProcessorValueTreeState& s, const char* id);

    std::atomic<float> *thresholdParam, *sensitivityParam, *retriggerParam, *dynAmountParam, *dynReleaseParam,
        *hpfParam, *lpfParam, *matchEnableParam, *matchAmountParam, *scEnableParam, *scMarginParam,
        *inLowParam, *inHighParam, *velMinParam, *velMaxParam, *velCurveParam, *noteParam, *channelParam,
        *noteLengthParam, *midiOffsetParam, *midiThruParam, *gateEnableParam, *gateAttackParam, *gateHoldParam,
        *gateReleaseParam, *gateRangeParam, *compEnableParam, *compThresholdParam, *compRatioParam,
        *compAttackParam, *compReleaseParam, *compKneeParam, *compMakeupParam, *outputModeParam, *mixParam,
        *outputGainParam;
};

struct FactoryPreset
{
    juce::String name;
    std::vector<std::pair<const char*, float>> values;
};

const std::vector<FactoryPreset>& getFactoryPresets();

juce::String midiNoteName (int note);
} // namespace latrigger
