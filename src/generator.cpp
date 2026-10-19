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

/** Small deterministic random generator (same numbers on every platform) */
struct Rng {
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {
		for (int i = 0; i < 4; i++)
			next();
	}
	uint32_t next() {
		// SplitMix64
		uint64_t z = (s += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return (uint32_t)((z ^ (z >> 31)) >> 32);
	}
	float uniform() { return (next() >> 8) * (1.f / 16777216.f); } // 0..1
	float range(float a, float b) { return a + (b - a) * uniform(); }
	int integer(int a, int b) { return a + (int)(next() % (uint32_t)(b - a + 1)); }
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
			if (g > 0.01f) {
				float w[N];
				toWave(s, w);
				float peak = 1e-9f;
				for (int n = 0; n < N; n++)
					peak = fmaxf(peak, fabsf(w[n]));
				// Oversample (exact: from the harmonics), saturate, then band-limit again
				std::vector<float> y(OVER);
				toSamples(s, y.data(), OVER);
				for (int n = 0; n < OVER; n++)
					y[n] = fold(y[n] / peak * (1.f + 1.5f * g)) * peak;
				Spectrum t;
				fromSamples(y.data(), OVER, t);
				for (int k = 1; k <= H; k++) {
					t.amp[k] *= lowpass(k, cutoff * 1.3f);
				}
				s = t;
			}
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

} // namespace

const char *typeName(Type t) {
	static const char *names[TYPE_COUNT] = {"Sub", "Reese", "Neuro", "Wobble", "Hoover", "Stab", "Acid", "Warm", "Glassy", "Evolving"};
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
	default: return "";
	}
}

Group typeGroup(Type t) {
	if (t <= WOBBLE)
		return BASS;
	if (t <= ACID)
		return LEAD;
	return PAD;
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
	default: break;
	}
	return p;
}

std::vector<float> generate(const Params &p) {
	int W = p.waves < 2 ? 2 : (p.waves > 4096 ? 4096 : p.waves);
	// Only the seed and type pick the random choices, so the sliders adjust the same table rather than reshuffling it
	Rng rng(((uint64_t)p.seed << 8) ^ (uint64_t)p.type);
	std::vector<Spectrum> spectra = keyframeSpectra(p, rng);
	int K = (int)spectra.size();
	if (K > W)
		K = W;

	std::vector<float> table((size_t)W * N, 0.f);
	std::vector<int> positions;
	for (int i = 0; i < K; i++) {
		int pos = (K == 1) ? 0 : (int)lroundf((float)i * (W - 1) / (K - 1));
		positions.push_back(pos);
		toWave(spectra[i], &table[(size_t)pos * N]);
	}
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

	// Same peak on every wave
	for (int w = 0; w < W; w++) {
		float *wave = &table[(size_t)w * N];
		float peak = peakOf(wave);
		float scale = peak > 1e-9f ? PEAK / peak : 0.f;
		for (int i = 0; i < N; i++)
			wave[i] *= scale;
	}
	return table;
}

std::string tableName(const Params &p) {
	char buf[64];
	snprintf(buf, sizeof(buf), "%s_%04u", typeName(p.type), (unsigned)(p.seed % 10000));
	return buf;
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
