#include "NoteInspector.h"
#include "Components/AppFont.h"

#include "PianoRollComponent.h"
#include "../Utils/Constants.h"
#include "../Utils/NoteGainCurve.h"
#include "../Utils/NoteNameParse.h"
#include "../Utils/UI/Theme.h"

#include <cmath>

namespace {
// Value shared by all notes, or nothing when they differ.
template <typename Get>
std::optional<float> common(const std::vector<Note*>& notes, Get get, float tolerance)
{
    if (notes.empty())
        return std::nullopt;
    const float first = get(*notes.front());
    for (const auto* note : notes)
        if (std::abs(get(*note) - first) > tolerance)
            return std::nullopt;
    return first;
}

bool isMuted(const Note& note) { return note.getVolumeDb() <= NoteGainCurve::kDeletedNoteDb + 0.5f; }

std::vector<Note*> liveSelection(const PianoRollComponent& pianoRoll)
{
    std::vector<Note*> notes;
    for (auto* note : pianoRoll.getSelectedNotes())
        if (note != nullptr && !note->isRest())
            notes.push_back(note);
    return notes;
}
} // namespace

NoteInspector::NoteInspector(PianoRollComponent& owner) : pianoRoll(owner)
{
    using P = PianoRollComponent::NoteProperty;
    setupField(pitch, "Pitch", "", [this](const juce::String& text) {
        pianoRoll.setSelectedNotesProperty(P::Pitch, [text](const Note& n) {
            const float current = n.getMidiNote() + n.getPitchOffset();
            return NoteNameParse::parse(text, current).value_or(current);
        });
    });
    setupField(modulation, "Modulation", " %", [this](const juce::String& text) {
        if (auto v = NoteNameParse::parseAmount(text.upToFirstOccurrenceOf("%", false, false)))
            pianoRoll.setSelectedNotesProperty(P::Modulation, [v](const Note&) { return *v / 100.0f; });
    });
    setupField(drift, "Drift", " %", [this](const juce::String& text) {
        if (auto v = NoteNameParse::parseAmount(text.upToFirstOccurrenceOf("%", false, false)))
            pianoRoll.setSelectedNotesProperty(P::Drift, [v](const Note&) { return *v / 100.0f; });
    });
    setupField(formant, "Formant", " st", [this](const juce::String& text) {
        if (auto v = NoteNameParse::parseAmount(text.upToFirstOccurrenceOf("st", false, true)))
            pianoRoll.setSelectedNotesProperty(P::Formant, [v](const Note&) { return *v; });
    });
    setupField(amplitude, "Amplitude", " dB", [this](const juce::String& text) {
        const auto number = text.upToFirstOccurrenceOf("db", false, true);
        if (auto v = NoteNameParse::parseAmount(number))
            pianoRoll.setSelectedNotesProperty(P::Amplitude, [v](const Note&) {
                return juce::jlimit(-60.0f, 24.0f, *v);
            });
    });

    hz.setJustificationType(juce::Justification::centredLeft);
    hz.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    addAndMakeVisible(hz);

    noteOff.setButtonText("Note Off");
    noteOff.setClickingTogglesState(false);
    noteOff.setTooltip("Mute the selected notes (Note Off); click again to bring them back");
    noteOff.onClick = [this] {
        const auto notes = liveSelection(pianoRoll);
        bool allMuted = !notes.empty();
        for (const auto* n : notes)
            allMuted = allMuted && isMuted(*n);
        pianoRoll.setSelectedNotesProperty(P::Amplitude, [allMuted](const Note&) {
            return allMuted ? 0.0f : NoteGainCurve::kDeletedNoteDb;
        });
        lastSignature = "<none>";
        refresh();
    };
    addAndMakeVisible(noteOff);

    empty.setText("Select notes to see and type their pitch, modulation, drift, formant and level",
                  juce::dontSendNotification);
    empty.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    empty.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(empty);
    lookAndFeelChanged(); // colours for the active skin
}

