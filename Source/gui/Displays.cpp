#include "Displays.h"
#include "../PluginProcessor.h"

namespace latrigger
{
namespace
{
float toDb (float g) { return juce::Decibels::gainToDecibels (g, -100.0f); }

juce::String statusText (const HitEvent& h)
{
    switch (h.status)
    {
        case HitStatus::accepted:          return "TRIGGERED";
        case HitStatus::learnedTarget:     return "LEARNED AS TARGET";
        case HitStatus::learnedReject:     return "LEARNED AS BLEED";
        case HitStatus::rejectedMatch:     return "REJECTED: SOUND MATCH";
        case HitStatus::rejectedBleed:     return "REJECTED: SIDECHAIN BLEED";
        case HitStatus::rejectedRetrigger: return "REJECTED: RETRIGGER";
        case HitStatus::manual:            return "TEST NOTE";
    }
    return {};
}

bool isTrigger (const HitEvent& h)
{
    return h.status == HitStatus::accepted || h.status == HitStatus::learnedTarget || h.status == HitStatus::manual;
}
} // namespace

//==============================================================================
WaveformDisplay::WaveformDisplay (LATriggerProcessor& p) : processor (p), ring ((size_t) ringSize)
{
    auto& s = processor.getState();
    thresholdParam = s.getParameter (ParamID::threshold);
    thresholdValue = s.getRawParameterValue (ParamID::threshold);
    gateEnabled = s.getRawParameterValue (ParamID::gateEnable);
    setOpaque (true);
    updateTooltip();
}

void WaveformDisplay::setFrozen (bool f)
{
    frozen = f;
    if (! frozen)
        hoverHit = -1;
    updateTooltip();
    repaint();
}

void WaveformDisplay::clearMarks (HitMark kind)
{
    for (auto& h : hits)
        if (h.mark == kind)
            h.mark = HitMark::none;
    repaint();
}

void WaveformDisplay::updateTooltip()
{
    setTooltip (frozen ? "Click a hit marker to mark it as bleed (never trigger on sounds like it) or as a real hit"
                       : "Drag the orange threshold line to set the trigger threshold. FREEZE the display to mark hits as bleed");
}

float WaveformDisplay::sampleToX (double samplePos) const
{
    const auto area = graphArea();
    const double sr = processor.getEngineSampleRate();
    const int decim = processor.getDisplayDecimation();
    const double latestSample = (double) (latest + 1) * decim;
    const double visibleSamples = spanSeconds * sr;
    return area.getRight() - (float) ((latestSample - samplePos) / visibleSamples) * area.getWidth();
}

HitEvent* WaveformDisplay::hitAt (juce::Point<float> p)
{
    if (! frozen || ! graphArea().expanded (0.0f, 4.0f).contains (p))
        return nullptr;

    HitEvent* best = nullptr;
    float bestDist = 7.0f;
    for (auto& h : hits)
    {
        if (h.status == HitStatus::manual)
            continue;
        const float d = std::abs (sampleToX ((double) h.samplePos) - p.x);
        if (d < bestDist)
        {
            bestDist = d;
            best = &h;
        }
    }
    return best;
}

void WaveformDisplay::showHitMenu (int64_t samplePos)
{
    auto it = std::find_if (hits.begin(), hits.end(), [samplePos] (const HitEvent& h) { return h.samplePos == samplePos; });
    if (it == hits.end())
        return;
    const auto& h = *it;

    juce::String info = juce::String (h.peakDb, 1) + " dB";
    if (h.matchScore >= 0.0f)
        info << "   match " << juce::roundToInt (h.matchScore) << "%";
    if (h.rejectScore >= 0.0f)
        info << "   bleed " << juce::roundToInt (h.rejectScore) << "%";

    juce::PopupMenu m;
    m.addSectionHeader (statusText (h));
    m.addItem (-1, info, false);
    m.addSeparator();
    m.addItem (1, "Mark as bleed  (reject sounds like this)", true, h.mark == HitMark::bleed);
    m.addItem (2, "Mark as real hit  (trigger on sounds like this)", true, h.mark == HitMark::hit);
    if (h.mark != HitMark::none)
        m.addItem (3, "Remove mark");

    juce::Component::SafePointer<WaveformDisplay> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                         .withTargetScreenArea ({ juce::Desktop::getMousePosition(), juce::Desktop::getMousePosition() }),
                     [safe, samplePos] (int r)
    {
        if (safe == nullptr || r <= 0)
            return;
        auto& self = *safe;
        auto hit = std::find_if (self.hits.begin(), self.hits.end(), [samplePos] (const HitEvent& e) { return e.samplePos == samplePos; });
        if (hit == self.hits.end())
            return;
        const auto mark = r == 1 ? HitMark::bleed : (r == 2 ? HitMark::hit : HitMark::none);
        self.processor.setHitMark (*hit, mark);
        hit->mark = mark;
        if (self.onHitMarked)
            self.onHitMarked (*hit);
        self.repaint();
    });
}

