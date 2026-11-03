#include "ScaleSelectionControl.h"
#include "Components/PitchPopupMenu.h"
#include "../Utils/Localization.h"
#include "../Utils/CustomScaleLibrary.h"
#include "../Utils/ScaleNames.h"
#include <array>

namespace
{
struct ScaleModeOption
{
    ScaleMode mode;
    const char* labelKey; // looked up in the string table, not shown as-is
};

constexpr std::array<const char*, 12> kRootLabels {{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
}};

constexpr std::array<ScaleModeOption, 14> kModeOptions {{
    { ScaleMode::Major, "scale.major" },
    { ScaleMode::Minor, "scale.minor" },
    { ScaleMode::Blues, "scale.blues" },
    { ScaleMode::Dorian, "scale.dorian" },
    { ScaleMode::HarmonicMinor, "scale.harmonic_minor" },
    { ScaleMode::Locrian, "scale.locrian" },
    { ScaleMode::Lydian, "scale.lydian" },
    { ScaleMode::MajorPentatonic, "scale.major_pentatonic" },
    { ScaleMode::MelodicMinor, "scale.melodic_minor" },
    { ScaleMode::MinorPentatonic, "scale.minor_pentatonic" },
    { ScaleMode::Mixolydian, "scale.mixolydian" },
    { ScaleMode::Phrygian, "scale.phrygian" },
    { ScaleMode::PhrygianDominant, "scale.phrygian_dominant" },
    { ScaleMode::WholeTone, "scale.whole_tone" }
}};

juce::String rootLabel(int root)
{
    return root >= 0 && root < static_cast<int>(kRootLabels.size())
        ? kRootLabels[static_cast<size_t>(root)]
        : "C";
}

juce::String modeLabel(ScaleMode mode)
{
    for (const auto& option : kModeOptions)
        if (option.mode == mode)
            return TR(option.labelKey);
    return TR("scale.major");
}

}

ScaleSelectionControl::ScaleSelectionControl()
{
    setButtonText(rootLabel(0) + " " + modeLabel(ScaleMode::Major));
    onClick = [this] { showPopup(); };
}

void ScaleSelectionControl::setProject(Project* projectToUse)
{
    project = projectToUse;
    refreshLabel();
    setEnabled(project != nullptr);
}

void ScaleSelectionControl::showPopup()
{
    if (project == nullptr)
        return;

    refreshLabel();
    constexpr int rootMenuBaseId = 7000;
    constexpr int modeMenuBaseId = 7100;

    juce::PopupMenu rootMenu;
    juce::PopupMenu modeMenu;
    juce::PopupMenu menu;
    auto* lookAndFeel = &pitchPopupMenu::getLookAndFeel();
    rootMenu.setLookAndFeel(lookAndFeel);
    modeMenu.setLookAndFeel(lookAndFeel);
    menu.setLookAndFeel(lookAndFeel);

    const int selectedRoot = projectKeyScope ? project->getProjectKeyRoot() : project->getScaleRootNote();
    for (int i = 0; i < static_cast<int>(kRootLabels.size()); ++i)
    {
        const juce::String label = kRootLabels[static_cast<size_t>(i)];
        auto hoverCallback =
            [safeThis = juce::Component::SafePointer<ScaleSelectionControl>(this), i]()
            {
                if (safeThis != nullptr)
                    safeThis->previewScaleRoot(i);
            };
        rootMenu.addCustomItem(
            rootMenuBaseId + i + 1,
            std::make_unique<pitchPopupMenu::MenuItemComponent>(
                label, selectedRoot == i, std::move(hoverCallback)),
            nullptr, label);
    }

    ScaleMode selectedMode = projectKeyScope ? project->getProjectKeyMode() : project->getPreferredScaleMode();
    if (selectedMode == ScaleMode::None || selectedMode == ScaleMode::Chromatic)
        selectedMode = ScaleMode::Major;
    for (size_t i = 0; i < kModeOptions.size(); ++i)
    {
        const auto mode = kModeOptions[i].mode;
        const juce::String label = TR(kModeOptions[i].labelKey);
        auto hoverCallback =
            [safeThis = juce::Component::SafePointer<ScaleSelectionControl>(this), mode]()
            {
                if (safeThis != nullptr)
                    safeThis->previewScaleMode(mode);
            };
        modeMenu.addCustomItem(
            modeMenuBaseId + static_cast<int>(i),
            std::make_unique<pitchPopupMenu::MenuItemComponent>(
                label, selectedMode == mode, std::move(hoverCallback)),
            nullptr, label);
    }

    // The user's own scale, its saved versions, and A4.
    const bool customInUse = selectedMode == ScaleMode::Custom;
    modeMenu.addSeparator();
    modeMenu.addItem(modeMenuBaseId + 50, ScaleNames::keyName(0, ScaleMode::Custom), true, customInUse);
    const auto savedScales = CustomScaleLibrary::load();
    juce::PopupMenu savedMenu;
    for (size_t i = 0; i < savedScales.size(); ++i)
        savedMenu.addItem(7400 + static_cast<int>(i), savedScales[i].name, true,
                          customInUse && savedScales[i].name == sharedMacroParameters()->customScale.name);

    if (!projectKeyScope) {
        menu.addItem(7200, TR("scale.use_project_key"), true, project->getUseProjectKey());
        menu.addSeparator();
    }
    menu.addSubMenu(TR("scale.root"), rootMenu);
    menu.addSubMenu(TR("scale.mode"), modeMenu);
    menu.addSeparator();
    menu.addSubMenu(TR("scale.saved_custom"), savedMenu, !savedScales.empty());
    menu.addItem(7300, TR("scale.edit_custom"));
    menu.addItem(7500, TR("scale.reference_pitch") + ": " +
                           juce::String(project->getPitchReferenceHz()) + " Hz...");
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(this)
            .withMinimumWidth(getWidth()),
        [safeThis = juce::Component::SafePointer<ScaleSelectionControl>(this)](int result)
        {
            if (safeThis == nullptr)
                return;

            safeThis->previewScaleRoot(std::nullopt);
            safeThis->previewScaleMode(std::nullopt);

            constexpr int rootMenuBaseId = 7000;
            constexpr int modeMenuBaseId = 7100;
            if (result == 7200 && safeThis->project && !safeThis->projectKeyScope) {
                safeThis->project->setUseProjectKey(true);
                safeThis->refreshLabel();
                if (safeThis->onScaleRootChanged)
                    safeThis->onScaleRootChanged(safeThis->project->getScaleRootNote());
                if (safeThis->onScaleModeChanged)
                    safeThis->onScaleModeChanged(safeThis->project->getPreferredScaleMode());
                return;
            }
            if (result == 7300) {
                if (safeThis->onEditCustomScale)
                    safeThis->onEditCustomScale();
                return;
            }
            if (result == 7500) {
                if (safeThis->onEditReferencePitch)
                    safeThis->onEditReferencePitch();
                return;
            }
            if (result >= 7400 && result < 7500) {
                const auto saved = CustomScaleLibrary::load();
                const auto index = static_cast<size_t>(result - 7400);
                if (index < saved.size()) {
                    sharedMacroParameters()->customScale = saved[index];
                    safeThis->setScaleMode(ScaleMode::Custom);
                }
                return;
            }
            if (result == modeMenuBaseId + 50) {
                safeThis->setScaleMode(ScaleMode::Custom);
                return;
            }
            const int rootIndex = result - rootMenuBaseId;
            if (rootIndex >= 1 &&
                rootIndex <= static_cast<int>(kRootLabels.size()))
            {
                safeThis->setScaleRoot(rootIndex - 1);
                return;
            }

            const int modeIndex = result - modeMenuBaseId;
            if (modeIndex >= 0 &&
                modeIndex < static_cast<int>(kModeOptions.size()))
                safeThis->setScaleMode(
                    kModeOptions[static_cast<size_t>(modeIndex)].mode);
        });
}

