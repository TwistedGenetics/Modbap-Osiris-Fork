// "SD Card" page: lay out the four Osiris banks (A-D, 32 wavetables each) and export them to an SD card.
// The card rules and file handling are in osiriscard.cpp and cardio.cpp; this file is only the UI.

#include "OsirisEdit.hpp"
#include "cardio.hpp"
#include "cardpage.hpp"
#include "wtprocess.hpp"
#include "theme.hpp"

#include "imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"

#include "osdialog/osdialog.h"

#include <algorithm>
#include <libgen.h>
#include <string.h>

using namespace osiris;

static Card card;
static int selBank = 0;
static int selSlot = 0;
static char packBuf[BANKS][48];
static std::string status;
static std::vector<std::string> notes;

// Drag and drop between slots
static bool dragging = false;
static int dragBank = -1, dragSlot = -1;

// Right-click menu and rename dialog
static int menuBank = 0, menuSlot = 0;
static bool openRename = false;
static char renameBuf[64];

// Export dialog
static bool openExport = false;
static bool openSummary = false;
static std::vector<std::string> volumes;
static std::string exportDest;
static int existingChoice = 0; // 0 backup, 1 merge
static int fixChoice = 0;      // 0 resample, 1 pad
static bool removeJunk = true;
static ExportResult lastResult;

// One level of undo for changes to the whole card
static Card undoCard;
static bool undoAvailable = false;
static std::string undoLabel;
static bool openCycleConfirm = false;
static bool exportCycle = false;

static ImU32 warnColor() {
	return theme.modern ? IM_COL32(0xf5, 0xb5, 0x4a, 0xff) : IM_COL32(0xc0, 0x60, 0x00, 0xff);
}

static void setStatus(const std::string &s) {
	status = s;
}

void cardSnapshot(const char *label) {
	undoCard = card;
	undoAvailable = true;
	undoLabel = label;
}

Slot *cardSlot(int bank, int slot) {
	if (bank < 0 || bank >= BANKS || slot < 0 || slot >= SLOTS)
		return NULL;
	return &card.slots[bank][slot];
}

void cardSetStatus(const std::string &s) {
	status = s;
}

static void cycleSlot(Slot &slot) {
	if (slot.filled && (int)slot.samples.size() >= slot.waveCount * WAVE_SAMPLES)
		wt::processTable(slot.samples.data(), slot.waveCount, true, false);
}

static void syncPackBuffers() {
	for (int b = 0; b < BANKS; b++)
		snprintf(packBuf[b], sizeof(packBuf[b]), "%s", card.pack[b].c_str());
}

static std::string slotLabel(int b, int s) {
	return std::string(1, bankLetter(b)) + std::to_string(s + 1);
}

static std::string fileBaseName(const std::string &path) {
	size_t p = path.find_last_of("/\\");
	return p == std::string::npos ? path : path.substr(p + 1);
}

/** Folder to start file dialogs in */
static char *startDir() {
	if (lastFilename[0] == '\0')
		return strdup(".");
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", lastFilename);
	return strdup(dirname(buf));
}

/** The next free slot at or after the selection, across banks. False if the card is full. */
static bool nextFree(int &bank, int &slot) {
	bool occupied[BANKS][SLOTS];
	card.getOccupied(occupied);
	std::vector<Position> p = planFill(occupied, selBank, selSlot, 1);
	if (p.empty())
		p = planFill(occupied, 0, 0, 1);
	if (p.empty())
		return false;
	bank = p[0].bank;
	slot = p[0].slot;
	return true;
}

static std::string editorName() {
	if (lastFilename[0]) {
		std::string base = fileBaseName(lastFilename);
		size_t dot = base.find_last_of('.');
		return parseMiddle(dot == std::string::npos ? base : base.substr(0, dot) + ".wav");
	}
	return "Editor";
}

/** Puts the wavetable open in the editor (after effects) into a slot */
static void slotFromEditor(int b, int s) {
	std::vector<float> samples(BANK_LEN * WAVE_LEN);
	currentBank.getPostSamples(samples.data());
	loadSlotFromSamples(card.slots[b][s], samples.data(), BANK_LEN, editorName(), "Editor");
	selBank = b;
	selSlot = s;
	setStatus("Added the editor's wavetable to " + slotLabel(b, s));
}

/** Stretches a slot's table to `count` waves for the editor */
static std::vector<float> wavesForEditor(const Slot &slot, int count) {
	std::vector<float> out(count * WAVE_SAMPLES);
	for (int w = 0; w < count; w++) {
		float pos = (count == 1 || slot.waveCount == 1) ? 0.f : (float)w * (slot.waveCount - 1) / (count - 1);
		int a = (int)pos;
		int c = std::min(a + 1, slot.waveCount - 1);
		float t = pos - a;
		for (int i = 0; i < WAVE_SAMPLES; i++)
			out[w * WAVE_SAMPLES + i] = slot.samples[a * WAVE_SAMPLES + i] * (1.f - t) + slot.samples[c * WAVE_SAMPLES + i] * t;
	}
	return out;
}

