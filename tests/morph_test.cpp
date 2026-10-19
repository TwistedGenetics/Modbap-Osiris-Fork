// Tests for Morph Between Keyframes (src/wtprocess.cpp, src/keyframes.cpp) and the wavetable generator
// (src/generator.cpp). Build and run with: make test

#include <algorithm>
#include "OsirisEdit.hpp"
#include "generator.hpp"
#include "keyframes.hpp"
#include "wtprocess.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// Normally defined in ui.cpp
int BANK_LEN = 32;
int selectedId = 0;
int lastSelectedId = 0;

static int failures = 0, checks = 0;
#define CHECK(c) check((c), #c, __LINE__)
static bool check(bool ok, const char *what, int line) {
	checks++;
	if (!ok) {
		failures++;
		if (failures < 40)
			printf("FAIL line %d: %s\n", line, what);
	}
	return ok;
}

static const float PI = 3.14159265358979f;

static void sine(float *w, float amp) {
	for (int i = 0; i < WAVE_LEN; i++)
		w[i] = amp * sinf(2.f * PI * i / WAVE_LEN);
}
static void square(float *w, float amp) {
	// Band-limited square (odd harmonics up to 63)
	for (int i = 0; i < WAVE_LEN; i++) {
		float v = 0.f;
		for (int k = 1; k < 64; k += 2)
			v += sinf(2.f * PI * k * i / WAVE_LEN) / k;
		w[i] = amp * v * 4.f / PI;
	}
}
static void fm(float *w, float amp) {
	for (int i = 0; i < WAVE_LEN; i++) {
		float th = 2.f * PI * i / WAVE_LEN;
		w[i] = amp * sinf(th + 2.f * sinf(3.f * th));
	}
}

static void testCurves() {
	wt::Curve curves[] = {wt::LINEAR, wt::EASE_IN, wt::EASE_OUT, wt::S_CURVE};
	for (wt::Curve c : curves) {
		CHECK(wt::curve(c, 0.f) == 0.f);
		CHECK(wt::curve(c, 1.f) == 1.f);
		bool monotonic = true;
		float last = -1.f;
		for (int i = 0; i <= 1000; i++) {
			float v = wt::curve(c, i / 1000.f);
			monotonic = monotonic && v >= last && v >= 0.f && v <= 1.f;
			last = v;
		}
		CHECK(monotonic);
	}
	CHECK(wt::curve(wt::EASE_IN, 0.5f) < 0.5f);
	CHECK(wt::curve(wt::EASE_OUT, 0.5f) > 0.5f);
	CHECK(fabsf(wt::curve(wt::S_CURVE, 0.5f) - 0.5f) < 1e-6f);
	CHECK(wt::curve(wt::S_CURVE, 0.1f) < 0.1f && wt::curve(wt::S_CURVE, 0.9f) > 0.9f);
}

static void testKeyframeSelection() {
	bool marked[32] = {};
	marked[0] = marked[15] = marked[31] = true;
	// No selection: every marked keyframe
	std::vector<int> k = wt::keyframesFor(32, marked, 5, 5);
	CHECK(k.size() == 3 && k[0] == 0 && k[1] == 15 && k[2] == 31);
	// Selection: its ends plus marked keyframes inside, in either direction
	k = wt::keyframesFor(32, marked, 4, 20);
	CHECK(k.size() == 3 && k[0] == 4 && k[1] == 15 && k[2] == 20);
	CHECK(wt::keyframesFor(32, marked, 20, 4) == k);
	bool none[32] = {};
	k = wt::keyframesFor(32, none, 4, 20);
	CHECK(k.size() == 2 && k[0] == 4 && k[1] == 20);
	// Selection ending on a marked keyframe: no duplicate
	k = wt::keyframesFor(32, marked, 10, 15);
	CHECK(k.size() == 2 && k[0] == 10 && k[1] == 15);

	std::vector<wt::Segment> segs = wt::segmentsFor(wt::keyframesFor(32, marked, 0, 0));
	CHECK(segs.size() == 2);
	CHECK(segs[0].from == 0 && segs[0].to == 15 && segs[0].count() == 14);
	CHECK(segs[1].from == 15 && segs[1].to == 31 && segs[1].count() == 15);
	// Neighbouring keyframes have nothing between them
	std::vector<int> adj;
	adj.push_back(3);
	adj.push_back(4);
	CHECK(wt::segmentsFor(adj).empty());
	CHECK(wt::segmentsFor(std::vector<int>(1, 7)).empty());
}

