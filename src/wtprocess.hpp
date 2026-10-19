#pragma once

/** Wavetable processing shared by the editor, the SD Card page and batch processing:
Cycle and Normalize (the exact maths the editor uses), blank-wave detection, and filling blank runs
by morphing between their neighbours. No UI, no file I/O. Tests: tests/process_test.cpp. */

#include <string>
#include <vector>

namespace wt {

static const int WAVE = 256; // samples per wave

/** A wave is blank if its peak is below this level (-60 dBFS) */
static const float BLANK_DB = -60.f;
float blankThreshold();
bool isBlank(const float *wave);

/** The editor's Cycle: tilts the wave so its end lines up with its start. In-place, one wave. */
void cycleWave(float *wave);
/** The editor's Normalize: rescales the wave to span -1..1 (a flat wave becomes silence). In-place, one wave. */
void normalizeWave(float *wave);
/** Clamps to -1..1, as the editor does at the end of its effects */
void clipWave(float *wave);

/** What opening a file and clicking Cycle All / Normalize All gives: cycle, then normalize, then clip.
`samples` holds `waveCount` waves. */
void processTable(float *samples, int waveCount, bool cycle, bool normalize);

/** A run of blank waves, indices inclusive. `before`/`after` are the filled neighbours, -1 at the table's edge. */
struct Run {
	int first, last;
	int before, after;
	int length() const { return last - first + 1; }
};

struct BlankReport {
	int waveCount;    // waves checked
	int paddedFrom;   // waves from here on are padding (the file was shorter than the table); -1 if none
	int blankCount;   // blank waves among those checked
	bool allBlank;    // nothing to morph from
	std::vector<int> blank; // indices
	std::vector<Run> runs;
	BlankReport() : waveCount(0), paddedFrom(-1), blankCount(0), allBlank(false) {}
	bool any() const { return blankCount > 0 && !allBlank; }
	bool hasEdgeRuns() const;
	/** "waves 9-14 (between 8 and 15)", 1-based for display */
	static std::string describe(const Run &run);
};

/** Finds blank waves. `sourceWaves` is how many waves the file really had: anything from there on is padding and is
ignored (pass waveCount if there's no padding). */
BlankReport findBlanks(const float *samples, int waveCount, int sourceWaves);

enum MorphMode {
	CROSSFADE, // blend sample by sample
	SPECTRAL,  // blend harmonic amplitudes and phases
};
enum EdgeMode {
	EDGE_HOLD,  // copy the nearest filled wave
	EDGE_WRAP,  // morph towards the wave at the other end of the table
	EDGE_LEAVE, // leave blank
};

/** One wave between `a` (t = 0) and `b` (t = 1) */
void morph(const float *a, const float *b, float t, MorphMode mode, float *out);

/** Fills the blank runs found by `report`. Returns how many waves were filled. */
int fillBlanks(float *samples, int waveCount, const BlankReport &report, MorphMode mode, EdgeMode edges);

////////////////////
// Keyframe morph
////////////////////

/** How the morph moves between two keyframes */
enum Curve {
	LINEAR,
	EASE_IN,  // starts slowly
	EASE_OUT, // finishes slowly
	S_CURVE,  // slow at both ends
};
float curve(Curve c, float t);

/** The keyframes to morph between. With a selection of two or more waves (selStart < selEnd), the first and last
selected waves plus any marked keyframes between them. Otherwise every marked keyframe. Sorted, no duplicates. */
std::vector<int> keyframesFor(int waveCount, const bool *marked, int selStart, int selEnd);

/** A stretch of waves between two neighbouring keyframes */
struct Segment {
	int from, to; // keyframes; the waves strictly between are replaced
	int count() const { return to - from - 1; }
};
/** Segments between neighbouring keyframes that have waves in between */
std::vector<Segment> segmentsFor(const std::vector<int> &keyframes);

/** Replaces the waves between neighbouring keyframes with morphs from one keyframe to the next. Keyframes are not
changed. Returns how many waves were written. */
int morphKeyframes(float *samples, int waveCount, const std::vector<int> &keyframes, MorphMode mode, Curve c);

/** Root-mean-square level of one wave */
float rms(const float *wave);

} // namespace wt
