// PluginHostProbe - load a VST3 the way a host without ARA (FL Studio) does,
// open its editor and save a screenshot, plus a list of visible controls.
//
//   PluginHostProbe <plugin.vst3> <out.png> [instances=1]
//
// With instances > 1 it loads that many copies in this one process (as FL
// does), so the editor of the first can show the others in its track list.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <cstdio>
#include <memory>
#include <vector>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

namespace {

// A host transport for feeding audio: playing or stopped, at a position.
struct ProbePlayHead final : juce::AudioPlayHead {
  bool playing = false;
  juce::int64 samples = 0;
  double rate = 48000.0;
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo info;
    info.setIsPlaying(playing);
    info.setTimeInSamples(samples);
    info.setTimeInSeconds(static_cast<double>(samples) / rate);
    info.setBpm(120.0);
    info.setTimeSignature(juce::AudioPlayHead::TimeSignature{4, 4});
    info.setPpqPosition(static_cast<double>(samples) / rate * 2.0);
    return info;
  }
};

#if JUCE_WINDOWS
// Capture one window (with the plug-in's own child window inside) through
// the compositor, even when other windows cover it. Writes a 24-bit BMP.
bool captureWindow(void *nativeHandle, const juce::File &bmpFile) {
  auto hwnd = static_cast<HWND>(nativeHandle);
  RECT rect{};
  if (hwnd == nullptr || !GetWindowRect(hwnd, &rect))
    return false;
  const int w = rect.right - rect.left, h = rect.bottom - rect.top;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h);
  auto old = SelectObject(mem, bitmap);
  const bool printed = PrintWindow(hwnd, mem, 2 /* PW_RENDERFULLCONTENT */) != 0;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = w;
  info.bmiHeader.biHeight = h;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 24;
  info.bmiHeader.biCompression = BI_RGB;
  const int stride = ((w * 3 + 3) / 4) * 4;
  std::vector<char> pixels(static_cast<size_t>(stride) * h);
  GetDIBits(mem, bitmap, 0, h, pixels.data(), &info, DIB_RGB_COLORS);
  SelectObject(mem, old);
  DeleteObject(bitmap);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);

  BITMAPFILEHEADER header{};
  header.bfType = 0x4D42;
  header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
  bmpFile.deleteFile();
  juce::FileOutputStream out(bmpFile);
  if (!out.openedOk())
    return false;
  out.write(&header, sizeof(header));
  out.write(&info.bmiHeader, sizeof(BITMAPINFOHEADER));
  out.write(pixels.data(), pixels.size());
  return printed;
}
#endif

void pump(int ms) {
  juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
}

// Message-thread stalls: a 100 Hz timer that notes every gap over 60 ms
// (what a user sees as the UI freezing). Printed when the probe ends.
struct StallWatch : juce::Timer {
  double last = 0.0, worst = 0.0;
  int stalls = 0;
  juce::String log;
  StallWatch() { last = juce::Time::getMillisecondCounterHiRes(); startTimer(10); }
  void timerCallback() override {
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double gap = now - last;
    last = now;
    if (gap > 60.0) {
      ++stalls;
      worst = std::max(worst, gap);
      log << juce::String(now - gap, 1) << " stall " << juce::String(gap, 1) << " ms\n";
    }
  }
};

