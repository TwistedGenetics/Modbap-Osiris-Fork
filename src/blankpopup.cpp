// The "Blank Waves Found" pop-up: shown when a wavetable with silent waves is loaded, offering to fill the gaps by
// morphing between the waves either side. The maths is in wtprocess.cpp.

#include "OsirisEdit.hpp"
#include "cardpage.hpp"
#include "theme.hpp"
#include "wtprocess.hpp"

#include "imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"

#include <SDL.h>
#include <deque>

int blankPrefMode = BLANK_ASK;
int blankPrefMorph = wt::CROSSFADE;
int blankPrefEdges = wt::EDGE_HOLD;

namespace {

struct Job {
	bool editor;    // the wavetable in the editor, or a slot on the SD card page
	int bank, slot; // for slots
	int sourceWaves;
	std::string name;
};

std::deque<Job> jobs;
bool openRequested = false;
int morphChoice = wt::CROSSFADE;
int edgeChoice = wt::EDGE_HOLD;
bool dontAsk = false;
bool applyToRest = true;
bool snapshotTaken = false;

// Preview of the current job with the current options
int previewSerial = -1, previewMorph = -1, previewEdges = -1;
int jobSerial = 0;
std::vector<float> previewBefore, previewAfter;

// Short message shown after a fill that happened without asking
std::string notice;
Uint32 noticeUntil = 0;

ImU32 warnColor() {
	return theme.modern ? IM_COL32(0xf5, 0xb5, 0x4a, 0xff) : IM_COL32(0xc0, 0x60, 0x00, 0xff);
}

/** The samples a job refers to. For the editor this is a copy (written back by apply()). */
bool getSamples(const Job &job, std::vector<float> &out, int &waveCount) {
	if (job.editor) {
		waveCount = BANK_LEN;
		out.resize((size_t)BANK_LEN * WAVE_LEN);
		for (int w = 0; w < BANK_LEN; w++)
			memcpy(&out[(size_t)w * WAVE_LEN], currentBank.waves[w].samples, sizeof(float) * WAVE_LEN);
		return true;
	}
	osiris::Slot *slot = cardSlot(job.bank, job.slot);
	if (!slot || !slot->filled)
		return false;
	waveCount = slot->waveCount;
	out = slot->samples;
	return (int)out.size() >= waveCount * WAVE_LEN;
}

std::string jobLabel(const Job &job) {
	if (job.editor)
		return job.name.empty() ? "This wavetable" : job.name;
	return std::string(1, osiris::bankLetter(job.bank)) + std::to_string(job.slot + 1) + (job.name.empty() ? "" : "  " + job.name);
}

/** Fills one job's blank waves. Returns how many waves were filled. */
int apply(const Job &job, wt::MorphMode morph, wt::EdgeMode edges) {
	std::vector<float> samples;
	int waveCount = 0;
	if (!getSamples(job, samples, waveCount))
		return 0;
	wt::BlankReport report = wt::findBlanks(samples.data(), waveCount, job.sourceWaves);
	if (!report.any())
		return 0;
	if (!job.editor && !snapshotTaken) {
		cardSnapshot("Fill Blank Waves");
		snapshotTaken = true;
	}
	int filled = wt::fillBlanks(samples.data(), waveCount, report, morph, edges);
	if (job.editor) {
		for (int w = 0; w < BANK_LEN; w++) {
			if (memcmp(currentBank.waves[w].samples, &samples[(size_t)w * WAVE_LEN], sizeof(float) * WAVE_LEN) != 0) {
				memcpy(currentBank.waves[w].samples, &samples[(size_t)w * WAVE_LEN], sizeof(float) * WAVE_LEN);
				currentBank.waves[w].commitSamples();
			}
		}
		// One undo step for the whole fill
		historyPushNew();
	}
	else {
		cardSlot(job.bank, job.slot)->samples = samples;
	}
	return filled;
}

void showNotice(const std::string &text) {
	notice = text;
	noticeUntil = SDL_GetTicks() + 4500;
}

/** Strip of waves: one column per wave, blank ones highlighted. `filledFrom` marks waves changed by the preview. */
void drawStrip(const char *id, const std::vector<float> &samples, int waveCount, const wt::BlankReport &report, const std::vector<float> *before) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 a = ImGui::GetCursorScreenPos();
	float width = ImGui::GetContentRegionAvailWidth();
	float height = 54.f;
	ImGui::InvisibleButton(id, ImVec2(width, height));
	dl->AddRectFilled(a, a + ImVec2(width, height), theme.display, 4.f);

