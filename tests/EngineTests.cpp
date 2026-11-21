// Headless tests for the LA-Trigger DSP engine using synthesized drum sounds.
#include "../Source/dsp/TriggerEngine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace latrigger;

namespace
{
int failures = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (! (cond)) {                                     \
            ++failures;                                     \
            std::printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf (__VA_ARGS__);                      \
            std::printf ("\n");                             \
        }                                                   \
    } while (0)

constexpr double sr = 48000.0;
std::mt19937 rng (1234);

enum class Drum { kick, snare, hat };

void addDrum (std::vector<float>& buf, int start, Drum d, float gain)
{
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);
    const int len = (int) (0.25 * sr);
    float hpState = 0.0f, prev = 0.0f;
    double phase = 0.0;
    for (int i = 0; i < len && start + i < (int) buf.size(); ++i)
    {
        const double t = i / sr;
        float s = 0.0f;
        switch (d)
        {
            case Drum::kick:
            {
                const double f = 50.0 + 90.0 * std::exp (-t * 30.0);
                phase += 2.0 * M_PI * f / sr;
                s = (float) (std::sin (phase) * std::exp (-t * 12.0));
                s += noise (rng) * 0.15f * (float) std::exp (-t * 300.0);
                break;
            }
            case Drum::snare:
            {
                phase += 2.0 * M_PI * 190.0 / sr;
                s = (float) (0.6 * std::sin (phase) * std::exp (-t * 25.0));
                s += noise (rng) * 0.7f * (float) std::exp (-t * 18.0);
                break;
            }
            case Drum::hat:
            {
                const float n = noise (rng);
                hpState = 0.6f * (hpState + n - prev); // crude high-pass
                prev = n;
                s = hpState * 1.5f * (float) std::exp (-t * 60.0);
                break;
            }
        }
        buf[(size_t) (start + i)] += s * gain;
    }
}

struct Collector : EngineListener
{
    struct Note { int64_t time; int vel; };
    std::vector<Note> notes;
    std::vector<HitEvent> hits;
    int64_t blockStart = 0;
    int noteOffs = 0;

    void midiEvent (int offset, uint8_t status, uint8_t, uint8_t d2) override
    {
        if ((status & 0xf0) == 0x90)
            notes.push_back ({ blockStart + offset, d2 });
        else
            ++noteOffs;
    }
    void hitEvent (const HitEvent& h) override { hits.push_back (h); }
};

void run (TriggerEngine& e, std::vector<float>& audio, Collector& c, int block = 256)
{
    std::vector<float> outBuf ((size_t) block);
    for (int startIdx = 0; startIdx < (int) audio.size(); startIdx += block)
    {
        const int n = std::min (block, (int) audio.size() - startIdx);
        const float* in[1] = { audio.data() + startIdx };
        float* out[1] = { outBuf.data() };
        c.blockStart = startIdx;
        e.process (in, out, 1, nullptr, n, c);
    }
}

bool nearAny (const std::vector<Collector::Note>& notes, int64_t t, int tol)
{
    for (auto& n : notes)
        if (std::llabs (n.time - t) <= tol)
            return true;
    return false;
}

void testBasicDetectionAndTiming()
{
    TriggerEngine e;
    e.prepare (sr, 256, 1);
    EngineParams p;
    p.thresholdDb = -30.0f;
    e.setParams (p);

    std::vector<float> audio ((size_t) (sr * 4), 0.0f);
    std::vector<int> onsets;
    for (int k = 0; k < 8; ++k)
    {
        const int t = (int) (0.1 * sr) + k * (int) (0.45 * sr);
        onsets.push_back (t);
        addDrum (audio, t, Drum::kick, 0.8f);
    }

    Collector c;
    run (e, audio, c);
    CHECK ((int) c.notes.size() == (int) onsets.size(), "expected %d kicks, got %d", (int) onsets.size(), (int) c.notes.size());
    for (int t : onsets)
        CHECK (nearAny (c.notes, t + e.getLatencySamples(), 48), "kick at %d not aligned (latency %d)", t, e.getLatencySamples());
    CHECK (c.noteOffs == (int) c.notes.size(), "note offs %d != note ons %d", c.noteOffs, (int) c.notes.size());
}

void testVelocityFollowsLevel()
{
    TriggerEngine e;
    e.prepare (sr, 256, 1);
    EngineParams p;
    p.thresholdDb = -45.0f;
    p.inLowDb = -40.0f;
    p.inHighDb = 0.0f;
    e.setParams (p);

    std::vector<float> audio ((size_t) (sr * 3), 0.0f);
    const float gains[] = { 0.05f, 0.1f, 0.2f, 0.4f, 0.8f };
    for (int k = 0; k < 5; ++k)
        addDrum (audio, (int) (0.1 * sr) + k * (int) (0.5 * sr), Drum::snare, gains[k]);

    Collector c;
    run (e, audio, c);
    CHECK (c.notes.size() == 5, "expected 5 snares, got %d", (int) c.notes.size());
    for (size_t k = 1; k < c.notes.size(); ++k)
        CHECK (c.notes[k].vel > c.notes[k - 1].vel, "velocity not increasing: %d then %d", c.notes[k - 1].vel, c.notes[k].vel);

    EngineParams v;
    v.inLowDb = -40.0f; v.inHighDb = 0.0f; v.velMin = 20; v.velMax = 100;
    CHECK (TriggerEngine::mapVelocity (-60.0f, v) == 20, "velocity lower limit");
    CHECK (TriggerEngine::mapVelocity (6.0f, v) == 100, "velocity upper limit");
}

