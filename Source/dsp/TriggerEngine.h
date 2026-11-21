#pragma once

#include "Biquad.h"
#include "FFT.h"

#include <array>
#include <cstdint>
#include <vector>

namespace latrigger
{
constexpr int kNumBands = 24;
using SpectrumFeatures = std::array<float, kNumBands>;

/** All user-facing settings consumed by the engine (plain values, no JUCE dependency). */
struct EngineParams
{
    // Detection
    float thresholdDb   = -30.0f;
    float sensitivity   = 50.0f;   // 0..100 %, how small a rise in level still counts as a transient
    float retriggerMs   = 40.0f;
    float dynAmount     = 0.0f;    // 0..100 %, dynamic (decaying) threshold after each hit
    float dynReleaseMs  = 200.0f;
    float hpfHz         = 20.0f;   // <= 20 Hz means off
    float lpfHz         = 20000.0f;// >= 20 kHz means off

    // Sound match
    bool  matchEnabled   = false;
    float matchThreshold = 50.0f;  // 0..100 %

    // Sidechain bleed rejection
    bool  scEnabled  = false;
    float scMarginDb = 0.0f;

    // Velocity
    float inLowDb  = -40.0f;
    float inHighDb = -6.0f;
    int   velMin   = 1;
    int   velMax   = 127;
    float velCurve = 0.0f;         // -100..100

    // MIDI
    int   note         = 36;
    int   channel      = 10;       // 1..16
    float noteLengthMs = 50.0f;
    float midiOffsetMs = 0.0f;

    // Gate
    bool  gateEnabled   = true;
    float gateAttackMs  = 0.5f;
    float gateHoldMs    = 80.0f;
    float gateReleaseMs = 120.0f;
    float gateRangeDb   = -80.0f;

    // Compressor
    bool  compEnabled     = false;
    float compThresholdDb = -18.0f;
    float compRatio       = 4.0f;
    float compAttackMs    = 5.0f;
    float compReleaseMs   = 100.0f;
    float compKneeDb      = 6.0f;
    float compMakeupDb    = 0.0f;

    // Output
    int   outputMode   = 0;        // 0 = processed audio, 1 = listen to detection filter, 2 = mute (MIDI only)
    float outputGainDb = 0.0f;
    float mix          = 100.0f;   // 0..100 % processed vs. (latency-aligned) dry
};

/** One column of the scrolling display (in output timeline). */
struct DisplayPoint
{
    int64_t index = 0;      // chunk index (output timeline)
    float inPeak  = 0.0f;   // raw input peak (linear)
    float detPeak = 0.0f;   // detection envelope peak (linear)
    float thrDb   = -100.0f;// effective threshold (static or dynamic)
    float outPeak = 0.0f;   // output peak (linear)
    float gate    = 1.0f;   // gate gain (0..1)
    float grDb    = 0.0f;   // compressor gain reduction (positive dB)
};

enum class HitStatus : uint8_t
{
    accepted,
    learnedTarget,
    learnedReject,
    rejectedMatch,
    rejectedBleed,
    rejectedRetrigger,
    manual
};

struct HitEvent
{
    int64_t samplePos = 0;   // output timeline sample position of the transient
    float peakDb      = -100.0f;
    int velocity      = 0;
    float matchScore  = -1.0f; // 0..100, -1 if no profile
    float rejectScore = -1.0f; // 0..100, -1 if no reject profile
    HitStatus status  = HitStatus::accepted;
    SpectrumFeatures spectrum {};
};

struct SoundProfile
{
    SpectrumFeatures sum {};
    int count = 0;

    void clear() noexcept { sum.fill (0.0f); count = 0; }
    SpectrumFeatures mean() const noexcept
    {
        SpectrumFeatures m {};
        if (count > 0)
            for (int i = 0; i < kNumBands; ++i)
                m[(size_t) i] = sum[(size_t) i] / (float) count;
        return m;
    }
};

/** Receives everything the engine produces while processing a block. */
struct EngineListener
{
    virtual ~EngineListener() = default;
    virtual void midiEvent (int sampleOffset, uint8_t status, uint8_t data1, uint8_t data2) = 0;
    virtual void displayPoint (const DisplayPoint&) {}
    virtual void hitEvent (const HitEvent&) {}
};

enum class LearnMode { none, target, reject };

/**
    The complete drum trigger: transient detection, spectral sound matching, sidechain bleed
    rejection, velocity mapping, MIDI scheduling, trigger-keyed gate and compressor.

    The audio path is delayed by getLatencySamples() so that the analysis window after each
    transient is available before the hit has to be output. Everything (audio, gate, MIDI) is
    aligned to the original transient once the host compensates for the reported latency.
*/
class TriggerEngine
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void setParams (const EngineParams& p) noexcept { params = p; }
    const EngineParams& getParams() const noexcept { return params; }