void WaveformDisplay::mouseExit (const juce::MouseEvent&)
{
    if (hoverHit >= 0)
    {
        hoverHit = -1;
        repaint();
    }
}

void WaveformDisplay::addPoint (const DisplayPoint& p)
{
    if (frozen)
        return;
    if (p.index < latest) // engine was reset (new playback / sample rate)
    {
        std::fill (ring.begin(), ring.end(), DisplayPoint {});
        hits.clear();
    }
    ring[(size_t) (p.index & (ringSize - 1))] = p;
    latest = p.index;
}

void WaveformDisplay::addHit (const HitEvent& h)
{
    if (frozen)
        return;
    hits.push_back (h);
    while (hits.size() > 600)
        hits.pop_front();
}

juce::Rectangle<float> WaveformDisplay::graphArea() const
{
    auto r = getLocalBounds().toFloat();
    r.removeFromLeft (34.0f);
    r.removeFromRight (4.0f);
    r.removeFromTop (22.0f); // legend strip
    r.removeFromBottom (18.0f + 18.0f);
    return r;
}

juce::Rectangle<float> WaveformDisplay::gateLane() const
{
    auto g = graphArea();
    return { g.getX(), g.getBottom() + 4.0f, g.getWidth(), 14.0f };
}

float WaveformDisplay::dbToY (float db, juce::Rectangle<float> a) const
{
    const float clamped = juce::jlimit (minDb, maxDb, db);
    return juce::jmap (clamped, maxDb, minDb, a.getY(), a.getBottom());
}

float WaveformDisplay::yToDb (float y, juce::Rectangle<float> a) const
{
    return juce::jmap (juce::jlimit (a.getY(), a.getBottom(), y), a.getY(), a.getBottom(), maxDb, minDb);
}

bool WaveformDisplay::nearThreshold (juce::Point<float> p) const
{
    const auto a = graphArea();
    return a.expanded (0.0f, 6.0f).contains (p) && std::abs (p.y - dbToY (thresholdValue->load(), a)) < 7.0f;
}

void WaveformDisplay::mouseMove (const juce::MouseEvent& e)
{
    const bool near = nearThreshold (e.position);
    const auto* hit = near ? nullptr : hitAt (e.position);
    const int64_t hover = hit != nullptr ? hit->samplePos : -1;
    if (near != hoverThreshold || hover != hoverHit)
    {
        hoverThreshold = near;
        hoverHit = hover;
        setMouseCursor (near ? juce::MouseCursor::UpDownResizeCursor
                             : (hover >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));
        repaint();
    }
}

void WaveformDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (nearThreshold (e.position) && thresholdParam != nullptr && ! e.mods.isPopupMenu())
    {
        draggingThreshold = true;
        thresholdParam->beginChangeGesture();
        return;
    }
    if (const auto* hit = hitAt (e.position))
        showHitMenu (hit->samplePos);
}

void WaveformDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingThreshold)
    {
        const float db = yToDb (e.position.y, graphArea());
        thresholdParam->setValueNotifyingHost (thresholdParam->convertTo0to1 (db));
        repaint();
    }
}

