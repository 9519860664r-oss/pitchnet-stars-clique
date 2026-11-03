#include "TrackListPanel.h"
#include <algorithm>
#include "Components/AppFont.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/UI/Skin.h"
#include "SkinIcons.h"

namespace
{
constexpr float rowCorner = 6.0f;
constexpr float borderThickness = 1.5f;
constexpr int textInsetX = 12;
constexpr float inactiveBorderAlpha = 0.3f;

const juce::Colour activeFill{0xFF353535u};
const juce::Colour inactiveText{0xFF9A9A9Au};
const juce::Colour referenceGrey{0xFFB4B4B4u};
const juce::Colour transferRed{0xFFE5484Du};

// A small horizontal note blob, the icon for Edit / Reference.
void drawBlob(juce::Graphics& g, juce::Rectangle<float> slot, juce::Colour colour,
              bool filled, bool hovered)
{
    const auto blob = slot.withSizeKeepingCentre(16.0f, 9.0f);
    if (filled)
    {
        g.setColour(colour);
        g.fillRoundedRectangle(blob, 4.5f);
    }
    g.setColour(colour.withAlpha(filled ? 1.0f : (hovered ? 0.9f : 0.55f)));
    g.drawRoundedRectangle(blob.reduced(0.5f), 4.5f, 1.3f);
}

void drawStar(juce::Graphics& g, juce::Point<float> centre, float radius,
              juce::Colour colour)
{
    juce::Path star;
    star.addStar(centre, 5, radius * 0.45f, radius, 0.0f);
    g.setColour(colour);
    g.fillPath(star);
}

// "This window's track": a small window outline.
void drawWindowMark(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto box = area.withSizeKeepingCentre(12.0f, 10.0f);
    g.setColour(colour);
    g.drawRoundedRectangle(box, 2.5f, 1.4f);
    g.fillRect(box.getX(), box.getY(), box.getWidth(), 2.6f);
}

// Group colours, in the order groups appear.
const juce::Colour groupPalette[] = {juce::Colour(0xFFC084FCu), juce::Colour(0xFF67D4FFu),
                                     juce::Colour(0xFFF4B860u), juce::Colour(0xFF4ADE80u),
                                     juce::Colour(0xFFF470B4u)};

constexpr int groupsHeaderHeight = 28;
constexpr int groupActionsHeight = 30;
constexpr int groupBlockHeight = 84;
constexpr int groupGap = 8;

// Small rounded button; as a switch it lights up when on.
class PillButton final : public juce::Button
{
public:
    PillButton(const juce::String& text, bool isSwitch) : juce::Button(text)
    {
        setClickingTogglesState(isSwitch);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    juce::Colour accent{0xFFC084FCu};

    void paintButton(juce::Graphics& g, bool hover, bool down) override
    {
        const auto r = getLocalBounds().toFloat().reduced(1.5f);
        const float corner = r.getHeight() * 0.5f;
        if (getToggleState())
        {
            g.setColour(accent.withAlpha(0.30f));
            g.fillRoundedRectangle(r.expanded(1.5f), corner + 1.5f);
            g.setColour(accent);
            g.fillRoundedRectangle(r, corner);
            g.setColour(juce::Colour(0xFF1B0B33u));
        }
        else
        {
            g.setColour(juce::Colours::white.withAlpha(down ? 0.16f : hover ? 0.10f : 0.05f));
            g.fillRoundedRectangle(r, corner);
            g.setColour(juce::Colours::white.withAlpha(0.30f));
            g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.9f));
        }
        g.setFont(AppFont::getFont(12.0f).boldened());
        g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
    }
};
} // namespace

TrackListPanel::TrackListPanel()
{
    setOpaque(false);

    // The label is painted by the panel so it can wrap in the narrow panel;
    // the toggle only draws the switch.
    useDawColourToggle.setClickingTogglesState(true);
    useDawColourToggle.setToggleState(useDawTrackColour, juce::dontSendNotification);
    useDawColourToggle.addListener(this);
    addAndMakeVisible(useDawColourToggle);

    spreadToggle.setClickingTogglesState(true);
    spreadToggle.onClick = [this] {
        if (onSpreadUnisonChanged)
            onSpreadUnisonChanged(spreadToggle.getToggleState());
    };
    addAndMakeVisible(spreadToggle);

    editingMix.setSliderStyle(juce::Slider::LinearHorizontal);
    editingMix.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    editingMix.setRange(-1.0, 1.0, 0.01);
    editingMix.setValue(0.0, juce::dontSendNotification);
    editingMix.setDoubleClickReturnValue(true, 0.0);
    editingMix.setTooltip("Editing mix: left = only the tracks being edited, "
                          "centre = with the reference tracks, right = every track. "
                          "Applies when PitchNet plays a preview with the DAW stopped.");
    editingMix.onValueChange = [this] {
        if (onEditingMixChanged)
            onEditingMixChanged(editingMix.getValue());
    };
    addChildComponent(editingMix);
    refreshLocalisedText();
}

void TrackListPanel::setEditingMixAvailable(bool available)
{
    if (editingMixAvailable == available)
        return;
    editingMixAvailable = available;
    editingMix.setVisible(available);
    resized();
    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
    repaint();
}

juce::Rectangle<int> TrackListPanel::spreadRowBounds() const
{
    return {0, switchRowBounds().getBottom(), getWidth(), switchRowHeight};
}

juce::Rectangle<int> TrackListPanel::mixRowBounds() const
{
    if (Skin::starsClique())
        return {0, spreadRowBounds().getBottom() + 8, getWidth(), editingMixAvailable ? 84 : 0};
    return {0, spreadRowBounds().getBottom() + 4, getWidth(), editingMixAvailable ? 46 : 0};
}

