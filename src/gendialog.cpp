// "Generate Wavetable" dialog: drum & bass / jungle tables from generator.cpp, auditioned live, sent to the editor or
// straight onto the SD Card page. Also the "Mutate & Breed" window, which grows variations of the current table.

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

// Start from my waves
bool useSource = false;
std::string sourceLabel;
bool sourceSilent = false;

void regenerate() {
	params.waves = BANK_LEN;
	if (generated && gen::sameParams(params, generatedFor))
		return;
	table = gen::generate(params);
	previewBank.clear();
	previewBank.setSamples(table.data());
	generatedFor = params;
	generated = true;
}

/** A fresh table: no mutations */
void resetLineage() {
	params.genes.clear();
	params.generation = 0;
}

void setType(gen::Type t) {
	gen::Params old = params;
	params = gen::defaults(t);
	// Keep the choices that aren't about the sound type
	params.motion = old.motion;
	params.vintage = old.vintage;
	params.source = old.source;
	params.sourceMix = old.sourceMix;
	params.seed = lockSeed ? old.seed : gen::nextSeed(old.seed ^ SDL_GetTicks());
	keyframesSlider = (float)params.keyframes;
	message.clear();
}

/** Takes the waves to start from out of the editor: its marked keyframes, else the ends of the selection, else the
selected wave */
void captureSource() {
	std::vector<int> picks;
	for (int w = 0; w < BANK_LEN; w++)
		if (currentBank.keyframes[w])
			picks.push_back(w);
	int selStart = mini(selectedId, lastSelectedId), selEnd = maxi(selectedId, lastSelectedId);
	if (picks.empty() && selStart < selEnd) {
		picks.push_back(selStart);
		picks.push_back(selEnd);
	}
	if (picks.empty())
		picks.push_back(clampi(selectedId, 0, BANK_LEN - 1));
	params.source.clear();
	sourceLabel.clear();
	float energy = 0.f;
	for (size_t i = 0; i < picks.size(); i++) {
		const float *wave = currentBank.waves[picks[i]].postSamples;
		params.source.insert(params.source.end(), wave, wave + WAVE_LEN);
		for (int j = 0; j < WAVE_LEN; j++)
			energy += wave[j] * wave[j];
		sourceLabel += (i ? ", " : "") + std::to_string(picks[i] + 1);
	}
	sourceLabel = (picks.size() == 1 ? "wave " : "waves ") + sourceLabel;
	sourceSilent = energy < 1e-6f;
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

/** A button that can be greyed out (ImGui 1.50 has no disabled state) */
bool maybeButton(const char *label, bool enabled, ImVec2 size = ImVec2(0, 0)) {
	if (!enabled)
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
	bool pressed = ImGui::Button(label, size);
	if (!enabled)
		ImGui::PopStyleVar();
	return pressed && enabled;
}

/** A small label in the left column of a row of controls */
void rowLabel(const char *label, float column = 70.f) {
	ImGui::AlignFirstTextHeightToWidgets();
	ImGui::TextDisabled("%s", label);
	ImGui::SameLine(column);
}

/** Type picker: one row of buttons per group */
void typePicker() {
	static gen::Type lastInGroup[3] = {gen::REESE, gen::HOOVER, gen::WARM};
	for (int g = 0; g < 3; g++) {
		std::vector<gen::Type> types = gen::typesIn((gen::Group)g);
		const char *labels[gen::TYPE_COUNT];
		int count = 0, selected = -1;
		for (gen::Type t : types) {
			labels[count] = gen::typeName(t);
			if (t == params.type)
				selected = count;
			count++;
		}
		// The group name is clickable too: it picks the type last used in that group
		if (selected >= 0)
			lastInGroup[g] = types[selected];
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
			setType(types[selected]);
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
	dl->AddText(ImVec2(a.x + width - ls.x - 8.f, a.y + height - ls.y - 4.f), theme.textDim, label);
}

/** Keyframe marks for a wave strip of the current settings */
std::vector<unsigned char> keyframeMarks(const gen::Params &p) {
	std::vector<unsigned char> marks(BANK_LEN, ui::STRIP_NORMAL);
	for (int pos : gen::keyframePositions(p))
		if (pos >= 0 && pos < BANK_LEN)
			marks[pos] = ui::STRIP_KEYFRAME;
	return marks;
}

/** Puts the table in the editor, with the keyframes marked so it can be re-morphed */
void useInEditor() {
	regenerate();
	currentBank.clear();
	currentBank.setSamples(table.data());
	for (int pos : gen::keyframePositions(params))
		if (pos >= 0 && pos < BANK_LEN)
			currentBank.keyframes[pos] = true;
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
			p.motion = params.motion;
			p.vintage = params.vintage;
		}
		// Each table is a fresh one from a new seed
		p.genes.clear();
		p.generation = 0;
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

////////////////////
// Mutate & Breed
////////////////////

const int CHILDREN = 6;
bool evolveOpenRequested = false;
bool evolveOpen = false;
gen::Params evolveStart;              // to go back to on Cancel
gen::Params parent;
std::vector<float> parentTable;
Bank parentBank;
gen::Params children[CHILDREN];
std::vector<float> childTables[CHILDREN];
Bank childBanks[CHILDREN];
int picked[2] = {-1, -1};             // up to two selected children
int hearing = -1;                     // -1 = the parent
float mutationAmount = 0.3f;
uint32_t mutationCounter = 1;
std::string evolveMessage;

void setChild(int i, const gen::Params &p) {
	children[i] = p;
	children[i].waves = BANK_LEN;
	childTables[i] = gen::generate(children[i]);
	childBanks[i].clear();
	childBanks[i].setSamples(childTables[i].data());
}

void setParent(const gen::Params &p) {
	parent = p;
	parent.waves = BANK_LEN;
	parentTable = gen::generate(parent);
	parentBank.clear();
	parentBank.setSamples(parentTable.data());
}

void clearPicks() {
	picked[0] = picked[1] = -1;
}

void mutateChildren() {
	for (int i = 0; i < CHILDREN; i++)
		setChild(i, gen::mutate(parent, mutationAmount, (mutationCounter++) * 7919u + (uint32_t)i));
	clearPicks();
	hearing = -1;
}

void breedChildren(const gen::Params &a, const gen::Params &b) {
	for (int i = 0; i < CHILDREN; i++)
		setChild(i, gen::breed(a, b, mutationAmount, (mutationCounter++) * 104729u + (uint32_t)i));
	clearPicks();
	hearing = -1;
}

int pickCount() {
	return (picked[0] >= 0) + (picked[1] >= 0);
}

/** Selects a child (up to two; a third replaces the oldest), or deselects it */
void togglePick(int i) {
	if (picked[0] == i) {
		picked[0] = picked[1];
		picked[1] = -1;
	}
	else if (picked[1] == i)
		picked[1] = -1;
	else if (picked[0] < 0)
		picked[0] = i;
	else if (picked[1] < 0)
		picked[1] = i;
	else {
		picked[0] = picked[1];
		picked[1] = i;
	}
}

/** A table drawn as a small waterfall: a few of its waves stacked back to front. Returns true when clicked. */
bool tableThumb(const char *id, const std::vector<float> &t, ImVec2 size, int badge, bool playing, const std::string &name) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 a = ImGui::GetCursorScreenPos();
	bool clicked = ImGui::InvisibleButton(id, size);
	bool hovered = ImGui::IsItemHovered();
	float rounding = ImGui::GetStyle().FrameRounding;
	dl->AddRectFilled(a, a + size, badge >= 0 ? theme.displayHi : theme.display, rounding);
	int waves = (int)t.size() / WAVE_LEN;
	const int LAYERS = 8;
	float labelH = ImGui::GetFontSize() + 6.f;
	float dx = 4.f, dy = 5.f;
	float left = a.x + 8.f, right = a.x + size.x - 8.f - dx * (LAYERS - 1);
	float amp = (size.y - labelH - 12.f - dy * (LAYERS - 1)) * 0.5f;
	for (int layer = LAYERS - 1; layer >= 0; layer--) {
		int w = waves <= 1 ? 0 : (int)lroundf((float)layer * (waves - 1) / (LAYERS - 1));
		const float *wave = &t[(size_t)w * WAVE_LEN];
		float baseY = a.y + 6.f + amp + dy * (LAYERS - 1) - layer * dy;
		ImVec2 pts[64];
		for (int i = 0; i < 64; i++) {
			int s = i * (WAVE_LEN - 1) / 63;
			pts[i] = ImVec2(left + layer * dx + (right - left) * i / 63.f, baseY - clampf(wave[s], -1.f, 1.f) * amp);
		}
		float alpha = layer == 0 ? 1.f : 0.25f + 0.5f * (1.f - (float)layer / LAYERS);
		dl->AddPolyline(pts, 64, colAlpha(colMix(theme.accent, theme.accent2, (float)layer / LAYERS), alpha), false, layer == 0 ? 1.6f : 1.f, true);
	}
	dl->AddText(ImVec2(a.x + 8.f, a.y + size.y - labelH + 2.f), playing ? theme.accent : theme.textDim, name.c_str());
	if (playing) {
		const char *p = "playing";
		ImVec2 ps = ImGui::CalcTextSize(p);
		dl->AddText(ImVec2(a.x + size.x - ps.x - 8.f, a.y + size.y - labelH + 2.f), theme.accent, p);
	}
	if (badge >= 0) {
		dl->AddRect(a, a + size, theme.accent, rounding, 0x0F, 2.f);
		char letter[2] = {(char)('A' + badge), 0};
		ImVec2 c = ImVec2(a.x + size.x - 14.f, a.y + 14.f);
		dl->AddCircleFilled(c, 9.f, theme.accent, 16);
		ImVec2 ls = ImGui::CalcTextSize(letter);
		dl->AddText(c - ls * 0.5f, theme.display, letter);
	}
	else if (hovered)
		dl->AddRect(a, a + size, theme.outline, rounding);
	return clicked;
}

void evolveOpenDialog() {
	regenerate();
	evolveStart = params;
	setParent(params);
	mutateChildren();
	evolveMessage.clear();
	evolveOpenRequested = true;
}

void evolveRenderDialog() {
	if (evolveOpenRequested) {
		ImGui::OpenPopup("Mutate & Breed");
		evolveOpenRequested = false;
	}
	ImGui::SetNextWindowContentWidth(720);
	ui::LimitModalHeight();
	if (!ImGui::BeginPopupModal("Mutate & Breed", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		evolveOpen = false;
		return;
	}
	ui::CenterModal();
	evolveOpen = true;
	bool compact = ImGui::GetIO().DisplaySize.y < 860.f;

	ImGui::PushTextWrapPos(720.f);
	ImGui::TextDisabled("Click a variation to hear it and pick it (A). Keep it to carry on from there, or pick two (A and B) and breed them.");
	ImGui::PopTextWrapPos();

	// The parent
	ui::SectionHeading("Current");
	float width = ImGui::GetContentRegionAvailWidth();
	std::string parentName = gen::tableName(parent);
	if (tableThumb("##parent", parentTable, ImVec2(width, compact ? 74.f : 90.f), -1, hearing == -1, parentName))
		hearing = -1;

	// The children
	ui::SectionHeading("Variations");
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	ImVec2 cell = ImVec2((width - 2.f * spacing) / 3.f, compact ? 96.f : 116.f);
	for (int i = 0; i < CHILDREN; i++) {
		if (i % 3)
			ImGui::SameLine();
		int badge = picked[0] == i ? 0 : (picked[1] == i ? 1 : -1);
		char id[16];
		snprintf(id, sizeof(id), "##child%d", i);
		std::string name = std::to_string(i + 1) + "  " + gen::tableName(children[i]);
		if (tableThumb(id, childTables[i], cell, badge, hearing == i, name)) {
			togglePick(i);
			hearing = i;
		}
	}

	// How much each mutation changes
	ImGui::Dummy(ImVec2(0, 2));
	ImGui::PushItemWidth(260);
	ui::Slider("##mutationAmount", "Mutation", &mutationAmount, 0.05f, 1.f, "%.2f");
	ImGui::PopItemWidth();
	ImGui::SameLine();
	ImGui::TextDisabled(mutationAmount < 0.25f ? "small tweaks" : (mutationAmount < 0.6f ? "clear variations" : "wild: can jump to new shapes"));

	// Audition
	bool playing = playEnabled;
	ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(playing ? colAlpha(theme.accent, 0.22f) : theme.control));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(playing ? theme.accent : theme.text));
	if (ImGui::Button(playing ? "Playing" : "Play", ImVec2(90, 0)))
		playEnabled = !playEnabled;
	ImGui::PopStyleColor(2);
	ImGui::SameLine();
	ImGui::PushItemWidth((ImGui::GetContentRegionAvailWidth() - spacing) * 0.5f);
	ui::Slider("##evolveMorphZ", "Morph Z", &morphZ, 0.f, (float)(BANK_LEN - 1), "%.2f");
	ImGui::SameLine();
	ui::Slider("##evolveSweep", "Sweep", &morphZSpeed, 0.f, 10.f, "%.2f Hz", 3.f);
	ImGui::PopItemWidth();
	playingBank = hearing >= 0 ? &childBanks[hearing] : &parentBank;
	playModeXY = false;

	// Actions
	ImGui::Dummy(ImVec2(0, 4));
	int picks = pickCount();
	if (accentButton("New Variations", ImVec2(140, 0))) {
		mutateChildren();
		evolveMessage = "New variations of " + gen::tableName(parent);
	}
	ImGui::SameLine();
	if (maybeButton("Keep A", picks >= 1, ImVec2(110, 0))) {
		std::string name = gen::tableName(children[picked[0]]);
		setParent(children[picked[0]]);
		mutateChildren();
		evolveMessage = "Kept " + name + ": these are its variations";
	}
	ImGui::SameLine();
	if (maybeButton("Breed A + B", picks == 2, ImVec2(130, 0))) {
		gen::Params a = children[picked[0]], b = children[picked[1]];
		setParent(a);
		breedChildren(a, b);
		evolveMessage = "Children of " + gen::tableName(a) + " and " + gen::tableName(b);
	}
	if (!evolveMessage.empty()) {
		ImGui::SameLine();
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.accent), "%s", evolveMessage.c_str());
	}

	ImGui::Dummy(ImVec2(0, 4));
	std::string useLabel = picks >= 1 ? "Use A" : "Use Current";
	bool use = accentButton(useLabel.c_str(), ImVec2(140, 0));
	ImGui::SameLine();
	bool cancel = ImGui::Button("Cancel", ImVec2(110, 0));
	ImGui::SameLine();
	ImGui::TextDisabled(picks >= 1 ? "takes variation A back to the generator" : "takes the current table back to the generator");
	if (use) {
		params = picks >= 1 ? children[picked[0]] : parent;
		keyframesSlider = (float)params.keyframes;
		message.clear();
	}
	if (cancel)
		params = evolveStart;
	if (use || cancel)
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

} // namespace

