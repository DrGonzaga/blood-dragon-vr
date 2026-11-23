# Cross-compile the 32-bit proxy DLL from Linux.
#
#   make NAME=mybuild CXX=i686-w64-mingw32-g++-posix
#
# build1 (review H2/H3): each source file is compiled on its own, so changing one
# file rebuilds that file and links (seconds, not the whole mod), and several can
# compile at once (make -j8). NAME is the build's name: it goes into the log's first
# line ("fc3vr phase6 <NAME> loaded") with the date, so nobody edits hook.cpp for a
# new build any more. Without NAME the log says "dev".
CXX = i686-w64-mingw32-g++
NAME = dev
CXXFLAGS = -Ibuild -O2 -Wall -std=c++17 -fno-exceptions -fno-rtti -Ithird_party/openxr/include
LDFLAGS = -shared -static -static-libgcc -static-libstdc++ -s -Wl,--enable-stdcall-fixup -ldxguid -luuid -lwindowscodecs -lole32 -lshell32 -lpsapi -lwinmm
SRC = src/proxy.cpp src/prewarm.cpp src/hook.cpp src/xr.cpp src/headtrack.cpp src/engine.cpp src/res.cpp src/settings.cpp src/memcheck.cpp src/patch.cpp src/d3dstate.cpp src/vrs.cpp src/keys.cpp src/input.cpp src/stereo.cpp src/trace.cpp src/bonehook.cpp src/laser.cpp src/menu.cpp src/gfx.cpp src/gunattach.cpp src/body.cpp src/hud.cpp src/shots.cpp src/scope.cpp src/loot.cpp src/turret.cpp src/vehgame.cpp src/online.cpp src/outpost.cpp src/damage.cpp src/markers.cpp src/stock.cpp src/prompts.cpp src/cpufix.cpp src/holster.cpp src/twohand.cpp src/bow.cpp src/bowstring.cpp src/hands.cpp src/cybereye.cpp src/slowmo.cpp src/slowfx.cpp src/ammo.cpp src/movie.cpp src/slowsnd.cpp src/comfort.cpp src/smaa.cpp src/imgui_dx11.cpp third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
OBJ = $(patsubst %.cpp,build/obj/%.o,$(SRC))

all: build/fc3vr.dll

build/fc3vr.dll: $(OBJ) src/proxy.def
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) src/proxy.def $(LDFLAGS)

# Every file of the mod sees fc3vr.h; a change to it rebuilds them all.
build/obj/src/%.o: src/%.cpp src/fc3vr.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/obj/third_party/%.o: third_party/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# The build name: hook.cpp is compiled again whenever NAME differs from last time.
build/name.txt: FORCE
	@mkdir -p build
	@echo '$(NAME)' | cmp -s - $@ || echo '$(NAME)' > $@
build/obj/src/hook.o: CXXFLAGS += -DFC3VR_BUILD='"$(NAME)"'
build/obj/src/hook.o: build/name.txt

# smaa1: the SMAA shader goes into smaa.cpp as a string (split: compilers cap one literal's length).
build/obj/src/smaa.o: build/smaa_hlsl.inc
build/smaa_hlsl.inc: src/smaa_pre.hlsl third_party/smaa/SMAA.hlsl src/smaa_post.hlsl
	mkdir -p build
	cat $^ | python3 -c "import sys; d=sys.stdin.read(); print(chr(10).join('R\"SMAA(' + d[i:i+12000] + ')SMAA\"' for i in range(0, len(d), 12000)))" > $@

clean:
	rm -rf build

FORCE:
.PHONY: all clean FORCE

# synth1: the VR menu lives in synth.inc, included by menu.cpp
build/obj/src/menu.o: src/synth.inc
