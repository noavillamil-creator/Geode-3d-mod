# Changelog

## v0.4.0 — 2026-11-25

### Added
- New default **Baseplate Transform Rig** bake engine.
- Each selected OBJ edge is represented by one reusable stretched Geometry Dash block.
- Added a looping 24-step animation chain using ordinary Move, Rotate, Scale, and Spawn triggers at 12 FPS.
- Added deterministic edge sampling with a 64-bar cap for dense meshes.
- Added a Geode setting to switch back to the previous raster flipbook baker.
- Kept CMake project metadata and build script banner in sync with v0.4.0.
- Increased serialized trigger-delay precision so 12 FPS stages use 0.0833 seconds rather than rounding to 0.08.

### Preserved
- Editor 3D preview and repositioned editor shortcut.
- OBJ vertex/face importer, including polygon triangulation and negative indices.
- Upper-right directional preview shading and optional outlines.
- Previous 32x32 raster flipbook bake as **Legacy raster flipbook bake**.

### Technical note
- The new rig is a trigger-driven sampled wireframe animation, not yet a full runtime Item-ID coordinate solver. Item Edit calculations are stored in Item IDs; standard Move/Rotate/Scale trigger numeric fields do not currently accept those Item IDs directly as dynamic position/scale inputs.

### Known limitations
- Only wireframe baseplate bars are generated; filled polygon surfaces are not included.
- Dense models are reduced to at most 64 sampled mesh edges.
- Transform values are sampled and precomputed when baking; arbitrary camera movement and free-form runtime vertex math are not implemented yet.
- MTL materials, UV maps, and textures remain unsupported.

## v0.2.5
- Kept the original OBJ preview and raster flipbook renderer.
- Added persistent legacy face-color and outline preferences.
- Repositioned the editor shortcut.
