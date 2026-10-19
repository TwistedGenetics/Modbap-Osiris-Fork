// Command-line access to the Osiris SD card code, used by the CI build to test exporting without the GUI.
//
//   osiris-card-tool export <destination> <pack name> <wav file or folder>... [--merge] [--pad] [--cancel]
//   osiris-card-tool verify <destination or Osiris folder>
//   osiris-card-tool open <destination or Osiris folder>
//   osiris-card-tool project-roundtrip <wav file or folder>... (saves and reloads a card project, checks it matches)
//   osiris-card-tool make-test-tables <dir>     (awkward wavetables for testing the batch: blank waves, 24-bit, stereo...)
//   osiris-card-tool batch <source dir> <output dir> [--no-cycle] [--normalize] [--fill crossfade|spectral] [--edges hold|wrap|leave]
//   osiris-card-tool verify-batch <source dir> <output dir> [--fill]   (checks the batch output independently)
//   osiris-card-tool generate <dir> [seeds per type] [--motion sweep|ping-pong|stepped|random-walk]
//                                     [--vintage s950|sp1200|crush] [--mutate]
//                                                      (one 32-wave table per generator type and seed, 16-bit 44.1 kHz;
//                                                       --mutate writes a mutated child and a bred child of each too)
//   osiris-card-tool verify-generated <dir> [--vintage] (checks the generator's rules on the written files; --vintage
//                                                       allows the aliasing that vintage character adds on purpose)

#include "cardio.hpp"
#include "batch.hpp"
#include "generator.hpp"

#include <math.h>

#include <dirent.h>
#include <regex>
#include <set>
#include <sndfile.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace osiris;

static bool isDir(const std::string &p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static std::vector<std::string> expandSources(const std::vector<std::string> &args) {
	std::vector<std::string> files;
	for (const std::string &a : args) {
		if (isDir(a)) {
			std::vector<std::string> f = listWavFiles(a);
			files.insert(files.end(), f.begin(), f.end());
		}
		else {
			files.push_back(a);
		}
	}
	return files;
}

static int cmdExport(int argc, char **argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: export <destination> <pack name> <wav file or folder>... [--merge] [--pad] [--cancel]\n");
		return 2;
	}
	ExportOptions options;
	std::vector<std::string> sources;
	for (int i = 4; i < argc; i++) {
		if (!strcmp(argv[i], "--merge"))
			options.existing = ExportOptions::MERGE;
		else if (!strcmp(argv[i], "--cancel"))
			options.existing = ExportOptions::CANCEL;
		else if (!strcmp(argv[i], "--pad"))
			options.fix = FIX_PAD;
		else if (!strcmp(argv[i], "--cycle"))
			options.cycle = true;
		else
			sources.push_back(argv[i]);
	}
	Card card;
	for (int b = 0; b < BANKS; b++)
		card.pack[b] = argv[3];
	std::vector<std::string> messages;
	int added = fillFromFiles(card, expandSources(sources), 0, 0, messages);
	for (const std::string &m : messages)
		printf("note: %s\n", m.c_str());
	printf("filled %d slots\n", added);

	ExportResult r = exportCard(card, argv[2], options);
	if (r.cancelled) {
		printf("cancelled: an Osiris folder already exists\n");
		return 3;
	}
	printf("wrote %d files to %s (A %d, B %d, C %d, D %d)\n", r.written, r.osirisPath.c_str(), r.perBank[0], r.perBank[1], r.perBank[2], r.perBank[3]);
	if (!r.backupPath.empty())
		printf("backup: %s\n", r.backupPath.c_str());
	for (const std::string &m : r.moved)
		printf("moved to backup: %s\n", m.c_str());
	printf("removed %d macOS junk files\n", r.junkRemoved);
	for (const std::string &e : r.errors)
		printf("error: %s\n", e.c_str());
	return r.ok ? 0 : 1;
}

