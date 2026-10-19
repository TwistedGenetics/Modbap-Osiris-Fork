#pragma once

/** Reading and writing Osiris SD cards and card projects. No UI code (the SD Card page lives in cardpage.cpp). */

#include "osiriscard.hpp"
#include "wtprocess.hpp"

#include <string>
#include <vector>

namespace osiris {

struct Slot {
	bool filled;
	std::string name;   // shown in the app, and used in file names if that option is on
	std::string source; // where it came from (file path, "Editor", ...)
	int waveCount;
	std::vector<float> samples; // waveCount * WAVE_SAMPLES

	Slot() : filled(false), waveCount(0) {}
	void clear() { *this = Slot(); }
};

struct Card {
	std::string pack[BANKS];
	Slot slots[BANKS][SLOTS];
	bool slotNamesInFileNames;

	Card();
	void clear();
	int filledCount() const;
	void getSlotInfo(SlotInfo out[BANKS][SLOTS]) const;
	void getOccupied(bool out[BANKS][SLOTS]) const;
	/** File name a slot will be written as */
	std::string fileNameFor(int bank, int slot) const;
	/** Moves filled slots up so each bank is numbered 1..N with no gaps */
	void closeGaps();
	/** Blank waves in one slot (0 for an empty slot) */
	wt::BlankReport blanks(int bank, int slot) const;
};

/** What to do with slots that aren't 32 waves */
enum FixMode {
	FIX_RESAMPLE, // stretch or squeeze to 32 waves
	FIX_PAD,      // keep the first 32, repeating the last wave if there are fewer
};
std::vector<float> slotTo32(const Slot &slot, FixMode mode);

/** Fills a slot from a WAV file. Any sample rate, bit depth or channel count is accepted (mixed to mono);
the length is split into 256-sample waves. Returns false and sets `error` if the file can't be read. */
bool loadSlotFromWav(Slot &slot, const std::string &path, std::string &error);
/** Fills a slot from samples in memory, e.g. the wavetable open in the editor */
void loadSlotFromSamples(Slot &slot, const float *samples, int waveCount, const std::string &name, const std::string &source);

/** WAV files in a folder (not subfolders), in natural order, skipping hidden and "._" files. Full paths. */
std::vector<std::string> listWavFiles(const std::string &dir);

/** Fills free slots from `paths`, starting at (bank, slot). Returns how many were added;
files that couldn't be read or didn't fit are described in `messages`. */
int fillFromFiles(Card &card, const std::vector<std::string> &paths, int startBank, int startSlot, std::vector<std::string> &messages);

/** Folder holding the bank folders: `dir` itself if it's named Osiris or contains A-D, otherwise dir/Osiris */
std::string osirisFolderFor(const std::string &dir);
bool directoryExists(const std::string &path);

/** Reads an Osiris folder (or a card root containing one) into the card. Files are placed by their _N number;
files without one go into the next free slots. Returns false if no bank folders were found. */
bool openCard(Card &card, const std::string &path, std::vector<std::string> &messages);

struct ExportOptions {
	enum Existing {
		BACKUP, // rename the existing Osiris folder to Osiris_backup_<date-time>, then write a fresh one
		MERGE,  // keep the existing folder; files that would clash with a new slot number are moved to a backup folder
		CANCEL, // don't touch anything if an Osiris folder already exists
	};
	Existing existing;
	FixMode fix;
	bool removeJunk; // delete macOS "._" and .DS_Store files from the Osiris folder afterwards
	bool cycle;      // apply the editor's Cycle to every wave on the way out
	ExportOptions() : existing(BACKUP), fix(FIX_RESAMPLE), removeJunk(true), cycle(false) {}
};

struct ExportResult {
	bool ok;
	bool cancelled;
	std::string osirisPath;
	std::string backupPath;            // empty if nothing was backed up
	int written;
	int perBank[BANKS];
	std::vector<std::string> moved;    // files moved into the backup folder when merging
	int junkRemoved;
	std::vector<std::string> errors;
	ExportResult() : ok(false), cancelled(false), written(0), perBank(), junkRemoved(0) {}
};

/** Writes the card to <destination>/Osiris/A..D. `destination` is the card root (or any folder). */
ExportResult exportCard(const Card &card, const std::string &destination, const ExportOptions &options);

/** Writes one 32-wave file: 16-bit, 44.1 kHz, mono */
bool writeWavetable(const std::string &path, const std::vector<float> &samples, std::string &error);

/** Deletes macOS "._*" AppleDouble files and .DS_Store files under `dir`. Returns how many were removed. */
int removeMacJunk(const std::string &dir);

/** Mounted volumes under /Volumes (macOS), excluding the startup disk. Full paths. */
std::vector<std::string> listVolumes();

/** Card project: slot layout, names, pack names and the audio itself, so the card can be rebuilt later */
bool saveProject(const Card &card, const std::string &path, std::string &error);
bool loadProject(Card &card, const std::string &path, std::string &error);

/** "20261019-163005" */
std::string timestamp();

} // namespace osiris