    int getLatencySamples() const noexcept { return latency; }
    int getDisplayDecimation() const noexcept { return decimation; }
    double getSampleRate() const noexcept { return sampleRate; }

    /** in/out may alias. sidechain may be nullptr. */
    void process (const float* const* in, float* const* out, int numChannels,
                  const float* sidechain, int numSamples, EngineListener& listener);

    // Learning ---------------------------------------------------------------------------------
    void setLearnMode (LearnMode m) noexcept { learnMode = m; }
    LearnMode getLearnMode() const noexcept { return learnMode; }
    SoundProfile& getTargetProfile() noexcept { return target; }
    SoundProfile& getRejectProfile() noexcept { return rejectProfile; }
    float getLearnedPeakMinDb() const noexcept { return learnedMinDb; }
    float getLearnedPeakMaxDb() const noexcept { return learnedMaxDb; }
    void resetLearnedPeakRange() noexcept { learnedMinDb = 100.0f; learnedMaxDb = -100.0f; }
    void setLearnedPeakRange (float lo, float hi) noexcept { learnedMinDb = lo; learnedMaxDb = hi; }

    /** Sends a note right away (for testing the MIDI routing). */
    void triggerManual (int velocity) noexcept { pendingManualVelocity = velocity; }

    /** Releases any notes still pending (call when transport stops or settings change drastically). */
    void flushNotes (EngineListener& listener);

    // Helpers exposed for the UI and tests -----------------------------------------------------
    static int mapVelocity (float peakDb, const EngineParams& p) noexcept;
    /** 0..1 similarity of two spectral fingerprints (1 = identical). */
    static float similarity (const SpectrumFeatures& a, const SpectrumFeatures& b) noexcept;

private:
    struct Capture
    {
        int64_t onset = 0;
        int64_t decisionAt = 0;
        float peak = 0.0f;
        float scPeak = 0.0f;
        bool active = false;
    };

    struct MidiEvt
    {
        int64_t time = 0;
        uint8_t status = 0, data1 = 0, data2 = 0;
        bool used = false;
    };

    struct InputChunk
    {
        float inPeak = 0.0f, detPeak = 0.0f, thrDb = -100.0f;
    };

    void updateFilters() noexcept;
    float effectiveThresholdDb (int64_t pos) const noexcept;
    void decide (Capture& c, EngineListener& listener);
    void computeFeatures (int64_t onset, SpectrumFeatures& out) noexcept;
    void scheduleNote (int64_t time, int velocity) noexcept;
    void scheduleGate (int64_t time) noexcept;
    void emitMidi (int64_t blockStart, int numSamples, EngineListener& listener);
    void pushMidi (int64_t time, uint8_t status, uint8_t d1, uint8_t d2) noexcept;

    EngineParams params;
    double sampleRate = 48000.0;
    int numCh = 2;
    int scale = 1;
    int fftSize = 512;
    int preRoll = 32;
    int latency = 768;
    int decimation = 64;

    int64_t pos = 0; // absolute sample counter (input timeline == output timeline index)

    // Detection
    Filter24 hpf, lpf;
    float lastHpf = -1.0f, lastLpf = -1.0f;
    Filter24 scHpf, scLpf;
    float envFast = 0.0f, envSlow = 0.0f, scEnv = 0.0f;
    bool armed = true;
    int64_t lastOnset = -1000000000, lastAccepted = -1000000000;
    float dynBaseDb = -200.0f;
    int64_t dynStart = 0;
    std::array<Capture, 16> captures {};

    // Raw mono history for spectral analysis
    std::vector<float> history;
    int historyMask = 0;
    FFT fft;
    std::vector<float> fftIn, power, window;
    std::array<int, kNumBands + 1> bandLo {}, bandHi {};
    std::array<float, kNumBands> bandCentreBin {};

    // Learning
    LearnMode learnMode = LearnMode::none;
    SoundProfile target, rejectProfile;
    float learnedMinDb = 100.0f, learnedMaxDb = -100.0f;

    // Delay lines (audio + detection-listen signal)
    std::vector<std::vector<float>> delay;
    std::vector<float> detDelay;
    int delayMask = 0;

    // MIDI
    std::array<MidiEvt, 256> midiQueue {};
    int pendingManualVelocity = -1;

    // Gate
    std::array<int64_t, 64> gateQueue {};
    int gateHead = 0, gateCount = 0;
    float gateGain = 1.0f;
    int64_t gateHoldUntil = -1;

    // Compressor
    float compGrDb = 0.0f;

    // Display
    std::vector<InputChunk> inputChunks;
    InputChunk curIn;
    float curOutPeak = 0.0f, curGateSum = 0.0f, curGrMax = 0.0f;
};
} // namespace latrigger