void WaveformDisplay::mouseUp (const juce::MouseEvent&)
{
    if (draggingThreshold)
    {
        thresholdParam->endChangeGesture();
        draggingThreshold = false;
    }
}

void WaveformDisplay::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    spanSeconds = juce::jlimit (0.5, 10.0, spanSeconds * (w.deltaY > 0 ? 0.85 : 1.0 / 0.85));
    if (onSpanChanged)
        onSpanChanged (spanSeconds);
    repaint();
}

void WaveformDisplay::paint (juce::Graphics& g)
{
    Hardware::drawScreen (g, getLocalBounds().toFloat());
    const auto area = graphArea();
    const auto lane = gateLane();
    const double sr = processor.getEngineSampleRate();
    const int decim = processor.getDisplayDecimation();
    const float thr = thresholdValue->load();

    // ---- Grid -----------------------------------------------------------------------------
    g.setFont (uiFont (10.0f));
    for (float db = 0.0f; db >= minDb; db -= 6.0f)
    {
        const float y = dbToY (db, area);
        g.setColour (Colours::grid.withAlpha (std::fmod (-db, 12.0f) < 0.1f ? 1.0f : 0.5f));
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        if (std::fmod (-db, 12.0f) < 0.1f)
        {
            g.setColour (Colours::textDim);
            g.drawText (juce::String ((int) db), 0, (int) y - 7, 30, 14, juce::Justification::centredRight);
        }
    }

    const int width = juce::jmax (1, (int) area.getWidth());
    const double visiblePoints = spanSeconds * sr / decim;
    const double latestSample = (double) (latest + 1) * decim;
    const double visibleSamples = visiblePoints * decim;
    auto xForSample = [&] (double samplePos)
    {
        return area.getRight() - (float) ((latestSample - samplePos) / visibleSamples) * area.getWidth();
    };

    // Time grid
    const double step = spanSeconds > 5.0 ? 1.0 : (spanSeconds > 1.5 ? 0.5 : 0.25);
    const double firstT = std::ceil ((latestSample - visibleSamples) / sr / step) * step;
    for (double t = firstT; t * sr <= latestSample; t += step)
    {
        const float x = xForSample (t * sr);
        g.setColour (Colours::grid.withAlpha (0.6f));
        g.drawVerticalLine ((int) x, area.getY(), lane.getBottom());
        g.setColour (Colours::textDim.withAlpha (0.7f));
        g.drawText (juce::String (t, 2) + "s", (int) x - 25, (int) lane.getBottom() + 2, 50, 14, juce::Justification::centred);
    }

    // ---- Reduce points to pixel columns ---------------------------------------------------------
    std::vector<float> inCol ((size_t) width, 0.0f), outCol ((size_t) width, 0.0f), detCol ((size_t) width, 0.0f),
        thrCol ((size_t) width, -100.0f), gateCol ((size_t) width, 1.0f), grCol ((size_t) width, 0.0f);
    std::vector<bool> valid ((size_t) width, false);

    if (latest >= 0)
    {
        const auto first = (int64_t) std::floor ((double) latest - visiblePoints + 1.0);
        const double pointsPerPx = visiblePoints / width;
        for (int64_t i = juce::jmax<int64_t> (0, first); i <= latest; ++i)
        {
            const auto& p = ring[(size_t) (i & (ringSize - 1))];
            if (p.index != i)
                continue;
            const int c = juce::jlimit (0, width - 1, (int) ((double) (i - first) / pointsPerPx));
            const auto cs = (size_t) c;
            if (! valid[cs])
            {
                gateCol[cs] = p.gate;
                valid[cs] = true;
            }
            inCol[cs] = juce::jmax (inCol[cs], p.inPeak);
            outCol[cs] = juce::jmax (outCol[cs], p.outPeak);
            detCol[cs] = juce::jmax (detCol[cs], p.detPeak);
            thrCol[cs] = p.thrDb;
            gateCol[cs] = juce::jmin (gateCol[cs], p.gate);
            grCol[cs] = juce::jmax (grCol[cs], p.grDb);
        }
    }

    auto filledPath = [&] (const std::vector<float>& col)
    {
        juce::Path path;
        path.startNewSubPath (area.getX(), area.getBottom());
        for (int c = 0; c < width; ++c)
            path.lineTo (area.getX() + (float) c, valid[(size_t) c] ? dbToY (toDb (col[(size_t) c]), area) : area.getBottom());
        path.lineTo (area.getRight(), area.getBottom());
        path.closeSubPath();
        return path;
    };

    auto linePath = [&] (const std::vector<float>& col, bool alreadyDb)
    {
        juce::Path path;
        bool started = false;
        for (int c = 0; c < width; ++c)
        {
            if (! valid[(size_t) c])
            {
                started = false;
                continue;
            }
            const float v = alreadyDb ? col[(size_t) c] : toDb (col[(size_t) c]);
            const float y = dbToY (v, area);
            if (! started)
                path.startNewSubPath (area.getX() + (float) c, y);
            else
                path.lineTo (area.getX() + (float) c, y);
            started = true;
        }
        return path;
    };

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area.toNearestInt());

        // Input (raw)
        g.setColour (Colours::input.withAlpha (0.45f));
        g.fillPath (filledPath (inCol));

        // Output (after gate / compressor)
        const auto outPath = filledPath (outCol);
        g.setGradientFill (juce::ColourGradient (Colours::accent.withAlpha (0.55f), 0.0f, area.getY(),
                                                 Colours::accent.withAlpha (0.12f), 0.0f, area.getBottom(), false));
        g.fillPath (outPath);

        // Detection envelope
        g.setColour (Colours::detection.withAlpha (0.85f));
        g.strokePath (linePath (detCol, false), juce::PathStrokeType (1.2f));

        // Effective (dynamic) threshold
        g.setColour (Colours::threshold.withAlpha (0.8f));
        g.strokePath (linePath (thrCol, true), juce::PathStrokeType (1.0f));

        // Compressor gain reduction hanging from the top
        juce::Path grPath;
        grPath.startNewSubPath (area.getX(), area.getY());
        for (int c = 0; c < width; ++c)
            grPath.lineTo (area.getX() + (float) c, area.getY() + juce::jlimit (0.0f, 1.0f, grCol[(size_t) c] / 24.0f) * area.getHeight() * 0.3f);
        grPath.lineTo (area.getRight(), area.getY());
        grPath.closeSubPath();
        g.setColour (Colours::danger.withAlpha (0.35f));
        g.fillPath (grPath);
    }

    // ---- Static threshold line with handle ------------------------------------------------------
    {
        const float y = dbToY (thr, area);
        juce::Path dashed;
        const float dashes[] = { 6.0f, 4.0f };
        juce::PathStrokeType (1.5f).createDashedStroke (dashed, [&] { juce::Path l; l.startNewSubPath (area.getX(), y); l.lineTo (area.getRight(), y); return l; }(),
                                                        dashes, 2);
        g.setColour (Colours::threshold.withAlpha (hoverThreshold || draggingThreshold ? 1.0f : 0.85f));
        g.fillPath (dashed);

        const auto label = juce::Rectangle<float> (area.getX() + 4.0f, y - 18.0f, 92.0f, 15.0f);
        g.setColour (Colours::threshold.withAlpha (0.9f));
        g.fillRoundedRectangle (label, 3.0f);
        g.setColour (Colours::background);
        g.setFont (uiFont (10.5f, true));
        g.drawText ("THR " + juce::String (thr, 1) + " dB", label, juce::Justification::centred);
    }

    // ---- Gate lane ----------------------------------------------------------------------------
    g.setColour (Colours::panel);
    g.fillRect (lane);
    if (gateEnabled->load() > 0.5f)
    {
        juce::Path gatePath;
        gatePath.startNewSubPath (lane.getX(), lane.getBottom());
        for (int c = 0; c < width; ++c)
            gatePath.lineTo (lane.getX() + (float) c, lane.getBottom() - (valid[(size_t) c] ? gateCol[(size_t) c] : 0.0f) * lane.getHeight());
        gatePath.lineTo (lane.getRight(), lane.getBottom());
        gatePath.closeSubPath();
        g.setColour (Colours::accent.withAlpha (0.5f));
        g.fillPath (gatePath);
    }
    g.setColour (Colours::textDim);
    g.setFont (uiFont (9.5f, true));
    g.drawText (gateEnabled->load() > 0.5f ? "GATE" : "GATE OFF", 0, (int) lane.getY(), 31, (int) lane.getHeight(),
                juce::Justification::centredRight);

    // ---- Hits -------------------------------------------------------------------------------------
    while (! hits.empty() && (double) hits.front().samplePos < latestSample - visibleSamples - sr)
        hits.pop_front();

    for (const auto& h : hits)
    {
        const float x = xForSample ((double) h.samplePos);
        if (x < area.getX() - 20.0f || x > area.getRight() + 20.0f)
            continue;

        if (isTrigger (h))
        {
            const auto c = h.status == HitStatus::learnedTarget ? Colours::learn : Colours::forVelocity (h.velocity);
            g.setColour (c.withAlpha (0.35f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            // velocity stem
            const float top = area.getBottom() - (float) h.velocity / 127.0f * area.getHeight();
            g.setColour (c.withAlpha (0.9f));
            g.fillRect (x - 1.0f, top, 2.0f, area.getBottom() - top);
            const auto bubble = juce::Rectangle<float> (26.0f, 15.0f).withCentre ({ x, juce::jmax (area.getY() + 8.0f, top - 9.0f) });
            g.fillRoundedRectangle (bubble, 3.0f);
            g.setColour (Colours::background);
            g.setFont (uiFont (10.5f, true));
            g.drawText (h.status == HitStatus::manual ? "T" : juce::String (h.velocity), bubble, juce::Justification::centred);
        }
        else
        {
            const auto c = h.status == HitStatus::learnedReject ? Colours::learn : Colours::danger;
            g.setColour (c.withAlpha (0.5f));
            const float dashes[] = { 3.0f, 3.0f };
            juce::Path dashed;
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, [&] { juce::Path l; l.startNewSubPath (x, area.getY() + 14.0f); l.lineTo (x, area.getBottom()); return l; }(), dashes, 2);
            g.fillPath (dashed);
            g.setColour (c);
            const float s = 4.0f, cy = area.getY() + 8.0f;
            g.drawLine (x - s, cy - s, x + s, cy + s, 1.8f);
            g.drawLine (x - s, cy + s, x + s, cy - s, 1.8f);
            if (h.matchScore >= 0.0f && h.status == HitStatus::rejectedMatch)
            {
                g.setFont (uiFont (9.5f));
                g.drawText (juce::String (juce::roundToInt (h.matchScore)) + "%", (int) x + 5, (int) cy - 6, 34, 12,
                            juce::Justification::centredLeft);
            }
        }

        // User marks (frozen view) and the hover highlight
        if (h.mark != HitMark::none)
        {
            const bool bleed = h.mark == HitMark::bleed;
            const auto c = bleed ? Colours::danger : Colours::learn;
            const auto tag = juce::Rectangle<float> (bleed ? 40.0f : 30.0f, 14.0f).withCentre ({ x, area.getBottom() - 10.0f });
            g.setColour (c);
            g.fillRoundedRectangle (tag, 3.0f);
            g.setColour (Colours::background);
            g.setFont (uiFont (9.5f, true));
            g.drawText (bleed ? "BLEED" : "HIT", tag, juce::Justification::centred);
        }
        if (frozen && h.samplePos == hoverHit)
        {
            g.setColour (Colours::text.withAlpha (0.12f));
            g.fillRect (juce::Rectangle<float> (x - 6.0f, area.getY(), 12.0f, area.getHeight()));
            g.setColour (Colours::text.withAlpha (0.6f));
            g.drawRect (juce::Rectangle<float> (x - 6.0f, area.getY(), 12.0f, area.getHeight()), 1.0f);
        }
    }

    // ---- Legend ---------------------------------------------------------------------------------
    {
        auto legend = juce::Rectangle<float> (area.getRight() - 300.0f, 4.0f, 296.0f, 14.0f);
        auto item = [&] (juce::Colour c, const juce::String& text, float w)
        {
            auto r = legend.removeFromLeft (w);
            g.setColour (c);
            g.fillRoundedRectangle (r.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 4.0f), 2.0f);
            g.setColour (Colours::textDim);
            g.setFont (uiFont (10.0f, true));
            g.drawText (text, r.withTrimmedLeft (4.0f), juce::Justification::centredLeft);
        };
        item (Colours::input, "INPUT", 62.0f);
        item (Colours::accent, "OUTPUT", 68.0f);
        item (Colours::detection, "DETECT", 66.0f);
        item (Colours::threshold, "THRESH", 66.0f);
        item (Colours::danger, "GR", 34.0f);
    }

    if (frozen)
    {
        g.setColour (Colours::learn);
        g.setFont (uiFont (11.0f, true));
        g.drawText ("FROZEN  -  click a hit to mark it",
                    juce::Rectangle<float> (area.getX() + 4.0f, 4.0f, 240.0f, 14.0f), juce::Justification::centredLeft);
    }

    g.setColour (Colours::edge);
    g.drawRect (getLocalBounds(), 1);
}