	int shown = waveCount;
	float colW = width / shown;
	std::vector<bool> isBlank(waveCount, false);
	for (int w : report.blank)
		isBlank[w] = true;
	for (int w = 0; w < shown; w++) {
		ImVec2 c0 = ImVec2(a.x + colW * w, a.y);
		ImVec2 c1 = ImVec2(a.x + colW * (w + 1) - (colW > 4.f ? 1.f : 0.f), a.y + height);
		bool padding = report.paddedFrom >= 0 && w >= report.paddedFrom;
		bool changed = before && memcmp(&(*before)[(size_t)w * WAVE_LEN], &samples[(size_t)w * WAVE_LEN], sizeof(float) * WAVE_LEN) != 0;
		if (padding)
			dl->AddRectFilled(c0, c1, colAlpha(theme.textDim, 0.08f));
		else if (changed)
			dl->AddRectFilled(c0, c1, colAlpha(theme.accent2, 0.22f));
		else if (isBlank[w] && !before)
			dl->AddRectFilled(c0, c1, colAlpha(warnColor(), 0.28f));
		ImU32 col = changed ? colMix(theme.accent2, theme.text, 0.3f) : (padding ? colAlpha(theme.textDim, 0.4f) : theme.accent);
		const float *wave = &samples[(size_t)w * WAVE_LEN];
		if (colW >= 6.f) {
			const int points = 24;
			ImVec2 pts[points];
			float mid = (c0.y + c1.y) * 0.5f, half = height * 0.5f - 5.f;
			for (int i = 0; i < points; i++) {
				float v = wave[i * WAVE_LEN / points];
				pts[i] = ImVec2(c0.x + 1.f + (c1.x - c0.x - 2.f) * i / (points - 1), mid - (v < -1.f ? -1.f : (v > 1.f ? 1.f : v)) * half);
			}
			dl->AddPolyline(pts, points, col, false, 1.f, true);
		}
		else {
			// Too narrow for a waveform: show the peak level
			float peak = 0.f;
			for (int i = 0; i < WAVE_LEN; i++)
				peak = fmaxf(peak, fabsf(wave[i]));
			float h = fminf(peak, 1.f) * (height - 6.f);
			dl->AddRectFilled(ImVec2(c0.x, c1.y - 3.f - h), ImVec2(c1.x, c1.y - 3.f), col);
		}
	}
	// Wave numbers every so often
	int step = shown <= 32 ? 4 : (shown <= 64 ? 8 : shown / 8);
	for (int w = 0; w < shown; w += step) {
		char num[8];
		snprintf(num, sizeof(num), "%d", w + 1);
		dl->AddText(ImVec2(a.x + colW * w + 2.f, a.y + height - ImGui::GetFontSize() - 1.f), colAlpha(theme.textDim, 0.7f), num);
	}
}

void finishJob(bool fill, bool rememberChoice) {
	wt::MorphMode morph = (wt::MorphMode)morphChoice;
	wt::EdgeMode edges = (wt::EdgeMode)edgeChoice;
	if (fill) {
		blankPrefMorph = morphChoice;
		blankPrefEdges = edgeChoice;
	}
	if (rememberChoice)
		blankPrefMode = fill ? BLANK_ALWAYS : BLANK_NEVER;

	int filledWaves = 0, filledTables = 0;
	Job job = jobs.front();
	jobs.pop_front();
	if (fill) {
		int n = apply(job, morph, edges);
		filledWaves += n;
		filledTables += n > 0;
	}
	if (applyToRest) {
		while (!jobs.empty()) {
			if (fill) {
				int n = apply(jobs.front(), morph, edges);
				filledWaves += n;
				filledTables += n > 0;
			}
			jobs.pop_front();
		}
	}
	if (!job.editor && filledWaves > 0)
		cardSetStatus("Filled " + std::to_string(filledWaves) + " blank waves in " + std::to_string(filledTables) + (filledTables == 1 ? " slot" : " slots"));
	jobSerial++;
	if (jobs.empty())
		snapshotTaken = false;
	else
		openRequested = true;
}

