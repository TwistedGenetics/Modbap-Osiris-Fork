#include "OsirisEdit.hpp"

#ifdef ARCH_LIN
#include <linux/limits.h>
#endif
#ifdef ARCH_MAC
#include <sys/syslimits.h>
#endif
#include <libgen.h>

#include <SDL.h>
#include <SDL_opengl.h>

#include "imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"
#include "theme.hpp"
#include "cardio.hpp"
#include "wtprocess.hpp"
#include "keyframes.hpp"

#include "osdialog/osdialog.h"

#include "lodepng/lodepng.h"

#include "tablabels.hpp"
#include <iostream>
#include <dirent.h>
#include <algorithm>
#include <string>
#include <vector>

int BANK_LEN = 32;
static bool showTestWindow = false;
static bool showConvertPopup = false;
static ImTextureID logoTextureLight;
static ImTextureID logoTextureDark;
static ImTextureID logoTexture;
char lastFilename[1024] = "";
char tableName[64] = "";
static int styleId = 0;
int selectedId = 0;
int lastSelectedId = 0;

static void refreshStyle();

enum Page
{
	EDITOR_PAGE = 0,
	EFFECT_PAGE,
	GRID_PAGE,
	WATERFALL_PAGE,
	IMPORT_PAGE,
	CARD_PAGE,
	//DB_PAGE,
	NUM_PAGES
};

Page currentPage = EDITOR_PAGE;

static ImVec4 lighten(ImVec4 col, float p)
{
	col.x = crossf(col.x, 1.0, p);
	col.y = crossf(col.y, 1.0, p);
	col.z = crossf(col.z, 1.0, p);
	col.w = crossf(col.w, 1.0, p);
	return col;
}

static ImVec4 darken(ImVec4 col, float p)
{
	col.x = crossf(col.x, 0.0, p);
	col.y = crossf(col.y, 0.0, p);
	col.z = crossf(col.z, 0.0, p);
	col.w = crossf(col.w, 0.0, p);
	return col;
}

static ImVec4 alpha(ImVec4 col, float a)
{
	col.w *= a;
	return col;
}

static ImTextureID loadImage(const char *filename)
{
	GLuint textureId;
	glGenTextures(1, &textureId);
	glBindTexture(GL_TEXTURE_2D, textureId);
	unsigned char *pixels;
	unsigned int width, height;
	unsigned err = lodepng_decode_file(&pixels, &width, &height, filename, LCT_RGBA, 8);
	assert(!err);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
	return (void *)(intptr_t)textureId;
}

static void getImageSize(ImTextureID id, int *width, int *height)
{
	GLuint textureId = (GLuint)(intptr_t)id;
	glBindTexture(GL_TEXTURE_2D, textureId);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, height);
	glBindTexture(GL_TEXTURE_2D, 0);
}

static void selectWave(int waveId)
{
	selectedId = waveId;
	lastSelectedId = selectedId;
	morphX = (float)(selectedId % BANK_GRID_WIDTH);
	morphY = (float)(selectedId / BANK_GRID_WIDTH);
	morphZ = (float)selectedId;
}

static void refreshMorphSnap()
{
	if (!morphInterpolate && morphZSpeed <= 0.f)
	{
		morphX = roundf(morphX);
		morphY = roundf(morphY);
		morphZ = roundf(morphZ);
	}
}

/** Focuses to a page which displays the current bank, useful when loading a new bank and showing the user some visual feedback that the bank has changed. */
static void showCurrentBankPage()
{
	switch (currentPage)
	{
	case EFFECT_PAGE:
	case IMPORT_PAGE:
	/*case DB_PAGE:
		currentPage = EDITOR_PAGE;
		break;*/
	default:
		break;
	}
}

static void menuManual()
{
	openBrowser("manual.pdf");
}

static void menuWebsite()
{
	openBrowser("https://modbap.com/osiris-edit/");
}

static void menuGithub()
{
	openBrowser("https://github.com/switchupcb/OsirisEdit");
}

static void menuSynthtech()
{
	openBrowser("https://synthtech.com/waveedit");
}

static void menuNewBank()
{
	showCurrentBankPage();
	currentBank.clear();
	lastFilename[0] = '\0';
	tableName[0] = '\0';
	historyPush();
}

/** Caller must free() return value, guaranteed to not be NULL */
static char *getLastDir()
{
	if (lastFilename[0] == '\0')
	{
		return strdup(".");
	}
	else
	{
		char filename[PATH_MAX];
		strncpy(filename, lastFilename, sizeof(filename));
		char *dir = dirname(filename);
		return strdup(dir);
	}
}

static void menuOpenBank()
{
	char *dir = getLastDir();
	char *path = osdialog_file(OSDIALOG_OPEN, dir, NULL, NULL);
	if (path)
	{
		showCurrentBankPage();
		int fileWaves = currentBank.loadWAV(path);
		snprintf(lastFilename, sizeof(lastFilename), "%s", path);
		tableName[0] = '\0';
		historyPushNew();
		if (fileWaves > 0)
		{
			char name[1024];
			snprintf(name, sizeof(name), "%s", path);
			blankCheckEditor(fileWaves, basename(name));
		}
		free(path);
	}
	free(dir);
}

static void menuSaveBankAs()
{
	char *dir = getLastDir();
	char suggested[80];
	snprintf(suggested, sizeof(suggested), "%s.wav", tableName[0] ? tableName : "Untitled");
	char *path = osdialog_file(OSDIALOG_SAVE, dir, suggested, NULL);
	if (path)
	{
		currentBank.saveWAV(path);
		snprintf(lastFilename, sizeof(lastFilename), "%s", path);
		tableName[0] = '\0';
		free(path);
	}
	free(dir);
}

static void menuSaveBank()
{
	if (lastFilename[0] != '\0')
		currentBank.saveWAV(lastFilename);
	else
		menuSaveBankAs();
}

static void menuSaveWaves()
{
	char *dir = getLastDir();
	char *path = osdialog_file(OSDIALOG_OPEN_DIR, dir, NULL, NULL);
	if (path)
	{
		currentBank.saveWaves(path);
		free(path);
	}
	free(dir);
}

static void menuConvert()
{
	showConvertPopup = true;
}

static void menuQuit()
{
	SDL_Event event;
	event.type = SDL_QUIT;
	SDL_PushEvent(&event);
}

static void menuSelectAll()
{
	selectedId = 0;
	lastSelectedId = BANK_LEN - 1;
}

static void menuCopy()
{
	currentBank.waves[selectedId].clipboardCopy();
}

static void menuCopyAll()
{
	currentBank.waves[0].clipboardSetLength(maxi(selectedId, lastSelectedId) - mini(selectedId, lastSelectedId));
	for (int i = mini(selectedId, lastSelectedId), j = 0; i <= maxi(selectedId, lastSelectedId); i++, j++)
	{
		currentBank.waves[i].clipboardCopyAll(j);
	}
}

static void menuCut()
{
	currentBank.waves[selectedId].clipboardCopy();
	currentBank.waves[selectedId].clear();
	historyPush();
}

static void menuPaste()
{
	currentBank.waves[selectedId].clipboardPaste();
	historyPush();
}

static void menuPasteAll()
{
	for (int i = selectedId, j = 0; i < BANK_LEN; i++, j++)
	{
		currentBank.waves[i].clipboardPasteAll(j);
	}
	historyPush();
}

static void menuClear()
{
	for (int i = mini(selectedId, lastSelectedId); i <= maxi(selectedId, lastSelectedId); i++)
	{
		currentBank.waves[i].clear();
	}
	historyPush();
}

static void menuRandomize()
{
	for (int i = mini(selectedId, lastSelectedId); i <= maxi(selectedId, lastSelectedId); i++)
	{
		currentBank.waves[i].randomizeEffects();
	}
	historyPush();
}

static void incrementSelectedId(int delta)
{
	selectWave(clampi(selectedId + delta, 0, BANK_LEN - 1));
}

