// "Generate Wavetable" dialog: drum & bass / jungle tables from generator.cpp, auditioned live, sent to the editor or
// straight onto the SD Card page.

#include "OsirisEdit.hpp"
#include "cardpage.hpp"
#include "generator.hpp"
#include "theme.hpp"

#include "imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"

#include <SDL.h>

namespace {

bool openRequested = false;
bool closeRequested = false;
bool isOpen = false;
bool lockSeed = false;
gen::Params params = gen::defaults(gen::REESE);
gen::Params generatedFor;
bool generated = false;
std::vector<float> table;
Bank previewBank;
float keyframesSlider = 4.f;
float bankCount = 8.f;
bool mixed = false;
std::string message;

bool same(const gen::Params &a, const gen::Params &b) {
	return a.type == b.type && a.brightness == b.brightness && a.movement == b.movement && a.grit == b.grit &&
		a.keyframes == b.keyframes && a.morph == b.morph && a.seed == b.seed && a.waves == b.waves;
}

void regenerate() {
	params.waves = BANK_LEN;
	if (generated && same(params, generatedFor))
		return;
	table = gen::generate(params);
	previewBank.clear();
	previewBank.setSamples(table.data());
	generatedFor = params;
	generated = true;
}

void setType(gen::Type t) {
	uint32_t seed = params.seed;
	params = gen::defaults(t);
	params.seed = lockSeed ? seed : gen::nextSeed(seed ^ SDL_GetTicks());
	keyframesSlider = (float)params.keyframes;
	message.clear();
}

bool accentButton(const char *label, ImVec2 size = ImVec2(0, 0)) {
	ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.22f)));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.32f)));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.42f)));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme.modern ? theme.accent : theme.text));
	bool pressed = ImGui::Button(label, size);
	ImGui::PopStyleColor(4);
	return pressed;
}

/** Type picker: one row of buttons per group */
void typePicker() {
	static const gen::Type groups[3][4] = {
		{gen::SUB, gen::REESE, gen::NEURO, gen::WOBBLE},
		{gen::HOOVER, gen::STAB, gen::ACID, gen::TYPE_COUNT},
		{gen::WARM, gen::GLASSY, gen::EVOLVING, gen::TYPE_COUNT},
	};
	static gen::Type lastInGroup[3] = {gen::REESE, gen::HOOVER, gen::WARM};
	for (int g = 0; g < 3; g++) {
		const char *labels[4];
		int count = 0, selected = -1;
		for (int i = 0; i < 4 && groups[g][i] != gen::TYPE_COUNT; i++) {
			labels[count] = gen::typeName(groups[g][i]);
			if (groups[g][i] == params.type)
				selected = count;
			count++;
		}
		// The group name is clickable too: it picks the type last used in that group
		if (selected >= 0)
			lastInGroup[g] = groups[g][selected];
		bool active = selected >= 0;
		ImVec2 pos = ImGui::GetCursorScreenPos();
		float rowHeight = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.f;
		ImGui::PushID(g);
		bool groupClicked = ImGui::InvisibleButton("##group", ImVec2(56.f, rowHeight));
		ImGui::PopID();
		bool hovered = ImGui::IsItemHovered();
		const char *name = gen::groupName((gen::Group)g);
		ImVec2 textSize = ImGui::CalcTextSize(name);
		ImU32 col = active ? theme.accent : (hovered ? theme.text : theme.textDim);
		ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, pos.y + (rowHeight - textSize.y) * 0.5f), col, name);
		if (groupClicked && !active)
			setType(lastInGroup[g]);
		ImGui::SameLine(70);
		int before = selected;
		char id[16];
		snprintf(id, sizeof(id), "group%d", g);
		ui::Segmented(id, labels, count, &selected);
		if (selected != before && selected >= 0)
			setType(groups[g][selected]);
	}
}

