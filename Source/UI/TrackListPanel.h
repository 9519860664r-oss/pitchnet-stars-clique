#pragma once

#include "../JuceHeader.h"
#include "IMainView.h"
#include "Workspace/PanelContent.h"
#include "Components/StyledWidgets.h"
#include "../Utils/Localization.h"
#include "Buttons.h"
#include <memory>
#include <set>
#include <functional>
#include <vector>

/**
 * Content of the left side panel: one header per track, as in a
 * plug-in track list.
 *
 *   [*] Name ................ [Edit] [Ref] [Transfer]
 *
 * - The star marks the instance this window belongs to (hosts without ARA).
 * - Edit (the coloured blob): show this track's notes for editing. The track
 *   being edited has a filled blob and a filled row.
 * - Reference (the grey blob): show this track's notes grey, for orientation
 *   only. Clicking Edit on a reference track swaps the two tracks' roles.
 * - Transfer: arm this instance to record ("transfer") the host's playback.
 *   Shown only where the host has no ARA.
 *
 * Clicking the rest of a row is the same as its Edit button.
 *
 * Below the rows, a "Use DAW Track Color" switch. Off - or when the DAW sent
 * no colour - rows use the default colour instead.
 */
class TrackListPanel : public juce::Component,
                       public juce::TooltipClient,
                       public PanelContent,
                       private juce::Button::Listener
{
public:
    TrackListPanel();

    void setTracks(const std::vector<MainViewTrackEntry>& tracks,
                   const juce::String& activeKey);

    void setUseDawTrackColour(bool use);
    bool getUseDawTrackColour() const { return useDawTrackColour; }

    int getPreferredHeight() const override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    /** A click at `position` with these modifiers (mouseUp and test scripts). */
    void clickAt(juce::Point<int> position, const juce::ModifierKeys& mods);
    /** Test scripts: the row's button centre ("row", "edit", "ghost", "rec"). */
    juce::Point<int> pointFor(int row, const juce::String& part) const;
    juce::String getTooltip() override;

    /** Edit: fired with a track key when the user clicks a track's Edit
        button (or the rest of its row) and it is not the edited track. */
    std::function<void(const juce::String&)> onTrackSelected;

    /** Ctrl/Cmd+Edit: fired with a track key to open it for editing next to
        the current one, or close it again. */
    std::function<void(const juce::String&)> onTrackEditAdded;

    /** Reference: fired with a track key and the new state. */
    std::function<void(const juce::String&, bool)> onTrackPinChanged;

    /** Transfer: fired with a track key and the requested armed state. */
    std::function<void(const juce::String&, bool)> onTrackTransferToggled;

    /** Editing Mix Fader: -1 only edited tracks, 0 edited and
        reference tracks equally, +1 every track. */
    std::function<void(double)> onEditingMixChanged;
    void setEditingMixAvailable(bool available);
    double getEditingMix() const { return editingMix.getValue(); }

    /** Spread Unison Tracks: fan out overlapping notes of different tracks. */
    std::function<void(bool)> onSpreadUnisonChanged;

    /** Fired when the user flips the "Use DAW Track Color" switch. */
    std::function<void(bool)> onUseDawTrackColourChanged;

    /** Fired when the number of rows changes, so the panel can re-measure. */
    std::function<void()> onPreferredHeightChanged;

    /** Lead / doubles groups: actions from the row menu and the group strip. */
    TrackGroupCallbacks groupCallbacks;

    static constexpr int rowHeight = 40;
    static constexpr int rowGap = 10;
    static constexpr int switchGap = 6;       // rows -> switch
    static constexpr int switchRowHeight = 34; // fits a two-line label
    static constexpr int switchWidth = 40;    // the toggle's switch area
    static constexpr int switchLabelX = 35;   // label start, from the content edge
    static constexpr int slotWidth = 22;      // one header button
    static constexpr int slotsRightInset = 6; // last button -> row's right edge

    enum class Element { None, Row, Edit, Reference, Transfer };

private:
    /** Index of the row under a point, or -1 (gaps between rows count as none). */
    int rowAt(juce::Point<int> position) const;
    Element elementAt(juce::Point<int> position, int row) const;
    juce::Rectangle<int> slotBounds(int row, Element element) const;
    int rowsHeight() const;
    static int rowH(); // 52 in Stars Clique, as in the design
    void paintStarsCliqueSettings(juce::Graphics& g);
    juce::Rectangle<int> switchRowBounds() const;
    juce::Rectangle<int> switchLabelBounds() const;
    juce::Colour colourFor(const MainViewTrackEntry& track) const;
    void buttonClicked(juce::Button* button) override;
    void refreshLocalisedText();

    std::vector<MainViewTrackEntry> tracks;
    juce::String activeKey;
    bool useDawTrackColour = true;
    StyledToggleButton useDawColourToggle;
    StyledToggleButton spreadToggle;
    juce::Slider editingMix;
    bool editingMixAvailable = false;
    juce::Rectangle<int> spreadRowBounds() const;
    juce::Rectangle<int> mixRowBounds() const;
    // Selected tracks (by key), as in a DAW's track list: click selects,
    // Ctrl+click adds or removes, Shift+click selects a range. Edit, Ghost
    // and Transfer on a selected track apply to every selected track.
    std::set<juce::String> selectedKeys;
    juce::String selectionAnchor;
    void selectRow(int index, const juce::ModifierKeys& mods);
    std::vector<int> targetRows(int clickedRow) const;
    void setEdit(int row, bool on);
    void setGhost(int row, bool on);
    void setTransfer(int row, bool armed);
    int hoverRow = -1;
    Element hoverElement = Element::None;

    // Lead / doubles groups, drawn under the rows: a Unison switch and a
    // strength slider per group, and a "Find doubles" button.
    struct GroupView
    {
        juce::String id;
        juce::String leadName;
        int doubles = 0;
        bool on = false;
        float strength = 1.0f;
        bool timingOn = false;
        float timingStrength = 1.0f;
    };
    std::vector<GroupView> groups;
    std::vector<std::unique_ptr<juce::Button>> unisonButtons;
    std::vector<std::unique_ptr<juce::Slider>> strengthSliders;
    std::vector<std::unique_ptr<juce::Button>> timingButtons;
    std::vector<std::unique_ptr<juce::Slider>> timingSliders;
    std::unique_ptr<juce::Button> findDoublesButton;
    std::unique_ptr<juce::Button> groupButton;   // "Group selected"
    std::unique_ptr<juce::Button> ungroupButton; // "Ungroup"
    void updateGroupButtons();
    void groupSelected();
    void ungroupSelected();
    std::vector<juce::String> selectedInOrder() const;
    juce::Rectangle<int> roleMarkBounds(int row) const;
    // Groups need two tracks and a host without ARA (FL Studio), where every
    // track is its own PitchNet instance.
    bool groupsAvailable() const { return tracks.size() >= 2 && static_cast<bool>(groupCallbacks.makeGroup); }
    void rebuildGroups();
    int groupsHeight() const;
    juce::Rectangle<int> groupBounds(int index) const;
    juce::Colour groupColour(const juce::String& groupId) const;
    void showRowMenu(int row);
    void paintGroups(juce::Graphics& g);

    LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackListPanel)
};
