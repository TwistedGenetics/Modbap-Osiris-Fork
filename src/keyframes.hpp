#pragma once

/** Morph Between Keyframes on the editor's wavetable. No UI here (the dialog is in morphdialog.cpp), so the tests can
run it directly. */

#include "wtprocess.hpp"

#include <vector>

/** Keyframes for the current selection (see wt::keyframesFor) */
std::vector<int> editorKeyframes();

/** The editor's waves as heard (after effects), BANK_LEN x 256 */
std::vector<float> editorWavesAsHeard();

/** What the table would look like after the morph (as heard) */
std::vector<float> editorKeyframePreview(wt::MorphMode mode, wt::Curve c);

/** Applies the morph: waves between keyframes get the morphed samples with their effects turned off; keyframes are
untouched. One undo step. Returns how many waves changed. */
int editorKeyframeMorph(wt::MorphMode mode, wt::Curve c);

/** Toggles the keyframe mark on the selected waves (all on, unless they're all on already). One undo step. */
void editorToggleKeyframes();
