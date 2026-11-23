#include "PluginEditor.h"

namespace latrigger
{
MainPanel::MainPanel (LATriggerProcessor& p)
    : processor (p),
      state (p.getState()),
      waveform (p),
      matchSection ("Sound Match", &state, ParamID::matchEnable, Colours::learn),
      hitSection ("Last Hit"),
      matchDisplay (p),
      hitInfo (state),
      detectionSection ("Detection"),
      velocitySection ("Velocity"),
      midiSection ("MIDI Out"),
      gateSection ("Gate", &state, ParamID::gateEnable),
      compSection ("Compressor", &state, ParamID::compEnable, Colours::danger),
      outputSection ("Output"),
      threshold (state, ParamID::threshold, "Threshold", Colours::threshold),
      sensitivity (state, ParamID::sensitivity, "Sensitivity"),
      retrigger (state, ParamID::retrigger, "Retrigger"),
      hpf (state, ParamID::hpf, "HPF"),
      lpf (state, ParamID::lpf, "LPF"),
      dynAmount (state, ParamID::dynAmount, "Dynamic", Colours::threshold),
      dynRelease (state, ParamID::dynRelease, "Dyn Rel", Colours::threshold),
      scMargin (state, ParamID::scMargin, "SC Margin", Colours::threshold, true),
      velocityCurve (state),
      inLow (state, ParamID::inLow, "In Low"),
      inHigh (state, ParamID::inHigh, "In High"),
      velCurve (state, ParamID::velCurve, "Curve", Colours::accent, true),
      velMin (state, ParamID::velMin, "Vel Min"),
      velMax (state, ParamID::velMax, "Vel Max"),
      note (state, ParamID::note, "Note"),
      channel (state, ParamID::channel, "Channel"),
      noteLength (state, ParamID::noteLength, "Length"),
      midiOffset (state, ParamID::midiOffset, "Offset", Colours::accent, true),
      gateAttack (state, ParamID::gateAttack, "Attack"),
      gateHold (state, ParamID::gateHold, "Hold"),
      gateRelease (state, ParamID::gateRelease, "Release"),
      gateRange (state, ParamID::gateRange, "Range"),
      compThreshold (state, ParamID::compThreshold, "Threshold", Colours::danger),
      compRatio (state, ParamID::compRatio, "Ratio", Colours::danger),
      compKnee (state, ParamID::compKnee, "Knee", Colours::danger),
      compAttack (state, ParamID::compAttack, "Attack", Colours::danger),
      compRelease (state, ParamID::compRelease, "Release", Colours::danger),
      compMakeup (state, ParamID::compMakeup, "Makeup", Colours::danger),
      mix (state, ParamID::mix, "Mix"),
      outputGain (state, ParamID::outputGain, "Gain", Colours::accent, true)
{
    // ---- Header ----------------------------------------------------------------------------
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        presetBox.addItem (processor.getProgramName (i), i + 1);
    presetBox.setTextWhenNothingSelected ("Factory presets");
    presetBox.setSelectedId (processor.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this]
    {
        if (presetBox.getSelectedId() > 0)
            processor.setCurrentProgram (presetBox.getSelectedId() - 1);
    };
    presetBox.setTooltip ("Factory starting points for common drum sources");
    addAndMakeVisible (presetBox);

    const double spans[] = { 1.0, 2.0, 3.0, 5.0, 8.0 };
    for (int i = 0; i < 5; ++i)
        spanBox.addItem ("View " + juce::String ((int) spans[i]) + " s", i + 1);
    spanBox.setSelectedId (3, juce::dontSendNotification);
    spanBox.onChange = [this, spans]
    {
        if (spanBox.getSelectedId() > 0)
            waveform.setTimeSpan (spans[spanBox.getSelectedId() - 1]);
    };
    spanBox.setTooltip ("Visible time span (mouse wheel over the display to zoom)");
    addAndMakeVisible (spanBox);
    waveform.onSpanChanged = [this] (double) { spanBox.setSelectedId (0, juce::dontSendNotification); spanBox.setText ("Custom", juce::dontSendNotification); };

    freezeButton.setClickingTogglesState (true);
    freezeButton.onClick = [this] { waveform.setFrozen (freezeButton.getToggleState()); };
    freezeButton.setTooltip ("Freeze the display to inspect hits");
    addAndMakeVisible (freezeButton);

    // ---- Displays ---------------------------------------------------------------------------
    waveform.setTooltip ("Drag the orange threshold line to set the trigger threshold");
    addAndMakeVisible (waveform);
    addAndMakeVisible (meters);

    addAndMakeVisible (matchSection);
    matchSection.addAndMakeVisible (matchDisplay);
    for (auto* b : { &learnHitButton, &learnBleedButton, &clearButton, &fitButton, &scButton })
        matchSection.addAndMakeVisible (*b);

    learnHitButton.setTooltip ("Learn the drum to trigger: play several hits while active, click again to stop");
    learnBleedButton.setTooltip ("Learn bleed to reject (hi-hat, other drums): play several bleed hits while active");
    clearButton.setTooltip ("Clear learned sounds");
    fitButton.setTooltip ("Set threshold and velocity input range from the hits learned with LEARN HIT");
    scButton.setTooltip ("Reject hits that are louder on the sidechain input (connect the bleed source's mic to the sidechain)");

    learnHitButton.onClick = [this]
    {
        processor.setLearnMode (processor.getLearnMode() == LearnMode::target ? LearnMode::none : LearnMode::target);
        if (processor.getLearnMode() == LearnMode::target)
            state.getParameter (ParamID::matchEnable)->setValueNotifyingHost (1.0f);
    };
    learnBleedButton.onClick = [this]
    {
        processor.setLearnMode (processor.getLearnMode() == LearnMode::reject ? LearnMode::none : LearnMode::reject);
    };
    clearButton.onClick = [this] { showClearMenu(); };
    fitButton.onClick = [this] { processor.fitToLearnedHits(); };

    scButton.setClickingTogglesState (true);
    scAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, ParamID::scEnable, scButton);

    matchSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    matchSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    matchSlider.setColour (juce::Slider::trackColourId, Colours::learn);
    matchSlider.setTooltip ("Minimum similarity to the learned sound for a hit to trigger");
    matchSlider.onValueChange = [this]
    {
        matchLabel.setText ("MATCH " + juce::String (juce::roundToInt (matchSlider.getValue())) + "%", juce::dontSendNotification);
    };
    matchLabel.setFont (uiFont (11.0f, true));
    matchLabel.setColour (juce::Label::textColourId, Colours::text);
    matchLabel.setInterceptsMouseClicks (false, false);
    matchSection.addAndMakeVisible (matchLabel);
    matchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, ParamID::matchAmount, matchSlider);
    matchSlider.onValueChange();
    matchSection.addAndMakeVisible (matchSlider);

    addAndMakeVisible (hitSection);
    hitSection.addAndMakeVisible (hitInfo);
    hitSection.addAndMakeVisible (testButton);
    testButton.setTooltip ("Send a test note (velocity 100) to check your MIDI routing");
    testButton.onClick = [this] { processor.sendTestHit (100); };

    // ---- Control sections -----------------------------------------------------------------
    for (auto* s : { &detectionSection, &velocitySection, &midiSection, &gateSection, &compSection, &outputSection })
        addAndMakeVisible (*s);

    for (auto* k : { &threshold, &sensitivity, &retrigger, &hpf, &lpf, &dynAmount, &dynRelease, &scMargin })
        detectionSection.addAndMakeVisible (*k);

    velocitySection.addAndMakeVisible (velocityCurve);
    for (auto* k : { &inLow, &inHigh, &velCurve, &velMin, &velMax })
        velocitySection.addAndMakeVisible (*k);

    for (auto* k : { &note, &channel, &noteLength, &midiOffset })
        midiSection.addAndMakeVisible (*k);
    thruButton.setClickingTogglesState (true);
    thruButton.setTooltip ("Pass incoming MIDI through to the output");
    thruAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, ParamID::midiThru, thruButton);
    midiSection.addAndMakeVisible (thruButton);

    for (auto* k : { &gateAttack, &gateHold, &gateRelease, &gateRange })
        gateSection.addAndMakeVisible (*k);
    for (auto* k : { &compThreshold, &compRatio, &compKnee, &compAttack, &compRelease, &compMakeup })
        compSection.addAndMakeVisible (*k);

    outputModeBox.addItemList ({ "Processed", "Listen Detect", "MIDI Only" }, 1);
    outputModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, ParamID::outputMode, outputModeBox);
    outputModeBox.setTooltip ("Processed: gated/compressed audio. Listen Detect: hear the detection filter. MIDI Only: mute audio");
    outputSection.addAndMakeVisible (outputModeBox);
    outputSection.addAndMakeVisible (mix);
    outputSection.addAndMakeVisible (outputGain);

    // Helpful tooltips on the less obvious controls
    sensitivity.slider.setTooltip ("How sharp a rise in level must be to count as a new hit. Higher catches softer, slower attacks");
    retrigger.slider.setTooltip ("Dead time after a hit: prevents double triggers (flams) from one hit");
    dynAmount.slider.setTooltip ("Dynamic threshold: after each hit the threshold jumps up and decays, rejecting ringing and bleed right after loud hits");
    scMargin.slider.setTooltip ("Sidechain bleed rejection: a hit is rejected when the sidechain is louder than the input by more than this");
    midiOffset.slider.setTooltip ("Shift MIDI notes earlier/later to align with your samples");

    gateSection.onActiveChanged = [this] { updateSectionEnablement(); };
    compSection.onActiveChanged = [this] { updateSectionEnablement(); };
    matchSection.onActiveChanged = [this] { updateSectionEnablement(); };
    updateSectionEnablement();

    setSize (baseWidth, baseHeight);
}