void TrackListPanel::refreshLocalisedText()
{
    useDawColourToggle.setTitle(TR("panel.use_daw_track_color"));
    repaint();
}

void TrackListPanel::setUseDawTrackColour(bool use)
{
    useDawTrackColour = use;
    useDawColourToggle.setToggleState(use, juce::dontSendNotification);
    repaint();
}

void TrackListPanel::buttonClicked(juce::Button* button)
{
    if (button != &useDawColourToggle)
        return;

    useDawTrackColour = useDawColourToggle.getToggleState();
    repaint();
    if (onUseDawTrackColourChanged)
        onUseDawTrackColourChanged(useDawTrackColour);
}

juce::Colour TrackListPanel::colourFor(const MainViewTrackEntry& track) const
{
    return resolveTrackColour(track, useDawTrackColour);
}

int TrackListPanel::rowH()
{
    return Skin::starsClique() ? 52 : rowHeight;
}

int TrackListPanel::rowsHeight() const
{
    // Room for the "no tracks" line when empty.
    if (tracks.empty())
        return rowH();

    const int n = static_cast<int>(tracks.size());
    return n * rowH() + (n - 1) * rowGap;
}

juce::Rectangle<int> TrackListPanel::switchRowBounds() const
{
    return {0, rowsHeight() + groupsHeight() + (Skin::starsClique() ? 16 : switchGap), getWidth(),
            switchRowHeight};
}

int TrackListPanel::groupsHeight() const
{
    if (!groupsAvailable())
        return 0;
    const int n = static_cast<int>(groups.size());
    return groupGap + groupsHeaderHeight + groupActionsHeight + n * (groupBlockHeight + groupGap);
}

juce::Rectangle<int> TrackListPanel::groupBounds(int index) const
{
    const int top = rowsHeight() + groupGap + groupsHeaderHeight + groupActionsHeight + groupGap;
    return {0, top + index * (groupBlockHeight + groupGap), getWidth(), groupBlockHeight};
}

juce::Colour TrackListPanel::groupColour(const juce::String& groupId) const
{
    for (size_t i = 0; i < groups.size(); ++i)
        if (groups[i].id == groupId)
            return groupPalette[i % (sizeof(groupPalette) / sizeof(groupPalette[0]))];
    return juce::Colours::grey;
}

void TrackListPanel::rebuildGroups()
{
    std::vector<GroupView> next;
    for (const auto& track : tracks)
        if (track.role == 1 && track.groupId.isNotEmpty())
        {
            GroupView view;
            view.id = track.groupId;
            view.leadName = track.name;
            view.on = track.unisonOn;
            view.strength = track.unisonStrength;
            view.timingOn = track.timingOn;
            view.timingStrength = track.timingStrength;
            for (const auto& other : tracks)
                if (other.role == 2 && other.groupId == track.groupId)
                    ++view.doubles;
            next.push_back(view);
        }

    bool sameGroups = next.size() == groups.size();
    for (size_t i = 0; sameGroups && i < next.size(); ++i)
        sameGroups = next[i].id == groups[i].id;
    groups = std::move(next);

    if (!findDoublesButton)
    {
        auto button = std::make_unique<PillButton>("Find doubles", false);
        button->setTooltip("Look through the tracks for doubles of a lead and suggest groups");
        button->onClick = [this] {
            if (groupCallbacks.findDoubles)
                groupCallbacks.findDoubles();
        };
        addChildComponent(*button);
        findDoublesButton = std::move(button);
    }
    findDoublesButton->setVisible(groupsAvailable());
    if (!groupButton)
    {
        auto group = std::make_unique<PillButton>("Group selected", false);
        group->setTooltip("Make a group of the selected tracks: this window's track (or the "
                          "top one) leads, the others are its doubles. Click a D to make "
                          "that track the lead.");
        group->onClick = [this] { groupSelected(); };
        addChildComponent(*group);
        groupButton = std::move(group);
        auto ungroup = std::make_unique<PillButton>("Ungroup", false);
        ungroup->setTooltip("Take the selected tracks out of their groups");
        ungroup->onClick = [this] { ungroupSelected(); };
        addChildComponent(*ungroup);
        ungroupButton = std::move(ungroup);
    }
    groupButton->setVisible(groupsAvailable());
    ungroupButton->setVisible(groupsAvailable());
    updateGroupButtons();

    if (!sameGroups)
    {
        unisonButtons.clear();
        strengthSliders.clear();
        timingButtons.clear();
        timingSliders.clear();
        for (size_t i = 0; i < groups.size(); ++i)
        {
            const auto id = groups[i].id;
            auto button = std::make_unique<PillButton>("Unison", true);
            button->accent = groupColour(id);
            button->setTooltip("Tune the doubles to the lead, note for note. "
                               "Lights up while on; the doubles follow every change to the lead.");
            auto* slider = new juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
            slider->setRange(0.0, 100.0, 1.0);
            slider->setDoubleClickReturnValue(true, 100.0);
            slider->setTooltip("How strongly the doubles are tuned to the lead: "
                               "0 % leaves them as sung, 100 % note for note");
            slider->setColour(juce::Slider::trackColourId, groupColour(id));
            slider->setColour(juce::Slider::thumbColourId, juce::Colour(0xFFF3E8FFu));
            slider->setColour(juce::Slider::backgroundColourId, juce::Colours::white.withAlpha(0.14f));
            auto* buttonPtr = button.get();
            const auto send = [this, id, buttonPtr, slider] {
                if (groupCallbacks.setUnison)
                    groupCallbacks.setUnison(id, buttonPtr->getToggleState(),
                                             static_cast<float>(slider->getValue() / 100.0));
            };
            button->onClick = send;
            // Apply when the drag ends (each change re-renders the doubles).
            slider->onDragEnd = send;
            slider->onValueChange = [this, slider, send] {
                repaint();
                if (!slider->isMouseButtonDown())
                    send();
            };
            addAndMakeVisible(*button);
            addAndMakeVisible(*slider);
            unisonButtons.push_back(std::move(button));
            strengthSliders.emplace_back(slider);

            // Timing: the doubles' syllables move to the lead's.
            auto timing = std::make_unique<PillButton>("Timing", true);
            timing->accent = groupColour(id);
            timing->setTooltip("Line the doubles up with the lead in time, syllable by syllable "
                               "(sibilants move without stretching). Lights up while on.");
            auto* timingSlider = new juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
            timingSlider->setRange(0.0, 100.0, 1.0);
            timingSlider->setDoubleClickReturnValue(true, 100.0);
            timingSlider->setTooltip("How tightly the doubles follow the lead's timing: "
                                     "0 % as sung, 100 % tight (small differences stay)");
            timingSlider->setColour(juce::Slider::trackColourId, groupColour(id));
            timingSlider->setColour(juce::Slider::thumbColourId, juce::Colour(0xFFF3E8FFu));
            timingSlider->setColour(juce::Slider::backgroundColourId, juce::Colours::white.withAlpha(0.14f));
            auto* timingPtr = timing.get();
            const auto sendTiming = [this, id, timingPtr, timingSlider] {
                if (groupCallbacks.setTiming)
                    groupCallbacks.setTiming(id, timingPtr->getToggleState(),
                                             static_cast<float>(timingSlider->getValue() / 100.0));
            };
            timing->onClick = sendTiming;
            timingSlider->onDragEnd = sendTiming;
            timingSlider->onValueChange = [this, timingSlider, sendTiming] {
                repaint();
                if (!timingSlider->isMouseButtonDown())
                    sendTiming();
            };
            addAndMakeVisible(*timing);
            addAndMakeVisible(*timingSlider);
            timingButtons.push_back(std::move(timing));
            timingSliders.emplace_back(timingSlider);
        }
    }
    for (size_t i = 0; i < groups.size() && i < timingButtons.size(); ++i)
    {
        timingButtons[i]->setToggleState(groups[i].timingOn, juce::dontSendNotification);
        if (!timingSliders[i]->isMouseButtonDown())
            timingSliders[i]->setValue(groups[i].timingStrength * 100.0, juce::dontSendNotification);
    }
    for (size_t i = 0; i < groups.size() && i < unisonButtons.size(); ++i)
    {
        unisonButtons[i]->setToggleState(groups[i].on, juce::dontSendNotification);
        if (!strengthSliders[i]->isMouseButtonDown())
            strengthSliders[i]->setValue(groups[i].strength * 100.0, juce::dontSendNotification);
    }
}

