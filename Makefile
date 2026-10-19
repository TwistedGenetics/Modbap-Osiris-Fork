VERSION = 1.1

FLAGS = -Wall -Wextra -Wno-unused-parameter -g -Wno-unused -O3 -ffast-math -fno-finite-math-only \
	-DVERSION=$(VERSION) -DPFFFT_SIMD_DISABLE \
	-I. -Iext -Iext/imgui -Idep/include -Idep/include/SDL2
CFLAGS =
CXXFLAGS = -std=c++11
LDFLAGS =


SOURCES = \
	ext/pffft/pffft.c \
	ext/lodepng/lodepng.cpp \
	ext/imgui/imgui.cpp \
	ext/imgui/imgui_draw.cpp \
	ext/imgui/imgui_demo.cpp \
	ext/imgui/examples/sdl_opengl2_example/imgui_impl_sdl.cpp \
	$(wildcard src/*.cpp)


# CPU-specific
# -march=nocona is an Intel-only flag; Apple Silicon (arm64) compilers reject it.
CPU := $(shell uname -m)
ifneq (,$(filter x86_64 i686 i386,$(CPU)))
	FLAGS += -march=nocona
endif


# OS-specific
include Makefile-arch.inc
ifeq ($(ARCH),lin)
	# Linux
	FLAGS += -DARCH_LIN $(shell pkg-config --cflags gtk+-2.0)
	LDFLAGS += -static-libstdc++ -static-libgcc \
		-lGL -lpthread \
		-Ldep/lib -lSDL2 -lsamplerate -lsndfile -ljansson -lcurl \
		-lgtk-x11-2.0 -lgobject-2.0
	SOURCES += ext/osdialog/osdialog_gtk2.c
else ifeq ($(ARCH),mac)
	# Mac (Apple Silicon or Intel)
	# Libraries come from Homebrew: brew install sdl2 libsndfile libsamplerate jansson dylibbundler
	# libcurl is the one that ships with macOS.
	BREW_PREFIX ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
	MACOS_MIN ?= 11.0
	SDL3_DYLIB ?= $(shell brew --prefix sdl3 2>/dev/null)/lib/libSDL3.0.dylib
	FLAGS += -DARCH_MAC -DGL_SILENCE_DEPRECATION \
		-mmacosx-version-min=$(MACOS_MIN) \
		-I$(BREW_PREFIX)/include -I$(BREW_PREFIX)/include/SDL2
	CXXFLAGS += -stdlib=libc++
	LDFLAGS += -mmacosx-version-min=$(MACOS_MIN) \
		-stdlib=libc++ -lpthread \
		-framework Cocoa -framework OpenGL -framework IOKit -framework CoreVideo \
		-L$(BREW_PREFIX)/lib -lSDL2 -lsamplerate -lsndfile -ljansson -lcurl
	SOURCES += ext/osdialog/osdialog_mac.m
else ifeq ($(ARCH),win)
	# Windows
	FLAGS += -DARCH_WIN
	LDFLAGS += \
		-Ldep/lib -lmingw32 -lSDL2main -lSDL2 -lsamplerate -lsndfile -ljansson -lcurl \
		-lopengl32 -mwindows
	SOURCES += ext/osdialog/osdialog_win.c
	OBJECTS += info.o
info.o: info.rc
	windres $^ $@
endif


.DEFAULT_GOAL := build
build: OsirisEdit

run: OsirisEdit
	LD_LIBRARY_PATH=dep/lib ./OsirisEdit

debug: OsirisEdit
ifeq ($(ARCH),mac)
	lldb ./OsirisEdit
else
	gdb -ex 'run' ./OsirisEdit
endif


OBJECTS += $(SOURCES:%=build/%.o)


OsirisEdit: $(OBJECTS)
	$(CXX) -o $@ $^ $(LDFLAGS)

clean:
	rm -frv $(OBJECTS) OsirisEdit dist build/card_test build/osiris-card-tool


# Unit tests: SD card naming/sorting (no libraries needed), and wavetable processing (links the editor's wave code)
.PHONY: test
test: build/card_test build/process_test
	./build/card_test
	./build/process_test

build/process_test: tests/process_test.cpp src/wtprocess.cpp src/wtprocess.hpp src/wave.cpp src/math.cpp build/pffft_test.o
	@mkdir -p build
	$(CXX) $(FLAGS) $(CXXFLAGS) -Isrc -o $@ tests/process_test.cpp src/wtprocess.cpp src/wave.cpp src/math.cpp build/pffft_test.o $(LDFLAGS)

build/pffft_test.o: ext/pffft/pffft.c
	@mkdir -p build
	$(CC) $(FLAGS) $(CFLAGS) -c -o $@ $<

build/card_test: tests/card_test.cpp src/osiriscard.cpp src/osiriscard.hpp
	@mkdir -p build
	$(CXX) -std=c++11 -Wall -Wextra -O1 -Isrc -o $@ tests/card_test.cpp src/osiriscard.cpp

# Command-line card tool, used by CI to export and verify a test card
.PHONY: cardtool
cardtool: build/osiris-card-tool

CARDTOOL_SOURCES = tools/osiris_card_tool.cpp src/cardio.cpp src/osiriscard.cpp src/wtprocess.cpp src/batch.cpp

build/osiris-card-tool: $(CARDTOOL_SOURCES) src/cardio.hpp src/osiriscard.hpp src/wtprocess.hpp src/batch.hpp build/pffft_test.o
	@mkdir -p build
	$(CXX) $(FLAGS) $(CXXFLAGS) -Isrc -o $@ $(CARDTOOL_SOURCES) build/pffft_test.o $(LDFLAGS)


.PHONY: dist
dist: OsirisEdit
	mkdir -p dist/OsirisEdit
	cp -R banks dist/OsirisEdit
	cp LICENSE* dist/OsirisEdit
	cp doc/manual.pdf dist/OsirisEdit
ifeq ($(ARCH),lin)
	cp -R logo*.png fonts catalog dist/OsirisEdit
	cp OsirisEdit OsirisEdit.sh dist/OsirisEdit
	cp dep/lib/libSDL2-2.0.so.0 dist/OsirisEdit
	cp dep/lib/libsamplerate.so.0 dist/OsirisEdit
	cp dep/lib/libsndfile.so.1 dist/OsirisEdit
	cp dep/lib/libjansson.so.4 dist/OsirisEdit
	cp dep/lib/libcurl.so.4 dist/OsirisEdit
else ifeq ($(ARCH),mac)
	mkdir -p dist/OsirisEdit/OsirisEdit.app/Contents/MacOS
	mkdir -p dist/OsirisEdit/OsirisEdit.app/Contents/Resources
	mkdir -p dist/OsirisEdit/OsirisEdit.app/Contents/Frameworks
	cp Info.plist dist/OsirisEdit/OsirisEdit.app/Contents
	cp OsirisEdit dist/OsirisEdit/OsirisEdit.app/Contents/MacOS
	cp -R logo*.png logo.icns fonts catalog dist/OsirisEdit/OsirisEdit.app/Contents/Resources
	# Copy the Homebrew dylibs (and everything they depend on) into the bundle and repoint the binary at them
	dylibbundler -of -b -cd \
		-x dist/OsirisEdit/OsirisEdit.app/Contents/MacOS/OsirisEdit \
		-d dist/OsirisEdit/OsirisEdit.app/Contents/Frameworks \
		-p @executable_path/../Frameworks/
	otool -L dist/OsirisEdit/OsirisEdit.app/Contents/MacOS/OsirisEdit
	# Homebrew's sdl2 is sdl2-compat, which dlopen()s SDL3 at startup, so dylibbundler can't see it.
	# sdl2-compat looks for libSDL3.dylib next to itself, so put a copy in Frameworks.
	if [ -f "$(SDL3_DYLIB)" ]; then \
		cp "$(SDL3_DYLIB)" dist/OsirisEdit/OsirisEdit.app/Contents/Frameworks/libSDL3.dylib && \
		chmod u+w dist/OsirisEdit/OsirisEdit.app/Contents/Frameworks/libSDL3.dylib && \
		install_name_tool -id @executable_path/../Frameworks/libSDL3.dylib dist/OsirisEdit/OsirisEdit.app/Contents/Frameworks/libSDL3.dylib && \
		otool -L dist/OsirisEdit/OsirisEdit.app/Contents/Frameworks/libSDL3.dylib; \
	fi
	# arm64 code must be signed to run; ad-hoc signing is enough for local use
	codesign --force --deep --sign - dist/OsirisEdit/OsirisEdit.app
	codesign --verify --deep --strict dist/OsirisEdit/OsirisEdit.app
else ifeq ($(ARCH),win)
	cp -R logo*.png fonts catalog dist/OsirisEdit
	cp OsirisEdit.exe dist/OsirisEdit
	cp /mingw32/bin/libgcc_s_dw2-1.dll dist/OsirisEdit
	cp /mingw32/bin/libwinpthread-1.dll dist/OsirisEdit
	cp /mingw32/bin/libstdc++-6.dll dist/OsirisEdit
	cp dep/bin/SDL2.dll dist/OsirisEdit
	cp dep/bin/libsamplerate-0.dll dist/OsirisEdit
	cp dep/bin/libsndfile-1.dll dist/OsirisEdit
	cp dep/bin/libjansson-4.dll dist/OsirisEdit
	cp dep/bin/libcurl-4.dll dist/OsirisEdit
endif
	cd dist && zip -9 -r -y OsirisEdit-$(VERSION)-$(ARCH)-$(CPU).zip OsirisEdit


# SUFFIXES:

build/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) $(FLAGS) $(CFLAGS) -c -o $@ $<

build/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(FLAGS) $(CXXFLAGS) -c -o $@ $<

build/%.m.o: %.m
	@mkdir -p $(@D)
	$(CC) $(FLAGS) $(CFLAGS) -c -o $@ $<
