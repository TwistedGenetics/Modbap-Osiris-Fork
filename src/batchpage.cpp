// File > Batch Process Folder: applies Cycle / Normalize / blank-wave filling to every wavetable in a folder.
// The processing is in batch.cpp; this is the dialog and the worker thread.

#include "OsirisEdit.hpp"
#include "batch.hpp"
#include "cardio.hpp"
#include "theme.hpp"

#include "imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"

#include "osdialog/osdialog.h"

#include <SDL.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace {

enum State {
	SETUP,
	RUNNING,
	DONE,
};

State state = SETUP;
bool openRequested = false;
bool closeRequested = false;
bool openOverwriteConfirm = false;

char sourceDir[1024] = "";
char outputDir[1024] = "";
bool outputEdited = false; // stop following the source once the output has been chosen by hand
bool overwrite = false;
batch::Options options;
int morphChoice = wt::CROSSFADE;
int edgeChoice = wt::EDGE_HOLD;

// Worker
std::thread worker;
std::atomic<int> progress(0);
std::atomic<bool> cancelFlag(false);
std::atomic<bool> finished(false);
std::mutex resultsMutex;
std::vector<batch::FileResult> results;
std::string currentFile;
int total = 0;
bool wasCancelled = false;
std::string runOutput;

// Folder listing, refreshed when the source changes and every second (files can be added or removed meanwhile)
std::string listedDir;
Uint32 listedAt = 0;
std::vector<std::string> files;

