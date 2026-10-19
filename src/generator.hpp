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
	// Added later (kept after the first ten so their seeds still give the same tables)
	TALKING,  // bass
	BELL,     // lead
	PLUCK,    // lead
	SYNC,     // lead
	FOLD,     // lead
	CHOIR,    // pad
	STRINGS,  // pad
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

/** The types in each group, in the order the dialog shows them */
std::vector<Type> typesIn(Group g);

/** How the table moves through its keyframes */
enum Motion {
	SWEEP,       // from the first keyframe to the last
	PING_PONG,   // out and back, so wave 32 leads straight back into wave 1 (loops cleanly with an LFO on Morph Z)
	STEPPED,     // holds each keyframe for a block of waves: rhythmic jumps when Morph Z is sequenced
	RANDOM_WALK, // wanders back and forth between the keyframes
	MOTION_COUNT
};
const char *motionName(Motion m);

/** Optional old-sampler character. Anything but OFF lets aliasing through on purpose, so those tables aren't band-limited. */
enum Vintage {
	VINTAGE_OFF,
	S950,    // 12-bit with a soft filter
	SP1200,  // 12-bit, lower rate, unfiltered aliasing
	CRUSH,   // 8-bit, very low rate
	VINTAGE_COUNT
};
const char *vintageName(Vintage v); // "S950"

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
	Motion motion;
	Vintage vintage;
	/** Optional waves to start from (count x 256 samples): the table starts on the first and turns into the type's
	sound by the end, `sourceMix` of the way */
	std::vector<float> source;
	float sourceMix;
	/** Mutate & breed: the random draws the recipe made, changed a little. Empty means "straight from the seed". */
	std::vector<uint32_t> genes;
	int generation;   // 0 for a fresh table, +1 for each mutation or breeding
	Params() : type(REESE), brightness(0.5f), movement(0.5f), grit(0.3f), keyframes(4), morph(wt::SPECTRAL), seed(427), waves(32),
		motion(SWEEP), vintage(VINTAGE_OFF), sourceMix(0.7f), generation(0) {}
};

bool sameParams(const Params &a, const Params &b);

/** Default settings for a type (bass types use the spectral morph) */
Params defaults(Type t);

/** Generates the table: params.waves x 256 samples. The same params always give the same table. `genesOut` receives the
random draws the recipe made, for mutating it. */
std::vector<float> generate(const Params &params, std::vector<uint32_t> *genesOut = NULL);

/** Waves that are exact keyframes (for marking them in the editor so the table can be re-morphed). Empty for the stepped
and random-walk motions, which aren't straight morphs between keyframes. */
std::vector<int> keyframePositions(const Params &params);

/** A variation of `parent`: its random draws and sliders nudged by `amount` (0..1). Deterministic for the same `seed`. */
Params mutate(const Params &parent, float amount, uint32_t seed);
/** A child of two tables of the same type: each random draw comes from one parent or the other, sliders in between */
Params breed(const Params &a, const Params &b, float amount, uint32_t seed);

/** "Reese_0427", "Reese_0427_g2" after mutating, "Reese_0427_SP1200" with vintage character */
std::string tableName(const Params &params);

/** A new random seed (0..9999) from a counter, the clock or anything else */
uint32_t nextSeed(uint32_t previous);

/** Harmonic amplitude and phase (cosine phase, radians) of one wave, k = 0..128 */
void analyse(const float *wave, float *amp, float *phase);

} // namespace gen
