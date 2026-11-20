# Droplet — Real-Time DirectX 11 Water Droplet Graphics Demo

A standalone, procedural **DirectX 11 (C++17 / HLSL Shader Model 5.0)** real-time 3D graphics showcase featuring a photorealistic water droplet forming on a mossy woodland branch, pinching off under surface tension, falling with capillary oscillation, and splashing into a forest pool with GPU compute-shader ripples and underwater caustics.

Zero external textures, models, or third-party libraries are required — all geometry, materials, and lighting are synthesized procedurally on the CPU and GPU.

---

## Visual & Technical Features

- **Hardware DX11 Tessellation (HS/DS)**:
  - **Mossy Branch (`shaders/branch_tess.hlsl`)**: Adaptive distance-based tessellation with 3D world-space ridged bark and velvet moss displacement (seam-free), animated moisture runoff film, crevice AO, and backlit moss sheen.
  - **Water Droplet (`shaders/droplet_tess.hlsl`)**: Smooth Laplace-Young pendant meniscus anchored seamlessly to the branch underside, transitioning through neck-pinch detachment, Rayleigh-Taylor falling capillary oscillation, and splash impact.
- **Physically Based Droplet Optics**:
  - Dual-interface refraction with **RGB chromatic dispersion** (`IOR = 1.326 / 1.333 / 1.344`), Schlick Fresnel reflection, internal golden-hour illumination, and analytical projected lens caustics.
- **GPU Compute Shader Shallow-Water & Caustics (`shaders/water_sim.hlsl`, `shaders/pool.hlsl`)**:
  - Real-time 256×256 2D wave simulation using a **9-point isotropic Laplacian** for clean circular ripples and Gaussian splash impulses.
  - Real-time Jacobian refractive caustic map with RGB spectral dispersion projected onto a procedural pebbled riverbed and refracted through the glassy emerald water surface.
- **Atmospheric Woodland Lighting & Post-Processing**:
  - 2048×2048 directional shadow map with 16-tap rotated Poisson PCF soft shadows.
  - Half-resolution raymarched **volumetric golden-hour god rays** (`shaders/sky_volumetric.hlsl`) with Henyey-Greenstein forward scattering and animated canopy gobo shadows.
  - Backlit subsurface-scattering emerald forest leaves (`shaders/leaf.hlsl`) and sunlit floating dust motes (`shaders/particles.hlsl`).
  - Multi-pass HDR post-processing (`shaders/postprocess.hlsl`): Golden-angle spiral **Bokeh Depth of Field**, 13-tap Jimenez **HDR Bloom**, subtle radial lens chromatic aberration, **ACES Filmic Tone Mapping**, photographic vignette, and **FXAA**.

---

## Verified Performance (2560×1440)

Measured on **AMD Radeon RX 5700 XT** at native **2560×1440** with full per-frame GPU synchronization (`--frames` benchmark mode; live interactive flip-model rendering with GPU pipelining runs even faster):

| Camera Preset | Resolution | Avg FPS (GPU-Synced) | Frame Time |
| :--- | :---: | :---: | :---: |
| Preset 1 — Hero Branch & Droplet | 2560×1440 | **76.7 FPS** | 13.03 ms |
| Preset 2 — Macro Droplet Close-Up | 2560×1440 | **77.2 FPS** | 12.95 ms |
| Preset 3 — Pool Ripples & Caustics | 2560×1440 | **63.9 FPS** | 15.65 ms |

---

## Building & Running

### Requirements
- Windows 10/11 (x64)
- Visual Studio 2019/2022 with C++ Desktop Development workload (`cl.exe`, Windows SDK for `d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`)

### Build
```bat
build.bat
```
Compiles `src\main.cpp` and `src\renderer.cpp` with `/O2 /std:c++17` into `build\DropletDemo.exe`.

### Run
```bat
build\DropletDemo.exe
```

### CLI Options
```bat
build\DropletDemo.exe --width 2560 --height 1440 --frames 120 --preset 1 --screenshot out.bmp
```
- `--width <px>` / `--height <px>`: Window/render resolution (default: `1600×900`, or native fullscreen with `F11`).
- `--preset <1-5>`: Initial camera preset.
- `--frames <N>`: Run for `N` frames, print average FPS & frame time, and exit.
- `--screenshot <path.bmp>`: Save the final rendered frame as a 24-bit BMP.

---

## Interactive Controls

| Input | Action |
| :--- | :--- |
| **Left-Click Drag** | Orbit camera |
| **Mouse Wheel** | Zoom in / out |
| **1 – 5** | Switch camera presets (Hero, Macro, Pool Caustics, Low Canopy, Wide Orbit) |
| **R** | Trigger a fresh droplet cycle immediately |
| **Space** | Pause / resume simulation |
| **W** | Toggle wireframe overlay (inspect live DX11 tessellation) |
| **D** | Toggle Bokeh Depth of Field |
| **V** | Toggle volumetric golden-hour god rays |
| **B** | Toggle HDR Bloom |
| **H** | Toggle top-right FPS / frame-time HUD |
| **F11** | Toggle borderless fullscreen |
| **Esc** | Exit |

---

## Project Structure

```text
droplet/
├── build.bat                  # Standalone MSVC x64 Release build script
├── src/
│   ├── main.cpp               # Win32 window, input, droplet lifecycle & benchmark CLI
│   ├── renderer.h / .cpp      # Full DirectX 11 multi-pass rendering pipeline
│   ├── grass_texture.h        # Seamless 1024x1024 procedural PBR woodland grass textures
│   ├── geometry.h             # Procedural branches, leaves, icosphere, pool bed & trunks
│   └── math_utils.h           # Vector/matrix math, noise & camera helpers
└── shaders/
    ├── common.hlsli           # Shared CBuffers, PBR, 3D fBm noise, PCF shadows, ACES, sky
    ├── branch_tess.hlsl       # Hardware tessellated bark/moss branch & distant trunks
    ├── droplet_tess.hlsl      # Hardware tessellated droplet deformation & dispersion optics
    ├── leaf.hlsl              # Backlit subsurface-scattering emerald leaves
    ├── water_sim.hlsl         # Compute shader 2D wave simulation & caustic map generation
    ├── pool.hlsl              # Caustic-lit pebbled pool bed & refractive water surface
    ├── foliage.hlsl           # GPU-instanced 3D wind-swaying grass blades & pinnate ferns
    ├── particles.hlsl         # Floating golden-hour woodland dust motes
    ├── sky_volumetric.hlsl    # Atmospheric forest sky & raymarched volumetric god rays
    └── postprocess.hlsl       # Bokeh DoF, HDR Bloom, ACES Tonemapping & FXAA
```
