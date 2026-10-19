#include "OsirisEdit.hpp"
#include "theme.hpp"

#include "imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"

#include <ctype.h>

UITheme theme;

ImU32 colAlpha(ImU32 col, float a)
{
	ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
	c.w *= a;
	return ImGui::ColorConvertFloat4ToU32(c);
}

ImU32 colMix(ImU32 a, ImU32 b, float t)
{
	ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a);
	ImVec4 cb = ImGui::ColorConvertU32ToFloat4(b);
	ImVec4 c = ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t, ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t);
	return ImGui::ColorConvertFloat4ToU32(c);
}

namespace ui
{

void SectionHeading(const char *text)
{
	char upper[128];
	int i = 0;
	for (; text[i] && i < (int)sizeof(upper) - 1; i++)
		upper[i] = toupper(text[i]);
	upper[i] = '\0';

	ImGui::Dummy(ImVec2(0, 2));
	if (theme.fontHeading)
		ImGui::PushFont(theme.fontHeading);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme.textDim));
	ImGui::TextUnformatted(upper);
	ImGui::PopStyleColor();
	if (theme.fontHeading)
		ImGui::PopFont();
}

/** Same mapping as ImGui's SliderBehaviorCalcRatioFromValue, for the ranges this app uses */
static float sliderRatio(float v, float vMin, float vMax, float power)
{
	if (vMin == vMax)
		return 0.f;
	v = clampf(v, fminf(vMin, vMax), fmaxf(vMin, vMax));
	if (power == 0.f)
		return logf(v / vMin) / logf(vMax / vMin);
	float t = (v - vMin) / (vMax - vMin);
	if (fabsf(power - 1.f) > 1e-5f && vMin >= 0.f)
		t = powf(t, 1.f / power);
	return clampf(t, 0.f, 1.f);
}

bool Slider(const char *id, const char *label, float *v, float vMin, float vMax, const char *valueFmt, float power)
{
	ImGuiStyle &style = ImGui::GetStyle();
	ImDrawList *drawList = ImGui::GetWindowDrawList();

	// Predict where ImGui will place the slider so the track can be drawn underneath it
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float width = ImGui::CalcItemWidth();
	float height = ImGui::GetFontSize() + style.FramePadding.y * 2.f;
	ImVec2 min = pos;
	ImVec2 max = ImVec2(pos.x + width, pos.y + height);

	bool hovered = ImGui::IsMouseHoveringRect(min, max);
	drawList->AddRectFilled(min, max, hovered ? theme.controlHover : theme.control, style.FrameRounding);

	// Accent fill up to the current value
	float t = sliderRatio(*v, vMin, vMax, power);
	float fillX = min.x + (max.x - min.x) * t;
	if (fillX > min.x + 1.f)
	{
		int corners = (fillX >= max.x - style.FrameRounding) ? ~0 : (ImGuiCorner_TopLeft | ImGuiCorner_BotLeft);
		drawList->AddRectFilled(min, ImVec2(fillX, max.y), colAlpha(theme.accent, theme.modern ? 0.22f : 0.35f), style.FrameRounding, corners);
	}

	// The real slider handles the mouse; its frame and grab are invisible because the track above replaces them
	ImVec4 clear = ImVec4(0, 0, 0, 0);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, clear);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, clear);
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, clear);
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, clear);
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, clear);
	bool changed = ImGui::SliderFloat(id, v, vMin, vMax, "", power);
	ImGui::PopStyleColor(5);
	bool active = ImGui::IsItemActive();

	// Value marker at the end of the fill, unless it would sit under the value text
	char value[64];
	snprintf(value, sizeof(value), valueFmt, *v);
	ImVec2 valueSize = ImGui::CalcTextSize(value);
	float valueLeft = max.x - style.FramePadding.x - valueSize.x - 4.f;
	{
		float t2 = sliderRatio(*v, vMin, vMax, power);
		float x = clampf(min.x + (max.x - min.x) * t2, min.x + 3.f, max.x - 3.f);
		if (x < valueLeft || x > max.x - style.FramePadding.x + 2.f)
			drawList->AddRectFilled(ImVec2(x - 1.f, min.y + 4.f), ImVec2(x + 1.f, max.y - 4.f), active ? theme.text : theme.accent, 1.f);
	}

	// Text: label on the left, value on the right, unless ctrl+click turned it into a text box
	if (!ImGui::IsItemActive() || !ImGui::GetIO().KeyCtrl)
	{
		ImVec2 rmin = ImGui::GetItemRectMin();
		ImVec2 rmax = ImGui::GetItemRectMax();
		float textY = rmin.y + (rmax.y - rmin.y - ImGui::GetFontSize()) * 0.5f;
		drawList->PushClipRect(rmin, rmax, true);
		drawList->AddText(ImVec2(rmin.x + style.FramePadding.x + 4.f, textY), theme.textDim, label);
		drawList->AddText(ImVec2(rmax.x - style.FramePadding.x - valueSize.x, textY), theme.text, value);
		drawList->PopClipRect();
	}
	return changed;
}

