#pragma once

/** Batch processing of a folder of wavetables (File > Batch Process Folder). No UI here; the dialog is in batchpage.cpp
and the CI tool calls this directly. */

#include "wtprocess.hpp"

#include <string>
#include <vector>

namespace batch {

struct Options {
	bool cycle;
	bool normalize;
	bool fill;
	wt::MorphMode morph;
	wt::EdgeMode edges;
	Options() : cycle(true), normalize(false), fill(false), morph(wt::CROSSFADE), edges(wt::EDGE_HOLD) {}
};

struct FileResult {
	std::string name;
	bool ok;
	std::string reason; // why it was skipped
	int waves;
	int blankFound;
	int blankFilled;
	FileResult() : ok(false), waves(0), blankFound(0), blankFilled(0) {}
};

/** Default output folder: "<source>_cycled" next to the source */
std::string defaultOutputDir(const std::string &sourceDir);

/** True if both paths are the same folder */
bool sameDir(const std::string &a, const std::string &b);

/** Reads `src`, processes every 256-sample wave in it, and writes `dst` in the same format (wave count, sample rate,
bit depth, channels). Writes to a temporary file first, so `dst` may be `src`. Samples after the last whole wave are
kept as they are. Files with a 'clm ' chunk declaring a frame size other than 256 (e.g. 2048 for Serum) are skipped. */
FileResult processFile(const std::string &src, const std::string &dst, const Options &options);

/** Output path for one file */
std::string outputPath(const std::string &srcFile, const std::string &outDir);

/** Files in `outDir` that would be replaced */
int countExisting(const std::vector<std::string> &files, const std::string &outDir);

bool makeDirs(const std::string &dir);

} // namespace batch