std::vector<juce::String> TrackListPanel::selectedInOrder() const
{
    std::vector<juce::String> keys;
    for (const auto& track : tracks)
        if (selectedKeys.count(track.key) != 0)
            keys.push_back(track.key);
    return keys;
}

void TrackListPanel::updateGroupButtons()
{
    if (!groupButton)
        return;
    const auto keys = selectedInOrder();
    bool anyGrouped = false;
    for (const auto& track : tracks)
        if (selectedKeys.count(track.key) != 0 && track.groupId.isNotEmpty())
            anyGrouped = true;
    groupButton->setEnabled(keys.size() >= 2);
    groupButton->setAlpha(keys.size() >= 2 ? 1.0f : 0.4f);
    ungroupButton->setEnabled(anyGrouped);
    ungroupButton->setAlpha(anyGrouped ? 1.0f : 0.4f);
}

void TrackListPanel::groupSelected()
{
    const auto keys = selectedInOrder();
    if (keys.size() < 2 || !groupCallbacks.makeGroup)
        return;
    // This window's track leads when it is selected, else the top one.
    juce::String lead = keys.front();
    for (const auto& track : tracks)
        if (track.ownWindow && selectedKeys.count(track.key) != 0)
            lead = track.key;
    std::vector<juce::String> doubles;
    for (const auto& key : keys)
        if (key != lead)
            doubles.push_back(key);
    groupCallbacks.makeGroup(lead, doubles);
}

void TrackListPanel::ungroupSelected()
{
    if (!groupCallbacks.removeFromGroup)
        return;
    // Doubles first, so a selected lead still finds its group.
    std::vector<juce::String> leads;
    for (const auto& track : tracks)
        if (selectedKeys.count(track.key) != 0 && track.groupId.isNotEmpty())
        {
            if (track.role == 1)
                leads.push_back(track.key);
            else
                groupCallbacks.removeFromGroup(track.key);
        }
    for (const auto& key : leads)
        groupCallbacks.removeFromGroup(key);
}

juce::Rectangle<int> TrackListPanel::roleMarkBounds(int row) const
{
    // Same place paint() puts the star / D: after the window mark, if any.
    if (row < 0 || row >= static_cast<int>(tracks.size()))
        return {};
    const auto& track = tracks[static_cast<size_t>(row)];
    if (track.groupId.isEmpty() || track.role == 0)
        return {};
    int x = Skin::starsClique() ? 12 : 6;
    if (track.ownWindow)
        x += 12 + 6;
    return {x - 3, row * (rowH() + rowGap), 14 + 6, rowH()};
}

