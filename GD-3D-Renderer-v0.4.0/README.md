# GD 3D Renderer v0.4.0

A Geode editor tool for importing Wavefront OBJ geometry, previewing a rotating 3D model, and generating Geometry Dash level content that does not need Geode at playback time.

## What changed in v0.4.0

### Default: Baseplate transform rig

The default bake creates one reusable stretched block for each selected mesh edge. A looping chain of ordinary Geometry Dash Move, Rotate, Scale, and Spawn triggers updates those baseplates through 24 projected poses at 12 FPS. The resulting level content is made from normal GD objects and triggers and is intended to keep working with Geode disabled.

This first version is a **sampled wireframe transform rig**, not a general-purpose shader or a full runtime 3D math processor. It projects the OBJ in the Geode editor tool, then stores the transform steps as numeric Move/Rotate/Scale trigger values. Dense meshes are uniformly reduced to at most 64 edge bars; low-poly models (especially the included cube) are recommended.

### Preserved legacy renderer

The previous 32x32 raster flipbook baker is still included. In Geode mod settings, enable **Legacy raster flipbook bake** to switch the `BAKE` button back to the previous renderer. OBJ preview, import, shading-only mode, face colors, and outlines are also preserved.

## Important trigger-math limitation

Geometry Dash Item Edit triggers calculate values into Item IDs, and Item Compare can branch based on those values. However, the current Move, Rotate, and Scale trigger properties accept literal numeric values or group/target-position IDs; they do not expose Item IDs as live X/Y/scale inputs. Therefore v0.4.0 does not claim to pipe a computed Item ID directly into a vertex coordinate. The baseplate rig uses trigger-driven transforms with sampled pose values. A future version can investigate state/lookup-based math networks or other native GD mechanisms without replacing this working fallback.

## Build on Windows

Prerequisites already used by this project:

- Geode CLI 4.x
- Geode SDK 6.0.0 for Geometry Dash 2.2091
- CMake 3.29+
- Ninja
- LLVM/Clang
- Visual Studio C++ workload and Windows SDK
- Git

Open **x64 Native Tools Command Prompt for VS 2026** (or your installed VS version), then:

```bat
set "GEODE_SDK=C:\GeodeSDK"
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d "C:\path\to\GD-3D-Renderer-v0.4.0"
geode build --ninja
```

## Usage

1. Build and install the mod.
2. Open a level in the Geometry Dash editor.
3. Press the **3D** editor button.
4. Use **IMPORT OBJ** to load a model, or use the built-in cube.
5. Press **BAKE 3D RIG** (default) to generate baseplates and transform triggers.
6. Playtest from a position that can see the generated model. The baseplate rig is added at the center of the current editor view.
7. To use the older raster output, enable **Legacy raster flipbook bake** in the mod's Geode settings and bake again.

The default rig is wireframe only in this first version. It uses a limited set of edge bars rather than filled faces, and dense OBJ models are simplified to 64 bars. MTL materials, UV textures, filled polygon surfaces, and fully arbitrary Item-ID-driven coordinates are not included yet.
