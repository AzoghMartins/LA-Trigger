// Integration test: drives the real LA-Trigger processor + editor the way a host would.
//  - checks bus layout, latency, MIDI output timing/velocity and state save/restore
//  - optionally renders the editor (with live data) to a PNG:  latrigger_integration [screenshot.png]
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../Source/PluginProcessor.h"
#include "../Source/gui/PluginEditor.h"

#include <random>

using namespace latrigger;

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.toRawUTF8());
    if (! ok)
        ++failures;
}

constexpr double sr = 48000.0;
std::mt19937 rng (99);

enum class Drum { kick, snare, hat };

void addDrum (juce::AudioBuffer<float>& buf, int start, Drum d, float gain)
{
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);
    double phase = 0.0;
    float hp = 0.0f, prev = 0.0f;
    for (int i = 0; i < (int) (0.3 * sr) && start + i < buf.getNumSamples(); ++i)
    {
        const double t = i / sr;
        float s = 0.0f;
        if (d == Drum::kick)
        {
            phase += 2.0 * juce::MathConstants<double>::pi * (50.0 + 90.0 * std::exp (-t * 30.0)) / sr;
            s = (float) (std::sin (phase) * std::exp (-t * 12.0)) + noise (rng) * 0.15f * (float) std::exp (-t * 300.0);
        }
        else if (d == Drum::snare)
        {
            phase += 2.0 * juce::MathConstants<double>::pi * 190.0 / sr;
            s = (float) (0.6 * std::sin (phase) * std::exp (-t * 25.0)) + noise (rng) * 0.7f * (float) std::exp (-t * 18.0);
        }
        else
        {
            const float n = noise (rng);
            hp = 0.6f * (hp + n - prev);
            prev = n;
            s = hp * 1.5f * (float) std::exp (-t * 60.0);
        }
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            buf.addSample (ch, start + i, s * gain);
    }
}

struct NoteOn { int64_t time; int note, channel, velocity; };

std::vector<NoteOn> runAudio (LATriggerProcessor& proc, juce::AudioBuffer<float>& audio, int block,
                              LATriggerEditor* editor = nullptr)
{
    std::vector<NoteOn> notes;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    for (int start = 0; start + block <= audio.getNumSamples(); start += block)
    {
        for (int ch = 0; ch < 2; ++ch)
            buf.copyFrom (ch, 0, audio, ch, start, block);
        midi.clear();
        proc.processBlock (buf, midi);
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOn())
                notes.push_back ({ start + meta.samplePosition, m.getNoteNumber(), m.getChannel(), m.getVelocity() });
        }
        if (editor != nullptr && (start / block) % 8 == 0)
            editor->updateDisplays();
    }
    return notes;
}

