# Quick build (Linux/macOS). For IDEs / Windows use CMakeLists.txt.
CXX      ?= c++
OPT      ?= -O1 $(shell $(CXX) -foptimize-sibling-calls -x c++ -c /dev/null -o /dev/null 2>/dev/null && echo -foptimize-sibling-calls)
CXXFLAGS ?= $(OPT) -std=c++17 -g0 -fno-strict-aliasing -fwrapv -w -Iruntime/include -Igenerated
# SDL2 window/input if sdl2-config is available (macOS: brew install sdl2), else headless
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)
ifneq ($(SDL_CFLAGS),)
CXXFLAGS += -DMVP_SDL $(SDL_CFLAGS)
endif
GEN_SRC  := $(sort $(wildcard generated/funcs_*.cpp)) generated/functable.cpp $(sort $(wildcard generated/vu1_blocks_*.cpp))
RT_SRC   := $(wildcard runtime/src/*.cpp)
BUILD    ?= build
OBJ      := $(patsubst %.cpp,$(BUILD)/%.o,$(GEN_SRC) $(RT_SRC))
# macOS: the Metal renderer and presenter (Objective-C++)
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
ifneq ($(SDL_CFLAGS),)
MM_SRC   := $(wildcard runtime/src/*.mm)
OBJ      += $(patsubst %.mm,$(BUILD)/%.o,$(MM_SRC))
GFX_LIBS += -framework Metal -framework QuartzCore -framework Foundation
endif
endif
# Vulkan renderer when the Vulkan headers + loader are found (macOS: brew install molten-vk vulkan-loader
# vulkan-headers shaderc; Linux: libvulkan-dev). The SPIR-V is in runtime/src/vk/*.inc; glslc remakes it on edits.
ifneq ($(SDL_CFLAGS),)
VK_PREFIX := $(firstword $(wildcard /opt/homebrew /usr/local))
# Only the headers are needed: the Vulkan loader is opened at run time (no driver = the game falls back to OpenGL).
ifneq ($(wildcard $(VK_PREFIX)/include/vulkan/vulkan.h),)
CXXFLAGS += -DMVP_VULKAN -I$(VK_PREFIX)/include
else ifneq ($(wildcard /usr/include/vulkan/vulkan.h),)
CXXFLAGS += -DMVP_VULKAN
else ifneq ($(VULKAN_SDK),)
CXXFLAGS += -DMVP_VULKAN -I$(VULKAN_SDK)/include
endif
endif
GLSLC    := $(shell command -v glslc 2>/dev/null)
VK_INC   := $(addsuffix .inc,$(wildcard runtime/src/vk/*.vert runtime/src/vk/*.frag))
ifneq ($(GLSLC),)
runtime/src/vk/%.inc: runtime/src/vk/% runtime/src/vk/common.glsl
	$(GLSLC) --target-env=vulkan1.1 -mfmt=c -Iruntime/src/vk $< -o $@
$(BUILD)/runtime/src/vulkan.o: $(VK_INC)
endif

# The C++ in generated/ is made from the game's executable and the tables in tools/recomp/. It is made again when
# one of those changes (files that come out the same keep their date, so only what changed is compiled again).
all:
	@$(MAKE) --no-print-directory generated/.stamp
	@$(MAKE) --no-print-directory $(BUILD)/mvp2005

generated/.stamp: tools/recomp/ee_recomp.py $(wildcard tools/recomp/*.txt)
	python3 tools/recomp/ee_recomp.py
	@touch $@

generated/functable.cpp:
	python3 tools/recomp/ee_recomp.py

RT_OPT   ?= -O2
$(BUILD)/runtime/%.o: runtime/%.cpp runtime/include/ee_ops.h runtime/include/ee_context.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(RT_OPT) -MMD -MP -c $< -o $@

$(BUILD)/runtime/%.o: runtime/%.mm runtime/include/ee_context.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(RT_OPT) -fobjc-arc -MMD -MP -c $< -o $@

# precompiled VU1 blocks (hot inner loops): full optimisation
# (strict aliasing: the VU1 core keeps to it - memcpy for every pun - and the blocks run 8 % faster when the
#  compiler may keep the cycle counter and the flag state in registers across the float stores)
$(BUILD)/generated/vu1_blocks_%.o: generated/vu1_blocks_%.cpp runtime/include/vu1_core.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(RT_OPT) -fstrict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.cpp runtime/include/ee_ops.h runtime/include/ee_context.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/mvp2005: $(OBJ)
	$(CXX) $(OBJ) -o $@.tmp -lpthread $(SDL_LIBS) $(GFX_LIBS) && mv -f $@.tmp $@   # (a running copy keeps its file)

-include $(OBJ:.o=.d)

clean:
	rm -rf $(BUILD)
.PHONY: all clean

test:
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O1 -w -Iruntime/include runtime/tests/test_ops.cpp -o $(BUILD)/test_ops && $(BUILD)/test_ops
.PHONY: test
