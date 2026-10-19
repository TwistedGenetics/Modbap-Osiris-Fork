#include "batch.hpp"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sndfile.h>

namespace batch {

// Keeps memory bounded for very long audio files (about 4 minutes of stereo at 44.1 kHz)
static const sf_count_t MAX_SAMPLES = 24 * 1024 * 1024;

static std::string stripSlash(std::string p) {
	while (p.size() > 1 && p.back() == '/')
		p.pop_back();
	return p;
}

static std::string baseName(const std::string &path) {
	std::string p = stripSlash(path);
	size_t s = p.find_last_of('/');
	return s == std::string::npos ? p : p.substr(s + 1);
}

std::string defaultOutputDir(const std::string &sourceDir) {
	return stripSlash(sourceDir) + "_cycled";
}

bool sameDir(const std::string &a, const std::string &b) {
	char ra[PATH_MAX], rb[PATH_MAX];
	if (realpath(a.c_str(), ra) && realpath(b.c_str(), rb))
		return strcmp(ra, rb) == 0;
	return stripSlash(a) == stripSlash(b);
}

std::string outputPath(const std::string &srcFile, const std::string &outDir) {
	return stripSlash(outDir) + "/" + baseName(srcFile);
}

int countExisting(const std::vector<std::string> &files, const std::string &outDir) {
	int n = 0;
	struct stat st;
	for (const std::string &f : files)
		if (stat(outputPath(f, outDir).c_str(), &st) == 0)
			n++;
	return n;
}

bool makeDirs(const std::string &dirIn) {
	std::string dir = stripSlash(dirIn);
	struct stat st;
	if (stat(dir.c_str(), &st) == 0)
		return S_ISDIR(st.st_mode);
	size_t s = dir.find_last_of('/');
	if (s != std::string::npos && s > 0 && !makeDirs(dir.substr(0, s)))
		return false;
	return mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

/** Frame size from a 'clm ' chunk ("<!>2048 ..."), 0 if there isn't one */
static int clmFrameSize(SNDFILE *sf, std::vector<char> &chunkData) {
	SF_CHUNK_INFO info;
	memset(&info, 0, sizeof(info));
	snprintf(info.id, sizeof(info.id), "clm ");
	info.id_size = 4;
	SF_CHUNK_ITERATOR *it = sf_get_chunk_iterator(sf, &info);
	if (!it)
		return 0;
	SF_CHUNK_INFO data;
	memset(&data, 0, sizeof(data));
	if (sf_get_chunk_size(it, &data) != SF_ERR_NO_ERROR || data.datalen == 0 || data.datalen > 4096)
		return 0;
	chunkData.assign(data.datalen + 1, 0);
	data.data = chunkData.data();
	if (sf_get_chunk_data(it, &data) != SF_ERR_NO_ERROR)
		return 0;
	chunkData.resize(data.datalen);
	std::string text(chunkData.begin(), chunkData.end());
	size_t p = text.find("<!>");
	if (p == std::string::npos)
		return 0;
	return atoi(text.c_str() + p + 3);
}

FileResult processFile(const std::string &src, const std::string &dst, const Options &options) {
	FileResult r;
	r.name = baseName(src);

	SF_INFO info;
	memset(&info, 0, sizeof(info));
	SNDFILE *in = sf_open(src.c_str(), SFM_READ, &info);
	if (!in) {
		r.reason = std::string("can't be read: ") + sf_strerror(NULL);
		return r;
	}
	std::vector<char> clm;
	int frameSize = clmFrameSize(in, clm);
	if (frameSize > 0 && frameSize != wt::WAVE) {
		sf_close(in);
		r.reason = "uses " + std::to_string(frameSize) + "-sample waves (Serum-style table), not 256";
		return r;
	}
	if (info.frames < wt::WAVE) {
		sf_close(in);
		r.reason = "is shorter than one wave (256 samples)";
		return r;
	}
	if (info.frames * info.channels > MAX_SAMPLES) {
		sf_close(in);
		r.reason = "is too long to be a wavetable";
		return r;
	}
	std::vector<float> interleaved((size_t)(info.frames * info.channels));
	sf_count_t got = sf_readf_float(in, interleaved.data(), info.frames);
	sf_close(in);
	if (got != info.frames) {
		r.reason = "couldn't be read to the end";
		return r;
	}

	int waves = (int)(info.frames / wt::WAVE);
	r.waves = waves;
	std::vector<float> chan((size_t)info.frames);
	for (int c = 0; c < info.channels; c++) {
		for (sf_count_t i = 0; i < info.frames; i++)
			chan[i] = interleaved[i * info.channels + c];
		if (options.fill) {
			wt::BlankReport report = wt::findBlanks(chan.data(), waves, waves);
			r.blankFound += report.blankCount;
			r.blankFilled += wt::fillBlanks(chan.data(), waves, report, options.morph, options.edges);
		}
		else {
			r.blankFound += wt::findBlanks(chan.data(), waves, waves).blankCount;
		}
		// Samples after the last whole wave are left alone
		wt::processTable(chan.data(), waves, options.cycle, options.normalize);
		for (sf_count_t i = 0; i < info.frames; i++)
			interleaved[i * info.channels + c] = chan[i];
	}

	// Write next to the destination, then swap it in, so a failed write never damages an existing file
	std::string tmp = dst + ".tmp-osiris";
	SF_INFO outInfo;
	memset(&outInfo, 0, sizeof(outInfo));
	outInfo.samplerate = info.samplerate;
	outInfo.channels = info.channels;
	outInfo.format = info.format;
	SNDFILE *out = sf_open(tmp.c_str(), SFM_WRITE, &outInfo);
	if (!out) {
		r.reason = std::string("can't be written in its format: ") + sf_strerror(NULL);
		return r;
	}
	if (!clm.empty()) {
		SF_CHUNK_INFO chunk;
		memset(&chunk, 0, sizeof(chunk));
		snprintf(chunk.id, sizeof(chunk.id), "clm ");
		chunk.id_size = 4;
		chunk.datalen = (unsigned)clm.size();
		chunk.data = clm.data();
		sf_set_chunk(out, &chunk);
	}
	sf_command(out, SFC_SET_CLIPPING, NULL, SF_TRUE);
	sf_count_t written = sf_writef_float(out, interleaved.data(), info.frames);
	sf_close(out);
	if (written != info.frames) {
		unlink(tmp.c_str());
		r.reason = "couldn't be written completely (disk full?)";
		return r;
	}
	if (rename(tmp.c_str(), dst.c_str()) != 0) {
		unlink(tmp.c_str());
		r.reason = std::string("couldn't be saved: ") + strerror(errno);
		return r;
	}
	r.ok = true;
	return r;
}

} // namespace batch