void genOpenDialog() {
	openRequested = true;
	message.clear();
	keyframesSlider = (float)params.keyframes;
	if (useSource)
		captureSource(); // the editor may have changed since last time
}

void genCloseDialog() {
	closeRequested = true;
}

/** For screenshots */
void genOpenMutateDialog() {
	evolveOpenDialog();
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
	bool compact = ImGui::GetIO().DisplaySize.y < 860.f;

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

	// Morph and vintage on one row, motion on the next
	int morph = params.morph;
	rowLabel("Morph");
	ImGui::RadioButton("Crossfade##gen", &morph, wt::CROSSFADE);
	ImGui::SameLine();
	ImGui::RadioButton("Spectral##gen", &morph, wt::SPECTRAL);
	params.morph = (wt::MorphMode)morph;
	ImGui::SameLine(330);
	ImGui::AlignFirstTextHeightToWidgets();
	ImGui::TextDisabled("Vintage");
	ImGui::SameLine(395);
	const char *vintageLabels[gen::VINTAGE_COUNT];
	for (int v = 0; v < gen::VINTAGE_COUNT; v++)
		vintageLabels[v] = v == gen::SP1200 ? "SP-1200" : gen::vintageName((gen::Vintage)v);
	int vintage = params.vintage;
	ui::Segmented("vintage", vintageLabels, gen::VINTAGE_COUNT, &vintage);
	params.vintage = (gen::Vintage)vintage;

	rowLabel("Motion");
	const char *motionLabels[gen::MOTION_COUNT];
	for (int m = 0; m < gen::MOTION_COUNT; m++)
		motionLabels[m] = gen::motionName((gen::Motion)m);
	int motion = params.motion;
	ui::Segmented("motion", motionLabels, gen::MOTION_COUNT, &motion);
	params.motion = (gen::Motion)motion;
	ImGui::SameLine();
	static const char *motionHelp[gen::MOTION_COUNT] = {"first keyframe to last", "out and back: loops with an LFO",
		"holds each keyframe: rhythmic jumps", "wanders back and forth"};
	ImGui::TextDisabled("%s", motionHelp[params.motion]);

	int seed = (int)params.seed;
	rowLabel("Seed");
	ImGui::PushItemWidth(160);
	if (ImGui::InputInt("##seed", &seed)) {
		params.seed = (uint32_t)(seed < 0 ? 0 : (seed > 9999 ? 9999 : seed));
		resetLineage();
	}
	ImGui::PopItemWidth();
	ImGui::SameLine();
	if (accentButton("Randomize")) {
		params.seed = gen::nextSeed(params.seed ^ SDL_GetTicks());
		resetLineage();
	}
	ImGui::SameLine();
	ImGui::Checkbox("Lock seed", &lockSeed);
	ImGui::SameLine();
	ImGui::TextDisabled("(keeps it when you change type)");

	// Start from my waves
	rowLabel("Start");
	if (ImGui::Checkbox("From my waves", &useSource)) {
		if (useSource)
			captureSource();
		else
			params.source.clear();
	}
	if (useSource) {
		ImGui::SameLine();
		ImGui::PushItemWidth(200);
		ui::Slider("##sourceMix", "Into type", &params.sourceMix, 0.f, 1.f, "%.2f");
		ImGui::PopItemWidth();
		ImGui::SameLine();
		if (ImGui::SmallButton("Grab Again"))
			captureSource();
		ImGui::SameLine();
		if (sourceSilent)
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.accent), "editor %s silent", sourceLabel.c_str());
		else
			ImGui::TextDisabled("editor %s", sourceLabel.c_str());
	}
	else {
		ImGui::SameLine();
		ImGui::TextDisabled("starts on the editor's keyframes (or selected wave) and turns into the type");
	}

	regenerate();

	// Preview
	ImGui::Dummy(ImVec2(0, 2));
	if (theme.fontHeading)
		ImGui::PushFont(theme.fontHeading);
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.text), "%s", gen::tableName(params).c_str());
	if (theme.fontHeading)
		ImGui::PopFont();
	std::vector<unsigned char> marks = keyframeMarks(params);
	ui::WaveStrip("##genStrip", table.data(), BANK_LEN, marks.data(), compact ? 46.f : 58.f);
	currentWaveDisplay(compact ? 56.f : 90.f);

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
	if (accentButton("Mutate & Breed...", ImVec2(160, 0)))
		evolveOpenDialog();
	ImGui::SameLine();
	ImGui::TextDisabled("grow variations of this table");

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

	// Mutate & Breed opens on top of this dialog
	evolveRenderDialog();
	if (evolveOpen)
		regenerate();
	ImGui::EndPopup();
}
