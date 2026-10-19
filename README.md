# OsirisEdit

The modified wavetable and bank editor for the Osiris Eurorack synthesizer modules.

This fork adds a native Apple Silicon (arm64) build for macOS 11 and later, and a modern dark interface (the classic colour schemes are still under Colors).

![OsirisEdit waveform editor](https://raw.githubusercontent.com/TwistedGenetics/Modbap-Osiris-Fork/screenshots/0-editor.png)

### SD Card page

Lay out a whole Osiris card on the **SD Card** tab: four banks (A to D) of 32 wavetables, filled from the editor, WAV files or whole folders, reordered by drag and drop. **Export to SD Card** writes the layout the Osiris expects:

	<card>/Osiris/A, B, C, D
	Osiris_<Bank>_<PackName>_<1-32>.wav    16-bit, 44.1 kHz, mono, 32 waves of 256 samples

An existing Osiris folder is never deleted: it's backed up to `Osiris_backup_<date-time>` first (or merged, with replaced files moved to a backup). macOS `._` and `.DS_Store` files are cleaned off the card afterwards. **Open SD Card** reads a card back in, and layouts can be saved as `.osiriscard` project files. **File > Convert Folder for Osiris** turns a folder of WAVs into the same layout in one go.

### Batch processing

**File > Batch Process Folder** applies Cycle, Normalize and/or blank-wave filling to every wavetable in a folder, exactly as if you opened each one and clicked Cycle All / Normalize All. Files keep their names and format (wave count, sample rate, bit depth, channels). Output goes to a new `<folder>_cycled` folder unless you tick **Overwrite originals** (which asks first). Tables with a `clm` chunk declaring 2048-sample waves (Serum format) are skipped rather than mangled. The SD Card page has **Cycle All Slots** (undoable), and Export / Convert have an **Apply Cycle to every wavetable** option.

### Blank waves

When a wavetable with silent waves (below -60 dBFS) is opened, or added to the SD Card page, a pop-up offers to fill each gap by morphing between the waves either side: **Crossfade** (sample by sample) or **Spectral** (blends harmonics, so the level holds up when the two waves are out of phase). Blank waves at the start or end can hold the nearest wave, morph round to the other end of the table, or stay blank. The fill is a single undo step. **Preferences > Blank Waves** sets whether to ask, always fill or never fill.

### Download (Apple Silicon Mac)

Grab the latest `OsirisEdit-*-mac-arm64.zip` from the Releases page, or from the artifacts of the latest "Build macOS (Apple Silicon)" run on the Actions tab.

The app is ad-hoc signed, not notarised, so macOS will block it the first time. After unzipping, run this once in Terminal:

	xattr -dr com.apple.quarantine /path/to/OsirisEdit.app

or right-click the app, choose Open, and confirm.

### Building on macOS (Apple Silicon or Intel)

Install the dependencies with [Homebrew](https://brew.sh), fetch the submodules, and build:

	brew install sdl2 libsndfile libsamplerate jansson dylibbundler
	git submodule update --init --recursive
	make
	./OsirisEdit

To build the self-contained `.app` and zip in `dist/`:

	make dist

### Building on Linux or Windows

Make dependencies with

	cd dep
	make

Clone the in-source dependencies.

	cd ..
	git submodule update --init --recursive

Compile the program. The Makefile will automatically detect your operating system.

	make

Launch the program.

	./OsirisEdit

You can even try your luck with building the polished distributable. Although this method is unsupported, it may work with some tweaks to the Makefile.

	make dist
