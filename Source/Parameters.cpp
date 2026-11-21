#include "Parameters.h"

namespace latrigger
{
namespace
{
using APF = juce::AudioParameterFloat;
using APFA = juce::AudioParameterFloatAttributes;

juce::String formatDb (float v, int) { return juce::String (v, 1) + " dB"; }
juce::String formatMs (float v, int) { return (v < 10.0f ? juce::String (v, 2) : juce::String (juce::roundToInt (v))) + " ms"; }
juce::String formatPct (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; }
juce::String formatHz (float v)
{
    return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                        : juce::String (juce::roundToInt (v)) + " Hz";
}

juce::NormalisableRange<float> skewed (float lo, float hi, float centre, float step = 0.0f)
{
    juce::NormalisableRange<float> r (lo, hi, step);
    r.setSkewForCentre (centre);
    return r;
}

std::unique_ptr<APF> floatParam (const char* id, const juce::String& name, juce::NormalisableRange<float> range,
                                 float def, std::function<juce::String (float, int)> fmt, const juce::String& label = {})
{
    return std::make_unique<APF> (juce::ParameterID { id, 1 }, name, range, def,
                                  APFA().withLabel (label).withStringFromValueFunction (std::move (fmt)));
}
} // namespace

juce::String midiNoteName (int note)
{
    return juce::MidiMessage::getMidiNoteName (note, true, true, 3) + " (" + juce::String (note) + ")";
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto detection = std::make_unique<juce::AudioProcessorParameterGroup> ("detection", "Detection", "|");
    detection->addChild (floatParam (ParamID::threshold, "Threshold", { -60.0f, 0.0f, 0.1f }, -30.0f, formatDb, "dB"));
    detection->addChild (floatParam (ParamID::sensitivity, "Sensitivity", { 0.0f, 100.0f, 1.0f }, 50.0f, formatPct, "%"));
    detection->addChild (floatParam (ParamID::retrigger, "Retrigger", skewed (5.0f, 500.0f, 50.0f, 0.1f), 40.0f, formatMs, "ms"));
    detection->addChild (floatParam (ParamID::dynAmount, "Dynamic Threshold", { 0.0f, 100.0f, 1.0f }, 0.0f,
                                     [] (float v, int) { return v <= 0.0f ? juce::String ("Off") : formatPct (v, 0); }, "%"));
    detection->addChild (floatParam (ParamID::dynRelease, "Dynamic Release", skewed (10.0f, 2000.0f, 200.0f, 1.0f), 200.0f, formatMs, "ms"));
    detection->addChild (floatParam (ParamID::hpf, "Detect HPF", skewed (20.0f, 5000.0f, 300.0f, 1.0f), 20.0f,
                                     [] (float v, int) { return v <= 20.5f ? juce::String ("Off") : formatHz (v); }, "Hz"));
    detection->addChild (floatParam (ParamID::lpf, "Detect LPF", skewed (100.0f, 20000.0f, 2000.0f, 1.0f), 20000.0f,
                                     [] (float v, int) { return v >= 19999.0f ? juce::String ("Off") : formatHz (v); }, "Hz"));
    layout.add (std::move (detection));

    auto match = std::make_unique<juce::AudioProcessorParameterGroup> ("match", "Sound Match", "|");
    match->addChild (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamID::matchEnable, 1 }, "Sound Match", false));
    match->addChild (floatParam (ParamID::matchAmount, "Match Threshold", { 0.0f, 100.0f, 1.0f }, 50.0f, formatPct, "%"));
    match->addChild (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamID::scEnable, 1 }, "Sidechain Bleed Reject", false));
    match->addChild (floatParam (ParamID::scMargin, "Sidechain Margin", { -24.0f, 24.0f, 0.1f }, 0.0f, formatDb, "dB"));
    layout.add (std::move (match));

    auto velocity = std::make_unique<juce::AudioProcessorParameterGroup> ("velocity", "Velocity", "|");
    velocity->addChild (floatParam (ParamID::inLow, "Input Low", { -60.0f, 0.0f, 0.1f }, -40.0f, formatDb, "dB"));
    velocity->addChild (floatParam (ParamID::inHigh, "Input High", { -60.0f, 0.0f, 0.1f }, -6.0f, formatDb, "dB"));
    velocity->addChild (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { ParamID::velMin, 1 }, "Velocity Min", 1, 127, 1));
    velocity->addChild (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { ParamID::velMax, 1 }, "Velocity Max", 1, 127, 127));
    velocity->addChild (floatParam (ParamID::velCurve, "Velocity Curve", { -100.0f, 100.0f, 1.0f }, 0.0f,
                                    [] (float v, int) { return std::abs (v) < 0.5f ? juce::String ("Linear") : (v > 0 ? "+" : "") + juce::String (juce::roundToInt (v)); }));
    layout.add (std::move (velocity));

    auto midi = std::make_unique<juce::AudioProcessorParameterGroup> ("midi", "MIDI", "|");
    midi->addChild (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { ParamID::note, 1 }, "MIDI Note", 0, 127, 36,
                                                               juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return midiNoteName (v); })));
    midi->addChild (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { ParamID::channel, 1 }, "MIDI Channel", 1, 16, 10,
                                                               juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return "Ch " + juce::String (v); })));
    midi->addChild (floatParam (ParamID::noteLength, "Note Length", skewed (5.0f, 1000.0f, 100.0f, 1.0f), 50.0f, formatMs, "ms"));
    midi->addChild (floatParam (ParamID::midiOffset, "MIDI Offset", { -5.0f, 5.0f, 0.01f }, 0.0f,
                                [] (float v, int) { return (v > 0 ? "+" : "") + juce::String (v, 2) + " ms"; }, "ms"));
    midi->addChild (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamID::midiThru, 1 }, "MIDI Thru", true));
    layout.add (std::move (midi));

    auto gate = std::make_unique<juce::AudioProcessorParameterGroup> ("gate", "Gate", "|");
    gate->addChild (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamID::gateEnable, 1 }, "Gate", true));
    gate->addChild (floatParam (ParamID::gateAttack, "Gate Attack", skewed (0.01f, 5.0f, 0.5f, 0.01f), 0.5f, formatMs, "ms"));
    gate->addChild (floatParam (ParamID::gateHold, "Gate Hold", skewed (0.0f, 1000.0f, 100.0f, 1.0f), 80.0f, formatMs, "ms"));
    gate->addChild (floatParam (ParamID::gateRelease, "Gate Release", skewed (1.0f, 2000.0f, 150.0f, 1.0f), 120.0f, formatMs, "ms"));
    gate->addChild (floatParam (ParamID::gateRange, "Gate Range", { -80.0f, 0.0f, 0.1f }, -80.0f,
                                [] (float v, int) { return v <= -79.9f ? juce::String ("-inf dB") : formatDb (v, 0); }, "dB"));
    layout.add (std::move (gate));

    auto comp = std::make_unique<juce::AudioProcessorParameterGroup> ("comp", "Compressor", "|");
    comp->addChild (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamID::compEnable, 1 }, "Compressor", false));
    comp->addChild (floatParam (ParamID::compThreshold, "Comp Threshold", { -60.0f, 0.0f, 0.1f }, -18.0f, formatDb, "dB"));
    comp->addChild (floatParam (ParamID::compRatio, "Comp Ratio", skewed (1.0f, 20.0f, 4.0f, 0.1f), 4.0f,
                                [] (float v, int) { return juce::String (v, 1) + ":1"; }));
    comp->addChild (floatParam (ParamID::compAttack, "Comp Attack", skewed (0.1f, 100.0f, 10.0f, 0.01f), 5.0f, formatMs, "ms"));
    comp->addChild (floatParam (ParamID::compRelease, "Comp Release", skewed (10.0f, 1000.0f, 120.0f, 1.0f), 100.0f, formatMs, "ms"));
    comp->addChild (floatParam (ParamID::compKnee, "Comp Knee", { 0.0f, 24.0f, 0.1f }, 6.0f, formatDb, "dB"));
    comp->addChild (floatParam (ParamID::compMakeup, "Comp Makeup", { 0.0f, 24.0f, 0.1f }, 0.0f, formatDb, "dB"));
    layout.add (std::move (comp));

    auto output = std::make_unique<juce::AudioProcessorParameterGroup> ("output", "Output", "|");
    output->addChild (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ParamID::outputMode, 1 }, "Output Mode",
                                                                    juce::StringArray { "Processed", "Listen Detection", "MIDI Only" }, 0));
    output->addChild (floatParam (ParamID::mix, "Mix", { 0.0f, 100.0f, 1.0f }, 100.0f, formatPct, "%"));
    output->addChild (floatParam (ParamID::outputGain, "Output Gain", { -24.0f, 12.0f, 0.1f }, 0.0f, formatDb, "dB"));
    layout.add (std::move (output));

    return layout;
}

