#pragma once

/** Random wavetable generator for drum & bass / jungle sounds. Each sound type is a recipe for a few keyframe waves,
built from harmonics 1-127 (so every wave is band-limited, has no DC and loops seamlessly), which are then morphed
across the table and normalised to the same peak. No UI here; the dialog is in gendialog.cpp. */

#include "wtprocess.hpp"

#include <stdint.h>
#include <string>
#include <vector>

namespace gen {

enum Type {
	// Bass
	SUB,
	REESE,
	NEURO,
	WOBBLE,
	// Lead
	HOOVER,
	STAB,
	ACID,
	// Pad
	WARM,
	GLASSY,
	EVOLVING,
	TYPE_COUNT
};

enum Group {
	BASS,
	LEAD,
	PAD,
};

const char *typeName(Type t);     // "Reese"
const char *typeBlurb(Type t);    // one line describing it
Group typeGroup(Type t);
const char *groupName(Group g);   // "Bass"
bool isBass(Type t);

static const int MAX_HARMONIC = 127;
static const float PEAK = 0.95f;  // every wave is normalised to this peak

struct Params {
	Type type;
	float brightness; // 0..1
	float movement;   // 0..1, how much the table changes from start to end
	float grit;       // 0..1, folding / FM / saturation
	int keyframes;    // 2..8
	wt::MorphMode morph;
	uint32_t seed;    // 0..9999
	int waves;        // waves in the table (16, 32 or 64)
	Params() : type(REESE), brightness(0.5f), movement(0.5f), grit(0.3f), keyframes(4), morph(wt::SPECTRAL), seed(427), waves(32) {}
};

/** Default settings for a type (bass types use the spectral morph) */
Params defaults(Type t);

/** Generates the table: params.waves x 256 samples. The same params always give the same table. */
std::vector<float> generate(const Params &params);

/** "Reese_0427" */
std::string tableName(const Params &params);

/** A new random seed (0..9999) from a counter, the clock or anything else */
uint32_t nextSeed(uint32_t previous);

/** Harmonic amplitude and phase (cosine phase, radians) of one wave, k = 0..128 */
void analyse(const float *wave, float *amp, float *phase);

} // namespace gen