void testRetriggerSuppressesFlams()
{
    TriggerEngine e;
    e.prepare (sr, 256, 1);
    EngineParams p;
    p.thresholdDb = -30.0f;
    p.retriggerMs = 50.0f;
    e.setParams (p);

    std::vector<float> audio ((size_t) (sr * 1), 0.0f);
    addDrum (audio, (int) (0.1 * sr), Drum::snare, 0.8f);
    addDrum (audio, (int) (0.1 * sr) + (int) (0.015 * sr), Drum::snare, 0.8f); // flam 15 ms later
    Collector c;
    run (e, audio, c);
    CHECK (c.notes.size() == 1, "retrigger should merge flam, got %d notes", (int) c.notes.size());
}

void testSoundMatchRejectsBleed()
{
    TriggerEngine e;
    e.prepare (sr, 256, 1);
    EngineParams p;
    p.thresholdDb = -40.0f;
    e.setParams (p);

    // 1) learn snare
    std::vector<float> learn ((size_t) (sr * 3), 0.0f);
    for (int k = 0; k < 5; ++k)
        addDrum (learn, (int) (0.1 * sr) + k * (int) (0.5 * sr), Drum::snare, 0.3f + 0.1f * k);
    e.setLearnMode (LearnMode::target);
    Collector lc;
    run (e, learn, lc);
    e.setLearnMode (LearnMode::none);
    CHECK (e.getTargetProfile().count == 5, "learned %d snare hits, expected 5", e.getTargetProfile().count);

    // 2) learn hats and kicks as rejection
    std::vector<float> learnRej ((size_t) (sr * 3), 0.0f);
    for (int k = 0; k < 5; ++k)
        addDrum (learnRej, (int) (0.1 * sr) + k * (int) (0.5 * sr), k % 2 ? Drum::hat : Drum::kick, 0.5f);
    e.setLearnMode (LearnMode::reject);
    Collector rc;
    run (e, learnRej, rc);
    e.setLearnMode (LearnMode::none);
    CHECK (rc.notes.empty(), "learning reject sounds must not trigger");

    // 3) mixed performance: snares (loud) with hats and kicks (bleed, quieter)
    p.matchEnabled = true;
    p.matchThreshold = 50.0f;
    e.setParams (p);
    std::vector<float> mix ((size_t) (sr * 6), 0.0f);
    std::vector<int> snares;
    for (int k = 0; k < 24; ++k)
    {
        const int t = (int) (0.1 * sr) + k * (int) (0.22 * sr);
        if (k % 4 == 2) { addDrum (mix, t, Drum::snare, 0.5f); snares.push_back (t); }
        else if (k % 4 == 0) addDrum (mix, t, Drum::kick, 0.3f);
        else addDrum (mix, t, Drum::hat, 0.3f);
    }
    Collector c;
    run (e, mix, c);
    int matched = 0;
    for (int t : snares)
        matched += nearAny (c.notes, t + e.getLatencySamples(), 96) ? 1 : 0;
    CHECK (matched == (int) snares.size(), "matched %d of %d snares", matched, (int) snares.size());
    CHECK ((int) c.notes.size() == (int) snares.size(), "bleed leaked through: %d notes for %d snares",
           (int) c.notes.size(), (int) snares.size());

    int rejected = 0;
    for (auto& h : c.hits)
        rejected += h.status == HitStatus::rejectedMatch ? 1 : 0;
    std::printf ("  sound match: %d snares triggered, %d bleed hits rejected\n", matched, rejected);
}

void testSoundMatchTargetOnly()
{
    // Only the target is learned (no bleed profile): kick and hat must still be rejected.
    TriggerEngine e;
    e.prepare (sr, 256, 1);
    EngineParams p;
    p.thresholdDb = -40.0f;
    e.setParams (p);
    std::vector<float> learn ((size_t) (sr * 3), 0.0f);
    for (int k = 0; k < 5; ++k)
        addDrum (learn, (int) (0.1 * sr) + k * (int) (0.5 * sr), Drum::snare, 0.3f + 0.1f * k);
    e.setLearnMode (LearnMode::target);
    Collector lc;
    run (e, learn, lc);
    e.setLearnMode (LearnMode::none);

    p.matchEnabled = true;
    e.setParams (p);
    std::vector<float> mix ((size_t) (sr * 3), 0.0f);
    const Drum seq[] = { Drum::kick, Drum::hat, Drum::snare, Drum::hat, Drum::kick, Drum::snare };
    for (int k = 0; k < 6; ++k)
        addDrum (mix, (int) (0.1 * sr) + k * (int) (0.3 * sr), seq[k], 0.4f);
    Collector c;
    run (e, mix, c);
    CHECK (c.notes.size() == 2, "target-only match: expected 2 snares, got %d notes", (int) c.notes.size());
}

