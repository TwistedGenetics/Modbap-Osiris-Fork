#pragma once

/** Rules for laying out a Modbap Osiris SD card: naming, sorting, numbering and checks.
No file I/O and no UI here, so it can be tested on its own (see tests/card_test.cpp).

Card layout (from the Osiris manual):
	<card root>/Osiris/A, B, C, D
	32 files per bank, each file 32 waves, 16-bit 44.1 kHz mono WAV
	file names end in _1.wav ... _32.wav, numbered separately in each bank
	this app names them Osiris_<Bank>_<PackName>_<Number>.wav
*/

#include <string>
#include <vector>

namespace osiris {

static const int BANKS = 4;
static const int SLOTS = 32;          // files per bank
static const int WAVES = 32;          // waves per file
static const int WAVE_SAMPLES = 256;  // samples per wave
static const int FILE_SAMPLES = WAVES * WAVE_SAMPLES;
static const int SAMPLE_RATE = 44100;
static const int MAX_FILE_NAME = 63;  // keep file names under 64 characters

/** 'A' .. 'D' */
char bankLetter(int bank);
/** 0..3 for 'A'..'D' (either case), -1 otherwise */
int bankIndex(const std::string &name);

/** Compares names so that numbers sort by value ("wave2" before "wave10"), ignoring case.
Returns <0, 0 or >0. */
int naturalCompare(const std::string &a, const std::string &b);
void naturalSort(std::vector<std::string> &names);

/** Makes a name safe for a FAT32 card: ASCII letters, digits, '-' and '_' only.
Spaces and other characters become '_', runs of '_' collapse, and leading/trailing '_' or '-' are trimmed.
Returns `fallback` if nothing is left. */
std::string sanitize(const std::string &name, const std::string &fallback = "Pack");

/** Osiris_<Bank>_<Pack>_<Number>.wav, or Osiris_<Bank>_<Pack>_<SlotName>_<Number>.wav when `includeSlotName` is set.
`number` is 1-based (1..32). Names are sanitised and shortened so the result is at most MAX_FILE_NAME characters. */
std::string fileName(int bank, const std::string &pack, const std::string &slotName, int number, bool includeSlotName);

/** True for "*.wav" (any case), ignoring macOS "._" AppleDouble files and hidden files */
bool isWavFile(const std::string &fileName);
/** The number N from a name ending in "_N.wav", if it's 1..32. Otherwise 0. */
int parseSlotNumber(const std::string &fileName);
/** The part between "Osiris_<Bank>_" and "_<N>.wav". Without that pattern, the file name without ".wav". */
std::string parseMiddle(const std::string &fileName);

struct Position {
	int bank;
	int slot; // 0-based
};

/** Picks free slots for `count` new items, starting at (startBank, startSlot) and moving on through later banks.
Occupied slots are skipped. Returns fewer positions than `count` if the card fills up. */
std::vector<Position> planFill(const bool occupied[BANKS][SLOTS], int startBank, int startSlot, int count);

/** Same for a whole card filled from a list of files (the Convert dialog): always 32 per bank, A to D. */
std::vector<Position> planBanks(int count, int startBank = 0);

/** What the checks need to know about one slot */
struct SlotInfo {
	bool filled;
	int waveCount;
	std::string name;
	SlotInfo() : filled(false), waveCount(0) {}
	SlotInfo(bool filled, int waveCount, const std::string &name) : filled(filled), waveCount(waveCount), name(name) {}
};

struct Issue {
	enum Kind {
		GAP,            // empty slot before a filled one in the same bank
		WAVE_COUNT,     // file isn't 32 waves
		DUPLICATE_NAME, // two slots share a name
		EMPTY_CARD,     // nothing to export
	};
	Kind kind;
	int bank;
	int slot; // 0-based, -1 when not about one slot
	std::string text;
};

/** Checks a card layout before export */
std::vector<Issue> checkLayout(const SlotInfo slots[BANKS][SLOTS], bool slotNamesInFileNames);

/** Stretches or squeezes `waveCount` waves of WAVE_SAMPLES into exactly 32, blending neighbouring waves */
std::vector<float> resampleWaves(const std::vector<float> &samples, int waveCount);
/** Keeps the first 32 waves; if there are fewer, repeats the last wave to fill */
std::vector<float> padWaves(const std::vector<float> &samples, int waveCount);

} // namespace osiris