void listVisible(juce::Component &c, int depth, juce::String &out) {
  if (!c.isVisible())
    return;
  juce::String line;
  line << juce::String::repeatedString("  ", depth)
       << typeid(c).name() << " '" << c.getName() << "' "
       << c.getBounds().toString();
  if (auto *button = dynamic_cast<juce::Button *>(&c))
    line << " button:'" << button->getButtonText() << "' tip:'"
         << button->getTooltip() << "' on=" << (int)button->getToggleState();
  out << line << "\n";
  for (auto *child : c.getChildren())
    listVisible(*child, depth + 1, out);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::printf("usage: PluginHostProbe plugin.vst3 out.png [instances]\n");
    return 2;
  }
  const juce::String pluginPath = juce::String::fromUTF8(argv[1]);
  const juce::File outPng(juce::String::fromUTF8(argv[2]));
  const int instances = argc > 3 ? juce::jmax(1, std::atoi(argv[3])) : 1;
  const int holdMs = argc > 4 ? juce::jmax(0, std::atoi(argv[4])) : 0;

  juce::ScopedJuceInitialiser_GUI juceInit;
  juce::VST3PluginFormat format;
  juce::OwnedArray<juce::PluginDescription> types;
  format.findAllTypesForFile(types, pluginPath);
  if (types.isEmpty()) {
    std::printf("no plug-in types found in %s\n", argv[1]);
    return 1;
  }
  std::printf("found: %s by %s (%s)\n", types[0]->name.toRawUTF8(),
              types[0]->manufacturerName.toRawUTF8(),
              types[0]->pluginFormatName.toRawUTF8());

  // Optional audio to play into instances, one WAV per instance:
  // PITCHNET_PROBE_FEED="a.wav;b.wav". Arm them first (script "arm:i").
  juce::AudioFormatManager formats;
  formats.registerBasicFormats();
  std::vector<juce::AudioBuffer<float>> feeds;
  double feedRate = 48000.0;
  for (const auto &path : juce::StringArray::fromTokens(
           juce::SystemStats::getEnvironmentVariable("PITCHNET_PROBE_FEED", {}), ";", "")) {
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(juce::File(path)));
    juce::AudioBuffer<float> buffer;
    if (reader != nullptr) {
      buffer.setSize(2, static_cast<int>(reader->lengthInSamples));
      reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
      feedRate = reader->sampleRate;
    }
    feeds.push_back(std::move(buffer));
  }
  ProbePlayHead playHead;
  playHead.rate = feedRate;

  std::vector<std::unique_ptr<juce::AudioPluginInstance>> plugins;
  for (int i = 0; i < instances; ++i) {
    juce::String error;
    auto plugin = format.createInstanceFromDescription(*types[0], feedRate, 512, error);
    if (plugin == nullptr) {
      std::printf("instance %d failed: %s\n", i, error.toRawUTF8());
      return 1;
    }
    plugin->setPlayHead(&playHead);
    plugin->prepareToPlay(feedRate, 512);
    juce::AudioProcessor::TrackProperties properties;
    properties.name = "Probe Track " + juce::String(i + 1);
    plugin->updateTrackProperties(properties);
    plugins.push_back(std::move(plugin));
  }
  pump(500);

  std::unique_ptr<juce::AudioProcessorEditor> editor(
      plugins[0]->createEditorIfNeeded());
  if (editor == nullptr) {
    std::printf("no editor\n");
    return 1;
  }
  // PITCHNET_PROBE_SIZE="1440x900": open the editor at this size.
  {
    const auto size = juce::SystemStats::getEnvironmentVariable("PITCHNET_PROBE_SIZE", {});
    if (size.containsChar('x'))
      editor->setSize(size.upToFirstOccurrenceOf("x", false, false).getIntValue(),
                      size.fromFirstOccurrenceOf("x", false, false).getIntValue());
  }
  juce::DocumentWindow window("probe", juce::Colours::black, 0);
  window.setUsingNativeTitleBar(true);
  window.setContentNonOwned(editor.get(), true);
  window.setTopLeftPosition(40, 40);
  window.setVisible(true);
  pump(2500);
  {
    // The editor may restore its saved size once it opens: size it again.
    const auto size = juce::SystemStats::getEnvironmentVariable("PITCHNET_PROBE_SIZE", {});
    if (size.containsChar('x')) {
      editor->setSize(size.upToFirstOccurrenceOf("x", false, false).getIntValue(),
                      size.fromFirstOccurrenceOf("x", false, false).getIntValue());
      pump(800);
      std::printf("editor resized to %dx%d\n", editor->getWidth(), editor->getHeight());
    }
  }

  // PITCHNET_PROBE_SECOND_EDITOR=1: also open the second instance's window,
  // as FL does when two channels each show their plug-in.
  std::unique_ptr<juce::AudioProcessorEditor> secondEditor;
  std::unique_ptr<juce::DocumentWindow> secondWindow;
  if (plugins.size() > 1 &&
      juce::SystemStats::getEnvironmentVariable("PITCHNET_PROBE_SECOND_EDITOR", {})
          .isNotEmpty()) {
    secondEditor.reset(plugins[1]->createEditorIfNeeded());
    if (secondEditor != nullptr) {
      secondWindow = std::make_unique<juce::DocumentWindow>(
          "probe 2", juce::Colours::black, 0);
      secondWindow->setUsingNativeTitleBar(true);
      secondWindow->setContentNonOwned(secondEditor.get(), true);
      secondWindow->setTopLeftPosition(80, 80);
      secondWindow->setVisible(true);
      window.toFront(true);
      pump(1500);
      std::printf("second editor open\n");
    }
  }

  // Play the feeds through every instance like a host would, then stop.
  if (!feeds.empty()) {
    int length = 0;
    for (const auto &feed : feeds)
      length = std::max(length, feed.getNumSamples());
    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    playHead.playing = true;
    for (int pos = 0; pos < length; pos += 512) {
      playHead.samples = pos;
      for (size_t i = 0; i < plugins.size(); ++i) {
        block.clear();
        if (i < feeds.size())
          for (int ch = 0; ch < 2; ++ch) {
            const int n = std::min(512, feeds[i].getNumSamples() - pos);
            if (n > 0)
              block.copyFrom(ch, 0, feeds[i], ch, pos, n);
          }
        plugins[i]->processBlock(block, midi);
      }
      if ((pos / 512) % 64 == 0)
        pump(1);
    }
    playHead.playing = false;
    for (int k = 0; k < 20; ++k)
      for (auto &plugin : plugins) {
        block.clear();
        plugin->processBlock(block, midi);
      }
    std::printf("fed %d samples\n", length);
  }

  juce::String tree;
  listVisible(*editor, 0, tree);
  outPng.getSiblingFile(outPng.getFileNameWithoutExtension() + ".txt")
      .replaceWithText(tree);

  auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
  outPng.deleteFile();
  juce::FileOutputStream stream(outPng);
  juce::PNGImageFormat png;
  const bool ok = stream.openedOk() && png.writeImageToStream(image, stream);
  std::printf("editor %dx%d, screenshot %s\n", editor->getWidth(),
              editor->getHeight(), ok ? "saved" : "FAILED");