static void menuKeyCommands()
{
	ImGuiContext &g = *GImGui;
	ImGuiIO &io = ImGui::GetIO();

	if (io.OSXBehaviors ? io.KeySuper : io.KeyCtrl)
	{
		if (ImGui::IsKeyPressed(SDLK_n) && !io.KeyShift && !io.KeyAlt)
			menuNewBank();
		if (ImGui::IsKeyPressed(SDLK_o) && !io.KeyShift && !io.KeyAlt)
			menuOpenBank();
		if (ImGui::IsKeyPressed(SDLK_s) && !io.KeyShift && !io.KeyAlt)
			menuSaveBank();
		if (ImGui::IsKeyPressed(SDLK_s) && io.KeyShift && !io.KeyAlt)
			menuSaveBankAs();
		if (ImGui::IsKeyPressed(SDLK_q) && !io.KeyShift && !io.KeyAlt)
			menuQuit();
		if (ImGui::IsKeyPressed(SDLK_z) && !io.KeyShift && !io.KeyAlt)
			historyUndo();
		if (ImGui::IsKeyPressed(SDLK_z) && io.KeyShift && !io.KeyAlt)
			historyRedo();
		if (ImGui::IsKeyPressed(SDLK_a) && !io.KeyShift && !io.KeyAlt)
			menuSelectAll();
		if (ImGui::IsKeyPressed(SDLK_x) && !io.KeyShift && !io.KeyAlt)
			menuCut();
		if (ImGui::IsKeyPressed(SDLK_v) && !io.KeyShift && !io.KeyAlt)
		{
			menuPaste();
			menuPasteAll();
		}
		if (ImGui::IsKeyPressed(SDLK_c) && !io.KeyShift && !io.KeyAlt)
		{
			if (lastSelectedId - selectedId != 0)
			{
				menuCopyAll();
			}
			else
			{
				menuCopy();
			}
		}
	}
	// I have NO idea why the scancode is needed here but the keycodes are needed for the letters.
	// It looks like SDLZ_F1 is not defined correctly or something.
	if (ImGui::IsKeyPressed(SDL_SCANCODE_F1))
		menuManual();

	if (!io.KeySuper && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt)
	{
		// Only trigger these key commands if no text box is focused
		if (!g.ActiveId || g.ActiveId != GImGui->InputTextState.Id)
		{
			if (ImGui::IsKeyPressed(SDLK_r))
				menuRandomize();
			if (ImGui::IsKeyPressed(SDLK_k))
				editorToggleKeyframes();
			if (ImGui::IsKeyPressed(io.OSXBehaviors ? SDLK_BACKSPACE : SDLK_DELETE))
				menuClear();
			// Pages
			if (ImGui::IsKeyPressed(SDLK_SPACE))
				playEnabled = !playEnabled;
			if (ImGui::IsKeyPressed(SDLK_1))
				currentPage = EDITOR_PAGE;
			if (ImGui::IsKeyPressed(SDLK_2))
				currentPage = EFFECT_PAGE;
			if (ImGui::IsKeyPressed(SDLK_3))
				currentPage = GRID_PAGE;
			if (ImGui::IsKeyPressed(SDLK_4))
				currentPage = WATERFALL_PAGE;
			if (ImGui::IsKeyPressed(SDLK_5))
				currentPage = IMPORT_PAGE;
			/*if (ImGui::IsKeyPressed(SDLK_6))
				currentPage = DB_PAGE;*/
			if (ImGui::IsKeyPressed(SDL_SCANCODE_UP))
				incrementSelectedId(currentPage == GRID_PAGE ? -BANK_GRID_WIDTH : -1);
			if (ImGui::IsKeyPressed(SDL_SCANCODE_DOWN))
				incrementSelectedId(currentPage == GRID_PAGE ? BANK_GRID_WIDTH : 1);
			if (ImGui::IsKeyPressed(SDL_SCANCODE_LEFT))
				incrementSelectedId(-1);
			if (ImGui::IsKeyPressed(SDL_SCANCODE_RIGHT))
				incrementSelectedId(1);
		}
	}
}

void renderWaveMenu()
{
	char menuName[128];
	int selectedStart = mini(selectedId, lastSelectedId);
	int selectedEnd = maxi(selectedId, lastSelectedId);
	if (selectedStart != selectedEnd)
		snprintf(menuName, sizeof(menuName), "(Wave %d to %d)", selectedStart + 1, selectedEnd + 1);
	else
		snprintf(menuName, sizeof(menuName), "(Wave %d)", selectedId + 1);
	ImGui::MenuItem(menuName, NULL, false, false);
	if (selectedId - lastSelectedId != 0)
	{
		if (ImGui::MenuItem("Copy All", ImGui::GetIO().OSXBehaviors ? "Cmd+C" : "Ctrl+C", false, selectedId - lastSelectedId != 0))
		{
			menuCopyAll();
		}
	}
	if (ImGui::MenuItem("Paste All", ImGui::GetIO().OSXBehaviors ? "Cmd+V" : "Ctrl+V", false, clipboardArrayActive))
	{
		menuPasteAll();
	}
	if (ImGui::MenuItem("Clear", "Delete"))
	{
		menuClear();
	}
	if (ImGui::MenuItem("Randomize Effects", "R"))
	{
		menuRandomize();
	}
	{
		bool allKeyframes = true;
		for (int w = selectedStart; w <= selectedEnd; w++)
			allKeyframes = allKeyframes && currentBank.keyframes[w];
		if (ImGui::MenuItem("Keyframe", "K", allKeyframes))
			editorToggleKeyframes();
		if (ImGui::MenuItem("Morph Between Keyframes..."))
			morphOpenDialog();
		if (ImGui::MenuItem("Generate Wavetable..."))
			genOpenDialog();
	}

	if (selectedStart != selectedEnd)
	{
		ImGui::MenuItem("##spacer3", NULL, false, false);
		snprintf(menuName, sizeof(menuName), "(Wave %d)", selectedId + 1);
		ImGui::MenuItem(menuName, NULL, false, false);
	}

	if (ImGui::MenuItem("Copy", ImGui::GetIO().OSXBehaviors ? "Cmd+C" : "Ctrl+C"))
	{
		menuCopy();
	}
	if (ImGui::MenuItem("Cut", ImGui::GetIO().OSXBehaviors ? "Cmd+X" : "Ctrl+X"))
	{
		menuCut();
	}
	if (ImGui::MenuItem("Paste", ImGui::GetIO().OSXBehaviors ? "Cmd+V" : "Ctrl+V", false, clipboardActive))
	{
		menuPaste();
	}

	if (ImGui::MenuItem("Open Wave..."))
	{
		char *dir = getLastDir();
		char *path = osdialog_file(OSDIALOG_OPEN, dir, NULL, NULL);
		if (path)
		{
			currentBank.waves[selectedId].loadWAV(path);
			historyPush();
			snprintf(lastFilename, sizeof(lastFilename), "%s", path);
			free(path);
		}
		free(dir);
	}
	if (ImGui::MenuItem("Save Wave As..."))
	{
		char *dir = getLastDir();
		char *path = osdialog_file(OSDIALOG_SAVE, dir, "Untitled.wav", NULL);
		if (path)
		{
			currentBank.waves[selectedId].saveWAV(path);
			snprintf(lastFilename, sizeof(lastFilename), "%s", path);
			free(path);
		}
		free(dir);
	}
}

void renderMenu()
{
	menuKeyCommands();

	// HACK
	// Display a window on top of the menu with the logo, since I'm too lazy to make my own custom MenuImageItem widget
	{
		int width, height;
		getImageSize(logoTexture, &width, &height);
		ImVec2 padding = ImVec2(7, 4);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(width + 2 * padding.x, height + 2 * padding.y));
		if (ImGui::Begin("Logo", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoInputs))
		{
			ImGui::Image(logoTexture, ImVec2(width, height));
			ImGui::End();
		}
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		ImGui::PopStyleVar();
	}

	// Draw main menu
	if (ImGui::BeginMenuBar())
	{
		// This will be hidden by the window with the logo
		if (ImGui::BeginMenu("                        v" TOSTRING(VERSION), false))
		{
			ImGui::EndMenu();
		}
		// File
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New Wavetable", ImGui::GetIO().OSXBehaviors ? "Cmd+N" : "Ctrl+N"))
				menuNewBank();
			if (ImGui::MenuItem("Open Wavetable...", ImGui::GetIO().OSXBehaviors ? "Cmd+O" : "Ctrl+O"))
				menuOpenBank();
			if (ImGui::MenuItem("Save Wavetable", ImGui::GetIO().OSXBehaviors ? "Cmd+S" : "Ctrl+S"))
				menuSaveBank();
			if (ImGui::MenuItem("Save Wavetable As...", ImGui::GetIO().OSXBehaviors ? "Cmd+Shift+S" : "Ctrl+Shift+S"))
				menuSaveBankAs();
			if (ImGui::MenuItem("Save Waves to Folder...", NULL))
				menuSaveWaves();
			if (ImGui::MenuItem("Batch Process Folder...", NULL))
				batchOpenDialog();
			if (ImGui::MenuItem("Convert Folder for Osiris...", NULL))
				menuConvert();
			if (ImGui::MenuItem("Quit", ImGui::GetIO().OSXBehaviors ? "Cmd+Q" : "Ctrl+Q"))
				menuQuit();

			ImGui::EndMenu();
		}
		// Edit
		if (ImGui::BeginMenu("Edit"))
		{
			if (ImGui::MenuItem("Undo", ImGui::GetIO().OSXBehaviors ? "Cmd+Z" : "Ctrl+Z"))
				historyUndo();
			if (ImGui::MenuItem("Redo", ImGui::GetIO().OSXBehaviors ? "Cmd+Shift+Z" : "Ctrl+Shift+Z"))
				historyRedo();
			if (ImGui::MenuItem("Select All", ImGui::GetIO().OSXBehaviors ? "Cmd+A" : "Ctrl+A"))
				menuSelectAll();
			ImGui::MenuItem("##spacer", NULL, false, false);
			renderWaveMenu();
			ImGui::EndMenu();
		}
		// Audio Output
		if (ImGui::BeginMenu("Audio Output"))
		{
			int deviceCount = audioGetDeviceCount();
			for (int deviceId = 0; deviceId < deviceCount; deviceId++)
			{
				const char *deviceName = audioGetDeviceName(deviceId);
				if (ImGui::MenuItem(deviceName, NULL, false))
					audioOpen(deviceId);
			}
			ImGui::EndMenu();
		}
		// Colors
		if (ImGui::BeginMenu("Colors"))
		{
			if (ImGui::MenuItem("Modern", NULL, styleId == 4))
			{
				styleId = 4;
				refreshStyle();
			}
			if (ImGui::MenuItem("Dark", NULL, styleId == 0))
			{
				styleId = 0;
				refreshStyle();
			}
			/*if (ImGui::MenuItem("Mars", NULL, styleId == 1))
			{
				styleId = 1;
				refreshStyle();
			}
			if (ImGui::MenuItem("Mercury", NULL, styleId == 2))
			{
				styleId = 2;
				refreshStyle();
			}*/
			if (ImGui::MenuItem("Light", NULL, styleId == 3))
			{
				styleId = 3;
				refreshStyle();
			}
			ImGui::EndMenu();
		}
		// Preferences
		if (ImGui::BeginMenu("Preferences"))
		{
			if (ImGui::BeginMenu("Blank Waves"))
			{
				blankPreferencesMenu();
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}
		// Help
		if (ImGui::BeginMenu("Help"))
		{
			if (ImGui::MenuItem("Webpage", "", false))
				menuWebsite();
			if (ImGui::MenuItem("Github", "", false))
				menuGithub();
			// if (ImGui::MenuItem("imgui Demo", NULL, showTestWindow)) showTestWindow = !showTestWindow;
			ImGui::EndMenu();
		}
		ImGui::EndMenuBar();
	}
}

