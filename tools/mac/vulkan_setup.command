#!/bin/bash
# Installs the Vulkan SDK pieces from Homebrew (MoltenVK = Vulkan on Metal, the loader, headers, shader compiler).
# Output: tools/mac/vulkan_setup.log
cd "$(dirname "$0")/../.."
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
{
  echo "== setup $(date)"
  HOMEBREW_NO_AUTO_UPDATE=1 brew install molten-vk vulkan-loader vulkan-headers shaderc
  echo "== brew exit $?"
  ls /opt/homebrew/lib | grep -i -E "vulkan|molten"
  ls /opt/homebrew/share/vulkan/icd.d /opt/homebrew/etc/vulkan/icd.d 2>/dev/null
  which glslc
  rm -rf tools/mac/vk_include; mkdir -p tools/mac/vk_include; cp -R /opt/homebrew/include/vulkan /opt/homebrew/include/vk_video tools/mac/vk_include/ 2>/dev/null
  echo "== done"
} > tools/mac/vulkan_setup.log 2>&1
exit 0