void enqueue(const Job &job, bool force) {
	std::vector<float> samples;
	int waveCount = 0;
	if (!getSamples(job, samples, waveCount))
		return;
	wt::BlankReport report = wt::findBlanks(samples.data(), waveCount, job.sourceWaves);
	if (!report.any())
		return;
	if (!force && blankPrefMode == BLANK_NEVER)
		return;
	if (!force && blankPrefMode == BLANK_ALWAYS) {
		int n = apply(job, (wt::MorphMode)blankPrefMorph, (wt::EdgeMode)blankPrefEdges);
		snapshotTaken = false;
		if (n > 0)
			showNotice("Filled " + std::to_string(n) + " blank " + (n == 1 ? "wave" : "waves") + " in " + jobLabel(job) + "  (Preferences > Blank Waves)");
		return;
	}
	if (jobs.empty()) {
		morphChoice = blankPrefMorph;
		edgeChoice = blankPrefEdges;
		dontAsk = false;
		applyToRest = true;
	}
	jobs.push_back(job);
	openRequested = true;
}

} // namespace

void blankCheckEditor(int sourceWaves, const std::string &name) {
	Job job;
	job.editor = true;
	job.bank = job.slot = -1;
	job.sourceWaves = sourceWaves < 0 ? BANK_LEN : sourceWaves;
	job.name = name;
	enqueue(job, false);
}

void blankCheckSlot(int bank, int slot, bool force) {
	osiris::Slot *s = cardSlot(bank, slot);
	if (!s || !s->filled)
		return;
	Job job;
	job.editor = false;
	job.bank = bank;
	job.slot = slot;
	job.sourceWaves = s->waveCount;
	job.name = s->name;
	enqueue(job, force);
}

bool blankPopupPending() {
	return !jobs.empty();
}

void blankPreferencesMenu() {
	ImGui::TextDisabled("When a wavetable has blank waves");
	if (ImGui::MenuItem("Ask", NULL, blankPrefMode == BLANK_ASK))
		blankPrefMode = BLANK_ASK;
	if (ImGui::MenuItem("Always Fill", NULL, blankPrefMode == BLANK_ALWAYS))
		blankPrefMode = BLANK_ALWAYS;
	if (ImGui::MenuItem("Never Fill", NULL, blankPrefMode == BLANK_NEVER))
		blankPrefMode = BLANK_NEVER;
	ImGui::Separator();
	ImGui::TextDisabled("Fill by");
	if (ImGui::MenuItem("Crossfade", NULL, blankPrefMorph == wt::CROSSFADE))
		blankPrefMorph = wt::CROSSFADE;
	if (ImGui::MenuItem("Spectral Morph", NULL, blankPrefMorph == wt::SPECTRAL))
		blankPrefMorph = wt::SPECTRAL;
	ImGui::Separator();
	ImGui::TextDisabled("Blank waves at the start or end");
	if (ImGui::MenuItem("Hold Nearest Wave", NULL, blankPrefEdges == wt::EDGE_HOLD))
		blankPrefEdges = wt::EDGE_HOLD;
	if (ImGui::MenuItem("Morph Round to the Other End", NULL, blankPrefEdges == wt::EDGE_WRAP))
		blankPrefEdges = wt::EDGE_WRAP;
	if (ImGui::MenuItem("Leave Blank", NULL, blankPrefEdges == wt::EDGE_LEAVE))
		blankPrefEdges = wt::EDGE_LEAVE;
}