void TrackListPanel::paintGroups(juce::Graphics& g)
{
    if (!groupsAvailable())
        return;
    const bool sc = Skin::starsClique();
    const auto text = sc ? juce::Colour(0xFFF1E6FFu) : APP_COLOR_TEXT_PRIMARY;
    const auto muted = sc ? juce::Colour(0xFFB9A6DCu) : APP_COLOR_TEXT_MUTED;
    const int top = rowsHeight() + groupGap;
    g.setColour(text);
    g.setFont(AppFont::getFont(13.0f).boldened());
    g.drawText("Doubles", juce::Rectangle<int>(4, top, getWidth() / 2, groupsHeaderHeight),
               juce::Justification::centredLeft);
    if (groups.empty())
    {
        g.setColour(muted);
        g.setFont(AppFont::getFont(11.0f));
        g.drawFittedText("Select the lead and its doubles, then Group selected.",
                         juce::Rectangle<int>(4, top + groupsHeaderHeight + groupActionsHeight,
                                              getWidth() - 8, 18),
                         juce::Justification::centredLeft, 1);
    }
    for (size_t i = 0; i < groups.size(); ++i)
    {
        const auto& group = groups[i];
        const auto box = groupBounds(static_cast<int>(i));
        const auto colour = groupColour(group.id);
        g.setColour(sc ? juce::Colours::black.withAlpha(0.16f) : Skin::colour(0xFF1E1E1Eu));
        g.fillRoundedRectangle(box.toFloat(), sc ? 14.0f : 6.0f);
        g.setColour(colour);
        g.fillRoundedRectangle(box.toFloat().withWidth(4.0f).reduced(0.0f, 8.0f), 2.0f);
        auto line = box.reduced(12, 6).removeFromTop(22);
        drawStar(g, {static_cast<float>(line.getX() + 6), static_cast<float>(line.getCentreY())},
                 6.0f, colour);
        line.removeFromLeft(16);
        g.setColour(text);
        g.setFont(AppFont::getFont(12.5f).boldened());
        g.drawText(group.leadName + "  +" + juce::String(group.doubles), line,
                   juce::Justification::centredLeft, true);
        g.setColour(muted);
        g.setFont(AppFont::getFont(11.0f));
        const auto unisonRow = box.reduced(12, 6).withTrimmedTop(26).withHeight(22);
        if (i < strengthSliders.size())
            g.drawText(juce::String(juce::roundToInt(strengthSliders[i]->getValue())) + " %",
                       unisonRow.withTrimmedLeft(unisonRow.getWidth() - 38),
                       juce::Justification::centredRight);
        if (i < timingSliders.size())
            g.drawText(juce::String(juce::roundToInt(timingSliders[i]->getValue())) + " %",
                       unisonRow.translated(0, 26).withTrimmedLeft(unisonRow.getWidth() - 38),
                       juce::Justification::centredRight);
    }
}

void TrackListPanel::showRowMenu(int row)
{
    if (row < 0 || row >= static_cast<int>(tracks.size()))
        return;
    const auto key = tracks[static_cast<size_t>(row)].key;
    // The menu works on the selection when the row is part of it.
    if (selectedKeys.count(key) == 0)
    {
        selectedKeys = {key};
        selectionAnchor = key;
        repaint();
    }
    std::vector<juce::String> others;
    for (const auto& track : tracks)
        if (selectedKeys.count(track.key) != 0 && track.key != key)
            others.push_back(track.key);

    juce::PopupMenu menu;
    menu.addItem(1, others.empty() ? "Make lead" : "Make lead, selected tracks as its doubles");
    juce::PopupMenu addTo;
    for (size_t i = 0; i < groups.size(); ++i)
        addTo.addItem(100 + static_cast<int>(i), groups[i].leadName);
    menu.addSubMenu("Add as double to", addTo, !groups.empty());
    menu.addItem(2, "Remove from group", tracks[static_cast<size_t>(row)].groupId.isNotEmpty());
    menu.addSeparator();
    menu.addItem(3, "Find doubles...");
    juce::Component::SafePointer<TrackListPanel> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                       [safe, key, others](int result) {
                           if (safe == nullptr || result == 0)
                               return;
                           auto& cb = safe->groupCallbacks;
                           if (result == 1 && cb.makeGroup)
                               cb.makeGroup(key, others);
                           else if (result == 2)
                               safe->ungroupSelected();
                           else if (result == 3 && cb.findDoubles)
                               cb.findDoubles();
                           else if (result >= 100 && cb.addToGroup)
                           {
                               const auto index = static_cast<size_t>(result - 100);
                               if (index < safe->groups.size())
                               {
                                   std::vector<juce::String> keys = others;
                                   keys.push_back(key);
                                   for (const auto& k : keys)
                                       cb.addToGroup(k, safe->groups[index].id);
                               }
                           }
                       });
}

juce::Rectangle<int> TrackListPanel::switchLabelBounds() const
{
    if (Skin::starsClique()) // label left, switch right
        return switchRowBounds().withTrimmedLeft(4).withTrimmedRight(switchWidth + 4);
    return switchRowBounds().withTrimmedLeft(switchLabelX);
}

juce::Rectangle<int> TrackListPanel::slotBounds(int row, Element element) const
{
    if (row < 0 || row >= static_cast<int>(tracks.size()))
        return {};
    const int rowY = row * (rowH() + rowGap);
    // Right to left: Transfer (only when available), Reference, Edit.
    const bool hasTransfer = tracks[static_cast<size_t>(row)].transferAvailable;
    int index = -1;
    switch (element)
    {
        case Element::Transfer: index = hasTransfer ? 0 : -1; break;
        case Element::Reference: index = hasTransfer ? 1 : 0; break;
        case Element::Edit: index = hasTransfer ? 2 : 1; break;
        default: break;
    }
    if (index < 0)
        return {};
    if (Skin::starsClique())
    {
        // Design: 28 x 16 pills and a 14 px ring, 9 px apart, 12 px inset.
        int right = getWidth() - 12;
        const int widths[] = {hasTransfer ? 14 : 28, 28, 28};
        for (int k = 0; k < index; ++k)
            right -= widths[k] + 9;
        const int w = (element == Element::Transfer) ? 14 : 28;
        return {right - w - 4, rowY, w + 8, rowH()};
    }
    const int right = getWidth() - slotsRightInset - index * slotWidth;
    return {right - slotWidth, rowY, slotWidth, rowHeight};
}

