// Command-line access to the Osiris SD card code, used by the CI build to test exporting without the GUI.
//
//   osiris-card-tool export <destination> <pack name> <wav file or folder>... [--merge] [--pad] [--cancel]
//   osiris-card-tool verify <destination or Osiris folder>
//   osiris-card-tool open <destination or Osiris folder>
//   osiris-card-tool project-roundtrip <wav file or folder>... (saves and reloads a card project, checks it matches)

#include "cardio.hpp"

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
		fprintf(stderr, "usage: verify <destination or Osiris folder>\n");
		return 2;
	}
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
	}
	fprintf(stderr, "usage: osiris-card-tool export|verify|open|project-roundtrip ...\n");
	return 2;
}
