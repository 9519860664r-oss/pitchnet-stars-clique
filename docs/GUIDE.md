# Quick start guide

**English** · [Русский](GUIDE.ru.md) · Full manual: [PDF (EN)](PitchNet-StarsClique-Manual-EN.pdf), [PDF (RU)](PitchNet-StarsClique-Manual-RU.pdf)

## 1. Install

1. Download **PitchNet-StarsClique-1.0.0-Setup.exe** from [Releases](https://github.com/9519860664r-oss/pitchnet-stars-clique/releases/latest).
2. Close your DAW, run the installer, keep the VST3 component selected.
3. Rescan plug-ins. The plug-in is called **PitchNet (Stars Clique ver)** (the host lists Session Loops as its maker).

| What | Where |
|---|---|
| VST3 plug-in | `C:\Program Files\Common Files\VST3\Stars Clique\` |
| Standalone app | `C:\Program Files\Stars Clique\PitchNet\` |
| Neural models | `C:\ProgramData\Session Loops\PitchNet\models\` |
| Settings, skin, saved scales | `%APPDATA%\PitchNet\` |

## 2. Get the audio in

**FL Studio and other hosts without ARA**

1. Insert PitchNet on the mixer channel of each vocal, before reverb and delay.
2. Open any PitchNet window. The track list shows every instance.
3. Click the pink **Transfer** ring of each track to record.
4. Play the song. Pause or stop: the takes are finished and analysed.

Playing again from any point re-records only that section. A loop jump also finishes the take.

**REAPER (ARA)**: add PitchNet as an FX on an item or track. The audio is available at once. For groups across tracks REAPER lets PitchNet read the other tracks only after the song has played: if the window says *Press Play once*, start playback for a moment.

## 3. Edit

| Do this | To |
|---|---|
| Drag a note up/down or sideways | Change pitch or timing (the first movement locks the axis) |
| Double-click a note | Snap it to the key |
| Alt + drag | Move without the grid |
| Type into the Note Inspector | Set pitch (`C3`, `+2`, `-30 ct`), vibrato, drift, formant, level |
| Blue **Edit** pill | Edit that track too (any number at once) |
| White **Ghost** pill | Show a track as grey notes for reference |
| Right-click the editor | Quantize Time, Align to Reference, Leveling, Sibilants (show, set, remove, balance), Restore Original, selection tools |
| Group selected, then **Unison** / **Timing** | Doubles follow the lead's pitch / timing (strength sliders) |
| Right-click the time ruler | Key change here, replace ranges |
| Key button | Key, custom scales with cents, A4 400–480 Hz |

## 4. Navigate

| Input | Result |
|---|---|
| Wheel / Shift + wheel | Scroll vertically / horizontally |
| Ctrl + Alt + wheel, pinch | Zoom both axes |
| Ctrl + Shift + drag | Hand |
| Ctrl + Alt + drag | Zoom tool |
| Drag the time ruler up/down | Zoom time |
| Double-click a scroll bar | Fit all |

## 5. Shortcuts

`1` `2` `3` `4` tools · `Space` play/pause · `Esc` stop · `Ctrl+Z` / `Ctrl+Y` undo/redo · `Ctrl+A` select all · `Ctrl+C` / `Ctrl+X` / `Ctrl+V` copy/cut/paste notes · `Up` / `Down` semitone · `Shift+Up` / `Shift+Down` octave · `Delete` mute notes

## 6. More

- Logo menu: change skin (Stars Clique / Classic), Export MIDI, user manual, credits.
- Problems? See the Troubleshooting chapter of the manual or open an [issue](https://github.com/9519860664r-oss/pitchnet-stars-clique/issues).