/** Sine at 1, square at 16, FM at 32 (waves 0, 15, 31), morphed in the editor */
static void testEditorExample() {
	BANK_LEN = 32;
	currentBank.clear();
	sine(currentBank.waves[0].samples, 0.9f);
	square(currentBank.waves[15].samples, 0.4f);
	currentBank.waves[15].normalize = true; // heard at full level: the morph must use that, not the raw samples
	fm(currentBank.waves[31].samples, 0.8f);
	for (int w = 1; w < 31; w++)
		if (w != 15)
			sine(currentBank.waves[w].samples, 0.1f), currentBank.waves[w].effects[PRE_GAIN] = 0.3f; // gets replaced
	for (int w = 0; w < 32; w++)
		currentBank.waves[w].commitSamples();
	currentBank.keyframes[0] = currentBank.keyframes[15] = currentBank.keyframes[31] = true;
	selectedId = lastSelectedId = 7; // no range selected: whole table

	historyClear();
	historyPushNew();
	static Bank before;
	before = currentBank;

	std::vector<int> keys = editorKeyframes();
	CHECK(keys.size() == 3 && keys[0] == 0 && keys[1] == 15 && keys[2] == 31);

	std::vector<float> preview = editorKeyframePreview(wt::CROSSFADE, wt::LINEAR);
	int written = editorKeyframeMorph(wt::CROSSFADE, wt::LINEAR);
	CHECK(written == 29);

	// Keyframes untouched, effects and all
	CHECK(memcmp(&currentBank.waves[0], &before.waves[0], sizeof(Wave)) == 0);
	CHECK(memcmp(&currentBank.waves[15], &before.waves[15], sizeof(Wave)) == 0);
	CHECK(memcmp(&currentBank.waves[31], &before.waves[31], sizeof(Wave)) == 0);

	// In-between waves: effects off, samples = crossfade of the keyframes as heard
	bool effectsOff = true, matches = true, previewMatches = true;
	for (int w = 1; w < 31; w++) {
		if (w == 15)
			continue;
		const Wave &wave = currentBank.waves[w];
		for (int e = 0; e < EFFECTS_LEN; e++)
			effectsOff = effectsOff && wave.effects[e] == 0.f;
		effectsOff = effectsOff && !wave.cycle && !wave.normalize;
		int from = w < 15 ? 0 : 15, to = w < 15 ? 15 : 31;
		float t = (float)(w - from) / (to - from);
		for (int i = 0; i < WAVE_LEN; i += 17) {
			float a = before.waves[from].postSamples[i], b = before.waves[to].postSamples[i];
			matches = matches && fabsf(wave.samples[i] - (a + (b - a) * t)) < 1e-5f;
			previewMatches = previewMatches && fabsf(preview[w * WAVE_LEN + i] - wave.postSamples[i]) < 1e-5f;
		}
	}
	CHECK(effectsOff);
	CHECK(matches);
	CHECK(previewMatches);
	// The square was quiet but normalised: half way to it, the morph is louder than the raw samples would give
	CHECK(wt::rms(currentBank.waves[14].postSamples) > 0.5f);

	// One undo step back to exactly where we were, and redo again
	static Bank morphed;
	morphed = currentBank;
	historyUndo();
	CHECK(memcmp(currentBank.waves, before.waves, sizeof(before.waves)) == 0);
	historyRedo();
	CHECK(memcmp(currentBank.waves, morphed.waves, sizeof(morphed.waves)) == 0);

	// Spectral and the other curves run too, still leaving keyframes alone
	historyUndo();
	CHECK(editorKeyframeMorph(wt::SPECTRAL, wt::S_CURVE) == 29);
	CHECK(memcmp(&currentBank.waves[15], &before.waves[15], sizeof(Wave)) == 0);

	// A selection uses its ends plus the marked keyframe inside
	historyUndo();
	selectedId = 4;
	lastSelectedId = 20;
	keys = editorKeyframes();
	CHECK(keys.size() == 3 && keys[0] == 4 && keys[1] == 15 && keys[2] == 20);
	CHECK(editorKeyframeMorph(wt::CROSSFADE, wt::EASE_IN) == 10 + 4);
	CHECK(memcmp(&currentBank.waves[2], &before.waves[2], sizeof(Wave)) == 0); // outside the selection

	// Keyframe toggling is undoable and moves with swaps
	historyUndo();
	selectedId = lastSelectedId = 5;
	editorToggleKeyframes();
	CHECK(currentBank.keyframes[5]);
	historyUndo();
	CHECK(!currentBank.keyframes[5]);
	currentBank.swap(0, 1);
	CHECK(currentBank.keyframes[1] && !currentBank.keyframes[0]);
}