void TrackListPanel::lookAndFeelChanged()
{
    const bool sc = Skin::starsClique();
    editingMix.setColour(juce::Slider::trackColourId, sc ? juce::Colour(0xFFC084FCu) : APP_COLOR_PRIMARY);
    editingMix.setColour(juce::Slider::thumbColourId, sc ? juce::Colour(0xFFF3E8FFu) : APP_COLOR_PRIMARY);
    editingMix.setColour(juce::Slider::backgroundColourId,
                         sc ? juce::Colours::white.withAlpha(0.14f) : APP_COLOR_SURFACE_ALT);
    resized();
    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
    repaint();
}

void TrackListPanel::resized()
{
    if (findDoublesButton)
        findDoublesButton->setBounds(getWidth() - 104, rowsHeight() + groupGap + 3, 100, 22);
    if (groupButton)
    {
        const int y = rowsHeight() + groupGap + groupsHeaderHeight + 3;
        const int w = (getWidth() - 12) / 2;
        groupButton->setBounds(4, y, w, 24);
        ungroupButton->setBounds(8 + w, y, w, 24);
    }
    for (size_t i = 0; i < unisonButtons.size(); ++i)
    {
        // Title row, then Unison and Timing: switch, strength, percent.
        const auto box = groupBounds(static_cast<int>(i)).reduced(12, 6);
        const auto unisonRow = box.withTrimmedTop(26).withHeight(22);
        unisonButtons[i]->setBounds(unisonRow.withWidth(66));
        strengthSliders[i]->setBounds(unisonRow.withTrimmedLeft(72).withTrimmedRight(40));
        if (i < timingButtons.size())
        {
            const auto timingRow = unisonRow.translated(0, 26);
            timingButtons[i]->setBounds(timingRow.withWidth(66));
            timingSliders[i]->setBounds(timingRow.withTrimmedLeft(72).withTrimmedRight(40));
        }
    }
    if (Skin::starsClique())
    {
        auto onRight = [this](juce::Rectangle<int> r) {
            return r.removeFromRight(switchWidth).withSizeKeepingCentre(switchWidth, 24);
        };
        useDawColourToggle.setBounds(onRight(switchRowBounds()));
        spreadToggle.setBounds(onRight(spreadRowBounds()));
        // Inside the dark box: title, slider, captions.
        editingMix.setBounds(mixRowBounds().reduced(10, 0).withTrimmedTop(30).withHeight(22));
        return;
    }
    const auto row = switchRowBounds();
    // Nudged 3px left of the content edge; the switch is drawn 4px inside
    // the toggle's bounds, so nothing is clipped.
    useDawColourToggle.setBounds(row.withWidth(switchWidth)
                                     .withSizeKeepingCentre(switchWidth, 24)
                                     .translated(-3, 0));
    spreadToggle.setBounds(spreadRowBounds().withWidth(switchWidth)
                               .withSizeKeepingCentre(switchWidth, 24)
                               .translated(-3, 0));
    editingMix.setBounds(mixRowBounds().withTrimmedTop(20).reduced(2, 0));
}

int TrackListPanel::rowAt(juce::Point<int> position) const
{
    if (position.x < 0 || position.x >= getWidth() || position.y < 0)
        return -1;

    const int stride = rowH() + rowGap;
    const int index = position.y / stride;
    if (index >= static_cast<int>(tracks.size()) || position.y % stride >= rowH())
        return -1;

    return index;
}

TrackListPanel::Element TrackListPanel::elementAt(juce::Point<int> position,
                                                  int row) const
{
    if (row < 0)
        return Element::None;
    for (auto element : {Element::Edit, Element::Reference, Element::Transfer})
        if (slotBounds(row, element).contains(position))
            return element;
    return Element::Row;
}

void TrackListPanel::mouseMove(const juce::MouseEvent& e)
{
    const int row = rowAt(e.getPosition());
    const auto element = elementAt(e.getPosition(), row);
    if (row != hoverRow || element != hoverElement)
    {
        hoverRow = row;
        hoverElement = element;
        repaint();
    }

    bool clickable = switchLabelBounds().contains(e.getPosition());
    if (row >= 0)
        clickable = true;
    setMouseCursor(clickable ? juce::MouseCursor::PointingHandCursor
                             : juce::MouseCursor::NormalCursor);
}

void TrackListPanel::mouseExit(const juce::MouseEvent&)
{
    hoverRow = -1;
    hoverElement = Element::None;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint();
}

void TrackListPanel::mouseUp(const juce::MouseEvent& e)
{
    if (e.mouseWasDraggedSinceMouseDown())
        return;
    if (e.mods.isPopupMenu())
    {
        if (groupsAvailable())
            showRowMenu(rowAt(e.getPosition()));
        return;
    }
    clickAt(e.getPosition(), e.mods);
}

juce::Point<int> TrackListPanel::pointFor(int row, const juce::String& part) const
{
    if (row < 0 || row >= static_cast<int>(tracks.size()))
        return {-1, -1};
    const auto element = part == "edit" ? Element::Edit
                       : part == "ghost" ? Element::Reference
                       : part == "rec" ? Element::Transfer : Element::Row;
    if (element == Element::Row)
        return {20, row * (rowH() + rowGap) + rowH() / 2};
    return slotBounds(row, element).getCentre();
}