//==============================================================================
void MatchDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    Hardware::drawScreen (g, r, 4.0f);

    auto area = r.reduced (6.0f, 6.0f);
    area.removeFromBottom (12.0f);
    const auto& target = processor.getTargetSnapshot();
    const auto& reject = processor.getRejectSnapshot();
    const int tCount = target.count.load();
    const int rCount = reject.count.load();

    auto yFor = [&] (float v) { return juce::jmap (juce::jlimit (-30.0f, 30.0f, v), 30.0f, -30.0f, area.getY(), area.getBottom()); };
    const float bw = area.getWidth() / (float) kNumBands;

    // Frequency labels
    g.setFont (uiFont (9.0f));
    g.setColour (Colours::textDim);
    const double fMax = juce::jmin (16000.0, processor.getEngineSampleRate() * 0.45);
    for (auto [f, text] : { std::pair<double, const char*> { 100.0, "100" }, { 1000.0, "1k" }, { 5000.0, "5k" }, { 300.0, "300" } })
    {
        const float b = (float) (kNumBands * std::log (f / 40.0) / std::log (fMax / 40.0));
        const float x = area.getX() + b * bw;
        g.drawText (text, (int) x - 12, (int) area.getBottom() + 1, 24, 11, juce::Justification::centred);
        g.setColour (Colours::grid);
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        g.setColour (Colours::textDim);
    }

    if (tCount > 0)
    {
        const auto m = target.get();
        for (int b = 0; b < kNumBands; ++b)
        {
            const float y = yFor (m[(size_t) b]);
            const auto bar = juce::Rectangle<float> (area.getX() + b * bw + 1.0f, y, bw - 2.0f, area.getBottom() - y);
            g.setColour (Colours::accent.withAlpha (0.45f));
            g.fillRect (bar);
            g.setColour (Colours::accent);
            g.fillRect (bar.withHeight (2.0f));
        }
    }

    auto polyline = [&] (const SpectrumFeatures& f, juce::Colour c, float thickness, bool dots)
    {
        juce::Path p;
        for (int b = 0; b < kNumBands; ++b)
        {
            const float x = area.getX() + (b + 0.5f) * bw;
            const float y = yFor (f[(size_t) b]);
            if (b == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            if (dots)
            {
                g.setColour (c);
                g.fillEllipse (x - 2.0f, y - 2.0f, 4.0f, 4.0f);
            }
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (thickness));
    };

    if (rCount > 0)
        polyline (reject.get(), Colours::danger.withAlpha (0.9f), 1.5f, false);

    if (hasHit)
        polyline (lastHit.spectrum, Colours::text.withAlpha (0.9f), 1.2f, true);

    if (tCount == 0 && rCount == 0)
    {
        g.setColour (Colours::textDim);
        g.setFont (uiFont (11.5f));
        g.drawFittedText ("Press LEARN HIT and play a few hits of the drum you want to trigger.\n"
                          "Optionally LEARN BLEED on hits of other drums to reject.",
                          area.reduced (6.0f).toNearestInt(), juce::Justification::centred, 4);
    }
}

