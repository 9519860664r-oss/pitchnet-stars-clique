#pragma once

#include "Project.h"

/** Places a captured project on the host timeline: pads (or trims) its
    audio by whole samples and every per-frame array and note by whole
    frames, so project time equals host time. Used by the editor and by
    instances that capture with no window open. Returns false when nothing
    was moved (no audio yet, or already at that offset). */
namespace ProjectTimelineOffset {
bool apply(Project &project, double timelineOffsetSeconds);
}
