// Tests for the Osiris SD card naming, sorting and layout rules (src/osiriscard.cpp).
// Build and run with: make test

#include "osiriscard.hpp"

#include <math.h>
#include <regex>
#include <set>
#include <stdio.h>

using namespace osiris;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(a, b) checkEq((a), (b), #a, __LINE__)

static void check(bool ok, const char *what, int line) {
	checks++;
	if (!ok) {
		failures++;
		printf("FAIL line %d: %s\n", line, what);
	}
}

template <typename T>
static void checkEq(const T &a, const T &b, const char *what, int line) {
	checks++;
	if (!(a == b)) {
		failures++;
		printf("FAIL line %d: %s\n", line, what);
	}
}

static void checkEq(const std::string &a, const char *b, const char *what, int line) {
	checks++;
	if (a != b) {
		failures++;
		printf("FAIL line %d: %s\n  got:      \"%s\"\n  expected: \"%s\"\n", line, what, a.c_str(), b);
	}
}

static void testNaturalSort() {
	std::vector<std::string> v = {"wave10.wav", "wave2.wav", "Wave1.wav", "wave02b.wav", "basic.wav", "wave100.wav", "wave20.wav"};
	naturalSort(v);
	std::vector<std::string> expected = {"basic.wav", "Wave1.wav", "wave2.wav", "wave02b.wav", "wave10.wav", "wave20.wav", "wave100.wav"};
	CHECK(v == expected);

	CHECK(naturalCompare("a2", "a10") < 0);
	CHECK(naturalCompare("a10", "a2") > 0);
	CHECK(naturalCompare("Osiris_A_Pack_9.wav", "Osiris_A_Pack_10.wav") < 0);
	CHECK(naturalCompare("abc", "ABD") < 0);
	CHECK(naturalCompare("x", "x") == 0);
	CHECK(naturalCompare("x1", "x01") < 0); // same value, fewer leading zeros first

	// A typical folder of numbered wavetables comes out 1..32, not 1, 10, 11...
	std::vector<std::string> files;
	for (int i = 32; i >= 1; i--)
		files.push_back("Table " + std::to_string(i) + ".wav");
	naturalSort(files);
	for (int i = 0; i < 32; i++)
		CHECK_EQ(files[i], "Table " + std::to_string(i + 1) + ".wav");
}

static void testSanitize() {
	CHECK_EQ(sanitize("Jungle Pads"), "Jungle_Pads");
	CHECK_EQ(sanitize("  Amen / Break (v2)!! "), "Amen_Break_v2");
	CHECK_EQ(sanitize("Déjà Vu"), "Dj_Vu");
	CHECK_EQ(sanitize("__--weird--__"), "weird");
	CHECK_EQ(sanitize("ok-name_1"), "ok-name_1");
	CHECK_EQ(sanitize(""), "Pack");
	CHECK_EQ(sanitize("!!!", "Wave"), "Wave");
	CHECK_EQ(sanitize("a  b"), "a_b");
	std::regex safe("^[A-Za-z0-9_-]+$");
	const char *nasty[] = {"a:b", "c*d?e", "f\"g<h>i|j", "k\\l/m", "tab\there", "dot.name", "100%", "emoji \xf0\x9f\x8e\xb5 x"};
	for (const char *n : nasty)
		CHECK(std::regex_match(sanitize(n), safe));
}

static void testFileNames() {
	CHECK_EQ(fileName(0, "Jungle1", "", 7, false), "Osiris_A_Jungle1_7.wav");
	CHECK_EQ(fileName(3, "Signature Pak", "", 32, false), "Osiris_D_Signature_Pak_32.wav");
	CHECK_EQ(fileName(1, "", "", 1, false), "Osiris_B_Pack_1.wav");
	CHECK_EQ(fileName(2, "Reese", "Detuned Saw", 12, true), "Osiris_C_Reese_Detuned_Saw_12.wav");

	// Every name for every bank and number matches the pattern, ends in _N.wav and stays under 64 characters
	std::regex pattern("^Osiris_[A-D]_[A-Za-z0-9_-]+_([1-9]|[12][0-9]|3[0-2])\\.wav$");
	std::string longName(200, 'x');
	for (int b = 0; b < BANKS; b++) {
		for (int n = 1; n <= SLOTS; n++) {
			for (int slotNames = 0; slotNames < 2; slotNames++) {
				std::string f = fileName(b, longName, longName, n, slotNames);
				CHECK((int)f.size() <= MAX_FILE_NAME);
				CHECK(std::regex_match(f, pattern));
				CHECK_EQ(parseSlotNumber(f), n);
				f = fileName(b, "Short", "Name", n, slotNames);
				CHECK(std::regex_match(f, pattern));
			}
		}
	}
	// Long names with slot names keep some of both parts
	std::string f = fileName(0, longName, "Lead", 5, true);
	CHECK(f.find("_Lead_5.wav") != std::string::npos);
}

static void testParsing() {
	CHECK_EQ(parseSlotNumber("Osiris_A_Beatppl_32.wav"), 32);
	CHECK_EQ(parseSlotNumber("Osiris_A_SignaturePak1_1.wav"), 1);
	CHECK_EQ(parseSlotNumber("Osiris_A_x_33.wav"), 0);
	CHECK_EQ(parseSlotNumber("Osiris_A_x_0.wav"), 0);
	CHECK_EQ(parseSlotNumber("something.wav"), 0);
	CHECK_EQ(parseSlotNumber("Osiris_A_x_7.WAV"), 7);
	CHECK_EQ(parseMiddle("Osiris_A_Beatppl_32.wav"), "Beatppl");
	CHECK_EQ(parseMiddle("Osiris_C_Reese_Detuned_12.wav"), "Reese_Detuned");
	CHECK_EQ(parseMiddle("My Table.wav"), "My Table");
	CHECK(isWavFile("Osiris_A_x_1.wav"));
	CHECK(isWavFile("UPPER.WAV"));
	CHECK(!isWavFile("._Osiris_A_x_1.wav"));
	CHECK(!isWavFile(".DS_Store"));
	CHECK(!isWavFile("notes.txt"));
	CHECK_EQ(bankIndex("A"), 0);
	CHECK_EQ(bankIndex("d"), 3);
	CHECK_EQ(bankIndex("E"), -1);
	CHECK_EQ(bankIndex("AB"), -1);
}

static void testNumbering() {
	// 32 files per bank, numbered separately in each bank
	std::vector<Position> p = planBanks(100);
	CHECK_EQ((int)p.size(), 100);
	int perBank[BANKS] = {};
	for (size_t i = 0; i < p.size(); i++) {
		perBank[p[i].bank]++;
		CHECK_EQ(p[i].bank, (int)i / 32);
		CHECK_EQ(p[i].slot, (int)i % 32);
	}
	CHECK_EQ(perBank[0], 32);
	CHECK_EQ(perBank[1], 32);
	CHECK_EQ(perBank[2], 32);
	CHECK_EQ(perBank[3], 4);

	// More than a card's worth: stops at 128
	CHECK_EQ((int)planBanks(500).size(), BANKS * SLOTS);
	// Starting at bank C leaves room for 64
	CHECK_EQ((int)planBanks(100, 2).size(), 64);

	// Filling skips occupied slots and carries on into the next bank
	bool occupied[BANKS][SLOTS] = {};
	occupied[0][30] = true;
	occupied[1][0] = true;
	std::vector<Position> q = planFill(occupied, 0, 29, 4);
	CHECK_EQ((int)q.size(), 4);
	CHECK(q[0].bank == 0 && q[0].slot == 29);
	CHECK(q[1].bank == 0 && q[1].slot == 31);
	CHECK(q[2].bank == 1 && q[2].slot == 1);
	CHECK(q[3].bank == 1 && q[3].slot == 2);

	// Every position becomes a unique, correctly numbered file name
	std::set<std::string> namesSeen;
	for (const Position &pos : planBanks(128)) {
		std::string f = fileName(pos.bank, "Pack", "", pos.slot + 1, false);
		CHECK(namesSeen.insert(f).second);
		CHECK_EQ(parseSlotNumber(f), pos.slot + 1);
	}
	CHECK_EQ((int)namesSeen.size(), 128);
}

static void testChecks() {
	SlotInfo slots[BANKS][SLOTS];
	std::vector<Issue> issues = checkLayout(slots, false);
	CHECK(issues.size() == 1 && issues[0].kind == Issue::EMPTY_CARD);

	slots[0][0] = SlotInfo{true, 32, "One"};
	slots[0][2] = SlotInfo{true, 16, "Two"};
	slots[1][0] = SlotInfo{true, 32, "one"};
	issues = checkLayout(slots, true);
	int gaps = 0, counts = 0, dupes = 0;
	for (const Issue &i : issues) {
		if (i.kind == Issue::GAP) {
			gaps++;
			CHECK(i.bank == 0 && i.slot == 1);
		}
		if (i.kind == Issue::WAVE_COUNT) {
			counts++;
			CHECK(i.bank == 0 && i.slot == 2);
		}
		if (i.kind == Issue::DUPLICATE_NAME)
			dupes++;
	}
	CHECK_EQ(gaps, 1);
	CHECK_EQ(counts, 1);
	CHECK_EQ(dupes, 1);
}

static void testWaveCounts() {
	// 4 waves, each a constant level 0, 1, 2, 3
	std::vector<float> four(4 * WAVE_SAMPLES);
	for (int w = 0; w < 4; w++)
		for (int i = 0; i < WAVE_SAMPLES; i++)
			four[w * WAVE_SAMPLES + i] = (float)w;

	std::vector<float> r = resampleWaves(four, 4);
	CHECK_EQ((int)r.size(), FILE_SAMPLES);
	CHECK(fabsf(r[0] - 0.f) < 1e-6f);
	CHECK(fabsf(r[31 * WAVE_SAMPLES] - 3.f) < 1e-6f);
	// Smoothly increasing from start to end
	for (int w = 1; w < WAVES; w++)
		CHECK(r[w * WAVE_SAMPLES] >= r[(w - 1) * WAVE_SAMPLES]);

	std::vector<float> p = padWaves(four, 4);
	CHECK_EQ((int)p.size(), FILE_SAMPLES);
	CHECK(fabsf(p[3 * WAVE_SAMPLES] - 3.f) < 1e-6f);
	CHECK(fabsf(p[20 * WAVE_SAMPLES] - 3.f) < 1e-6f);

	// 64 waves squeeze to 32; padding 64 keeps the first 32
	std::vector<float> many(64 * WAVE_SAMPLES);
	for (int w = 0; w < 64; w++)
		for (int i = 0; i < WAVE_SAMPLES; i++)
			many[w * WAVE_SAMPLES + i] = (float)w;
	CHECK(fabsf(resampleWaves(many, 64)[31 * WAVE_SAMPLES] - 63.f) < 1e-4f);
	CHECK(fabsf(padWaves(many, 64)[31 * WAVE_SAMPLES] - 31.f) < 1e-6f);

	// A single wave fills all 32
	std::vector<float> one(WAVE_SAMPLES, 0.5f);
	std::vector<float> r1 = resampleWaves(one, 1);
	CHECK(fabsf(r1[FILE_SAMPLES - 1] - 0.5f) < 1e-6f);
}

int main() {
	testNaturalSort();
	testSanitize();
	testFileNames();
	testParsing();
	testNumbering();
	testChecks();
	testWaveCounts();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
