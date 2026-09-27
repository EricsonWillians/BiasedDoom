# AGENTS.md

This file provides essential guidance for AI coding agents working with the BiasedDoom codebase. The reader is assumed to know nothing about the project.

## Project Overview

**BiasedDoom** is a modern fork of [GZDoom](https://zdoom.org/) (version 4.15pre) that extends the classic DOOM engine with native **glTF 2.0 support** and first-class **embedded CPython scripting**, enabling skeletal animations, PBR materials, seamless Blender workflows, and full game-logic modding in Python while maintaining full backward compatibility with traditional DOOM assets (MD2, MD3, voxels, DECORATE/ZScript).

Key differentiators:
- Native `.gltf` and `.glb` file loading via `fastgltf`
- Skeletal animation with bone weights and blending
- PBR metallic-roughness rendering under OpenGL/Vulkan
- GPU-skinned animation for performance
- Direct Blender export workflow support
- Embedded CPython API (`src/python/`, opt-in via `-python`) with live actor/sector/line handles, an event bus, savegame persistence, and a `-scripttest` CI mode
- Engine-shipped Python framework packs (`src/python/lib/`): `bd_quests`, `bd_vtm`, `bd_dnd`, `bd_rpg`, `bd_dialogue`, `bd_horror`, `bd_npcs`
- Python-scripted Dear ImGui overlay (`bd.imgui`) rendered on all backends
- Runtime SDL video backend switching (OpenGL/Vulkan/GLES) with backend-owned texture, material, postprocess, 2D shape, and level-geometry resources rebuilt at the frame boundary

The project was previously named "NeoDoom" and was renamed to "BiasedDoom". The executable produced is `biaseddoom`.

**License**: GNU General Public License v3 (or later). Most source files carry a 3-clause BSD-style header for the original GZDoom portions.

## Technology Stack

| Layer | Technology |
|-------|-----------|
| **Language** | C++17 (primary), C (third-party/embed), Objective-C/C++ (macOS) |
| **Build System** | CMake 3.16+ |
| **Dependency Manager** | vcpkg (with manifest in `vcpkg.json`) |
| **Graphics APIs** | OpenGL, Vulkan (via ZVulkan), GLES2 |
| **Audio** | OpenAL (dynamic/static), ZMusic (internal) |
| **Windowing** | SDL2 (Linux/Windows), Cocoa (macOS native) |
| **Scripting** | ZScript (custom VM), DECORATE (legacy), ACS, embedded CPython |
| **Model Formats** | MD2, MD3, IQM, OBJ, KVX (voxels), UE1, **glTF 2.0** |
| **Compression** | bzip2, LZMA, miniz (zip) |
| **Debugging** | cppdap (Debug Adapter Protocol) |
| **Networking** | Custom netcode (`d_net.cpp`) |

**Internal Libraries** (in `libraries/`):
- `ZMusic` — Audio/music playback system
- `ZVulkan` — Vulkan abstraction layer
- `ZWidget` — UI widget system
- `asmjit` — JIT compilation for the script VM
- `discordrpc` — Discord Rich Presence
- `imgui` — Dear ImGui 1.92.8 **docking branch** (MIT) for the engine overlay layer (docking enabled, multi-viewport deliberately off)
- `cppdap` — Debug Adapter Protocol client
- `bzip2`, `lzma`, `miniz`, `webp` — Compression and image formats

## Build System

### Prerequisites

- CMake 3.16 or newer
- C++17 compiler (GCC 9+, Clang 11+, or Visual Studio 2022)
- Git
- vcpkg (bootstrapped automatically by `supreme-build.sh`)
- Platform-specific dependencies:
  - **Linux**: `libsdl2-dev`, `libvpx-dev`, `libwebp-dev`, GTK2/GTK3 dev packages
  - **macOS**: MoltenVK, Vulkan-Volk, libvpx (via Homebrew)
  - **Windows**: Visual Studio 2022 with C++ workload

### Build Commands

**Quick build (using the provided script):**
```bash
./supreme-build.sh
```

**Manual build:**
```bash
# Configure
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release

# For development builds
cmake --build build --config Debug
```

**Clean build:**
```bash
rm -rf build/
cmake -B build -S .
cmake --build build
```

**Build script options (`supreme-build.sh`):**
- `--clean` — Remove build directory before building
- `--release` — Release mode
- `--debug` — Debug mode (default)
- `--relwithdebinfo` — RelWithDebInfo mode
- `--no-gltf` — Disable glTF support
- `--jobs N` — Use N parallel compilation jobs
- `--verbose` — Enable verbose output

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BIASEDDOOM_ENABLE_GLTF` | ON | Enable glTF 2.0 model support |
| `BIASEDDOOM_BUILD_GLTF` | ON | Build experimental glTF implementation |
| `HAVE_VULKAN` | ON | Enable Vulkan support |
| `HAVE_GLES2` | ON (OFF on macOS) | Enable GLES2 support |
| `NO_OPENAL` | OFF | Disable OpenAL sound support |
| `DYN_OPENAL` | ON | Dynamically load OpenAL |
| `OPENAL_SOFT_VCPKG` | OFF | Use OpenAL from vcpkg |
| `LIBVPX_VCPKG` | OFF | Use libvpx from vcpkg |
| `BIASEDDOOM_ENABLE_PYTHON` | ON | Build the embedded CPython scripting runtime when CPython development files are available |
| `BIASEDDOOM_REQUIRE_PYTHON` | OFF | Fail configuration when the requested embedded Python runtime is unavailable |
| `BIASEDDOOM_ENABLE_IMGUI` | ON | Enable the Dear ImGui overlay layer (requires embedded Python) |
| `FORCE_INTERNAL_ZMUSIC` | ON | Use bundled ZMusic |
| `FORCE_INTERNAL_ASMJIT` | ON | Use bundled asmjit |
| `FORCE_INTERNAL_CPPDAP` | ON | Use bundled cppdap |
| `ZDOOM_ENABLE_SWR` | ON | Enable software renderer |
| `WITH_ASAN` | OFF | Enable Address Sanitizer (GCC/Clang) |
| `WITH_MSAN` | OFF | Enable Memory Sanitizer (Clang only) |
| `WITH_UBSAN` | OFF | Enable Undefined Behavior Sanitizer |

### vcpkg Features

Defined in `vcpkg.json`:
- `gltf-support` — Pulls in `fastgltf` for glTF 2.0 loading
- `vcpkg-libvpx` — Use vcpkg-provided libvpx
- `vcpkg-openal-soft` — Use vcpkg-provided OpenAL Soft

## Source Code Organization

The project contains approximately **1,195 source files** (~596 `.cpp`, ~574 `.h`) under `src/`.

### Top-Level Directories

| Directory | Purpose |
|-----------|---------|
| `src/common/` | Shared engine components (rendering, audio, scripting core, filesystem, textures, models, etc.) |
| `src/rendering/` | DOOM-specific rendering code (hardware and software renderers) |
| `src/playsim/` | Game simulation: actors, physics, AI, effects, ACS scripting |
| `src/scripting/` | Scripting engine: ZScript compiler, DECORATE parser, VM backend, codegen |
| `src/python/` | Embedded CPython runtime and game API (`python_runtime`, `python_game_api`, `python_displaylist`, `python_imgui`) plus `lib/` framework packages (`bd_quests`, `bd_vtm`, `bd_dnd`, `bd_rpg`, `bd_dialogue`, `bd_horror`, `bd_npcs`) |
| `src/gamedata/` | Game data definitions: weapons, keys, map info, skills, DEHACKED, textures |
| `src/sound/` | Sound system integration |
| `src/menu/` | Menu system |
| `src/console/` | Console and command system |
| `src/maploader/` | Map loading (UDMF, nodes, polyobjects, slopes) |
| `src/intermission/` | Intermission screens |
| `src/launcher/` | Game launcher UI |
| `src/g_statusbar/` | HUD and status bar |
| `src/posix/` | POSIX-specific code (Linux, macOS) |
| `src/win32/` | Windows-specific code |
| `src/utility/` | Additional utilities including node builder |
| `src/r_data/` | Rendering data: sprites, colormaps, translations, models registry |

### Key Subsystems in `src/common/`

| Directory | Purpose |
|-----------|---------|
| `common/imgui/` | **ImGui overlay layer** (`bd_imgui.cpp/h`, `namespace BdImGui`): per-frame Dear ImGui rendering translated into `F2DDrawer` commands (backend-agnostic), GUI input capture, `py_imgui` master cvar, `py_imgui_demo` CCMD smoke test; dispatches the `imgui_frame` Python event (`bd.on("imgui_frame")`) each visible frame; owns a runtime font registry (TTF/embedded-default fonts, name-addressed, layer-owned source bytes) with deferred atlas rebuilds that upload uniquely named atlas textures, the global UI scale (`set_ui_scale`, FontScaleMain + ScaleAllSizes by ratio), and the default-font push around every `imgui_frame` dispatch |
| `common/models/` | **Model loading system** — MD2, MD3, IQM, OBJ, KVX, UE1, **glTF 2.0** (`model_gltf.cpp/h`, `model_gltf_render.cpp`, `model_gltf_debug.cpp/h`, `model_gltf_helpers.cpp`) |
| `common/rendering/nullvideo/` | **Headless video driver** — `NullVideo`/`NullFrameBuffer` (`null_video.cpp/h`). `-headless` or `BIASEDDOOM_HEADLESS=1` boots the engine with no display/GL/Vulkan (SDL `dummy` video driver); `D_Display` early-outs, `I_IsHeadless()` (declared in `i_video.h`) is the query point. Used for CI without X11/xvfb. |
| `common/rendering/` | Rendering subsystem — OpenGL (`gl/`), GLES (`gles/`), Vulkan (`vulkan/`), hardware renderer (`hwrenderer/`), headless null video driver (`nullvideo/`, used by `-headless` / `BIASEDDOOM_HEADLESS=1` for display-less CI runs), and optional sector-edge light bleed (`bd_sectorlight_*`; low-res world-space map generated by `hw_sectorbleed.cpp`) |
| `common/scripting/` | Scripting VM backend, JIT, frontend parser, DAP integration |
| `common/audio/` | Audio abstractions (sound and music) |
| `common/textures/` | Texture management, material system, PBR materials (`hw_material_pbr.cpp/h`) |
| `common/filesystem/` | WAD/PK3 virtual filesystem |
| `common/platform/` | Platform abstraction — `win32/`, `posix/sdl/`, `posix/cocoa/`, `posix/osx/`, `posix/unix/` |
| `common/engine/` | Core engine utilities (CVars, scanner, random, etc.) |
| `common/utility/` | General utilities: `TArray`, `FString`, vectors, matrices, memory allocators |
| `common/thirdparty/` | Embedded third-party code (animlib, earcut, libsmackerdec, math libs, rapidjson, utf8proc) |

### glTF-Specific Files

- `src/common/models/model_gltf.h` / `model_gltf.cpp` — Core glTF model class (`FGLTFModel`)
- `src/common/models/model_gltf_render.cpp` — glTF rendering integration
- `src/common/models/model_gltf_debug.cpp/h` — Debug visualization helpers
- `src/common/models/model_gltf_helpers.cpp` — Utility functions for glTF processing
- `src/common/rendering/hw_material_pbr.cpp/h` — PBR material system for metallic-roughness workflow
- `src/playsim/gltf_zscript.cpp` — ZScript native bindings for glTF animation control

## Code Style Guidelines

### Header Guards
- **Prefer `#pragma once`** for include guards. This is the dominant convention in the codebase (~193 files use `#pragma once` vs ~99 using `#ifndef` guards).
- Legacy files may still use `#ifndef __FILENAME__` style guards.

### Naming Conventions
- Classes: `F` prefix for engine classes (e.g., `FModel`, `FString`, `FGameTexture`)
- Structs: Often plain names or `S` prefix
- Global functions: Often `I_` for system interface, `P_` for playsim, `R_` for rendering
- Member variables: No strict prefix, but often descriptive names
- Constants: `ALL_CAPS` or `kCamelCase`
- Enums: Often plain names or `E` prefix

### Containers and Strings
- Use `TArray<T>` (custom dynamic array from `tarray.h`) instead of `std::vector`
- Use `FString` (custom string class from `zstring.h`) instead of `std::string`
- Use `TMap<K,V>` for hash maps
- Use `TDeletingArray<T*>` for arrays that own their elements

### Memory Management
- The engine uses a custom memory allocator (`M_Malloc`, `M_Free` in `m_alloc.h`)
- Many objects are garbage-collected via the `DObject` hierarchy
- Use `new`/`delete` for non-GC objects; be careful with ownership

### Math Types
- Vectors: `DVector2`, `DVector3`, `FVector2`, `FVector3`, `FVector4`
- Matrices: `VSMatrix`
- Rotations: `DRotator`
- Quaternions: `FQuat`
- Fixed-point: `fixed_t` (legacy DOOM)

### Include Style
- Use quoted includes for project headers: `#include "actor.h"`
- Use angle brackets for system/standard headers: `#include <math.h>`
- Include paths are relative to `src/` due to CMake `include_directories`

### File Organization
- One major class per file (generally)
- Header and source file names match (e.g., `model_gltf.h` / `model_gltf.cpp`)
- Platform-specific code is segregated into `common/platform/` subdirectories

## Testing Strategy

**There is no traditional unit test suite in this project.** Testing is primarily integration-based:

1. **CI/CD Builds** — GitHub Actions (`.github/workflows/continuous_integration.yml`) builds on every push and PR:
   - **Windows**: Visual Studio 2022 (Release, Debug)
   - **macOS**: macOS-14 with Xcode (Release, Debug), requires MoltenVK and Vulkan-Volk
   - **Linux**: Ubuntu-22.04 with GCC 9/12/latest and Clang 11/15/latest (multiple build types)
   - AppImage generation on Ubuntu 22.04 for distribution, with `tools/check-appimage-deps.sh` verifying that bundled dependencies do not silently resolve from the build host and `tools/smoke-appimage.sh` checking startup in clean Ubuntu 22.04/20.04 containers

2. **Manual Testing** — The engine is tested by running it with various WAD files and verifying:
   - Map loading and gameplay
   - Model rendering (glTF, MD2, MD3, etc.)
   - Script execution (ZScript, ACS)
   - Audio playback
   - Renderer correctness (OpenGL/Vulkan/software)

3. **Build Verification** — The `supreme-build.sh` script verifies the executable is produced and checks for glTF symbols via `nm`.

4. **Python Scripting CI Mode** — `tools/test-python-scripting.sh --iwad PATH` runs the engine with `-python` against the `examples/python/hello_world` fixture and asserts `PYTEST` log markers over lifecycle, events, and savegames. Scripts can also self-test with `bd.assert_true`/`bd.warn` under `-scripttest <tics> [ff]` (fast-forward via time scale), with structured JSON failures emitted by `-pyerrorlog <file>`. `tools/test-python-examples.sh` validates and smoke-runs the `examples/python/` suite; it also runs `tools/test-framework-hotfixes.py`, the offline engine-API contract checks for the engine-shipped framework packages. Linux CI and release jobs run those framework contract checks directly.

## Deployment / Distribution

- **Linux**: self-contained AppImage packages are generated in CI from Ubuntu 22.04/Jammy and dependency-validated; manual installation via `cmake --install`
- **Windows**: Portable zip with `.exe` and `.pk3` files
- **macOS**: `.app` bundle
- **PK3 Files**: Built from `wadsrc/`, `wadsrc_bm/`, `wadsrc_lights/`, `wadsrc_extra/`, `wadsrc_widepix/` via CMake `add_pk3()` custom commands

## Important Files for Agents

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Root build configuration |
| `src/CMakeLists.txt` | Source-level build configuration (1,638 lines) |
| `vcpkg.json` | Dependency manifest |
| `src/version.h` | Version and build info (`4.15pre`) |
| `src/doomdef.h` | Core engine definitions and constants |
| `src/d_main.cpp` | Main entry point and game loop |
| `src/common/models/model.h` | Base model class (`FModel`) |
| `src/common/models/model_gltf.h` | glTF model class (`FGLTFModel`) |
| `src/common/rendering/hw_material_pbr.h` | PBR material definitions |
| `src/common/utility/tarray.h` | Dynamic array container |
| `src/common/utility/zstring.h` | String class |
| `src/common/utility/vectors.h` | Vector math |
| `src/common/scripting/vm/vm.h` | Script VM interface |
| `src/playsim/actor.h` | Actor base class |
| `src/gamedata/gi.h` | Game info definitions |
| `docs/scripting/python.md` | Python scripting guide (API v2 contracts, persistence, performance budgets) |
| `docs/scripting/biaseddoom.pyi` | Generated Python type stub (single-sourced via `dumppystub`; regenerable in-engine — do not hand-edit) |
| `src/python/python_runtime.cpp` | Python runtime core: event dispatch, `bd` module, deterministic RNG, `bd.state`, scripttest, error log, plus the 32 generic custom action buttons (`Button_PyAction1..32` / `+pyaction1..32`: per-tic `custom_action` edge scan in `OnWorldPreTick`, and `bd.custom_action_down` / `custom_action_mask` / `set_custom_action` / `input_binding` / `PYACTION_COUNT`) |
| `src/python/lib/` | Engine-shipped Python framework packages (`bd_quests`, `bd_vtm`, `bd_dnd`, `bd_rpg`, `bd_dialogue`, `bd_horror`, `bd_npcs`), staged beside the embedded stdlib by the `stage_python_frameworks` CMake target |
| `src/common/imgui/bd_imgui.cpp` | Dear ImGui overlay layer (`namespace BdImGui`): ImDrawList → `F2DDrawer` translation, GUI input capture, `py_imgui` CVar, runtime font registry + dynamic atlas rebuilds, UI scale, default-font push per `imgui_frame` |
| `src/rendering/hwrenderer/scene/hw_sectorbleed.cpp` | Builds/refreshes the low-res sector-light blend texture (`bd_sectorlight_bleed`, `_distance`, `_strength`), using mode-independent sector colors, nearest-boundary falloff, sky/fog/portal and 3D-floor-control exclusions, and backend-switch re-uploads |
| `wadsrc/static/zscript/engine/ui/menu/presetmenu.zs` | Searchable Graphics/Lighting/Fog preset picker classes used by the "Browse ... Presets" MENUDEF rows; preset OptionValue IDs are append-only because they persist in user INI files. Graphics presets own only the image pipeline (postfx/bloom/CRT/colorgrade/tonemap/atmosphere/exposure/quality toggles) and auto-pair a named lighting + fog preset via the `GGraphicsPresetPairing` table in `hw_postprocess_cvars.cpp` by moving the `bd_lighting_preset`/`bd_fog_preset` selectors themselves; a user-selected ("explicit") lighting/fog preset is never overridden by graphics presets (tracked by `GAutoPairedLighting`/`GAutoPairedFog`), and `KeepPresetPlayable` clamps postfx only |
| `src/python/lib/bd_quests/` | Engine-shipped Python framework package (quest/journal system with reward-hook dispatch: a quest's `rewards["xp"]` fires the log's `on_xp_reward` callbacks and `rewards["disposition"]` fires `on_disposition_reward`), staged next to the embedded stdlib by the CMake block in `src/CMakeLists.txt` (search "engine-shipped Python packages") so mods can `import bd_quests` |
| `src/python/lib/bd_dnd/` | Engine-shipped Python framework package (D&D-style d20 rules: dice, checks, `Character` XP/HP/resources, kill XP, world helpers, `Party`/`Companion` (fit-checked spawn/teleport ring with optional probed `anchor=`, stuck followers teleport early, proactive combat: engages what hurts the player, what the player hurts, what hurts it, and the nearest visible hostile while combat is recent), plus the classes layer: `CharacterClass` definitions with per-level features and ASI points, `bind_class` progression, the `CreationWizard` character-creation model, and use-based skill mastery), staged the same way so mods can `import bd_dnd` |
| `src/python/lib/bd_vtm/` | Engine-shipped Python framework package (VtM-inspired chronicle rules: blood pool, hunger/frenzy, humanity, disciplines, feeding, masquerade, factions, `VtMState` persistence, ImGui HUD), staged the same way so mods can `import bd_vtm`. All of `src/python/lib/` re-stages on every build via the always-run `stage_python_frameworks` custom target |
| `src/python/lib/bd_dialogue/` | Engine-shipped Python framework package (branching NPC dialogue trees: `Dialogue`/`Node`/`Choice` with build-time target validation, condition/faction-gate/skill-check choice gating, native player-log writes, real-time `DialogueSession`s, ImGui dialogue window in `ui.py`), staged the same way so mods can `import bd_dialogue` |
| `src/python/lib/bd_horror/` | Engine-shipped Python framework package (horror UX layer: `theme.py` ImGui skin + widget helpers, `toasts.py` diegetic notification queue, `atmosphere.py` with `Dread` meter, tag-based `LightManager` candle/fluorescent/blackout programs, `StalkerDirector`, and `HorrorState` bd.state persistence), staged the same way so mods can `import bd_horror` |
| `src/python/lib/bd_rpg/` | Engine-shipped Python framework package (elemental combat: `DamageTypes` registry, per-actor/class affinities in `bd.actor_data`, `resolve_attack` pipeline layered under the `actor_before_damage` filter, `StatusEngine` timed effects on one consolidated task, `LootTable`/`LootRules` with rarity feedback, kill-XP glue, `RpgState` persistence), staged the same way so mods can `import bd_rpg` |
| `src/python/lib/bd_npcs/` | Engine-shipped Python framework package (NPC hub layer: `NPCDefinition`/`NPCManager` registered world NPCs with savegame/hub TID rebind via `spawn_all(from_savegame, from_hub)`, auto-allocated stable nonzero TIDs for `tid_base`-less NPCs, occupied-TID warn-and-advance, `NPCManager.retire` for taking an NPC off duty (recruit-into-follower) with the retired set persisted, per-NPC `Disposition` standings persisted via `bd.state`, nearest-NPC talk targeting through `bd_dialogue` sessions with disposition ctx injection and stale-session recovery, `Service`/`HealerService`/`TrainerService` offers, and a `Shop` with restock timers plus a guarded ImGui `ShopUI`), staged the same way so mods can `import bd_npcs` |
| `examples/python/33_rpg_campaign/` | Capstone Python example ("Ashvale Crossing"): a four-module mini-RPG hub demonstrating every shipped framework pack: `bd_dnd` `CreationWizard`/`CharacterClass` creation, kill XP from real Doom kills with level-toughened blows, a working class active per class on Custom Action 3 (Wren's heal service doubles as the breather that restores charges), `bd_npcs` dispositions/shop/services (`NPCManager.retire` turns the recruitable Korr into his follower, spawned at his own probed slot via the Companion `anchor=`), `bd_dialogue` trees, `bd_quests` with in-world giver markers/objective beacons and a persistent objective HUD strip, and a checkpoint round-trip, all under a headless autotest |
| `examples/python/34_scripted_menus/` | ImGui capstone example ("Overture Menu Kit"): a keyboard-first menu suite (title/pause menus, settings, credits, popups, docked tool panel) scripted entirely in Python on the extended `bd.imgui` API: runtime fonts from `bd.read_bytes`, `set_ui_scale`/`style_theme`/`set_style_color`, `is_key_pressed`/`shortcut` hotkeys, popup/focus management, and `bd.state` settings persistence, all under a headless autotest |
| `tools/test-framework-hotfixes.py` | Offline regression checks (IWAD-free `biaseddoom` stub) for engine-API contracts used by `bd_rpg`, `bd_vtm`, `bd_npcs`, `bd_dialogue`, and `bd_horror`: actor-handle identity, map-local scheduling, shop delivery/refund, dialogue map-unload cleanup, stale-session talk recovery, hub/savegame TID adoption with auto-allocated free TIDs, and hub-restore light-program state |
| `tools/check-appimage-deps.sh` | AppImage dependency-closure gate: extraction, AppRun/libc sanity, and host-resolution checks for every bundled ELF object |
| `tools/smoke-appimage.sh` | Clean-container AppImage startup smoke test used with Ubuntu 22.04 and 20.04 images |
| `supreme-build.sh` | Automated build script with vcpkg bootstrapping |
| `CLAUDE.md` | Additional AI assistant guidance (includes glTF implementation architecture) |

## Security Considerations

- The engine loads user-provided WAD/PK3 files and now also `.gltf`/`.glb` files. Any file parsing code is a potential attack surface.
- The scripting VM (ZScript) executes user-provided code. The VM has sandboxing but native function bindings should be reviewed carefully.
- Network code (`d_net.cpp`) handles multiplayer; buffer sizes and protocol parsing should be validated.
- The project uses `stricmp`/`strnicmp` macros mapped to `strcasecmp`/`strncasecmp` on POSIX systems.
- See `SECURITY.md` for vulnerability reporting (references upstream GZDoom security policy).

## Platform-Specific Notes

**Windows**:
- Static linking with MSVC runtime (`/MT`)
- Prebuilt libvpx in `bin/Windows/vpx/`
- Uses Win32 APIs for input, windowing, and crash handling

**macOS**:
- Can use native Cocoa backend (`OSX_COCOA_BACKEND=ON`) or SDL2
- Requires MoltenVK and Vulkan-Volk for Vulkan support
- Uses `.mm` files for Objective-C++ integration

**Linux**:
- SDL2 for windowing and input
- GTK2/GTK3 for IWAD picker dialog (can be disabled with `NO_GTK`)
- Position-independent executable (`-fPIE`) enabled by default

## Development Workflow Tips

- Use the `supreme-build.sh` script for the easiest first-time build experience; it bootstraps vcpkg automatically.
- If modifying glTF code, ensure `BIASEDDOOM_ENABLE_GLTF=ON` (default).
- The `FASTMATH_SOURCES` list in `src/CMakeLists.txt` marks files compiled with fast-math flags; be careful with floating-point assumptions in those files.
- Precompiled headers are used for a large set of common source files (`PCH_SOURCES` in `src/CMakeLists.txt`); adding new commonly-included headers may benefit from PCH inclusion.
- Generated files (`xlat_parser.c`, `zcc-parse.c`, `sc_man_scanner.h`) are produced by `lemon` and `re2c` tools during build.
- The `revision_check` CMake target updates `src/gitinfo.h` from git metadata on every build.