//==============================================================================
ParameterCache::ParameterCache (juce::AudioProcessorValueTreeState& s)
{
    thresholdParam = get (s, ParamID::threshold);
    sensitivityParam = get (s, ParamID::sensitivity);
    retriggerParam = get (s, ParamID::retrigger);
    dynAmountParam = get (s, ParamID::dynAmount);
    dynReleaseParam = get (s, ParamID::dynRelease);
    hpfParam = get (s, ParamID::hpf);
    lpfParam = get (s, ParamID::lpf);
    matchEnableParam = get (s, ParamID::matchEnable);
    matchAmountParam = get (s, ParamID::matchAmount);
    scEnableParam = get (s, ParamID::scEnable);
    scMarginParam = get (s, ParamID::scMargin);
    inLowParam = get (s, ParamID::inLow);
    inHighParam = get (s, ParamID::inHigh);
    velMinParam = get (s, ParamID::velMin);
    velMaxParam = get (s, ParamID::velMax);
    velCurveParam = get (s, ParamID::velCurve);
    noteParam = get (s, ParamID::note);
    channelParam = get (s, ParamID::channel);
    noteLengthParam = get (s, ParamID::noteLength);
    midiOffsetParam = get (s, ParamID::midiOffset);
    midiThruParam = get (s, ParamID::midiThru);
    gateEnableParam = get (s, ParamID::gateEnable);
    gateAttackParam = get (s, ParamID::gateAttack);
    gateHoldParam = get (s, ParamID::gateHold);
    gateReleaseParam = get (s, ParamID::gateRelease);
    gateRangeParam = get (s, ParamID::gateRange);
    compEnableParam = get (s, ParamID::compEnable);
    compThresholdParam = get (s, ParamID::compThreshold);
    compRatioParam = get (s, ParamID::compRatio);
    compAttackParam = get (s, ParamID::compAttack);
    compReleaseParam = get (s, ParamID::compRelease);
    compKneeParam = get (s, ParamID::compKnee);
    compMakeupParam = get (s, ParamID::compMakeup);
    outputModeParam = get (s, ParamID::outputMode);
    mixParam = get (s, ParamID::mix);
    outputGainParam = get (s, ParamID::outputGain);
}