ImU32 warnColor() {
	return theme.modern ? IM_COL32(0xf5, 0xb5, 0x4a, 0xff) : IM_COL32(0xc0, 0x60, 0x00, 0xff);
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

void refreshFiles(bool force = false) {
	bool changed = listedDir != sourceDir;
	if (!changed && !force && SDL_GetTicks() - listedAt < 1000)
		return;
	listedDir = sourceDir;
	listedAt = SDL_GetTicks();
	files = sourceDir[0] ? osiris::listWavFiles(sourceDir) : std::vector<std::string>();
	if (!changed)
		return;
	if (!outputEdited)
		snprintf(outputDir, sizeof(outputDir), "%s", sourceDir[0] ? batch::defaultOutputDir(sourceDir).c_str() : "");
}

void joinWorker() {
	if (worker.joinable())
		worker.join();
}

void startRun() {
	joinWorker();
	refreshFiles(true);
	std::string out = overwrite ? std::string(sourceDir) : std::string(outputDir);
	runOutput = out;
	results.clear();
	currentFile.clear();
	total = (int)files.size();
	progress = 0;
	cancelFlag = false;
	finished = false;
	wasCancelled = false;
	options.morph = (wt::MorphMode)morphChoice;
	options.edges = (wt::EdgeMode)edgeChoice;
	state = RUNNING;

	std::vector<std::string> list = files;
	batch::Options opts = options;
	worker = std::thread([list, out, opts]() {
		if (!batch::makeDirs(out)) {
			batch::FileResult r;
			r.name = out;
			r.reason = "the output folder couldn't be created";
			std::lock_guard<std::mutex> lock(resultsMutex);
			results.push_back(r);
			finished = true;
			return;
		}
		for (const std::string &src : list) {
			if (cancelFlag)
				break;
			{
				std::lock_guard<std::mutex> lock(resultsMutex);
				currentFile = src.substr(src.find_last_of('/') + 1);
			}
			batch::FileResult r = batch::processFile(src, batch::outputPath(src, out), opts);
			{
				std::lock_guard<std::mutex> lock(resultsMutex);
				results.push_back(r);
			}
			progress++;
		}
		finished = true;
	});
}

void browse(char *buf, size_t size) {
	char *path = osdialog_file(OSDIALOG_OPEN_DIR, buf[0] ? buf : NULL, NULL, NULL);
	if (path) {
		snprintf(buf, size, "%s", path);
		free(path);
	}
}

void setupView() {
	refreshFiles();
	ImGui::TextWrapped("Processes every WAV wavetable in a folder, the same as opening each one and using Cycle All / "
		"Normalize All. Files keep their names and format (wave count, sample rate, bit depth).");

	ui::SectionHeading("Source folder");
	ImGui::PushItemWidth(-110);
	ImGui::InputText("##batchSource", sourceDir, sizeof(sourceDir));
	ImGui::PopItemWidth();
	ImGui::SameLine();
	if (ImGui::Button("Browse...##batchSource", ImVec2(100, 0))) {
		browse(sourceDir, sizeof(sourceDir));
	}
	refreshFiles();
	if (sourceDir[0]) {
		if (files.empty())
			ImGui::TextDisabled("No WAV files in that folder");
		else
			ImGui::TextDisabled("%d WAV %s, in number order", (int)files.size(), files.size() == 1 ? "file" : "files");
	}

	ui::SectionHeading("Processing");
	ImGui::Checkbox("Cycle all waves", &options.cycle);
	ImGui::SameLine(250);
	ImGui::TextDisabled("lines up each wave's end with its start");
	ImGui::Checkbox("Normalize all waves", &options.normalize);
	ImGui::SameLine(250);
	ImGui::TextDisabled("scales each wave to full level");
	ImGui::Checkbox("Fill blank frames", &options.fill);
	ImGui::SameLine(250);
	ImGui::TextDisabled("morphs across silent waves");
	if (options.fill) {
		ImGui::Indent(28);
		ImGui::RadioButton("Crossfade##batch", &morphChoice, wt::CROSSFADE);
		ImGui::SameLine();
		ImGui::RadioButton("Spectral##batch", &morphChoice, wt::SPECTRAL);
		ImGui::AlignFirstTextHeightToWidgets();
		ImGui::TextDisabled("At the start or end:");
		ImGui::SameLine();
		ImGui::RadioButton("Hold nearest##batch", &edgeChoice, wt::EDGE_HOLD);
		ImGui::SameLine();
		ImGui::RadioButton("Morph round##batch", &edgeChoice, wt::EDGE_WRAP);
		ImGui::SameLine();
		ImGui::RadioButton("Leave blank##batch", &edgeChoice, wt::EDGE_LEAVE);
		ImGui::Unindent(28);
	}

	ui::SectionHeading("Output");
	if (!overwrite) {
		ImGui::PushItemWidth(-110);
		if (ImGui::InputText("##batchOutput", outputDir, sizeof(outputDir)))
			outputEdited = true;
		ImGui::PopItemWidth();
		ImGui::SameLine();
		if (ImGui::Button("Browse...##batchOutput", ImVec2(100, 0))) {
			browse(outputDir, sizeof(outputDir));
			outputEdited = true;
		}
	}
	ImGui::Checkbox("Overwrite originals", &overwrite);
	if (overwrite) {
		ImGui::SameLine();
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(warnColor()), "the files in the source folder will be replaced");
	}

	// What's stopping it from running
	std::string problem;
	bool anything = options.cycle || options.normalize || options.fill;
	if (!sourceDir[0])
		problem = "Choose a source folder.";
	else if (files.empty())
		problem = "There are no WAV files to process.";
	else if (!anything)
		problem = "Choose at least one kind of processing.";
	else if (!overwrite && !outputDir[0])
		problem = "Choose an output folder.";
	else if (!overwrite && batch::sameDir(sourceDir, outputDir))
		problem = "The output folder is the source folder. Choose another folder, or tick Overwrite originals.";
	if (problem.empty() && !overwrite) {
		int existing = batch::countExisting(files, outputDir);
		if (existing)
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(warnColor()), "%d %s in the output folder will be replaced", existing, existing == 1 ? "file" : "files");
	}
	if (!problem.empty())
		ImGui::TextDisabled("%s", problem.c_str());

	ImGui::Dummy(ImVec2(0, 4));
	if (problem.empty() && accentButton("Process", ImVec2(120, 0))) {
		if (overwrite)
			openOverwriteConfirm = true;
		else
			startRun();
	}
	if (problem.empty())
		ImGui::SameLine();
	if (ImGui::Button("Close", ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();

	// Confirmation before replacing the originals
	if (openOverwriteConfirm) {
		ImGui::OpenPopup("Overwrite Originals?");
		openOverwriteConfirm = false;
	}
	ui::LimitModalHeight();
	if (ImGui::BeginPopupModal("Overwrite Originals?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		ui::CenterModal();
		ImGui::Text("Replace %d %s in", (int)files.size(), files.size() == 1 ? "file" : "files");
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.accent), "%s", sourceDir);
		ImGui::Text("with the processed versions? This can't be undone.");
		ImGui::Dummy(ImVec2(0, 4));
		if (ImGui::Button("Replace Originals", ImVec2(160, 0))) {
			ImGui::CloseCurrentPopup();
			startRun();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

void runningView() {
	std::string file;
	{
		std::lock_guard<std::mutex> lock(resultsMutex);
		file = currentFile;
	}
	int done = progress;
	ImGui::Text("Processing %d of %d", done < total ? done + 1 : total, total);
	char overlay[64];
	snprintf(overlay, sizeof(overlay), "%d / %d", done, total);
	ImGui::ProgressBar(total ? (float)done / total : 1.f, ImVec2(-1, 0), overlay);
	ImGui::TextDisabled("%s", file.c_str());
	ImGui::Dummy(ImVec2(0, 4));
	if (cancelFlag)
		ImGui::TextDisabled("Stopping after this file...");
	else if (ImGui::Button("Cancel", ImVec2(120, 0)))
		cancelFlag = true;

	if (finished) {
		joinWorker();
		wasCancelled = cancelFlag;
		state = DONE;
	}
}

void doneView() {
	int ok = 0, skipped = 0, filled = 0, found = 0;
	for (const batch::FileResult &r : results) {
		if (r.ok) {
			ok++;
			filled += r.blankFilled;
		}
		else {
			skipped++;
		}
		found += r.blankFound;
	}
	if (wasCancelled)
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(warnColor()), "Cancelled after %d of %d files", (int)results.size(), total);
	ImGui::Text("Processed %d %s into", ok, ok == 1 ? "file" : "files");
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.accent), "%s", runOutput.c_str());
	if (options.fill)
		ImGui::Text("Filled %d blank %s", filled, filled == 1 ? "wave" : "waves");
	else if (found)
		ImGui::TextDisabled("%d blank %s found but left alone (Fill blank frames was off)", found, found == 1 ? "wave" : "waves");
	if (skipped) {
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(warnColor()), "Skipped %d:", skipped);
		ImGui::BeginChild("skipped", ImVec2(0, fminf(160.f, 22.f * skipped + 8.f)), false);
		for (const batch::FileResult &r : results)
			if (!r.ok)
				ImGui::TextWrapped("%s %s", r.name.c_str(), r.reason.c_str());
		ImGui::EndChild();
	}
	ImGui::Dummy(ImVec2(0, 4));
	if (ImGui::Button("Process Another Folder", ImVec2(200, 0)))
		state = SETUP;
	ImGui::SameLine();
	if (accentButton("Done", ImVec2(120, 0))) {
		state = SETUP;
		ImGui::CloseCurrentPopup();
	}
}

} // namespace

void batchOpenDialog() {
	if (state != RUNNING)
		state = SETUP;
	openRequested = true;
}

void batchDemo(const char *dir) {
	if (!dir) {
		closeRequested = true;
		return;
	}
	snprintf(sourceDir, sizeof(sourceDir), "%s", dir);
	listedDir.clear();
	outputEdited = false;
	options.fill = true;
	batchOpenDialog();
}

void batchShutdown() {
	cancelFlag = true;
	joinWorker();
}

void batchRenderDialog() {
	if (openRequested) {
		ImGui::OpenPopup("Batch Process Folder");
		openRequested = false;
	}
	ImGui::SetNextWindowContentWidth(640);
	ui::LimitModalHeight();
	if (!ImGui::BeginPopupModal("Batch Process Folder", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		closeRequested = false;
		return;
	}
	ui::CenterModal();
	if (state == SETUP)
		setupView();
	else if (state == RUNNING)
		runningView();
	else
		doneView();
	if (closeRequested && state != RUNNING) {
		ImGui::CloseCurrentPopup();
		closeRequested = false;
	}
	ImGui::EndPopup();
}