void NoteInspector::setupField(Field& field, const juce::String& caption,
                               const juce::String& suffix,
                               std::function<void(const juce::String&)> onCommit)
{
    field.suffix = suffix;
    field.caption.setText(caption, juce::dontSendNotification);
    field.caption.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    field.caption.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(field.caption);

    auto& editor = field.editor;
    editor.setJustification(juce::Justification::centredLeft);
    editor.setSelectAllWhenFocused(true);
    editor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha(0.25f));
    editor.setColour(juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha(0.08f));
    editor.setColour(juce::TextEditor::textColourId, APP_COLOR_TEXT_PRIMARY);
    editor.setIndents(4, 3);
    auto commit = [this, &editor, onCommit] {
        onCommit(editor.getText());
        editor.giveAwayKeyboardFocus();
        lastSignature = "<none>";
        refresh();
    };
    editor.onReturnKey = commit;
    editor.onEscapeKey = [this, &editor] {
        editor.giveAwayKeyboardFocus();
        lastSignature = "<none>";
        refresh();
    };
    editor.onFocusLost = [this] {
        lastSignature = "<none>";
        refresh();
    };
    addAndMakeVisible(editor);
}

void NoteInspector::showValue(Field& field, std::optional<float> value, int decimals)
{
    if (field.editor.hasKeyboardFocus(true))
        return; // never overwrite what the user is typing
    field.editor.setText(value ? juce::String(*value, decimals) + field.suffix
                               : juce::String(juce::CharPointer_UTF8("\xe2\x80\x93")),
                         false);
}

juce::String NoteInspector::selectionSignature() const
{
    juce::String signature;
    for (const auto* n : liveSelection(pianoRoll))
        signature << juce::String::toHexString(static_cast<juce::int64>(
                         reinterpret_cast<std::uintptr_t>(n)))
                  << ':' << n->getMidiNote() + n->getPitchOffset() << ':' << n->getVibrato()
                  << ':' << n->getPitchDrift() << ':' << n->getFormantShift() << ':'
                  << n->getVolumeDb() << ';';
    return signature;
}

void NoteInspector::refresh()
{
    const auto signature = selectionSignature();
    if (signature == lastSignature)
        return;
    lastSignature = signature;

    const auto notes = liveSelection(pianoRoll);
    const bool any = !notes.empty();
    // Stars Clique keeps the fields in place and shows "-" with nothing
    // selected; Classic shows a hint instead.
    const bool alwaysShown = Skin::starsClique();
    for (auto* c : std::initializer_list<juce::Component*>{
             &pitch.caption, &pitch.editor, &modulation.caption, &modulation.editor,
             &drift.caption, &drift.editor, &formant.caption, &formant.editor,
             &amplitude.caption, &amplitude.editor, &hz, &noteOff})
        c->setVisible(any || alwaysShown);
    empty.setVisible(!any && !alwaysShown);
    for (auto* field : {&pitch, &modulation, &drift, &formant, &amplitude})
        field->editor.setEnabled(any);
    noteOff.setEnabled(any);
    if (!any)
    {
        const juce::String dash(juce::CharPointer_UTF8("\xe2\x80\x93"));
        for (auto* field : {&pitch, &modulation, &drift, &formant, &amplitude})
            field->editor.setText(dash, false);
        hz.setText({}, juce::dontSendNotification);
        noteOff.muted = false;
        noteOff.repaint();
        return;
    }

    const auto midi = common(notes, [](const Note& n) { return n.getMidiNote() + n.getPitchOffset(); }, 0.005f);
    if (!pitch.editor.hasKeyboardFocus(true))
        pitch.editor.setText(midi ? NoteNameParse::nameFor(*midi)
                                  : juce::String(juce::CharPointer_UTF8("\xe2\x80\x93")),
                             false);
    hz.setText(midi ? juce::String(440.0 * std::pow(2.0, (*midi - 69.0) / 12.0), 1) + " Hz"
                    : juce::String(),
               juce::dontSendNotification);
    showValue(modulation, common(notes, [](const Note& n) { return n.getVibrato() * 100.0f; }, 0.05f), 0);
    showValue(drift, common(notes, [](const Note& n) { return n.getPitchDrift() * 100.0f; }, 0.05f), 0);
    showValue(formant, common(notes, [](const Note& n) { return n.getFormantShift(); }, 0.005f), 1);

    bool allMuted = true;
    for (const auto* n : notes)
        allMuted = allMuted && isMuted(*n);
    if (allMuted)
    {
        if (!amplitude.editor.hasKeyboardFocus(true))
            amplitude.editor.setText("Off", false);
    }
    else
        showValue(amplitude, common(notes, [](const Note& n) { return n.getVolumeDb(); }, 0.05f), 1);
    noteOff.muted = allMuted;
    noteOff.repaint();
    noteOff.setColour(juce::TextButton::buttonColourId,
                      allMuted ? juce::Colours::white.withAlpha(0.28f)
                               : juce::Colours::black.withAlpha(0.25f));
}

