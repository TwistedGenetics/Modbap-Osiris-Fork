// Tests for src/wtprocess.cpp: Cycle/Normalize maths, blank detection, run finding and morph fill.
// Also checks the editor's own Wave::updatePost gives the same result as the batch code.
// Build and run with: make test

#include "OsirisEdit.hpp"
#include "wtprocess.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using namespace wt;

static int failures = 0, checks = 0;
#define CHECK(c) check((c), #c, __LINE__)
static void check(bool ok, const char *what, int line) {
	checks++;
	if (!ok) {
		failures++;
		printf("FAIL line %d: %s\n", line, what);
	}
}

static void sine(float *w, float harmonic, float amp, float phase) {
	for (int i = 0; i < WAVE; i++)
		w[i] = amp * sinf(2.f * (float)M_PI * harmonic * i / WAVE + phase);
}

/** Pseudo-random table so the tests cover arbitrary shapes */
static void noiseTable(float *s, int waves, unsigned seed) {
	for (int i = 0; i < waves * WAVE; i++) {
		seed = seed * 1103515245u + 12345u;
		s[i] = ((seed >> 8) & 0xffff) / 32768.f - 1.f;
	}
}

// The editor's Cycle, Normalize and clip exactly as they were in Wave::updatePost before they moved to wtprocess.cpp
static void referenceProcess(float *out, bool cycle, bool normalize) {
	if (cycle) {
		float start = out[0];
		float end = out[WAVE_LEN - 1] / (WAVE_LEN - 1) * WAVE_LEN;
		for (int i = 0; i < WAVE_LEN; i++)
			out[i] -= (end - start) * (i - WAVE_LEN / 2) / WAVE_LEN;
	}
	if (normalize) {
		float max = -INFINITY, min = INFINITY;
		for (int i = 0; i < WAVE_LEN; i++) {
			if (out[i] > max) max = out[i];
			if (out[i] < min) min = out[i];
		}
		if (max - min >= 1e-6) {
			for (int i = 0; i < WAVE_LEN; i++)
				out[i] = rescalef(out[i], min, max, -1.0, 1.0);
		}
		else {
			memset(out, 0, sizeof(float) * WAVE_LEN);
		}
	}
	for (int i = 0; i < WAVE_LEN; i++)
		out[i] = clampf(out[i], -1.0, 1.0);
}

static void testCycleMatchesEditor() {
	std::vector<float> table(64 * WAVE);
	noiseTable(table.data(), 64, 7);
	for (int k = 0; k < 64; k++)
		for (int i = 0; i < WAVE; i++)
			table[k * WAVE + i] *= 0.4f + k / 64.f; // some waves clip after cycling, some don't

	for (int mode = 0; mode < 4; mode++) {
		bool cycle = mode & 1, normalize = mode & 2;
		std::vector<float> ours = table, ref = table;
		processTable(ours.data(), 64, cycle, normalize);
		for (int k = 0; k < 64; k++)
			referenceProcess(&ref[k * WAVE], cycle, normalize);
		// The frozen copy is compiled separately (with -ffast-math), so allow float rounding differences
		float diff = 0.f;
		for (size_t i = 0; i < ours.size(); i++)
			diff = fmaxf(diff, fabsf(ours[i] - ref[i]));
		CHECK(diff < 1e-5f);

		// And the editor itself: load each wave, set Cycle/Normalize, compare its output
		bool same = true;
		for (int k = 0; k < 64; k++) {
			static Wave wave;
			wave.clear();
			memcpy(wave.samples, &table[k * WAVE], sizeof(float) * WAVE);
			wave.cycle = cycle;
			wave.normalize = normalize;
			wave.commitSamples();
			same = same && memcmp(wave.postSamples, &ours[k * WAVE], sizeof(float) * WAVE) == 0;
		}
		CHECK(same);
	}

	// Cycle closes the loop: the jump from the last sample back to the first shrinks to at most |first|/256
	float w[WAVE];
	sine(w, 1.f, 0.5f, 0.3f);
	for (int i = 0; i < WAVE; i++)
		w[i] += 0.3f * i / WAVE; // a ramp leaves a big jump at the loop point
	float before = fabsf(w[WAVE - 1] - w[0]);
	cycleWave(w);
	float after = fabsf(w[WAVE - 1] - w[0]);
	CHECK(before > 0.25f);
	CHECK(after < 0.01f);
}

