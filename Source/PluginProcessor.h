#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"
#include "dsp/TriggerEngine.h"

namespace latrigger
{
/** Lock-free single-producer / single-consumer queue (audio thread -> UI). */
template <typename T, int Capacity>
class SpscQueue
{
public:
    bool push (const T& item) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 + n2 == 0)
            return false;
        data[(size_t) (n1 > 0 ? s1 : s2)] = item;
        fifo.finishedWrite (1);
        return true;
    }

    bool pop (T& item) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (1, s1, n1, s2, n2);
        if (n1 + n2 == 0)
            return false;
        item = data[(size_t) (n1 > 0 ? s1 : s2)];
        fifo.finishedRead (1);
        return true;
    }

    void clear() noexcept { fifo.reset(); }

private:
    juce::AbstractFifo fifo { Capacity };
    std::array<T, (size_t) Capacity> data {};
};

/** Thread-safe snapshot of a learned profile for the UI. */
struct ProfileSnapshot
{
    std::array<std::atomic<float>, kNumBands> mean {};
    std::atomic<int> count { 0 };

    SpectrumFeatures get() const noexcept
    {
        SpectrumFeatures f {};
        for (size_t i = 0; i < f.size(); ++i)
            f[i] = mean[i].load (std::memory_order_relaxed);
        return f;
    }
};

class LATriggerProcessor : public juce::AudioProcessor
{
public:
    LATriggerProcessor();
    ~LATriggerProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return (int) getFactoryPresets().size(); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- UI interface (message thread) ------------------------------------------------------
    juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }

    void setLearnMode (LearnMode m) noexcept { requestedLearnMode.store ((int) m); }
    LearnMode getLearnMode() const noexcept { return (LearnMode) requestedLearnMode.load(); }
    void clearTargetProfile() noexcept { markedExamples[0].clear(); clearTargetRequest.store (true); }
    void clearRejectProfile() noexcept { markedExamples[1].clear(); clearRejectRequest.store (true); }

    /** Marks a captured hit (from the frozen display) as a real hit or as bleed, or removes its mark.
        The hit's fingerprint is moved into / out of the target and bleed profiles accordingly, and
        sound matching is switched on so the mark takes effect. Message thread only. */
    void setHitMark (const HitEvent& hit, HitMark newMark);
    void sendTestHit (int velocity) noexcept { manualVelocity.store (velocity); }
    /** Sets threshold and velocity input range from the hits captured while learning. */
    bool fitToLearnedHits();

    const ProfileSnapshot& getTargetSnapshot() const noexcept { return targetSnap; }
    const ProfileSnapshot& getRejectSnapshot() const noexcept { return rejectSnap; }

    double getEngineSampleRate() const noexcept { return engineSampleRate.load(); }
    int getDisplayDecimation() const noexcept { return displayDecimation.load(); }
    bool isSidechainConnected() const noexcept { return sidechainActive.load(); }

    SpscQueue<DisplayPoint, 16384> displayQueue;
    SpscQueue<HitEvent, 512> hitQueue;

private:
    struct Listener : EngineListener
    {
        juce::MidiBuffer* midi = nullptr;
        int offsetBase = 0;
        LATriggerProcessor* owner = nullptr;

        void midiEvent (int offset, uint8_t status, uint8_t d1, uint8_t d2) override
        {
            midi->addEvent (juce::MidiMessage (status, d1, d2), offsetBase + offset);
        }
        void displayPoint (const DisplayPoint& p) override { owner->displayQueue.push (p); }
        void hitEvent (const HitEvent& h) override { owner->hitQueue.push (h); }
    };

    /** One change to a learned profile, sent from the UI to the audio thread. */
    struct ProfileEdit
    {
        bool reject = false;
        int8_t averageDelta = 0, exampleDelta = 0;
        float peakDb = -100.0f;
        SpectrumFeatures spectrum {};
    };

    void handleRequests() noexcept;
    void publishProfiles() noexcept;
    void applyPendingProfiles() noexcept;

    juce::AudioProcessorValueTreeState apvts;
    ParameterCache params;
    TriggerEngine engine;
    Listener listener;

    juce::AudioBuffer<float> sidechainMono;

    std::atomic<int> requestedLearnMode { 0 };
    int activeLearnMode = 0;
    std::atomic<bool> clearTargetRequest { false }, clearRejectRequest { false };
    std::atomic<int> manualVelocity { -1 };
    SpscQueue<ProfileEdit, 256> profileEdits;
    std::array<std::vector<SpectrumFeatures>, 2> markedExamples; // target / bleed, message thread copy for saving

    ProfileSnapshot targetSnap, rejectSnap;
    int publishedTargetCount = -1, publishedRejectCount = -1;
    std::atomic<float> learnedMinDb { 100.0f }, learnedMaxDb { -100.0f };

    // Profiles loaded from a saved state, waiting to be handed to the audio thread.
    juce::SpinLock pendingLock;
    std::atomic<bool> pendingProfiles { false };
    SoundProfile pendingTarget, pendingReject;
    float pendingMinDb = 100.0f, pendingMaxDb = -100.0f;

    std::atomic<double> engineSampleRate { 48000.0 };
    std::atomic<int> displayDecimation { 64 };
    std::atomic<bool> sidechainActive { false };
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LATriggerProcessor)
};
} // namespace latrigger
