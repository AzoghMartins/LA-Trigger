#include "TriggerEngine.h"

#include <algorithm>
#include <cmath>

namespace latrigger
{
namespace
{
inline float gainToDb (float g) noexcept { return g > 1.0e-6f ? 20.0f * std::log10 (g) : -120.0f; }
inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }

inline float onePoleCoef (double ms, double sr) noexcept
{
    const double samples = std::max (1.0, ms * 0.001 * sr);
    return (float) (1.0 - std::exp (-1.0 / samples));
}

inline int nextPow2 (int v) noexcept
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}
} // namespace

//==============================================================================
void TriggerEngine::prepare (double sr, int /*maxBlockSize*/, int numChannels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    numCh = std::max (1, numChannels);

    scale = sampleRate <= 60000.0 ? 1 : (sampleRate <= 120000.0 ? 2 : 4);
    fftSize = 512 * scale;
    preRoll = 32 * scale;
    latency = fftSize + 256 * scale;
    decimation = 64 * scale;

    history.assign ((size_t) nextPow2 (fftSize * 4), 0.0f);
    historyMask = (int) history.size() - 1;

    fft.prepare (fftSize);
    fftIn.assign ((size_t) fftSize, 0.0f);
    power.assign ((size_t) fftSize / 2 + 1, 0.0f);

    // Analysis window: short raised-cosine fade-in over the pre-roll, flat body, cosine fade-out
    // over the last quarter. Keeps the transient itself at full weight.
    window.assign ((size_t) fftSize, 1.0f);
    const int rise = preRoll * 2;
    const int fall = fftSize / 4;
    for (int i = 0; i < rise; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos ((float) M_PI * (float) i / (float) rise);
    for (int i = 0; i < fall; ++i)
        window[(size_t) (fftSize - 1 - i)] = 0.5f - 0.5f * std::cos ((float) M_PI * (float) i / (float) fall);

    // Log-spaced analysis bands between 40 Hz and 16 kHz (or 0.45 * fs)
    const double fMin = 40.0, fMax = std::min (16000.0, sampleRate * 0.45);
    const double binHz = sampleRate / fftSize;
    for (int b = 0; b < kNumBands; ++b)
    {
        const double lo = fMin * std::pow (fMax / fMin, (double) b / kNumBands);
        const double hi = fMin * std::pow (fMax / fMin, (double) (b + 1) / kNumBands);
        bandLo[(size_t) b] = (int) std::ceil (lo / binHz);
        bandHi[(size_t) b] = (int) std::ceil (hi / binHz) - 1;
        bandCentreBin[(size_t) b] = (float) (std::sqrt (lo * hi) / binHz);
    }

    const int delaySize = nextPow2 (latency + 1);
    delay.assign ((size_t) numCh, std::vector<float> ((size_t) delaySize, 0.0f));
    detDelay.assign ((size_t) delaySize, 0.0f);
    delayMask = delaySize - 1;

    inputChunks.assign (256, InputChunk {});

    lastHpf = lastLpf = -1.0f;
    reset();
}

void TriggerEngine::reset()
{
    pos = 0;
    hpf.reset(); lpf.reset(); scHpf.reset(); scLpf.reset();
    envFast = envSlow = scEnv = 0.0f;
    armed = true;
    lastOnset = lastAccepted = -1000000000;
    dynBaseDb = -200.0f;
    dynStart = 0;
    for (auto& c : captures)
        c.active = false;

    std::fill (history.begin(), history.end(), 0.0f);
    for (auto& d : delay)
        std::fill (d.begin(), d.end(), 0.0f);
    std::fill (detDelay.begin(), detDelay.end(), 0.0f);

    for (auto& e : midiQueue)
        e.used = false;
    gateHead = gateCount = 0;
    gateGain = params.gateEnabled ? 0.0f : 1.0f;
    gateHoldUntil = -1;
    compGrDb = 0.0f;

    std::fill (inputChunks.begin(), inputChunks.end(), InputChunk {});
    curIn = {};
    curOutPeak = curGateSum = curGrMax = 0.0f;
}

void TriggerEngine::updateFilters() noexcept
{
    if (params.hpfHz != lastHpf)
    {
        lastHpf = params.hpfHz;
        if (params.hpfHz <= 20.5f) { hpf.setBypass(); scHpf.setBypass(); }
        else { hpf.setHighPass (sampleRate, params.hpfHz); scHpf.setHighPass (sampleRate, params.hpfHz); }
    }
    if (params.lpfHz != lastLpf)
    {
        lastLpf = params.lpfHz;
        if (params.lpfHz >= 19999.0f) { lpf.setBypass(); scLpf.setBypass(); }
        else { lpf.setLowPass (sampleRate, params.lpfHz); scLpf.setLowPass (sampleRate, params.lpfHz); }
    }
}

float TriggerEngine::effectiveThresholdDb (int64_t p) const noexcept
{
    if (params.dynAmount <= 0.0f || dynBaseDb < -150.0f)
        return params.thresholdDb;

    const double relSamples = std::max (1.0, params.dynReleaseMs * 0.001 * sampleRate);
    const float dyn = dynBaseDb - 60.0f * (float) ((double) (p - dynStart) / relSamples);
    return std::max (params.thresholdDb, dyn);
}

//==============================================================================
int TriggerEngine::mapVelocity (float peakDb, const EngineParams& p) noexcept
{
    const float range = p.inHighDb - p.inLowDb;
    float x = range > 0.1f ? (peakDb - p.inLowDb) / range : (peakDb >= p.inHighDb ? 1.0f : 0.0f);
    x = std::clamp (x, 0.0f, 1.0f);
    const float exponent = std::pow (2.0f, -p.velCurve / 50.0f);
    const float y = std::pow (x, exponent);
    const int lo = std::clamp (std::min (p.velMin, p.velMax), 1, 127);
    const int hi = std::clamp (std::max (p.velMin, p.velMax), 1, 127);
    return std::clamp ((int) std::lround ((float) lo + y * (float) (hi - lo)), 1, 127);
}

float TriggerEngine::similarity (const SpectrumFeatures& a, const SpectrumFeatures& b) noexcept
{
    // Both vectors are level-normalised log spectra (dB, mean removed). The score falls off with
    // the RMS spectral distance: ~2-3 dB for the same drum, 10+ dB for a different one.
    double sum = 0.0;
    for (int i = 0; i < kNumBands; ++i)
    {
        const double d = (double) a[(size_t) i] - (double) b[(size_t) i];
        sum += d * d;
    }
    const double rmsDb = std::sqrt (sum / kNumBands);
    return (float) std::exp (-(rmsDb / 10.0) * (rmsDb / 10.0));
}

void TriggerEngine::computeFeatures (int64_t onset, SpectrumFeatures& out) noexcept
{
    const int64_t start = onset - preRoll;
    for (int i = 0; i < fftSize; ++i)
        fftIn[(size_t) i] = history[(size_t) ((start + i) & historyMask)] * window[(size_t) i];

    fft.powerSpectrum (fftIn.data(), power.data());

    const int maxBin = fftSize / 2;
    float maxDb = -200.0f;
    for (int b = 0; b < kNumBands; ++b)
    {
        const int lo = std::max (1, bandLo[(size_t) b]);
        const int hi = std::min (maxBin, bandHi[(size_t) b]);
        double e = 0.0;
        if (hi >= lo)
        {
            for (int k = lo; k <= hi; ++k)
                e += power[(size_t) k];
            e /= (double) (hi - lo + 1);
        }
        else
        {
            const float c = std::clamp (bandCentreBin[(size_t) b], 0.0f, (float) maxBin - 1.0f);
            const int k0 = (int) c;
            const float frac = c - (float) k0;
            e = power[(size_t) k0] * (1.0f - frac) + power[(size_t) k0 + 1] * frac;
        }
        const float db = 10.0f * std::log10 ((float) e + 1.0e-12f);
        out[(size_t) b] = db;
        maxDb = std::max (maxDb, db);
    }

    // Shape only: clamp to a 60 dB range below the strongest band and remove the mean.
    float mean = 0.0f;
    for (auto& v : out)
    {
        v = std::max (v, maxDb - 60.0f);
        mean += v;
    }
    mean /= (float) kNumBands;
    for (auto& v : out)
        v -= mean;
}

//==============================================================================
void TriggerEngine::pushMidi (int64_t time, uint8_t status, uint8_t d1, uint8_t d2) noexcept
{
    for (auto& e : midiQueue)
        if (! e.used)
        {
            e = { time, status, d1, d2, true };
            return;
        }
}

void TriggerEngine::scheduleNote (int64_t time, int velocity) noexcept
{
    const auto ch = (uint8_t) (std::clamp (params.channel, 1, 16) - 1);
    const auto note = (uint8_t) std::clamp (params.note, 0, 127);

    // If the same note is still sounding, end it right before the new one.
    for (auto& e : midiQueue)
        if (e.used && e.status == (0x80 | ch) && e.data1 == note && e.time > time)
            e.time = time;

    const auto len = (int64_t) std::max (1.0, params.noteLengthMs * 0.001 * sampleRate);
    pushMidi (time, (uint8_t) (0x90 | ch), note, (uint8_t) std::clamp (velocity, 1, 127));
    pushMidi (time + len, (uint8_t) (0x80 | ch), note, 0);
}

void TriggerEngine::scheduleGate (int64_t time) noexcept
{
    if (gateCount >= (int) gateQueue.size())
        return;
    gateQueue[(size_t) ((gateHead + gateCount) % (int) gateQueue.size())] = time;
    ++gateCount;
}

void TriggerEngine::emitMidi (int64_t blockStart, int numSamples, EngineListener& listener)
{
    const int64_t blockEnd = blockStart + numSamples;
    std::array<int, 256> due {};
    int numDue = 0;

    for (int i = 0; i < (int) midiQueue.size(); ++i)
        if (midiQueue[(size_t) i].used && midiQueue[(size_t) i].time < blockEnd)
            due[(size_t) numDue++] = i;

    std::sort (due.begin(), due.begin() + numDue, [this] (int a, int b)
    {
        const auto& ea = midiQueue[(size_t) a];
        const auto& eb = midiQueue[(size_t) b];
        if (ea.time != eb.time)
            return ea.time < eb.time;
        return (ea.status & 0xf0) < (eb.status & 0xf0); // note-off (0x80) before note-on (0x90)
    });

    for (int k = 0; k < numDue; ++k)
    {
        auto& e = midiQueue[(size_t) due[(size_t) k]];
        const int offset = (int) std::clamp<int64_t> (e.time - blockStart, 0, numSamples - 1);
        listener.midiEvent (offset, e.status, e.data1, e.data2);
        e.used = false;
    }
}

void TriggerEngine::flushNotes (EngineListener& listener)
{
    for (auto& e : midiQueue)
    {
        if (e.used && (e.status & 0xf0) == 0x80)
            listener.midiEvent (0, e.status, e.data1, e.data2);
        e.used = false;
    }
}

//==============================================================================
void TriggerEngine::decide (Capture& c, EngineListener& listener)
{
    c.active = false;

    HitEvent hit;
    hit.samplePos = c.onset + latency;
    hit.peakDb = gainToDb (c.peak);
    computeFeatures (c.onset, hit.spectrum);

    float simTarget = -1.0f, simReject = -1.0f;
    if (target.count > 0)
    {
        simTarget = similarity (hit.spectrum, target.mean());
        hit.matchScore = std::max (0.0f, simTarget) * 100.0f;
    }
    if (rejectProfile.count > 0)
    {
        simReject = similarity (hit.spectrum, rejectProfile.mean());
        hit.rejectScore = std::max (0.0f, simReject) * 100.0f;
    }

    bool trigger = true;

    if (learnMode == LearnMode::target)
    {
        for (int i = 0; i < kNumBands; ++i)
            target.sum[(size_t) i] += hit.spectrum[(size_t) i];
        ++target.count;
        learnedMinDb = std::min (learnedMinDb, hit.peakDb);
        learnedMaxDb = std::max (learnedMaxDb, hit.peakDb);
        hit.status = HitStatus::learnedTarget;
    }
    else if (learnMode == LearnMode::reject)
    {
        for (int i = 0; i < kNumBands; ++i)
            rejectProfile.sum[(size_t) i] += hit.spectrum[(size_t) i];
        ++rejectProfile.count;
        hit.status = HitStatus::learnedReject;
        trigger = false;
    }
    else
    {
        if (params.matchEnabled && target.count > 0)
        {
            const bool scoreOk = hit.matchScore >= params.matchThreshold;
            const bool closerToTarget = rejectProfile.count == 0 || simTarget >= simReject;
            if (! (scoreOk && closerToTarget))
            {
                trigger = false;
                hit.status = HitStatus::rejectedMatch;
            }
        }

        if (trigger && params.scEnabled && c.scPeak > 0.0f)
        {
            if (gainToDb (c.scPeak) - hit.peakDb > params.scMarginDb)
            {
                trigger = false;
                hit.status = HitStatus::rejectedBleed;
            }
        }
    }

    if (trigger)
    {
        const auto retrig = (int64_t) (params.retriggerMs * 0.001 * sampleRate);
        if (c.onset - lastAccepted < retrig)
            return; // part of a hit we already triggered: drop silently

        lastAccepted = c.onset;
        hit.velocity = mapVelocity (hit.peakDb, params);

        const auto offset = (int64_t) std::llround (params.midiOffsetMs * 0.001 * sampleRate);
        scheduleNote (std::max (pos, c.onset + latency + offset), hit.velocity);
        scheduleGate (c.onset + latency);

        if (params.dynAmount > 0.0f)
        {
            dynBaseDb = hit.peakDb - (30.0f - 27.0f * params.dynAmount / 100.0f);
            dynStart = c.onset;
        }
    }

    listener.hitEvent (hit);
}

//==============================================================================
void TriggerEngine::process (const float* const* in, float* const* out, int numChannels,
                             const float* sidechain, int numSamples, EngineListener& listener)
{
    updateFilters();
    const int64_t blockStart = pos;
    const int chans = std::min (numChannels, numCh);

    if (pendingManualVelocity > 0)
    {
        scheduleNote (pos, pendingManualVelocity);
        HitEvent hit;
        hit.samplePos = pos;
        hit.velocity = pendingManualVelocity;
        hit.status = HitStatus::manual;
        listener.hitEvent (hit);
        pendingManualVelocity = -1;
    }

    // Per-block constants
    const float fastRel = onePoleCoef (10.0, sampleRate);
    const float slowAtk = onePoleCoef (5.0, sampleRate);
    const float slowRel = onePoleCoef (50.0, sampleRate);
    const float riseReq = 20.0f - 19.0f * std::clamp (params.sensitivity, 0.0f, 100.0f) / 100.0f;
    const auto retrig = (int64_t) (params.retriggerMs * 0.001 * sampleRate);
    const auto minGap = (int64_t) (0.002 * sampleRate);
    const int64_t analysisLen = fftSize - preRoll;

    const auto attackSamples = (int64_t) std::max (1.0, params.gateAttackMs * 0.001 * sampleRate);
    const auto holdSamples = (int64_t) (params.gateHoldMs * 0.001 * sampleRate);
    const float gateRel = (float) std::exp (-6.9 / std::max (1.0, params.gateReleaseMs * 0.001 * sampleRate));
    const float gateFloor = params.gateRangeDb <= -79.9f ? 0.0f : dbToGain (params.gateRangeDb);

    const float compAtk = onePoleCoef (params.compAttackMs, sampleRate);
    const float compRel = onePoleCoef (params.compReleaseMs, sampleRate);
    const float compSlope = 1.0f - 1.0f / std::max (1.0f, params.compRatio);
    const float knee = std::max (0.0f, params.compKneeDb);
    const float makeup = dbToGain (params.compMakeupDb);
    const float outGain = dbToGain (params.outputGainDb);
    const float mix = std::clamp (params.mix, 0.0f, 100.0f) / 100.0f;
    const int chunksBehind = latency / decimation;

    std::array<float, 16> procSample {};

    for (int i = 0; i < numSamples; ++i)
    {
        // ---- Analysis (input timeline) -------------------------------------------------------
        float mono = 0.0f, inPeak = 0.0f;
        for (int ch = 0; ch < chans; ++ch)
        {
            const float x = in[ch][i];
            mono += x;
            inPeak = std::max (inPeak, std::abs (x));
        }
        mono /= (float) std::max (1, chans);
        history[(size_t) (pos & historyMask)] = mono;

        const float det = lpf.process (hpf.process (mono));
        const float a = std::abs (det);
        envFast = a > envFast ? a : envFast + (a - envFast) * fastRel;
        envSlow += (a - envSlow) * (a > envSlow ? slowAtk : slowRel);

        float scA = 0.0f;
        if (sidechain != nullptr)
        {
            scA = std::abs (scLpf.process (scHpf.process (sidechain[i])));
            scEnv = scA > scEnv ? scA : scEnv + (scA - scEnv) * fastRel;
        }

        const float thr = effectiveThresholdDb (pos);
        const float fastDb = gainToDb (envFast);
        const float rise = fastDb - gainToDb (envSlow);

        if (! armed && (fastDb < thr - 3.0f || rise < riseReq * 0.5f))
            armed = true;

        if (armed && fastDb > thr && rise >= riseReq
            && pos - lastOnset >= minGap && pos - lastAccepted >= retrig)
        {
            for (auto& c : captures)
                if (! c.active)
                {
                    c = { pos, pos + analysisLen, 0.0f, 0.0f, true };
                    break;
                }
            armed = false;
            lastOnset = pos;
        }

        // Captures are started in order; process their decisions oldest first.
        for (auto& c : captures)
            if (c.active)
            {
                c.peak = std::max (c.peak, a);
                c.scPeak = std::max (c.scPeak, scEnv);
            }

        for (;;)
        {
            Capture* oldest = nullptr;
            for (auto& c : captures)
                if (c.active && pos + 1 >= c.decisionAt && (oldest == nullptr || c.onset < oldest->onset))
                    oldest = &c;
            if (oldest == nullptr)
                break;
            decide (*oldest, listener);
        }

        // ---- Output (delayed by `latency`) ---------------------------------------------------
        const int wr = (int) (pos & delayMask);
        const int rd = (int) ((pos - latency) & delayMask);
        detDelay[(size_t) wr] = det;
        const float detOut = detDelay[(size_t) rd];

        // Gate driven by accepted triggers
        bool attacking = false;
        while (gateCount > 0)
        {
            const int64_t q = gateQueue[(size_t) gateHead];
            if (pos >= q)
            {
                gateHead = (gateHead + 1) % (int) gateQueue.size();
                --gateCount;
                gateHoldUntil = q + holdSamples;
                gateGain = 1.0f;
                attacking = true;
                continue;
            }
            if (pos >= q - attackSamples)
            {
                gateGain += (1.0f - gateGain) / (float) std::max<int64_t> (1, q - pos);
                attacking = true;
            }
            break;
        }
        if (! attacking)
        {
            if (pos < gateHoldUntil)
                gateGain = 1.0f;
            else
                gateGain = gateFloor + (gateGain - gateFloor) * gateRel;
        }
        const float g = params.gateEnabled ? gateGain : 1.0f;

        float procPeak = 0.0f;
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& d = delay[(size_t) ch];
            d[(size_t) wr] = in[ch][i];
            procSample[(size_t) ch] = d[(size_t) rd] * g;
            procPeak = std::max (procPeak, std::abs (procSample[(size_t) ch]));
        }

        float compGain = 1.0f;
        if (params.compEnabled)
        {
            const float over = gainToDb (procPeak) - params.compThresholdDb;
            float grTarget = 0.0f;
            if (2.0f * over <= -knee)
                grTarget = 0.0f;
            else if (knee > 0.0f && 2.0f * std::abs (over) <= knee)
                grTarget = compSlope * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
            else
                grTarget = compSlope * over;

            compGrDb += (grTarget - compGrDb) * (grTarget > compGrDb ? compAtk : compRel);
            compGain = dbToGain (-compGrDb) * makeup;
        }
        else
        {
            compGrDb = 0.0f;
        }

        float outPeak = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float y = 0.0f;
            if (ch < chans)
            {
                switch (params.outputMode)
                {
                    case 1:  y = detOut; break;
                    case 2:  y = 0.0f; break;
                    default:
                    {
                        const float dry = delay[(size_t) ch][(size_t) rd];
                        y = dry + (procSample[(size_t) ch] * compGain - dry) * mix;
                        break;
                    }
                }
                y *= outGain;
            }
            out[ch][i] = y;
            outPeak = std::max (outPeak, std::abs (y));
        }

        // ---- Display data -------------------------------------------------------------------
        curIn.inPeak = std::max (curIn.inPeak, inPeak);
        curIn.detPeak = std::max (curIn.detPeak, envFast);
        curOutPeak = std::max (curOutPeak, outPeak);
        curGateSum += g;
        curGrMax = std::max (curGrMax, compGrDb);

        if ((pos + 1) % decimation == 0)
        {
            const int64_t chunk = pos / decimation;
            curIn.thrDb = thr;
            inputChunks[(size_t) (chunk & 255)] = curIn;
            curIn = {};

            DisplayPoint p;
            p.index = chunk;
            const int64_t srcChunk = chunk - chunksBehind;
            if (srcChunk >= 0)
            {
                const auto& src = inputChunks[(size_t) (srcChunk & 255)];
                p.inPeak = src.inPeak;
                p.detPeak = src.detPeak;
                p.thrDb = src.thrDb;
            }
            p.outPeak = curOutPeak;
            p.gate = curGateSum / (float) decimation;
            p.grDb = curGrMax;
            listener.displayPoint (p);
            curOutPeak = curGateSum = curGrMax = 0.0f;
        }

        ++pos;
    }

    // Channels the engine wasn't prepared for are silenced.
    for (int ch = chans; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            out[ch][i] = 0.0f;

    emitMidi (blockStart, numSamples, listener);
}
} // namespace latrigger
