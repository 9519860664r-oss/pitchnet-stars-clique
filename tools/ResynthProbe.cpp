// ResynthProbe - offline check of the analysis -> resynthesis chain.
//
// Loads a WAV the same way the standalone app does, raises every note by a
// fixed number of semitones (so the whole take goes through the synthesiser),
// renders it, and writes the result. Compare input and output loudness
// envelopes to measure any timing offset the chain adds:
//
//   ResynthProbe in.wav out.wav [semitones=1] [psola]
//
// out.wav is at PitchNet's analysis rate (44.1 kHz), as is
// out.original.wav - the input after PitchNet's own resampling, so the two
// can be compared sample for sample.

#include "../Source/JuceHeader.h"
#include "../Source/Audio/EditorController.h"
#include "../Source/Audio/Vocoder.h"
#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "../Source/Utils/PlatformPaths.h"

#include <atomic>
#include <cstdio>

namespace {

bool waitFor(const std::atomic<bool> &flag, double timeoutSeconds) {
  const auto start = juce::Time::getMillisecondCounterHiRes();
  while (!flag.load()) {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    if (juce::Time::getMillisecondCounterHiRes() - start >
        timeoutSeconds * 1000.0)
      return false;
  }
  return true;
}

bool writeWav(const juce::File &file, const juce::AudioBuffer<float> &buffer,
              double sampleRate) {
  file.deleteFile();
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
  if (stream == nullptr)
    return false;
  std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(
      stream.get(), sampleRate,
      static_cast<unsigned int>(buffer.getNumChannels()), 32, {}, 0));
  if (writer == nullptr)
    return false;
  stream.release(); // the writer owns it now
  return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::printf("usage: ResynthProbe in.wav out.wav [semitones=1] [psola]\n");
    return 2;
  }
  const juce::File input(juce::String::fromUTF8(argv[1]));
  const juce::File output(juce::String::fromUTF8(argv[2]));
  const float semitones = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 1.0f;
  const bool usePsola = argc > 4 && juce::String(argv[4]) == "psola";

  juce::ScopedJuceInitialiser_GUI juceInit;
  EditorController controller(false);
  controller.setSynthesisEngineType(usePsola ? SynthesisEngineType::Psola
                                             : SynthesisEngineType::Vocoder);
  // The app loads the detectors at startup; do the same, synchronously.
  controller.reloadInferenceModels(false);

  std::atomic<bool> loaded{false};
  std::atomic<bool> failed{false};
  controller.loadAudioFileAsync(
      input, [](double, const juce::String &) {},
      [&](const juce::AudioBuffer<float> &) { loaded = true; },
      [&]() { failed = true; loaded = true; });
  if (!waitFor(loaded, 600.0) || failed) {
    std::printf("load/analysis failed\n");
    return 1;
  }

  auto *project = controller.getProject();
  if (project == nullptr) {
    std::printf("no project\n");
    return 1;
  }
  if (auto *vocoder = controller.getVocoder(); vocoder && !vocoder->isLoaded())
    if (!vocoder->loadModel(PlatformPaths::getVocoderModelFile())) {
      std::printf("vocoder load failed\n");
      return 1;
    }

  auto &audio = project->getAudioData();
  const double rate = audio.sampleRate;
  writeWav(output.getSiblingFile(output.getFileNameWithoutExtension() +
                                 ".original.wav"),
           audio.originalWaveform.getNumSamples() > 0 ? audio.originalWaveform
                                                       : audio.waveform,
           rate);

  {
    // Per-frame analysis dump: what the vocoder will be told about pitch.
    juce::String csv("frame,time,rawF0,cleanedF0,denseF0,f0,voiced,vad\n");
    const size_t frames = audio.f0.size();
    auto at = [](const std::vector<float> &v, size_t i) {
      return i < v.size() ? v[i] : -1.0f;
    };
    for (size_t i = 0; i < frames; ++i)
      csv << (int)i << "," << (double)i * HOP_SIZE / SAMPLE_RATE << ","
          << at(audio.rawF0, i) << "," << at(audio.cleanedF0, i) << ","
          << at(audio.denseF0, i) << "," << at(audio.f0, i) << ","
          << (i < audio.voicedMask.size() && audio.voicedMask[i] ? 1 : 0) << ","
          << (i < audio.vadMask.size() && audio.vadMask[i] ? 1 : 0) << "\n";
    output.getSiblingFile(output.getFileNameWithoutExtension() + ".f0.csv")
        .replaceWithText(csv);
  }

  int edited = 0;
  for (auto &note : project->getNotes()) {
    if (note.isRest())
      continue;
    note.setMidiNote(note.getMidiNote() + semitones);
    note.markDirty();
    note.markSynthDirty();
    ++edited;
  }
  PitchCurveProcessor::rebuildBaseFromNotes(*project);
  for (auto &note : project->getNotes())
    if (!note.isRest())
      project->markNoteEditDirtyRange(note.getStartFrame(), note.getEndFrame());
  std::printf("analysed: %d notes edited (+%.2f st), %s, %.0f Hz\n", edited,
              semitones, usePsola ? "PSOLA" : "vocoder", rate);

  std::atomic<bool> pendingRerun{false};
  std::atomic<bool> synthDone{false};
  std::atomic<bool> synthOk{false};
  controller.resynthesizeIncrementalAsync(
      *project, [](const juce::String &) {},
      [&](bool ok) { synthOk = ok; synthDone = true; }, pendingRerun, false);
  if (!waitFor(synthDone, 900.0) || !synthOk) {
    std::printf("synthesis failed\n");
    return 1;
  }

  if (!writeWav(output, audio.waveform, rate)) {
    std::printf("write failed\n");
    return 1;
  }
  std::printf("wrote %s\n", output.getFullPathName().toRawUTF8());
  controller.requestShutdown();
  return 0;
}
