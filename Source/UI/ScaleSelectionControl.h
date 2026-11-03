#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Utils/Localization.h"
#include "Components/StyledWidgets.h"
#include <functional>
#include <optional>

class ScaleSelectionControl : public CompactSelectionButton
{
public:
    ScaleSelectionControl();

    void setProject(Project* projectToUse);
    void showPopup();
    void setProjectKeyScope(bool value) { projectKeyScope = value; refreshLabel(); }
    Project* getProject() const { return project; }
    void setScaleRoot(int rootNote);
    void setScaleMode(ScaleMode mode);
    int getPreferredWidth() const;

    std::function<void(int)> onScaleRootChanged;
    std::function<void(ScaleMode)> onScaleModeChanged;
    std::function<void(std::optional<int>)> onScaleRootPreviewChanged;
    std::function<void(std::optional<ScaleMode>)> onScaleModePreviewChanged;
    std::function<void()> onPreferredWidthChanged;
    // "Edit custom scale..." and "Reference pitch (A4)..." in the menu.
    std::function<void()> onEditCustomScale;
    std::function<void()> onEditReferencePitch;
    /** Re-reads the key (it is shared by every track and may change in
        another window). */
    void refreshFromProject() { refreshLabel(); }

    void previewScaleRoot(std::optional<int> rootNote);
    void previewScaleMode(std::optional<ScaleMode> mode);

    /** Re-reads the scale name after a language change. */
    void refreshLocalisedText();

private:
    void refreshLabel();

    Project* project = nullptr;
    bool projectKeyScope = false;

    LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScaleSelectionControl)
};
