# 3D Quick Look

A macOS Quick Look extension that previews 3D model files when you press Space in Finder:

- **3MF** — including multicolor prints from Bambu Studio, OrcaSlicer and PrusaSlicer
- **STEP** (`.step`, `.stp`) — with part and face colors
- **STL** — binary and ASCII
- **OBJ** — with material colors (`.mtl`) and vertex colors
- **glTF** (`.glb`, `.gltf`) — with material colors and vertex colors, including meshopt-compressed files
- **PLY** — binary and ASCII, with vertex and face colors

The preview is interactive: drag to rotate, pinch or use the mouse wheel to zoom, two-finger scroll to pan, double-click to reset the view.

## Install with Homebrew

```bash
brew tap den1k/quicklook3d https://github.com/den1k/3D-quicklook
brew trust den1k/quicklook3d   # recent Homebrew versions ask for this before using a third-party tap
brew install quicklook3d
quicklook3d-setup
```

Homebrew builds the app from source on your Mac (about 20 seconds, using Homebrew's OpenCASCADE for STEP). `quicklook3d-setup` copies it to `~/Applications` and turns the extension on; run it again after an upgrade.

## Multicolor support

For 3MF the loader understands:

- base materials and color groups from the 3MF core spec and materials extension, per object, per triangle and per vertex
- models split across several files (production extension), as Bambu Studio and OrcaSlicer write them
- per-object and per-part filament assignments, using the project's filament colors
- painted triangles (`paint_color` in Bambu/Orca, `slic3rpe:mmu_segmentation` in PrusaSlicer), including the sub-triangle splits the paint tools create
- modifier, negative and support-blocker parts are left out, since they are not part of the printed model

For STEP, colors assigned to assemblies, parts, solids and individual faces are applied, most specific first.

## Speed

Meshes are parsed by purpose-built code rather than general XML/geometry libraries, and large 3MF files are decompressed and parsed in parallel. On an Apple Silicon laptop, a 3MF holding 234 MB of XML (3.4 million triangles) loads in about a quarter of a second.

Mesh data is handed to the GPU in place, without copying, so the first frame of even a multi-million-triangle model appears quickly.

STEP files hold exact surfaces rather than triangles, so they are converted with [OpenCASCADE](https://dev.opencascade.org) and take longer the first time: typically 0.1–2 seconds. The converted mesh is cached, so reopening an unchanged STEP file is instant. The cache lives in the extension's own sandbox container (`~/Library/Containers/com.local.quicklook3d.preview/Data/Library/Caches/meshes`) and is capped at 512 MB, oldest entries removed first.

## Build from source

### Requirements

- macOS 13 or later, Apple Silicon
- Xcode (for `swiftc` and `clang++`)
- `git` and `cmake`, only for STEP support

### Build and install

```bash
# Optional, once: build OpenCASCADE for STEP support (takes 10-20 minutes)
./scripts/build-occt.sh

# Build the app, copy it to ~/Applications and register the extension
./build.sh install
```

Without the first step everything except STEP works.

To check that macOS sees the extension:

```bash
pluginkit -m -p com.apple.quicklook.preview | grep quicklook3d
```

If another app's Quick Look extension claims the same file types, macOS may pick that one. Switch extensions on and off under System Settings → Login Items & Extensions → Quick Look.

## Tests

```bash
./build.sh
python3 tests/run_tests.py
```

The tests generate small STL, OBJ, PLY, glTF and 3MF files (including slicer-style multicolor ones), load them and the STEP and compressed-GLB fixtures in `tests/fixtures`, and check triangle counts, sizes and colors.

Two command-line tools are built alongside the app:

- `build/ql3d-bench <files...>` prints load time, triangle count, size and colors for each file
- `build/ql3d-render <model> <out.png>` renders a model to an image with the same scene the preview shows

## Layout

| Path | Contents |
| --- | --- |
| `Core/` | C++ loaders (`stl.cpp`, `obj.cpp`, `ply.cpp`, `gltf.cpp`, `threemf.cpp`, `step.cpp`) behind a small C interface, `ql3d.h` |
| `Shared/SceneBuilder.swift` | turns a loaded mesh into a SceneKit scene |
| `Extension/` | the Quick Look preview extension |
| `App/` | the host app that carries the extension |
| `Tools/` | benchmark and render tools |

## Notes and limits

- The build is ad-hoc signed, which is enough to run on the Mac that built it. Distributing a built app to other Macs needs a Developer ID signature and notarization.
- The extension is sandboxed but has read-only access to the file system, because a sandboxed previewer otherwise cannot read the `.mtl` file next to an `.obj`.
- OBJ and glTF textures are not shown, only material and vertex colors. A textured glTF material is drawn in the default gray.
- glTF files compressed with Draco are not supported (meshopt compression is), and animations and skinning are ignored.
- PLY point clouds (files without faces) are not supported. PLY has no fixed up axis; Z is assumed to be up, as for STL.
- Pure black parts are drawn as very dark gray so their shape stays visible.
- There is no thumbnail extension; Finder icons are unchanged.
- `Core/meshopt/` holds the decoder from [meshoptimizer](https://github.com/zeux/meshoptimizer) v1.3 (MIT, license included there).
- OpenCASCADE is licensed under the LGPL 2.1 with an additional exception. `scripts/build-occt.sh` links it statically; the Homebrew build links Homebrew's shared libraries.

## License

MIT, see [LICENSE](LICENSE).