static void testBlankDetection() {
	float t = blankThreshold();
	CHECK(fabsf(t - 0.001f) < 1e-6f);
	float w[WAVE] = {};
	CHECK(isBlank(w));
	w[100] = 0.0009f;
	CHECK(isBlank(w));
	w[100] = -0.0011f;
	CHECK(!isBlank(w));

	const int N = 32;
	std::vector<float> s(N * WAVE);
	for (int k = 0; k < N; k++)
		sine(&s[k * WAVE], 1 + k % 5, 0.8f, 0.f);
	BlankReport r = findBlanks(s.data(), N, N);
	CHECK(r.blankCount == 0 && r.runs.empty() && !r.any());

	// Runs at the start, middle and end
	int blanks[] = {0, 1, 8, 9, 10, 11, 12, 13, 20, 30, 31};
	for (int b : blanks)
		memset(&s[b * WAVE], 0, sizeof(float) * WAVE);
	r = findBlanks(s.data(), N, N);
	CHECK(r.blankCount == 11);
	CHECK(r.runs.size() == 4);
	CHECK(r.runs[0].first == 0 && r.runs[0].last == 1 && r.runs[0].before == -1 && r.runs[0].after == 2);
	CHECK(r.runs[1].first == 8 && r.runs[1].last == 13 && r.runs[1].before == 7 && r.runs[1].after == 14);
	CHECK(r.runs[2].first == 20 && r.runs[2].last == 20 && r.runs[2].before == 19 && r.runs[2].after == 21);
	CHECK(r.runs[3].first == 30 && r.runs[3].last == 31 && r.runs[3].before == 29 && r.runs[3].after == -1);
	CHECK(r.hasEdgeRuns());
	CHECK(BlankReport::describe(r.runs[1]) == "waves 9-14 (between 8 and 15)");
	CHECK(BlankReport::describe(r.runs[2]) == "wave 21 (between 20 and 22)");

	// Padding: a 16-wave file loaded into a 32-wave table. The empty second half isn't reported.
	std::vector<float> p(N * WAVE, 0.f);
	for (int k = 0; k < 16; k++)
		sine(&p[k * WAVE], 2.f, 0.5f, 0.f);
	r = findBlanks(p.data(), N, 16);
	CHECK(r.blankCount == 0 && r.paddedFrom == 16 && r.waveCount == 16);
	memset(&p[5 * WAVE], 0, sizeof(float) * WAVE);
	r = findBlanks(p.data(), N, 16);
	CHECK(r.blankCount == 1 && r.runs.size() == 1 && r.runs[0].after == 6);

	// Fully blank table
	std::vector<float> z(N * WAVE, 0.f);
	r = findBlanks(z.data(), N, N);
	CHECK(r.allBlank && !r.any());
	CHECK(fillBlanks(z.data(), N, r, CROSSFADE, EDGE_WRAP) == 0);
}

static void testCrossfade() {
	float a[WAVE], b[WAVE], out[WAVE];
	sine(a, 1.f, 0.9f, 0.f);
	sine(b, 3.f, 0.6f, 1.f);
	morph(a, b, 0.f, CROSSFADE, out);
	CHECK(memcmp(out, a, sizeof(a)) == 0);
	morph(a, b, 1.f, CROSSFADE, out);
	CHECK(memcmp(out, b, sizeof(b)) == 0);
	morph(a, b, 0.25f, CROSSFADE, out);
	CHECK(fabsf(out[17] - (a[17] * 0.75f + b[17] * 0.25f)) < 1e-6f);

	// Filling a gap: neighbours untouched, steps evenly spaced from one to the other
	const int N = 8;
	std::vector<float> s(N * WAVE, 0.f);
	sine(&s[1 * WAVE], 1.f, 1.f, 0.f);
	sine(&s[6 * WAVE], 2.f, 0.5f, 0.f);
	for (int k : {0, 7})
		sine(&s[k * WAVE], 1.f, 0.2f, 0.f);
	std::vector<float> orig = s;
	BlankReport r = findBlanks(s.data(), N, N);
	CHECK(r.runs.size() == 1 && r.runs[0].before == 1 && r.runs[0].after == 6);
	CHECK(fillBlanks(s.data(), N, r, CROSSFADE, EDGE_HOLD) == 4);
	CHECK(memcmp(&s[1 * WAVE], &orig[1 * WAVE], sizeof(float) * WAVE) == 0);
	CHECK(memcmp(&s[6 * WAVE], &orig[6 * WAVE], sizeof(float) * WAVE) == 0);
	for (int k = 2; k <= 5; k++) {
		float t = (k - 1) / 5.f;
		float expected = orig[1 * WAVE + 40] * (1 - t) + orig[6 * WAVE + 40] * t;
		CHECK(fabsf(s[k * WAVE + 40] - expected) < 1e-5f);
	}
	CHECK(findBlanks(s.data(), N, N).blankCount == 0);
}

