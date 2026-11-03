# Changelog

## 1.0.0 — Stars Clique Edition (2026-10-29)

The first release of PitchNet Stars Clique Edition: changes by Goth Angel (Stars Clique) on top of
[PitchNet](https://github.com/SessionLoops/PitchNet) 0.7.2 by Session Loops and contributors, which is
based on [HachiTune](https://github.com/KCKT0112/HachiTune). Everything below is new in this edition;
the pitch detection, note segmentation, vocoder and editor they are built on are the original authors' work.

### Tracks and multitrack editing
- REAPER: Edit on any track edits it together with the active one; its clips appear on the timeline as editable windows (the view switches to Track).
- REAPER: the track the window shows can be switched off or made a ghost too, as in hosts without ARA. Group and Unison/Timing changes are saved into the REAPER project once they stop, not on every slider step.
- The Up/Down arrow keys move the selected notes of every track being edited, not only this window's.
- Track list in every window: a window mark for this window's track, blue **Edit** pill, white **Ghost** pill, pink **Transfer** ring. Open by default.
- Edit is an on/off switch: any number of tracks can be edited at once. The window's own track can be switched off or made a ghost.
- Select tracks like in a DAW: click, Ctrl+click to add or remove, Shift+click for a range. Edit, Ghost and Transfer on a selected track change every selected track at once.
- Edit another track while its own window is open; changes appear in every window at once.
- Ghost notes are grey waveform blobs, lighter the closer they are to the key. Double-click a ghost note to swap the two tracks' roles.
- Highlight track affiliation: other tracks turn grey while you click a note.
- Editing Mix fader, Spread Unison Tracks, Use DAW Track Color.

### Lead and doubles
- Groups of a lead and its doubles: select the tracks and press Group selected (click a D to make that track the lead), or Find doubles suggests groups from the tracks' notes. Ungroup takes every selected track out. The lead has a star, doubles a "D", each group its own colour.
- Unison switch per group: the doubles take the lead's notes, by a strength of 0-100 %, live, even with their windows closed. Each double keeps what makes it a separate singer: its own vibrato, jitter, slides and note transitions. It takes only where the lead's notes sit and how they drift, and the lead and double are compared at the moments they sing the same sound (as Timing finds it), so a double sung a little early or late still gets the right note. The correction changes smoothly from note to note, at the double's own transitions: no jumps, spikes or clicks between notes. Octave doubles stay in their octave. The last few cents stay the double's own, so stacked doubles sound like people, not copies. The lead's sibilants (an "s", the "th" of "the") never set a double's pitch. Where the lead is silent, or a double note is more than about 3.5 semitones from it (another syllable, not a missed note), the double is left alone. Off, or out of the group: back to their own pitch.
- This window's own track is now marked with a small window instead of the star.
- Timing switch per group: the doubles line up with the lead syllable by syllable. They are matched by the shape of the sound (spectrum, loudness, onsets), not just by note starts, and the sound warps inside a note where a syllable is longer or shorter than the lead's. Differences under about 23 ms stay, so the doubles still sound sung. Strength 0-100 %, at most 300 ms of shift, live, even with their windows closed; sibilants move without stretching. Timing is lined up first, then the pitch. Off puts every note back on its own timing.
- REAPER (ARA) has the same lead and doubles as hosts without ARA: groups (Group selected, Find doubles, Ungroup, make lead), Unison and Timing across tracks, live, saved with the REAPER project. The group's clips are analysed by themselves once REAPER allows reading them (after the song has played once).
- Rendering uses the fast GPU: unless you picked a device yourself (parameters panel), every track renders on the GPU Windows treats as high-performance (the CPU when there is no discrete GPU). The editor window used to render on the CPU, or on a laptop's integrated graphics, while closed tracks used the fast GPU: a whole-song render of the lead took 30 s instead of 14 s, and rendering on the graphics chip that draws the screen froze the window for a second at a time.
- Doubles follow the lead much sooner: they start rendering about 0.2 s after a lead edit, together with the lead, instead of after its render. Editing in a group no longer freezes the window while whole projects are copied, and memory no longer grows with every edit of a lead in unison.

### Sibilants
- Sibilants (s, sh, f, t, k and breaths) are found at the start and end of every note and drawn hatched (Show Sibilants). A breath can be a hatched note of its own.
- A note moves with its sibilants, but they keep their original sound when the pitch changes (short t and k too) and their length when the note is moved or stretched: "sweet" stretched sounds s-weeee-t.
- Quantize Time and Align to Reference place a note's musical start after its sibilant, not at the "s".
- Right-click the editor, Sibilants: Show Sibilants, find them again, set a sibilant from the note start to the click or from the click to the note end, remove them (they are then pitch-shifted too), and Sibilant Balance from -100 % (sibilants muted) to +100 % (only sibilants).

### Recording (Transfer) in hosts without ARA
- Arm any number of tracks from one window and record them all in one pass.
- Pause or stop finishes every armed take; the jump back at a loop end finishes the take too.
- Punch in/out during playback; re-recording from any point replaces only that section.
- Replace Ranges on the time ruler: show, drag the edges, set to notes, reset.

### Editing
- Main tool: axis locked by the first movement, double-click snaps to the scale (Alt keeps the offset), Alt drags without the grid; moving to another beat keeps the offset from the beat.
- Note Inspector: pitch as a note name, cents or Hz, modulation, drift, formant, amplitude and Note Off; "–" when the selected notes differ.
- Muted notes show only their outline.
- Copy, cut and paste notes with their audio, inside a track and between tracks.
- Selection: Ctrl+click, Shift+click range, Ctrl+Shift box, by pitch on the keyboard (double-click for every octave), Invert, Restore Last Selection, Select Special.
- Restore Original by category, Whole Track: Undo All Changes, Add Random Deviations.
- Soft and hard note boundaries; Pitch Lines background.
- Arrow keys move notes by a semitone, Shift+arrows by an octave.

### Macros
- Correct Pitch with "don't touch manual edits" and remembered values per note.
- Correct Pitch opens with the value applied to the selected notes, or 0 when none was applied: cancelling, undo and Restore Original no longer leave 100 behind.
- Correct Pitch: with Snap to Scale on, notes go to the nearest note of the key in force at each note (key changes, custom scales with cents); off, to the nearest semitone. Both follow the A4 reference pitch.
- Quantize Time: groove reference Auto, 1/1 to 1/32 with triplets, or another track.
- Align to Reference: line backing vocals up with the lead (timing, optionally pitch and level) and tune to it in unison or an interval.
- Leveling.

### Key and tuning
- Rows follow the key: notes of the key are light rows, the rest dark (like a scale highlight in a DAW piano roll), key changes included. Custom scales show a dashed line at each detuned note.
- Note colour shows how well a note hits the key: blue exactly on a note of the key, through violet and pink to red half a semitone or more away; a note outside the key is red. The overview strip uses the same colours.
- One key shared by every track and window.
- Key-change markers on the time ruler.
- Reference pitch A4 from 400 to 480 Hz.
- Custom scales: choose notes, detune each in cents, name and save them, use them for snapping and Correct Pitch.

### Navigation
- Wheel scrolls vertically, Shift+wheel horizontally, Ctrl+Alt+wheel or pinch zooms both axes.
- Ctrl+Shift+drag hand, Ctrl+Alt+drag zoom tool, drag the ruler to zoom time.
- Double-click a scroll bar to fit all; Ctrl+Shift+double-click a note to zoom to it; Ctrl+Alt+double-click for the previous zoom.
- Wider zoom range on both axes. Auto-scroll pauses while notes are selected or after manual scrolling.

### Look
- New **Stars Clique** skin: deep violet glass, vector icons, piano-style rows shaded by key. The **Classic** skin is still available.
- Logo menu: change skin (remembered), export MIDI, user manual and releases, credits.
- Multi-track MIDI export (one channel per track).

### Sound and fixes
- Fixed a resynthesis timing offset of about 11 ms.
- Breaths, rasp and other unpitched sounds stay as in the original.
- Fewer octave errors on rough voice.
- FL Studio: every PitchNet instance is listed with its mixer channel name.

### Other
- The standalone app checks this repository's GitHub releases for updates.
- Credits window with a link to the releases page.
- User manuals in English and Russian (PDF).
