#include "GridRenderer.h"
#include "PianoRollViewHelpers.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

void GridRenderer::draw(juce::Graphics &g, const Params &params)
{
  if (!coordMapper)
    return;

  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const double scrollX = coordMapper->getScrollX();
  const double scrollY = coordMapper->getScrollY();

  float duration = static_cast<float>(params.timelineDuration);
  if (duration <= 0.0f)
    duration = project ? project->getAudioData().getDuration()
                       : DEFAULT_EMPTY_PROJECT_DURATION_SECONDS;
  if (duration <= 0.0f)
    duration = DEFAULT_EMPTY_PROJECT_DURATION_SECONDS;
  // The timeline may start before project time zero (Track mode).
  const float startX = static_cast<float>(
      std::min(0.0, coordMapper->getViewStartSeconds()) * pixelsPerSecond);
  const float width =
      std::max(duration * pixelsPerSecond, static_cast<float>(params.componentWidth));
  const float height = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

  // Only draw the visible area to avoid spending time on off-screen rows/columns.
  const float visibleStartX =
      juce::jlimit(startX, width, static_cast<float>(scrollX));
  const float visibleEndX = juce::jlimit(
      startX, width,
      visibleStartX + static_cast<float>(params.visibleContentWidth) + 2.0f);
  const float visibleTopY = juce::jlimit(0.0f, height, static_cast<float>(scrollY));
  const float visibleBottomY = juce::jlimit(
      0.0f, height,
      visibleTopY + static_cast<float>(params.visibleContentHeight) + pixelsPerSemitone);

  if (visibleEndX <= visibleStartX || visibleBottomY <= visibleTopY)
    return;

  const int visibleTopMidi = juce::jlimit(
      MIN_MIDI_NOTE, MAX_MIDI_NOTE,
      static_cast<int>(std::ceil(coordMapper->yToMidi(visibleTopY))));
  const int visibleBottomMidi = juce::jlimit(
      MIN_MIDI_NOTE, MAX_MIDI_NOTE,
      static_cast<int>(std::floor(coordMapper->yToMidi(visibleBottomY))));
  const int startMidi = juce::jlimit(MIN_MIDI_NOTE, MAX_MIDI_NOTE,
                                     visibleBottomMidi - 1);
  const int endMidi = juce::jlimit(MIN_MIDI_NOTE, MAX_MIDI_NOTE,
                                   visibleTopMidi + 1);

  // Stars Clique: thin light hairlines instead of dark lines.
  const juce::Colour gridLineColour = Skin::starsClique() ? juce::Colour(0x09FFFFFFu) : APP_COLOR_GRID;
  const juce::Colour gridBarColour = Skin::starsClique() ? juce::Colour(0x1AFFFFFFu) : APP_COLOR_GRID_BAR;
  const bool showScaleOverlay =
      params.scaleMode != ScaleMode::None && params.scaleMode != ScaleMode::Chromatic &&
      params.scaleRootNote >= 0;
  // Stars Clique: rows barely shaded, over the translucent canvas.
  const bool sc = Skin::starsClique();
  // Light rows for notes of the key (or white keys), dark rows for the rest:
  // clearly apart, like a scale highlight in a DAW piano roll.
  const auto whiteKeyRowColour = sc ? juce::Colours::white.withAlpha(0.055f)
                                        : Skin::colour(0xFF232322u);
  const auto blackKeyRowColour = sc ? juce::Colours::black.withAlpha(0.26f)
                                        : Skin::colour(0xFF121212u);

  if (params.pitchLines)
  {
    if (params.drawRowBackgrounds)
    {
      g.setColour(whiteKeyRowColour);
      g.fillRect(visibleStartX, visibleTopY, visibleEndX - visibleStartX,
                 visibleBottomY - visibleTopY);
    }
    for (int midi = startMidi; midi <= endMidi; ++midi)
    {
      const int noteInOctave = (midi % 12 + 12) % 12;
      const bool inScale = !showScaleOverlay ||
          pianoRollView::getScaleToneState(params.scaleMode, noteInOctave, params.scaleRootNote) !=
              pianoRollView::ScaleToneState::OutOfScale;
      const bool root = showScaleOverlay
          ? pianoRollView::getScaleToneState(params.scaleMode, noteInOctave, params.scaleRootNote) ==
                pianoRollView::ScaleToneState::Root
          : noteInOctave == 0;
      const float y = coordMapper->midiToY(static_cast<float>(midi) + params.pitchAxisOffsetSemitones) +
                      pixelsPerSemitone * 0.5f;
      g.setColour(root ? gridBarColour.brighter(0.2f)
                       : inScale ? gridLineColour.brighter(0.15f) : gridLineColour.darker(0.3f));
      g.drawHorizontalLine(static_cast<int>(y), visibleStartX, visibleEndX);
    }
  }

  if (params.drawRowBackgrounds && !params.pitchLines && !showScaleOverlay)
  {
    for (int midi = startMidi; midi <= endMidi; ++midi)
    {
      const int noteInOctave = (midi % 12 + 12) % 12;
      g.setColour(pianoRollView::isBlackKey(noteInOctave) ? blackKeyRowColour
                                                          : whiteKeyRowColour);
      const float y = coordMapper->midiToY(
          static_cast<float>(midi) + params.pitchAxisOffsetSemitones);
      g.fillRect(visibleStartX, y, visibleEndX - visibleStartX,
                 pixelsPerSemitone);
    }
  }

  if (params.drawRowBackgrounds && showScaleOverlay && !params.pitchLines)
  {
    const auto inScaleRowColour = whiteKeyRowColour;
    const auto outOfScaleRowColour = blackKeyRowColour;

    for (int midi = startMidi; midi <= endMidi; ++midi)
    {
      const int noteInOctave = (midi % 12 + 12) % 12;
      const auto toneState = pianoRollView::getScaleToneState(
          params.scaleMode, noteInOctave, params.scaleRootNote);
      g.setColour(toneState == pianoRollView::ScaleToneState::OutOfScale
                      ? outOfScaleRowColour
                      : inScaleRowColour);
      const float y = coordMapper->midiToY(
          static_cast<float>(midi) + params.pitchAxisOffsetSemitones);
      g.fillRect(visibleStartX, y, visibleEndX - visibleStartX, pixelsPerSemitone);
    }

    if (params.scaleMode == ScaleMode::Custom)
    {
      g.setColour(sc ? juce::Colour(0x59D8B4FEu) : Skin::colour(0x59FF9D4Du));
      for (int midi = startMidi; midi <= endMidi; ++midi)
      {
        const float cents = ScaleUtils::customCents(midi);
        if (std::abs(cents) < 0.5f ||
            !ScaleUtils::isPitchClassInScale(params.scaleMode, midi, params.scaleRootNote))
          continue;
        const float y = coordMapper->midiToY(static_cast<float>(midi) + cents / 100.0f +
                                             params.pitchAxisOffsetSemitones) +
                        pixelsPerSemitone * 0.5f;
        const float dash[] = {4.0f, 4.0f};
        g.drawDashedLine(juce::Line<float>(static_cast<float>(visibleStartX), y,
                                           static_cast<float>(visibleEndX), y),
                         dash, 2, 1.0f);
      }
    }
  }

  if (!params.drawGridLines)
    return;

  // Horizontal pitch lines.
  for (int midi = startMidi; midi <= endMidi && !params.pitchLines; ++midi)
  {
    const float y = coordMapper->midiToY(
        static_cast<float>(midi) + params.pitchAxisOffsetSemitones);
    const int noteInOctave = (midi % 12 + 12) % 12;

    if (!showScaleOverlay)
    {
      g.setColour(noteInOctave == 0 ? gridBarColour : gridLineColour);
    }
    else if (pianoRollView::getScaleToneState(params.scaleMode, noteInOctave,
                                              params.scaleRootNote) ==
             pianoRollView::ScaleToneState::Root)
    {
      g.setColour(gridBarColour);
    }
    else if (pianoRollView::getScaleToneState(params.scaleMode, noteInOctave,
                                              params.scaleRootNote) ==
             pianoRollView::ScaleToneState::InScale)
    {
      g.setColour(gridLineColour);
    }
    else
    {
      g.setColour(gridLineColour.darker(0.25f));
    }

    g.drawHorizontalLine(static_cast<int>(y), visibleStartX, visibleEndX);
  }

  if (params.timelineDisplayMode == TimelineDisplayMode::Beats)
  {
    if (params.gridSeconds > 1.0e-6 && params.beatSeconds > 1.0e-6 &&
        params.barSeconds > 1.0e-6)
    {
      // Grid lines sit on HOST beats, like the ruler above them; the canvas
      // is in project time, so each host position is mapped back through the
      // display offset.
      const double visibleStartTime =
          coordMapper->projectToTimeline(visibleStartX / pixelsPerSecond);
      const double visibleEndTime =
          coordMapper->projectToTimeline(visibleEndX / pixelsPerSecond);
      const int firstGrid = std::max(
          0,
          static_cast<int>(std::floor(visibleStartTime / params.gridSeconds)) - 1);

      for (int i = firstGrid;; ++i)
      {
        const double time = static_cast<double>(i) * params.gridSeconds;
        if (time > visibleEndTime + params.gridSeconds)
          break;

        const float x = static_cast<float>(
            coordMapper->timelineToProject(time) * pixelsPerSecond);
        if (x < visibleStartX - 1.0f || x > visibleEndX + 1.0f)
          continue;

        const bool isBarLine = pianoRollView::isMultipleOf(time, params.barSeconds);
        if (isBarLine)
        {
          g.setColour(gridBarColour);
        }
        else if (pianoRollView::isMultipleOf(time, params.beatSeconds))
        {
          g.setColour(gridLineColour);
        }
        else
        {
          g.setColour(gridLineColour.darker(0.2f));
        }
        if (isBarLine)
          g.fillRect(x - 0.5f, visibleTopY, 2.0f,
                     visibleBottomY - visibleTopY);
        else
          g.drawVerticalLine(static_cast<int>(x), visibleTopY, visibleBottomY);
      }
    }
  }
  else
  {
    float secondsPerTick;
    if (pixelsPerSecond >= 200.0f)
      secondsPerTick = 0.5f;
    else if (pixelsPerSecond >= 100.0f)
      secondsPerTick = 1.0f;
    else if (pixelsPerSecond >= 50.0f)
      secondsPerTick = 2.0f;
    else if (pixelsPerSecond >= 25.0f)
      secondsPerTick = 5.0f;
    else
      secondsPerTick = 10.0f;

    const float secondsPerLine = secondsPerTick * 2.0f;
    const float pixelsPerLine = secondsPerLine * pixelsPerSecond;
    if (pixelsPerLine > 1.0e-4f)
    {
      g.setColour(gridLineColour);
      const double startTime =
          coordMapper->projectToTimeline(visibleStartX / pixelsPerSecond);
      const double endTime =
          coordMapper->projectToTimeline(visibleEndX / pixelsPerSecond);
      const double firstTime = std::floor(startTime / secondsPerLine) * secondsPerLine;
      for (double time = firstTime; time <= endTime; time += secondsPerLine) {
        const float x = coordMapper->timeToX(coordMapper->timelineToProject(time));
        if (x >= visibleStartX)
          g.drawVerticalLine(static_cast<int>(x), visibleTopY, visibleBottomY);
      }
    }
  }
}