//==============================================================================
VelocityCurveDisplay::VelocityCurveDisplay (juce::AudioProcessorValueTreeState& s) : state (s) {}

void VelocityCurveDisplay::tick()
{
    if (hitAge < 1000)
        ++hitAge;
    repaint();
}

void VelocityCurveDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    Hardware::drawScreen (g, r, 4.0f);
    const auto a = r.reduced (6.0f);

    EngineParams p;
    p.inLowDb = state.getRawParameterValue (ParamID::inLow)->load();
    p.inHighDb = state.getRawParameterValue (ParamID::inHigh)->load();
    p.velMin = (int) state.getRawParameterValue (ParamID::velMin)->load();
    p.velMax = (int) state.getRawParameterValue (ParamID::velMax)->load();
    p.velCurve = state.getRawParameterValue (ParamID::velCurve)->load();
    const float thr = state.getRawParameterValue (ParamID::threshold)->load();

    auto xFor = [&] (float db) { return juce::jmap (juce::jlimit (-60.0f, 0.0f, db), -60.0f, 0.0f, a.getX(), a.getRight()); };
    auto yFor = [&] (float vel) { return juce::jmap (vel, 0.0f, 127.0f, a.getBottom(), a.getY()); };

    // Grid
    g.setColour (Colours::grid);
    for (int v : { 32, 64, 96 })
        g.drawHorizontalLine ((int) yFor ((float) v), a.getX(), a.getRight());
    for (float db : { -48.0f, -36.0f, -24.0f, -12.0f })
        g.drawVerticalLine ((int) xFor (db), a.getY(), a.getBottom());

    // Active input window
    const float x0 = xFor (juce::jmin (p.inLowDb, p.inHighDb)), x1 = xFor (juce::jmax (p.inLowDb, p.inHighDb));
    g.setColour (Colours::accent.withAlpha (0.07f));
    g.fillRect (juce::Rectangle<float> (x0, a.getY(), juce::jmax (1.0f, x1 - x0), a.getHeight()));

    // Threshold (nothing below can trigger)
    g.setColour (Colours::threshold.withAlpha (0.18f));
    g.fillRect (juce::Rectangle<float> (a.getX(), a.getY(), xFor (thr) - a.getX(), a.getHeight()));
    g.setColour (Colours::threshold.withAlpha (0.7f));
    g.drawVerticalLine ((int) xFor (thr), a.getY(), a.getBottom());

    // Curve
    juce::Path curve;
    const float exponent = std::pow (2.0f, -p.velCurve / 50.0f);
    const float lo = (float) juce::jmin (p.velMin, p.velMax), hi = (float) juce::jmax (p.velMin, p.velMax);
    for (int i = 0; i <= 120; ++i)
    {
        const float db = -60.0f + 0.5f * (float) i;
        const float range = p.inHighDb - p.inLowDb;
        float x = range > 0.1f ? (db - p.inLowDb) / range : (db >= p.inHighDb ? 1.0f : 0.0f);
        x = juce::jlimit (0.0f, 1.0f, x);
        const float vel = lo + std::pow (x, exponent) * (hi - lo);
        if (i == 0) curve.startNewSubPath (xFor (db), yFor (vel)); else curve.lineTo (xFor (db), yFor (vel));
    }
    g.setColour (Colours::accent);
    g.strokePath (curve, juce::PathStrokeType (2.0f));

    // Last hit
    if (hitAge < 120 && lastVel > 0)
    {
        const float alpha = 1.0f - (float) hitAge / 120.0f;
        const auto c = Colours::forVelocity (lastVel).withAlpha (alpha);
        const auto pt = juce::Point<float> (xFor (lastDb), yFor ((float) lastVel));
        g.setColour (c.withMultipliedAlpha (0.35f));
        g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (pt));
        g.setColour (c);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (pt));
    }

    g.setColour (Colours::textDim);
    g.setFont (uiFont (9.0f));
    g.drawText ("IN dB", a.withTrimmedTop (a.getHeight() - 11.0f), juce::Justification::centredRight);
    g.drawText ("VEL", a.withHeight (11.0f), juce::Justification::centredLeft);
}

