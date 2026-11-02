# Architecture Decision Records

## ADR-001: C++23 + CMake + SDL3 GPU (Metal) for platform and rendering

- **Status:** accepted (2026-11-02)
- **Context:** We need efficient voxel rendering on the dev machine (Apple M4, macOS 15, Command Line
  Tools only, no full Xcode). We also need a path to Windows and Linux, where most RPG/MMO players
  are.
- **Decision:**
  - Language and build: C++23 with CMake ≥ 3.28, Ninja and AppleClang.
  - Window, input and GPU: SDL3, using its GPU API. That API is a thin explicit layer (command
    buffers, render passes, storage buffers, MSAA) that runs on Metal here and on Vulkan/D3D12
    elsewhere.
  - Math: glm. Noise: FastNoiseLite.
- **Alternatives considered:**
  - Raw Metal (metal-cpp): fastest on Mac, but locks out Windows.
  - Vulkan + MoltenVK: about 3× the boilerplate, adds a translation layer on Mac, and MoltenVK has
    no mesh shaders.
  - WebGPU (Dawn / wgpu-native): clean API, but not installed; Dawn is a heavy build and
    wgpu-native means vendoring prebuilt binaries. Remains the fallback.
  - OpenGL: deprecated on macOS and capped at 4.1.
  - An engine (Godot, Unreal, Bevy): the goal is C++ with full control of the voxel pipeline.
- **Consequences:** the GPU API surface is small and explicit. SDL3 also covers windowing, input and
  audio later, so there is one system dependency (`brew install sdl3`).

## ADR-002: Runtime-compiled MSL shaders (for now)

- **Status:** accepted (2026-11-02)
- **Context:** without full Xcode there is no offline `metal` compiler. glslc and spirv-cross are not
  installed either.
- **Decision:** write shaders in MSL (`shaders/world.metal`) and let SDL compile them at load via
  `SDL_GPU_SHADERFORMAT_MSL`. Shader creation failure is logged and the app exits with code 1.
- **Consequences:** zero shader tooling, and compilation takes milliseconds. Shader errors only show
  up at runtime; the `--frames` smoke run catches them. The shaders are macOS-only.
- **Follow-up:** for Windows/Linux (M1), port to HLSL or Slang and compile to SPIR-V/DXIL/MSL at
  build time with SDL_shadercross.

## ADR-003: Greedy meshing + 8-byte packed quads + vertex pulling

- **Status:** accepted (2026-11-02)
- **Context:** voxel worlds are bandwidth- and vertex-bound. A classic float vertex costs about 32
  bytes × 4 vertices per face.
- **Decision:**
  - Greedy-merge faces that share block id and all 4 AO values.
  - Store each merged face as 8 bytes in one storage buffer for the whole world.
  - Draw with `SDL_DrawGPUPrimitives(first_vertex = 0)`, pass the draw's first quad index in its
    uniforms, and rebuild corners in the vertex shader from `vertex_id`. SDL_gpu.h states that a
    non-zero `first_vertex` combined with `vertex_id` is not portable across backends.
- **Alternatives considered:**
  - Per-face instancing: same idea, but more API surface.
  - Mesh shaders: unavailable through SDL GPU, and they would need a compute pipeline.
  - Binary greedy meshing (bitmask): faster meshing, but more complex. We'll revisit when streaming
    needs remeshing budgets.
- **Consequences:** about 16× less vertex data than float vertices, ~1.8M quads = 14 MB for the
  whole world, and frame times around 1 ms. Region size is capped at 64 per axis (6-bit fields);
  chunks use 32.

## ADR-004: Render to an owned resolve target, then blit to the swapchain

- **Status:** accepted (2026-11-02)
- **Context:** the swapchain texture cannot be read back, but we want screenshots for automated
  visual verification.
- **Decision:** resolve MSAA into our own texture, blit it to the swapchain, and download it on
  request (`--screenshot`).
- **Consequences:** one extra full-screen blit per frame (negligible on M4). Screenshots work
  headlessly in CI-like runs.

## ADR-005: Header-only dependencies through FetchContent, without their CMake

- **Status:** accepted (2026-11-02)
- **Decision:**
  - glm 1.0.3 and FastNoiseLite v1.1.1 are fetched as pinned release tarballs.
  - `SOURCE_SUBDIR` points to a missing directory, so only the headers are used. Their CMake
    projects, tests and install rules never run.
  - Both are marked `SYSTEM`, so their warnings don't mix with ours.
- **Consequences:** the first configure needs network access. There are no extra system packages.

## ADR-006: Dear ImGui for the debug UI

- **Status:** accepted (2026-11-02)
- **Context:** we need an in-game debug window with tabs, sliders, checkboxes, text input and
  graphs for tuning movement, inspecting meshes and controlling time and world generation.
- **Decision:** Dear ImGui v1.93.0, using its SDL3 platform backend and SDL_GPU renderer backend.
  It's compiled from a pinned, hash-checked tarball as a separate static library, so our warning
  flags don't apply to it. The UI is drawn in a second render pass into the resolve target, before
  the blit.
- **Alternatives considered:**
  - Hand-written UI on our renderer: weeks of work for widgets ImGui already has.
  - Separate tool windows or a web inspector: more moving parts, and no in-world context.
- **Consequences:**
  - ImGui's colors are authored in sRGB. Our swapchain is sRGB with linear-light output, so the
    style colors are converted to linear at startup.
  - Screenshots include the UI when it's open.
  - An ImGui frame runs every frame because the in-game HUD draws into it. The UI render pass is
    skipped when there's nothing to draw.

## ADR-007: Classless WoW-style attributes

- **Status:** accepted (2026-11-02)
- **Context:** combat needs stats, and the game has no classes.
- **Decision:**
  - Five attributes (stamina, strength, agility, intellect, spirit); everything else derives from
    them directly, with no attack power or other in-between step.
  - Stamina gives 10 health per point.
  - Physical damage is `baseDamage + strength * speed`, using the attack's base speed, as WoW
    uses weapon speed.
  - Agility is haste.
  - Spirit regenerates health out of combat.
  - Intellect waits for magic.
- **Consequences:**
  - All formulas and tuning constants live in `src/stats.h`.
  - Equipment can later add attributes and replace the punch's `Attack` without changing the
    formulas.
