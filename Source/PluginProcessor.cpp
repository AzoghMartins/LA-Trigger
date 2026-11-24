#include "PluginProcessor.h"
#include "gui/PluginEditor.h"

namespace latrigger
{
LATriggerProcessor::LATriggerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Bleed Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "LATrigger", createParameterLayout()),
      params (apvts)
{
    listener.owner = this;
}

bool LATriggerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainOut = layouts.getMainOutputChannelSet();
    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != mainOut)
        return false;

    if (layouts.inputBuses.size() > 1)
    {
        const auto& sc = layouts.inputBuses[1];
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void LATriggerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int channels = std::max (1, getMainBusNumOutputChannels());
    engine.prepare (sampleRate, samplesPerBlock, channels);
    engine.setParams (params.read());
    engine.reset();
    setLatencySamples (engine.getLatencySamples());

    sidechainMono.setSize (1, std::max (512, samplesPerBlock), false, true, false);

    engineSampleRate.store (sampleRate);
    displayDecimation.store (engine.getDisplayDecimation());
    applyPendingProfiles();
    publishedTargetCount = publishedRejectCount = -1;
    publishProfiles();
}

//==============================================================================
void LATriggerProcessor::applyPendingProfiles() noexcept
{
    if (! pendingProfiles.load (std::memory_order_acquire))
        return;

    const juce::SpinLock::ScopedTryLockType lock (pendingLock);
    if (! lock.isLocked())
        return;

    engine.getTargetProfile() = pendingTarget;
    engine.getRejectProfile() = pendingReject;
    engine.setLearnedPeakRange (pendingMinDb, pendingMaxDb);
    pendingProfiles.store (false);
    publishedTargetCount = publishedRejectCount = -1;
}

void LATriggerProcessor::handleRequests() noexcept
{
    applyPendingProfiles();

    ProfileEdit edit;
    bool edited = false;
    while (profileEdits.pop (edit))
    {
        auto& profile = edit.reject ? engine.getRejectProfile() : engine.getTargetProfile();
        profile.addToAverage (edit.spectrum, edit.averageDelta);
        if (edit.exampleDelta > 0)
            profile.addExample (edit.spectrum);
        else if (edit.exampleDelta < 0)
            profile.removeExample (edit.spectrum);
        if (! edit.reject && edit.averageDelta > 0)
            engine.setLearnedPeakRange (std::min (engine.getLearnedPeakMinDb(), edit.peakDb),
                                        std::max (engine.getLearnedPeakMaxDb(), edit.peakDb));
        edited = true;
    }
    if (edited)
        publishedTargetCount = publishedRejectCount = -1;

    if (clearTargetRequest.exchange (false))
    {
        engine.getTargetProfile().clear();
        engine.resetLearnedPeakRange();
        learnedMinDb.store (100.0f);
        learnedMaxDb.store (-100.0f);
    }
    if (clearRejectRequest.exchange (false))
        engine.getRejectProfile().clear();

    const int mode = requestedLearnMode.load();
    if (mode != activeLearnMode)
    {
        if (mode == (int) LearnMode::target)
        {
            engine.resetLearnedPeakRange();
            learnedMinDb.store (100.0f);
            learnedMaxDb.store (-100.0f);
        }
        engine.setLearnMode ((LearnMode) mode);
        activeLearnMode = mode;
    }

    const int vel = manualVelocity.exchange (-1);
    if (vel > 0)
        engine.triggerManual (vel);
}

void LATriggerProcessor::publishProfiles() noexcept
{
    auto publish = [] (const SoundProfile& p, ProfileSnapshot& snap, int& published)
    {
        if (p.count == published)
            return;
        const auto m = p.mean();
        for (size_t i = 0; i < m.size(); ++i)
            snap.mean[i].store (m[i], std::memory_order_relaxed);
        snap.count.store (p.count, std::memory_order_release);
        published = p.count;
    };

    publish (engine.getTargetProfile(), targetSnap, publishedTargetCount);
    publish (engine.getRejectProfile(), rejectSnap, publishedRejectCount);
    learnedMinDb.store (engine.getLearnedPeakMinDb());
    learnedMaxDb.store (engine.getLearnedPeakMaxDb());
}

void LATriggerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    handleRequests();
    engine.setParams (params.read());

    if (! params.midiThru())
        midi.clear();

    auto mainBus = getBusBuffer (buffer, false, 0);
    const int numChannels = mainBus.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    // Sidechain (bleed source) summed to mono
    juce::AudioBuffer<float> scBus;
    bool hasSidechain = false;
    if (auto* bus = getBus (true, 1); bus != nullptr && bus->isEnabled() && bus->getNumberOfChannels() > 0)
    {
        scBus = getBusBuffer (buffer, true, 1);
        hasSidechain = scBus.getNumChannels() > 0;
    }
    sidechainActive.store (hasSidechain);

    listener.midi = &midi;

    const int chunkMax = sidechainMono.getNumSamples();
    for (int start = 0; start < numSamples; start += chunkMax)
    {
        const int n = std::min (chunkMax, numSamples - start);

        const float* scPtr = nullptr;
        if (hasSidechain)
        {
            auto* sc = sidechainMono.getWritePointer (0);
            juce::FloatVectorOperations::copy (sc, scBus.getReadPointer (0, start), n);
            for (int ch = 1; ch < scBus.getNumChannels(); ++ch)
                juce::FloatVectorOperations::add (sc, scBus.getReadPointer (ch, start), n);
            if (scBus.getNumChannels() > 1)
                juce::FloatVectorOperations::multiply (sc, 1.0f / (float) scBus.getNumChannels(), n);
            scPtr = sc;
        }

        std::array<float*, 2> ptrs {};
        for (int ch = 0; ch < std::min (numChannels, 2); ++ch)
            ptrs[(size_t) ch] = mainBus.getWritePointer (ch, start);

        listener.offsetBase = start;
        engine.process (ptrs.data(), ptrs.data(), std::min (numChannels, 2), scPtr, n, listener);
    }

    publishProfiles();
}

//==============================================================================
void LATriggerProcessor::setHitMark (const HitEvent& hit, HitMark newMark)
{
    if (hit.status == HitStatus::manual || newMark == hit.mark)
        return;

    // Which profiles the hit belongs to with a given mark. Without a mark it stays where learning
    // put it; a marked hit is in exactly one profile, both in the average and as an example.
    struct Membership { bool targetAverage, rejectAverage, targetExample, rejectExample; };
    auto membership = [&hit] (HitMark m) -> Membership
    {
        switch (m)
        {
            case HitMark::hit:   return { true, false, true, false };
            case HitMark::bleed: return { false, true, false, true };
            case HitMark::none:  break;
        }
        return { hit.status == HitStatus::learnedTarget, hit.status == HitStatus::learnedReject, false, false };
    };

    const auto before = membership (hit.mark), after = membership (newMark);
    auto delta = [] (bool was, bool now) { return (int8_t) ((now ? 1 : 0) - (was ? 1 : 0)); };

    for (const bool reject : { false, true })
    {
        ProfileEdit edit;
        edit.reject = reject;
        edit.peakDb = hit.peakDb;
        edit.spectrum = hit.spectrum;
        edit.averageDelta = reject ? delta (before.rejectAverage, after.rejectAverage) : delta (before.targetAverage, after.targetAverage);
        edit.exampleDelta = reject ? delta (before.rejectExample, after.rejectExample) : delta (before.targetExample, after.targetExample);
        if (edit.averageDelta == 0 && edit.exampleDelta == 0)
            continue;

        profileEdits.push (edit);

        auto& examples = markedExamples[reject ? 1 : 0];
        if (edit.exampleDelta > 0)
        {
            examples.push_back (hit.spectrum);
            if ((int) examples.size() > kMaxProfileExamples)
                examples.erase (examples.begin());
        }
        else if (edit.exampleDelta < 0)
        {
            if (auto it = std::find (examples.rbegin(), examples.rend(), hit.spectrum); it != examples.rend())
                examples.erase (std::next (it).base());
        }
    }

    // A mark only does something with sound matching on.
    if (newMark != HitMark::none)
        if (auto* p = apvts.getParameter (ParamID::matchEnable); p != nullptr && p->getValue() < 0.5f)
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (1.0f);
            p->endChangeGesture();
        }
}

bool LATriggerProcessor::fitToLearnedHits()
{
    const float lo = learnedMinDb.load(), hi = learnedMaxDb.load();
    if (lo > hi)
        return false;

    auto set = [this] (const char* id, float value)
    {
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (value));
            p->endChangeGesture();
        }
    };

    set (ParamID::threshold, juce::jlimit (-60.0f, 0.0f, lo - 6.0f));
    set (ParamID::inLow, juce::jlimit (-60.0f, 0.0f, lo - 1.0f));
    set (ParamID::inHigh, juce::jlimit (-60.0f, 0.0f, std::max (hi, lo + 3.0f)));
    return true;
}

void LATriggerProcessor::setCurrentProgram (int index)
{
    const auto& presets = getFactoryPresets();
    if (! juce::isPositiveAndBelow (index, (int) presets.size()))
        return;

    currentProgram = index;

    // One change per parameter, wrapped in a gesture: listed parameters take the preset value, the
    // rest their default. (Resetting everything first and then applying the preset sent two
    // competing edits per parameter, which some hosts merge or echo back out of order.)
    for (auto* p : getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (rp == nullptr)
            continue;
        float target = rp->getDefaultValue();
        for (const auto& [id, value] : presets[(size_t) index].values)
            if (rp->getParameterID() == id)
                target = rp->convertTo0to1 (value);
        if (std::abs (rp->getValue() - target) < 1.0e-6f)
            continue;
        rp->beginChangeGesture();
        rp->setValueNotifyingHost (target);
        rp->endChangeGesture();
    }
}