/** Independent check of an exported card: layout, names and WAV format */
static int cmdVerify(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: verify <destination or Osiris folder> [--cycled]\n");
		return 2;
	}
	bool cycled = argc > 3 && !strcmp(argv[3], "--cycled");
	float worstJump = 0.f;
	std::string osirisPath = osirisFolderFor(argv[2]);
	int problems = 0;
	int files = 0;
	std::regex pattern("^Osiris_[A-D]_[A-Za-z0-9_-]+_([1-9]|[12][0-9]|3[0-2])\\.wav$");
	if (!isDir(osirisPath)) {
		printf("FAIL: no Osiris folder at %s\n", osirisPath.c_str());
		return 1;
	}
	// Nothing but the four bank folders inside Osiris
	DIR *d = opendir(osirisPath.c_str());
	while (struct dirent *ent = readdir(d)) {
		std::string name = ent->d_name;
		if (name == "." || name == "..")
			continue;
		if (name != "A" && name != "B" && name != "C" && name != "D") {
			printf("FAIL: unexpected entry in Osiris folder: %s\n", name.c_str());
			problems++;
		}
	}
	closedir(d);

	for (int b = 0; b < BANKS; b++) {
		std::string bankDir = osirisPath + "/" + bankLetter(b);
		if (!isDir(bankDir)) {
			printf("FAIL: missing bank folder %c\n", bankLetter(b));
			problems++;
			continue;
		}
		std::set<int> numbers;
		int count = 0;
		DIR *bd = opendir(bankDir.c_str());
		while (struct dirent *ent = readdir(bd)) {
			std::string name = ent->d_name;
			if (name == "." || name == "..")
				continue;
			count++;
			files++;
			if (!std::regex_match(name, pattern) || name[7] != bankLetter(b)) {
				printf("FAIL: %c/%s doesn't match Osiris_%c_<Pack>_<1-32>.wav\n", bankLetter(b), name.c_str(), bankLetter(b));
				problems++;
				continue;
			}
			if (name.size() >= 64) {
				printf("FAIL: %c/%s is 64 characters or longer\n", bankLetter(b), name.c_str());
				problems++;
			}
			int n = parseSlotNumber(name);
			if (!numbers.insert(n).second) {
				printf("FAIL: bank %c has two files numbered %d\n", bankLetter(b), n);
				problems++;
			}
			SF_INFO info;
			memset(&info, 0, sizeof(info));
			SNDFILE *sf = sf_open((bankDir + "/" + name).c_str(), SFM_READ, &info);
			if (!sf) {
				printf("FAIL: %c/%s can't be read\n", bankLetter(b), name.c_str());
				problems++;
				continue;
			}
			if (cycled) {
				// Every wave loops cleanly: the jump from the last sample to the first is at most 1/256 plus 16-bit rounding
				std::vector<float> data((size_t)info.frames * info.channels);
				sf_readf_float(sf, data.data(), info.frames);
				for (int w = 0; w + 1 <= (int)(info.frames / 256); w++) {
					float jump = fabsf(data[w * 256 + 255] - data[w * 256]);
					worstJump = fmaxf(worstJump, jump);
					if (jump > 1.f / 256 + 1e-3f) {
						printf("FAIL: %c/%s wave %d doesn't loop cleanly (jump %.4f)\n", bankLetter(b), name.c_str(), w + 1, jump);
						problems++;
						break;
					}
				}
			}
			sf_close(sf);
			bool formatOk = (info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV && (info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_16;
			if (!formatOk || info.channels != 1 || info.samplerate != SAMPLE_RATE || info.frames != FILE_SAMPLES) {
				printf("FAIL: %c/%s is %d ch, %d Hz, %lld frames, format 0x%x (want mono, 44100 Hz, %d frames, 16-bit WAV)\n", bankLetter(b), name.c_str(),
					info.channels, info.samplerate, (long long)info.frames, info.format, FILE_SAMPLES);
				problems++;
			}
		}
		closedir(bd);
		if (count > SLOTS) {
			printf("FAIL: bank %c has %d files (max 32)\n", bankLetter(b), count);
			problems++;
		}
		printf("bank %c: %d files\n", bankLetter(b), count);
	}
	if (cycled)
		printf("worst loop jump %.4f\n", worstJump);
	printf("%d files checked, %d problems\n", files, problems);
	return problems ? 1 : 0;
}

static int cmdOpen(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: open <destination or Osiris folder>\n");
		return 2;
	}
	Card card;
	std::vector<std::string> messages;
	bool ok = openCard(card, argv[2], messages);
	for (const std::string &m : messages)
		printf("note: %s\n", m.c_str());
	if (!ok)
		return 1;
	for (int b = 0; b < BANKS; b++) {
		int n = 0;
		for (int s = 0; s < SLOTS; s++)
			n += card.slots[b][s].filled;
		printf("bank %c: pack \"%s\", %d slots\n", bankLetter(b), card.pack[b].c_str(), n);
	}
	printf("%d slots filled\n", card.filledCount());
	return 0;
}