static void openInEditor(int b, int s) {
	const Slot &slot = card.slots[b][s];
	if (!slot.filled)
		return;
	std::vector<float> samples = wavesForEditor(slot, BANK_LEN);
	currentBank.clear();
	currentBank.setSamples(samples.data());
	historyPushNew();
	uiSetPage(0);
	blankCheckEditor(BANK_LEN, slot.name);
}

static void addWavFile() {
	char *dir = startDir();
	char *path = osdialog_file(OSDIALOG_OPEN, dir, NULL, NULL);
	free(dir);
	if (!path)
		return;
	int b, s;
	std::string error;
	Slot loaded;
	if (!nextFree(b, s))
		setStatus("The card is full");
	else if (!loadSlotFromWav(loaded, path, error))
		setStatus(error);
	else {
		card.slots[b][s] = loaded;
		selBank = b;
		selSlot = s;
		setStatus("Added " + fileBaseName(path) + " to " + slotLabel(b, s));
		blankCheckSlot(b, s, false);
	}
	free(path);
}

static void addFolder() {
	char *dir = startDir();
	char *path = osdialog_file(OSDIALOG_OPEN_DIR, dir, NULL, NULL);
	free(dir);
	if (!path)
		return;
	std::vector<std::string> files = listWavFiles(path);
	notes.clear();
	int b = selBank, s = selSlot;
	bool before[BANKS][SLOTS];
	card.getOccupied(before);
	int added = fillFromFiles(card, files, b, s, notes);
	setStatus("Added " + std::to_string(added) + " of " + std::to_string(files.size()) + " WAV files from " + fileBaseName(path));
	free(path);
	for (int bb = 0; bb < BANKS; bb++)
		for (int ss = 0; ss < SLOTS; ss++)
			if (!before[bb][ss] && card.slots[bb][ss].filled)
				blankCheckSlot(bb, ss, false);
}

static void openSdCard() {
	char *path = osdialog_file(OSDIALOG_OPEN_DIR, volumes.empty() ? NULL : volumes[0].c_str(), NULL, NULL);
	if (!path)
		return;
	Card loaded;
	notes.clear();
	if (openCard(loaded, path, notes)) {
		card = loaded;
		syncPackBuffers();
		selBank = 0;
		selSlot = 0;
		setStatus("Opened " + std::to_string(card.filledCount()) + " wavetables from " + path);
	}
	else {
		setStatus("Couldn't open that folder as an Osiris card");
	}
	free(path);
}

static void openProject() {
	char *dir = startDir();
	char *path = osdialog_file(OSDIALOG_OPEN, dir, NULL, NULL);
	free(dir);
	if (!path)
		return;
	std::string error;
	Card loaded;
	if (loadProject(loaded, path, error)) {
		card = loaded;
		syncPackBuffers();
		setStatus("Opened project " + fileBaseName(path));
	}
	else {
		setStatus(error);
	}
	free(path);
}

static void saveProjectAs() {
	char *dir = startDir();
	char *path = osdialog_file(OSDIALOG_SAVE, dir, "Card.osiriscard", NULL);
	free(dir);
	if (!path)
		return;
	std::string p = path;
	free(path);
	if (p.size() < 11 || p.compare(p.size() - 11, 11, ".osiriscard") != 0)
		p += ".osiriscard";
	std::string error;
	if (saveProject(card, p, error))
		setStatus("Saved project " + fileBaseName(p));
	else
		setStatus(error);
}

static void fixAll(FixMode mode) {
	cardSnapshot(mode == FIX_PAD ? "Pad All to 32" : "Resample All to 32");
	int n = 0;
	for (int b = 0; b < BANKS; b++) {
		for (int s = 0; s < SLOTS; s++) {
			Slot &slot = card.slots[b][s];
			if (slot.filled && slot.waveCount != WAVES) {
				slot.samples = slotTo32(slot, mode);
				slot.waveCount = WAVES;
				n++;
			}
		}
	}
	setStatus(std::string(mode == FIX_PAD ? "Padded " : "Resampled ") + std::to_string(n) + (n == 1 ? " slot" : " slots") + " to 32 waves");
}

static void startExport() {
	volumes = listVolumes();
	if (exportDest.empty() || std::find(volumes.begin(), volumes.end(), exportDest) == volumes.end())
		exportDest = volumes.empty() ? "" : volumes[0];
	existingChoice = 0;
	openExport = true;
}

////////////////////
// Drawing
////////////////////