static void testSpectral() {
	float a[WAVE], b[WAVE], out[WAVE];
	sine(a, 1.f, 0.9f, 0.f);
	sine(b, 3.f, 0.6f, 1.f);
	morph(a, b, 0.f, SPECTRAL, out);
	CHECK(memcmp(out, a, sizeof(a)) == 0);
	morph(a, b, 1.f, SPECTRAL, out);
	CHECK(memcmp(out, b, sizeof(b)) == 0);
	// Inverse of the forward transform gives the wave back
	morph(a, a, 0.5f, SPECTRAL, out);
	float err = 0;
	for (int i = 0; i < WAVE; i++)
		err = fmaxf(err, fabsf(out[i] - a[i]));
	CHECK(err < 1e-5f);

	// Out of phase: a sine and the same sine inverted. A crossfade cancels to silence half way; a spectral morph
	// keeps the level steady all the way across.
	float inv[WAVE];
	sine(a, 2.f, 0.8f, 0.f);
	sine(inv, 2.f, 0.8f, (float)M_PI);
	float level = rms(a);
	morph(a, inv, 0.5f, CROSSFADE, out);
	CHECK(rms(out) < 0.01f * level);
	float worst = 0.f;
	for (int k = 1; k < 16; k++) {
		morph(a, inv, k / 16.f, SPECTRAL, out);
		worst = fmaxf(worst, fabsf(rms(out) - level) / level);
	}
	CHECK(worst < 0.01f);

	// Same for a richer wave against its inverse, filled as a gap in a table
	const int N = 10;
	std::vector<float> s(N * WAVE, 0.f);
	noiseTable(&s[0], 1, 3);
	for (int i = 0; i < WAVE; i++)
		s[(N - 1) * WAVE + i] = -s[i];
	BlankReport r = findBlanks(s.data(), N, N);
	CHECK(fillBlanks(s.data(), N, r, SPECTRAL, EDGE_HOLD) == N - 2);
	float l0 = rms(&s[0]);
	worst = 0.f;
	for (int k = 1; k < N - 1; k++)
		worst = fmaxf(worst, fabsf(rms(&s[k * WAVE]) - l0) / l0);
	CHECK(worst < 0.02f);
}

static void testEdges() {
	const int N = 12;
	std::vector<float> base(N * WAVE, 0.f);
	// Filled: 3..8. Blank: 0..2 at the start, 9..11 at the end
	for (int k = 3; k <= 8; k++)
		sine(&base[k * WAVE], 1.f + k, 0.5f, 0.f);

	// Hold copies the nearest filled wave
	std::vector<float> s = base;
	BlankReport r = findBlanks(s.data(), N, N);
	CHECK(r.runs.size() == 2 && r.hasEdgeRuns());
	CHECK(fillBlanks(s.data(), N, r, CROSSFADE, EDGE_HOLD) == 6);
	CHECK(memcmp(&s[0], &base[3 * WAVE], sizeof(float) * WAVE) == 0);
	CHECK(memcmp(&s[11 * WAVE], &base[8 * WAVE], sizeof(float) * WAVE) == 0);

	// Leave keeps the edges blank
	s = base;
	CHECK(fillBlanks(s.data(), N, r, CROSSFADE, EDGE_LEAVE) == 0);
	CHECK(findBlanks(s.data(), N, N).blankCount == 6);

	// Wrap: one morph from wave 8 round the end of the table to wave 3, through 9, 10, 11, 0, 1, 2
	s = base;
	CHECK(fillBlanks(s.data(), N, r, CROSSFADE, EDGE_WRAP) == 6);
	int order[] = {9, 10, 11, 0, 1, 2};
	for (int j = 0; j < 6; j++) {
		float t = (j + 1) / 7.f;
		float expected = base[8 * WAVE + 50] * (1 - t) + base[3 * WAVE + 50] * t;
		CHECK(fabsf(s[order[j] * WAVE + 50] - expected) < 1e-5f);
	}

	// Only blanks at the end, wrapping back to the first wave
	s = base;
	for (int k = 0; k < 3; k++)
		sine(&s[k * WAVE], 1.f, 0.5f, 0.f);
	r = findBlanks(s.data(), N, N);
	CHECK(r.runs.size() == 1 && r.runs[0].after == -1);
	CHECK(fillBlanks(s.data(), N, r, SPECTRAL, EDGE_WRAP) == 3);
	CHECK(findBlanks(s.data(), N, N).blankCount == 0);

	// Padding is never filled
	s = base;
	r = findBlanks(s.data(), N, 9); // waves 9..11 are padding
	CHECK(r.paddedFrom == 9 && r.runs.size() == 1 && r.runs[0].first == 0);
	fillBlanks(s.data(), N, r, CROSSFADE, EDGE_WRAP);
	for (int k = 9; k < N; k++)
		CHECK(isBlank(&s[k * WAVE]));
	CHECK(!isBlank(&s[0]));
}

int main() {
	testCycleMatchesEditor();
	testBlankDetection();
	testCrossfade();
	testSpectral();
	testEdges();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