void ScaleSelectionControl::refreshLocalisedText()
{
    refreshLabel();
}

void ScaleSelectionControl::refreshLabel()
{
    const int root = project != nullptr ? (projectKeyScope ? project->getProjectKeyRoot() : project->getScaleRootNote()) : 0;
    ScaleMode mode =
        project != nullptr ? (projectKeyScope ? project->getProjectKeyMode() : project->getPreferredScaleMode()) : ScaleMode::Major;
    if (mode == ScaleMode::None || mode == ScaleMode::Chromatic)
        mode = ScaleMode::Major;
    const auto text = mode == ScaleMode::Custom ? ScaleNames::keyName(root, mode)
                                                : rootLabel(root) + " " + modeLabel(mode);
    if (getButtonText() == text)
        return;

    setButtonText(text);
    if (onPreferredWidthChanged)
        onPreferredWidthChanged();
}

int ScaleSelectionControl::getPreferredWidth() const
{
    constexpr int minimumWidth = 92;
    constexpr int horizontalPadding = 20;
    const int textWidth = juce::GlyphArrangement::getStringWidthInt(
        AppFont::getFont(13.0f), getButtonText());
    return juce::jmax(minimumWidth, textWidth + horizontalPadding);
}

void ScaleSelectionControl::setScaleRoot(int rootNote)
{
    if (project == nullptr)
        return;
    const int normalized = juce::jlimit(0, 11, rootNote);
    if (projectKeyScope)
        project->setProjectKey(normalized, project->getProjectKeyMode());
    else {
        project->setUseProjectKey(false);
        project->setScaleRootNote(normalized);
    }
    refreshLabel();
    if (onScaleRootChanged)
        onScaleRootChanged(normalized);
}

void ScaleSelectionControl::setScaleMode(ScaleMode mode)
{
    if (project == nullptr)
        return;
    if (mode == ScaleMode::None || mode == ScaleMode::Chromatic)
        mode = ScaleMode::Major;

    if (projectKeyScope)
        project->setProjectKey(project->getProjectKeyRoot(), mode);
    else {
        project->setUseProjectKey(false);
        project->setPreferredScaleMode(mode);
    }
    if (onScaleModeChanged)
        onScaleModeChanged(mode);
    refreshLabel();
}

void ScaleSelectionControl::previewScaleRoot(std::optional<int> rootNote)
{
    if (onScaleRootPreviewChanged)
        onScaleRootPreviewChanged(rootNote);
}

void ScaleSelectionControl::previewScaleMode(std::optional<ScaleMode> mode)
{
    if (onScaleModePreviewChanged)
        onScaleModePreviewChanged(mode);
}