std::atomic<float>* ParameterCache::get (juce::AudioProcessorValueTreeState& s, const char* id)
{
    auto* p = s.getRawParameterValue (id);
    jassert (p != nullptr);
    return p;
}

EngineParams ParameterCache::read() const noexcept
{
    EngineParams p;
    p.thresholdDb = thresholdParam->load();
    p.sensitivity = sensitivityParam->load();
    p.retriggerMs = retriggerParam->load();
    p.dynAmount = dynAmountParam->load();
    p.dynReleaseMs = dynReleaseParam->load();
    p.hpfHz = hpfParam->load();
    p.lpfHz = lpfParam->load();
    p.matchEnabled = matchEnableParam->load() > 0.5f;
    p.matchThreshold = matchAmountParam->load();
    p.scEnabled = scEnableParam->load() > 0.5f;
    p.scMarginDb = scMarginParam->load();
    p.inLowDb = inLowParam->load();
    p.inHighDb = inHighParam->load();
    p.velMin = (int) velMinParam->load();
    p.velMax = (int) velMaxParam->load();
    p.velCurve = velCurveParam->load();
    p.note = (int) noteParam->load();
    p.channel = (int) channelParam->load();
    p.noteLengthMs = noteLengthParam->load();
    p.midiOffsetMs = midiOffsetParam->load();
    p.gateEnabled = gateEnableParam->load() > 0.5f;
    p.gateAttackMs = gateAttackParam->load();
    p.gateHoldMs = gateHoldParam->load();
    p.gateReleaseMs = gateReleaseParam->load();
    p.gateRangeDb = gateRangeParam->load();
    p.compEnabled = compEnableParam->load() > 0.5f;
    p.compThresholdDb = compThresholdParam->load();
    p.compRatio = compRatioParam->load();
    p.compAttackMs = compAttackParam->load();
    p.compReleaseMs = compReleaseParam->load();
    p.compKneeDb = compKneeParam->load();
    p.compMakeupDb = compMakeupParam->load();
    p.outputMode = (int) outputModeParam->load();
    p.mix = mixParam->load();
    p.outputGainDb = outputGainParam->load();
    return p;
}

