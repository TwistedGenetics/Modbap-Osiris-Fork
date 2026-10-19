#pragma once

#include "imgui.h"

/** Colours and fonts used by the custom-drawn widgets (waveforms, grids, tabs, sliders).
Filled in by refreshStyle() in ui.cpp for whichever colour scheme is active. */
struct UITheme
{
	bool modern;      // the "Modern" scheme gets glow and gradient effects
	ImU32 accent;     // main highlight (lines, active controls)
	ImU32 accent2;    // secondary highlight, used for gradients
	ImU32 text;
	ImU32 textDim;
	ImU32 display;    // background of waveform/graph displays
	ImU32 displayHi;  // background of a selected display cell
	ImU32 grid;       // faint grid lines
	ImU32 gridStrong; // centre line, major grid lines
	ImU32 control;    // slider/button background
	ImU32 controlHover;
	ImU32 outline;    // thin border around displays
	ImFont *fontHeading;
};

extern UITheme theme;

/** Returns `col` with its alpha multiplied by `a` */
ImU32 colAlpha(ImU32 col, float a);
/** Linear blend between two colours */
ImU32 colMix(ImU32 a, ImU32 b, float t);

namespace ui
{
/** Small uppercase label above a group of controls */
void SectionHeading(const char *text);

/** Horizontal slider with an accent fill, the label on the left and the value on the right.
`valueFmt` is a printf format for the value, e.g. "%.2f dB". `power` matches ImGui::SliderFloat. */
bool Slider(const char *id, const char *label, float *v, float vMin, float vMax, const char *valueFmt, float power = 1.0f);

/** Row of joined buttons where exactly one is selected */
bool Segmented(const char *id, const char **labels, int count, int *selected);

/** Page tabs drawn as text with an underline on the selected one */
bool Tabs(const char **labels, int count, int *selected);

/** Polyline with a soft glow (the glow is only drawn in the Modern scheme) */
void GlowPolyline(ImDrawList *drawList, const ImVec2 *points, int count, ImU32 col, float thickness);

/** Gradient fill between a curve and a horizontal baseline. `colCurve` at the curve fading to `colBase` at the baseline. */
void FillUnderCurve(ImDrawList *drawList, const ImVec2 *points, int count, float baseY, ImU32 colCurve, ImU32 colBase);

/** Call straight after BeginPopupModal() returns true: centres the dialog in the window, re-centring whenever its size
changes (dialogs that grow stay centred) and always keeping it on screen */
void CenterModal();

/** Vertical gradient rectangle with optional rounding on the top corners */
void GradientRect(ImDrawList *drawList, ImVec2 a, ImVec2 b, ImU32 colTop, ImU32 colBottom);
} // namespace ui