/** Convert: turns a folder of WAV files into an Osiris card layout (Osiris/A..D, 32 files per bank).
Uses the same naming, sorting and export code as the SD Card page. */
void renderPopup()
{
	static char sourceDir[1024] = "";
	static char destDir[1024] = "";
	static char packName[48] = "Pack";
	static int startBank = 0;
	static int fixChoice = 0;
	static bool cycleAll = false;
	static int existingChoice = 0;
	static std::vector<std::string> resultLines;
	static bool resultOk = true;

	if (showConvertPopup)
	{
		ImGui::OpenPopup("Convert Folder for Osiris");
		showConvertPopup = false;
		resultLines.clear();
	}

	ImGui::SetNextWindowContentWidth(640.0);
	ui::LimitModalHeight();
	if (!ImGui::BeginPopupModal("Convert Folder for Osiris", NULL, ImGuiWindowFlags_AlwaysAutoResize))
		return;
	ui::CenterModal();

	ImGui::TextWrapped("Converts every WAV file in a folder into Osiris wavetables: 16-bit, 44.1 kHz mono, 32 waves each, "
		"32 files per bank, named Osiris_<Bank>_<Pack>_<N>.wav. Files are taken in number order (2 before 10).");
	ImGui::Dummy(ImVec2(0, 4));

	// Source
	ui::SectionHeading("Source folder");
	ImGui::PushItemWidth(-110);
	ImGui::InputText("##convertSource", sourceDir, sizeof(sourceDir));
	ImGui::PopItemWidth();
	ImGui::SameLine();
	if (ImGui::Button("Browse...##source", ImVec2(100, 0)))
	{
		char *dir = getLastDir();
		char *path = osdialog_file(OSDIALOG_OPEN_DIR, dir, NULL, NULL);
		if (path)
		{
			snprintf(sourceDir, sizeof(sourceDir), "%s", path);
			free(path);
		}
		free(dir);
	}
	std::vector<std::string> files;
	if (sourceDir[0])
		files = osiris::listWavFiles(sourceDir);
	int fit = (int)osiris::planBanks((int)files.size(), startBank).size();
	if (sourceDir[0])
	{
		if (files.empty())
			ImGui::TextDisabled("No WAV files in that folder");
		else
		{
			std::string from = std::string(1, osiris::bankLetter(startBank)) + "1";
			std::vector<osiris::Position> plan = osiris::planBanks(fit, startBank);
			std::string to = std::string(1, osiris::bankLetter(plan.back().bank)) + std::to_string(plan.back().slot + 1);
			ImGui::TextDisabled("%d WAV files, going into %s to %s", (int)files.size(), from.c_str(), to.c_str());
			if (fit < (int)files.size())
				ImGui::TextColored(ImVec4(0.96f, 0.71f, 0.29f, 1.f), "%d files won't fit (a card holds 32 per bank)", (int)files.size() - fit);
		}
	}

	// Naming
	ui::SectionHeading("Naming");
	ImGui::AlignFirstTextHeightToWidgets();
	ImGui::Text("Pack name");
	ImGui::SameLine(110);
	ImGui::PushItemWidth(220);
	ImGui::InputText("##convertPack", packName, sizeof(packName));
	ImGui::PopItemWidth();
	ImGui::SameLine(0, 24);
	ImGui::AlignFirstTextHeightToWidgets();
	ImGui::Text("Start at bank");
	ImGui::SameLine();
	ImGui::PushItemWidth(70);
	ImGui::Combo("##convertBank", &startBank, "A\0B\0C\0D\0\0");
	ImGui::PopItemWidth();
	ImGui::TextDisabled("First file: %s", osiris::fileName(startBank, packName, "", 1, false).c_str());

	ImGui::AlignFirstTextHeightToWidgets();
	ImGui::Text("Files that aren't 32 waves:");
	ImGui::SameLine();
	ImGui::RadioButton("Resample##convert", &fixChoice, 0);
	ImGui::SameLine();
	ImGui::RadioButton("Pad / trim##convert", &fixChoice, 1);
	ImGui::Checkbox("Apply Cycle to every wavetable##convert", &cycleAll);

	// Destination
	ui::SectionHeading("Destination (SD card or folder)");
	std::vector<std::string> volumes = osiris::listVolumes();
	for (const std::string &v : volumes)
	{
		if (ImGui::RadioButton(v.c_str(), std::string(destDir) == v))
			snprintf(destDir, sizeof(destDir), "%s", v.c_str());
	}
	ImGui::PushItemWidth(-110);
	ImGui::InputText("##convertDest", destDir, sizeof(destDir));
	ImGui::PopItemWidth();
	ImGui::SameLine();
	if (ImGui::Button("Browse...##dest", ImVec2(100, 0)))
	{
		char *dir = getLastDir();
		char *path = osdialog_file(OSDIALOG_OPEN_DIR, dir, NULL, NULL);
		if (path)
		{
			snprintf(destDir, sizeof(destDir), "%s", path);
			free(path);
		}
		free(dir);
	}
	if (destDir[0])
	{
		std::string dest = destDir;
		std::string base = dest.substr(dest.find_last_of('/') + 1);
		bool isOsiris = (base == "Osiris" || base == "osiris" || base == "OSIRIS");
		std::string osirisPath = isOsiris ? dest : dest + "/Osiris";
		ImGui::TextDisabled("Files go to %s/A, B, C, D", osirisPath.c_str());
		if (osiris::directoryExists(osirisPath))
		{
			ImGui::Text("There's already an Osiris folder here:");
			ImGui::RadioButton("Back it up first, then write a fresh one (recommended)##convert", &existingChoice, 0);
			ImGui::RadioButton("Merge, moving only the files being replaced into a backup folder##convert", &existingChoice, 1);
		}
	}

	// Result of the last conversion
	for (const std::string &line : resultLines)
	{
		if (resultOk)
			ImGui::TextWrapped("%s", line.c_str());
		else
			ImGui::TextColored(ImVec4(0.96f, 0.71f, 0.29f, 1.f), "%s", line.c_str());
	}

	ImGui::Dummy(ImVec2(0, 4));
	bool ready = !files.empty() && destDir[0];
	if (ready && ImGui::Button("Convert", ImVec2(120, 0)))
	{
		osiris::Card card;
		for (int b = 0; b < osiris::BANKS; b++)
			card.pack[b] = packName;
		std::vector<std::string> messages;
		osiris::fillFromFiles(card, files, startBank, 0, messages);
		osiris::ExportOptions options;
		options.existing = existingChoice == 1 ? osiris::ExportOptions::MERGE : osiris::ExportOptions::BACKUP;
		options.fix = fixChoice == 1 ? osiris::FIX_PAD : osiris::FIX_RESAMPLE;
		options.cycle = cycleAll;
		osiris::ExportResult r = osiris::exportCard(card, destDir, options);
		resultOk = r.ok;
		resultLines.clear();
		resultLines.push_back("Wrote " + std::to_string(r.written) + " files to " + r.osirisPath +
			" (A " + std::to_string(r.perBank[0]) + ", B " + std::to_string(r.perBank[1]) + ", C " + std::to_string(r.perBank[2]) + ", D " + std::to_string(r.perBank[3]) + ")");
		if (!r.backupPath.empty())
			resultLines.push_back("Previous files backed up to " + r.backupPath);
		for (const std::string &m : messages)
			resultLines.push_back(m);
		for (const std::string &e : r.errors)
			resultLines.push_back(e);
	}
	if (ready)
		ImGui::SameLine();
	if (ImGui::Button(resultLines.empty() ? "Cancel" : "Close", ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void renderPreview()
{
	// Play toggle
	bool playing = playEnabled;
	ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(playing ? colAlpha(theme.accent, 0.22f) : theme.control));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(playing ? colAlpha(theme.accent, 0.30f) : theme.controlHover));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(playing ? theme.accent : theme.text));
	if (ImGui::Button(playing ? "Playing" : "Play", ImVec2(96, 0)))
		playEnabled = !playEnabled;
	ImGui::PopStyleColor(3);

	ImGui::SameLine();
	ImGui::PushItemWidth(260.0);
	ui::Slider("##playVolume", "Volume", &playVolume, -60.0f, 0.0f, "%.1f dB");
	ImGui::PopItemWidth();
	ImGui::SameLine();
	ImGui::PushItemWidth(-1.0);
	ui::Slider("##playFrequency", "Frequency", &playFrequency, 1.0f, 10000.0f, "%.1f Hz", 0.0f);
	ImGui::PopItemWidth();

	ImGui::Checkbox("Morph Interpolate", &morphInterpolate);
	ImGui::SameLine(0, 12);
	float width = (ImGui::GetContentRegionAvailWidth() - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
	ImGui::PushItemWidth(width);
	if (playModeXY)
	{
		ui::Slider("##Morph X", "Morph X", &morphX, 0.0, BANK_GRID_WIDTH - 1, "%.3f");
		ImGui::SameLine();
		ui::Slider("##Morph Y", "Morph Y", &morphY, 0.0, BANK_GRID_HEIGHT - 1, "%.3f");
	}
	else
	{
		ui::Slider("##Morph Z", "Morph Z", &morphZ, 0.0, BANK_LEN - 1, "%.3f");
		ImGui::SameLine();
		ui::Slider("##Morph Z Speed", "Morph Z Speed", &morphZSpeed, 0.f, 10.f, "%.3f Hz", 3.f);
	}
	ImGui::PopItemWidth();

	refreshMorphSnap();
}

void renderToolSelector(Tool *tool)
{
	static const char *labels[] = {"Pencil", "Brush", "Grab", "Line", "Eraser"};
	static const Tool tools[] = {PENCIL_TOOL, BRUSH_TOOL, GRAB_TOOL, LINE_TOOL, ERASER_TOOL};
	int selected = 0;
	for (int i = 0; i < 5; i++)
		if (tools[i] == *tool)
			selected = i;
	ui::Segmented("tools", labels, 5, &selected);
	*tool = tools[selected];
}

void effectSlider(EffectID effect)
{
	char id[64];
	snprintf(id, sizeof(id), "##%s", effectNames[effect]);
	char text[64];
	if (ui::Slider(id, effectNames[effect], &currentBank.waves[selectedId].effects[effect], 0.0f, 1.0f, "%.3f"))
	{
		currentBank.waves[selectedId].updatePost();
		historyPush();
	}
}

void editorPage()
{
	ImGui::BeginChild("Sidebar", ImVec2(200, 0), true);
	{
		float dummyZ = 0.0;
		ImGui::PushItemWidth(-1);
		renderBankGrid("SidebarGrid", BANK_LEN * 35.0, 1, &dummyZ, &morphZ);
		refreshMorphSnap();
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("Editor", ImVec2(0, 0), true);
	{
		Wave *wave = &currentBank.waves[selectedId];
		float *effects = wave->effects;

		// Share the leftover height between the waveform and harmonics graphs
		float lineHeight = ImGui::GetItemsLineHeightWithSpacing();
		float headingHeight = (theme.fontHeading ? theme.fontHeading->FontSize * ImGui::GetIO().FontGlobalScale : 12.f) + ImGui::GetStyle().ItemSpacing.y * 2.f + 2.f;
		float fixedHeight = lineHeight * (2 + (EFFECTS_LEN + 1) / 2) + headingHeight * 3.f + ImGui::GetStyle().ItemSpacing.y * 3.f + 6.f;
		float graphHeight = ImGui::GetContentRegionAvail().y - fixedHeight;
		float waveHeight = fmaxf(110.f, graphHeight * 0.6f);
		float harmonicsHeight = fmaxf(70.f, graphHeight * 0.4f);

		ImGui::PushItemWidth(-1);

		static enum Tool tool = PENCIL_TOOL;
		renderToolSelector(&tool);

		ImGui::SameLine(0, 16);
		if (ImGui::Button("Clear"))
		{
			currentBank.waves[selectedId].clear();
			historyPush();
		}

		for (const CatalogCategory &catalogCategory : catalogCategories)
		{
			ImGui::SameLine();
			if (ImGui::Button(catalogCategory.name.c_str()))
				ImGui::OpenPopup(catalogCategory.name.c_str());
			if (ImGui::BeginPopup(catalogCategory.name.c_str()))
			{
				for (const CatalogFile &catalogFile : catalogCategory.files)
				{
					if (ImGui::Selectable(catalogFile.name.c_str()))
					{
						memcpy(currentBank.waves[selectedId].samples, catalogFile.samples, sizeof(float) * WAVE_LEN);
						currentBank.waves[selectedId].commitSamples();
						historyPush();
					}
				}
				ImGui::EndPopup();
			}
		}

		ImGui::SameLine(0, 16);
		if (ImGui::Button("Morph..."))
			morphOpenDialog();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Morph between keyframes (mark them with K or right-click > Keyframe)");
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme.modern ? theme.accent : theme.text));
		if (ImGui::Button("Generate..."))
			genOpenDialog();
		ImGui::PopStyleColor();

		// ImGui::SameLine();
		// if (ImGui::RadioButton("Smooth", tool == SMOOTH_TOOL)) tool = SMOOTH_TOOL;

		ui::SectionHeading("Waveform");
		const int oversample = 4;
		float waveOversample[WAVE_LEN * oversample];
		cyclicOversample(wave->postSamples, waveOversample, WAVE_LEN, oversample);
		if (renderWave("WaveEditor", waveHeight, wave->samples, WAVE_LEN, waveOversample, WAVE_LEN * oversample, tool))
		{
			currentBank.waves[selectedId].commitSamples();
			historyPush();
		}

		ui::SectionHeading("Harmonics");
		if (renderHistogram("HarmonicEditor", harmonicsHeight, wave->harmonics, WAVE_LEN / 2, wave->postHarmonics, WAVE_LEN / 2, tool))
		{
			currentBank.waves[selectedId].commitHarmonics();
			historyPush();
		}

		ui::SectionHeading("Effects");
		{
			// Two columns of effect sliders
			float spacing = ImGui::GetStyle().ItemSpacing.x;
			float columnWidth = (ImGui::GetContentRegionAvailWidth() - spacing) / 2.0f;
			ImGui::PushItemWidth(columnWidth);
			for (int i = 0; i < EFFECTS_LEN; i++)
			{
				if (i % 2 == 1)
					ImGui::SameLine();
				effectSlider((EffectID)i);
			}
			ImGui::PopItemWidth();
		}
		ImGui::Dummy(ImVec2(0, 2));

		if (ImGui::Checkbox("Cycle", &currentBank.waves[selectedId].cycle))
		{
			currentBank.waves[selectedId].updatePost();
			historyPush();
		}
		ImGui::SameLine();
		if (ImGui::Checkbox("Normalize", &currentBank.waves[selectedId].normalize))
		{
			currentBank.waves[selectedId].updatePost();
			historyPush();
		}
		ImGui::SameLine();
		if (ImGui::Button("Randomize"))
		{
			currentBank.waves[selectedId].randomizeEffects();
			historyPush();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset"))
		{
			currentBank.waves[selectedId].clearEffects();
			historyPush();
		}
		ImGui::SameLine();
		if (ImGui::Button("Bake"))
		{
			currentBank.waves[selectedId].bakeEffects();
			historyPush();
		}

		ImGui::PopItemWidth();
	}
	ImGui::EndChild();
}

void effectHistogram(EffectID effect, Tool tool)
{
	float value[BANK_LEN];
	float average = 0.0;
	for (int i = 0; i < BANK_LEN; i++)
	{
		value[i] = currentBank.waves[i].effects[effect];
		average += value[i];
	}
	average /= BANK_LEN;
	float oldAverage = average;

	ui::SectionHeading(effectNames[effect]);

	char id[64];
	snprintf(id, sizeof(id), "##%sAverage", effectNames[effect]);
	if (ui::Slider(id, "Average", &average, 0.0f, 1.0f, "%.3f"))
	{
		// Change the average effect level to the new average
		float deltaAverage = average - oldAverage;
		for (int i = 0; i < BANK_LEN; i++)
		{
			if (0.0 < average && average < 1.0)
			{
				currentBank.waves[i].effects[effect] = clampf(currentBank.waves[i].effects[effect] + deltaAverage, 0.0, 1.0);
			}
			else
			{
				currentBank.waves[i].effects[effect] = average;
			}
			currentBank.waves[i].updatePost();
			historyPush();
		}
	}

	if (renderHistogram(effectNames[effect], 110, value, BANK_LEN, NULL, 0, tool))
	{
		for (int i = 0; i < BANK_LEN; i++)
		{
			if (currentBank.waves[i].effects[effect] != value[i])
			{
				// TODO This always selects the highest index. Select the index the mouse is hovering (requires renderHistogram() to return an int)
				selectWave(i);
				currentBank.waves[i].effects[effect] = value[i];
				currentBank.waves[i].updatePost();
				historyPush();
			}
		}
	}
}

void effectPage()
{
	ImGui::BeginChild("Effect Editor", ImVec2(0, 0), true);
	{
		static Tool tool = PENCIL_TOOL;
		renderToolSelector(&tool);

		ImGui::PushItemWidth(-1);
		{
			// Two columns of effect graphs
			float spacing = ImGui::GetStyle().ItemSpacing.x * 2.0f;
			float columnWidth = (ImGui::GetContentRegionAvailWidth() - spacing) / 2.0f;
			for (int i = 0; i < EFFECTS_LEN; i++)
			{
				if (i % 2 == 1)
					ImGui::SameLine(0, spacing);
				ImGui::BeginGroup();
				ImGui::PushItemWidth(columnWidth);
				effectHistogram((EffectID)i, tool);
				ImGui::PopItemWidth();
				ImGui::EndGroup();
			}
		}
		ImGui::PopItemWidth();
		ImGui::Dummy(ImVec2(0, 4));

		if (ImGui::Button("Cycle All"))
		{
			for (int i = 0; i < BANK_LEN; i++)
			{
				currentBank.waves[i].cycle = true;
				currentBank.waves[i].updatePost();
				historyPush();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cycle None"))
		{
			for (int i = 0; i < BANK_LEN; i++)
			{
				currentBank.waves[i].cycle = false;
				currentBank.waves[i].updatePost();
				historyPush();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Normalize All"))
		{
			for (int i = 0; i < BANK_LEN; i++)
			{
				currentBank.waves[i].normalize = true;
				currentBank.waves[i].updatePost();
				historyPush();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Normalize None"))
		{
			for (int i = 0; i < BANK_LEN; i++)
			{
				currentBank.waves[i].normalize = false;
				currentBank.waves[i].updatePost();
				historyPush();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Randomize"))
		{
			for (int i = 0; i < BANK_LEN; i++)
			{
				currentBank.waves[i].randomizeEffects();
				historyPush();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset"))
		{
			for (int i = 0; i < BANK_LEN; i++)
			{
				currentBank.waves[i].clearEffects();
				historyPush();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Bake"))
		{
			for (int i = 0; i < BANK_LEN; i++)
			{
				currentBank.waves[i].bakeEffects();
				historyPush();
			}
		}
	}
	ImGui::EndChild();
}

void gridPage()
{
	playModeXY = true;
	ImGui::BeginChild("Grid Page", ImVec2(0, 0), true);
	{
		ImGui::PushItemWidth(-1.0);
		renderBankGrid("WaveGrid", -1.f, BANK_GRID_WIDTH, &morphX, &morphY);
		refreshMorphSnap();
	}
	ImGui::EndChild();
}

void waterfallPage()
{
	ImGui::BeginChild("3D View", ImVec2(0, 0), true);
	{
		ImGui::PushItemWidth(-1.0);
		static float amplitude = 0.25;
		static float angle = 1.0;
		float width = (ImGui::GetContentRegionAvailWidth() - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
		ImGui::PushItemWidth(width);
		ui::Slider("##amplitude", "Scale", &amplitude, 0.01, 1.0, "%.3f", 0.0);
		ImGui::SameLine();
		ui::Slider("##angle", "Angle", &angle, 0.0, 1.0, "%.3f");
		ImGui::PopItemWidth();

		renderWaterfall("##waterfall", -1.0, amplitude, angle, &morphZ);
	}
	ImGui::EndChild();
}

void renderMain()
{
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2((int)ImGui::GetIO().DisplaySize.x, (int)ImGui::GetIO().DisplaySize.y));

	ImGui::Begin("", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_MenuBar);
	{
		// Menu bar
		renderMenu();
		renderPopup();
		batchRenderDialog();
		blankRenderPopup();
		renderPreview();
		// Tab bar

		ImGui::Dummy(ImVec2(0, 2));

		// Selectable Bank Length
		const char *bankLens[] = {"16", "32", "64"};
		static int len = 1;
		ImGui::AlignFirstTextHeightToWidgets();
		ImGui::TextDisabled("Waves");
		ImGui::SameLine();
		ImGui::PushItemWidth(70.0);
		ImGui::Combo("##width", &len, bankLens, IM_ARRAYSIZE(bankLens));
		ImGui::PopItemWidth();
		ImGui::SameLine(0, 20);
		BANK_LEN = atoi(bankLens[len]);

		{
			static const char *tabLabels[NUM_PAGES] = {
				"Waveform Editor",
				"Effect Editor",
				"Grid XY View",
				"Waterfall View",
				"Import",
				"SD Card"};
			ui::Tabs(tabLabels, NUM_PAGES, (int *)&currentPage);
		}

		// Page
		// Reset some audio variables. These might be changed within the pages.
		playModeXY = false;
		playingBank = &currentBank;
		switch (currentPage)
		{
		case EDITOR_PAGE:
			editorPage();
			break;
		case EFFECT_PAGE:
			effectPage();
			break;
		case GRID_PAGE:
			gridPage();
			break;
		case WATERFALL_PAGE:
			waterfallPage();
			break;
		case IMPORT_PAGE:
			importPage();
			break;
		case CARD_PAGE:
			cardPage();
			break;
		/*case DB_PAGE:
			dbPage();
			break;*/
		default:
			break;
		}
		morphRenderDialog();
		genRenderDialog();
	}
	ImGui::End();

	if (showTestWindow)
	{
		ImGui::ShowTestWindow(&showTestWindow);
	}
}

static void refreshStyle()
{
	const ImVec4 transparent = ImVec4(0.0, 0.0, 0.0, 0.0);

	ImGuiStyle &style = ImGui::GetStyle();

	style.Alpha = 1.f;
	// Classic layout (the Modern scheme overrides these below)
	style.WindowRounding = 2.f;
	style.GrabRounding = 2.f;
	style.ChildWindowRounding = 2.f;
	style.ScrollbarRounding = 2.f;
	style.FrameRounding = 2.f;
	style.WindowPadding = ImVec2(8, 8);
	style.FramePadding = ImVec2(4, 3);
	style.ItemSpacing = ImVec2(8, 4);
	style.ItemInnerSpacing = ImVec2(4, 4);
	style.ScrollbarSize = 16.f;
	style.GrabMinSize = 10.f;
	style.FramePadding = ImVec2(6.0f, 4.0f);

	if (styleId == 0)
	{
		style.Colors[ImGuiCol_Text] = ImVec4(0.73f, 0.73f, 0.73f, 1.00f);
		style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
		style.Colors[ImGuiCol_WindowBg] = ImVec4(0.26f, 0.26f, 0.26f, 0.95f);
		style.Colors[ImGuiCol_ChildWindowBg] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
		style.Colors[ImGuiCol_PopupBg] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
		style.Colors[ImGuiCol_Border] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
		style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		style.Colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
		style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
		style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
		style.Colors[ImGuiCol_TitleBg] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
		style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
		style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_ComboBg] = ImVec4(0.32f, 0.32f, 0.32f, 1.00f);
		style.Colors[ImGuiCol_CheckMark] = ImVec4(0.78f, 0.78f, 0.78f, 1.00f);
		style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.74f, 0.74f, 0.74f, 1.00f);
		style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.74f, 0.74f, 0.74f, 1.00f);
		style.Colors[ImGuiCol_Button] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.43f, 0.43f, 0.43f, 1.00f);
		style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
		style.Colors[ImGuiCol_Header] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		// style.Colors[ImGuiCol_Column]                = ImVec4(0.39f, 0.39f, 0.39f, 1.00f);
		// style.Colors[ImGuiCol_ColumnHovered]         = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		// style.Colors[ImGuiCol_ColumnActive]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		style.Colors[ImGuiCol_CloseButton] = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
		style.Colors[ImGuiCol_CloseButtonHovered] = ImVec4(0.98f, 0.39f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_CloseButtonActive] = ImVec4(0.98f, 0.39f, 0.36f, 1.00f);
		style.Colors[ImGuiCol_PlotLines] = ImVec4(0.39f, 0.39f, 0.39f, 1.00f);
		style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
		style.Colors[ImGuiCol_PlotHistogram] = ImVec4(1.0, 0.8, 0.2, 1.0);
		style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.7, 0.5, 0.1, 0.5);
		style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.32f, 0.52f, 0.65f, 1.00f);
		style.Colors[ImGuiCol_ModalWindowDarkening] = ImVec4(0.20f, 0.20f, 0.20f, 0.50f);
		logoTexture = logoTextureLight;
	}
	else if (styleId == 1)
	{
		// base16-atelier-dune
		ImVec4 base[16] = {
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x20, 0x20, 0x1d, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x29, 0x28, 0x24, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x6e, 0x6b, 0x5e, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x7d, 0x7a, 0x68, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x99, 0x95, 0x80, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xa6, 0xa2, 0x8c, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xe8, 0xe4, 0xcf, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xfe, 0xfb, 0xec, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xd7, 0x37, 0x37, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xb6, 0x56, 0x11, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xae, 0x95, 0x13, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x60, 0xac, 0x39, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x1f, 0xad, 0x83, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x66, 0x84, 0xe1, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xb8, 0x54, 0xd4, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xd4, 0x35, 0x52, 0xff)),
		};

		style.Colors[ImGuiCol_Text] = base[0x6];
		style.Colors[ImGuiCol_TextDisabled] = base[0x4];
		style.Colors[ImGuiCol_WindowBg] = base[0x2];
		style.Colors[ImGuiCol_ChildWindowBg] = base[0x2];
		style.Colors[ImGuiCol_PopupBg] = base[0x2];
		style.Colors[ImGuiCol_Border] = transparent;
		style.Colors[ImGuiCol_BorderShadow] = transparent;
		style.Colors[ImGuiCol_FrameBg] = base[0x1];
		style.Colors[ImGuiCol_FrameBgHovered] = base[0x1];
		style.Colors[ImGuiCol_FrameBgActive] = base[0x1];
		style.Colors[ImGuiCol_TitleBg] = base[0x3];
		style.Colors[ImGuiCol_TitleBgCollapsed] = base[0x3];
		style.Colors[ImGuiCol_TitleBgActive] = base[0x3];
		style.Colors[ImGuiCol_MenuBarBg] = base[0x2];
		style.Colors[ImGuiCol_ScrollbarBg] = base[0x3];
		style.Colors[ImGuiCol_ScrollbarGrab] = base[0x4];
		style.Colors[ImGuiCol_ScrollbarGrabHovered] = base[0x4];
		style.Colors[ImGuiCol_ScrollbarGrabActive] = base[0x4];
		style.Colors[ImGuiCol_ComboBg] = base[0x4];
		style.Colors[ImGuiCol_CheckMark] = base[0x4];
		style.Colors[ImGuiCol_SliderGrab] = base[0x4];
		style.Colors[ImGuiCol_SliderGrabActive] = base[0x4];
		style.Colors[ImGuiCol_Button] = base[0x3];
		style.Colors[ImGuiCol_ButtonHovered] = base[0x4];
		style.Colors[ImGuiCol_ButtonActive] = base[0x4];
		style.Colors[ImGuiCol_Header] = base[0x3];
		style.Colors[ImGuiCol_HeaderHovered] = base[0x3];
		style.Colors[ImGuiCol_HeaderActive] = base[0x3];
		// style.Colors[ImGuiCol_Column]               = base[0x2];
		// style.Colors[ImGuiCol_ColumnHovered]        = base[0x2];
		// style.Colors[ImGuiCol_ColumnActive]         = base[0x2];
		style.Colors[ImGuiCol_ResizeGrip] = base[0x2];
		style.Colors[ImGuiCol_ResizeGripHovered] = base[0x2];
		style.Colors[ImGuiCol_ResizeGripActive] = base[0x2];
		style.Colors[ImGuiCol_CloseButton] = base[0x2];
		style.Colors[ImGuiCol_CloseButtonHovered] = base[0x2];
		style.Colors[ImGuiCol_CloseButtonActive] = base[0x2];
		style.Colors[ImGuiCol_PlotLines] = base[0x4];
		style.Colors[ImGuiCol_PlotLinesHovered] = base[0x4];
		style.Colors[ImGuiCol_PlotHistogram] = darken(base[0x8], 0.2);
		style.Colors[ImGuiCol_PlotHistogramHovered] = alpha(base[0x7], 0.2);
		style.Colors[ImGuiCol_TextSelectedBg] = base[0x3];
		style.Colors[ImGuiCol_ModalWindowDarkening] = alpha(base[0x1], 0.5);
		logoTexture = logoTextureLight;
	}
	else if (styleId == 2)
	{
		// base16-atelier-dune
		ImVec4 base[16] = {
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x20, 0x20, 0x1d, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x29, 0x28, 0x24, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x6e, 0x6b, 0x5e, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x7d, 0x7a, 0x68, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x99, 0x95, 0x80, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xa6, 0xa2, 0x8c, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xe8, 0xe4, 0xcf, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xfe, 0xfb, 0xec, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xd7, 0x37, 0x37, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xb6, 0x56, 0x11, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xae, 0x95, 0x13, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x60, 0xac, 0x39, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x1f, 0xad, 0x83, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x66, 0x84, 0xe1, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xb8, 0x54, 0xd4, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xd4, 0x35, 0x52, 0xff)),
		};

		style.Colors[ImGuiCol_Text] = base[0x1];
		style.Colors[ImGuiCol_TextDisabled] = base[0x2];
		style.Colors[ImGuiCol_WindowBg] = base[0x7];
		style.Colors[ImGuiCol_ChildWindowBg] = base[0x7];
		style.Colors[ImGuiCol_PopupBg] = base[0x7];
		style.Colors[ImGuiCol_Border] = transparent;
		style.Colors[ImGuiCol_BorderShadow] = transparent;
		style.Colors[ImGuiCol_FrameBg] = base[0x6];
		style.Colors[ImGuiCol_FrameBgHovered] = base[0x6];
		style.Colors[ImGuiCol_FrameBgActive] = base[0x6];
		style.Colors[ImGuiCol_TitleBg] = base[0x4];
		style.Colors[ImGuiCol_TitleBgCollapsed] = base[0x4];
		style.Colors[ImGuiCol_TitleBgActive] = base[0x4];
		style.Colors[ImGuiCol_MenuBarBg] = base[0x7];
		style.Colors[ImGuiCol_ScrollbarBg] = base[0x6];
		style.Colors[ImGuiCol_ScrollbarGrab] = base[0x5];
		style.Colors[ImGuiCol_ScrollbarGrabHovered] = base[0x5];
		style.Colors[ImGuiCol_ScrollbarGrabActive] = base[0x5];
		style.Colors[ImGuiCol_ComboBg] = base[0x6];
		style.Colors[ImGuiCol_CheckMark] = base[0x5];
		style.Colors[ImGuiCol_SliderGrab] = base[0x5];
		style.Colors[ImGuiCol_SliderGrabActive] = base[0x5];
		style.Colors[ImGuiCol_Button] = base[0x5];
		style.Colors[ImGuiCol_ButtonHovered] = base[0x6];
		style.Colors[ImGuiCol_ButtonActive] = base[0x6];
		style.Colors[ImGuiCol_Header] = base[0x6];
		style.Colors[ImGuiCol_HeaderHovered] = base[0x5];
		style.Colors[ImGuiCol_HeaderActive] = base[0x5];
		// style.Colors[ImGuiCol_Column]               = base[0x2];
		// style.Colors[ImGuiCol_ColumnHovered]        = base[0x2];
		// style.Colors[ImGuiCol_ColumnActive]         = base[0x2];
		style.Colors[ImGuiCol_ResizeGrip] = base[0x2];
		style.Colors[ImGuiCol_ResizeGripHovered] = base[0x2];
		style.Colors[ImGuiCol_ResizeGripActive] = base[0x2];
		style.Colors[ImGuiCol_CloseButton] = base[0x2];
		style.Colors[ImGuiCol_CloseButtonHovered] = base[0x2];
		style.Colors[ImGuiCol_CloseButtonActive] = base[0x2];
		style.Colors[ImGuiCol_PlotLines] = base[0x4];
		style.Colors[ImGuiCol_PlotLinesHovered] = base[0x4];
		style.Colors[ImGuiCol_PlotHistogram] = base[0xc];
		style.Colors[ImGuiCol_PlotHistogramHovered] = alpha(base[0x5], 0.8);
		style.Colors[ImGuiCol_TextSelectedBg] = base[0x3];
		style.Colors[ImGuiCol_ModalWindowDarkening] = alpha(base[0x2], 0.5);
		logoTexture = logoTextureDark;
	}
	else if (styleId == 3)
	{
		// base16-ashes
		ImVec4 base[16] = {
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x0c, 0x0d, 0x0e, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x2e, 0x2f, 0x30, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x51, 0x52, 0x53, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x73, 0x74, 0x75, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x95, 0x96, 0x97, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xb7, 0xb8, 0xb9, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xda, 0xdb, 0xdc, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xfc, 0xfd, 0xfe, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xe3, 0x1a, 0x1c, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xe6, 0x55, 0x0d, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xdc, 0xa0, 0x60, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x31, 0xa3, 0x54, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x80, 0xb1, 0xd3, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x31, 0x82, 0xbd, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0x75, 0x6b, 0xb1, 0xff)),
			ImGui::ColorConvertU32ToFloat4(IM_COL32(0xb1, 0x59, 0x28, 0xff)),
		};

		style.Colors[ImGuiCol_Text] = base[0x1];
		style.Colors[ImGuiCol_TextDisabled] = base[0x2];
		style.Colors[ImGuiCol_WindowBg] = base[0x7];
		style.Colors[ImGuiCol_ChildWindowBg] = base[0x7];
		style.Colors[ImGuiCol_PopupBg] = base[0x7];
		style.Colors[ImGuiCol_Border] = transparent;
		style.Colors[ImGuiCol_BorderShadow] = transparent;
		style.Colors[ImGuiCol_FrameBg] = base[0x6];
		style.Colors[ImGuiCol_FrameBgHovered] = base[0x6];
		style.Colors[ImGuiCol_FrameBgActive] = base[0x6];
		style.Colors[ImGuiCol_TitleBg] = base[0x4];
		style.Colors[ImGuiCol_TitleBgCollapsed] = base[0x4];
		style.Colors[ImGuiCol_TitleBgActive] = base[0x4];
		style.Colors[ImGuiCol_MenuBarBg] = base[0x7];
		style.Colors[ImGuiCol_ScrollbarBg] = base[0x6];
		style.Colors[ImGuiCol_ScrollbarGrab] = base[0x5];
		style.Colors[ImGuiCol_ScrollbarGrabHovered] = base[0x5];
		style.Colors[ImGuiCol_ScrollbarGrabActive] = base[0x5];
		style.Colors[ImGuiCol_ComboBg] = base[0x6];
		style.Colors[ImGuiCol_CheckMark] = base[0x5];
		style.Colors[ImGuiCol_SliderGrab] = base[0x5];
		style.Colors[ImGuiCol_SliderGrabActive] = base[0x5];
		style.Colors[ImGuiCol_Button] = base[0x5];
		style.Colors[ImGuiCol_ButtonHovered] = base[0x6];
		style.Colors[ImGuiCol_ButtonActive] = base[0x6];
		style.Colors[ImGuiCol_Header] = base[0x6];
		style.Colors[ImGuiCol_HeaderHovered] = base[0x5];
		style.Colors[ImGuiCol_HeaderActive] = base[0x5];
		// style.Colors[ImGuiCol_Column]               = base[0x2];
		// style.Colors[ImGuiCol_ColumnHovered]        = base[0x2];
		// style.Colors[ImGuiCol_ColumnActive]         = base[0x2];
		style.Colors[ImGuiCol_ResizeGrip] = base[0x2];
		style.Colors[ImGuiCol_ResizeGripHovered] = base[0x2];
		style.Colors[ImGuiCol_ResizeGripActive] = base[0x2];
		style.Colors[ImGuiCol_CloseButton] = base[0x2];
		style.Colors[ImGuiCol_CloseButtonHovered] = base[0x2];
		style.Colors[ImGuiCol_CloseButtonActive] = base[0x2];
		style.Colors[ImGuiCol_PlotLines] = base[0x4];
		style.Colors[ImGuiCol_PlotLinesHovered] = base[0x4];
		style.Colors[ImGuiCol_PlotHistogram] = base[0xd];
		style.Colors[ImGuiCol_PlotHistogramHovered] = alpha(base[0xc], 0.8);
		style.Colors[ImGuiCol_TextSelectedBg] = base[0x3];
		style.Colors[ImGuiCol_ModalWindowDarkening] = alpha(base[0x2], 0.5);
		logoTexture = logoTextureDark;
	}
	else if (styleId == 4)
	{
		// Modern: deep charcoal panels with a cyan accent
		ImU32 bg = IM_COL32(0x11, 0x13, 0x18, 0xff);
		ImU32 panel = IM_COL32(0x17, 0x1a, 0x20, 0xff);
		ImU32 raised = IM_COL32(0x1e, 0x22, 0x29, 0xff);
		ImU32 control = IM_COL32(0x22, 0x27, 0x30, 0xff);
		ImU32 hover = IM_COL32(0x2b, 0x31, 0x3b, 0xff);
		ImU32 pressed = IM_COL32(0x34, 0x3b, 0x47, 0xff);
		ImU32 border = IM_COL32(0x25, 0x2a, 0x33, 0xff);
		ImU32 text = IM_COL32(0xdc, 0xe2, 0xea, 0xff);
		ImU32 textDim = IM_COL32(0x87, 0x90, 0xa0, 0xff);
		ImU32 accent = IM_COL32(0x46, 0xd3, 0xea, 0xff);
		ImU32 accent2 = IM_COL32(0x7c, 0x6c, 0xf4, 0xff);
#define C(x) ImGui::ColorConvertU32ToFloat4(x)

		style.WindowRounding = 0.f;
		style.ChildWindowRounding = 10.f;
		style.FrameRounding = 6.f;
		style.GrabRounding = 3.f;
		style.ScrollbarRounding = 6.f;
		style.WindowPadding = ImVec2(14, 12);
		style.FramePadding = ImVec2(10, 6);
		style.ItemSpacing = ImVec2(8, 8);
		style.ItemInnerSpacing = ImVec2(6, 6);
		style.ScrollbarSize = 10.f;
		style.GrabMinSize = 3.f;

		style.Colors[ImGuiCol_Text] = C(text);
		style.Colors[ImGuiCol_TextDisabled] = C(textDim);
		style.Colors[ImGuiCol_WindowBg] = C(bg);
		style.Colors[ImGuiCol_ChildWindowBg] = C(panel);
		style.Colors[ImGuiCol_PopupBg] = C(raised);
		style.Colors[ImGuiCol_Border] = C(border);
		style.Colors[ImGuiCol_BorderShadow] = transparent;
		style.Colors[ImGuiCol_FrameBg] = C(control);
		style.Colors[ImGuiCol_FrameBgHovered] = C(hover);
		style.Colors[ImGuiCol_FrameBgActive] = C(pressed);
		style.Colors[ImGuiCol_TitleBg] = C(panel);
		style.Colors[ImGuiCol_TitleBgCollapsed] = C(panel);
		style.Colors[ImGuiCol_TitleBgActive] = C(raised);
		style.Colors[ImGuiCol_MenuBarBg] = C(IM_COL32(0x0d, 0x0f, 0x13, 0xff));
		style.Colors[ImGuiCol_ScrollbarBg] = transparent;
		style.Colors[ImGuiCol_ScrollbarGrab] = C(hover);
		style.Colors[ImGuiCol_ScrollbarGrabHovered] = C(pressed);
		style.Colors[ImGuiCol_ScrollbarGrabActive] = C(colAlpha(accent, 0.7f));
		style.Colors[ImGuiCol_ComboBg] = C(raised);
		style.Colors[ImGuiCol_CheckMark] = C(accent);
		style.Colors[ImGuiCol_SliderGrab] = C(accent);
		style.Colors[ImGuiCol_SliderGrabActive] = C(text);
		style.Colors[ImGuiCol_Button] = C(control);
		style.Colors[ImGuiCol_ButtonHovered] = C(hover);
		style.Colors[ImGuiCol_ButtonActive] = C(colAlpha(accent, 0.35f));
		style.Colors[ImGuiCol_Header] = C(colAlpha(accent, 0.16f));
		style.Colors[ImGuiCol_HeaderHovered] = C(colAlpha(accent, 0.24f));
		style.Colors[ImGuiCol_HeaderActive] = C(colAlpha(accent, 0.32f));
		style.Colors[ImGuiCol_Separator] = C(border);
		style.Colors[ImGuiCol_SeparatorHovered] = C(hover);
		style.Colors[ImGuiCol_SeparatorActive] = C(accent);
		style.Colors[ImGuiCol_ResizeGrip] = transparent;
		style.Colors[ImGuiCol_ResizeGripHovered] = C(colAlpha(accent, 0.4f));
		style.Colors[ImGuiCol_ResizeGripActive] = C(accent);
		style.Colors[ImGuiCol_CloseButton] = C(textDim);
		style.Colors[ImGuiCol_CloseButtonHovered] = C(text);
		style.Colors[ImGuiCol_CloseButtonActive] = C(accent);
		style.Colors[ImGuiCol_PlotLines] = C(textDim);
		style.Colors[ImGuiCol_PlotLinesHovered] = C(accent);
		style.Colors[ImGuiCol_PlotHistogram] = C(accent);
		style.Colors[ImGuiCol_PlotHistogramHovered] = C(colAlpha(accent2, 0.5f));
		style.Colors[ImGuiCol_TextSelectedBg] = C(colAlpha(accent, 0.3f));
		style.Colors[ImGuiCol_ModalWindowDarkening] = ImVec4(0.0f, 0.0f, 0.0f, 0.6f);
		logoTexture = logoTextureLight;

		theme.modern = true;
		theme.accent = accent;
		theme.accent2 = accent2;
		theme.text = text;
		theme.textDim = textDim;
		theme.display = IM_COL32(0x0b, 0x0d, 0x11, 0xff);
		theme.displayHi = IM_COL32(0x12, 0x1c, 0x24, 0xff);
		theme.grid = IM_COL32(0xff, 0xff, 0xff, 0x0c);
		theme.gridStrong = IM_COL32(0xff, 0xff, 0xff, 0x1c);
		theme.control = control;
		theme.controlHover = hover;
		theme.outline = border;
#undef C
		return;
	}

	// Classic schemes: derive the custom widget colours from the ImGui palette
	theme.modern = false;
	theme.accent = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
	theme.accent2 = ImGui::GetColorU32(ImGuiCol_PlotHistogramHovered);
	theme.text = ImGui::GetColorU32(ImGuiCol_Text);
	theme.textDim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	theme.display = ImGui::GetColorU32(ImGuiCol_FrameBg);
	theme.displayHi = ImGui::GetColorU32(ImGuiCol_WindowBg);
	theme.grid = ImGui::GetColorU32(ImGuiCol_WindowBg);
	theme.gridStrong = ImGui::GetColorU32(ImGuiCol_WindowBg);
	theme.control = ImGui::GetColorU32(ImGuiCol_FrameBg);
	theme.controlHover = ImGui::GetColorU32(ImGuiCol_FrameBgHovered);
	theme.outline = 0;
}

void uiInit()
{
	ImGui::GetIO().IniFilename = NULL;
	styleId = 4;

	// Load fonts. On Retina screens, rasterise them at the screen's pixel density and scale back down so text stays sharp.
	float pixelScale = 1.f;
	SDL_Window *window = SDL_GL_GetCurrentWindow();
	if (window)
	{
		int w, h, drawW, drawH;
		SDL_GetWindowSize(window, &w, &h);
		SDL_GL_GetDrawableSize(window, &drawW, &drawH);
		if (w > 0 && drawW > w)
			pixelScale = (float)drawW / w;
	}
	ImFontConfig fontConfig;
	fontConfig.OversampleH = (pixelScale > 1.f) ? 1 : 3;
	fontConfig.OversampleV = (pixelScale > 1.f) ? 1 : 2;
	ImGuiIO &io = ImGui::GetIO();
	io.Fonts->AddFontFromFileTTF("fonts/Inter-Medium.ttf", 14.5f * pixelScale, &fontConfig);
	theme.fontHeading = io.Fonts->AddFontFromFileTTF("fonts/Inter-SemiBold.ttf", 11.f * pixelScale, &fontConfig);
	io.FontGlobalScale = 1.f / pixelScale;
	logoTextureLight = loadImage("logo-light.png");
	logoTextureDark = loadImage("logo-dark.png");

	// Load UI settings
	// If this gets any more complicated, it should be JSON.
	{
		// Screenshot mode ignores saved settings; OSIRIS_SCREENSHOT_STYLE picks the colour scheme
		if (getenv("OSIRIS_SCREENSHOTS") && getenv("OSIRIS_SCREENSHOT_STYLE"))
			styleId = atoi(getenv("OSIRIS_SCREENSHOT_STYLE"));
		FILE *f = getenv("OSIRIS_SCREENSHOTS") ? NULL : fopen("ui.dat", "rb");
		if (f)
		{
			fread(&styleId, sizeof(styleId), 1, f);
			// Added later: blank-wave preferences (older files just stop here)
			int prefs[3];
			if (fread(prefs, sizeof(int), 3, f) == 3)
			{
				if (prefs[0] >= BLANK_ASK && prefs[0] <= BLANK_NEVER)
					blankPrefMode = prefs[0];
				if (prefs[1] >= wt::CROSSFADE && prefs[1] <= wt::SPECTRAL)
					blankPrefMorph = prefs[1];
				if (prefs[2] >= wt::EDGE_HOLD && prefs[2] <= wt::EDGE_LEAVE)
					blankPrefEdges = prefs[2];
			}
			fclose(f);
		}
	}

	refreshStyle();
}

void uiDestroy()
{
	// Save UI settings
	{
		FILE *f = getenv("OSIRIS_SCREENSHOTS") ? NULL : fopen("ui.dat", "wb");
		if (f)
		{
			fwrite(&styleId, sizeof(styleId), 1, f);
			int prefs[3] = {blankPrefMode, blankPrefMorph, blankPrefEdges};
			fwrite(prefs, sizeof(int), 3, f);
			fclose(f);
		}
	}
	batchShutdown();
}

void uiRender()
{
	renderMain();
}

void uiSetPage(int page)
{
	if (page >= 0 && page < NUM_PAGES)
		currentPage = (Page)page;
}

bool uiScreenshotStep(SDL_Window *window, const char *dir)
{
	static const char *pageNames[NUM_PAGES + 5] = {"editor", "effects", "grid", "waterfall", "import", "sdcard", "batch", "blank-waves", "keyframe-morph", "generator", "mutate-breed"};
	static int frame = 0;
	static int page = 0;
	// Give each page a few frames to settle (fonts, layout, audio thread)
	if (++frame < 30)
		return false;

	int w, h;
	SDL_GL_GetDrawableSize(window, &w, &h);
	std::vector<unsigned char> pixels(w * h * 4);
	std::vector<unsigned char> flipped(w * h * 4);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadBuffer(GL_BACK);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	for (int y = 0; y < h; y++)
	{
		memcpy(&flipped[y * w * 4], &pixels[(h - 1 - y) * w * 4], w * 4);
		for (int x = 0; x < w; x++)
			flipped[(y * w + x) * 4 + 3] = 255;
	}
	char path[1024];
	snprintf(path, sizeof(path), "%s/%d-%s.png", dir, page, pageNames[page]);
	unsigned err = lodepng_encode32_file(path, flipped.data(), w, h);
	printf("Screenshot %s (%dx%d)%s\n", path, w, h, err ? " FAILED" : "");

	frame = 0;
	page++;
	if (page >= NUM_PAGES + 5)
		return true;
	if (page < NUM_PAGES)
	{
		currentPage = (Page)page;
	}
	else if (page == NUM_PAGES)
	{
		// Batch dialog, pointed at the factory banks
		currentPage = EDITOR_PAGE;
		char banks[1024];
		snprintf(banks, sizeof(banks), "%s", getenv("OSIRIS_SCREENSHOT_BANK") ? getenv("OSIRIS_SCREENSHOT_BANK") : "banks/ROM A.wav");
		batchDemo(dirname(banks));
	}
	else if (page == NUM_PAGES + 2)
	{
		// Keyframes: sine at 1, square at 16, FM at 32
		blankCancelAll();
		currentPage = EDITOR_PAGE;
		for (int w = 0; w < BANK_LEN; w++)
			currentBank.keyframes[w] = false;
		int last = BANK_LEN - 1, mid = BANK_LEN / 2 - 1;
		for (int i = 0; i < WAVE_LEN; i++)
		{
			float th = 2.f * (float)M_PI * i / WAVE_LEN, sq = 0.f;
			for (int k = 1; k < 40; k += 2)
				sq += sinf(k * th) / k;
			currentBank.waves[0].samples[i] = 0.9f * sinf(th);
			currentBank.waves[mid].samples[i] = 0.75f * sq;
			currentBank.waves[last].samples[i] = 0.85f * sinf(th + 2.f * sinf(3.f * th));
		}
		currentBank.waves[0].commitSamples();
		currentBank.waves[mid].commitSamples();
		currentBank.waves[last].commitSamples();
		currentBank.keyframes[0] = currentBank.keyframes[mid] = currentBank.keyframes[last] = true;
		selectedId = lastSelectedId = 0;
		morphOpenDialog();
	}
	else if (page == NUM_PAGES + 3)
	{
		morphCloseDialog();
		genOpenDialog();
	}
	else if (page == NUM_PAGES + 4)
	{
		// Mutate & Breed, on top of the generator
		genOpenMutateDialog();
	}
	else
	{
		// A table with blank waves in the middle and at the end
		batchDemo(NULL);
		currentPage = EDITOR_PAGE;
		for (int w = 0; w < BANK_LEN; w++)
		{
			if ((w >= 8 && w <= 13) || w >= BANK_LEN - 2)
			{
				memset(currentBank.waves[w].samples, 0, sizeof(float) * WAVE_LEN);
				currentBank.waves[w].commitSamples();
			}
		}
		blankCheckEditor(BANK_LEN, "ROM A.wav");
	}
	return false;
}