static int cmdProjectRoundtrip(int argc, char **argv) {
	std::vector<std::string> sources(argv + 2, argv + argc);
	Card card;
	card.pack[0] = "Round Trip";
	card.slotNamesInFileNames = true;
	std::vector<std::string> messages;
	fillFromFiles(card, expandSources(sources), 0, 0, messages);
	std::string path = "/tmp/osiris-card-roundtrip.osiriscard";
	std::string error;
	if (!saveProject(card, path, error)) {
		printf("FAIL: %s\n", error.c_str());
		return 1;
	}
	Card loaded;
	if (!loadProject(loaded, path, error)) {
		printf("FAIL: %s\n", error.c_str());
		return 1;
	}
	unlink(path.c_str());
	int problems = 0;
	if (loaded.pack[0] != card.pack[0] || loaded.slotNamesInFileNames != card.slotNamesInFileNames)
		problems++;
	for (int b = 0; b < BANKS; b++) {
		for (int s = 0; s < SLOTS; s++) {
			const Slot &x = card.slots[b][s], &y = loaded.slots[b][s];
			if (x.filled != y.filled || x.name != y.name || x.waveCount != y.waveCount || x.samples != y.samples) {
				printf("FAIL: slot %c%d differs after reloading\n", bankLetter(b), s + 1);
				problems++;
			}
		}
	}
	printf("%d slots round-tripped, %d problems\n", card.filledCount(), problems);
	return problems ? 1 : 0;
}

////////////////////
// Batch testing
////////////////////

struct Audio {
	SF_INFO info;
	std::vector<float> data; // interleaved
	std::string clm;
};

static bool readAudio(const std::string &path, Audio &a) {
	memset(&a.info, 0, sizeof(a.info));
	SNDFILE *sf = sf_open(path.c_str(), SFM_READ, &a.info);
	if (!sf)
		return false;
	a.data.resize((size_t)(a.info.frames * a.info.channels));
	sf_readf_float(sf, a.data.data(), a.info.frames);
	SF_CHUNK_INFO ci;
	memset(&ci, 0, sizeof(ci));
	snprintf(ci.id, sizeof(ci.id), "clm ");
	ci.id_size = 4;
	SF_CHUNK_ITERATOR *it = sf_get_chunk_iterator(sf, &ci);
	if (it) {
		SF_CHUNK_INFO d;
		memset(&d, 0, sizeof(d));
		if (sf_get_chunk_size(it, &d) == SF_ERR_NO_ERROR && d.datalen < 4096) {
			std::vector<char> buf(d.datalen + 1);
			d.data = buf.data();
			if (sf_get_chunk_data(it, &d) == SF_ERR_NO_ERROR)
				a.clm.assign(buf.data(), d.datalen);
		}
	}
	sf_close(sf);
	return true;
}

