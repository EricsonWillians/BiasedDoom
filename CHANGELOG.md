# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Changed
- **Project Rename**: The project has been renamed from "NeoDoom" to "BiasedDoom".
  - Executable is now `biaseddoom`.
  - Build scripts updated to produce `biaseddoom` binary.
  - CMake variables and macros updated to `BIASEDDOOM_` prefix.
  - All documentation updated to reflect the new name.
- **SBARINFO Support**: Added custom mugshot scaling and positioning support (from previous commits).

### Added
- **glTF Support**: Complete integration of glTF 2.0 model loading using `fastgltf`.
  - Support for .gltf and .glb files.
  - Initial skeletal animation support.
  - Material rendering fixes.
