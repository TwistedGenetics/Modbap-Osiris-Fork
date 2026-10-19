# OsirisEdit

The modified wavetable and bank editor for the Osiris Eurorack synthesizer modules.

This fork adds a native Apple Silicon (arm64) build for macOS 11 and later, and a modern dark interface (the classic colour schemes are still under Colors).

![OsirisEdit waveform editor](https://raw.githubusercontent.com/TwistedGenetics/Modbap-Osiris-Fork/screenshots/0-editor.png)

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