////////////////////
// Generator
////////////////////

struct RuleReport {
	float worstNyquist, worstDc, worstPeakError, worstLoopRatio, minRms, worstBassRatio, worstBassPhase;
	RuleReport() : worstNyquist(0), worstDc(0), worstPeakError(0), worstLoopRatio(0), minRms(1), worstBassRatio(1e9), worstBassPhase(0) {}
};

static void checkRules(const std::vector<float> &t, int waves, bool bass, RuleReport &r) {
	float amp[WAVE_LEN / 2 + 1], ph[WAVE_LEN / 2 + 1];
	for (int w = 0; w < waves; w++) {
		const float *wave = &t[(size_t)w * WAVE_LEN];
		gen::analyse(wave, amp, ph);
		r.worstNyquist = fmaxf(r.worstNyquist, amp[WAVE_LEN / 2]);
		r.worstDc = fmaxf(r.worstDc, amp[0]);
		float peak = 0.f, maxStep = 0.f;
		for (int i = 0; i < WAVE_LEN; i++)
			peak = fmaxf(peak, fabsf(wave[i]));
		for (int i = 0; i + 1 < WAVE_LEN; i++)
			maxStep = fmaxf(maxStep, fabsf(wave[i + 1] - wave[i]));
		r.worstPeakError = fmaxf(r.worstPeakError, fabsf(peak - gen::PEAK));
		float wrap = fabsf(wave[0] - wave[WAVE_LEN - 1]);
		r.worstLoopRatio = fmaxf(r.worstLoopRatio, wrap / fmaxf(maxStep, 1e-9f));
		r.minRms = fminf(r.minRms, wt::rms(wave));
		if (bass) {
			float maxOther = 0.f;
			for (int k = 2; k < WAVE_LEN / 2; k++)
				maxOther = fmaxf(maxOther, amp[k]);
			r.worstBassRatio = fminf(r.worstBassRatio, amp[1] / fmaxf(maxOther, 1e-9f));
			float d = remainderf(ph[1] - (-PI / 2), 2.f * PI);
			r.worstBassPhase = fmaxf(r.worstBassPhase, fabsf(d));
		}
	}
}

/** A click at the loop point makes the wrap step stand out from the steps inside the wave */
static const float LOOP_RATIO_LIMIT = 1.5f;