void MainPanel::updateSectionEnablement()
{
    for (auto* k : { &gateAttack, &gateHold, &gateRelease, &gateRange })
        k->setAlpha (gateSection.isActive() ? 1.0f : 0.45f);
    for (auto* k : { &compThreshold, &compRatio, &compKnee, &compAttack, &compRelease, &compMakeup })
        k->setAlpha (compSection.isActive() ? 1.0f : 0.45f);
    matchSlider.setAlpha (matchSection.isActive() ? 1.0f : 0.45f);
    matchLabel.setAlpha (matchSection.isActive() ? 1.0f : 0.45f);
}

void MainPanel::showClearMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Clear learned target (" + juce::String (processor.getTargetSnapshot().count.load()) + " hits)");
    m.addItem (2, "Clear learned bleed (" + juce::String (processor.getRejectSnapshot().count.load()) + " hits)");
    m.addItem (3, "Clear all");
    m.addSeparator();
    m.addItem (4, "Reset hit counters");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (clearButton), [this] (int r)
    {
        if (r == 1 || r == 3) processor.clearTargetProfile();
        if (r == 2 || r == 3) processor.clearRejectProfile();
        if (r == 4) hitInfo.resetCounters();
    });
}

void MainPanel::layoutKnobGrid (Section&, std::initializer_list<Knob*> knobs, int columns, juce::Rectangle<int> area)
{
    const int rows = ((int) knobs.size() + columns - 1) / columns;
    const int cw = area.getWidth() / columns;
    const int rh = area.getHeight() / juce::jmax (1, rows);
    int i = 0;
    for (auto* k : knobs)
    {
        const int c = i % columns, r = i / columns;
        k->setBounds (area.getX() + c * cw, area.getY() + r * rh, cw, rh);
        ++i;
    }
}