const juce::String LATriggerProcessor::getProgramName (int index)
{
    const auto& presets = getFactoryPresets();
    return juce::isPositiveAndBelow (index, (int) presets.size()) ? presets[(size_t) index].name : juce::String();
}

//==============================================================================
namespace
{
juce::String profileToString (const ProfileSnapshot& s)
{
    juce::StringArray parts;
    for (auto& v : s.mean)
        parts.add (juce::String (v.load(), 4));
    return parts.joinIntoString (",");
}

juce::String examplesToString (const std::vector<SpectrumFeatures>& examples)
{
    juce::StringArray items;
    for (const auto& e : examples)
    {
        juce::StringArray parts;
        for (auto v : e)
            parts.add (juce::String (v, 4));
        items.add (parts.joinIntoString (","));
    }
    return items.joinIntoString (";");
}

std::vector<SpectrumFeatures> examplesFromString (const juce::String& text)
{
    std::vector<SpectrumFeatures> result;
    juce::StringArray items;
    items.addTokens (text, ";", {});
    for (const auto& item : items)
    {
        juce::StringArray parts;
        parts.addTokens (item, ",", {});
        if (parts.size() != kNumBands || (int) result.size() >= kMaxProfileExamples)
            continue;
        SpectrumFeatures f {};
        for (int i = 0; i < kNumBands; ++i)
            f[(size_t) i] = parts[i].getFloatValue();
        result.push_back (f);
    }
    return result;
}

SoundProfile profileFromString (const juce::String& text, int count)
{
    SoundProfile p;
    juce::StringArray parts;
    parts.addTokens (text, ",", {});
    if (parts.size() != kNumBands || count <= 0)
        return p;
    for (int i = 0; i < kNumBands; ++i)
        p.sum[(size_t) i] = parts[i].getFloatValue() * (float) count;
    p.count = count;
    return p;
}
} // namespace

void LATriggerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    juce::ValueTree profiles ("PROFILES");
    profiles.setProperty ("targetCount", targetSnap.count.load(), nullptr);
    profiles.setProperty ("target", profileToString (targetSnap), nullptr);
    profiles.setProperty ("rejectCount", rejectSnap.count.load(), nullptr);
    profiles.setProperty ("reject", profileToString (rejectSnap), nullptr);
    profiles.setProperty ("learnedMin", learnedMinDb.load(), nullptr);
    profiles.setProperty ("learnedMax", learnedMaxDb.load(), nullptr);
    profiles.setProperty ("program", currentProgram, nullptr);
    profiles.setProperty ("targetExamples", examplesToString (markedExamples[0]), nullptr);
    profiles.setProperty ("rejectExamples", examplesToString (markedExamples[1]), nullptr);

    if (auto existing = state.getChildWithName ("PROFILES"); existing.isValid())
        state.removeChild (existing, nullptr);
    state.appendChild (profiles, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void LATriggerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    auto profiles = tree.getChildWithName ("PROFILES");
    if (profiles.isValid())
    {
        auto target = profileFromString (profiles["target"].toString(), (int) profiles["targetCount"]);
        auto reject = profileFromString (profiles["reject"].toString(), (int) profiles["rejectCount"]);
        currentProgram = (int) profiles.getProperty ("program", 0);

        markedExamples[0] = examplesFromString (profiles["targetExamples"].toString());
        markedExamples[1] = examplesFromString (profiles["rejectExamples"].toString());
        for (const auto& e : markedExamples[0])
            if (target.count > 0) target.addExample (e);
        for (const auto& e : markedExamples[1])
            if (reject.count > 0) reject.addExample (e);

        {
            const juce::SpinLock::ScopedLockType lock (pendingLock);
            pendingTarget = target;
            pendingReject = reject;
            pendingMinDb = (float) profiles.getProperty ("learnedMin", 100.0f);
            pendingMaxDb = (float) profiles.getProperty ("learnedMax", -100.0f);
            pendingProfiles.store (true, std::memory_order_release);
        }

        // Make the UI reflect the loaded profiles immediately.
        auto show = [] (const SoundProfile& p, ProfileSnapshot& snap)
        {
            const auto m = p.mean();
            for (size_t i = 0; i < m.size(); ++i)
                snap.mean[i].store (m[i]);
            snap.count.store (p.count);
        };
        show (target, targetSnap);
        show (reject, rejectSnap);
        learnedMinDb.store ((float) profiles.getProperty ("learnedMin", 100.0f));
        learnedMaxDb.store ((float) profiles.getProperty ("learnedMax", -100.0f));
        tree.removeChild (profiles, nullptr);
    }

    apvts.replaceState (tree);
}

juce::AudioProcessorEditor* LATriggerProcessor::createEditor()
{
    return new LATriggerEditor (*this);
}
} // namespace latrigger

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new latrigger::LATriggerProcessor();
}