/** The wave under the playhead, large */
void currentWaveDisplay(float height) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 a = ImGui::GetCursorScreenPos();
	float width = ImGui::GetContentRegionAvailWidth();
	ImGui::Dummy(ImVec2(width, height));
	dl->AddRectFilled(a, a + ImVec2(width, height), theme.display, ImGui::GetStyle().FrameRounding);
	int w = (int)clampf(roundf(morphZ), 0.f, (float)(BANK_LEN - 1));
	const float *wave = previewBank.waves[w].postSamples;
	ImVec2 pts[WAVE_LEN];
	float mid = a.y + height * 0.5f, half = height * 0.5f - 8.f;
	for (int i = 0; i < WAVE_LEN; i++)
		pts[i] = ImVec2(a.x + 8.f + (width - 16.f) * i / (WAVE_LEN - 1), mid - clampf(wave[i], -1.f, 1.f) * half);
	dl->AddLine(ImVec2(a.x + 8.f, mid), ImVec2(a.x + width - 8.f, mid), theme.gridStrong);
	if (theme.modern)
		ui::FillUnderCurve(dl, pts, WAVE_LEN, mid, colAlpha(theme.accent, 0.28f), colAlpha(theme.accent, 0.02f));
	ui::GlowPolyline(dl, pts, WAVE_LEN, theme.accent, 1.6f);
	char label[32];
	snprintf(label, sizeof(label), "wave %d", w + 1);
	ImVec2 ls = ImGui::CalcTextSize(label);
	dl->AddText(ImVec2(a.x + width - ls.x - 10.f, a.y + height - ls.y - 6.f), colAlpha(theme.textDim, 0.8f), label);
}

/** Puts the table in the editor, with the keyframes marked so it can be re-morphed */
void useInEditor() {
	regenerate();
	currentBank.clear();
	currentBank.setSamples(table.data());
	int K = params.keyframes < 2 ? 2 : (params.keyframes > BANK_LEN ? BANK_LEN : params.keyframes);
	for (int i = 0; i < K; i++)
		currentBank.keyframes[(int)lroundf((float)i * (BANK_LEN - 1) / (K - 1))] = true;
	lastFilename[0] = '\0';
	snprintf(tableName, sizeof(tableName), "%s", gen::tableName(params).c_str());
	historyPushNew();
	uiSetPage(0);
}

void addToCard() {
	int count = (int)lroundf(bankCount);
	std::vector<osiris::Position> slots = cardFreeSlots(count);
	if (slots.empty()) {
		message = "The SD card layout is full";
		return;
	}
	cardSnapshot("Generate Tables");
	uint32_t seed = params.seed;
	uint32_t typeSeed = params.seed * 7919u + 17u;
	for (const osiris::Position &pos : slots) {
		gen::Params p = params;
		if (mixed) {
			typeSeed = typeSeed * 1103515245u + 12345u;
			p = gen::defaults((gen::Type)((typeSeed >> 16) % gen::TYPE_COUNT));
		}
		seed = gen::nextSeed(seed);
		p.seed = seed;
		p.waves = osiris::WAVES;
		std::vector<float> t = gen::generate(p);
		osiris::Slot *slot = cardSlot(pos.bank, pos.slot);
		osiris::loadSlotFromSamples(*slot, t.data(), osiris::WAVES, gen::tableName(p), "Generator");
		cardSetPackIfEmpty(pos.bank, mixed ? "Mixed" : gen::typeName(params.type));
	}
	const osiris::Position &first = slots.front(), &last = slots.back();
	char buf[160];
	snprintf(buf, sizeof(buf), "Added %d %s to %c%d-%c%d on the SD Card page", (int)slots.size(), slots.size() == 1 ? "table" : "tables",
		osiris::bankLetter(first.bank), first.slot + 1, osiris::bankLetter(last.bank), last.slot + 1);
	message = buf;
	if ((int)slots.size() < count)
		message += " (the card is now full)";
	cardSetStatus(message);
}

} // namespace

void genOpenDialog() {
	openRequested = true;
	message.clear();
	keyframesSlider = (float)params.keyframes;
}

void genCloseDialog() {
	closeRequested = true;
}

