#include "osiriscard.hpp"

#include <algorithm>
#include <ctype.h>
#include <map>
#include <stdio.h>
#include <stdlib.h>

namespace osiris {

char bankLetter(int bank) {
	return (char)('A' + bank);
}

int bankIndex(const std::string &name) {
	if (name.size() != 1)
		return -1;
	char c = (char)toupper((unsigned char)name[0]);
	if (c < 'A' || c >= 'A' + BANKS)
		return -1;
	return c - 'A';
}

int naturalCompare(const std::string &a, const std::string &b) {
	size_t i = 0, j = 0;
	while (i < a.size() && j < b.size()) {
		unsigned char ca = a[i], cb = b[j];
		if (isdigit(ca) && isdigit(cb)) {
			// Compare whole numbers by value: skip leading zeros, then longer is bigger, then digit by digit
			size_t si = i, sj = j;
			while (si < a.size() && a[si] == '0')
				si++;
			while (sj < b.size() && b[sj] == '0')
				sj++;
			size_t ei = si, ej = sj;
			while (ei < a.size() && isdigit((unsigned char)a[ei]))
				ei++;
			while (ej < b.size() && isdigit((unsigned char)b[ej]))
				ej++;
			size_t li = ei - si, lj = ej - sj;
			if (li != lj)
				return li < lj ? -1 : 1;
			int c = a.compare(si, li, b, sj, lj);
			if (c != 0)
				return c;
			// Same value: fewer leading zeros first
			if ((si - i) != (sj - j))
				return (si - i) < (sj - j) ? -1 : 1;
			i = ei;
			j = ej;
		}
		else {
			int la = tolower(ca), lb = tolower(cb);
			if (la != lb)
				return la < lb ? -1 : 1;
			i++;
			j++;
		}
	}
	if (i < a.size())
		return 1;
	if (j < b.size())
		return -1;
	// Equal ignoring case: fall back to exact order so the sort is stable and deterministic
	return a.compare(b);
}

void naturalSort(std::vector<std::string> &names) {
	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
		return naturalCompare(a, b) < 0;
	});
}

std::string sanitize(const std::string &name, const std::string &fallback) {
	std::string out;
	for (unsigned char c : name) {
		bool ok = (c < 128) && (isalnum(c) || c == '-' || c == '_');
		char d = ok ? (char)c : '_';
		// Non-ASCII bytes (accents, emoji) are dropped rather than turned into runs of '_'
		if (c >= 128)
			continue;
		if (d == '_' && !out.empty() && out.back() == '_')
			continue;
		out += d;
	}
	size_t start = out.find_first_not_of("_-");
	if (start == std::string::npos)
		return fallback;
	size_t end = out.find_last_not_of("_-");
	return out.substr(start, end - start + 1);
}

static std::string trimEnd(std::string s) {
	while (!s.empty() && (s.back() == '_' || s.back() == '-'))
		s.pop_back();
	return s;
}

std::string fileName(int bank, const std::string &pack, const std::string &slotName, int number, bool includeSlotName) {
	std::string prefix = std::string("Osiris_") + bankLetter(bank) + "_";
	std::string suffix = "_" + std::to_string(number) + ".wav";
	int budget = MAX_FILE_NAME - (int)prefix.size() - (int)suffix.size();

	std::string p = sanitize(pack, "Pack");
	std::string middle;
	if (includeSlotName) {
		std::string s = sanitize(slotName, "Wave");
		// Shorten the slot name first, keeping at least a few characters of it, then the pack name
		int room = budget - (int)p.size() - 1;
		if (room < 4) {
			int packRoom = std::max(1, budget - 1 - 4);
			p = trimEnd(p.substr(0, packRoom));
			room = budget - (int)p.size() - 1;
		}
		if ((int)s.size() > room)
			s = trimEnd(s.substr(0, std::max(1, room)));
		middle = p + "_" + s;
	}
	else {
		middle = p;
	}
	if ((int)middle.size() > budget)
		middle = trimEnd(middle.substr(0, budget));
	return prefix + middle + suffix;
}

static bool endsWithWav(const std::string &name) {
	if (name.size() < 4)
		return false;
	std::string ext = name.substr(name.size() - 4);
	for (char &c : ext)
		c = (char)tolower((unsigned char)c);
	return ext == ".wav";
}

bool isWavFile(const std::string &fileName) {
	if (fileName.empty() || fileName[0] == '.')
		return false; // hidden files and macOS "._" AppleDouble files
	return endsWithWav(fileName);
}

int parseSlotNumber(const std::string &fileName) {
	if (!endsWithWav(fileName))
		return 0;
	std::string stem = fileName.substr(0, fileName.size() - 4);
	size_t us = stem.find_last_of('_');
	if (us == std::string::npos || us + 1 >= stem.size())
		return 0;
	std::string digits = stem.substr(us + 1);
	if (digits.size() > 2)
		return 0;
	for (char c : digits)
		if (!isdigit((unsigned char)c))
			return 0;
	int n = atoi(digits.c_str());
	return (n >= 1 && n <= SLOTS) ? n : 0;
}

