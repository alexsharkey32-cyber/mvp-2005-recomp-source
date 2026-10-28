# Renderers

The GPU renderer draws the game's main 3D/UI target (everything the console's GS draws into its main
framebuffer) on the graphics card, at 1x-4x the console's resolution. The software rasterizer (`gs.cpp`) stays
the reference: it owns VRAM, the GS state machine and everything drawn outside the main target, and it is what
the GPU output is compared with on GS replays.

Settings > Graphics > **Graphics API** picks the API (applies at the next start; `MVP_GPU_API=gl|metal|vulkan`
overrides it for one run):

| API | Platforms | Status |
| --- | --- | --- |
| OpenGL 3.3 | Windows, Linux, macOS (4.1) | complete, the reference GPU path |
| Metal | macOS on Apple GPUs (M1 and later) | complete, default on Apple Silicon Macs |
| Vulkan 1.1 | Linux, Windows; macOS through MoltenVK | complete |
| Direct3D 11 | Windows | planned (below) |
| Direct3D 12 | Windows | planned (below) |

If the chosen API can't start (no driver, unsupported GPU) the game falls back to OpenGL, then to SDL's
renderer with the software rasterizer.

## How it is put together

```
gs.cpp (GS state, VRAM, software rasterizer)
   |  gpu_prim(state, vertices)                         one GS primitive
   v
gs_gpu.cpp  - batching shared by every API: sprites -> triangles, vertex layout (17 floats), the sprite
   |          texture clamp, the decisions "needs a target copy" / "destination alpha test mode", and
   |          merging consecutive primitives with the same state into GpuBatch runs
   v
GpuBackend  - one per API: execute(batches), target upload / read-back (native resolution, colour + depth),
              texture create/free, anisotropic filtering, target_handle() for the presenter
   OpenGL: gs_gpu.cpp   Metal: metal.mm   Vulkan: vulkan.cpp

Presenter   - one per API, the window side: game picture -> [FXAA] -> resample (nearest / bilinear / bicubic /
              area) -> sharpen + colour + CRT -> window, then the UI quads (fonts, rounded rects, shadows,
              images, gradients). present_gl.cpp, metal.mm, vulkan.cpp (+ present_sdl.cpp, no post effects)
```

The renderer and presenter of one API share the device/context; the presenter takes the game picture straight
from the renderer's target (`set_game_gpu`), no CPU round trip.