void MainPanel::resized()
{
    const int margin = 12, gap = 8;

    // Header
    presetBox.setBounds (330, 11, 210, 26);
    spanBox.setBounds (548, 11, 110, 26);
    freezeButton.setBounds (666, 11, 74, 26);

    // Top row
    const int topY = 54, topH = 318;
    const int rightW = 312;
    const int rightX = baseWidth - margin - rightW;
    auto waveArea = juce::Rectangle<int> (margin, topY, rightX - gap - margin, topH);
    meters.setBounds (waveArea.removeFromRight (64).reduced (2, 4));
    waveform.setBounds (waveArea.withTrimmedRight (4));

    matchSection.setBounds (rightX, topY, rightW, 196);
    {
        auto c = matchSection.getContentArea();
        matchDisplay.setBounds (c.removeFromTop (92));
        c.removeFromTop (6);
        auto row = c.removeFromTop (24);
        const int bw = (row.getWidth() - 3 * 4) / 4;
        learnHitButton.setBounds (row.removeFromLeft (bw + 6));
        row.removeFromLeft (4);
        learnBleedButton.setBounds (row.removeFromLeft (bw + 10));
        row.removeFromLeft (4);
        fitButton.setBounds (row.removeFromLeft (bw - 4));
        row.removeFromLeft (4);
        clearButton.setBounds (row);
        c.removeFromTop (6);
        auto sliderRow = c.removeFromTop (24);
        scButton.setBounds (sliderRow.removeFromRight (78));
        sliderRow.removeFromRight (6);
        matchLabel.setBounds (sliderRow.removeFromLeft (88));
        matchSlider.setBounds (sliderRow);
    }

    hitSection.setBounds (rightX, topY + 196 + gap, rightW, topH - 196 - gap);
    testButton.setBounds (hitSection.getWidth() - 62, 5, 54, 19);
    hitInfo.setBounds (hitSection.getContentArea());

    // Bottom row
    const int botY = topY + topH + gap;
    const int botH = baseHeight - botY - margin;
    const float colsTotal = 4.0f + 5.0f + 2.0f + 2.0f + 3.0f + 1.6f;
    const int pad = 16;
    const float cw = ((float) (baseWidth - 2 * margin - 5 * gap) - 6.0f * pad) / colsTotal;
    int x = margin;
    auto place = [&] (Section& s, float cols)
    {
        const int w = juce::roundToInt (cols * cw) + pad;
        s.setBounds (x, botY, w, botH);
        x += w + gap;
    };
    place (detectionSection, 4.0f);
    place (velocitySection, 5.0f);
    place (midiSection, 2.0f);
    place (gateSection, 2.0f);
    place (compSection, 3.0f);
    outputSection.setBounds (x, botY, baseWidth - margin - x, botH);

    layoutKnobGrid (detectionSection, { &threshold, &sensitivity, &retrigger, &dynAmount, &hpf, &lpf, &dynRelease, &scMargin }, 4,
                    detectionSection.getContentArea());

    {
        auto c = velocitySection.getContentArea();
        auto curveArea = c.removeFromLeft (juce::roundToInt (2.0f * cw));
        velocityCurve.setBounds (curveArea.reduced (0, 14).withTrimmedRight (6));
        auto top = c.removeFromTop (c.getHeight() / 2);
        layoutKnobGrid (velocitySection, { &inLow, &inHigh, &velCurve }, 3, top);
        layoutKnobGrid (velocitySection, { &velMin, &velMax }, 3, c);
    }

    {
        auto c = midiSection.getContentArea();
        layoutKnobGrid (midiSection, { &note, &channel, &noteLength, &midiOffset }, 2, c);
        thruButton.setBounds (midiSection.getWidth() - 52, 5, 44, 19);
    }

    layoutKnobGrid (gateSection, { &gateAttack, &gateHold, &gateRelease, &gateRange }, 2, gateSection.getContentArea());
    layoutKnobGrid (compSection, { &compThreshold, &compRatio, &compKnee, &compAttack, &compRelease, &compMakeup }, 3,
                    compSection.getContentArea());

    {
        auto c = outputSection.getContentArea();
        outputModeBox.setBounds (c.removeFromTop (24));
        c.removeFromTop (4);
        mix.setBounds (c.removeFromTop (c.getHeight() / 2));
        outputGain.setBounds (c);
    }
}

