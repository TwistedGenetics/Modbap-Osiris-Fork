#include "wtprocess.hpp"

#include <math.h>
#include <string.h>

#include "pffft/pffft.h"

namespace wt {

float blankThreshold() {
	return powf(10.f, BLANK_DB / 20.f);
}

bool isBlank(const float *wave) {
	float threshold = blankThreshold();
	for (int i = 0; i < WAVE; i++)
		if (fabsf(wave[i]) >= threshold)
			return false;
	return true;
}

// Cycle, Normalize and the final clip are the editor's own maths (Wave::updatePost calls these)

void cycleWave(float *out) {
	float start = out[0];
	float end = out[WAVE - 1] / (WAVE - 1) * WAVE;
	for (int i = 0; i < WAVE; i++)
		out[i] -= (end - start) * (i - WAVE / 2) / WAVE;
}

void normalizeWave(float *out) {
	float max = -INFINITY;
	float min = INFINITY;
	for (int i = 0; i < WAVE; i++) {
		if (out[i] > max)
			max = out[i];
		if (out[i] < min)
			min = out[i];
	}
	if (max - min >= 1e-6) {
		for (int i = 0; i < WAVE; i++)
			out[i] = (out[i] - min) / (max - min) * 2.f - 1.f;
	}
	else {
		memset(out, 0, sizeof(float) * WAVE);
	}
}

void clipWave(float *out) {
	for (int i = 0; i < WAVE; i++)
		out[i] = out[i] < -1.f ? -1.f : (out[i] > 1.f ? 1.f : out[i]);
}

void processTable(float *samples, int waveCount, bool cycle, bool normalize) {
	for (int w = 0; w < waveCount; w++) {
		float *wave = samples + w * WAVE;
		if (cycle)
			cycleWave(wave);
		if (normalize)
			normalizeWave(wave);
		clipWave(wave);
	}
}

float rms(const float *wave) {
	double sum = 0.0;
	for (int i = 0; i < WAVE; i++)
		sum += (double)wave[i] * wave[i];
	return (float)sqrt(sum / WAVE);
}

////////////////////
// Blank detection
////////////////////

bool BlankReport::hasEdgeRuns() const {
	for (const Run &r : runs)
		if (r.before < 0 || r.after < 0)
			return true;
	return false;
}

std::string BlankReport::describe(const Run &run) {
	std::string s = (run.first == run.last) ? "wave " + std::to_string(run.first + 1)
		: "waves " + std::to_string(run.first + 1) + "-" + std::to_string(run.last + 1);
	if (run.before >= 0 && run.after >= 0)
		s += " (between " + std::to_string(run.before + 1) + " and " + std::to_string(run.after + 1) + ")";
	else if (run.before < 0 && run.after >= 0)
		s += " (at the start, before " + std::to_string(run.after + 1) + ")";
	else if (run.before >= 0)
		s += " (at the end, after " + std::to_string(run.before + 1) + ")";
	return s;
}

BlankReport findBlanks(const float *samples, int waveCount, int sourceWaves) {
	BlankReport r;
	int checked = waveCount;
	if (sourceWaves >= 0 && sourceWaves < waveCount) {
		checked = sourceWaves;
		r.paddedFrom = sourceWaves;
	}
	r.waveCount = checked;
	for (int w = 0; w < checked; w++)
		if (isBlank(samples + w * WAVE))
			r.blank.push_back(w);
	r.blankCount = (int)r.blank.size();
	r.allBlank = checked > 0 && r.blankCount == checked;
	if (r.allBlank || r.blankCount == 0)
		return r;

	for (size_t i = 0; i < r.blank.size();) {
		size_t j = i;
		while (j + 1 < r.blank.size() && r.blank[j + 1] == r.blank[j] + 1)
			j++;
		Run run;
		run.first = r.blank[i];
		run.last = r.blank[j];
		run.before = run.first > 0 ? run.first - 1 : -1;
		run.after = run.last < checked - 1 ? run.last + 1 : -1;
		r.runs.push_back(run);
		i = j + 1;
	}
	return r;
}

////////////////////
// Morphing
////////////////////

/** pffft ordered real FFT of one wave: out[0] = DC, out[1] = Nyquist, then (re, im) for bins 1..WAVE/2-1 */
struct Fft {
	PFFFT_Setup *setup;
	Fft() { setup = pffft_new_setup(WAVE, PFFFT_REAL); }
	~Fft() { pffft_destroy_setup(setup); }
	void forward(const float *in, float *out) { pffft_transform_ordered(setup, in, out, NULL, PFFFT_FORWARD); }
	void inverse(const float *in, float *out) {
		pffft_transform_ordered(setup, in, out, NULL, PFFFT_BACKWARD);
		for (int i = 0; i < WAVE; i++)
			out[i] /= WAVE;
	}
};

void morph(const float *a, const float *b, float t, MorphMode mode, float *out) {
	// The ends are the neighbours themselves, exactly
	if (t <= 0.f || t >= 1.f) {
		memcpy(out, t <= 0.f ? a : b, sizeof(float) * WAVE);
		return;
	}
	if (mode == CROSSFADE) {
		for (int i = 0; i < WAVE; i++)
			out[i] = a[i] + (b[i] - a[i]) * t;
		return;
	}

	Fft fft;
	float fa[WAVE], fb[WAVE], fo[WAVE];
	fft.forward(a, fa);
	fft.forward(b, fb);
	// DC and Nyquist are real: blend directly
	fo[0] = fa[0] + (fb[0] - fa[0]) * t;
	fo[1] = fa[1] + (fb[1] - fa[1]) * t;
	for (int k = 1; k < WAVE / 2; k++) {
		float ar = fa[2 * k], ai = fa[2 * k + 1];
		float br = fb[2 * k], bi = fb[2 * k + 1];
		float magA = hypotf(ar, ai), magB = hypotf(br, bi);
		float mag = magA + (magB - magA) * t;
		// Phase: take the shorter way round; if one side is silent use the other side's phase
		float phA = atan2f(ai, ar), phB = atan2f(bi, br);
		if (magA < 1e-9f)
			phA = phB;
		if (magB < 1e-9f)
			phB = phA;
		float d = phB - phA;
		while (d > (float)M_PI)
			d -= 2.f * (float)M_PI;
		while (d < -(float)M_PI)
			d += 2.f * (float)M_PI;
		float ph = phA + d * t;
		fo[2 * k] = mag * cosf(ph);
		fo[2 * k + 1] = mag * sinf(ph);
	}
	fft.inverse(fo, out);
}

int fillBlanks(float *samples, int waveCount, const BlankReport &report, MorphMode mode, EdgeMode edges) {
	if (!report.any())
		return 0;
	int n = report.waveCount; // padding is never filled
	int filled = 0;
	float tmp[WAVE];

	// Inner runs: morph from the wave before to the wave after
	for (const Run &run : report.runs) {
		if (run.before < 0 || run.after < 0)
			continue;
		const float *a = samples + run.before * WAVE;
		const float *b = samples + run.after * WAVE;
		int span = run.after - run.before;
		for (int w = run.first; w <= run.last; w++) {
			morph(a, b, (float)(w - run.before) / span, mode, tmp);
			memcpy(samples + w * WAVE, tmp, sizeof(tmp));
			filled++;
		}
	}
	if (edges == EDGE_LEAVE)
		return filled;

	// Edge runs
	const Run *lead = NULL, *trail = NULL;
	for (const Run &run : report.runs) {
		if (run.before < 0)
			lead = &run;
		else if (run.after < 0)
			trail = &run;
	}
	if (!lead && !trail)
		return filled;
	int firstFilled = lead ? lead->last + 1 : 0;
	int lastFilled = trail ? trail->first - 1 : n - 1;

	if (edges == EDGE_HOLD) {
		if (lead)
			for (int w = lead->first; w <= lead->last; w++, filled++)
				memcpy(samples + w * WAVE, samples + firstFilled * WAVE, sizeof(float) * WAVE);
		if (trail)
			for (int w = trail->first; w <= trail->last; w++, filled++)
				memcpy(samples + w * WAVE, samples + lastFilled * WAVE, sizeof(float) * WAVE);
		return filled;
	}

	// Wrap: the trailing and leading runs form one gap that goes from the last filled wave, round the end of the
	// table, to the first filled wave
	int trailLen = trail ? trail->length() : 0;
	int leadLen = lead ? lead->length() : 0;
	int gap = trailLen + leadLen;
	std::vector<float> a(samples + lastFilled * WAVE, samples + (lastFilled + 1) * WAVE);
	std::vector<float> b(samples + firstFilled * WAVE, samples + (firstFilled + 1) * WAVE);
	for (int j = 0; j < gap; j++) {
		int w = (j < trailLen) ? trail->first + j : lead->first + (j - trailLen);
		morph(a.data(), b.data(), (float)(j + 1) / (gap + 1), mode, tmp);
		memcpy(samples + w * WAVE, tmp, sizeof(tmp));
		filled++;
	}
	return filled;
}

////////////////////
// Keyframe morph
////////////////////

float curve(Curve c, float t) {
	t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
	switch (c) {
	case EASE_IN:
		return t * t;
	case EASE_OUT:
		return 1.f - (1.f - t) * (1.f - t);
	case S_CURVE:
		return t * t * (3.f - 2.f * t);
	default:
		return t;
	}
}

std::vector<int> keyframesFor(int waveCount, const bool *marked, int selStart, int selEnd) {
	std::vector<int> keys;
	if (selStart > selEnd) {
		int tmp = selStart;
		selStart = selEnd;
		selEnd = tmp;
	}
	bool range = selStart < selEnd && selStart >= 0 && selEnd < waveCount;
	int lo = range ? selStart : 0;
	int hi = range ? selEnd : waveCount - 1;
	for (int w = lo; w <= hi; w++) {
		bool key = marked && marked[w];
		if (range && (w == lo || w == hi))
			key = true;
		if (key)
			keys.push_back(w);
	}
	return keys;
}

std::vector<Segment> segmentsFor(const std::vector<int> &keyframes) {
	std::vector<Segment> segments;
	for (size_t i = 0; i + 1 < keyframes.size(); i++) {
		Segment seg;
		seg.from = keyframes[i];
		seg.to = keyframes[i + 1];
		if (seg.count() > 0)
			segments.push_back(seg);
	}
	return segments;
}

int morphKeyframes(float *samples, int waveCount, const std::vector<int> &keyframes, MorphMode mode, Curve c) {
	int written = 0;
	float tmp[WAVE];
	for (const Segment &seg : segmentsFor(keyframes)) {
		if (seg.from < 0 || seg.to >= waveCount)
			continue;
		// Copies, so the keyframes stay exactly as they were
		std::vector<float> a(samples + seg.from * WAVE, samples + (seg.from + 1) * WAVE);
		std::vector<float> b(samples + seg.to * WAVE, samples + (seg.to + 1) * WAVE);
		int span = seg.to - seg.from;
		for (int w = seg.from + 1; w < seg.to; w++) {
			morph(a.data(), b.data(), curve(c, (float)(w - seg.from) / span), mode, tmp);
			memcpy(samples + w * WAVE, tmp, sizeof(tmp));
			written++;
		}
	}
	return written;
}

} // namespace wt