static bool accentButton(const char *label) {
	ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.22f)));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.32f)));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.42f)));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme.modern ? theme.accent : theme.text));
	bool pressed = ImGui::Button(label);
	ImGui::PopStyleColor(4);
	return pressed;
}

/** Small picture of a table: the first wave bright, the last wave faint */
static void drawThumbnail(ImDrawList *dl, ImVec2 a, ImVec2 b, const Slot &slot, bool selected) {
	if (!slot.filled || slot.samples.empty())
		return;
	const int points = 64;
	ImVec2 pts[points];
	float midY = (a.y + b.y) * 0.5f, halfH = (b.y - a.y) * 0.5f - 1.f;
	for (int pass = 0; pass < 2; pass++) {
		int wave = (pass == 0) ? slot.waveCount - 1 : 0;
		for (int i = 0; i < points; i++) {
			float v = slot.samples[wave * WAVE_SAMPLES + i * WAVE_SAMPLES / points];
			pts[i] = ImVec2(a.x + (b.x - a.x) * i / (points - 1), midY - clampf(v, -1.f, 1.f) * halfH);
		}
		ImU32 col = (pass == 0) ? colAlpha(theme.accent2, 0.55f) : (selected ? theme.accent : colMix(theme.accent, theme.textDim, 0.25f));
		dl->AddPolyline(pts, points, col, false, pass == 0 ? 1.f : 1.3f, true);
	}
}

static void drawSlotRow(int b, int s, ImVec2 pos, ImVec2 size) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImGuiStyle &style = ImGui::GetStyle();
	const Slot &slot = card.slots[b][s];

	char id[16];
	snprintf(id, sizeof(id), "##slot%d_%d", b, s);
	ImGui::SetCursorScreenPos(pos);
	ImGui::InvisibleButton(id, size);
	bool hovered = ImGui::IsMouseHoveringRect(pos, pos + size);
	bool selected = (b == selBank && s == selSlot);

	// Interaction
	if (ImGui::IsItemClicked(0)) {
		selBank = b;
		selSlot = s;
	}
	if (hovered && ImGui::IsMouseDoubleClicked(0) && slot.filled)
		openInEditor(b, s);
	if (ImGui::IsItemActive() && slot.filled && !dragging && ImGui::IsMouseDragging(0, 6.f)) {
		dragging = true;
		dragBank = b;
		dragSlot = s;
	}
	bool dropTarget = dragging && hovered && !(b == dragBank && s == dragSlot);
	if (dropTarget && ImGui::IsMouseReleased(0)) {
		// Move into an empty slot, or swap with a filled one
		std::swap(card.slots[b][s], card.slots[dragBank][dragSlot]);
		setStatus("Moved " + slotLabel(dragBank, dragSlot) + " to " + slotLabel(b, s));
		selBank = b;
		selSlot = s;
		dragging = false;
	}
	if (hovered && ImGui::IsMouseClicked(1)) {
		menuBank = b;
		menuSlot = s;
		selBank = b;
		selSlot = s;
		ImGui::OpenPopup("slot menu");
	}

	// Background
	float rounding = style.FrameRounding;
	ImU32 bg = slot.filled ? theme.display : colAlpha(theme.control, 0.35f);
	if (hovered && !dragging)
		bg = colMix(bg, theme.controlHover, 0.5f);
	if (dropTarget)
		bg = colAlpha(theme.accent, 0.18f);
	dl->AddRectFilled(pos, pos + size, bg, rounding);
	if (selected)
		dl->AddRect(pos, pos + size, colAlpha(theme.accent, 0.85f), rounding, ~0, 1.5f);
	else if (theme.outline && slot.filled)
		dl->AddRect(pos, pos + size, theme.outline, rounding);

	// Number
	float fontSize = ImGui::GetFontSize();
	char num[8];
	snprintf(num, sizeof(num), "%d", s + 1);
	float textY = pos.y + (size.y - fontSize) * 0.5f;
	ImVec2 numSize = ImGui::CalcTextSize(num);
	dl->AddText(ImVec2(pos.x + 24.f - numSize.x, textY), slot.filled ? theme.textDim : colAlpha(theme.textDim, 0.5f), num);

	if (!slot.filled) {
		dl->AddText(ImVec2(pos.x + 34.f, textY), colAlpha(theme.textDim, 0.45f), "Empty");
		return;
	}

	// Thumbnail, name and a badge if it isn't 32 waves
	float thumbW = std::min(64.f, size.x * 0.3f);
	ImVec2 ta = ImVec2(pos.x + 32.f, pos.y + 5.f);
	ImVec2 tb = ImVec2(ta.x + thumbW, pos.y + size.y - 5.f);
	drawThumbnail(dl, ta, tb, slot, selected);

	float nameX = tb.x + 8.f;
	float right = pos.x + size.x - 8.f;
	if (slot.waveCount != WAVES) {
		char badge[16];
		snprintf(badge, sizeof(badge), "%dw", slot.waveCount);
		ImVec2 bs = ImGui::CalcTextSize(badge);
		ImVec2 ba = ImVec2(right - bs.x - 8.f, pos.y + (size.y - bs.y) * 0.5f - 2.f);
		dl->AddRectFilled(ba, ba + bs + ImVec2(8.f, 4.f), colAlpha(warnColor(), 0.18f), 4.f);
		dl->AddText(ba + ImVec2(4.f, 2.f), warnColor(), badge);
		right = ba.x - 6.f;
	}
	dl->PushClipRect(ImVec2(nameX, pos.y), ImVec2(right, pos.y + size.y), true);
	dl->AddText(ImVec2(nameX, textY), selected ? theme.text : colMix(theme.text, theme.textDim, 0.3f), slot.name.empty() ? "(unnamed)" : slot.name.c_str());
	dl->PopClipRect();
}

