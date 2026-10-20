# Assimp dependency

- Upstream: [assimp/assimp v6.0.3](https://github.com/assimp/assimp/releases/tag/v6.0.3).
- Source archive: `https://codeload.github.com/assimp/assimp/tar.gz/refs/tags/v6.0.3`.
- SHA-256: `9be912589023c7d5a6f2b1db8858b689ce815d5eacf0fea82f869708479b1e51`.
- License: upstream BSD 3-Clause, reproduced in `LICENSE`; retained contributions keep their upstream license files and source notices.
- CMake minimum: 3.22. The project uses C++20; this dependency builds as C++17.
- Upstream version discrepancy: the v6.0.3 archive's `PROJECT(Assimp VERSION 6.0.2)` and `aiGetVersion*()` still report 6.0.2.
  The release is identified by its tag and archive hash above; the project does not rewrite the upstream version.

The source is vendored directly in this directory and added with `add_subdirectory(assimp)`.
Configuration does not download Assimp or use FetchContent; generated headers and compiled output stay under
`build/3rdparty/assimp`, outside Git. Retained files, including both upstream CMake build scripts, are unchanged
from the archive identified above. Project options are set in `3rdparty/CMakeLists.txt`; no custom implementation
list or reduced postprocessing registry is maintained.
For a build tree created with the former FetchContent integration, configure once with
`cmake -S . -B build -U "ASSIMP_*_OUTPUT_DIRECTORY"` to clear its old output paths. A fresh build tree needs no
migration. Old files under `build/_deps/assimp-*` are unused by the local integration.

## Source subset

- Keep complete upstream `include/`, `cmake-modules/`, common code and postprocessing, plus root `CMakeLists.txt`,
  `assimp.pc.in`, `LICENSE` and `CREDITS`. Optional postprocessing is not disabled according to ModelLoader's
  current call sites, and upstream build scripts are not replaced with a reduced implementation list.
- Under `code/AssetLib/`, keep the enabled OBJ and GLTF groups (`Obj`, `glTF`, `glTF2`, `glTFCommon`). Remove
  other format directories except STEPParser, `Step/STEPFile.h`, FBX headers and `FBXUtil.cpp`: these are
  required by common sources compiled unconditionally by upstream CMake. STEP and FBX importers stay disabled.
  The disabled PBRT exporter's separate `code/Pbrt/` directory is also omitted.
- Keep the bundled libraries used by the enabled formats or compiled unconditionally by upstream CMake:
  earcut, clipper, Open3DGC, OpenDDL parser, poly2tri, pugixml, RapidJSON, stb_image, unzip, utf8cpp and zlib.
  Retain their source/build inputs and license notices; remove unrelated Draco, tinyusdz, meshlab, zip,
  android-cmake and googletest contributions.
- Omit upstream tests/model fixtures, documentation, samples, tools, fuzzing, ports/bindings, packaging,
  maintenance scripts and CI/development metadata. Remove contrib/googletest and nested contribution
  tests/docs/examples, test translation units and explanatory Markdown/reStructuredText documents.
- Omit zlib's pregenerated `zconf.h` so configuration generates it in the build tree without renaming a source
  file. Keep zlib's `README` because it carries its license, and keep all contribution license documents.

Omitted formats, contributions and auxiliary targets (tests, documentation, tools and samples) must stay disabled;
enabling them later requires restoring their upstream files. The current OBJ/GLTF options and complete
postprocessing implementation remain unchanged.
The archive checksum identifies the original release, not a checksum of this source subset.

Only OBJ and GLTF importer groups are enabled, matching the original project configuration. ModelLoader's public
input remains `.obj` and static `.gltf` 2.0; the GLTF group's other upstream formats do not expand that contract.
Export, tools, samples, upstream tests, documentation, install and Draco are disabled. Bundled zlib is built;
library output is static. `USE_STATIC_CRT=OFF` keeps the MSVC dynamic CRT (Debug `/MDd`, Release `/MD`), and
debug postfix injection is disabled. Third-party warnings-as-errors are disabled within Assimp only.

Runtime links `assimp::assimp` privately. Source-input checks use the release's bundled RapidJSON privately;
Resource public headers expose neither dependency. The private IO adapter test links Assimp to exercise its actual ABI.

## Fixed-version conversion details

- glTF's importer flips V into Assimp's bottom-left UV convention; ModelLoader converts both importer outputs with `(u, 1-v)`.
- glTF material properties retain base color, base UV source, UV-transform presence and `AI_MATKEY_GLTF_ALPHAMODE`.
  Base alpha is also copied into generic opacity, so generic opacity is deliberately not used for glTF OPAQUE rejection.
- OBJ `d` and `Tr` assign the same opacity field in input order; the last encountered declaration wins, with `Tr` mapped to `1-Tr`.
- OBJ inserts gray 0.6 when Kd is absent and discards map_Kd UV options/repeated declarations. A narrow MTL source check records
  Kd presence and rejects unsupported map declarations; Assimp still parses all values and paths.
- glTF compacts used vertices and may drop bad faces or triangle remainders. Source POSITION counts are checked before import;
  a scoped diagnostic observer rejects dropped-face/remainder and UV-stream repair, as well as unknown OBJ material fallback.
  It restores Assimp's logger after the synchronous import and does not emit another engine log.
- A source hierarchy check rejects cycles and multiple parents before Assimp's recursive node cloning. ModelResource validates
  the final owned data independently, without rechecking it during access or frame expansion.