//==============================================================================
const std::vector<FactoryPreset>& getFactoryPresets()
{
    static const std::vector<FactoryPreset> presets {
        { "Default", {} },
        { "Kick In", { { ParamID::threshold, -24.0f }, { ParamID::hpf, 20.0f }, { ParamID::lpf, 180.0f },
                       { ParamID::retrigger, 60.0f }, { ParamID::note, 36.0f }, { ParamID::gateHold, 120.0f },
                       { ParamID::gateRelease, 150.0f }, { ParamID::inLow, -36.0f }, { ParamID::inHigh, -4.0f } } },
        { "Kick Out / Sub", { { ParamID::threshold, -22.0f }, { ParamID::hpf, 20.0f }, { ParamID::lpf, 120.0f },
                              { ParamID::retrigger, 70.0f }, { ParamID::note, 36.0f }, { ParamID::gateHold, 160.0f },
                              { ParamID::gateRelease, 220.0f } } },
        { "Snare Top", { { ParamID::threshold, -26.0f }, { ParamID::hpf, 150.0f }, { ParamID::lpf, 9000.0f },
                         { ParamID::retrigger, 40.0f }, { ParamID::dynAmount, 30.0f }, { ParamID::note, 38.0f },
                         { ParamID::gateHold, 90.0f }, { ParamID::gateRelease, 130.0f }, { ParamID::sensitivity, 60.0f } } },
        { "Snare Bottom", { { ParamID::threshold, -28.0f }, { ParamID::hpf, 350.0f }, { ParamID::lpf, 12000.0f },
                            { ParamID::retrigger, 40.0f }, { ParamID::note, 38.0f }, { ParamID::gateHold, 70.0f },
                            { ParamID::gateRelease, 100.0f } } },
        { "Rack Tom", { { ParamID::threshold, -28.0f }, { ParamID::hpf, 70.0f }, { ParamID::lpf, 1500.0f },
                        { ParamID::retrigger, 60.0f }, { ParamID::dynAmount, 40.0f }, { ParamID::note, 48.0f },
                        { ParamID::gateHold, 200.0f }, { ParamID::gateRelease, 350.0f } } },
        { "Floor Tom", { { ParamID::threshold, -28.0f }, { ParamID::hpf, 45.0f }, { ParamID::lpf, 900.0f },
                         { ParamID::retrigger, 70.0f }, { ParamID::dynAmount, 40.0f }, { ParamID::note, 41.0f },
                         { ParamID::gateHold, 250.0f }, { ParamID::gateRelease, 450.0f } } },
        { "Overheads -> Kick", { { ParamID::threshold, -20.0f }, { ParamID::hpf, 20.0f }, { ParamID::lpf, 100.0f },
                                 { ParamID::retrigger, 80.0f }, { ParamID::sensitivity, 40.0f }, { ParamID::note, 36.0f },
                                 { ParamID::outputMode, 2.0f } } },
        { "Live / Tight Gate", { { ParamID::threshold, -30.0f }, { ParamID::gateAttack, 0.1f }, { ParamID::gateHold, 40.0f },
                                 { ParamID::gateRelease, 60.0f }, { ParamID::retrigger, 30.0f } } },
    };
    return presets;
}
} // namespace latrigger