static bool writeAudio(const std::string &path, int format, int rate, int channels, const std::vector<float> &data, const char *clm = NULL) {
	SF_INFO info;
	memset(&info, 0, sizeof(info));
	info.samplerate = rate;
	info.channels = channels;
	info.format = format;
	SNDFILE *sf = sf_open(path.c_str(), SFM_WRITE, &info);
	if (!sf) {
		printf("can't write %s: %s\n", path.c_str(), sf_strerror(NULL));
		return false;
	}
	if (clm) {
		SF_CHUNK_INFO ci;
		memset(&ci, 0, sizeof(ci));
		snprintf(ci.id, sizeof(ci.id), "clm ");
		ci.id_size = 4;
		ci.datalen = (unsigned)strlen(clm);
		ci.data = (void *)clm;
		sf_set_chunk(sf, &ci);
	}
	sf_writef_float(sf, data.data(), (sf_count_t)(data.size() / channels));
	sf_close(sf);
	return true;
}

/** Waves with a deliberate jump at the loop point (a ramp on top), so Cycle has something to fix */
static void testWave(float *w, int k, int stride) {
	for (int i = 0; i < 256; i++) {
		float x = (float)i / 256;
		float v = 0.5f * sinf(2.f * (float)M_PI * (1 + k % 4) * x + k * 0.4f) + 0.3f * (x - 0.5f);
		w[i * stride] = v;
	}
}

static int cmdMakeTestTables(int argc, char **argv) {
	if (argc < 3)
		return 2;
	std::string dir = argv[2];
	batch::makeDirs(dir);
	const int PCM16 = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	bool ok = true;

	// 32 waves, 16-bit, blanks in the middle (waves 9-14) and at the end (31-32)
	std::vector<float> a(32 * 256, 0.f);
	for (int k = 0; k < 32; k++)
		if (!((k >= 8 && k <= 13) || k >= 30))
			testWave(&a[k * 256], k, 1);
	ok &= writeAudio(dir + "/01 Blanks.wav", PCM16, 44100, 1, a);

	// 64 waves, 24-bit, 48 kHz, blanks at the start
	std::vector<float> b(64 * 256, 0.f);
	for (int k = 3; k < 64; k++)
		testWave(&b[k * 256], k, 1);
	ok &= writeAudio(dir + "/02 Wide 24bit.wav", SF_FORMAT_WAV | SF_FORMAT_PCM_24, 48000, 1, b);

	// Stereo float, 16 waves
	std::vector<float> c(16 * 256 * 2, 0.f);
	for (int k = 0; k < 16; k++) {
		testWave(&c[k * 256 * 2], k, 2);
		testWave(&c[k * 256 * 2 + 1], k + 1, 2);
	}
	ok &= writeAudio(dir + "/03 Stereo Float.wav", SF_FORMAT_WAV | SF_FORMAT_FLOAT, 44100, 2, c);

	// 8 waves plus 100 extra samples that must come through untouched
	std::vector<float> d(8 * 256 + 100, 0.f);
	for (int k = 0; k < 8; k++)
		testWave(&d[k * 256], k, 1);
	for (int i = 0; i < 100; i++)
		d[8 * 256 + i] = 0.25f * sinf(i * 0.3f);
	ok &= writeAudio(dir + "/04 Leftover.wav", PCM16, 44100, 1, d);

	// Serum-style table (2048-sample waves): must be skipped
	std::vector<float> e(4 * 2048, 0.f);
	for (int i = 0; i < (int)e.size(); i++)
		e[i] = 0.5f * sinf(2.f * (float)M_PI * i / 2048);
	ok &= writeAudio(dir + "/05 Serum 2048.wav", SF_FORMAT_WAV | SF_FORMAT_FLOAT, 44100, 1, e, "<!>2048 00000000 wavetable (www.xferrecords.com)");

	// A 256-sample table with a clm chunk saying so: processed, and the chunk kept
	std::vector<float> f(4 * 256, 0.f);
	for (int k = 0; k < 4; k++)
		testWave(&f[k * 256], k, 1);
	ok &= writeAudio(dir + "/06 Tagged 256.wav", PCM16, 44100, 1, f, "<!>256 00000000 wavetable");

	// Numbered so natural order matters
	for (int n : {2, 10}) {
		std::vector<float> g(32 * 256);
		for (int k = 0; k < 32; k++)
			testWave(&g[k * 256], k + n, 1);
		ok &= writeAudio(dir + "/Table " + std::to_string(n) + ".wav", PCM16, 44100, 1, g);
	}
	printf("test tables written to %s\n", dir.c_str());
	return ok ? 0 : 1;
}