void TrackListPanel::clickAt(juce::Point<int> position, const juce::ModifierKeys& mods)
{
    // The label toggles the switch too.
    if (switchLabelBounds().contains(position))
    {
        useDawColourToggle.triggerClick();
        return;
    }

    const int index = rowAt(position);
    if (index < 0)
    {
        // A click on empty space between the rows and the groups clears the
        // selection.
        if (position.y < rowsHeight() + groupGap && !selectedKeys.empty())
        {
            selectedKeys.clear();
            updateGroupButtons();
            repaint();
        }
        return;
    }

    const auto& track = tracks[static_cast<size_t>(index)];
    if (track.role == 2 && roleMarkBounds(index).contains(position) && groupCallbacks.setLead)
    {
        groupCallbacks.setLead(track.key);
        return;
    }
    const auto targets = targetRows(index);

    switch (elementAt(position, index))
    {
        case Element::Transfer:
        {
            // The clicked track decides on or off; every selected track follows.
            const bool armed = !track.transferArmed;
            for (int row : targets)
                setTransfer(row, armed);
            repaint();
            return;
        }

        case Element::Reference:
        {
            const bool on = !track.pinned;
            for (int row : targets)
                setGhost(row, on);
            repaint();
            return;
        }

        case Element::Edit:
        {
            // Edit is an on/off switch: any number of tracks can be edited at
            // once. Where the editor has no such switch (ARA), it switches to
            // the track instead.
            if (!onTrackEditAdded)
            {
                if (track.key != activeKey && onTrackSelected)
                    onTrackSelected(track.key);
                return;
            }
            const bool on = !track.coEdited;
            for (int row : targets)
                setEdit(row, on);
            repaint();
            return;
        }

        case Element::Row:
            selectRow(index, mods);
            return;

        case Element::None:
            return;
    }
}

void TrackListPanel::selectRow(int index, const juce::ModifierKeys& mods)
{
    const auto& key = tracks[static_cast<size_t>(index)].key;
    int anchor = -1;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (tracks[static_cast<size_t>(i)].key == selectionAnchor)
            anchor = i;

    if (mods.isShiftDown() && anchor >= 0)
    {
        if (!mods.isCommandDown())
            selectedKeys.clear();
        for (int i = juce::jmin(anchor, index); i <= juce::jmax(anchor, index); ++i)
            selectedKeys.insert(tracks[static_cast<size_t>(i)].key);
    }
    else if (mods.isCommandDown())
    {
        if (!selectedKeys.erase(key))
            selectedKeys.insert(key);
        selectionAnchor = key;
    }
    else
    {
        selectedKeys = {key};
        selectionAnchor = key;
    }
    updateGroupButtons();
    repaint();
}

std::vector<int> TrackListPanel::targetRows(int clickedRow) const
{
    // A button on a selected track works on the whole selection; on any
    // other track, on that track alone.
    const auto& clickedKey = tracks[static_cast<size_t>(clickedRow)].key;
    if (selectedKeys.count(clickedKey) == 0)
        return {clickedRow};
    std::vector<int> rows;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (selectedKeys.count(tracks[static_cast<size_t>(i)].key) != 0)
            rows.push_back(i);
    return rows;
}

void TrackListPanel::setEdit(int row, bool on)
{
    auto& track = tracks[static_cast<size_t>(row)];
    if (track.coEdited == on)
        return;
    track.coEdited = on;
    if (on)
        track.pinned = false;
    if (onTrackEditAdded)
        onTrackEditAdded(track.key); // a switch: flips this track
}

void TrackListPanel::setGhost(int row, bool on)
{
    auto& track = tracks[static_cast<size_t>(row)];
    if (track.pinned == on)
        return;
    track.pinned = on;
    if (on)
        track.coEdited = false;
    if (onTrackPinChanged)
        onTrackPinChanged(track.key, on);
}

void TrackListPanel::setTransfer(int row, bool armed)
{
    auto& track = tracks[static_cast<size_t>(row)];
    if (!track.transferAvailable || track.transferArmed == armed)
        return;
    track.transferArmed = armed; // the editor republishes
    if (onTrackTransferToggled)
        onTrackTransferToggled(track.key, armed);
}

juce::String TrackListPanel::getTooltip()
{
    if (hoverRow < 0 || hoverRow >= static_cast<int>(tracks.size()))
        return {};
    switch (hoverElement)
    {
        case Element::Edit: return TR("tooltip.track_edit");
        case Element::Reference: return TR("tooltip.track_reference");
        case Element::Transfer: return TR("tooltip.track_transfer");
        case Element::Row:
            return "Click to select. Ctrl+click adds or removes a track, Shift+click selects a range. "
                   "Edit, Ghost and Rec on a selected track change every selected track.";
        case Element::None: break;
    }
    return {};
}

void TrackListPanel::setTracks(const std::vector<MainViewTrackEntry>& newTracks,
                               const juce::String& newActiveKey)
{
    const bool countChanged = newTracks.size() != tracks.size();
    const auto groupsBefore = groups.size();
    tracks = newTracks;
    activeKey = newActiveKey;
    rebuildGroups();
    if (groups.size() != groupsBefore)
    {
        resized();
        if (onPreferredHeightChanged)
            onPreferredHeightChanged();
    }
    for (auto it = selectedKeys.begin(); it != selectedKeys.end();)
    {
        const bool exists = std::any_of(tracks.begin(), tracks.end(),
                                        [&](const MainViewTrackEntry& t) { return t.key == *it; });
        it = exists ? std::next(it) : selectedKeys.erase(it);
    }

    if (countChanged)
    {
        resized(); // the switch sits below the rows
        if (onPreferredHeightChanged)
            onPreferredHeightChanged();
    }

    repaint();
}