void blankRenderPopup() {
	// Notice after an automatic fill
	if (!notice.empty() && SDL_GetTicks() < noticeUntil) {
		ImGuiIO &io = ImGui::GetIO();
		ImVec2 size = ImGui::CalcTextSize(notice.c_str()) + ImVec2(28, 18);
		ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - size.x) * 0.5f, io.DisplaySize.y - size.y - 24.f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.control, 0.97f)));
		ImGui::Begin("##blank notice", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
		ImGui::TextUnformatted(notice.c_str());
		ImGui::End();
		ImGui::PopStyleColor();
	}

	if (jobs.empty())
		return;
	if (openRequested) {
		ImGui::OpenPopup("Blank Waves Found");
		openRequested = false;
	}
	ImGui::SetNextWindowContentWidth(620);
	if (!ImGui::BeginPopupModal("Blank Waves Found", NULL, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	const Job &job = jobs.front();
	std::vector<float> samples;
	int waveCount = 0;
	if (!getSamples(job, samples, waveCount)) {
		// The slot went away; move on
		jobs.pop_front();
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		if (!jobs.empty())
			openRequested = true;
		return;
	}
	wt::BlankReport report = wt::findBlanks(samples.data(), waveCount, job.sourceWaves);

	if (theme.fontHeading)
		ImGui::PushFont(theme.fontHeading);
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textDim), "%s", jobLabel(job).c_str());
	if (theme.fontHeading)
		ImGui::PopFont();
	ImGui::Text("This wavetable has %d blank %s out of %d.", report.blankCount, report.blankCount == 1 ? "wave" : "waves", report.waveCount);
	if (report.paddedFrom >= 0)
		ImGui::TextDisabled("The file has %d waves, so waves %d-%d are only empty because Waves is set to %d. They're left alone.",
			report.paddedFrom, report.paddedFrom + 1, waveCount, waveCount);

	// Runs
	int listed = 0;
	for (const wt::Run &run : report.runs) {
		if (listed == 6) {
			ImGui::TextDisabled("and %d more", (int)report.runs.size() - listed);
			break;
		}
		ImGui::Bullet();
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(warnColor()), "%s", wt::BlankReport::describe(run).c_str());
		listed++;
	}

	// Options
	ui::SectionHeading("Fill gaps by morphing");
	ImGui::RadioButton("Crossfade", &morphChoice, wt::CROSSFADE);
	ImGui::SameLine(150);
	ImGui::TextDisabled("blend the two waves sample by sample");
	ImGui::RadioButton("Spectral", &morphChoice, wt::SPECTRAL);
	ImGui::SameLine(150);
	ImGui::TextDisabled("blend harmonics; keeps its level when the waves are out of phase");
	if (report.hasEdgeRuns()) {
		ui::SectionHeading("Blank waves at the start or end");
		ImGui::RadioButton("Hold nearest wave", &edgeChoice, wt::EDGE_HOLD);
		ImGui::SameLine();
		ImGui::RadioButton("Morph round to the other end", &edgeChoice, wt::EDGE_WRAP);
		ImGui::SameLine();
		ImGui::RadioButton("Leave blank", &edgeChoice, wt::EDGE_LEAVE);
	}

	// Before and after
	if (previewSerial != jobSerial || previewMorph != morphChoice || previewEdges != edgeChoice || previewBefore != samples) {
		previewBefore = samples;
		previewAfter = samples;
		wt::fillBlanks(previewAfter.data(), waveCount, report, (wt::MorphMode)morphChoice, (wt::EdgeMode)edgeChoice);
		previewSerial = jobSerial;
		previewMorph = morphChoice;
		previewEdges = edgeChoice;
	}
	ui::SectionHeading("Now");
	drawStrip("##before", previewBefore, waveCount, report, NULL);
	ui::SectionHeading("After filling");
	drawStrip("##after", previewAfter, waveCount, report, &previewBefore);

	ImGui::Dummy(ImVec2(0, 2));
	ImGui::Checkbox("Don't ask again", &dontAsk);
	if (dontAsk) {
		ImGui::SameLine();
		ImGui::TextDisabled("(change it under Preferences > Blank Waves)");
	}
	if (jobs.size() > 1)
		ImGui::Checkbox(("Do the same for the other " + std::to_string(jobs.size() - 1) + (jobs.size() == 2 ? " wavetable" : " wavetables")).c_str(), &applyToRest);

	ImGui::Dummy(ImVec2(0, 2));
	ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.22f)));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(colAlpha(theme.accent, 0.32f)));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme.modern ? theme.accent : theme.text));
	bool fill = ImGui::Button("Fill Gaps", ImVec2(130, 0));
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	bool leave = ImGui::Button("Leave As Is", ImVec2(130, 0));
	if (fill || leave) {
		finishJob(fill, dontAsk);
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}