static int cmdBatch(int argc, char **argv) {
	if (argc < 4)
		return 2;
	batch::Options o;
	for (int i = 4; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--no-cycle")
			o.cycle = false;
		else if (a == "--normalize")
			o.normalize = true;
		else if (a == "--fill" && i + 1 < argc) {
			o.fill = true;
			o.morph = !strcmp(argv[++i], "spectral") ? wt::SPECTRAL : wt::CROSSFADE;
		}
		else if (a == "--edges" && i + 1 < argc) {
			std::string e = argv[++i];
			o.edges = e == "wrap" ? wt::EDGE_WRAP : (e == "leave" ? wt::EDGE_LEAVE : wt::EDGE_HOLD);
		}
	}
	std::string out = argv[3];
	if (!batch::makeDirs(out)) {
		printf("can't create %s\n", out.c_str());
		return 1;
	}
	int ok = 0, skipped = 0, filled = 0;
	for (const std::string &f : listWavFiles(argv[2])) {
		batch::FileResult r = batch::processFile(f, batch::outputPath(f, out), o);
		if (r.ok) {
			ok++;
			filled += r.blankFilled;
			printf("ok      %s (%d waves, %d blank, %d filled)\n", r.name.c_str(), r.waves, r.blankFound, r.blankFilled);
		}
		else {
			skipped++;
			printf("skipped %s %s\n", r.name.c_str(), r.reason.c_str());
		}
	}
	printf("%d processed, %d skipped, %d blank waves filled\n", ok, skipped, filled);
	return 0;
}

/** Independent checks of a batch run with Cycle on */
static int cmdVerifyBatch(int argc, char **argv) {
	if (argc < 4)
		return 2;
	bool fill = argc > 4 && !strcmp(argv[4], "--fill");
	std::string src = argv[2], out = argv[3];
	int problems = 0, checked = 0;
	float worstJump = 0.f, worstJumpBefore = 0.f;
	for (const std::string &f : listWavFiles(src)) {
		std::string name = f.substr(f.find_last_of('/') + 1);
		std::string o = out + "/" + name;
		Audio a, b;
		readAudio(f, a);
		bool serum = a.clm.find("<!>2048") != std::string::npos;
		struct stat st;
		bool exists = stat(o.c_str(), &st) == 0;
		if (serum) {
			if (exists) {
				printf("FAIL: %s is a 2048-sample table and should have been skipped\n", name.c_str());
				problems++;
			}
			continue;
		}
		if (!exists || !readAudio(o, b)) {
			printf("FAIL: %s missing from the output\n", name.c_str());
			problems++;
			continue;
		}
		checked++;
		// Same format
		if (a.info.format != b.info.format || a.info.channels != b.info.channels || a.info.samplerate != b.info.samplerate || a.info.frames != b.info.frames) {
			printf("FAIL: %s format changed (format 0x%x->0x%x, %d->%d ch, %d->%d Hz, %lld->%lld frames)\n", name.c_str(), a.info.format, b.info.format,
				a.info.channels, b.info.channels, a.info.samplerate, b.info.samplerate, (long long)a.info.frames, (long long)b.info.frames);
			problems++;
			continue;
		}
		if (a.clm != b.clm) {
			printf("FAIL: %s lost its clm chunk\n", name.c_str());
			problems++;
		}
		int ch = a.info.channels;
		int waves = (int)(a.info.frames / 256);
		for (int c = 0; c < ch; c++) {
			for (int w = 0; w < waves; w++) {
				float first = b.data[((size_t)w * 256) * ch + c], last = b.data[((size_t)w * 256 + 255) * ch + c];
				float firstA = a.data[((size_t)w * 256) * ch + c], lastA = a.data[((size_t)w * 256 + 255) * ch + c];
				worstJump = fmaxf(worstJump, fabsf(last - first));
				worstJumpBefore = fmaxf(worstJumpBefore, fabsf(lastA - firstA));
				// After Cycle the loop jump is at most |first sample| / 256, plus 16-bit rounding
				if (fabsf(last - first) > 1.f / 256 + 1e-3f) {
					printf("FAIL: %s wave %d (channel %d) doesn't loop cleanly: jumps %.4f\n", name.c_str(), w + 1, c + 1, fabsf(last - first));
					problems++;
				}
				if (fill) {
					bool blank = true;
					for (int i = 0; i < 256 && blank; i++)
						blank = fabsf(b.data[((size_t)w * 256 + i) * ch + c]) < 0.001f;
					if (blank) {
						printf("FAIL: %s wave %d (channel %d) is still blank\n", name.c_str(), w + 1, c + 1);
						problems++;
					}
				}
			}
			// Samples after the last whole wave come through unchanged
			for (sf_count_t i = (sf_count_t)waves * 256; i < a.info.frames; i++) {
				if (fabsf(a.data[i * ch + c] - b.data[i * ch + c]) > 1e-4f) {
					printf("FAIL: %s sample %lld after the last wave changed\n", name.c_str(), (long long)i);
					problems++;
					break;
				}
			}
		}
	}
	printf("%d files checked; loop jump before %.3f, after %.4f; %d problems\n", checked, worstJumpBefore, worstJump, problems);
	if (worstJumpBefore < 0.1f) {
		printf("FAIL: the test tables should have had big loop jumps before cycling\n");
		problems++;
	}
	return problems ? 1 : 0;
}