//==============================================================================
void HitInfoPanel::setHit (const HitEvent& h)
{
    if (isTrigger (h))
    {
        if (h.status != HitStatus::manual)
            ++accepted;
        last = h;
        hasHit = true;
        flash = 1.0f;
    }
    else
    {
        if (h.status == HitStatus::rejectedMatch || h.status == HitStatus::rejectedBleed)
            ++rejected;
        last = h;
        hasHit = true;
        flash = 0.6f;
    }
    repaint();
}

void HitInfoPanel::tick()
{
    if (flash > 0.0f)
    {
        flash = juce::jmax (0.0f, flash - 0.06f);
        repaint();
    }
}

void HitInfoPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    Hardware::drawScreen (g, r, 4.0f);

    const bool trig = hasHit && isTrigger (last);
    const auto colour = ! hasHit ? Colours::textDim
                                 : (trig ? (last.status == HitStatus::learnedTarget ? Colours::learn : Colours::forVelocity (last.velocity))
                                         : (last.status == HitStatus::learnedReject ? Colours::learn : Colours::danger));

    if (flash > 0.0f)
    {
        g.setColour (colour.withAlpha (0.18f * flash));
        g.fillRoundedRectangle (r, 4.0f);
    }

    auto inner = r.reduced (10.0f, 6.0f);
    auto left = inner.removeFromLeft (84.0f);

    g.setColour (Colours::textDim);
    g.setFont (uiFont (10.0f, true));
    g.drawText ("VELOCITY", left.removeFromTop (14.0f), juce::Justification::centredLeft);
    g.setColour (trig ? colour : Colours::textDim);
    g.setFont (uiFont (40.0f, true));
    g.drawText (trig ? juce::String (last.velocity) : juce::String ("--"), left.removeFromTop (44.0f), juce::Justification::centredLeft);

    // Velocity bar
    auto bar = left.removeFromTop (6.0f).withTrimmedRight (8.0f);
    g.setColour (Colours::panelLight);
    g.fillRoundedRectangle (bar, 3.0f);
    if (trig)
    {
        g.setColour (colour);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) last.velocity / 127.0f), 3.0f);
    }

    const int note = (int) state.getRawParameterValue (ParamID::note)->load();
    const int ch = (int) state.getRawParameterValue (ParamID::channel)->load();

    g.setColour (Colours::text);
    g.setFont (uiFont (14.0f, true));
    g.drawText (midiNoteName (note), inner.removeFromTop (18.0f), juce::Justification::centredLeft);
    g.setColour (Colours::textDim);
    g.setFont (uiFont (11.0f));
    g.drawText ("MIDI channel " + juce::String (ch), inner.removeFromTop (15.0f), juce::Justification::centredLeft);

    g.setColour (colour);
    g.setFont (uiFont (10.5f, true));
    juce::String status = hasHit ? statusText (last) : juce::String ("WAITING FOR HITS");
    if (hasHit && last.status != HitStatus::manual)
    {
        status << "  " << juce::String (last.peakDb, 1) << " dB";
        if (last.matchScore >= 0.0f)
            status << "  match " << juce::roundToInt (last.matchScore) << "%";
    }
    g.drawFittedText (status, inner.removeFromTop (26.0f).toNearestInt(), juce::Justification::topLeft, 2);

    g.setColour (Colours::textDim);
    g.setFont (uiFont (10.5f));
    g.drawText ("Triggered " + juce::String (accepted) + "   Rejected " + juce::String (rejected),
                inner.removeFromTop (14.0f), juce::Justification::centredLeft);
}
} // namespace latrigger
