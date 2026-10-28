# Extra rules for building the Metal and Vulkan backends on this Mac with the project's existing Makefile:
#   make -f Makefile -f tools/mac/gfx.mk build/mvp2005
# (the same rules are in the updated Makefile delivered with the renderer work)
ifeq ($(GFX_LIBS),)
MM_SRC   := $(wildcard runtime/src/*.mm)
MM_OBJ   := $(patsubst %.mm,$(BUILD)/%.o,$(MM_SRC))
SDL_LIBS += -framework Metal -framework QuartzCore -framework Foundation
OBJ      += $(MM_OBJ)
$(BUILD)/mvp2005: $(MM_OBJ)
$(BUILD)/runtime/%.o: runtime/%.mm runtime/include/ee_context.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(RT_OPT) -fobjc-arc -MMD -MP -c $< -o $@
# Vulkan (Homebrew: molten-vk vulkan-loader vulkan-headers shaderc)
ifneq ($(wildcard /opt/homebrew/include/vulkan/vulkan.h),)
CXXFLAGS += -DMVP_VULKAN -I/opt/homebrew/include
VK_SHADERS := $(wildcard runtime/src/vk/*.vert runtime/src/vk/*.frag)
VK_INC   := $(addsuffix .inc,$(VK_SHADERS))
runtime/src/vk/%.inc: runtime/src/vk/% runtime/src/vk/common.glsl
	/opt/homebrew/bin/glslc --target-env=vulkan1.1 -mfmt=c -Iruntime/src/vk $< -o $@
$(BUILD)/runtime/src/vulkan.o: $(VK_INC)
endif
endif
