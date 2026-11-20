# AGENTS.md — Droplet DX11 Graphics Demo

Guide for AI coding agents making future visual, physics, or performance tweaks to this project.

---

## 1. Project Overview & Constraints

- **Stack**: Standalone **C++17 + DirectX 11 (HLSL Shader Model 5.0)** Win32 application.
- **Zero External Dependencies**: Uses only Windows SDK (`d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`). All geometry, textures, and lighting are procedurally generated on the CPU/GPU — do not introduce external asset files or third-party package dependencies unless explicitly requested.
- **Target Hardware & Performance Budget**: Must run smoothly at **≥ 30 FPS at 2560×1440** on an **AMD Radeon RX 5700 XT** (current baseline: **66–79 FPS GPU-synced** in CLI benchmark mode, higher in live interactive flip-model playback).

---

## 2. Build, Run & Verification Workflow

### Build (MSVC x64 Release)
```bat
cmd /c build.bat
```
- Compiles `src\main.cpp` and `src\renderer.cpp` into `build\DropletDemo.exe`.
- **Hot-reload note**: HLSL shaders in `shaders\` are compiled at runtime via `D3DCompileFromFile`. Shader-only edits do **not** require running `build.bat` — just re-run `DropletDemo.exe`.

### Headless Visual & Performance Verification
Always visually inspect rendered output after shader, lighting, or geometry changes:
```bat
.\build\DropletDemo.exe --width 2560 --height 1440 --frames 120 --preset 1 --screenshot out_hero.bmp
.\build\DropletDemo.exe --width 2560 --height 1440 --frames 95  --preset 2 --screenshot out_macro.bmp
.\build\DropletDemo.exe --width 2560 --height 1440 --frames 195 --preset 3 --screenshot out_pool.bmp
.\build\DropletDemo.exe --width 2560 --height 1440 --frames 90  --preset 5 --screenshot out_wide.bmp
```
- Frame counts are deterministic (`dt = 1/60s`) when `--screenshot` is passed:
  - `frames 95` = plump pendant droplet clinging to branch tip
  - `frames 120` = droplet necking / pinching off
  - `frames 195` = post-impact ripples, underwater caustics & bouncing micro-droplet
- `--frames` / `--screenshot` runs call `Renderer::waitForGPU()` each frame (`D3D11_QUERY_EVENT`) so reported FPS reflects **true GPU throughput** (not hidden-window `DXGI_STATUS_OCCLUDED` CPU submission speed).

---

## 3. Codebase Map

| File | Responsibility |
| :--- | :--- |
| `build.bat` | Standalone MSVC x64 `/O2 /std:c++17` build script |
| `src/main.cpp` | Win32 window, orbit/zoom camera & presets `1`–`5`, keyboard toggles, CLI flags, wall-clock FPS telemetry |
| `src/renderer.h` / `.cpp` | DX11 device/swapchain (`FLIP_DISCARD` + `ALLOW_TEARING`), render targets, 10-pass pipeline, droplet lifecycle physics, BMP export |
| `src/geometry.h` | Procedural Bezier branch + rounded tip, 10 backlit leaves, icosphere, `evalTerrainHeight(x,z)`, pool bed/water, distant trunks, dust motes, GPU-instanced 3D grass blades (`42,000`) & 3D pinnate fern fronds (`~1,300`) |
| `src/grass_texture.h` | Startup generator for seamless 1024×1024 PBR woodland grass Albedo+AO and Sobel Normal+Roughness maps (mipmapped, 16× anisotropic) |
| `src/math_utils.h` | `Vec2/3/4`, `Mat4`, `smoothstep`, `hash`, `fbm2D` |
| `shaders/common.hlsli` | Shared `FrameCB` (`b0`) & `DropletCB` (`b1`), 3D noise/fBm, GGX PBR, 16-tap PCF shadows, ACES tonemap, `evalForestSky` |
| `shaders/branch_tess.hlsl` | Hardware tessellated branch (3D world-space bark/moss displacement) & distant tree trunk silhouettes (`extra.w >= 8.0`) |
| `shaders/droplet_tess.hlsl` | Hardware tessellated droplet (pendant meniscus, neck pinch, falling oscillation) + dual-interface RGB chromatic dispersion refraction |
| `shaders/leaf.hlsl` | Backlit subsurface-scattering emerald forest leaves |
| `shaders/water_sim.hlsl` | 256×256 Compute Shader 9-point isotropic Laplacian wave sim & Jacobian RGB caustic map |
| `shaders/pool.hlsl` | Dual-scale anti-tiled PBR grass bank + caustic-lit riverbed (`PSPoolBed`) & refractive Fresnel pool water (`PSPoolWater`) |
| `shaders/foliage.hlsl` | GPU-instanced 3D curved grass blades & 3D pinnate sword ferns with coherent wind field, backlit SSS, and fern shadow casting |
| `shaders/particles.hlsl` | Sunlit floating woodland dust motes |
| `shaders/sky_volumetric.hlsl` | Moody golden-hour forest sky & half-res raymarched volumetric god rays |
| `shaders/postprocess.hlsl` | Half-res 28-tap spiral Bokeh DoF, 5-mip Jimenez HDR Bloom, full-res sharp + half-res bokeh composite, ACES tonemap, FXAA, and top-right 5×7 bitmap FPS HUD |

---

## 4. Render Pipeline & Shader Register Conventions

### Shared Bindings
- **Constant Buffers**: `b0` = `FrameCB`, `b1` = `DropletCB`, `b2` = `PostCB` (`WaterSimCB` uses `b0` in compute passes).
- **Samplers**: `s0` = linear clamp (`gLinearClamp`), `s1` = shadow comparison (`gShadowSamp`), `s2` = 16× anisotropic wrap (`gAnisoWrap`).

### Pass Order (`Renderer::renderFrame`)
1. **Shadow Map (2048×2048)**: Branch, leaves, and GPU-instanced 3D ferns (`VSFernShadow`).
2. **Opaque HDR Scene (`m_hdrTex`, R16G16B16A16F)**:
   - `2a` Sky → `2b` Tessellated branch & background trunks → `2c` Leaves → `2d` Pool bed & grass floor (`t0` shadow, `t1` water normal, `t2` caustics, `t4` grass albedo, `t5` grass normal) → `2e` 42k GPU-instanced grass (`VS t0` instances, `PS t1` shadow) → `2f` 3D ferns (`VS t0` instances, `PS t1` shadow).
3. **Scene Copy → Pool Water**: Copies `m_hdrTex` → `m_sceneCopyTex` for screen-space refraction, then renders `PSPoolWater`.
4. **Scene Copy → Tessellated Droplets**: Copies `m_hdrTex` → `m_sceneCopyTex`, then renders two-pass back-face thickness (`m_dropletBackTex`) + front-face chromatic refraction for pendant, falling, and splash-jet droplets.
5. **Dust Motes**: Additive/alpha backlit particles.
6. **Half-Res Volumetric God Rays (`m_volTex`)**: 28-step raymarch with shadow map & canopy gobo.
7. **Half-Res Bokeh DoF (`m_dofTex`)**: 28-tap golden-angle spiral bokeh.
8. **5-Mip HDR Bloom Pyramid (`m_bloomTex[0..4]`)**: Prefilter (`m_dofSRV`) → 13-tap Jimenez downsample → 9-tap additive tent upsample.
9. **Full-Res HDR Composite (`m_ldrTex`)**: Blends full-res sharp `m_hdrSRV` (`t0`) with half-res `m_dofSRV` (`t4`) by depth CoC (`t3`), adds `m_volSRV` (`t1`) + `m_bloomSRV[0]` (`t2`), applies ACES tonemap + vignette + gamma.
10. **FXAA + Top-Right FPS HUD → Backbuffer**: Reads `m_ldrSRV` (`t0`), receives `{fps, ms, showFps, 0}` via `PostCB` (`b2`), composites the top-right FPS badge.

---

## 5. Key Invariants & Gotchas

- **Droplet-to-Branch Attachment**: `geo::DRIP_TIP_X/Y/Z` (`-0.02, 1.355, 0.0`) is tightly coupled with the hemispherical branch tip in `geometry.h` and Mode 0 meniscus anchoring in `droplet_tess.hlsl`. If you move the branch, update `DRIP_TIP_*` and verify Preset 2 (Macro).
- **Seam-Free Branch Displacement**: `evalBarkDisplacement` in `branch_tess.hlsl` intentionally uses **3D world-space coordinates** (not cylindrical UVs) to prevent cracking at the UV wrap seam.
- **Terrain Height Single Source of Truth**: Always use `geo::evalTerrainHeight(x, z)` when placing any ground object/foliage so roots sit cleanly in the terrain without floating or poking through `geo::POOL_WATER_Y`.
- **Distant Trunk Normals**: Background trunks in `branch_tess.hlsl` (`i.extra.w >= 8.0`) flip `N` if `dot(N, V) < 0` before computing rim lighting; otherwise `1 - saturate(dot(N, V))` evaluates to `1.0` across the entire trunk and washes them out to beige.
- **Caustic Exposure**: Keep Jacobian caustic values bounded in `water_sim.hlsl` (`focus` clamped, `causticDepth = depth * 0.18`) so HDR bloom (`threshold = 1.10`) never blows out the pool surface to solid white.
- **Do Not Call `waitForGPU()` Interactively**: `Renderer::waitForGPU()` is only for CLI `--frames` / `--screenshot` benchmarking. Calling it in interactive play stalls CPU-GPU pipelining and lowers live FPS.