static void testLoopCheckCatchesClicks() {
	// Negative controls: tables that do click must fail the loop check
	std::vector<float> ramp(WAVE_LEN), jump(WAVE_LEN);
	for (int i = 0; i < WAVE_LEN; i++) {
		ramp[i] = -0.9f + 1.8f * i / (WAVE_LEN - 1);                                // sawtooth with a hard reset
		jump[i] = 0.8f * sinf(2.f * PI * i / WAVE_LEN) + 0.08f * i / (WAVE_LEN - 1); // a small 0.08 step at the loop
	}
	RuleReport a, b;
	checkRules(ramp, 1, false, a);
	checkRules(jump, 1, false, b);
	CHECK(a.worstLoopRatio > LOOP_RATIO_LIMIT);
	CHECK(b.worstLoopRatio > LOOP_RATIO_LIMIT);
	CHECK(a.worstNyquist > 1e-3f && b.worstNyquist > 1e-4f); // and they aren't band-limited either
}

/** Two band-limited waves to start from: a sine with some 3rd and 5th, and a softened saw */
static std::vector<float> testSource(uint32_t seed) {
	std::vector<float> s(2 * WAVE_LEN);
	for (int i = 0; i < WAVE_LEN; i++) {
		float th = 2.f * PI * i / WAVE_LEN;
		s[i] = sinf(th) + 0.3f * sinf(3.f * th + seed) + 0.1f * sinf(5.f * th);
		float saw = 0.f;
		for (int k = 1; k < 40; k++)
			saw += sinf(k * th) / k;
		s[WAVE_LEN + i] = 0.5f * saw;
	}
	return s;
}

static float waveDistance(const float *a, const float *b) {
	float sum = 0.f;
	for (int i = 0; i < WAVE_LEN; i++)
		sum += (a[i] - b[i]) * (a[i] - b[i]);
	return sqrtf(sum / WAVE_LEN);
}

static float tableDistance(const std::vector<float> &a, const std::vector<float> &b) {
	float sum = 0.f;
	int waves = (int)a.size() / WAVE_LEN;
	for (int w = 0; w < waves; w++)
		sum += waveDistance(&a[w * WAVE_LEN], &b[w * WAVE_LEN]);
	return sum / waves;
}

/** How different two tables sound: the average difference in harmonic levels (phase doesn't change the sound) */
static float soundDistance(const std::vector<float> &a, const std::vector<float> &b) {
	float sum = 0.f;
	int waves = (int)a.size() / WAVE_LEN;
	float ampA[WAVE_LEN / 2 + 1], ampB[WAVE_LEN / 2 + 1], ph[WAVE_LEN / 2 + 1];
	for (int w = 0; w < waves; w++) {
		gen::analyse(&a[w * WAVE_LEN], ampA, ph);
		gen::analyse(&b[w * WAVE_LEN], ampB, ph);
		float d = 0.f;
		for (int k = 1; k < WAVE_LEN / 2; k++)
			d += fabsf(ampA[k] - ampB[k]);
		sum += d;
	}
	return sum / waves;
}

