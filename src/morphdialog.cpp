// "Morph Between Keyframes" dialog. The morph itself is in keyframes.cpp / wtprocess.cpp.

#include "OsirisEdit.hpp"
#include "keyframes.hpp"
#include "theme.hpp"

#include "imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"

namespace {

bool openRequested = false;
bool closeRequested = false;
int morphChoice = wt::CROSSFADE;
int curveChoice = wt::LINEAR;

/** A small picture of a curve */
void curveIcon(wt::Curve c, bool selected) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 a = ImGui::GetCursorScreenPos();
	ImVec2 size = ImVec2(34, 22);
	ImGui::Dummy(size);
	dl->AddRectFilled(a, a + size, theme.display, 3.f);
	ImVec2 pts[16];
	for (int i = 0; i < 16; i++) {
		float t = i / 15.f;
		pts[i] = ImVec2(a.x + 3.f + t * (size.x - 6.f), a.y + size.y - 3.f - wt::curve(c, t) * (size.y - 6.f));
	}
	dl->AddPolyline(pts, 16, selected ? theme.accent : theme.textDim, false, 1.5f, true);
}

std::string waveRange(int first, int last) {
	return first == last ? std::to_string(first + 1) : std::to_string(first + 1) + "-" + std::to_string(last + 1);
}

} // namespace

void morphOpenDialog() {
	openRequested = true;
}

void morphCloseDialog() {
	closeRequested = true;
}

void morphRenderDialog() {
	if (openRequested) {
		ImGui::OpenPopup("Morph Between Keyframes");
		openRequested = false;
	}
	ImGui::SetNextWindowContentWidth(640);
	ui::LimitModalHeight();
	if (!ImGui::BeginPopupModal("Morph Between Keyframes", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		closeRequested = false;
		return;
	}
	ui::CenterModal();

	int selStart = mini(selectedId, lastSelectedId), selEnd = maxi(selectedId, lastSelectedId);
	std::vector<int> keys = editorKeyframes();
	std::vector<wt::Segment> segments = wt::segmentsFor(keys);

	// What will be morphed
	if (selStart < selEnd)
		ImGui::Text("Selection: waves %d-%d", selStart + 1, selEnd + 1);
	else
		ImGui::Text("Whole table");
	if (!keys.empty()) {
		std::string list;
		for (size_t i = 0; i < keys.size(); i++)
			list += (i ? ", " : "") + std::to_string(keys[i] + 1);
		ImGui::TextDisabled("Keyframes: %s", list.c_str());
	}

	if (segments.empty()) {
		ImGui::Dummy(ImVec2(0, 4));
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textDim),
			"There's nothing to morph yet. Mark at least two keyframes with waves between them (right-click a wave > "
			"Keyframe, or select it and press K), or shift-click to select a range of waves: its first and last waves "
			"are used as keyframes.");
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, 4));
		if (ImGui::Button("Close", ImVec2(120, 0)) || closeRequested) {
			closeRequested = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
		return;
	}

	// Segments
	int total = 0;
	for (const wt::Segment &seg : segments) {
		ImGui::Bullet();
		ImGui::Text("%s: wave %d to wave %d", waveRange(seg.from + 1, seg.to - 1).c_str(), seg.from + 1, seg.to + 1);
		total += seg.count();
	}

	// Options
	ui::SectionHeading("Morph");
	ImGui::RadioButton("Crossfade", &morphChoice, wt::CROSSFADE);
	ImGui::SameLine(150);
	ImGui::TextDisabled("blend sample by sample");
	ImGui::RadioButton("Spectral", &morphChoice, wt::SPECTRAL);
	ImGui::SameLine(150);
	ImGui::TextDisabled("blend harmonics; keeps its level between out-of-phase waves");

	ui::SectionHeading("Curve");
	static const char *curveNames[] = {"Linear", "Ease In", "Ease Out", "S-curve"};
	for (int c = 0; c < 4; c++) {
		if (c)
			ImGui::SameLine(0, 18);
		ImGui::BeginGroup();
		curveIcon((wt::Curve)c, curveChoice == c);
		ImGui::SameLine();
		ImGui::RadioButton(curveNames[c], &curveChoice, c);
		ImGui::EndGroup();
	}

	// Before and after
	std::vector<float> before = editorWavesAsHeard();
	std::vector<float> after = editorKeyframePreview((wt::MorphMode)morphChoice, (wt::Curve)curveChoice);
	std::vector<unsigned char> marksBefore(BANK_LEN, ui::STRIP_NORMAL), marksAfter(BANK_LEN, ui::STRIP_NORMAL);
	for (int k : keys) {
		marksBefore[k] = ui::STRIP_KEYFRAME;
		marksAfter[k] = ui::STRIP_KEYFRAME;
	}
	for (const wt::Segment &seg : segments)
		for (int w = seg.from + 1; w < seg.to; w++)
			marksAfter[w] = ui::STRIP_CHANGED;
	ui::SectionHeading("Now");
	ui::WaveStrip("##morphBefore", before.data(), BANK_LEN, marksBefore.data());
	ui::SectionHeading("After morphing");
	ui::WaveStrip("##morphAfter", after.data(), BANK_LEN, marksAfter.data());
	ImGui::TextDisabled("%d %s will be replaced, with their effects turned off. Keyframes stay as they are.", total, total == 1 ? "wave" : "waves");

	ImGui::Dummy(ImVec2(0, 2));
	ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.22f)));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.32f)));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme.modern ? theme.accent : theme.text));
	bool apply = ImGui::Button("Morph", ImVec2(130, 0));
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	bool cancel = ImGui::Button("Cancel", ImVec2(130, 0));
	if (apply)
		editorKeyframeMorph((wt::MorphMode)morphChoice, (wt::Curve)curveChoice);
	if (apply || cancel || closeRequested) {
		closeRequested = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}
