#include "PianoKeysRenderer.h"
#include "PianoRollViewHelpers.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

void PianoKeysRenderer::draw(juce::Graphics &g,
                             int componentHeight,
                             int scrollBarSize,
                             ScaleMode activeScaleMode,
                             int activeScaleRootNote,
                             float pitchAxisOffsetSemitones)
{
  if (!coordMapper)
    return;

  const int pianoKeysWidth = CoordinateMapper::pianoKeysWidth;
  const int headerHeight = CoordinateMapper::headerHeight;

  auto keyArea = juce::Rectangle<int>(
      0, headerHeight, pianoKeysWidth,
      juce::jmax(0, componentHeight - headerHeight - scrollBarSize));

  juce::Graphics::ScopedSaveState savedState(g);
  g.reduceClipRegion(keyArea);

  // Background
  g.setColour(APP_COLOR_SURFACE_ALT);
  g.fillRect(keyArea);

  static const char *noteNames[] = {"C", "C#", "D", "D#", "E", "F",
                                    "F#", "G", "G#", "A", "A#", "B"};
  const bool showScaleOverlay =
      activeScaleMode != ScaleMode::None &&
      activeScaleMode != ScaleMode::Chromatic &&
      activeScaleRootNote >= 0;

  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  // Use truncated scrollY to match grid origin (which uses static_cast<int>(scrollY))
  const int scrollYInt = static_cast<int>(coordMapper->getScrollY());

  for (int midi = MIN_MIDI_NOTE; midi <= MAX_MIDI_NOTE; ++midi)
  {
    float y = coordMapper->midiToY(
                  static_cast<float>(midi) + pitchAxisOffsetSemitones) -
              static_cast<float>(scrollYInt) + headerHeight;
    int noteInOctave = (midi % 12 + 12) % 12;

    const auto toneState = pianoRollView::getScaleToneState(
        activeScaleMode, noteInOctave, activeScaleRootNote);

    juce::Colour keyFill = Skin::colour(0xFF232323u);
    if (showScaleOverlay)
      keyFill = toneState == pianoRollView::ScaleToneState::OutOfScale
                    ? Skin::colour(0xFF151515u)
                    : Skin::colour(0xFF1C1C1Bu);
    // Stars Clique: keys like a piano - black keys dark, white keys light (with a
    // key: out-of-scale dark, in-scale light).
    if (Skin::starsClique())
    {
      const bool dark = showScaleOverlay
                            ? toneState == pianoRollView::ScaleToneState::OutOfScale
                            : pianoRollView::isBlackKey(noteInOctave);
      keyFill = dark ? juce::Colour(0x8C0A0418u) : juce::Colour(0x17FFFFFFu);
    }

    g.setColour(keyFill);
    g.fillRect(0.0f, y, static_cast<float>(pianoKeysWidth),
               pixelsPerSemitone - 1);
    g.setColour(Skin::colour(0xFF0D0B0Bu));
    g.fillRect(0.0f, y + pixelsPerSemitone - 1.0f,
               static_cast<float>(pianoKeysWidth), 1.0f);

    int octave = midi / 12 - 1;
    juce::String noteName =
        juce::String(noteNames[noteInOctave]) + juce::String(octave);

    juce::Colour textColour = Skin::starsClique()
        ? (pianoRollView::isBlackKey(noteInOctave) ? juce::Colour(0xFFA996CCu)
                                                   : juce::Colour(0xFFEFE7FBu))
        : Skin::colour(0xFF9A9A9Au);
    if (showScaleOverlay)
    {
      if (toneState == pianoRollView::ScaleToneState::OutOfScale)
        textColour = textColour.withMultipliedAlpha(0.72f);
    }
    g.setColour(textColour);
    g.setFont(Skin::starsClique() ? juce::Font(juce::FontOptions(11.5f, juce::Font::bold)) : juce::Font(13.0f));
    if (Skin::starsClique()) // right-aligned, as in the design
      g.drawText(noteName, 0, static_cast<int>(y), pianoKeysWidth - 4,
                 static_cast<int>(pixelsPerSemitone),
                 juce::Justification::centredRight);
    else
      g.drawText(noteName, 0, static_cast<int>(y), pianoKeysWidth,
                 static_cast<int>(pixelsPerSemitone),
                 juce::Justification::centred);
  }
}