static void testGeneratorOptions() {
	// Types: every one has a name, a group and a place in the dialog
	int listed = 0;
	for (int g = 0; g < 3; g++)
		for (gen::Type t : gen::typesIn((gen::Group)g)) {
			CHECK(gen::typeGroup(t) == g);
			listed++;
		}
	CHECK(listed == gen::TYPE_COUNT);
	CHECK(gen::isBass(gen::TALKING) && gen::typeGroup(gen::SYNC) == gen::LEAD && gen::typeGroup(gen::CHOIR) == gen::PAD);

	// Ping-pong: goes out and back, so the last wave is close to the first and the middle is the far keyframe
	gen::Params p = gen::defaults(gen::HOOVER);
	p.motion = gen::PING_PONG;
	std::vector<float> t = gen::generate(p);
	CHECK(waveDistance(&t[31 * WAVE_LEN], &t[0]) < 0.5f * waveDistance(&t[16 * WAVE_LEN], &t[0]));
	std::vector<int> keys = gen::keyframePositions(p);
	CHECK(!keys.empty() && keys.front() == 0 && std::find(keys.begin(), keys.end(), 16) != keys.end());
	for (int w = 1; w < 16; w++) // symmetric: wave 16 + n matches wave 16 - n
		CHECK(waveDistance(&t[(16 + w) * WAVE_LEN], &t[(16 - w) * WAVE_LEN]) < 1e-4f);
	// Every keyframe sits on a whole wave, out and back: 4 keyframes on 0, 5, 11, 16 and back on 21, 27
	CHECK((int)keys.size() == 2 * p.keyframes - 2);
	for (size_t i = 1; i < keys.size(); i++)
		CHECK(keys[i] > keys[i - 1]);

	// Stepped: each keyframe held for a block of identical waves, then a jump
	p = gen::defaults(gen::STAB);
	p.motion = gen::STEPPED;
	p.keyframes = 4;
	t = gen::generate(p);
	for (int w = 1; w < 32; w++) {
		float d = waveDistance(&t[w * WAVE_LEN], &t[(w - 1) * WAVE_LEN]);
		CHECK(w % 8 == 0 ? d > 1e-3f : d < 1e-6f);
	}
	CHECK(gen::keyframePositions(p).empty());

	// Random walk: deterministic, visits both ends, differs from the sweep
	p = gen::defaults(gen::EVOLVING);
	p.motion = gen::RANDOM_WALK;
	CHECK(gen::generate(p) == gen::generate(p));
	gen::Params sweep = p;
	sweep.motion = gen::SWEEP;
	CHECK(gen::generate(p) != gen::generate(sweep));

	// Start from my waves: with none of the type mixed in, every wave is the user's wave (scaled to the peak)
	p = gen::defaults(gen::WARM);
	std::vector<float> src = testSource(3);
	p.source.assign(src.begin(), src.begin() + WAVE_LEN);
	p.sourceMix = 0.f;
	t = gen::generate(p);
	float peak = 0.f;
	for (int i = 0; i < WAVE_LEN; i++)
		peak = fmaxf(peak, fabsf(src[i]));
	std::vector<float> expected(WAVE_LEN);
	for (int i = 0; i < WAVE_LEN; i++)
		expected[i] = src[i] * gen::PEAK / peak;
	CHECK(waveDistance(&t[0], expected.data()) < 1e-3f);
	CHECK(waveDistance(&t[31 * WAVE_LEN], expected.data()) < 1e-3f);
	// Mixed in fully: starts on the user's wave, ends on the type's sound
	p.sourceMix = 1.f;
	t = gen::generate(p);
	gen::Params plain = gen::defaults(gen::WARM);
	std::vector<float> typeOnly = gen::generate(plain);
	CHECK(waveDistance(&t[0], expected.data()) < 1e-3f);
	CHECK(waveDistance(&t[31 * WAVE_LEN], &typeOnly[31 * WAVE_LEN]) < 1e-3f);
	// A blank source is ignored
	p.source.assign(WAVE_LEN, 0.f);
	CHECK(gen::generate(p) == typeOnly);

	// Mutate: deterministic, a new generation, and a small change compared with a different seed
	float mutationDistance = 0.f, seedDistance = 0.f;
	for (int ty = 0; ty < gen::TYPE_COUNT; ty++) {
		gen::Params parent = gen::defaults((gen::Type)ty);
		std::vector<float> a = gen::generate(parent);
		gen::Params child = gen::mutate(parent, 0.3f, 11);
		CHECK(gen::sameParams(child, gen::mutate(parent, 0.3f, 11)));
		CHECK(child.generation == 1 && child.type == parent.type && child.seed == parent.seed);
		mutationDistance += soundDistance(a, gen::generate(child));
		gen::Params other = parent;
		other.seed = 9000 + ty;
		seedDistance += soundDistance(a, gen::generate(other));
		// No change at amount 0, and breeding a table with itself gives it back
		gen::Params same = gen::mutate(parent, 0.f, 5);
		CHECK(gen::generate(same) == a);
		gen::Params selfChild = gen::breed(parent, parent, 0.f, 9);
		CHECK(gen::generate(selfChild) == a);
	}
	printf("mutation changes a table %.3f on average, a new seed %.3f\n", mutationDistance / gen::TYPE_COUNT, seedDistance / gen::TYPE_COUNT);
	CHECK(mutationDistance < 0.6f * seedDistance);
	// More amount, more change
	float gentle = 0.f, wild = 0.f;
	for (int ty = 0; ty < gen::TYPE_COUNT; ty++) {
		gen::Params parent = gen::defaults((gen::Type)ty);
		std::vector<float> a = gen::generate(parent);
		gentle += soundDistance(a, gen::generate(gen::mutate(parent, 0.1f, 21)));
		wild += soundDistance(a, gen::generate(gen::mutate(parent, 1.f, 21)));
	}
	CHECK(gentle < wild);
	CHECK(mutationDistance > 0.f);
	// A child of two parents takes after both
	gen::Params pa = gen::mutate(gen::defaults(gen::REESE), 0.8f, 1), pb = gen::mutate(gen::defaults(gen::REESE), 0.8f, 2);
	std::vector<float> ta = gen::generate(pa), tb = gen::generate(pb), tc = gen::generate(gen::breed(pa, pb, 0.2f, 3));
	CHECK(tc != ta && tc != tb);
	CHECK(gen::breed(pa, pb, 0.2f, 3).generation == 2);

	// Names
	p = gen::defaults(gen::SYNC);
	p.seed = 42;
	CHECK(gen::tableName(p) == "Sync_0042");
	p.generation = 3;
	p.vintage = gen::SP1200;
	CHECK(gen::tableName(p) == "Sync_0042_g3_SP1200");

	// Vintage: aliasing is allowed on purpose, every other rule still holds
	uint32_t seeds[] = {1, 427, 9999};
	for (int v = gen::S950; v < gen::VINTAGE_COUNT; v++) {
		RuleReport r;
		bool changed = true;
		for (int ty = 0; ty < gen::TYPE_COUNT; ty++)
			for (uint32_t seed : seeds) {
				gen::Params q = gen::defaults((gen::Type)ty);
				q.seed = seed;
				std::vector<float> clean = gen::generate(q);
				q.vintage = (gen::Vintage)v;
				std::vector<float> old = gen::generate(q);
				changed = changed && old != clean;
				checkRules(old, q.waves, gen::isBass(q.type), r);
			}
		bool ok = check(changed, "vintage changes the table", __LINE__);
		ok &= check(r.worstDc < 2e-3f, "vintage: no DC offset", __LINE__);
		ok &= check(r.worstPeakError < 1e-4f, "vintage: same peak", __LINE__);
		ok &= check(r.minRms > 0.05f, "vintage: no blank waves", __LINE__);
		ok &= check(r.worstLoopRatio <= LOOP_RATIO_LIMIT, "vintage: no click at the loop point", __LINE__);
		ok &= check(r.worstBassRatio >= 1.15f, "vintage: bass fundamental still the loudest", __LINE__);
		printf("vintage %-7s %s  dc %.1e  min rms %.3f  loop/max step %.2f  fundamental %.2fx\n", gen::vintageName((gen::Vintage)v),
			ok ? "ok  " : "FAIL", r.worstDc, r.minRms, r.worstLoopRatio, r.worstBassRatio);
	}
}