int TrackListPanel::getPreferredHeight() const
{
    return mixRowBounds().getBottom() + 4;
}

// Edit switch colour, the same on every track.
static const juce::Colour editBlue{0xFF4FC3F7};

void TrackListPanel::paintStarsCliqueSettings(juce::Graphics& g)
{
    const auto text = juce::Colour(0xFFF1E6FFu);
    g.setFont(AppFont::getFont(13.5f));
    g.setColour(text);
    g.drawFittedText(TR("panel.use_daw_track_color"), switchLabelBounds(),
                     juce::Justification::centredLeft, 2, 1.0f);
    g.drawFittedText("Spread unison tracks",
                     spreadRowBounds().withTrimmedLeft(4).withTrimmedRight(switchWidth + 4),
                     juce::Justification::centredLeft, 2, 1.0f);
    if (!editingMixAvailable)
        return;
    const auto box = mixRowBounds().toFloat();
    g.setColour(juce::Colours::black.withAlpha(0.16f));
    g.fillRoundedRectangle(box, 18.0f);
    auto inner = mixRowBounds().reduced(14, 12);
    g.setFont(AppFont::getFont(12.0f).boldened());
    g.setColour(juce::Colour(0xFFE2C8FFu));
    g.drawText("Editing Mix", inner.removeFromTop(16), juce::Justification::centredLeft);
    const auto captions = inner.removeFromBottom(14);
    g.setFont(AppFont::getFont(11.0f));
    g.setColour(juce::Colour(0xFFD2BFF0u));
    g.drawText("Edited", captions, juce::Justification::centredLeft);
    g.drawText("+ Ref", captions, juce::Justification::centred);
    g.drawText("All", captions, juce::Justification::centredRight);
}

