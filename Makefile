# Grand Multiplayer Adventure - build with MinGW-w64 (MSYS2 mingw64 or similar).
#
#   make            build/GMA-Multiplayer-Setup.exe - the only thing you ship
#                   (it embeds the mod; intermediates go to build/obj/)
#   make test       build and run the unit + network tests
#   make tools      build/obj/fakeclient.exe (test bot)
#   make VERSION=v1.2.3 ...   stamp a version into the installer

CXX     ?= g++
WINDRES ?= windres
VERSION ?= dev
BUILD   := build
OBJ     := build/obj

CXXFLAGS := -O2 -std=c++17 -Wall -Wno-unused-function -DGMAMP_VERSION=\"$(VERSION)\"
HEADERS  := $(wildcard src/*.h)
MOD_SRC  := src/dllmain.cpp src/game.cpp src/gui.cpp src/hook.cpp src/log.cpp src/net.cpp src/steam.cpp src/settings_menu.cpp src/map_extras.cpp
NET_SRC  := src/net.cpp src/log.cpp

.PHONY: all mod tools tests test installer clean
all: installer

mod: $(OBJ)/zlib.dll
tools: $(OBJ)/fakeclient.exe
tests: $(OBJ)/gma_mp_tests.exe
installer: $(BUILD)/GMA-Multiplayer-Setup.exe

$(BUILD) $(OBJ):
	mkdir -p $@

# The mod: a zlib.dll proxy that forwards every export to zlib_orig.dll.
$(OBJ)/zlib.dll: $(MOD_SRC) $(HEADERS) src/zlib_proxy.def | $(OBJ)
	$(CXX) $(CXXFLAGS) -shared -static -s -o $@ $(MOD_SRC) src/zlib_proxy.def -lws2_32 -lopengl32 -lgdi32

$(OBJ)/fakeclient.exe: tools/fakeclient.cpp $(NET_SRC) $(HEADERS) | $(OBJ)
	$(CXX) $(CXXFLAGS) -static -s -o $@ tools/fakeclient.cpp $(NET_SRC) -lws2_32

$(OBJ)/gma_mp_tests.exe: tests/pose_tests.cpp $(NET_SRC) $(HEADERS) | $(OBJ)
	$(CXX) $(CXXFLAGS) -static -o $@ tests/pose_tests.cpp $(NET_SRC) -lws2_32

test: tests
	./$(OBJ)/gma_mp_tests.exe

$(OBJ)/setup_res.o: installer/setup.rc installer/setup.manifest installer/icon.ico \
		installer/fonts/Staatliches-Regular.ttf installer/fonts/ContrailOne-Regular.ttf $(OBJ)/zlib.dll | $(OBJ)
	$(WINDRES) installer/setup.rc -O coff -o $@

$(BUILD)/GMA-Multiplayer-Setup.exe: installer/setup.cpp installer/scene.cpp installer/scene.h $(OBJ)/setup_res.o | $(BUILD)
	$(CXX) $(CXXFLAGS) -D_WIN32_WINNT=0x0A00 -municode -mwindows -static -s -o $@ installer/setup.cpp \
		installer/scene.cpp $(OBJ)/setup_res.o -lshell32 -lole32 -luser32 -lgdi32 -lgdiplus -ldwmapi -ladvapi32

clean:
	rm -rf $(BUILD)