static void testGenerator() {
	testLoopCheckCatchesClicks();
	uint32_t seeds[] = {0, 1, 2, 7, 42, 427, 1000, 2024, 5555, 9999};
	int tables = 0;
	for (int t = 0; t < gen::TYPE_COUNT; t++) {
		gen::Type type = (gen::Type)t;
		RuleReport r;
		bool same = true, differ = false;
		std::vector<float> previous;
		for (uint32_t seed : seeds) {
			for (int variant = 0; variant < 12; variant++) {
				gen::Params p = gen::defaults(type);
				p.seed = seed;
				switch (variant) {
				case 1: p.brightness = 0.f; p.movement = 0.f; p.grit = 0.f; p.keyframes = 2; break;
				case 2: p.brightness = 1.f; p.movement = 1.f; p.grit = 1.f; p.keyframes = 8; break;
				case 3: p.morph = (p.morph == wt::SPECTRAL) ? wt::CROSSFADE : wt::SPECTRAL; break;
				case 4: p.waves = 16; break;
				case 5: p.waves = 64; p.keyframes = 7; break;
				case 6: p.motion = gen::PING_PONG; p.morph = (seed & 1) ? wt::CROSSFADE : wt::SPECTRAL; break;
				case 7: p.motion = gen::STEPPED; break;
				case 8: p.motion = gen::RANDOM_WALK; p.movement = 1.f; p.morph = wt::CROSSFADE; break;
				case 9: p.source = testSource(seed); p.sourceMix = 0.6f; break;
				case 10: p = gen::mutate(p, 0.6f, seed + 1); break;
				case 11: p = gen::breed(gen::mutate(p, 0.5f, seed + 2), gen::mutate(p, 0.5f, seed + 3), 0.5f, seed); break;
				default: break;
				}
				std::vector<float> a = gen::generate(p);
				std::vector<float> b = gen::generate(p);
				same = same && a == b;
				CHECK((int)a.size() == p.waves * WAVE_LEN);
				checkRules(a, p.waves, gen::isBass(type), r);
				if (variant == 0) {
					if (!previous.empty() && previous != a)
						differ = true;
					previous = a;
				}
				tables++;
			}
		}
		const char *name = gen::typeName(type);
		bool ok = true;
		ok &= check(same, "same seed and settings give an identical table", __LINE__);
		ok &= check(differ, "different seeds give different tables", __LINE__);
		ok &= check(r.worstNyquist < 1e-4f, "band-limited: nothing at the Nyquist bin (no hard edges, seamless loop)", __LINE__);
		ok &= check(r.worstDc < 1e-4f, "no DC offset", __LINE__);
		ok &= check(r.worstPeakError < 1e-4f, "every wave has the same peak", __LINE__);
		ok &= check(r.minRms > 0.05f, "no blank or near-silent waves", __LINE__);
		// The loop point is just another point on a smooth periodic wave: its step can be the steepest (a bass sine starts
		// at its steepest), but never stands out the way a click does (an uncycled ramp scores about 256 here)
		ok &= check(r.worstLoopRatio <= LOOP_RATIO_LIMIT, "no click at the loop point", __LINE__);
		if (gen::isBass(type)) {
			ok &= check(r.worstBassRatio >= 1.25f, "bass: fundamental is the loudest harmonic", __LINE__);
			ok &= check(r.worstBassPhase < 2e-3f, "bass: fundamental keeps the same phase on every wave", __LINE__);
		}
		printf("%-9s %s  nyquist %.1e  dc %.1e  peak err %.1e  min rms %.3f  loop/max step %.2f", name, ok ? "ok  " : "FAIL",
			r.worstNyquist, r.worstDc, r.worstPeakError, r.minRms, r.worstLoopRatio);
		if (gen::isBass(type))
			printf("  fundamental %.2fx louder, phase drift %.1e", r.worstBassRatio, r.worstBassPhase);
		printf("\n");
	}

	// Names, defaults and seeds
	gen::Params p = gen::defaults(gen::REESE);
	p.seed = 427;
	CHECK(gen::tableName(p) == "Reese_0427");
	CHECK(gen::defaults(gen::SUB).morph == wt::SPECTRAL);
	CHECK(gen::defaults(gen::WARM).morph == wt::CROSSFADE);
	uint32_t s = 427;
	for (int i = 0; i < 100; i++) {
		s = gen::nextSeed(s);
		CHECK(s < 10000);
	}
	printf("%d generated tables checked\n", tables);
}

int main() {
	testCurves();
	testKeyframeSelection();
	testEditorExample();
	testGenerator();
	testGeneratorOptions();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