////////////////////
// Generator
////////////////////

static int cmdGenerate(int argc, char **argv) {
	if (argc < 3)
		return 2;
	std::string dir = argv[2];
	int perType = 1;
	gen::Motion motion = gen::SWEEP;
	gen::Vintage vintage = gen::VINTAGE_OFF;
	bool mutations = false;
	for (int i = 3; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--motion" && i + 1 < argc) {
			std::string m = argv[++i];
			motion = m == "ping-pong" ? gen::PING_PONG : (m == "stepped" ? gen::STEPPED : (m == "random-walk" ? gen::RANDOM_WALK : gen::SWEEP));
		}
		else if (a == "--vintage" && i + 1 < argc) {
			std::string v = argv[++i];
			vintage = v == "s950" ? gen::S950 : (v == "sp1200" ? gen::SP1200 : (v == "crush" ? gen::CRUSH : gen::VINTAGE_OFF));
		}
		else if (a == "--mutate")
			mutations = true;
		else
			perType = atoi(a.c_str());
	}
	batch::makeDirs(dir);
	int written = 0;
	auto write = [&](const gen::Params &p) {
		std::vector<float> table = gen::generate(p);
		std::string error;
		std::string path = dir + "/" + gen::tableName(p) + ".wav";
		if (!writeWavetable(path, table, error)) {
			printf("%s\n", error.c_str());
			return false;
		}
		written++;
		return true;
	};
	for (int t = 0; t < gen::TYPE_COUNT; t++) {
		gen::Params p = gen::defaults((gen::Type)t);
		p.waves = 32;
		p.motion = motion;
		p.vintage = vintage;
		for (int i = 0; i < perType; i++) {
			p.seed = i == 0 ? 427 : gen::nextSeed(p.seed);
			if (!write(p))
				return 1;
			if (mutations) {
				gen::Params child = gen::mutate(p, 0.5f, p.seed);            // ..._g1
				gen::Params grandchild = gen::breed(child, gen::mutate(p, 0.5f, p.seed + 1), 0.5f, p.seed); // ..._g2
				if (!write(child) || !write(grandchild))
					return 1;
			}
		}
	}
	printf("%d tables written to %s\n", written, dir.c_str());
	return 0;
}