void TrackListPanel::paint(juce::Graphics& g)
{
    paintGroups(g);
    if (Skin::starsClique())
        paintStarsCliqueSettings(g);
    else
    {
    // Switch label, wrapped to two lines if the panel is too narrow for one.
    // Same 14px as the switch labels DarkLookAndFeel draws in the right panel.
    g.setFont(AppFont::getFont(14.0f));
    g.setColour(APP_COLOR_TEXT_PRIMARY);
    g.drawFittedText(TR("panel.use_daw_track_color"), switchLabelBounds(),
                     juce::Justification::centredLeft, 2, 1.0f);
    g.drawFittedText("Spread unison tracks", spreadRowBounds().withTrimmedLeft(switchLabelX),
                     juce::Justification::centredLeft, 2, 1.0f);
    if (editingMixAvailable)
    {
        const auto mix = mixRowBounds();
        g.setFont(AppFont::getFont(12.0f));
        g.setColour(APP_COLOR_TEXT_MUTED);
        const auto labels = mix.withHeight(18);
        g.drawText("Edited", labels, juce::Justification::centredLeft);
        g.drawText("+ Ref", labels, juce::Justification::centred);
        g.drawText("All", labels, juce::Justification::centredRight);
        g.setFont(AppFont::getFont(14.0f));
        g.setColour(APP_COLOR_TEXT_PRIMARY);
    }
    }

    const auto font = AppFont::getFont(13.0f);
    g.setFont(font);

    if (tracks.empty())
    {
        g.setColour(APP_COLOR_TEXT_MUTED);
        g.drawText(TR("panel.no_tracks"),
                   juce::Rectangle<int>(0, 0, getWidth(), rowH()).withTrimmedLeft(textInsetX),
                   juce::Justification::centredLeft, true);
        return;
    }

    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
    {
        const auto& track = tracks[static_cast<size_t>(i)];
        const bool isActive = track.key == activeKey;
        const bool isSelected = selectedKeys.count(track.key) != 0;
        const auto colour = colourFor(track);
        const auto row = juce::Rectangle<int>(0, i * (rowH() + rowGap), getWidth(), rowH());
        const auto rowF = row.toFloat().reduced(borderThickness * 0.5f);

        const bool sc = Skin::starsClique();
        if (sc)
        {
            // Glass rows: the window's own track lighter, with a soft
            // lavender edge.
            const float corner = 18.0f;
            const bool hover = i == hoverRow && hoverElement == Element::Row;
            if (isSelected)
            {
                g.setColour(juce::Colour(0xFFC084FCu).withAlpha(0.20f * Skin::glowLevel));
                g.fillRoundedRectangle(rowF.expanded(2.0f), corner + 2.0f);
            }
            g.setColour(Skin::glass(isSelected ? 0.13f : hover ? 0.08f : 0.05f));
            g.fillRoundedRectangle(rowF, corner);
            g.setColour(isSelected ? juce::Colour(0xFFE9D5FFu).withAlpha(0.55f * Skin::glassLevel) : Skin::glass(0.11f));
            g.drawRoundedRectangle(rowF, corner, 1.0f);
        }
        else
        {
            if (isSelected)
            {
                g.setColour(activeFill);
                g.fillRoundedRectangle(rowF, rowCorner);
            }
            else if (i == hoverRow && hoverElement == Element::Row)
            {
                g.setColour(activeFill.withAlpha(0.5f));
                g.fillRoundedRectangle(rowF, rowCorner);
            }

            g.setColour(isSelected ? colour : colour.withAlpha(inactiveBorderAlpha));
            g.drawRoundedRectangle(rowF, rowCorner, borderThickness);
        }

        // Marks before the name: a window for this window's own track, a
        // star for a group's lead, "D" for a double; a stripe in the group's
        // colour down the row's left edge.
        const auto markColour = sc ? juce::Colours::white : isActive ? colour : inactiveText;
        int markX = row.getX() + (sc ? 12 : 6);
        const auto markArea = [&](int width) {
            const auto area = juce::Rectangle<float>(static_cast<float>(markX), rowF.getY(),
                                                     static_cast<float>(width), rowF.getHeight());
            markX += width + 6;
            return area;
        };
        const bool inLiveGroup =
            track.groupId.isNotEmpty() &&
            std::any_of(groups.begin(), groups.end(),
                        [&](const GroupView& group) { return group.id == track.groupId; });
        if (inLiveGroup)
        {
            g.setColour(groupColour(track.groupId));
            g.fillRoundedRectangle(rowF.withWidth(3.5f).reduced(0.0f, rowF.getHeight() * 0.22f)
                                       .translated(sc ? 3.0f : 1.5f, 0.0f), 1.75f);
        }
        if (track.ownWindow)
            drawWindowMark(g, markArea(12), markColour);
        if (track.role == 1 && inLiveGroup)
        {
            const auto area = markArea(12);
            drawStar(g, area.getCentre(), 6.5f, groupColour(track.groupId));
        }
        else if (track.role == 2 && inLiveGroup)
        {
            const auto area = markArea(14);
            g.setColour(groupColour(track.groupId));
            g.drawRoundedRectangle(area.withSizeKeepingCentre(14.0f, 14.0f), 3.0f, 1.2f);
            g.setFont(AppFont::getFont(10.0f).boldened());
            g.drawText("D", area.toNearestInt(), juce::Justification::centred);
        }
        int textLeft = juce::jmax(markX - row.getX(), sc ? 33 : textInsetX);

        // Name, up to the first button.
        const auto firstSlot = slotBounds(i, Element::Edit);
        if (sc)
        {
            g.setColour(isSelected || isActive ? juce::Colours::white : juce::Colour(0xFFE6D8FAu));
            g.setFont(AppFont::getFont(14.0f).boldened());
        }
        else
            g.setColour(isActive ? colour : inactiveText);
        g.drawText(track.name,
                   row.withTrimmedLeft(textLeft).withRight(firstSlot.getX() - 2),
                   juce::Justification::centredLeft, true);

        if (sc)
        {
            // Design: Edit = blue pill, Ghost = white pill, Transfer = pink ring.
            const bool editOn = onTrackEditAdded ? track.coEdited : (isActive || track.coEdited);
            const auto blue = juce::Colour(0xFF67D4FFu);
            auto pill = [&](Element element, juce::Colour c, bool on, float onAlpha, float offAlpha) {
                const auto r = slotBounds(i, element).toFloat().withSizeKeepingCentre(28.0f, 16.0f);
                const bool hv = i == hoverRow && hoverElement == element;
                if (on)
                {
                    if (c == blue)
                    {
                        g.setColour(blue.withAlpha(0.30f * Skin::glowLevel));
                        g.fillRoundedRectangle(r.expanded(2.0f), 10.0f);
                    }
                    g.setColour(c.withAlpha(onAlpha));
                    g.fillRoundedRectangle(r, 8.0f);
                }
                else
                {
                    g.setColour(c.withAlpha(hv ? juce::jmin(1.0f, offAlpha + 0.3f) : offAlpha));
                    g.drawRoundedRectangle(r.reduced(1.0f), 7.0f, 2.0f);
                }
            };
            pill(Element::Edit, blue, editOn, 1.0f, 1.0f);
            pill(Element::Reference, juce::Colours::white, track.pinned, 0.72f, 0.40f);
            if (track.transferAvailable)
            {
                const auto pink = juce::Colour(0xFFFF6B9Au);
                const auto dot = slotBounds(i, Element::Transfer).toFloat().withSizeKeepingCentre(14.0f, 14.0f);
                if (track.transferActive)
                {
                    g.setColour(pink.withAlpha(0.35f));
                    g.fillEllipse(dot.expanded(3.0f));
                }
                g.setColour(pink);
                if (track.transferArmed)
                    g.fillEllipse(dot);
                else
                    g.drawEllipse(dot.reduced(1.0f), 2.0f);
            }
            g.setFont(font);
            continue;
        }

        const auto hovered = [&](Element element) {
            return i == hoverRow && hoverElement == element;
        };

        // Edit: light blue, filled on every track being edited (this
        // window's own track included, which can be switched off too).
        const bool editOn = onTrackEditAdded ? track.coEdited : (isActive || track.coEdited);
        drawBlob(g, firstSlot.toFloat(), editBlue, editOn, hovered(Element::Edit));

        // Reference (ghost): grey, filled when on.
        drawBlob(g, slotBounds(i, Element::Reference).toFloat(), referenceGrey,
                 track.pinned, hovered(Element::Reference));
        g.setFont(font);

        // Transfer: a record dot; outlined when off, filled when armed,
        // with a halo while audio is actually being recorded.
        if (track.transferAvailable)
        {
            const auto slot = slotBounds(i, Element::Transfer).toFloat();
            const auto dot = slot.withSizeKeepingCentre(10.0f, 10.0f);
            if (track.transferActive)
            {
                g.setColour(transferRed.withAlpha(0.35f));
                g.fillEllipse(dot.expanded(3.0f));
            }
            if (track.transferArmed)
            {
                g.setColour(transferRed);
                g.fillEllipse(dot);
            }
            else
            {
                g.setColour(transferRed.withAlpha(hovered(Element::Transfer) ? 0.95f : 0.6f));
                g.drawEllipse(dot.reduced(0.6f), 1.3f);
            }
        }
    }
}
