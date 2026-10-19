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

### Morph between keyframes

Mark waves as keyframes (select a wave and press **K**, or right-click > **Keyframe**; they get a diamond on their thumbnail), then **Morph...** fills every wave between neighbouring keyframes with a morph from one to the next. For example, a sine at 1, a square at 16 and an FM wave at 32 gives a sine-to-square morph across 2-15 and square-to-FM across 17-31. With a range selected (shift-click), its first and last waves count as keyframes too. Choose Crossfade or Spectral and a curve (Linear, Ease In, Ease Out, S-curve); keyframes are used as you hear them and are never changed. One undo step.

### Generate wavetables (drum & bass / jungle)

**Generate...** builds tables from recipes:

- **Bass:** Sub, Reese, Neuro, Wobble, Talking (walks through the vowels like it's saying a word)
- **Lead:** Hoover, Stab, Acid, Bell (two-operator FM), Pluck (plucked string), Sync (hard-sync sweep), Fold (wavefolder)
- **Pad:** Warm, Glassy, Evolving, Choir (singing vowels), Strings (detuned saw ensemble)

Shape them with Brightness, Movement, Grit, Keyframes, morph type and a seed. The same seed and settings always give the same table (named like `Reese_0427`). Press Play in the dialog to audition it, then **Use in Editor** (keyframes are marked so you can re-morph it) or **Add Tables** to fill the next free slots on the SD Card page.

- **Motion** sets how the table moves through its keyframes: **Sweep** (first to last), **Ping-pong** (out and back, so wave 32 leads straight back into wave 1: loops cleanly with an LFO on Morph Z), **Stepped** (holds each keyframe for a block of waves, for rhythmic jumps when Morph Z is sequenced) or **Random walk**.
- **Vintage** adds old-sampler character: **S950** (12-bit, softly filtered), **SP-1200** (12-bit, lower rate, unfiltered) or **Crush** (8-bit, very low rate). These let aliasing through on purpose; the steps are centred on the loop point, so the loop still never clicks. The name gets a suffix, like `Reese_0427_SP1200`.
- **From my waves** starts the table on the editor's marked keyframes (or the selected wave) and turns into the chosen type by the end; **Into type** sets how far.
- **Mutate & Breed...** shows six variations of the current table. Click one to hear it, **Keep** it to carry on mutating from there, or pick two and **Breed** them (each of the recipe's random choices comes from one parent or the other). **Mutation** sets how far the variations stray: small amounts fine-tune, big ones can flip choices like an FM ratio. Mutated tables are named by generation, like `Reese_0427_g3`.

Without vintage, every wave is built from harmonics 1-127, so it's band-limited, has no DC and loops seamlessly, and every wave has the same peak. On bass tables the fundamental stays the loudest harmonic with the same phase on every wave, so the sub never thins out as the table morphs.

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
