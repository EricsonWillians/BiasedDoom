# BiasedDoom Documentation

Use this page as the front door for project docs. The folder is organized by
audience:

- [glTF modding](gltf/README.md) - Blender assets, player replacement, ZScript, and glTF tools.
- [Heresy Editor](https://github.com/EricsonWillians/heresy-editor) - recommended companion map editor ([website](https://ericsonwillians.github.io/heresy-editor/), [integration guide](https://ericsonwillians.github.io/heresy-editor/BiasedDoom.html)).
- [Development notes](development/README.md) - implementation details, diagnostics, and historical status notes.
- [Engine features](engine/README.md) - non-glTF gameplay/engine feature guides, including rendering presets and sector light bleed.
- [Scripting](scripting/README.md) - embedded Python plus ACS/ZScript interoperability.
- [Audio troubleshooting](audio-troubleshooting.md) - Windows/OpenAL diagnostics, logs, device recovery, and custom OGG checks.
- [Release process](release/README.md) - maintainer release checklist.
- [Root troubleshooting](../TROUBLESHOOTING.md) - startup, IWAD, audio, mods, Python, builds, and reporting.

Root-level markdown is reserved for repository-wide entry points and metadata
such as `README.md`, `TROUBLESHOOTING.md`, `CHANGELOG.md`, `SECURITY.md`, and
`AGENTS.md`. Source-local notes and packaged license files stay beside the code
or assets they describe.

## I Want To...

| Task | Start Here | Then Read |
|------|------------|-----------|
| Make maps for BiasedDoom | [Heresy Editor](https://github.com/EricsonWillians/heresy-editor) | [Integration guide](https://ericsonwillians.github.io/heresy-editor/BiasedDoom.html) |
| Replace an actor with a glTF model | [glTF quick start](gltf/quick-start.md) | [Blender authoring](gltf/blender-authoring.md) |
| Replace the player in third-person | [Player replacement workflow](gltf/player-replacement-workflow.md) | [ZScript usage](gltf/zscript-usage.md) |
| Learn Blender export rules | [Blender authoring](gltf/blender-authoring.md) | [Beginner tutorial](gltf/beginner-tutorial.md) |
| Use the `GLTFModel` mixin | [ZScript usage](gltf/zscript-usage.md) | [ZScript API](gltf/zscript-api.md) |
| Build a trusted Python mod | [Focused example suite](../examples/python/) | [Complete Python guide](scripting/python.md) |
| Find installed Doom IWADs | [IWAD discovery](engine/iwad-discovery.md) | [Engine features](engine/README.md) |
| Tune rendering presets and bloom | [Rendering presets and light bleed](engine/rendering-presets.md) | Root [changelog](../CHANGELOG.md) |
| Set up a gamepad (or fix mod autoaim) | [Gamepad support](engine/gamepad.md) | Root [changelog](../CHANGELOG.md) |
| Generate a mod skeleton | [glTF tools](gltf/tools.md) | [Script robustness notes](development/create-gltf-replacement-script-improvements.md) |
| Understand the implementation | [glTF implementation](development/gltf-implementation.md) | [Implementation status](development/gltf-implementation-status.md) |
| Debug glTF build issues | [Compilation fixes](development/gltf-compilation-fixes.md) | [Robustness notes](development/gltf-robustness-improvements.md) |
| Prepare a release | [Release checklist](release/releasing.md) | Root [README](../README.md) |
| Study the procedural generator | [Research paper](engine/procedural-generation-research-paper.md) | [Feature and usage guide](engine/procedural-map-generation.md) |
| Read the latest release notes | [BiasedDoom 4.15.7](release/4.15.7.md) | Root [changelog](../CHANGELOG.md) |

## Recommended glTF Path

1. [glTF quick start](gltf/quick-start.md)
2. [Blender authoring](gltf/blender-authoring.md)
3. [Player replacement workflow](gltf/player-replacement-workflow.md)
4. [ZScript usage](gltf/zscript-usage.md)
5. [ZScript API](gltf/zscript-api.md)

For textured glTF assets, use `.gltf + .bin + external textures`. `.glb` is
supported for geometry and animation loading, but embedded image texture decoding
is still limited, so external textures are the safest modding workflow.

## Full Index

### glTF Modding

- [glTF section index](gltf/README.md)
- [Quick start](gltf/quick-start.md)
- [Player replacement workflow](gltf/player-replacement-workflow.md)
- [Blender authoring guide](gltf/blender-authoring.md)
- [Blender player replacement tutorial](gltf/blender-player-replacement-tutorial.md)
- [Beginner tutorial](gltf/beginner-tutorial.md)
- [Workflow reference](gltf/workflow-reference.md)
- [Production workflow guide](gltf/production-workflow-guide.md)
- [ZScript usage](gltf/zscript-usage.md)
- [ZScript API](gltf/zscript-api.md)
- [glTF tools](gltf/tools.md)
- [v2 improvements](gltf/v2-improvements.md)

### Development

- [Development section index](development/README.md)
- [glTF implementation](development/gltf-implementation.md)
- [glTF implementation status](development/gltf-implementation-status.md)
- [glTF compilation fixes](development/gltf-compilation-fixes.md)
- [glTF robustness improvements](development/gltf-robustness-improvements.md)
- [Create glTF replacement script improvements](development/create-gltf-replacement-script-improvements.md)

### Engine And Maintenance

- [Engine section index](engine/README.md)
- [Rendering presets, bloom, and sector light bleed](engine/rendering-presets.md)
- [Automatic IWAD discovery](engine/iwad-discovery.md)
- [Procedural map generation](engine/procedural-map-generation.md)
- [Procedural generation research paper](engine/procedural-generation-research-paper.md)
- [Mugshot scaling](engine/mugshot-scaling.md)
- [Mugshot tutorial](engine/mugshot-tutorial.md)
- [Scripting section index](scripting/README.md)
- [Embedded Python tutorial and API](scripting/python.md)
- [Embedded Python example suite](../examples/python/)
- [Release section index](release/README.md)
- [Release checklist](release/releasing.md)
- [BiasedDoom 4.15.8 release notes](release/4.15.8.md)
- [BiasedDoom 4.15.7 release notes](release/4.15.7.md)
- [BiasedDoom 4.15.6 release notes](release/4.15.6.md)
