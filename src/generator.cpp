#include "generator.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pffft/pffft.h"

namespace gen {

namespace {

const int N = wt::WAVE;   // samples per wave
const int H = MAX_HARMONIC;
const int OVER = 2048;    // oversampled length for recipes shaped in the time domain (FM, folding, clipping)
const float PI = 3.14159265358979f;
const float BASS_PHASE = -PI / 2; // fundamental as a sine starting at zero, rising
const float BASS_DOMINANCE = 1.3f; // the fundamental is at least this much louder than any other harmonic

/** Small deterministic random generator (same numbers on every platform). It can replay a recorded list of draws (the
genes of a mutated table) and record the draws it makes. */
struct Rng {
	uint64_t s;
	const std::vector<uint32_t> *replay;
	std::vector<uint32_t> *record;
	size_t pos;
	explicit Rng(uint64_t seed, const std::vector<uint32_t> *replay = NULL, std::vector<uint32_t> *record = NULL)
		: s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull), replay(replay), record(record), pos(0) {
		for (int i = 0; i < 4; i++)
			raw();
	}
	uint32_t raw() {
		// SplitMix64
		uint64_t z = (s += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return (uint32_t)((z ^ (z >> 31)) >> 32);
	}
	uint32_t next() {
		uint32_t r = raw();
		if (replay && pos < replay->size())
			r = (*replay)[pos];
		pos++;
		if (record)
			record->push_back(r);
		return r;
	}
	float uniform() { return (next() >> 8) * (1.f / 16777216.f); } // 0..1
	float range(float a, float b) { return a + (b - a) * uniform(); }
	// Scaled from the top bits, so a slightly mutated draw only occasionally picks a different integer
	int integer(int a, int b) { return a + (int)(((uint64_t)next() * (uint64_t)(b - a + 1)) >> 32); }
	float gaussian() { return (uniform() + uniform() + uniform() - 1.5f) * 2.f; }
};

/** Harmonics 1..H; [0] is DC and always zero */
struct Spectrum {
	float amp[H + 1];
	float ph[H + 1];
	Spectrum() {
		memset(amp, 0, sizeof(amp));
		memset(ph, 0, sizeof(ph));
	}
};

struct Fft {
	PFFFT_Setup *setup;
	int n;
	explicit Fft(int n) : n(n) { setup = pffft_new_setup(n, PFFFT_REAL); }
	~Fft() { pffft_destroy_setup(setup); }
};

/** Sum of amp[k] cos(k theta + ph[k]), sampled `len` times over one cycle */
void toSamples(const Spectrum &s, float *out, int len) {
	Fft fft(len);
	std::vector<float> X(len, 0.f);
	for (int k = 1; k <= H; k++) {
		X[2 * k] = 0.5f * len * s.amp[k] * cosf(s.ph[k]);
		X[2 * k + 1] = 0.5f * len * s.amp[k] * sinf(s.ph[k]);
	}
	pffft_transform_ordered(fft.setup, X.data(), out, NULL, PFFFT_BACKWARD);
	for (int i = 0; i < len; i++)
		out[i] /= len;
}

/** One wave of N samples */
void toWave(const Spectrum &s, float *out) {
	toSamples(s, out, N);
}

/** Harmonics 1..H of a waveform sampled `len` times over one cycle (anything above H is dropped) */
void fromSamples(const float *y, int len, Spectrum &s) {
	Fft fft(len);
	std::vector<float> Y(len);
	pffft_transform_ordered(fft.setup, y, Y.data(), NULL, PFFFT_FORWARD);
	s = Spectrum();
	for (int k = 1; k <= H; k++) {
		float re = Y[2 * k], im = Y[2 * k + 1];
		s.amp[k] = 2.f * hypotf(re, im) / len;
		s.ph[k] = atan2f(im, re);
	}
}

/** Smooth lowpass over harmonic numbers */
float lowpass(int k, float cutoff, float order = 4.f) {
	return 1.f / (1.f + powf(k / fmaxf(cutoff, 1.f), order));
}

/** Resonant lowpass (acid style) */
float resonant(int k, float cutoff, float q) {
	float x = k / fmaxf(cutoff, 1.f);
	return 1.f / sqrtf((1.f - x * x) * (1.f - x * x) + (x / q) * (x / q));
}

float gauss(float x, float centre, float width) {
	float d = (x - centre) / width;
	return expf(-d * d);
}

/** Soft clip / wavefold used for grit */
float fold(float x) {
	return sinf(x * PI * 0.5f);
}

/** Triangle wavefolder: reflects x back into -1..1 however far it goes */
float triFold(float x) {
	float y = fmodf(x + 1.f, 4.f);
	if (y < 0.f)
		y += 4.f;
	return y < 2.f ? y - 1.f : 3.f - y;
}

float spectrumRms(const Spectrum &s) {
	float sum = 0.f;
	for (int k = 1; k <= H; k++)
		sum += s.amp[k] * s.amp[k];
	return sqrtf(sum * 0.5f);
}

/** Saturates a keyframe (oversampled, from its harmonics) and band-limits it again */
void saturate(Spectrum &s, float amount, float cutoff) {
	float w[N];
	toWave(s, w);
	float peak = 1e-9f;
	for (int n = 0; n < N; n++)
		peak = fmaxf(peak, fabsf(w[n]));
	std::vector<float> y(OVER);
	toSamples(s, y.data(), OVER);
	for (int n = 0; n < OVER; n++)
		y[n] = fold(y[n] / peak * (1.f + 1.5f * amount)) * peak;
	Spectrum t;
	fromSamples(y.data(), OVER, t);
	for (int k = 1; k <= H; k++)
		t.amp[k] *= lowpass(k, cutoff);
	s = t;
}

/** A blend of two keyframes, t = 0..1. Crossfade mixes the waves (their harmonics as complex numbers); spectral mixes
the harmonic levels and turns each phase the short way round, which keeps the level up between out-of-phase waves. */
Spectrum blend(const Spectrum &a, const Spectrum &b, float t, wt::MorphMode mode) {
	Spectrum s;
	if (mode == wt::CROSSFADE) {
		for (int k = 1; k <= H; k++) {
			float re = (1.f - t) * a.amp[k] * cosf(a.ph[k]) + t * b.amp[k] * cosf(b.ph[k]);
			float im = (1.f - t) * a.amp[k] * sinf(a.ph[k]) + t * b.amp[k] * sinf(b.ph[k]);
			s.amp[k] = hypotf(re, im);
			s.ph[k] = atan2f(im, re);
		}
		// Thinned out (the harmonics cancel): use the spectral blend instead
		if (spectrumRms(s) >= 0.35f * fminf(spectrumRms(a), spectrumRms(b)))
			return s;
	}
	for (int k = 1; k <= H; k++) {
		s.amp[k] = (1.f - t) * a.amp[k] + t * b.amp[k];
		s.ph[k] = a.ph[k] + t * remainderf(b.ph[k] - a.ph[k], 2.f * PI);
	}
	return s;
}

/** A value that wanders from keyframe to keyframe: `positions` evenly spread, random walk scaled by movement */
std::vector<float> wander(Rng &rng, int count, float start, float spread, float movement) {
	std::vector<float> v(count);
	float x = start;
	for (int i = 0; i < count; i++) {
		v[i] = x;
		x += rng.range(-1.f, 1.f) * spread * movement;
	}
	return v;
}

// Formants for vowels, as harmonic numbers of a 55 Hz bass note (F1, F2)
const float VOWELS[5][2] = {
	{300 / 55.f, 870 / 55.f},   // oo
	{500 / 55.f, 900 / 55.f},   // oh
	{700 / 55.f, 1150 / 55.f},  // ah
	{530 / 55.f, 1850 / 55.f},  // eh
	{270 / 55.f, 2300 / 55.f},  // ee
};

/** Builds the keyframe spectra for one table */
std::vector<Spectrum> keyframeSpectra(const Params &p, Rng &rng) {
	int K = p.keyframes < 2 ? 2 : (p.keyframes > 8 ? 8 : p.keyframes);
	float b = p.brightness, m = p.movement, g = p.grit;
	std::vector<Spectrum> keys(K);

	// Phases fixed for the whole table (used by pads and leads so neighbouring keyframes don't cancel)
	float fixedPhase[H + 1];
	for (int k = 0; k <= H; k++)
		fixedPhase[k] = rng.range(-PI, PI);

	switch (p.type) {
	case SUB: {
		float h2 = rng.range(0.6f, 1.f), h3 = rng.range(0.4f, 1.f), h4 = rng.range(0.f, 1.f);
		std::vector<float> drift = wander(rng, K, 0.f, 0.6f, m);
		for (int i = 0; i < K; i++) {
			Spectrum &s = keys[i];
			float d = 1.f + drift[i];
			s.amp[1] = 1.f;
			s.amp[2] = fmaxf(0.f, (0.06f + 0.22f * b) * h2 * d);
			s.amp[3] = fmaxf(0.f, (0.03f + 0.12f * b) * h3 * (2.f - d));
			s.amp[4] = fmaxf(0.f, 0.08f * g * h4 * d);
			s.amp[5] = 0.04f * g * g;
			for (int k = 1; k <= 5; k++)
				s.ph[k] = BASS_PHASE + (k > 1 ? fixedPhase[k] * 0.3f : 0.f);
		}
		break;
	}
	case REESE: {
		// Two saws with a small phase offset between them that drifts across the table: the harmonics comb in and out
		// like detuned saws beating. The offset stays small so the fundamental never cancels.
		std::vector<float> delta = wander(rng, K, rng.range(0.02f, 0.08f), 0.18f, 0.3f + m);
		float cutoff = 6.f + powf(b, 1.5f) * 110.f;
		float mix = rng.range(0.7f, 1.f);
		for (int i = 0; i < K; i++) {
			Spectrum &s = keys[i];
			float dl = fabsf(delta[i]);
			for (int k = 1; k <= H; k++) {
				float re = cosf(-PI / 2) + mix * cosf(-PI / 2 + k * dl);
				float im = sinf(-PI / 2) + mix * sinf(-PI / 2 + k * dl);
				s.amp[k] = hypotf(re, im) / k * lowpass(k, cutoff);
				s.ph[k] = atan2f(im, re);
			}
			// Grit: a touch of saturation, done in the time domain then band-limited again
			if (g > 0.01f)
				saturate(s, g, cutoff * 1.3f);
		}
		break;
	}
	case NEURO: {
		// FM into a wavefolder, then a formant bump that moves across the table
		int ratio = rng.integer(1, 3);
		std::vector<float> index = wander(rng, K, rng.range(0.6f, 1.6f), 1.2f, m);
		std::vector<float> formant = wander(rng, K, rng.range(6.f, 14.f), 8.f + 30.f * b, m);
		float drive = 1.f + 2.5f * g;
		float cutoff = 10.f + powf(b, 1.3f) * 110.f;
		for (int i = 0; i < K; i++) {
			std::vector<float> y(OVER);
			float I = fabsf(index[i]) * (0.6f + 1.8f * g);
			for (int n = 0; n < OVER; n++) {
				float th = 2.f * PI * n / OVER;
				y[n] = fold(sinf(th + I * sinf(ratio * th)) * drive);
			}
			fromSamples(y.data(), OVER, keys[i]);
			float fc = fmaxf(3.f, fabsf(formant[i]));
			for (int k = 1; k <= H; k++)
				keys[i].amp[k] *= (0.35f + 1.6f * gauss(k, fc, 2.f + fc * 0.25f)) * lowpass(k, cutoff);
		}
		break;
	}
	case WOBBLE: {
		// Vowel formants: each keyframe is a vowel, so the table "talks" as it morphs
		int vowel = rng.integer(0, 4);
		float shift = 0.6f + 0.8f * b;
		float odd = rng.range(0.f, 1.f);
		float f1 = VOWELS[vowel][0], f2 = VOWELS[vowel][1];
		for (int i = 0; i < K; i++) {
			if (i > 0) {
				// Move to another vowel; with little movement, only part of the way
				int step = 1 + (int)(m * 3.f * rng.uniform());
				vowel = (vowel + (rng.uniform() < 0.5f ? step : 5 - step)) % 5;
				float amount = 0.25f + 0.75f * m;
				f1 += (VOWELS[vowel][0] - f1) * amount;
				f2 += (VOWELS[vowel][1] - f2) * amount;
			}
			float F1 = f1 * shift, F2 = f2 * shift;
			Spectrum &s = keys[i];
			for (int k = 1; k <= H; k++) {
				float source = (1.f / k) * ((k % 2 == 0) ? (1.f - 0.7f * odd) : 1.f);
				float filter = 0.12f + 1.6f * gauss(k, F1, 1.5f + F1 * 0.2f) + 1.1f * gauss(k, F2, 2.f + F2 * 0.15f);
				s.amp[k] = source * filter * (1.f + g * 0.6f * (k % 2)) * lowpass(k, 20.f + 100.f * b);
				s.ph[k] = -PI / 2;
			}
		}
		break;
	}
	case HOOVER: {
		// Pulse width sweep plus a second, slightly different pulse (the "detuned" layer)
		std::vector<float> pw = wander(rng, K, rng.range(0.42f, 0.5f), 0.25f, m);
		float layer = 0.35f + 0.5f * rng.uniform();
		float offset = rng.range(0.05f, 0.3f);
		float cutoff = 18.f + powf(b, 1.2f) * 109.f;
		for (int i = 0; i < K; i++) {
			float w1 = fminf(0.5f, fmaxf(0.06f, fabsf(pw[i])));
			float w2 = fminf(0.5f, fmaxf(0.06f, w1 * (0.75f + 0.2f * m)));
			Spectrum &s = keys[i];
			for (int k = 1; k <= H; k++) {
				// Pulse of width w: (2/(k pi)) sin(k pi w) cos(k(theta - pi w))
				float a1 = 2.f / (k * PI) * sinf(k * PI * w1), p1 = -k * PI * w1;
				float a2 = layer * 2.f / (k * PI) * sinf(k * PI * w2), p2 = -k * PI * w2 + k * offset * (1.f + m);
				float re = a1 * cosf(p1) + a2 * cosf(p2), im = a1 * sinf(p1) + a2 * sinf(p2);
				s.amp[k] = hypotf(re, im) * lowpass(k, cutoff);
				s.ph[k] = atan2f(im, re);
			}
		}
		break;
	}
	case STAB: {
		// Bright and rich at the start, closing down quickly across the table
		float sq = rng.uniform();
		float cutoff0 = 24.f + powf(b, 1.1f) * 103.f;
		float res = 1.f + 0.8f * g;
		for (int i = 0; i < K; i++) {
			float u = (float)i / (K - 1);
			float cutoff = 3.f + cutoff0 * expf(-u * (1.f + 4.f * m));
			Spectrum &s = keys[i];
			for (int k = 1; k <= H; k++) {
				float saw = 1.f / k, square = (k % 2) ? 1.f / k : 0.f;
				s.amp[k] = ((1.f - sq) * saw + sq * square) * lowpass(k, cutoff) * (1.f + (res - 1.f) * gauss(k, cutoff, 2.f));
				s.ph[k] = -PI / 2 + fixedPhase[k] * 0.15f;
			}
		}
		break;
	}
	case ACID: {
		bool square = rng.uniform() < 0.4f;
		std::vector<float> env = wander(rng, K, rng.range(0.2f, 0.6f), 0.5f, m);
		float q = 1.5f + 6.f * g;
		for (int i = 0; i < K; i++) {
			float e = fminf(1.f, fabsf(env[i]));
			float cutoff = 3.f + (6.f + 100.f * b) * e;
			Spectrum &s = keys[i];
			for (int k = 1; k <= H; k++) {
				float src = square ? ((k % 2) ? 1.f / k : 0.f) : 1.f / k;
				s.amp[k] = src * fminf(resonant(k, cutoff, q), 8.f) * lowpass(k, cutoff * 2.5f, 6.f);
				s.ph[k] = -PI / 2;
			}
		}
		break;
	}
	case WARM: {
		float tilt = rng.range(1.5f, 2.1f);
		std::vector<float> even = wander(rng, K, rng.range(0.4f, 1.f), 0.6f, m);
		float cutoff = 4.f + powf(b, 1.4f) * 40.f;
		float wobble[H + 1];
		for (int k = 0; k <= H; k++)
			wobble[k] = rng.range(0.8f, 1.2f);
		for (int i = 0; i < K; i++) {
			float ev = fminf(1.f, fabsf(even[i]));
			Spectrum &s = keys[i];
			for (int k = 1; k <= H; k++) {
				s.amp[k] = powf((float)k, -tilt) * wobble[k] * ((k % 2 == 0) ? ev : 1.f) * lowpass(k, cutoff) * (1.f + 0.4f * g * gauss(k, 3.f, 1.5f));
				s.ph[k] = fixedPhase[k];
			}
		}
		break;
	}
	case GLASSY: {
		// A few bell-like partials whose levels drift at their own rates
		int partials = rng.integer(5, 9);
		int maxK = 12 + (int)(b * 90.f);
		std::vector<int> ks;
		std::vector<float> level, rate, phase;
		ks.push_back(1);
		for (int j = 0; j < partials; j++)
			ks.push_back(rng.integer(3, maxK));
		for (size_t j = 0; j < ks.size(); j++) {
			level.push_back(j == 0 ? 0.8f : rng.range(0.25f, 1.f) / sqrtf((float)ks[j]));
			rate.push_back(rng.range(0.5f, 2.5f));
			phase.push_back(rng.range(0.f, 2.f * PI));
		}
		for (int i = 0; i < K; i++) {
			float u = (float)i / (K - 1);
			Spectrum &s = keys[i];
			for (size_t j = 0; j < ks.size(); j++) {
				float lv = level[j] * (1.f - m * 0.85f * (0.5f + 0.5f * sinf(2.f * PI * rate[j] * u + phase[j])));
				s.amp[ks[j]] += lv * (1.f + g * 0.5f);
				s.ph[ks[j]] = fixedPhase[ks[j]];
			}
		}
		break;
	}
	case TALKING: {
		// Walks through the vowels in order, so the table says a word as it morphs; three sharper formants than Wobble
		int start = rng.integer(0, 4);
		int dir = rng.uniform() < 0.5f ? 1 : 4; // forwards or backwards round the vowels
		float shift = 0.55f + 0.9f * b;
		float pulse = rng.uniform();
		float F3 = rng.range(2400.f, 2900.f) / 55.f;
		float span = 0.6f + 2.4f * m; // how many vowels the table passes through
		for (int i = 0; i < K; i++) {
			float x = span * i / (K - 1);
			int n = (int)floorf(x);
			float fr = x - n;
			int v0 = (start + dir * n) % 5, v1 = (v0 + dir) % 5;
			float F1 = (VOWELS[v0][0] + (VOWELS[v1][0] - VOWELS[v0][0]) * fr) * shift;
			float F2 = (VOWELS[v0][1] + (VOWELS[v1][1] - VOWELS[v0][1]) * fr) * shift;
			float f3 = F3 * shift;
			Spectrum &s = keys[i];
			for (int k = 1; k <= H; k++) {
				float source = (1.f / k) * ((k % 2 == 0) ? (1.f - 0.8f * pulse) : 1.f);
				float filter = 0.06f + 2.2f * gauss(k, F1, 1.f + F1 * 0.12f) + 1.6f * gauss(k, F2, 1.5f + F2 * 0.1f) +
					0.8f * gauss(k, f3, 3.f + f3 * 0.08f);
				s.amp[k] = source * filter * lowpass(k, 24.f + 100.f * b);
				s.ph[k] = -PI / 2;
			}
			if (g > 0.01f)
				saturate(s, g, 30.f + 97.f * b);
		}
		break;
	}
	case BELL: {
		// Two-operator FM at a whole-number ratio (so it still loops), with the index dying away like a struck bell
		static const int RATIOS[8][2] = {{1, 2}, {1, 3}, {2, 5}, {2, 7}, {3, 7}, {1, 4}, {3, 8}, {1, 5}};
		int r = rng.integer(0, 7);
		float c = (float)RATIOS[r][0], mo = (float)RATIOS[r][1];
		float I0 = 1.f + 5.f * b;
		std::vector<float> wobble = wander(rng, K, 1.f, 0.35f, m);
		for (int i = 0; i < K; i++) {
			float u = (float)i / (K - 1);
			float I = I0 * fmaxf(0.05f, fabsf(wobble[i])) * expf(-u * 2.5f * m);
			float I2 = I * 0.5f * g; // grit: a second, octave-up modulator for a harsher strike
			std::vector<float> y(OVER);
			for (int n = 0; n < OVER; n++) {
				float th = 2.f * PI * n / OVER;
				y[n] = sinf(c * th + I * sinf(mo * th) + I2 * sinf(2.f * mo * th));
			}
			fromSamples(y.data(), OVER, keys[i]);
			for (int k = 1; k <= H; k++)
				keys[i].amp[k] *= lowpass(k, 30.f + 97.f * b);
		}
		break;
	}
	case PLUCK: {
		// A plucked string: bright at the start, the high harmonics dying away first. The pick position notches some out.
		float pick = rng.range(0.08f, 0.3f);
		float cutoff = 16.f + powf(b, 1.2f) * 111.f;
		float tilt = 1.3f - 0.5f * g; // grit: a harder pick
		for (int i = 0; i < K; i++) {
			float u = (float)i / (K - 1);
			Spectrum &s = keys[i];
			for (int k = 1; k <= H; k++) {
				float damp = expf(-u * (0.5f + 6.f * m) * powf(k / 12.f, 1.3f));
				float comb = 0.15f + 0.85f * fabsf(sinf(PI * k * pick));
				s.amp[k] = powf((float)k, -tilt) * comb * damp * lowpass(k, cutoff);
				s.ph[k] = -PI / 2 + fixedPhase[k] * 0.1f;
			}
		}
		break;
	}
	case SYNC: {
		// Hard sync: a second oscillator restarted every cycle, its pitch sweeping up across the table
		int slave = rng.integer(0, 2); // saw, square, triangle
		float r0 = rng.range(1.1f, 2.f);
		std::vector<float> jitter = wander(rng, K, 0.f, 0.5f, m);
		float cutoff = 20.f + powf(b, 1.2f) * 107.f;
		for (int i = 0; i < K; i++) {
			float u = (float)i / (K - 1);
			float ratio = r0 + u * (1.f + 6.f * m) + 0.3f * fabsf(jitter[i]);
			std::vector<float> y(OVER);
			for (int n = 0; n < OVER; n++) {
				float ph = ratio * n / OVER;
				ph -= floorf(ph);
				float v = slave == 0 ? 1.f - 2.f * ph : (slave == 1 ? (ph < 0.5f ? 1.f : -1.f) : 1.f - 4.f * fabsf(ph - 0.5f));
				y[n] = g > 0.01f ? fold(v * (1.f + 2.f * g)) : v;
			}
			fromSamples(y.data(), OVER, keys[i]);
			// Half a cycle later, so the reset edge sits in the middle of the wave rather than on the loop point
			for (int k = 1; k <= H; k++) {
				keys[i].amp[k] *= lowpass(k, cutoff);
				keys[i].ph[k] = remainderf(keys[i].ph[k] + k * PI, 2.f * PI);
			}
		}
		break;
	}
	case FOLD: {
		// A sine or triangle driven harder and harder into a wavefolder (tearout / neuro leads)
		bool triangle = rng.uniform() < 0.4f;
		float bias = rng.range(0.f, 0.3f) * (0.3f + g); // asymmetry adds even harmonics
		std::vector<float> jitter = wander(rng, K, 0.f, 0.6f, m);
		float cutoff = 20.f + powf(b, 1.2f) * 107.f;
		for (int i = 0; i < K; i++) {
			float u = (float)i / (K - 1);
			float drive = 1.f + (1.f + 7.f * m) * powf(u, 1.2f) + 0.4f * fabsf(jitter[i]);
			std::vector<float> y(OVER);
			for (int n = 0; n < OVER; n++) {
				float th = 2.f * PI * n / OVER;
				float in = triangle ? triFold(4.f * n / OVER - 1.f) : sinf(th);
				float x = in * drive + bias;
				y[n] = (1.f - g) * sinf(x * PI * 0.5f) + g * triFold(x);
			}
			fromSamples(y.data(), OVER, keys[i]);
			for (int k = 1; k <= H; k++)
				keys[i].amp[k] *= lowpass(k, cutoff);
		}
		break;
	}
	case CHOIR: {
		// Voices singing vowels: formants (for a note around 110 Hz) on a soft source, each harmonic's level drifting a
		// little like an ensemble
		int vowel = rng.integer(0, 4);
		float reg = 0.8f + 0.9f * b;
		for (int i = 0; i < K; i++) {
			if (i > 0 && rng.uniform() < 0.3f + 0.6f * m)
				vowel = (vowel + rng.integer(1, 4)) % 5;
			float F1 = VOWELS[vowel][0] * 0.5f * reg, F2 = VOWELS[vowel][1] * 0.5f * reg, F3 = 2700.f / 110.f * reg;
			Spectrum &s = keys[i];
			for (int k = 1; k <= H; k++) {
				float filter = 0.05f + 1.4f * gauss(k, F1, 1.f + F1 * 0.2f) + 1.f * gauss(k, F2, 1.2f + F2 * 0.15f) +
					0.5f * gauss(k, F3, 2.f + F3 * 0.1f);
				float ensemble = 1.f + 0.3f * rng.range(-1.f, 1.f) * (0.3f + m);
				float breath = 0.12f * g * lowpass(k, 60.f) * (k > 6);
				s.amp[k] = (powf((float)k, -1.1f) * filter + breath) * ensemble * lowpass(k, 10.f + 80.f * b);
				s.ph[k] = fixedPhase[k];
			}
		}
		break;
	}
	case STRINGS: {
		// An ensemble of slightly detuned saws (more voices and softer than a Reese) with a body resonance
		int voices = rng.integer(3, 5);
		float offset[5], drift[5];
		for (int v = 0; v < voices; v++) {
			offset[v] = rng.range(-1.f, 1.f) * 0.12f;
			drift[v] = rng.range(-1.f, 1.f) * 0.9f * m;
		}
		float body1 = rng.range(3.f, 8.f), body2 = rng.range(10.f, 20.f);
		float cutoff = 8.f + powf(b, 1.2f) * 60.f;
		for (int i = 0; i < K; i++) {
			float u = (float)i / (K - 1);
			float swell = cutoff * (1.f + 1.2f * m * sinf(PI * u)); // opens up in the middle of the table, like a bow swell
			Spectrum &s = keys[i];
			for (int k = 1; k <= H; k++) {
				float re = 0.f, im = 0.f;
				for (int v = 0; v < voices; v++) {
					float ph = -PI / 2 + k * (offset[v] + drift[v] * u);
					re += cosf(ph);
					im += sinf(ph);
				}
				float body = 1.f + 0.5f * gauss(k, body1, 2.f) + 0.3f * gauss(k, body2, 4.f);
				s.amp[k] = hypotf(re, im) / voices / k * lowpass(k, swell) * body;
				s.ph[k] = remainderf(atan2f(im, re) + k * PI, 2.f * PI); // saw edges mid-wave, not on the loop point
			}
			if (g > 0.01f)
				saturate(s, g * 0.6f, cutoff * 1.5f);
		}
		break;
	}
	case EVOLVING:
	default: {
		// Three or four broad spectral bumps that drift slowly across the table
		int bumps = rng.integer(3, 4);
		std::vector<float> centre, width, height, drift;
		for (int j = 0; j < bumps; j++) {
			centre.push_back(rng.range(1.f, 8.f + 40.f * b));
			width.push_back(rng.range(1.5f, 6.f));
			height.push_back(rng.range(0.4f, 1.f));
			drift.push_back(rng.range(-1.f, 1.f) * (4.f + 24.f * b));
		}
		for (int i = 0; i < K; i++) {
			float u = (float)i / (K - 1);
			Spectrum &s = keys[i];
			for (int k = 1; k <= H; k++) {
				float env = 0.08f;
				for (int j = 0; j < bumps; j++)
					env += height[j] * gauss(k, fmaxf(1.f, centre[j] + drift[j] * m * u), width[j]);
				s.amp[k] = env / powf((float)k, 0.8f) * lowpass(k, 6.f + 80.f * b) * (1.f + 0.3f * g * (k % 2));
				s.ph[k] = fixedPhase[k];
			}
		}
		break;
	}
	}

	// Start from the user's waves: the first keyframe is their wave, and each later one is more of the type's sound
	if (p.source.size() >= (size_t)N) {
		std::vector<Spectrum> src;
		for (size_t j = 0; j + N <= p.source.size(); j += N) {
			Spectrum s;
			fromSamples(&p.source[j], N, s);
			float r = spectrumRms(s);
			if (r > 1e-4f) {
				for (int k = 1; k <= H; k++)
					s.amp[k] /= r;
				src.push_back(s);
			}
		}
		if (!src.empty()) {
			int S = (int)src.size();
			float mix = p.sourceMix < 0.f ? 0.f : (p.sourceMix > 1.f ? 1.f : p.sourceMix);
			for (int i = 0; i < K; i++) {
				const Spectrum &a = src[S == 1 ? 0 : (int)lroundf((float)i * (S - 1) / (K - 1))];
				Spectrum &key = keys[i];
				float r = spectrumRms(key);
				float t = mix * i / (K - 1);
				for (int k = 1; k <= H; k++) {
					float kb = r > 1e-9f ? key.amp[k] / r : 0.f;
					float re = (1.f - t) * cosf(a.ph[k]) + t * cosf(key.ph[k]);
					float im = (1.f - t) * sinf(a.ph[k]) + t * sinf(key.ph[k]);
					key.amp[k] = (1.f - t) * a.amp[k] + t * kb;
					key.ph[k] = (fabsf(re) + fabsf(im) > 1e-6f) ? atan2f(im, re) : key.ph[k];
				}
			}
		}
	}

	// Bass: the fundamental is a sine at the same phase on every keyframe (shift the whole wave in time so the shape is
	// kept), and stays the loudest harmonic, so the sub never thins out or cancels as the table morphs
	if (isBass(p.type)) {
		for (Spectrum &s : keys) {
			if (s.amp[1] < 1e-6f)
				s.amp[1] = 1e-3f;
			float shift = BASS_PHASE - s.ph[1];
			for (int k = 1; k <= H; k++)
				s.ph[k] = remainderf(s.ph[k] + k * shift, 2.f * PI);
			s.ph[1] = BASS_PHASE;
			float maxOther = 0.f;
			for (int k = 2; k <= H; k++)
				maxOther = fmaxf(maxOther, s.amp[k]);
			if (s.amp[1] < BASS_DOMINANCE * maxOther) {
				float scale = s.amp[1] / (BASS_DOMINANCE * maxOther);
				for (int k = 2; k <= H; k++)
					s.amp[k] *= scale;
			}
		}
	}
	return keys;
}

float peakOf(const float *w) {
	float peak = 0.f;
	for (int i = 0; i < N; i++)
		peak = fmaxf(peak, fabsf(w[i]));
	return peak;
}

int keyframeCount(const Params &p, int W) {
	int K = p.keyframes < 2 ? 2 : (p.keyframes > 8 ? 8 : p.keyframes);
	return K > W ? W : K;
}

int waveCount(const Params &p) {
	return p.waves < 2 ? 2 : (p.waves > 4096 ? 4096 : p.waves);
}

/** Waves the keyframes sit on during the outward half of a ping-pong */
std::vector<int> pingPongKeys(int W, int K) {
	std::vector<int> at(K);
	for (int j = 0; j < K; j++)
		at[j] = (int)lroundf((float)j * (W / 2) / (K - 1));
	return at;
}

/** Where each wave sits between the keyframes, 0..K-1 (for every motion except the plain sweep) */
std::vector<float> motionPath(const Params &p, int W, int K) {
	std::vector<float> pos(W);
	switch (p.motion) {
	case PING_PONG: {
		// Out over the first half and back over the second, so the last wave sits next to the first. Every keyframe
		// lands on a whole wave, on the way out and again on the way back.
		std::vector<int> at = pingPongKeys(W, K);
		for (int w = 0; w < W; w++) {
			int x = w <= W / 2 ? w : W - w;
			int j = 0;
			while (j + 2 < K && x >= at[j + 1])
				j++;
			int span = at[j + 1] - at[j];
			pos[w] = j + (span > 0 ? (float)(x - at[j]) / span : 0.f);
		}
		break;
	}
	case STEPPED:
		for (int w = 0; w < W; w++)
			pos[w] = (float)((w * K / W) < K - 1 ? (w * K / W) : K - 1);
		break;
	case RANDOM_WALK: {
		// Its own random numbers, so mutating the sound doesn't change the path
		Rng walk((((uint64_t)p.seed << 8) ^ (uint64_t)p.type) + 0xABCDEFull);
		float x = 0.f, v = 0.f, lo = 0.f, hi = 0.f;
		for (int w = 0; w < W; w++) {
			pos[w] = x;
			lo = fminf(lo, x);
			hi = fmaxf(hi, x);
			v = 0.8f * v + walk.range(-1.f, 1.f) * (0.4f + 0.8f * p.movement);
			x += v;
		}
		// Stretch it to visit the first and last keyframes
		for (int w = 0; w < W; w++)
			pos[w] = hi - lo > 1e-6f ? (pos[w] - lo) / (hi - lo) * (K - 1) : (float)w * (K - 1) / (W - 1);
		break;
	}
	default:
		for (int w = 0; w < W; w++)
			pos[w] = (float)w * (K - 1) / (W - 1);
		break;
	}
	return pos;
}

/** Old-sampler character on one wave: a lower rate (sample and hold) and fewer bits */
void applyVintage(float *wave, Vintage v) {
	int hold = 1, bits = 16;
	bool filter = false;
	switch (v) {
	case S950: hold = 2; bits = 12; filter = true; break;
	// Hold lengths divide 256, so every step is the same length
	case SP1200: hold = 4; bits = 12; break;
	case CRUSH: hold = 8; bits = 8; break;
	default: return;
	}
	float x[N], y[N];
	for (int i = 0; i < N; i++)
		x[i] = filter ? 0.25f * wave[(i + N - 1) % N] + 0.5f * wave[i] + 0.25f * wave[(i + 1) % N] : wave[i];
	float mean = 0.f;
	// The steps are centred on the loop point: the last and first samples share one flat step, so the loop never steps
	for (int i = 0; i < N; i++) {
		int j = ((i + hold / 2) / hold) * hold;
		y[i] = x[j % N];
		mean += y[i];
	}
	mean /= N;
	float peak = 1e-9f;
	for (int i = 0; i < N; i++) {
		y[i] -= mean;
		peak = fmaxf(peak, fabsf(y[i]));
	}
	float levels = (float)(1 << (bits - 1));
	// Fewer bits, then take out the small offset the rounding leaves (the steps stay evenly spaced)
	mean = 0.f;
	for (int i = 0; i < N; i++) {
		wave[i] = roundf(y[i] / peak * levels) / levels;
		mean += wave[i];
	}
	mean /= N;
	for (int i = 0; i < N; i++)
		wave[i] -= mean;
}

/** Nudges each random draw a little (reflecting at the ends, so a value near 1 doesn't jump to 0) */
void mutateGenes(std::vector<uint32_t> &genes, float amount, Rng &r) {
	for (uint32_t &g : genes) {
		if (r.uniform() >= 0.25f + 0.75f * amount)
			continue;
		// Small amounts mostly fine-tune; big ones can also flip choices like an FM ratio or a wave shape
		double u = g / 4294967296.0 + r.gaussian() * (0.05 * amount + 0.2 * amount * amount);
		if (u < 0.0)
			u = -u;
		if (u > 1.0)
			u = 2.0 - u;
		u = u < 0.0 ? 0.0 : (u > 0.9999999 ? 0.9999999 : u);
		g = (uint32_t)(u * 4294967296.0);
	}
}

float clamp01(float x) {
	return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
}

std::vector<uint32_t> genesOf(const Params &p) {
	if (!p.genes.empty())
		return p.genes;
	std::vector<uint32_t> genes;
	generate(p, &genes);
	return genes;
}

} // namespace

const char *typeName(Type t) {
	static const char *names[TYPE_COUNT] = {"Sub", "Reese", "Neuro", "Wobble", "Hoover", "Stab", "Acid", "Warm", "Glassy", "Evolving",
		"Talking", "Bell", "Pluck", "Sync", "Fold", "Choir", "Strings"};
	return (t >= 0 && t < TYPE_COUNT) ? names[t] : "Table";
}

const char *typeBlurb(Type t) {
	switch (t) {
	case SUB: return "clean sub: a sine with a little 2nd and 3rd harmonic";
	case REESE: return "detuned-saw style beating that phases across the table";
	case NEURO: return "FM into a wavefolder with a moving formant";
	case WOBBLE: return "vowel-like formant sweeps that talk as the table morphs";
	case HOOVER: return "pulse-width and detune sweep";
	case STAB: return "bright and rich, closing down quickly";
	case ACID: return "saw or square with a moving resonant peak";
	case WARM: return "soft, mostly low harmonics";
	case GLASSY: return "sparse, bell-like high partials";
	case EVOLVING: return "slow spectral drift";
	case TALKING: return "talking bass: walks through the vowels like it's saying a word";
	case BELL: return "FM bell: metallic partials that ring and die away";
	case PLUCK: return "plucked string: bright attack, highs dying away first";
	case SYNC: return "hard sync: a tearing sweep as the synced pitch rises";
	case FOLD: return "wavefolder driven harder and harder: tearout and neuro leads";
	case CHOIR: return "voices singing vowels, with a slow ensemble drift";
	case STRINGS: return "detuned saw ensemble with a body resonance";
	default: return "";
	}
}

Group typeGroup(Type t) {
	if (t <= WOBBLE || t == TALKING)
		return BASS;
	if (t <= ACID || (t >= BELL && t <= FOLD))
		return LEAD;
	return PAD;
}

std::vector<Type> typesIn(Group g) {
	std::vector<Type> types;
	static const Type order[TYPE_COUNT] = {SUB, REESE, NEURO, WOBBLE, TALKING, HOOVER, STAB, ACID, BELL, PLUCK, SYNC, FOLD,
		WARM, GLASSY, EVOLVING, CHOIR, STRINGS};
	for (int i = 0; i < TYPE_COUNT; i++)
		if (typeGroup(order[i]) == g)
			types.push_back(order[i]);
	return types;
}

const char *motionName(Motion m) {
	static const char *names[MOTION_COUNT] = {"Sweep", "Ping-pong", "Stepped", "Random walk"};
	return (m >= 0 && m < MOTION_COUNT) ? names[m] : "";
}

const char *vintageName(Vintage v) {
	static const char *names[VINTAGE_COUNT] = {"Off", "S950", "SP1200", "Crush"};
	return (v >= 0 && v < VINTAGE_COUNT) ? names[v] : "";
}

bool sameParams(const Params &a, const Params &b) {
	return a.type == b.type && a.brightness == b.brightness && a.movement == b.movement && a.grit == b.grit &&
		a.keyframes == b.keyframes && a.morph == b.morph && a.seed == b.seed && a.waves == b.waves && a.motion == b.motion &&
		a.vintage == b.vintage && a.source == b.source && a.sourceMix == b.sourceMix && a.genes == b.genes &&
		a.generation == b.generation;
}

const char *groupName(Group g) {
	return g == BASS ? "Bass" : (g == LEAD ? "Lead" : "Pad");
}

bool isBass(Type t) {
	return typeGroup(t) == BASS;
}

Params defaults(Type t) {
	Params p;
	p.type = t;
	p.morph = isBass(t) ? wt::SPECTRAL : wt::CROSSFADE;
	switch (t) {
	case SUB: p.brightness = 0.3f; p.movement = 0.3f; p.grit = 0.1f; p.keyframes = 3; break;
	case REESE: p.brightness = 0.6f; p.movement = 0.6f; p.grit = 0.2f; p.keyframes = 5; break;
	case NEURO: p.brightness = 0.6f; p.movement = 0.7f; p.grit = 0.6f; p.keyframes = 6; break;
	case WOBBLE: p.brightness = 0.5f; p.movement = 0.7f; p.grit = 0.3f; p.keyframes = 5; break;
	case HOOVER: p.brightness = 0.7f; p.movement = 0.6f; p.grit = 0.2f; p.keyframes = 4; break;
	case STAB: p.brightness = 0.8f; p.movement = 0.5f; p.grit = 0.3f; p.keyframes = 4; break;
	case ACID: p.brightness = 0.6f; p.movement = 0.7f; p.grit = 0.5f; p.keyframes = 6; break;
	case WARM: p.brightness = 0.35f; p.movement = 0.4f; p.grit = 0.1f; p.keyframes = 4; break;
	case GLASSY: p.brightness = 0.6f; p.movement = 0.5f; p.grit = 0.1f; p.keyframes = 5; break;
	case EVOLVING: p.brightness = 0.5f; p.movement = 0.8f; p.grit = 0.2f; p.keyframes = 6; break;
	case TALKING: p.brightness = 0.5f; p.movement = 0.7f; p.grit = 0.3f; p.keyframes = 6; break;
	case BELL: p.brightness = 0.6f; p.movement = 0.6f; p.grit = 0.2f; p.keyframes = 4; break;
	case PLUCK: p.brightness = 0.8f; p.movement = 0.6f; p.grit = 0.3f; p.keyframes = 4; break;
	case SYNC: p.brightness = 0.7f; p.movement = 0.6f; p.grit = 0.2f; p.keyframes = 5; break;
	case FOLD: p.brightness = 0.6f; p.movement = 0.7f; p.grit = 0.4f; p.keyframes = 5; break;
	case CHOIR: p.brightness = 0.5f; p.movement = 0.5f; p.grit = 0.2f; p.keyframes = 5; break;
	case STRINGS: p.brightness = 0.5f; p.movement = 0.5f; p.grit = 0.2f; p.keyframes = 4; break;
	default: break;
	}
	return p;
}

std::vector<float> generate(const Params &p, std::vector<uint32_t> *genesOut) {
	int W = waveCount(p);
	if (genesOut)
		genesOut->clear();
	// Only the seed and type pick the random choices, so the sliders adjust the same table rather than reshuffling it
	Rng rng(((uint64_t)p.seed << 8) ^ (uint64_t)p.type, p.genes.empty() ? NULL : &p.genes, genesOut);
	std::vector<Spectrum> spectra = keyframeSpectra(p, rng);
	int K = keyframeCount(p, W);

	std::vector<float> table((size_t)W * N, 0.f);
	if (p.motion == SWEEP) {
		std::vector<int> positions = keyframePositions(p);
		for (int i = 0; i < K; i++)
			toWave(spectra[i], &table[(size_t)positions[i] * N]);
		wt::morphKeyframes(table.data(), W, positions, p.morph, wt::LINEAR);

		// A crossfade can thin out between keyframes whose harmonics are out of phase. Where that happens, redo the
		// segment as a spectral morph, which keeps the level.
		if (p.morph == wt::CROSSFADE) {
			for (const wt::Segment &seg : wt::segmentsFor(positions)) {
				float floor = 0.35f * fminf(wt::rms(&table[(size_t)seg.from * N]), wt::rms(&table[(size_t)seg.to * N]));
				bool thin = false;
				for (int w = seg.from + 1; w < seg.to && !thin; w++)
					thin = wt::rms(&table[(size_t)w * N]) < floor;
				if (thin) {
					std::vector<int> pair;
					pair.push_back(seg.from);
					pair.push_back(seg.to);
					wt::morphKeyframes(table.data(), W, pair, wt::SPECTRAL, wt::LINEAR);
				}
			}
		}
	}
	else {
		std::vector<float> path = motionPath(p, W, K);
		for (int w = 0; w < W; w++) {
			float x = path[w] < 0.f ? 0.f : (path[w] > K - 1 ? (float)(K - 1) : path[w]);
			int i0 = (int)floorf(x);
			if (i0 > K - 2)
				i0 = K - 2;
			float t = x - i0;
			Spectrum s = t < 1e-6f ? spectra[i0] : (t > 1.f - 1e-6f ? spectra[i0 + 1] : blend(spectra[i0], spectra[i0 + 1], t, p.morph));
			toWave(s, &table[(size_t)w * N]);
		}
	}

	// Old-sampler character, then the same peak on every wave
	for (int w = 0; w < W; w++) {
		float *wave = &table[(size_t)w * N];
		applyVintage(wave, p.vintage);
		float peak = peakOf(wave);
		float scale = peak > 1e-9f ? PEAK / peak : 0.f;
		for (int i = 0; i < N; i++)
			wave[i] *= scale;
	}
	return table;
}

std::vector<int> keyframePositions(const Params &p) {
	int W = waveCount(p), K = keyframeCount(p, W);
	std::vector<int> positions;
	if (p.motion == SWEEP) {
		for (int i = 0; i < K; i++)
			positions.push_back((K == 1) ? 0 : (int)lroundf((float)i * (W - 1) / (K - 1)));
	}
	else if (p.motion == PING_PONG) {
		std::vector<int> at = pingPongKeys(W, K);
		positions = at;
		for (int j = K - 2; j >= 1; j--) // and again on the way back
			if (W - at[j] < W && W - at[j] > at[K - 1])
				positions.push_back(W - at[j]);
	}
	return positions;
}

Params mutate(const Params &parent, float amount, uint32_t seed) {
	Params c = parent;
	amount = clamp01(amount);
	c.genes = genesOf(parent);
	Rng r((uint64_t)seed * 2654435761ull + 99u);
	mutateGenes(c.genes, amount, r);
	c.brightness = clamp01(c.brightness + r.range(-1.f, 1.f) * 0.12f * amount);
	c.movement = clamp01(c.movement + r.range(-1.f, 1.f) * 0.12f * amount);
	c.grit = clamp01(c.grit + r.range(-1.f, 1.f) * 0.12f * amount);
	c.generation = parent.generation + 1;
	return c;
}

Params breed(const Params &a, const Params &b, float amount, uint32_t seed) {
	Params c = a;
	std::vector<uint32_t> ga = genesOf(a), gb = genesOf(b);
	if (b.type != a.type)
		gb = ga; // different recipes draw different things: only the sliders cross over
	Rng r((uint64_t)seed * 2246822519ull + 7u);
	size_t n = ga.size() > gb.size() ? ga.size() : gb.size();
	c.genes.resize(n);
	for (size_t i = 0; i < n; i++) {
		bool fromA = r.uniform() < 0.5f;
		if (i >= gb.size())
			fromA = true;
		if (i >= ga.size())
			fromA = false;
		c.genes[i] = fromA ? ga[i] : gb[i];
	}
	mutateGenes(c.genes, clamp01(amount) * 0.3f, r);
	float t = r.uniform();
	c.brightness = a.brightness + (b.brightness - a.brightness) * t;
	t = r.uniform();
	c.movement = a.movement + (b.movement - a.movement) * t;
	t = r.uniform();
	c.grit = a.grit + (b.grit - a.grit) * t;
	c.generation = (a.generation > b.generation ? a.generation : b.generation) + 1;
	return c;
}

std::string tableName(const Params &p) {
	char buf[64];
	snprintf(buf, sizeof(buf), "%s_%04u", typeName(p.type), (unsigned)(p.seed % 10000));
	std::string name = buf;
	if (p.generation > 0)
		name += "_g" + std::to_string(p.generation);
	if (p.vintage != VINTAGE_OFF)
		name += std::string("_") + vintageName(p.vintage);
	return name;
}

uint32_t nextSeed(uint32_t previous) {
	Rng rng(previous + 1);
	return rng.next() % 10000;
}

void analyse(const float *wave, float *amp, float *phase) {
	static Fft fft(N);
	float X[N];
	pffft_transform_ordered(fft.setup, wave, X, NULL, PFFFT_FORWARD);
	amp[0] = fabsf(X[0]) / N;
	phase[0] = 0.f;
	amp[N / 2] = fabsf(X[1]) / N;
	phase[N / 2] = 0.f;
	for (int k = 1; k < N / 2; k++) {
		amp[k] = 2.f * hypotf(X[2 * k], X[2 * k + 1]) / N;
		phase[k] = atan2f(X[2 * k + 1], X[2 * k]);
	}
}

} // namespace gen