All backends reproduce the same per-pixel maths (the software rasterizer's): integer texture wrap modes
(repeat / clamp / region clamp / region repeat), integer bilinear, TFX modulate/decal/highlight, fog, alpha test
with its four fail modes, 32/24/16-bit Z as `z / 2^32` in a 32-bit float depth buffer with GEQUAL/GREATER.
The GS blend formula `((A - B) * C >> 7) + D`:

* **OpenGL, Vulkan**: dual-source blending covers the common cases in fixed-function hardware; C = destination
  alpha and the wrap-around (no COLCLAMP) cases copy the target under the primitive and blend in the shader.
  The destination alpha test (DATE) is done with the stencil buffer.
* **Metal**: Apple GPUs read the destination pixel in the fragment shader (framebuffer fetch), so blending,
  FBMSK (bit by bit, not only whole bytes) and DATE are exact for every primitive with no copies. One pipeline
  covers every GS state.

Shaders: OpenGL GLSL strings in `gs_gpu.cpp` / `present_gl.cpp`; Metal MSL strings in `metal.mm` (compiled at
start-up); Vulkan GLSL in `runtime/src/vk/`, compiled to SPIR-V by `glslc` into `runtime/src/vk/*.inc`
(checked in, so building needs no shader compiler; the Makefile/CMake remake them when glslc is present).

### Building

* macOS: Metal is always built. Vulkan: `brew install molten-vk vulkan-loader vulkan-headers shaderc`, then
  `make` picks it up.
* Linux: Vulkan when `pkg-config vulkan` finds the loader (`libvulkan-dev`).
* Windows (CMake): Vulkan when the Vulkan SDK is installed (`find_package(Vulkan)`).

## Plan: Direct3D 11 and Direct3D 12

Both slot in as one more `GpuBackend` + `Presenter` pair (`d3d11.cpp`, `d3d12.cpp`), chosen in the same setting
(entries 4 and 5), Windows only. SDL gives the window's HWND (`SDL_GetWindowWMInfo`). Nothing above the backend
interface changes.

### Shaders
Write the shaders once more in HLSL (Shader Model 5.0 for D3D11, 5.1/6.0 for D3D12); they are line-for-line
ports of `runtime/src/vk/*.frag|vert` (same push-constant block -> a constant buffer). Compile at build time
with `fxc` (D3D11, DXBC) / `dxc` (D3D12, DXIL) into headers like the SPIR-V `.inc` files and check them in.
Alternative: generate the HLSL from the Vulkan SPIR-V with SPIRV-Cross to keep one source of truth.

### Direct3D 11 (first: simpler, runs everywhere Windows 7+ with a DX10.1+ GPU)
* Target: `R8G8B8A8_UNORM` render target + `D32_FLOAT_S8X24_UINT` depth-stencil, a copy texture for the
  "target copy" path (`CopySubresourceRegion` on the bounding box, as GL's `glCopyTexSubImage2D`).
* State objects are created on first use and cached by key, like the Vulkan pipelines: blend states
  (dual-source: `D3D11_BLEND_SRC1_COLOR`, ADD / SUBTRACT / REV_SUBTRACT), depth-stencil states (GEQUAL /
  GREATER, write on/off, the three DATE stencil modes), one rasterizer state with scissor on.
* Vertices: one dynamic `ByteAddressBuffer`/structured buffer read with `SV_VertexID` (the flat-shading colour
  comes from the primitive's last vertex, as in the Metal/Vulkan vertex shaders); per-batch constants in a
  `D3D11_MAP_WRITE_DISCARD` constant buffer (or `ID3D11DeviceContext1::VSSetConstantBuffers1` with offsets).
* Read-backs: `CopyResource` to a STAGING texture + `Map` (native-resolution down-sample pass first, as Vulkan).
* Presenter: DXGI flip-model swap chain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`, tearing flag when VSync is off),
  the FXAA / resample / final / UI pixel shaders as full-screen triangles.
* Effort: ~1200 lines, about the size of `vulkan.cpp`.

### Direct3D 12 (after D3D11; maps almost 1:1 onto the Vulkan backend)
* Same structure as `vulkan.cpp`: two frames in flight with a fence each, a per-frame upload ring (vertices,
  staging), descriptor heaps instead of descriptor pools (one shader-visible CBV/SRV heap ring + a sampler
  heap), root signature = root constants for the 36-dword GS block (mirrors the push constants) + a descriptor
  table for the GS texture and the target copy.
* PSOs cached by the same key as the Vulkan pipelines (topology type, blend, write mask, depth, stencil).
* Resource states instead of Vulkan's GENERAL layout: the target moves between RENDER_TARGET, COPY_SOURCE and
  PIXEL_SHADER_RESOURCE with explicit barriers at the same points `vulkan.cpp` puts its full barriers.
* Texture churn: the game replaces textures thousands of times a minute, so textures come from a placed-resource
  pool (one heap per size class) and are recycled once their frame's fence has passed, as the Vulkan image pool.
* Optional later: on GPUs with Rasterizer Ordered Views, do blending / FBMSK / DATE in the shader like Metal
  (exact, no copies), keeping the dual-source path as the fallback.

### Testing
Each new backend is checked the same way the Metal and Vulkan ones were: the in-game menu self-test
(`MVP_GUI_TEST`, `MVP_GUI_SHOT`), window captures through a match / the attract-mode demo
(`MVP_WIN_SHOT=frame:path_%d.png`, `MVP_WIN_SHOT_EVERY`), frame-rate logs (`MVP_FPS=1`), and GS replays
compared pixel by pixel with the OpenGL backend and the software rasterizer.