bool Segmented(const char *id, const char **labels, int count, int *selected)
{
	bool changed = false;
	ImGui::PushID(id);
	for (int i = 0; i < count; i++)
	{
		bool on = (*selected == i);
		ImU32 bg = on ? colAlpha(theme.accent, 0.20f) : theme.control;
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(bg));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(on ? colAlpha(theme.accent, 0.28f) : theme.controlHover));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.35f)));
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(on ? theme.accent : theme.textDim));
		if (i > 0)
			ImGui::SameLine(0, 2);
		ImGui::PushID(i);
		if (ImGui::Button(labels[i]))
		{
			changed = (*selected != i);
			*selected = i;
		}
		ImGui::PopID();
		ImGui::PopStyleColor(4);
	}
	ImGui::PopID();
	return changed;
}

bool Tabs(const char **labels, int count, int *selected)
{
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	ImGuiStyle &style = ImGui::GetStyle();
	bool changed = false;
	float padX = 14.f;
	float height = ImGui::GetFontSize() + style.FramePadding.y * 2.f + 4.f;
	ImGui::PushID("tabs");
	for (int i = 0; i < count; i++)
	{
		ImVec2 textSize = ImGui::CalcTextSize(labels[i]);
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImVec2 size = ImVec2(textSize.x + padX * 2.f, height);
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("tab", size))
		{
			changed = (*selected != i);
			*selected = i;
		}
		ImGui::PopID();
		bool on = (*selected == i);
		bool hovered = ImGui::IsItemHovered();
		ImU32 textCol = on ? theme.text : (hovered ? colMix(theme.textDim, theme.text, 0.6f) : theme.textDim);
		if (hovered && !on)
			drawList->AddRectFilled(pos, pos + size, colAlpha(theme.control, 0.6f), style.FrameRounding);
		drawList->AddText(ImVec2(pos.x + padX, pos.y + (height - textSize.y) * 0.5f - 1.f), textCol, labels[i]);
		if (on)
		{
			ImVec2 a = ImVec2(pos.x + padX - 4.f, pos.y + size.y - 3.f);
			ImVec2 b = ImVec2(pos.x + size.x - padX + 4.f, pos.y + size.y - 1.f);
			if (theme.modern)
				drawList->AddRectFilled(a - ImVec2(0, 3), b + ImVec2(0, 1), colAlpha(theme.accent, 0.18f), 3.f);
			drawList->AddRectFilled(a, b, theme.accent, 1.f);
		}
		ImGui::SameLine(0, 2);
	}
	ImGui::NewLine();
	ImGui::PopID();
	return changed;
}

void GlowPolyline(ImDrawList *drawList, const ImVec2 *points, int count, ImU32 col, float thickness)
{
	if (count < 2)
		return;
	if (theme.modern)
	{
		drawList->AddPolyline(points, count, colAlpha(col, 0.07f), false, thickness + 7.f, true);
		drawList->AddPolyline(points, count, colAlpha(col, 0.16f), false, thickness + 3.f, true);
	}
	drawList->AddPolyline(points, count, col, false, thickness, true);
}

void FillUnderCurve(ImDrawList *drawList, const ImVec2 *points, int count, float baseY, ImU32 colCurve, ImU32 colBase)
{
	if (count < 2)
		return;
	const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
	// One quad per segment, 4 vertices / 6 indices each
	drawList->PrimReserve((count - 1) * 6, (count - 1) * 4);
	for (int i = 0; i < count - 1; i++)
	{
		ImVec2 p0 = points[i];
		ImVec2 p1 = points[i + 1];
		ImDrawIdx idx = (ImDrawIdx)drawList->_VtxCurrentIdx;
		drawList->PrimWriteIdx(idx);
		drawList->PrimWriteIdx(idx + 1);
		drawList->PrimWriteIdx(idx + 2);
		drawList->PrimWriteIdx(idx);
		drawList->PrimWriteIdx(idx + 2);
		drawList->PrimWriteIdx(idx + 3);
		drawList->PrimWriteVtx(p0, uv, colCurve);
		drawList->PrimWriteVtx(p1, uv, colCurve);
		drawList->PrimWriteVtx(ImVec2(p1.x, baseY), uv, colBase);
		drawList->PrimWriteVtx(ImVec2(p0.x, baseY), uv, colBase);
	}
}

void GradientRect(ImDrawList *drawList, ImVec2 a, ImVec2 b, ImU32 colTop, ImU32 colBottom)
{
	drawList->AddRectFilledMultiColor(a, b, colTop, colTop, colBottom, colBottom);
}

} // namespace ui
