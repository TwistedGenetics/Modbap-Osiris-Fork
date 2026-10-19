#include "cardio.hpp"

#include <algorithm>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <map>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <jansson.h>
#include <sndfile.h>

#ifdef _WIN32
#include <direct.h>
#define makeDir(p) _mkdir(p)
#else
#define makeDir(p) mkdir(p, 0755)
#endif

namespace osiris {

// Longest table accepted from a WAV file, to keep memory bounded for long audio files
static const int MAX_SOURCE_WAVES = 4096;

////////////////////
// Paths
////////////////////

static std::string stripSlash(std::string p) {
	while (p.size() > 1 && (p.back() == '/' || p.back() == '\\'))
		p.pop_back();
	return p;
}

static std::string baseName(const std::string &path) {
	std::string p = stripSlash(path);
	size_t s = p.find_last_of("/\\");
	return (s == std::string::npos) ? p : p.substr(s + 1);
}

static std::string parentDir(const std::string &path) {
	std::string p = stripSlash(path);
	size_t s = p.find_last_of("/\\");
	if (s == std::string::npos)
		return ".";
	if (s == 0)
		return "/";
	return p.substr(0, s);
}

static std::string joinPath(const std::string &a, const std::string &b) {
	if (a.empty())
		return b;
	if (a.back() == '/' || a.back() == '\\')
		return a + b;
	return a + "/" + b;
}

static std::string lower(std::string s) {
	for (char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

bool directoryExists(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool fileExists(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool ensureDir(const std::string &path) {
	if (directoryExists(path))
		return true;
	return makeDir(path.c_str()) == 0 || directoryExists(path);
}

/** Names of the entries in a folder (no "." or ".."), unsorted */
static std::vector<std::string> listDir(const std::string &dir) {
	std::vector<std::string> out;
	DIR *d = opendir(dir.c_str());
	if (!d)
		return out;
	while (struct dirent *ent = readdir(d)) {
		std::string name = ent->d_name;
		if (name != "." && name != "..")
			out.push_back(name);
	}
	closedir(d);
	return out;
}

/** Child folder by name, ignoring case (FAT32 cards can show "a" instead of "A") */
static std::string findChildDir(const std::string &dir, const std::string &name) {
	std::string exact = joinPath(dir, name);
	if (directoryExists(exact))
		return exact;
	for (const std::string &entry : listDir(dir)) {
		if (lower(entry) == lower(name) && directoryExists(joinPath(dir, entry)))
			return joinPath(dir, entry);
	}
	return "";
}

std::string osirisFolderFor(const std::string &dirIn) {
	std::string dir = stripSlash(dirIn);
	if (lower(baseName(dir)) == "osiris")
		return dir;
	std::string child = findChildDir(dir, "Osiris");
	if (!child.empty())
		return child;
	// A folder that already holds the bank folders
	for (int b = 0; b < BANKS; b++) {
		if (!findChildDir(dir, std::string(1, bankLetter(b))).empty())
			return dir;
	}
	return joinPath(dir, "Osiris");
}

std::string timestamp() {
	char buf[32];
	time_t t = time(NULL);
	struct tm *lt = localtime(&t);
	strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", lt);
	return buf;
}

////////////////////
// Card
////////////////////

Card::Card() {
	clear();
}

void Card::clear() {
	for (int b = 0; b < BANKS; b++) {
		pack[b] = "";
		for (int s = 0; s < SLOTS; s++)
			slots[b][s].clear();
	}
	slotNamesInFileNames = false;
}

int Card::filledCount() const {
	int n = 0;
	for (int b = 0; b < BANKS; b++)
		for (int s = 0; s < SLOTS; s++)
			n += slots[b][s].filled ? 1 : 0;
	return n;
}

wt::BlankReport Card::blanks(int bank, int slot) const {
	const Slot &s = slots[bank][slot];
	if (!s.filled || (int)s.samples.size() < s.waveCount * WAVE_SAMPLES)
		return wt::BlankReport();
	return wt::findBlanks(s.samples.data(), s.waveCount, s.waveCount);
}

void Card::getSlotInfo(SlotInfo out[BANKS][SLOTS]) const {
	for (int b = 0; b < BANKS; b++)
		for (int s = 0; s < SLOTS; s++)
			out[b][s] = SlotInfo(slots[b][s].filled, slots[b][s].waveCount, slots[b][s].name, blanks(b, s).blankCount);
}

void Card::getOccupied(bool out[BANKS][SLOTS]) const {
	for (int b = 0; b < BANKS; b++)
		for (int s = 0; s < SLOTS; s++)
			out[b][s] = slots[b][s].filled;
}

std::string Card::fileNameFor(int bank, int slot) const {
	return fileName(bank, pack[bank], slots[bank][slot].name, slot + 1, slotNamesInFileNames);
}

void Card::closeGaps() {
	for (int b = 0; b < BANKS; b++) {
		int next = 0;
		for (int s = 0; s < SLOTS; s++) {
			if (!slots[b][s].filled)
				continue;
			if (s != next) {
				slots[b][next] = slots[b][s];
				slots[b][s].clear();
			}
			next++;
		}
	}
}

std::vector<float> slotTo32(const Slot &slot, FixMode mode) {
	if (slot.waveCount == WAVES && (int)slot.samples.size() == FILE_SAMPLES)
		return slot.samples;
	return (mode == FIX_PAD) ? padWaves(slot.samples, slot.waveCount) : resampleWaves(slot.samples, slot.waveCount);
}

////////////////////
// Loading
////////////////////

bool loadSlotFromWav(Slot &slot, const std::string &path, std::string &error) {
	SF_INFO info;
	memset(&info, 0, sizeof(info));
	SNDFILE *sf = sf_open(path.c_str(), SFM_READ, &info);
	if (!sf) {
		error = baseName(path) + ": " + sf_strerror(NULL);
		return false;
	}
	sf_count_t frames = info.frames;
	if (frames > (sf_count_t)MAX_SOURCE_WAVES * WAVE_SAMPLES)
		frames = (sf_count_t)MAX_SOURCE_WAVES * WAVE_SAMPLES;
	int waveCount = (int)(frames / WAVE_SAMPLES);
	if (waveCount < 1 || info.channels < 1) {
		sf_close(sf);
		error = baseName(path) + " is too short to be a wavetable (under 256 samples)";
		return false;
	}

	std::vector<float> interleaved((size_t)waveCount * WAVE_SAMPLES * info.channels);
	sf_count_t got = sf_readf_float(sf, interleaved.data(), (sf_count_t)waveCount * WAVE_SAMPLES);
	sf_close(sf);
	if (got < WAVE_SAMPLES) {
		error = baseName(path) + " couldn't be read";
		return false;
	}
	waveCount = (int)(got / WAVE_SAMPLES);

	Slot loaded;
	loaded.filled = true;
	loaded.waveCount = waveCount;
	loaded.source = path;
	loaded.name = parseMiddle(baseName(path));
	loaded.samples.resize((size_t)waveCount * WAVE_SAMPLES);
	for (size_t i = 0; i < loaded.samples.size(); i++) {
		// Mix to mono
		float sum = 0.f;
		for (int c = 0; c < info.channels; c++)
			sum += interleaved[i * info.channels + c];
		loaded.samples[i] = sum / info.channels;
	}
	slot = loaded;
	return true;
}

void loadSlotFromSamples(Slot &slot, const float *samples, int waveCount, const std::string &name, const std::string &source) {
	slot.clear();
	slot.filled = true;
	slot.name = name;
	slot.source = source;
	slot.waveCount = waveCount;
	slot.samples.assign(samples, samples + (size_t)waveCount * WAVE_SAMPLES);
}

std::vector<std::string> listWavFiles(const std::string &dir) {
	std::vector<std::string> names;
	for (const std::string &name : listDir(dir)) {
		if (isWavFile(name) && fileExists(joinPath(dir, name)))
			names.push_back(name);
	}
	naturalSort(names);
	std::vector<std::string> paths;
	for (const std::string &name : names)
		paths.push_back(joinPath(dir, name));
	return paths;
}

int fillFromFiles(Card &card, const std::vector<std::string> &paths, int startBank, int startSlot, std::vector<std::string> &messages) {
	int added = 0;
	int bank = startBank, slot = startSlot;
	for (size_t i = 0; i < paths.size(); i++) {
		bool occupied[BANKS][SLOTS];
		card.getOccupied(occupied);
		std::vector<Position> pos = planFill(occupied, bank, slot, 1);
		if (pos.empty()) {
			int left = (int)(paths.size() - i);
			messages.push_back(std::to_string(left) + (left == 1 ? " file didn't" : " files didn't") + " fit on the card");
			break;
		}
		std::string error;
		Slot loaded;
		if (!loadSlotFromWav(loaded, paths[i], error)) {
			messages.push_back(error);
			continue;
		}
		card.slots[pos[0].bank][pos[0].slot] = loaded;
		bank = pos[0].bank;
		slot = pos[0].slot + 1;
		added++;
	}
	return added;
}

bool openCard(Card &card, const std::string &path, std::vector<std::string> &messages) {
	std::string osirisPath = osirisFolderFor(path);
	if (!directoryExists(osirisPath)) {
		messages.push_back("No Osiris folder found in " + path);
		return false;
	}
	std::string bankDirs[BANKS];
	bool anyBank = false;
	for (int b = 0; b < BANKS; b++) {
		bankDirs[b] = findChildDir(osirisPath, std::string(1, bankLetter(b)));
		anyBank = anyBank || !bankDirs[b].empty();
	}
	if (!anyBank) {
		messages.push_back("No bank folders (A, B, C, D) found in " + osirisPath);
		return false;
	}

	card.clear();
	for (int b = 0; b < BANKS; b++) {
		if (bankDirs[b].empty())
			continue;
		std::vector<std::string> files = listWavFiles(bankDirs[b]);
		std::vector<std::string> unnumbered;
		std::map<std::string, int> middles;
		for (const std::string &file : files) {
			std::string name = baseName(file);
			int n = parseSlotNumber(name);
			if (n == 0 || card.slots[b][n - 1].filled) {
				unnumbered.push_back(file);
				continue;
			}
			std::string error;
			if (!loadSlotFromWav(card.slots[b][n - 1], file, error))
				messages.push_back(error);
			else
				middles[parseMiddle(name)]++;
		}
		// Files without a _N number (or a repeated number) go into the bank's free slots
		for (const std::string &file : unnumbered) {
			int free = -1;
			for (int s = 0; s < SLOTS && free < 0; s++)
				if (!card.slots[b][s].filled)
					free = s;
			if (free < 0) {
				messages.push_back(baseName(file) + " didn't fit in bank " + bankLetter(b));
				continue;
			}
			std::string error;
			if (!loadSlotFromWav(card.slots[b][free], file, error))
				messages.push_back(error);
			else
				messages.push_back(baseName(file) + " has no _1 to _32 number, so it went into " + bankLetter(b) + std::to_string(free + 1));
		}

		// Pack name: the most common middle part of the file names
		std::string best;
		int bestCount = 0;
		for (auto &it : middles) {
			if (it.second > bestCount) {
				best = it.first;
				bestCount = it.second;
			}
		}
		card.pack[b] = best;
		// If the names carry slot names after the pack name, split them off
		if (bestCount == 1 && middles.size() > 1) {
			std::string prefix = middles.begin()->first;
			for (auto &it : middles) {
				size_t k = 0;
				while (k < prefix.size() && k < it.first.size() && prefix[k] == it.first[k])
					k++;
				prefix = prefix.substr(0, k);
			}
			size_t us = prefix.find_last_of('_');
			card.pack[b] = (us != std::string::npos && us > 0) ? prefix.substr(0, us) : "";
			if (!card.pack[b].empty())
				card.slotNamesInFileNames = true;
		}
		for (int s = 0; s < SLOTS; s++) {
			Slot &slot = card.slots[b][s];
			if (slot.filled && !card.pack[b].empty() && slot.name.compare(0, card.pack[b].size() + 1, card.pack[b] + "_") == 0)
				slot.name = slot.name.substr(card.pack[b].size() + 1);
		}
	}
	return true;
}

////////////////////
// Writing
////////////////////

bool writeWavetable(const std::string &path, const std::vector<float> &samples, std::string &error) {
	SF_INFO info;
	memset(&info, 0, sizeof(info));
	info.samplerate = SAMPLE_RATE;
	info.channels = 1;
	info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	SNDFILE *sf = sf_open(path.c_str(), SFM_WRITE, &info);
	if (!sf) {
		error = "Couldn't write " + path + ": " + sf_strerror(NULL);
		return false;
	}
	sf_command(sf, SFC_SET_CLIPPING, NULL, SF_TRUE);
	sf_count_t written = sf_write_float(sf, samples.data(), (sf_count_t)samples.size());
	sf_close(sf);
	if (written != (sf_count_t)samples.size()) {
		error = "Couldn't finish writing " + path + " (is the card full?)";
		return false;
	}
	return true;
}

int removeMacJunk(const std::string &dir) {
	int removed = 0;
	for (const std::string &name : listDir(dir)) {
		std::string path = joinPath(dir, name);
		if (directoryExists(path)) {
			removed += removeMacJunk(path);
		}
		else if ((name.compare(0, 2, "._") == 0 || name == ".DS_Store") && fileExists(path)) {
			if (unlink(path.c_str()) == 0)
				removed++;
		}
	}
	return removed;
}

/** A backup folder name next to the Osiris folder that isn't taken yet */
static std::string newBackupDir(const std::string &cardRoot) {
	std::string base = joinPath(cardRoot, "Osiris_backup_" + timestamp());
	std::string dir = base;
	for (int k = 2; directoryExists(dir) || fileExists(dir); k++)
		dir = base + "_" + std::to_string(k);
	return dir;
}

/** Moves a file into the backup folder, keeping its bank subfolder. Never overwrites anything already there. */
static bool moveToBackup(const std::string &file, const std::string &backupRoot, const std::string &bankName, std::string &error) {
	std::string bankDir = joinPath(backupRoot, bankName);
	if (!ensureDir(backupRoot) || !ensureDir(bankDir)) {
		error = "Couldn't create backup folder " + bankDir;
		return false;
	}
	std::string target = joinPath(bankDir, baseName(file));
	for (int k = 2; fileExists(target); k++)
		target = joinPath(bankDir, baseName(file) + "." + std::to_string(k));
	if (rename(file.c_str(), target.c_str()) != 0) {
		error = "Couldn't move " + file + " to the backup folder: " + strerror(errno);
		return false;
	}
	return true;
}

/** Where an export goes: the chosen folder if it's named Osiris, otherwise an Osiris folder inside it */
static std::string exportFolderFor(const std::string &dirIn) {
	std::string dir = stripSlash(dirIn);
	if (lower(baseName(dir)) == "osiris")
		return dir;
	std::string child = findChildDir(dir, "Osiris");
	return child.empty() ? joinPath(dir, "Osiris") : child;
}

ExportResult exportCard(const Card &card, const std::string &destination, const ExportOptions &options) {
	ExportResult result;
	result.osirisPath = exportFolderFor(destination);
	std::string cardRoot = parentDir(result.osirisPath);
	bool exists = directoryExists(result.osirisPath);

	if (exists) {
		if (options.existing == ExportOptions::CANCEL) {
			result.cancelled = true;
			return result;
		}
		if (options.existing == ExportOptions::BACKUP) {
			std::string backup = newBackupDir(cardRoot);
			if (rename(result.osirisPath.c_str(), backup.c_str()) != 0) {
				result.errors.push_back("Couldn't back up the existing Osiris folder: " + std::string(strerror(errno)) + ". Nothing was written.");
				return result;
			}
			result.backupPath = backup;
		}
	}

	if (!ensureDir(result.osirisPath)) {
		result.errors.push_back("Couldn't create " + result.osirisPath);
		return result;
	}

	for (int b = 0; b < BANKS; b++) {
		std::string bankName(1, bankLetter(b));
		std::string bankDir = findChildDir(result.osirisPath, bankName);
		if (bankDir.empty())
			bankDir = joinPath(result.osirisPath, bankName);
		if (!ensureDir(bankDir)) {
			result.errors.push_back("Couldn't create " + bankDir);
			continue;
		}

		// When merging, move aside anything that shares a number with a slot about to be written
		if (exists && options.existing == ExportOptions::MERGE) {
			for (const std::string &file : listWavFiles(bankDir)) {
				int n = parseSlotNumber(baseName(file));
				if (n < 1 || !card.slots[b][n - 1].filled)
					continue;
				if (result.backupPath.empty())
					result.backupPath = newBackupDir(cardRoot);
				std::string error;
				if (moveToBackup(file, result.backupPath, bankName, error))
					result.moved.push_back(bankName + "/" + baseName(file));
				else
					result.errors.push_back(error);
			}
		}

		for (int s = 0; s < SLOTS; s++) {
			const Slot &slot = card.slots[b][s];
			if (!slot.filled)
				continue;
			std::string path = joinPath(bankDir, card.fileNameFor(b, s));
			std::string error;
			std::vector<float> data = slotTo32(slot, options.fix);
			if (options.cycle)
				wt::processTable(data.data(), WAVES, true, false);
			if (writeWavetable(path, data, error)) {
				result.written++;
				result.perBank[b]++;
			}
			else {
				result.errors.push_back(error);
			}
		}
	}

	if (options.removeJunk)
		result.junkRemoved = removeMacJunk(result.osirisPath);
	result.ok = result.errors.empty();
	return result;
}

std::vector<std::string> listVolumes() {
	std::vector<std::string> out;
#ifndef _WIN32
	const std::string root = "/Volumes";
	std::vector<std::string> names = listDir(root);
	naturalSort(names);
	for (const std::string &name : names) {
		if (name.empty() || name[0] == '.')
			continue;
		std::string path = joinPath(root, name);
		char resolved[PATH_MAX];
		// The startup disk appears as a link to "/"
		if (realpath(path.c_str(), resolved) && strcmp(resolved, "/") == 0)
			continue;
		if (directoryExists(path))
			out.push_back(path);
	}
#endif
	return out;
}

////////////////////
// Project files
////////////////////

static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const unsigned char *data, size_t len) {
	std::string out;
	out.reserve((len + 2) / 3 * 4);
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = data[i] << 16;
		if (i + 1 < len)
			v |= data[i + 1] << 8;
		if (i + 2 < len)
			v |= data[i + 2];
		out += B64[(v >> 18) & 63];
		out += B64[(v >> 12) & 63];
		out += (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
		out += (i + 2 < len) ? B64[v & 63] : '=';
	}
	return out;
}

static std::vector<unsigned char> base64Decode(const char *s) {
	std::vector<unsigned char> out;
	unsigned v = 0;
	int bits = 0;
	for (; *s; s++) {
		const char *p = strchr(B64, *s);
		if (*s == '=' || !p)
			continue;
		v = (v << 6) | (unsigned)(p - B64);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back((unsigned char)((v >> bits) & 0xff));
		}
	}
	return out;
}

/** Floats stored as little-endian 32-bit so projects are exact and portable */
static std::string encodeSamples(const std::vector<float> &samples) {
	std::vector<unsigned char> bytes(samples.size() * 4);
	for (size_t i = 0; i < samples.size(); i++) {
		uint32_t u;
		memcpy(&u, &samples[i], 4);
		bytes[i * 4 + 0] = u & 0xff;
		bytes[i * 4 + 1] = (u >> 8) & 0xff;
		bytes[i * 4 + 2] = (u >> 16) & 0xff;
		bytes[i * 4 + 3] = (u >> 24) & 0xff;
	}
	return base64Encode(bytes.data(), bytes.size());
}

static std::vector<float> decodeSamples(const char *text) {
	std::vector<unsigned char> bytes = base64Decode(text);
	std::vector<float> samples(bytes.size() / 4);
	for (size_t i = 0; i < samples.size(); i++) {
		uint32_t u = bytes[i * 4] | (bytes[i * 4 + 1] << 8) | (bytes[i * 4 + 2] << 16) | ((uint32_t)bytes[i * 4 + 3] << 24);
		memcpy(&samples[i], &u, 4);
	}
	return samples;
}

bool saveProject(const Card &card, const std::string &path, std::string &error) {
	json_t *root = json_object();
	json_object_set_new(root, "format", json_string("osiris-card"));
	json_object_set_new(root, "version", json_integer(1));
	json_object_set_new(root, "slotNamesInFileNames", json_boolean(card.slotNamesInFileNames));
	json_t *banks = json_array();
	for (int b = 0; b < BANKS; b++) {
		json_t *bank = json_object();
		json_object_set_new(bank, "bank", json_string(std::string(1, bankLetter(b)).c_str()));
		json_object_set_new(bank, "pack", json_string(card.pack[b].c_str()));
		json_t *slots = json_array();
		for (int s = 0; s < SLOTS; s++) {
			const Slot &slot = card.slots[b][s];
			if (!slot.filled)
				continue;
			json_t *j = json_object();
			json_object_set_new(j, "number", json_integer(s + 1));
			json_object_set_new(j, "name", json_string(slot.name.c_str()));
			json_object_set_new(j, "source", json_string(slot.source.c_str()));
			json_object_set_new(j, "waves", json_integer(slot.waveCount));
			json_object_set_new(j, "samples", json_string(encodeSamples(slot.samples).c_str()));
			json_array_append_new(slots, j);
		}
		json_object_set_new(bank, "slots", slots);
		json_array_append_new(banks, bank);
	}
	json_object_set_new(root, "banks", banks);
	int err = json_dump_file(root, path.c_str(), JSON_INDENT(1));
	json_decref(root);
	if (err) {
		error = "Couldn't save " + path;
		return false;
	}
	return true;
}

bool loadProject(Card &card, const std::string &path, std::string &error) {
	json_error_t jerr;
	json_t *root = json_load_file(path.c_str(), 0, &jerr);
	if (!root) {
		error = baseName(path) + " isn't a card project (" + jerr.text + ")";
		return false;
	}
	const char *format = json_string_value(json_object_get(root, "format"));
	if (!format || strcmp(format, "osiris-card") != 0) {
		json_decref(root);
		error = baseName(path) + " isn't a card project";
		return false;
	}
	Card loaded;
	loaded.slotNamesInFileNames = json_is_true(json_object_get(root, "slotNamesInFileNames"));
	json_t *banks = json_object_get(root, "banks");
	for (size_t i = 0; i < json_array_size(banks); i++) {
		json_t *bank = json_array_get(banks, i);
		const char *letter = json_string_value(json_object_get(bank, "bank"));
		int b = letter ? bankIndex(letter) : -1;
		if (b < 0)
			continue;
		const char *pack = json_string_value(json_object_get(bank, "pack"));
		loaded.pack[b] = pack ? pack : "";
		json_t *slots = json_object_get(bank, "slots");
		for (size_t k = 0; k < json_array_size(slots); k++) {
			json_t *j = json_array_get(slots, k);
			int n = (int)json_integer_value(json_object_get(j, "number"));
			int waves = (int)json_integer_value(json_object_get(j, "waves"));
			const char *samples = json_string_value(json_object_get(j, "samples"));
			if (n < 1 || n > SLOTS || waves < 1 || !samples)
				continue;
			Slot &slot = loaded.slots[b][n - 1];
			slot.filled = true;
			slot.waveCount = waves;
			const char *name = json_string_value(json_object_get(j, "name"));
			const char *source = json_string_value(json_object_get(j, "source"));
			slot.name = name ? name : "";
			slot.source = source ? source : "";
			slot.samples = decodeSamples(samples);
			slot.samples.resize((size_t)waves * WAVE_SAMPLES, 0.f);
		}
	}
	json_decref(root);
	card = loaded;
	return true;
}

} // namespace osiris
