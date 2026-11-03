#include "Skin.h"

#include <atomic>
#include <algorithm>
#include <cstdlib>

namespace Skin
{
namespace
{
juce::File settingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("PitchNet")
        .getChildFile("skin.txt");
}

std::atomic<int>& current()
{
    // Read once, the first time any colour is asked for.
    static std::atomic<int> skin{[] {
        const auto text = settingsFile().loadFileAsString().trim();
        // "aurora" was this skin's first name; the retired Midnight skin
        // falls back to Classic.
        return text == "starsclique" || text == "aurora" ? static_cast<int>(Id::StarsClique)
                                                         : static_cast<int>(Id::Classic);
    }()};
    return skin;
}
} // namespace

Id active()
{
    return static_cast<Id>(current().load());
}

void setActive(Id skin)
{
    current().store(static_cast<int>(skin));
    auto file = settingsFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText(skin == Id::StarsClique ? "starsclique" : "classic");
}

juce::String name(Id skin)
{
    return skin == Id::StarsClique ? "Stars Clique" : "Classic";
}

juce::Colour colour(juce::uint32 classicArgb)
{
    const juce::Colour classic(classicArgb);
    const auto skin = active();
    if (skin == Id::Classic)
        return classic;

    if (skin == Id::StarsClique)
    {
        // The orange accent becomes lavender; other colours stay.
        const float hue = classic.getHue();
        if (classic.getSaturation() > 0.7f && classic.getBrightness() > 0.6f &&
            hue > 0.02f && hue < 0.12f)
            return juce::Colour(0xFFC084FCu).withAlpha(classic.getFloatAlpha());
        const int r = classic.getRed(), g = classic.getGreen(), b = classic.getBlue();
        const int spread = std::max({std::abs(r - g), std::abs(g - b), std::abs(r - b)});
        if (spread > 24)
            return classic;
        // Greys become violets: deep and rich when dark, almost white when light.
        const float level = classic.getBrightness();
        const float saturation = juce::jmap(level, 0.0f, 1.0f, 0.50f, 0.08f);
        const float brightness = juce::jlimit(0.0f, 1.0f, level * 1.06f + 0.035f);
        return juce::Colour::fromHSV(0.755f, saturation, brightness, classic.getFloatAlpha());
    }

    return classic;
}

namespace
{
// Held by JUCE's shutdown list, not a static: a juce::Image must be freed
// while JUCE is still running, never at DLL unload (that hung the host).
struct BackdropCache : public juce::DeletedAtShutdown
{
    ~BackdropCache() override { clearSingletonInstance(); }
    juce::Image image;
    JUCE_DECLARE_SINGLETON(BackdropCache, false)
};
JUCE_IMPLEMENT_SINGLETON(BackdropCache)

juce::Image backdropFor(int width, int height)
{
    auto& cached = BackdropCache::getInstance()->image;
    if (cached.isValid() && cached.getWidth() == width && cached.getHeight() == height)
        return cached;
    juce::Image image(juce::Image::RGB, juce::jmax(1, width), juce::jmax(1, height), true);
    juce::Graphics g(image);
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    g.fillAll(juce::Colour(0xFF0D0719u)); // #120A24, 30% darker
    const auto glow = [&](float cx, float cy, float radius, juce::Colour colour) {
        juce::ColourGradient gradient(colour, cx, cy, colour.withAlpha(0.0f), cx + radius, cy, true);
        g.setGradientFill(gradient);
        g.fillRect(0.0f, 0.0f, w, h);
    };
    glow(w * 0.15f, h * 0.08f, w * 0.46f, juce::Colour(0x29A855F7u)); // violet, top left
    glow(w * 0.88f, h * 0.18f, w * 0.42f, juce::Colour(0x17EC4899u)); // pink, top right
    glow(w * 0.55f, h * 1.12f, w * 0.60f, juce::Colour(0x264F46E5u)); // indigo, bottom
    cached = image;
    return cached;
}
} // namespace

void paintBackdrop(juce::Graphics& g, const juce::Component& component)
{
    const auto* top = component.getTopLevelComponent();
    if (top == nullptr)
        top = &component;
    const auto image = backdropFor(top->getWidth(), top->getHeight());
    // Where the window's top-left corner is, in this component's coordinates.
    const auto origin = component.getLocalPoint(top, juce::Point<int>());
    g.drawImageAt(image, origin.x, origin.y);
}
} // namespace Skin