static void slotMenu() {
	if (!ImGui::BeginPopup("slot menu"))
		return;
	Slot &slot = card.slots[menuBank][menuSlot];
	ImGui::TextDisabled("%s", slotLabel(menuBank, menuSlot).c_str());
	if (slot.filled) {
		if (ImGui::MenuItem("Open in Editor"))
			openInEditor(menuBank, menuSlot);
		if (ImGui::MenuItem("Replace with Editor Wavetable"))
			slotFromEditor(menuBank, menuSlot);
		if (ImGui::MenuItem("Rename...")) {
			snprintf(renameBuf, sizeof(renameBuf), "%s", slot.name.c_str());
			openRename = true;
		}
		if (ImGui::MenuItem("Duplicate")) {
			int b = menuBank, s = menuSlot;
			bool occupied[BANKS][SLOTS];
			card.getOccupied(occupied);
			std::vector<Position> p = planFill(occupied, b, s, 1);
			if (p.empty())
				setStatus("No free slot after " + slotLabel(b, s));
			else {
				card.slots[p[0].bank][p[0].slot] = slot;
				setStatus("Duplicated " + slotLabel(b, s) + " to " + slotLabel(p[0].bank, p[0].slot));
			}
		}
		if (ImGui::MenuItem("Cycle")) {
			cardSnapshot("Cycle");
			cycleSlot(slot);
			setStatus("Cycled " + slotLabel(menuBank, menuSlot));
		}
		if (card.blanks(menuBank, menuSlot).any() && ImGui::MenuItem("Fill Blank Waves..."))
			blankCheckSlot(menuBank, menuSlot, true);
		if (slot.waveCount != WAVES) {
			if (ImGui::MenuItem("Resample to 32 Waves")) {
				cardSnapshot("Resample");
				slot.samples = slotTo32(slot, FIX_RESAMPLE);
				slot.waveCount = WAVES;
			}
			if (ImGui::MenuItem("Pad / Trim to 32 Waves")) {
				cardSnapshot("Pad / Trim");
				slot.samples = slotTo32(slot, FIX_PAD);
				slot.waveCount = WAVES;
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Clear")) {
			cardSnapshot("Clear Slot");
			slot.clear();
			setStatus("Cleared " + slotLabel(menuBank, menuSlot));
		}
	}
	else {
		if (ImGui::MenuItem("Add Editor Wavetable Here"))
			slotFromEditor(menuBank, menuSlot);
		if (ImGui::MenuItem("Add WAV File..."))
			addWavFile();
		if (ImGui::MenuItem("Add Folder From Here..."))
			addFolder();
	}
	ImGui::EndPopup();
}

static void renameDialog() {
	if (openRename) {
		ImGui::OpenPopup("Rename Slot");
		openRename = false;
	}
	if (!ImGui::BeginPopupModal("Rename Slot", NULL, ImGuiWindowFlags_AlwaysAutoResize))
		return;
	ImGui::Text("Name for %s", slotLabel(menuBank, menuSlot).c_str());
	ImGui::PushItemWidth(320);
	if (ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	bool enter = ImGui::InputText("##rename", renameBuf, sizeof(renameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::PopItemWidth();
	ImGui::TextDisabled("On the card: %s", fileName(menuBank, card.pack[menuBank], renameBuf, menuSlot + 1, card.slotNamesInFileNames).c_str());
	if (ImGui::Button("Rename", ImVec2(120, 0)) || enter) {
		card.slots[menuBank][menuSlot].name = renameBuf;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

static void exportDialog(const std::vector<Issue> &issues) {
	if (openExport) {
		ImGui::OpenPopup("Export to SD Card");
		openExport = false;
	}
	ImGui::SetNextWindowContentWidth(560);
	if (!ImGui::BeginPopupModal("Export to SD Card", NULL, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ui::SectionHeading("Destination");
	if (volumes.empty()) {
		ImGui::TextDisabled("No SD card found. Insert the card, or choose a folder.");
	}
	for (const std::string &v : volumes) {
		if (ImGui::RadioButton(v.c_str(), exportDest == v))
			exportDest = v;
	}
	if (ImGui::Button("Choose Folder...")) {
		char *path = osdialog_file(OSDIALOG_OPEN_DIR, exportDest.empty() ? NULL : exportDest.c_str(), NULL, NULL);
		if (path) {
			exportDest = path;
			free(path);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Refresh"))
		volumes = listVolumes();

	bool hasDest = !exportDest.empty();
	std::string osirisPath;
	if (hasDest) {
		std::string base = fileBaseName(exportDest);
		for (char &c : base)
			c = (char)tolower((unsigned char)c);
		osirisPath = (base == "osiris") ? exportDest : exportDest + "/Osiris";
		ImGui::TextDisabled("Files go to %s/A, B, C, D", osirisPath.c_str());
	}

	// Existing Osiris folder
	if (hasDest && directoryExists(osirisPath)) {
		ui::SectionHeading("Existing Osiris folder");
		ImGui::TextWrapped("This card already has an Osiris folder.");
		ImGui::RadioButton("Back it up to Osiris_backup_<date-time>, then write a fresh one (recommended)", &existingChoice, 0);
		ImGui::RadioButton("Merge: keep it, and move only the files being replaced into a backup folder", &existingChoice, 1);
	}

	// Checks
	int waveCountIssues = 0, gaps = 0;
	for (const Issue &i : issues) {
		waveCountIssues += i.kind == Issue::WAVE_COUNT;
		gaps += i.kind == Issue::GAP;
	}
	if (waveCountIssues) {
		ui::SectionHeading("Wavetables that aren't 32 waves");
		ImGui::Text("%d %s", waveCountIssues, waveCountIssues == 1 ? "slot needs fixing:" : "slots need fixing:");
		ImGui::RadioButton("Resample to 32 waves (stretch or squeeze the whole table)", &fixChoice, 0);
		ImGui::RadioButton("Pad or trim to 32 waves (repeat the last wave, or cut extras)", &fixChoice, 1);
	}
	if (gaps) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(warnColor()));
		ImGui::TextWrapped("%d empty %s between filled ones. The Osiris may not expect gaps in the numbering; use Close Gaps first if you're not sure.", gaps, gaps == 1 ? "slot sits" : "slots sit");
		ImGui::PopStyleColor();
	}

	ImGui::Checkbox("Apply Cycle to every wavetable", &exportCycle);
	ImGui::Checkbox("Remove macOS ._ and .DS_Store files afterwards", &removeJunk);
	ImGui::Dummy(ImVec2(0, 4));

	bool canExport = hasDest && card.filledCount() > 0;
	if (!canExport)
		ImGui::TextDisabled(card.filledCount() == 0 ? "The card is empty." : "Choose where to export.");
	if (canExport && accentButton("  Export  ")) {
		ExportOptions options;
		options.existing = existingChoice == 1 ? ExportOptions::MERGE : ExportOptions::BACKUP;
		options.fix = fixChoice == 1 ? FIX_PAD : FIX_RESAMPLE;
		options.removeJunk = removeJunk;
		options.cycle = exportCycle;
		lastResult = exportCard(card, exportDest, options);
		setStatus(lastResult.ok ? "Exported " + std::to_string(lastResult.written) + " files" : "Export finished with errors");
		ImGui::CloseCurrentPopup();
		openSummary = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel"))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

static void summaryDialog() {
	if (openSummary) {
		ImGui::OpenPopup("Export Summary");
		openSummary = false;
	}
	ImGui::SetNextWindowContentWidth(520);
	if (!ImGui::BeginPopupModal("Export Summary", NULL, ImGuiWindowFlags_AlwaysAutoResize))
		return;
	const ExportResult &r = lastResult;
	if (r.ok)
		ImGui::Text("Wrote %d wavetables to %s", r.written, r.osirisPath.c_str());
	else
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(warnColor()), "Wrote %d wavetables, with %d %s", r.written, (int)r.errors.size(), r.errors.size() == 1 ? "error" : "errors");
	ImGui::Text("Bank A: %d   Bank B: %d   Bank C: %d   Bank D: %d", r.perBank[0], r.perBank[1], r.perBank[2], r.perBank[3]);
	if (!r.backupPath.empty())
		ImGui::TextWrapped("Previous files backed up to %s", r.backupPath.c_str());
	if (!r.moved.empty())
		ImGui::Text("%d replaced %s moved to the backup", (int)r.moved.size(), r.moved.size() == 1 ? "file" : "files");
	if (r.junkRemoved)
		ImGui::Text("Removed %d macOS ._ / .DS_Store %s", r.junkRemoved, r.junkRemoved == 1 ? "file" : "files");
	for (const std::string &e : r.errors)
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(warnColor()), "%s", e.c_str());
	ImGui::Dummy(ImVec2(0, 4));
	ImGui::TextDisabled("Eject the card in Finder before taking it out.");
	if (ImGui::Button("OK", ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

static void sidebar(const std::vector<Issue> &issues) {
	const Slot &slot = card.slots[selBank][selSlot];

	// Selected slot
	ui::SectionHeading("Selected slot");
	ImGui::Text("%s  %s", slotLabel(selBank, selSlot).c_str(), slot.filled ? (slot.name.empty() ? "(unnamed)" : slot.name.c_str()) : "Empty");
	{
		ImDrawList *dl = ImGui::GetWindowDrawList();
		ImVec2 a = ImGui::GetCursorScreenPos();
		ImVec2 size = ImVec2(ImGui::GetContentRegionAvailWidth(), ImGui::GetContentRegionAvail().y > 560.f ? 110.f : 72.f);
		dl->AddRectFilled(a, a + size, theme.display, ImGui::GetStyle().FrameRounding);
		if (theme.outline)
			dl->AddRect(a, a + size, theme.outline, ImGui::GetStyle().FrameRounding);
		if (slot.filled) {
			// Every wave in the table, faint, with the first and last brighter
			const int points = 96;
			ImVec2 pts[points];
			float midY = a.y + size.y * 0.5f, halfH = size.y * 0.5f - 8.f;
			int step = std::max(1, slot.waveCount / 32);
			for (int w = 0; w < slot.waveCount; w += step) {
				for (int i = 0; i < points; i++) {
					float v = slot.samples[w * WAVE_SAMPLES + i * WAVE_SAMPLES / points];
					pts[i] = ImVec2(a.x + 8.f + (size.x - 16.f) * i / (points - 1), midY - clampf(v, -1.f, 1.f) * halfH);
				}
				float t = slot.waveCount > 1 ? (float)w / (slot.waveCount - 1) : 0.f;
				dl->AddPolyline(pts, points, colAlpha(colMix(theme.accent, theme.accent2, t), 0.35f), false, 1.f, true);
			}
		}
		ImGui::Dummy(size);
	}
	if (slot.filled) {
		if (slot.waveCount == WAVES)
			ImGui::TextDisabled("32 waves");
		else
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(warnColor()), "%d waves (needs to be 32)", slot.waveCount);
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextDisabled("From %s", slot.source.c_str());
		ImGui::PopTextWrapPos();
		if (ImGui::Button("Open in Editor"))
			openInEditor(selBank, selSlot);
		ImGui::SameLine();
		if (ImGui::Button("Rename")) {
			menuBank = selBank;
			menuSlot = selSlot;
			snprintf(renameBuf, sizeof(renameBuf), "%s", slot.name.c_str());
			openRename = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
			card.slots[selBank][selSlot].clear();
	}
	else {
		if (ImGui::Button("Add Editor Wavetable"))
			slotFromEditor(selBank, selSlot);
	}

	// File names for the selected bank
	ui::SectionHeading(("Files in Osiris/" + std::string(1, bankLetter(selBank))).c_str());
	// The list shrinks on small windows so the Checks below stay visible
	float namesHeight = clampf(ImGui::GetContentRegionAvail().y - 210.f, 70.f, 190.f);
	ImGui::BeginChild("file names", ImVec2(0, namesHeight), false);
	{
		int shown = 0;
		for (int s = 0; s < SLOTS; s++) {
			if (!card.slots[selBank][s].filled)
				continue;
			std::string f = card.fileNameFor(selBank, s);
			bool sel = (s == selSlot);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sel ? theme.accent : theme.text));
			if (ImGui::Selectable(f.c_str(), sel))
				selSlot = s;
			ImGui::PopStyleColor();
			shown++;
		}
		if (!shown)
			ImGui::TextDisabled("Nothing in bank %c yet", bankLetter(selBank));
	}
	ImGui::EndChild();

	// Checks
	ui::SectionHeading("Checks");
	int waveCountIssues = 0, gaps = 0, blankIssues = 0;
	bool empty = false;
	for (const Issue &i : issues) {
		blankIssues += i.kind == Issue::BLANK_WAVES;
		waveCountIssues += i.kind == Issue::WAVE_COUNT;
		gaps += i.kind == Issue::GAP;
		empty = empty || i.kind == Issue::EMPTY_CARD;
	}
	if (issues.empty()) {
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.accent), "Ready to export");
	}
	int buttonRows = (waveCountIssues || gaps ? 1 : 0) + (blankIssues ? 1 : 0);
	ImGui::BeginChild("checks", ImVec2(0, -(ImGui::GetItemsLineHeightWithSpacing() * buttonRows) - (buttonRows ? 4.f : 0.f)), false);
	for (const Issue &i : issues) {
		ImU32 col = (i.kind == Issue::DUPLICATE_NAME || i.kind == Issue::EMPTY_CARD) ? theme.textDim : warnColor();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
		ImGui::PushTextWrapPos(0.f);
		if (i.slot >= 0) {
			char id[64];
			snprintf(id, sizeof(id), "%s##issue%d_%d_%d", i.text.c_str(), (int)i.kind, i.bank, i.slot);
			if (ImGui::Selectable(id)) {
				selBank = i.bank;
				selSlot = i.slot;
			}
		}
		else {
			ImGui::TextUnformatted(i.text.c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}
	ImGui::EndChild();
	if (waveCountIssues) {
		if (ImGui::Button("Resample All to 32"))
			fixAll(FIX_RESAMPLE);
		ImGui::SameLine();
		if (ImGui::Button("Pad All to 32"))
			fixAll(FIX_PAD);
		if (gaps)
			ImGui::SameLine();
	}
	if (gaps && ImGui::Button("Close Gaps")) {
		cardSnapshot("Close Gaps");
		card.closeGaps();
	}
	if (blankIssues && ImGui::Button("Fill Blank Waves...")) {
		for (int b = 0; b < BANKS; b++)
			for (int s = 0; s < SLOTS; s++)
				if (card.blanks(b, s).any())
					blankCheckSlot(b, s, true);
	}
}

void cardPage() {
	ImGui::BeginChild("SD Card", ImVec2(0, 0), true);

	// Toolbar
	if (ImGui::Button("Open Card..."))
		openSdCard();
	ImGui::SameLine();
	if (ImGui::Button("Open Project..."))
		openProject();
	ImGui::SameLine();
	if (ImGui::Button("Save Project..."))
		saveProjectAs();
	ImGui::SameLine(0, 18);
	if (ImGui::Button("Add From Editor")) {
		int b, s;
		if (nextFree(b, s))
			slotFromEditor(b, s);
		else
			setStatus("The card is full");
	}
	ImGui::SameLine();
	if (ImGui::Button("Add WAV..."))
		addWavFile();
	ImGui::SameLine();
	if (ImGui::Button("Add Folder..."))
		addFolder();
	ImGui::SameLine(0, 18);
	if (ImGui::Button("Cycle All Slots"))
		openCycleConfirm = true;
	ImGui::SameLine();
	if (ImGui::Button("Close Gaps")) {
		cardSnapshot("Close Gaps");
		card.closeGaps();
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear Card"))
		ImGui::OpenPopup("Clear Card?");
	if (openCycleConfirm) {
		ImGui::OpenPopup("Cycle All Slots?");
		openCycleConfirm = false;
	}
	if (ImGui::BeginPopupModal("Cycle All Slots?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("Apply Cycle to every wave in all %d wavetables on the card?", card.filledCount());
		ImGui::TextDisabled("Cycle lines up the end of each wave with its start, removing the click at the loop point.");
		ImGui::TextDisabled("You can undo this with the Undo button.");
		if (accentButton("Cycle All")) {
			cardSnapshot("Cycle All Slots");
			int n = 0;
			for (int b = 0; b < BANKS; b++)
				for (int s = 0; s < SLOTS; s++)
					if (card.slots[b][s].filled) {
						cycleSlot(card.slots[b][s]);
						n++;
					}
			setStatus("Cycled " + std::to_string(n) + (n == 1 ? " wavetable" : " wavetables"));
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopupModal("Clear Card?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("Remove all %d wavetables from the layout?", card.filledCount());
		ImGui::TextDisabled("Nothing on your SD card is touched.");
		if (ImGui::Button("Clear", ImVec2(120, 0))) {
			cardSnapshot("Clear Card");
			card.clear();
			syncPackBuffers();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// Second row: option, status, and the Export button on the right
	ImGui::Checkbox("Put slot names in file names", &card.slotNamesInFileNames);
	if (undoAvailable) {
		ImGui::SameLine(0, 18);
		if (ImGui::Button(("Undo " + undoLabel).c_str())) {
			Card redo = card;
			card = undoCard;
			undoCard = redo;
			syncPackBuffers();
			setStatus("Undid " + undoLabel);
			undoAvailable = false;
		}
	}
	{
		const char *exportLabel = "Export to SD Card...";
		float exportW = ImGui::CalcTextSize(exportLabel).x + ImGui::GetStyle().FramePadding.x * 2.f;
		float exportX = ImGui::GetWindowContentRegionMax().x - exportW;
		ImGui::SameLine(0, 18);
		// Shorten the status line so it never runs under the Export button
		std::string line = std::to_string(card.filledCount()) + " / 128 slots   " + status;
		float room = exportX - ImGui::GetCursorPosX() - 12.f;
		if (ImGui::CalcTextSize(line.c_str()).x > room) {
			while (!line.empty() && ImGui::CalcTextSize((line + "...").c_str()).x > room)
				line.pop_back();
			line += "...";
		}
		ImGui::AlignFirstTextHeightToWidgets();
		ImGui::TextDisabled("%s", line.c_str());
		ImGui::SameLine(exportX);
		if (accentButton(exportLabel))
			startExport();
	}

	SlotInfo info[BANKS][SLOTS];
	card.getSlotInfo(info);
	std::vector<Issue> issues = checkLayout(info, card.slotNamesInFileNames);

	// Banks on the left, details on the right
	const float sidebarWidth = clampf(ImGui::GetContentRegionAvailWidth() * 0.26f, 260.f, 360.f);
	ImGui::BeginChild("banks", ImVec2(-sidebarWidth - ImGui::GetStyle().ItemSpacing.x, 0), false);
	{
		float spacing = 10.f;
		float colW = (ImGui::GetContentRegionAvailWidth() - spacing * (BANKS - 1)) / BANKS;

		// Bank headers with pack names
		ImVec2 origin = ImGui::GetCursorScreenPos();
		for (int b = 0; b < BANKS; b++) {
			ImGui::SetCursorScreenPos(ImVec2(origin.x + b * (colW + spacing), origin.y));
			ImGui::BeginGroup();
			int count = 0;
			for (int s = 0; s < SLOTS; s++)
				count += card.slots[b][s].filled;
			char heading[32];
			snprintf(heading, sizeof(heading), "Bank %c  %d/32", bankLetter(b), count);
			ui::SectionHeading(heading);
			ImGui::PushItemWidth(colW);
			char id[16];
			snprintf(id, sizeof(id), "##pack%d", b);
			if (ImGui::InputText(id, packBuf[b], sizeof(packBuf[b])))
				card.pack[b] = packBuf[b];
			// Placeholder when empty
			if (packBuf[b][0] == '\0' && !ImGui::IsItemActive()) {
				ImVec2 min = ImGui::GetItemRectMin();
				ImGui::GetWindowDrawList()->AddText(min + ImGui::GetStyle().FramePadding, colAlpha(theme.textDim, 0.6f), "Pack name");
			}
			ImGui::PopItemWidth();
			ImGui::EndGroup();
		}

		// Slots: all four banks scroll together
		ImGui::BeginChild("slots", ImVec2(0, 0), false);
		{
			float rowH = 34.f, gap = 4.f;
			ImVec2 top = ImGui::GetCursorScreenPos();
			for (int s = 0; s < SLOTS; s++)
				for (int b = 0; b < BANKS; b++)
					drawSlotRow(b, s, ImVec2(top.x + b * (colW + spacing), top.y + s * (rowH + gap)), ImVec2(colW, rowH));
			ImGui::SetCursorScreenPos(top);
			ImGui::Dummy(ImVec2(colW * BANKS, SLOTS * (rowH + gap)));
			slotMenu();
		}
		ImGui::EndChild();
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("details", ImVec2(0, 0), true);
	sidebar(issues);
	if (!notes.empty()) {
		ui::SectionHeading("Notes");
		for (const std::string &n : notes)
			ImGui::TextWrapped("%s", n.c_str());
	}
	ImGui::EndChild();

	// Drag feedback
	if (dragging) {
		const Slot &slot = card.slots[dragBank][dragSlot];
		ImGui::SetTooltip("Move %s  %s", slotLabel(dragBank, dragSlot).c_str(), slot.name.c_str());
		if (!ImGui::IsMouseDown(0))
			dragging = false;
	}

	renameDialog();
	exportDialog(issues);
	summaryDialog();
	ImGui::EndChild();
}

/** Screenshot mode: a populated card so the page shows what it looks like in use */
void cardPageDemo(const char *banksDir) {
	std::vector<std::string> messages;
	card.clear();
	card.pack[0] = "Digital";
	card.pack[1] = "Glitch";
	card.pack[2] = "Factory";
	const char *folders[] = {"catalog/00Digital", "catalog/01Analog", "catalog/02FM", "catalog/03Glitch"};
	for (const char *f : folders)
		fillFromFiles(card, listWavFiles(f), 0, 0, messages);
	// The catalog files are single waves; make them full tables, except one to show the check
	fixAll(FIX_RESAMPLE);
	card.slots[1][5].waveCount = 1;
	card.slots[1][5].samples.resize(WAVE_SAMPLES);
	if (banksDir)
		fillFromFiles(card, listWavFiles(banksDir), 2, 0, messages);
	syncPackBuffers();
	selBank = 0;
	selSlot = 2;
	status = "";
}