std::string parseMiddle(const std::string &fileName) {
	std::string stem = endsWithWav(fileName) ? fileName.substr(0, fileName.size() - 4) : fileName;
	// Strip "_<N>" if present
	if (parseSlotNumber(fileName) > 0)
		stem = stem.substr(0, stem.find_last_of('_'));
	// Strip "Osiris_<Bank>_" if present
	if (stem.size() > 9 && stem.compare(0, 7, "Osiris_") == 0 && bankIndex(stem.substr(7, 1)) >= 0 && stem[8] == '_')
		stem = stem.substr(9);
	return stem;
}

std::vector<Position> planFill(const bool occupied[BANKS][SLOTS], int startBank, int startSlot, int count) {
	std::vector<Position> out;
	if (startBank < 0 || startBank >= BANKS)
		startBank = 0;
	if (startSlot < 0 || startSlot >= SLOTS)
		startSlot = 0;
	for (int b = startBank; b < BANKS && (int)out.size() < count; b++) {
		for (int s = (b == startBank ? startSlot : 0); s < SLOTS && (int)out.size() < count; s++) {
			if (!occupied[b][s])
				out.push_back(Position{b, s});
		}
	}
	return out;
}

std::vector<Position> planBanks(int count, int startBank) {
	bool occupied[BANKS][SLOTS] = {};
	return planFill(occupied, startBank, 0, count);
}

std::vector<Issue> checkLayout(const SlotInfo slots[BANKS][SLOTS], bool slotNamesInFileNames) {
	std::vector<Issue> issues;
	bool any = false;
	std::map<std::string, std::vector<Position>> names;

	for (int b = 0; b < BANKS; b++) {
		int last = -1;
		for (int s = 0; s < SLOTS; s++)
			if (slots[b][s].filled)
				last = s;
		for (int s = 0; s < SLOTS; s++) {
			const SlotInfo &slot = slots[b][s];
			char where[16];
			snprintf(where, sizeof(where), "%c%d", bankLetter(b), s + 1);
			if (!slot.filled) {
				if (s < last)
					issues.push_back(Issue{Issue::GAP, b, s, std::string(where) + " is empty, so bank " + bankLetter(b) + " has a gap in its numbering"});
				continue;
			}
			any = true;
			if (slot.waveCount != WAVES)
				issues.push_back(Issue{Issue::WAVE_COUNT, b, s, std::string(where) + " has " + std::to_string(slot.waveCount) + " waves, not 32"});
			std::string key = sanitize(slot.name, "Wave");
			for (char &c : key)
				c = (char)tolower((unsigned char)c);
			names[key].push_back(Position{b, s});
		}
	}
	if (!any)
		issues.push_back(Issue{Issue::EMPTY_CARD, -1, -1, "The card is empty"});

	// Duplicate names only matter when they end up in the file names; otherwise they're a hint about a doubled-up slot
	for (auto &it : names) {
		if (it.second.size() < 2)
			continue;
		std::string list;
		for (size_t k = 0; k < it.second.size(); k++) {
			if (k)
				list += ", ";
			list += bankLetter(it.second[k].bank);
			list += std::to_string(it.second[k].slot + 1);
		}
		std::string text = "Slots " + list + " share the name \"" + it.first + "\"";
		if (!slotNamesInFileNames)
			text += " (fine, but check it's not the same wavetable twice)";
		issues.push_back(Issue{Issue::DUPLICATE_NAME, it.second[0].bank, it.second[0].slot, text});
	}
	return issues;
}

std::vector<float> resampleWaves(const std::vector<float> &samples, int waveCount) {
	std::vector<float> out(FILE_SAMPLES, 0.f);
	if (waveCount <= 0 || (int)samples.size() < waveCount * WAVE_SAMPLES)
		return out;
	for (int w = 0; w < WAVES; w++) {
		// Position of this output wave within the source table, end to end
		float pos = (waveCount == 1) ? 0.f : (float)w * (waveCount - 1) / (WAVES - 1);
		int a = (int)pos;
		int b = std::min(a + 1, waveCount - 1);
		float t = pos - a;
		for (int i = 0; i < WAVE_SAMPLES; i++) {
			float va = samples[a * WAVE_SAMPLES + i];
			float vb = samples[b * WAVE_SAMPLES + i];
			out[w * WAVE_SAMPLES + i] = va + (vb - va) * t;
		}
	}
	return out;
}

std::vector<float> padWaves(const std::vector<float> &samples, int waveCount) {
	std::vector<float> out(FILE_SAMPLES, 0.f);
	if (waveCount <= 0 || (int)samples.size() < waveCount * WAVE_SAMPLES)
		return out;
	for (int w = 0; w < WAVES; w++) {
		int src = std::min(w, waveCount - 1);
		std::copy(samples.begin() + src * WAVE_SAMPLES, samples.begin() + (src + 1) * WAVE_SAMPLES, out.begin() + w * WAVE_SAMPLES);
	}
	return out;
}

} // namespace osiris