static int cmdVerifyGenerated(int argc, char **argv) {
	if (argc < 3)
		return 2;
	int problems = 0, files = 0;
	float worst16 = 1.f / 32768.f * 4.f; // allowance for 16-bit rounding
	bool vintage = argc > 3 && !strcmp(argv[3], "--vintage");
	for (const std::string &f : listWavFiles(argv[2])) {
		std::string name = f.substr(f.find_last_of('/') + 1);
		Audio a;
		if (!readAudio(f, a) || a.info.channels != 1 || a.info.samplerate != 44100 || a.info.frames != 32 * 256 ||
			(a.info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) {
			printf("FAIL: %s is not a 16-bit 44.1 kHz mono 32 x 256 wavetable\n", name.c_str());
			problems++;
			continue;
		}
		files++;
		bool bass = false;
		for (int t = 0; t < gen::TYPE_COUNT; t++)
			if (name.compare(0, strlen(gen::typeName((gen::Type)t)) + 1, std::string(gen::typeName((gen::Type)t)) + "_") == 0)
				bass = gen::isBass((gen::Type)t);
		float amp[129], ph[129];
		for (int w = 0; w < 32; w++) {
			const float *wave = &a.data[w * 256];
			gen::analyse(wave, amp, ph);
			float peak = 0.f, maxStep = 0.f;
			for (int i = 0; i < 256; i++)
				peak = fmaxf(peak, fabsf(wave[i]));
			for (int i = 0; i + 1 < 256; i++)
				maxStep = fmaxf(maxStep, fabsf(wave[i + 1] - wave[i]));
			float wrap = fabsf(wave[0] - wave[255]);
			const char *fail = NULL;
			if (amp[128] > 1e-4f && !vintage)
				fail = "has content at the Nyquist bin (not band-limited)";
			else if (amp[0] > (vintage ? 2e-3f : 1e-4f))
				fail = "has a DC offset";
			else if (fabsf(peak - gen::PEAK) > worst16)
				fail = "has a different peak";
			else if (wt::rms(wave) < 0.05f)
				fail = "is near-silent";
			else if (wrap > 1.5f * maxStep + worst16)
				fail = "clicks at the loop point";
			if (!fail && bass) {
				float maxOther = 0.f;
				for (int k = 2; k < 128; k++)
					maxOther = fmaxf(maxOther, amp[k]);
				if (amp[1] < (vintage ? 1.15f : 1.25f) * maxOther)
					fail = "fundamental isn't the loudest harmonic";
				else if (!vintage && fabsf(remainderf(ph[1] + 1.5707963f, 6.2831853f)) > 5e-3f)
					fail = "fundamental phase drifts";
			}
			if (fail) {
				printf("FAIL: %s wave %d %s\n", name.c_str(), w + 1, fail);
				problems++;
				break;
			}
		}
	}
	printf("%d generated tables checked, %d problems\n", files, problems);
	return (problems || files == 0) ? 1 : 0;
}

int main(int argc, char **argv) {
	if (argc >= 2) {
		if (!strcmp(argv[1], "export"))
			return cmdExport(argc, argv);
		if (!strcmp(argv[1], "verify"))
			return cmdVerify(argc, argv);
		if (!strcmp(argv[1], "open"))
			return cmdOpen(argc, argv);
		if (!strcmp(argv[1], "project-roundtrip"))
			return cmdProjectRoundtrip(argc, argv);
		if (!strcmp(argv[1], "make-test-tables"))
			return cmdMakeTestTables(argc, argv);
		if (!strcmp(argv[1], "batch"))
			return cmdBatch(argc, argv);
		if (!strcmp(argv[1], "verify-batch"))
			return cmdVerifyBatch(argc, argv);
		if (!strcmp(argv[1], "generate"))
			return cmdGenerate(argc, argv);
		if (!strcmp(argv[1], "verify-generated"))
			return cmdVerifyGenerated(argc, argv);
	}
	fprintf(stderr, "usage: osiris-card-tool export|verify|open|project-roundtrip|make-test-tables|batch|verify-batch ...\n");
	return 2;
}