void genRenderDialog() {
	if (openRequested) {
		ImGui::OpenPopup("Generate Wavetable");
		openRequested = false;
	}
	ImGui::SetNextWindowContentWidth(680);
	ui::LimitModalHeight();
	if (!ImGui::BeginPopupModal("Generate Wavetable", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (isOpen) {
			// Closed: the transport goes back to playing the editor's table
			isOpen = false;
			playingBank = &currentBank;
		}
		closeRequested = false;
		return;
	}
	ui::CenterModal();
	isOpen = true;
	regenerate();
	// Audition the preview while the dialog is open
	playingBank = &previewBank;
	playModeXY = false;

	typePicker();
	ImGui::TextDisabled("%s", gen::typeBlurb(params.type));

	ui::SectionHeading("Shape");
	float half = (ImGui::GetContentRegionAvailWidth() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
	ImGui::PushItemWidth(half);
	ui::Slider("##brightness", "Brightness", &params.brightness, 0.f, 1.f, "%.2f");
	ImGui::SameLine();
	ui::Slider("##movement", "Movement", &params.movement, 0.f, 1.f, "%.2f");
	ui::Slider("##grit", "Grit", &params.grit, 0.f, 1.f, "%.2f");
	ImGui::SameLine();
	if (ui::Slider("##keyframes", "Keyframes", &keyframesSlider, 2.f, 8.f, "%.0f"))
		keyframesSlider = roundf(keyframesSlider);
	params.keyframes = (int)lroundf(keyframesSlider);
	ImGui::PopItemWidth();

	int morph = params.morph;
	ImGui::AlignFirstTextHeightToWidgets();
	ImGui::TextDisabled("Morph");
	ImGui::SameLine(70);
	ImGui::RadioButton("Crossfade##gen", &morph, wt::CROSSFADE);
	ImGui::SameLine();
	ImGui::RadioButton("Spectral##gen", &morph, wt::SPECTRAL);
	params.morph = (wt::MorphMode)morph;

	int seed = (int)params.seed;
	ImGui::AlignFirstTextHeightToWidgets();
	ImGui::TextDisabled("Seed");
	ImGui::SameLine(70);
	ImGui::PushItemWidth(160);
	if (ImGui::InputInt("##seed", &seed))
		params.seed = (uint32_t)(seed < 0 ? 0 : (seed > 9999 ? 9999 : seed));
	ImGui::PopItemWidth();
	ImGui::SameLine();
	if (accentButton("Randomize"))
		params.seed = gen::nextSeed(params.seed ^ SDL_GetTicks());
	ImGui::SameLine();
	ImGui::Checkbox("Lock seed", &lockSeed);
	ImGui::SameLine();
	ImGui::TextDisabled("(keeps it when you change type)");

	regenerate();

	// Preview
	ImGui::Dummy(ImVec2(0, 2));
	if (theme.fontHeading)
		ImGui::PushFont(theme.fontHeading);
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.text), "%s", gen::tableName(params).c_str());
	if (theme.fontHeading)
		ImGui::PopFont();
	std::vector<unsigned char> marks(BANK_LEN, ui::STRIP_NORMAL);
	int K = params.keyframes < 2 ? 2 : params.keyframes;
	for (int i = 0; i < K && K <= BANK_LEN; i++)
		marks[(int)lroundf((float)i * (BANK_LEN - 1) / (K - 1))] = ui::STRIP_KEYFRAME;
	bool compact = ImGui::GetIO().DisplaySize.y < 860.f;
	ui::WaveStrip("##genStrip", table.data(), BANK_LEN, marks.data(), compact ? 46.f : 58.f);
	currentWaveDisplay(compact ? 64.f : 110.f);

	// Audition
	bool playing = playEnabled;
	ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(playing ? colAlpha(theme.accent, 0.22f) : theme.control));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(playing ? theme.accent : theme.text));
	if (ImGui::Button(playing ? "Playing" : "Play", ImVec2(90, 0)))
		playEnabled = !playEnabled;
	ImGui::PopStyleColor(2);
	ImGui::SameLine();
	float rest = (ImGui::GetContentRegionAvailWidth() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
	ImGui::PushItemWidth(rest);
	ui::Slider("##genMorphZ", "Morph Z", &morphZ, 0.f, (float)(BANK_LEN - 1), "%.2f");
	ImGui::SameLine();
	ui::Slider("##genSweep", "Sweep", &morphZSpeed, 0.f, 10.f, "%.2f Hz", 3.f);
	ImGui::PopItemWidth();

	// Send it somewhere
	ImGui::Dummy(ImVec2(0, 4));
	if (accentButton("Use in Editor", ImVec2(150, 0))) {
		useInEditor();
		closeRequested = true;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("replaces the editor's table; keyframes are marked so you can re-morph it");

	ui::SectionHeading("Generate a bank to the SD Card page");
	ImGui::PushItemWidth(220);
	ui::Slider("##bankCount", "Tables", &bankCount, 1.f, 32.f, "%.0f");
	bankCount = roundf(bankCount);
	ImGui::PopItemWidth();
	ImGui::SameLine();
	ImGui::Checkbox("Mixed types", &mixed);
	ImGui::SameLine();
	char addLabel[48];
	snprintf(addLabel, sizeof(addLabel), "Add %d %s", (int)bankCount, bankCount == 1.f ? "Table" : "Tables");
	if (ImGui::Button(addLabel))
		addToCard();
	if (!message.empty())
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.accent), "%s", message.c_str());
	else
		ImGui::TextDisabled(mixed ? "Each table gets a random type and a new seed. Goes into the next free slots."
			: "Each table gets a new seed with these settings. Goes into the next free slots.");

	ImGui::Dummy(ImVec2(0, 4));
	if (ImGui::Button("Close", ImVec2(120, 0)) || closeRequested) {
		closeRequested = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}