#if JUCE_WINDOWS
  // Optional clicks, in editor coordinates: argv[5] = "x,y;x,y;...".
  // The target child window is found inside our own window, whatever else
  // is on screen above it.
  if (argc > 5) {
    for (const auto &click : juce::StringArray::fromTokens(argv[5], ";", "")) {
      const auto xy = juce::StringArray::fromTokens(click, ",", "");
      auto top = static_cast<HWND>(window.getWindowHandle());
      auto editorPos = editor->getScreenPosition();
      POINT screenPt{editorPos.x + xy[0].getIntValue(),
                     editorPos.y + xy[1].getIntValue()};
      HWND target = top;
      for (;;) {
        POINT local = screenPt;
        ScreenToClient(target, &local);
        HWND child = ChildWindowFromPointEx(
            target, local, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (child == nullptr || child == target)
          break;
        target = child;
      }
      POINT local = screenPt;
      ScreenToClient(target, &local);
      const LPARAM lp = MAKELPARAM(local.x, local.y);
      PostMessage(target, WM_MOUSEMOVE, 0, lp);
      pump(150);
      PostMessage(target, WM_LBUTTONDOWN, MK_LBUTTON, lp);
      pump(150);
      PostMessage(target, WM_LBUTTONUP, 0, lp);
      pump(1500);
      static int clickIndex = 0;
      captureWindow(window.getWindowHandle(),
                    outPng.getSiblingFile(outPng.getFileNameWithoutExtension() +
                                          "_click" + juce::String(++clickIndex) +
                                          ".bmp"));
    }
  }
  const auto bmp = outPng.withFileExtension(".bmp");
  std::printf("window capture %s\n",
              captureWindow(window.getWindowHandle(), bmp) ? "saved" : "FAILED");
#endif

  {
    StallWatch watch;
    pump(holdMs);
    std::printf("stalls over 60 ms: %d, worst %.1f ms\n%s", watch.stalls, watch.worst,
                watch.log.toRawUTF8());
  }
#if JUCE_WINDOWS
  if (holdMs > 0)
    captureWindow(window.getWindowHandle(),
                  outPng.getSiblingFile(outPng.getFileNameWithoutExtension() + "_final.bmp"));
#endif
  // Close the second window first, while the first may still be editing its
  // track, then show what the first window does without it.
  if (secondEditor != nullptr) {
    secondWindow->clearContentComponent();
    secondEditor.reset();
    secondWindow.reset();
    pump(1500);
#if JUCE_WINDOWS
    captureWindow(window.getWindowHandle(),
                  outPng.getSiblingFile(outPng.getFileNameWithoutExtension() + "_closed.bmp"));
#endif
    std::printf("second editor closed\n");
  }
  window.clearContentComponent();
  editor.reset();
  for (auto &plugin : plugins)
    plugin->releaseResources();
  plugins.clear();
  pump(200);
  return 0;
}
