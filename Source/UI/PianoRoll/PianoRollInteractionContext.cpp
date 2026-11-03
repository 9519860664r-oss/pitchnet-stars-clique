#include "PianoRollInteractionContext.h"
#include "../PianoRollComponent.h"

PianoRollInteractionContext::PianoRollInteractionContext(
    PianoRollComponent &component)
    : project(component.project), undoManager(component.undoManager),
      coordMapper(component.coordMapper), pitchEditor(component.pitchEditor),
      boxSelector(component.boxSelector), noteSplitter(component.noteSplitter),
      pitchToolHandles(component.pitchToolHandles),
      pitchToolController(component.pitchToolController),
      viewState(component.viewState),
      hoveredPitchToolHandle(component.hoveredPitchToolHandle),
      pixelsPerSecond(component.pixelsPerSecond),
      pixelsPerSemitone(component.pixelsPerSemitone),
      scrollX(component.scrollX),
      snapToSemitoneDrag(component.snapToSemitoneDrag),
      pitchReferenceHz(component.pitchReferenceHz),
      selectedScaleMode(component.selectedScaleMode),
      selectedScaleRootNote(component.selectedScaleRootNote),
      lastDragRepaintTime(component.lastDragRepaintTime),
      onNoteSelected(component.onNoteSelected),
      onPitchEdited(component.onPitchEdited),
      onPitchEditFinished(component.onPitchEditFinished),
      onPitchPreviewRenderRequested(component.onPitchPreviewRenderRequested),
      onPitchEditCommitted(component.onPitchEditCommitted),
      onNoteDragAudition(component.onNoteDragAudition),
      onNoteDragAuditionFinished(component.onNoteDragAuditionFinished),
      onLoopRangeChanged(component.onLoopRangeChanged),
      onReinterpolateUV(component.onReinterpolateUV), componentRef(component)
{
}

void PianoRollInteractionContext::repaint() { componentRef.repaint(); }

void PianoRollInteractionContext::setMouseCursor(
    const juce::MouseCursor &cursorType)
{
  componentRef.setMouseCursor(cursorType);
}

float PianoRollInteractionContext::midiToY(float midiNote) const
{
  return componentRef.midiToY(midiNote);
}

float PianoRollInteractionContext::yToMidi(float y) const
{
  return componentRef.yToMidi(y);
}

float PianoRollInteractionContext::timeToX(double time) const
{
  return componentRef.timeToX(time);
}

double PianoRollInteractionContext::xToTime(float x) const
{
  return componentRef.xToTime(x);
}

double PianoRollInteractionContext::snapTimeToTimelineGrid(
    double timeSeconds) const
{
  return componentRef.snapTimeToTimelineGrid(timeSeconds);
}

double PianoRollInteractionContext::noteTimeGridSeconds() const
{
  return componentRef.timelineDisplayMode == TimelineDisplayMode::Beats
             ? componentRef.getTimelineGridSeconds()
             : 0.0;
}

bool PianoRollInteractionContext::toggleMuteSelectedNotes()
{
  return componentRef.toggleMuteSelectedNotes();
}

bool PianoRollInteractionContext::isCanvasPoint(
    const juce::MouseEvent &e) const
{
  return componentRef.isCanvasPoint(e);
}

Note *PianoRollInteractionContext::findNoteAt(float x, float y)
{
  return componentRef.findNoteAt(x, y);
}

std::vector<Note *> PianoRollInteractionContext::getSelectedNotes() const
{
  return componentRef.getSelectedNotes();
}

void PianoRollInteractionContext::updatePitchToolHandlesFromSelection()
{
  componentRef.updatePitchToolHandlesFromSelection();
}

void PianoRollInteractionContext::invalidateWaveformCache()
{
  componentRef.invalidateWaveformCache();
}

void PianoRollInteractionContext::invalidateBasePitchCache()
{
  componentRef.invalidateBasePitchCache();
}

void PianoRollInteractionContext::updatePitchDrawingConfirmation()
{
  componentRef.updatePitchDrawingConfirmationPopup();
}