void testGateOpensOnHits()
{
    TriggerEngine e;
    e.prepare (sr, 256, 1);
    EngineParams p;
    p.thresholdDb = -30.0f;
    p.gateEnabled = true;
    p.gateHoldMs = 50.0f;
    p.gateReleaseMs = 50.0f;
    p.gateRangeDb = -80.0f;
    e.setParams (p);

    // Constant low noise plus one snare: noise must be gated before the hit, pass during it.
    std::vector<float> audio ((size_t) sr, 0.0f);
    std::uniform_real_distribution<float> noise (-0.01f, 0.01f);
    for (auto& s : audio)
        s = noise (rng);
    const int hitAt = (int) (0.5 * sr);
    addDrum (audio, hitAt, Drum::snare, 0.8f);

    std::vector<float> out (audio.size());
    Collector c;
    for (int s = 0; s < (int) audio.size(); s += 256)
    {
        const int n = std::min (256, (int) audio.size() - s);
        const float* in[1] = { audio.data() + s };
        float* o[1] = { out.data() + s };
        e.process (in, o, 1, nullptr, n, c);
    }
    const int L = e.getLatencySamples();
    double before = 0.0, during = 0.0;
    for (int i = hitAt - 4000; i < hitAt - 1000; ++i) before = std::max (before, (double) std::abs (out[(size_t) (i + L)]));
    for (int i = hitAt; i < hitAt + 1000; ++i) during = std::max (during, (double) std::abs (out[(size_t) (i + L)]));
    CHECK (before < 1.0e-4, "gate should be closed before the hit (peak %g)", before);
    CHECK (during > 0.3, "gate should be open during the hit (peak %g)", during);
    // Transient preserved: first samples after the onset should already be at full gain.
    CHECK (std::abs (out[(size_t) (hitAt + L + 2)]) > 0.5 * std::abs (audio[(size_t) hitAt + 2]),
           "gate attack must not cut the transient");
}

void testSidechainBleedRejection()
{
    TriggerEngine e;
    e.prepare (sr, 256, 1);
    EngineParams p;
    p.thresholdDb = -40.0f;
    p.scEnabled = true;
    p.scMarginDb = 0.0f;
    e.setParams (p);

    // Target mic: loud snare + quiet tom bleed. Sidechain (tom mic): loud tom.
    std::vector<float> target ((size_t) sr * 2, 0.0f), sc ((size_t) sr * 2, 0.0f);
    addDrum (target, (int) (0.2 * sr), Drum::snare, 0.8f);
    addDrum (target, (int) (1.0 * sr), Drum::kick, 0.15f);
    addDrum (sc, (int) (1.0 * sr), Drum::kick, 0.8f);
    addDrum (sc, (int) (0.2 * sr), Drum::snare, 0.1f);

    Collector c;
    std::vector<float> out (256);
    for (int s = 0; s < (int) target.size(); s += 256)
    {
        const float* in[1] = { target.data() + s };
        float* o[1] = { out.data() };
        c.blockStart = s;
        e.process (in, o, 1, sc.data() + s, 256, c);
    }
    CHECK (c.notes.size() == 1, "sidechain should reject tom bleed: got %d notes", (int) c.notes.size());
}

void testHighSampleRate()
{
    TriggerEngine e;
    e.prepare (96000.0, 512, 1);
    EngineParams p;
    e.setParams (p);
    std::vector<float> audio (96000, 0.0f);
    // simple decaying burst
    for (int i = 0; i < 9600; ++i)
        audio[(size_t) (20000 + i)] = (float) (0.8 * std::sin (i * 0.02) * std::exp (-i / 2000.0));
    Collector c;
    std::vector<float> out (512);
    for (int s = 0; s + 512 <= (int) audio.size(); s += 512)
    {
        const float* in[1] = { audio.data() + s };
        float* o[1] = { out.data() };
        c.blockStart = s;
        e.process (in, o, 1, nullptr, 512, c);
    }
    CHECK (c.notes.size() == 1, "96k: expected 1 note, got %d", (int) c.notes.size());
    CHECK (e.getLatencySamples() == 1536, "96k latency %d", e.getLatencySamples());
}
} // namespace

int main()
{
    std::printf ("LA-Trigger engine tests\n");
    testBasicDetectionAndTiming();
    testVelocityFollowsLevel();
    testRetriggerSuppressesFlams();
    testSoundMatchRejectsBleed();
    testSoundMatchTargetOnly();
    testGateOpensOnHits();
    testSidechainBleedRejection();
    testHighSampleRate();
    if (failures == 0)
        std::printf ("All tests passed\n");
    else
        std::printf ("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