void setParam (LATriggerProcessor& p, const char* id, float value)
{
    auto* param = p.getState().getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("LA-Trigger plugin integration test\n");

    auto proc = std::make_unique<LATriggerProcessor>();
    check (proc->producesMidi() && proc->acceptsMidi(), "plugin declares MIDI input and output");
    check (proc->getBusCount (true) == 2, "main input + sidechain input bus");

    proc->setPlayConfigDetails (2, 2, sr, 512);
    proc->prepareToPlay (sr, 512);
    const int latency = proc->getLatencySamples();
    check (latency > 0 && latency < 2048, "reports latency for compensation: " + juce::String (latency) + " samples");

    // ---- MIDI note / channel / timing ---------------------------------------------------------
    setParam (*proc, ParamID::note, 38.0f);
    setParam (*proc, ParamID::channel, 3.0f);
    setParam (*proc, ParamID::threshold, -30.0f);
    setParam (*proc, ParamID::inHigh, 0.0f);

    juce::AudioBuffer<float> audio (2, (int) (sr * 3));
    audio.clear();
    std::vector<int> hits;
    const float gains[] = { 0.1f, 0.25f, 0.5f, 0.9f };
    for (int k = 0; k < 4; ++k)
    {
        hits.push_back ((int) (0.2 * sr) + k * (int) (0.6 * sr));
        addDrum (audio, hits.back(), Drum::snare, gains[k]);
    }
    auto notes = runAudio (*proc, audio, 480);
    check (notes.size() == hits.size(), "one note per hit (" + juce::String ((int) notes.size()) + ")");
    bool timingOk = notes.size() == hits.size(), routeOk = true, velOk = true;
    for (size_t i = 0; i < notes.size() && i < hits.size(); ++i)
    {
        timingOk &= std::abs ((int) (notes[i].time - (hits[i] + latency))) <= 48;
        routeOk &= notes[i].note == 38 && notes[i].channel == 3;
        if (i > 0) velOk &= notes[i].velocity > notes[i - 1].velocity;
    }
    check (timingOk, "notes aligned with transients after latency compensation (+/- 1 ms)");
    check (routeOk, "notes sent on chosen note (D1/38) and channel (3)");
    check (velOk, "velocity follows hit level");

    // ---- Velocity limits ---------------------------------------------------------------------------
    setParam (*proc, ParamID::velMin, 40.0f);
    setParam (*proc, ParamID::velMax, 90.0f);
    notes = runAudio (*proc, audio, 480);
    bool limitsOk = ! notes.empty();
    for (auto& n : notes)
        limitsOk &= n.velocity >= 40 && n.velocity <= 90;
    check (limitsOk, "velocity range limits respected (40..90)");

    // ---- State round trip -----------------------------------------------------------------------
    juce::MemoryBlock stateData;
    proc->getStateInformation (stateData);
    auto copyPtr = std::make_unique<LATriggerProcessor>(); // large object: keep it off the stack (1 MB on Windows)
    auto& copy = *copyPtr;
    copy.setStateInformation (stateData.getData(), (int) stateData.getSize());
    bool same = true;
    for (auto* p : proc->getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            same &= std::abs (rp->getValue() - copy.getState().getParameter (rp->paramID)->getValue()) < 1.0e-5f;
    check (same, "state save/restore round trip");

    // ---- Presets -------------------------------------------------------------------------------------
    proc->setCurrentProgram (1);
    check (proc->getProgramName (1) == "Kick In" && proc->getState().getRawParameterValue (ParamID::lpf)->load() < 200.0f,
           "factory preset applies (Kick In)");
    proc->setCurrentProgram (0);

    // ---- Learn + sound match, with the editor open --------------------------------------------------
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditorIfNeeded());
    auto* editor = dynamic_cast<LATriggerEditor*> (ed.get());
    check (editor != nullptr, "editor created");

    setParam (*proc, ParamID::threshold, -34.0f);
    setParam (*proc, ParamID::dynAmount, 25.0f);
    juce::AudioBuffer<float> learn (2, (int) (sr * 2.5));
    learn.clear();
    for (int k = 0; k < 5; ++k)
        addDrum (learn, (int) (0.1 * sr) + k * (int) (0.45 * sr), Drum::snare, 0.25f + 0.12f * (float) k);
    proc->setLearnMode (LearnMode::target);
    runAudio (*proc, learn, 480, editor);
    proc->setLearnMode (LearnMode::none);
    juce::AudioBuffer<float> silence (2, 4800);
    silence.clear();
    runAudio (*proc, silence, 480, editor);
    check (proc->getTargetSnapshot().count.load() == 5, "learned 5 target hits (" + juce::String (proc->getTargetSnapshot().count.load()) + ")");

    setParam (*proc, ParamID::matchEnable, 1.0f);
    setParam (*proc, ParamID::compEnable, 1.0f);
    juce::AudioBuffer<float> perf (2, (int) (sr * 3.4));
    perf.clear();
    int snareCount = 0;
    for (int k = 0; k < 16; ++k)
    {
        const int t = (int) (0.05 * sr) + k * (int) (0.19 * sr);
        if (k % 4 == 2) { addDrum (perf, t, Drum::snare, 0.35f + 0.05f * (float) (k % 3)); ++snareCount; }
        else if (k % 4 == 0) addDrum (perf, t, Drum::kick, 0.3f);
        else addDrum (perf, t, Drum::hat, 0.35f);
    }
    // a quiet ghost note
    addDrum (perf, (int) (3.08 * sr), Drum::snare, 0.08f);
    ++snareCount;
    notes = runAudio (*proc, perf, 480, editor);
    check ((int) notes.size() == snareCount, "sound match triggers only the learned drum: " + juce::String ((int) notes.size())
                                                + " notes for " + juce::String (snareCount) + " snares");

    if (argc > 1 && editor != nullptr)
    {
        editor->setSize (1280, 640);
        editor->updateDisplays();
        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
        out.deleteFile();
        juce::FileOutputStream stream (out);
        juce::PNGImageFormat png;
        check (stream.openedOk() && png.writeImageToStream (image, stream), "screenshot written to " + out.getFullPathName());
    }

    ed.reset();
    proc.reset();

    // ---- Mark a captured hit as bleed (frozen-display workflow) ------------------------------------
    {
        auto tomPtr = std::make_unique<LATriggerProcessor>();
        auto& tom = *tomPtr;
        tom.setPlayConfigDetails (2, 2, sr, 512);
        tom.prepareToPlay (sr, 512);
        setParam (tom, ParamID::threshold, -30.0f);

        // A "tom" (low pitched drum) with snare spill that is loud enough to cross the threshold
        juce::AudioBuffer<float> take (2, (int) (sr * 3.2));
        take.clear();
        int toms = 0, spills = 0;
        for (int k = 0; k < 12; ++k)
        {
            const int t = (int) (0.05 * sr) + k * (int) (0.25 * sr);
            if (k % 3 == 0) { addDrum (take, t, Drum::kick, 0.5f); ++toms; }
            else { addDrum (take, t, Drum::snare, 0.2f); ++spills; }
        }
        notes = runAudio (tom, take, 480);
        check ((int) notes.size() == toms + spills, "before marking, snare spill triggers too (" + juce::String ((int) notes.size()) + " notes)");

        // Mark the first triggered snare as bleed, as the UI does from the frozen display.
        HitEvent h, spill;
        bool found = false;
        while (tom.hitQueue.pop (h))
            if (! found && h.status == HitStatus::accepted && std::abs (h.samplePos - (int64_t) (0.30 * sr) - latency) < (int64_t) (0.02 * sr))
            {
                spill = h;
                found = true;
            }
        check (found, "found the snare spill hit to mark");
        tom.setHitMark (spill, HitMark::bleed);
        runAudio (tom, silence, 480);
        check (tom.getState().getRawParameterValue (ParamID::matchEnable)->load() > 0.5f, "marking bleed switches sound match on");
        check (tom.getRejectSnapshot().count.load() == 1, "marked hit added to the bleed profile");

        notes = runAudio (tom, take, 480);
        check ((int) notes.size() == toms, "after marking one spill hit, only the tom triggers: " + juce::String ((int) notes.size())
                                              + " notes for " + juce::String (toms) + " toms");

        // The mark survives a save / restore
        juce::MemoryBlock saved;
        tom.getStateInformation (saved);
        auto restoredPtr = std::make_unique<LATriggerProcessor>();
        auto& restored = *restoredPtr;
        restored.setStateInformation (saved.getData(), (int) saved.getSize());
        restored.setPlayConfigDetails (2, 2, sr, 512);
        restored.prepareToPlay (sr, 512);
        notes = runAudio (restored, take, 480);
        check ((int) notes.size() == toms, "bleed mark restored with the session (" + juce::String ((int) notes.size()) + " notes)");

        // Removing the mark brings the spill back
        spill.mark = HitMark::bleed;
        tom.setHitMark (spill, HitMark::none);
        runAudio (tom, silence, 480);
        notes = runAudio (tom, take, 480);
        check ((int) notes.size() == toms + spills && tom.getRejectSnapshot().count.load() == 0,
               "removing the mark undoes it (" + juce::String ((int) notes.size()) + " notes)");

        // Re-selecting the current preset after a tweak restores it
        tom.setCurrentProgram (1);
        setParam (tom, ParamID::lpf, 15000.0f);
        tom.setCurrentProgram (1);
        check (tom.getState().getRawParameterValue (ParamID::lpf)->load() < 200.0f, "re-applying the same preset resets tweaked values");
    }
    std::printf (failures == 0 ? "All integration checks passed\n" : "%d integration check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