void NoteInspector::lookAndFeelChanged()
{
    const bool sc = Skin::starsClique();
    for (auto* field : {&pitch, &modulation, &drift, &formant, &amplitude})
    {
        field->caption.setColour(juce::Label::textColourId,
                                 sc ? juce::Colour(0xFFB9A6DCu) : APP_COLOR_TEXT_MUTED);
        field->editor.setColour(juce::TextEditor::backgroundColourId,
                                juce::Colours::black.withAlpha(sc ? 0.16f : 0.25f));
        field->editor.setColour(juce::TextEditor::outlineColourId,
                                juce::Colours::white.withAlpha(sc ? 0.07f : 0.08f));
        field->editor.applyColourToAllText(APP_COLOR_TEXT_PRIMARY);
        field->caption.setFont(AppFont::getFont(sc ? 12.5f : 14.0f));
        field->editor.setFont(sc ? AppFont::getFont(12.5f).boldened() : AppFont::getFont(14.0f));
        field->editor.setIndents(sc ? 8 : 4, sc ? 6 : 3);
    }
    hz.setFont(AppFont::getFont(sc ? 12.5f : 14.0f));
    noteOff.setButtonText(sc ? juce::String() : juce::String("Note Off"));
    hz.setColour(juce::Label::textColourId, sc ? juce::Colour(0xFFA895C8u) : APP_COLOR_TEXT_MUTED);
    empty.setColour(juce::Label::textColourId, sc ? juce::Colour(0xFFB9A6DCu) : APP_COLOR_TEXT_MUTED);
    lastSignature = "<none>";
    refresh();
    resized();
    repaint();
}

void NoteInspector::paint(juce::Graphics& g)
{
    if (Skin::starsClique())
    {
        // A glass capsule.
        const auto pill = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(Skin::glass(0.075f));
        g.fillRoundedRectangle(pill, pill.getHeight() * 0.5f);
        g.setColour(Skin::glass(0.10f));
        g.drawRoundedRectangle(pill, pill.getHeight() * 0.5f, 1.0f);
        return;
    }
    g.setColour(juce::Colours::black.withAlpha(0.18f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
}

void NoteInspector::resized()
{
    if (Skin::starsClique())
    {
        // Design: 18 px in, 14 px between groups, 28 px fields, 36 px Note Off.
        auto area = getLocalBounds().withTrimmedLeft(18).withTrimmedRight(6);
        noteOff.setBounds(area.removeFromRight(36).withSizeKeepingCentre(36, 36));
        const auto font = AppFont::getFont(12.5f);
        const auto captionW = [&font](Field& f) {
            return juce::GlyphArrangement::getStringWidthInt(font, f.caption.getText()) + 4;
        };
        // Narrow editor: smaller gaps, and the Hz readout goes first.
        int natural = 70 + 84 + 58 * 3 + 62 + 5 * (5 + 14) + 10;
        for (auto* f : {&pitch, &modulation, &drift, &formant, &amplitude})
            natural += captionW(*f);
        const bool tight = natural > area.getWidth();
        const int gap = tight ? 8 : 14;
        const auto place = [&](Field& field, int editorWidth) {
            field.caption.setBounds(area.removeFromLeft(captionW(field)));
            area.removeFromLeft(5);
            field.editor.setBounds(area.removeFromLeft(editorWidth).withSizeKeepingCentre(editorWidth, 28));
            area.removeFromLeft(gap);
        };
        place(pitch, 84);
        hz.setBounds(tight ? juce::Rectangle<int>() : area.removeFromLeft(64));
        if (!tight)
            area.removeFromLeft(6);
        place(modulation, 58);
        place(drift, 58);
        place(formant, 58);
        place(amplitude, 62);
        empty.setBounds({});
        return;
    }
    auto area = getLocalBounds().reduced(Skin::starsClique() ? 16 : 6, 2);
    empty.setBounds(area);
    const auto place = [&area](Field& field, int captionWidth, int editorWidth) {
        field.caption.setBounds(area.removeFromLeft(captionWidth));
        area.removeFromLeft(4);
        field.editor.setBounds(area.removeFromLeft(editorWidth));
        area.removeFromLeft(10);
    };
    place(pitch, 34, 92);
    hz.setBounds(area.removeFromLeft(72));
    area.removeFromLeft(6);
    place(modulation, 70, 56);
    place(drift, 36, 56);
    place(formant, 56, 60);
    place(amplitude, 66, 64);
    noteOff.setBounds(area.removeFromLeft(70));
}