void MainPanel::paint (juce::Graphics& g)
{
    Hardware::drawChassis (g, { 0, 0, baseWidth, baseHeight }, 46);

    // Logo: stylised transient mark + name
    {
        juce::Path mark;
        const float x0 = 14.0f, y0 = 23.0f;
        mark.startNewSubPath (x0, y0);
        mark.lineTo (x0 + 6.0f, y0);
        mark.lineTo (x0 + 9.0f, y0 - 13.0f);
        mark.lineTo (x0 + 13.0f, y0 + 11.0f);
        mark.lineTo (x0 + 16.0f, y0 - 5.0f);
        mark.lineTo (x0 + 19.0f, y0 + 3.0f);
        mark.lineTo (x0 + 26.0f, y0);
        g.setColour (Colours::signature);
        g.strokePath (mark, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setFont (uiFont (21.0f, true));
        g.setColour (Colours::text);
        g.drawText ("LA", 48, 8, 30, 30, juce::Justification::centredLeft);
        g.setColour (Colours::signature);
        g.drawText ("TRIGGER", 76, 8, 110, 30, juce::Justification::centredLeft);
        g.setColour (Colours::textDim);
        g.setFont (uiFont (10.5f));
        g.drawText ("DRUM TRIGGER  /  GATE  /  MIDI", 178, 15, 150, 16, juce::Justification::centredLeft);
    }

    // Status (right side of header)
    g.setFont (uiFont (11.5f));
    g.setColour (Colours::textDim);
    g.drawText (infoText, baseWidth - 520, 10, 508, 26, juce::Justification::centredRight);
}

void MainPanel::tick()
{
    // Drain audio -> UI queues
    DisplayPoint p;
    float inPeak = 0.0f, outPeak = 0.0f, gr = 0.0f;
    bool any = false;
    while (processor.displayQueue.pop (p))
    {
        waveform.addPoint (p);
        inPeak = juce::jmax (inPeak, p.inPeak);
        outPeak = juce::jmax (outPeak, p.outPeak);
        gr = juce::jmax (gr, p.grDb);
        any = true;
    }
    meters.setLevels (inPeak, outPeak, any ? gr : 0.0f);

    HitEvent h;
    while (processor.hitQueue.pop (h))
    {
        waveform.addHit (h);
        hitInfo.setHit (h);
        if (h.status != HitStatus::manual)
            matchDisplay.setLastHit (h);
        if (h.status == HitStatus::accepted || h.status == HitStatus::learnedTarget)
            velocityCurve.setLastHit (h.peakDb, h.velocity);
    }

    waveform.repaint();
    velocityCurve.tick();
    hitInfo.tick();
    matchDisplay.repaint();

    // Learn buttons blink while active
    blink = (blink + 1) % 40;
    const auto mode = processor.getLearnMode();
    learnHitButton.setToggleState (mode == LearnMode::target && blink < 28, juce::dontSendNotification);
    learnBleedButton.setToggleState (mode == LearnMode::reject && blink < 28, juce::dontSendNotification);
    learnHitButton.setButtonText (mode == LearnMode::target ? "LEARNING " + juce::String (processor.getTargetSnapshot().count.load()) : juce::String ("LEARN HIT"));
    learnBleedButton.setButtonText (mode == LearnMode::reject ? "LEARNING " + juce::String (processor.getRejectSnapshot().count.load()) : juce::String ("LEARN BLEED"));

    // Header info
    const double sr = processor.getEngineSampleRate();
    juce::String info;
    info << "Target " << processor.getTargetSnapshot().count.load() << " / Bleed " << processor.getRejectSnapshot().count.load()
         << "    SC " << (processor.isSidechainConnected() ? "connected" : "off")
         << "    Latency " << juce::String (1000.0 * processor.getLatencySamples() / juce::jmax (1.0, sr), 1) << " ms"
         << "    " << juce::String (sr / 1000.0, 1) << " kHz";
    if (info != infoText)
    {
        infoText = info;
        repaint (baseWidth - 520, 0, 520, 46);
    }

    if (presetBox.getSelectedId() != processor.getCurrentProgram() + 1)
        presetBox.setSelectedId (processor.getCurrentProgram() + 1, juce::dontSendNotification);
}

//==============================================================================
LATriggerEditor::LATriggerEditor (LATriggerProcessor& p)
    : AudioProcessorEditor (p), panel (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (panel);

    setResizable (true, true);
    setResizeLimits (MainPanel::baseWidth * 6 / 10, MainPanel::baseHeight * 6 / 10,
                     MainPanel::baseWidth * 2, MainPanel::baseHeight * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) MainPanel::baseWidth / MainPanel::baseHeight);
    setSize (MainPanel::baseWidth, MainPanel::baseHeight);

    startTimerHz (60);
}

LATriggerEditor::~LATriggerEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void LATriggerEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
}

void LATriggerEditor::resized()
{
    const float scale = (float) getWidth() / (float) MainPanel::baseWidth;
    panel.setTransform (juce::AffineTransform::scale (scale));
    panel.setBounds (0, 0, MainPanel::baseWidth, MainPanel::baseHeight);
}

void LATriggerEditor::timerCallback()
{
    updateDisplays();
}
} // namespace latrigger
