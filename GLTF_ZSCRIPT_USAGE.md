# BiasedDoom glTF ZScript Usage Guide

## Summary

✅ **Build Status**: BiasedDoom now compiles with glTF support and ZScript bindings!
✅ **C++ Implementation**: glTF model loading code is integrated
✅ **ZScript Mixin**: GLTFModel mixin is available in gzdoom.pk3

⚠️ **Issue Found**: Custom PK3s with their own ZSCRIPT.zs file cannot access the GLTFModel mixin due to ZScript namespace isolation.

## The Problem

When a custom PK3 declares its own `ZSCRIPT.zs` with a version string, it creates an isolated namespace that doesn't inherit mixins from the main gzdoom.pk3.

**Example of problematic structure:**
```
NeoPlayer_cube.pk3/
├── ZSCRIPT.zs          ← This creates isolated namespace!
│   version "4.5"
│   #include "zscript/NeoPlayer.zs"
└── zscript/
    └── NeoPlayer.zs    ← Cannot see GLTFModel mixin
        mixin GLTFModel  ← ERROR: Mixin does not exist
```

## Solutions

### Option 1: Use MODELDEF (Recommended for now)

Remove the ZScript mixin approach and use traditional MODELDEF:

**MODELDEF:**
```
Model "NeoPlayerModel"
{
    Path "models/player"
    Model 0 "player.gltf"
    Scale 1.0 1.0 1.0
    
    FrameIndex PLAY A 0 0
}
```

**ZScript:**
```zscript
class NeoPlayer : DoomPlayer replaces DoomPlayer
{
    Default
    {
        Player.DisplayName "NeoPlayer (glTF)";
    }
    
    States
    {
    Spawn:
        PLAY A -1;
        Loop;
    }
}
```

### Option 2: Include GLTFModel in Your PK3

Copy the GLTFModel mixin into your own PK3:

**Your PK3 structure:**
```
YourMod.pk3/
├── ZSCRIPT.zs
│   version "4.5"
│   #include "zscript/gltf_model.zs"   ← Include FIRST
│   #include "zscript/your_actor.zs"
└── zscript/
    ├── gltf_model.zs  ← Copy from BiasedDoom
    └── your_actor.zs
```

Extract the mixin from gzdoom.pk3:
```bash
unzip -j gzdoom.pk3 "zscript/models/gltf_model.zs" -d your_pk3/zscript/
```

Helper (recommended): There's a convenience script in this repository to copy the mixin into a mod folder and ensure `ZSCRIPT.zs` includes it. Usage:

```bash
./tools/copy_gltf_mixin_to_mod.sh /path/to/NeoPlayer_cube  # pass the unzipped mod folder
# then rezip into NeoPlayer_cube.pk3 or point the engine to the folder
```

This avoids the namespace isolation error by making the mixin available inside the mod's own ZScript namespace.

### Option 3: Don't Use Custom ZSCRIPT.zs

If your mod only has a few actors, don't create a ZSCRIPT.zs file. Instead, put your ZScript in the wadsrc/static/zscript/ directory when building from source, or contribute it upstream.

## Current ZScript Bindings Status

The following native functions are implemented as **stubs** (they log but don't actually manipulate models yet):

- ✅ `NativePlayAnimation(name, loop, blendTime)` - Logs animation playback
- ✅ `NativeStopAnimation()` - Logs stop request
- ✅ `NativePauseAnimation()` - Logs pause request
- ✅ `NativeResumeAnimation()` - Logs resume request
- ✅ `NativeSetAnimationSpeed(speed)` - Logs speed change
- ✅ `NativeSetPBREnabled(enable)` - Logs PBR toggle
- ✅ `NativeSetMetallicFactor(metallic)` - Logs metallic value
- ✅ `NativeSetRoughnessFactor(roughness)` - Logs roughness value
- ✅ `NativeSetEmissive(color, strength)` - Logs emissive values
- ✅ `NativeUpdateModel(deltaTime)` - Silent update tick

## Next Steps for Full Implementation

To make the ZScript bindings fully functional, the following work is needed:

1. **Link C++ glTF Model System to Actors**
   - Create actor→model mapping system
   - Store glTF model instance per actor
   - Hook into actor rendering pipeline

2. **Implement Animation Playback**
   - Connect ZScript animation calls to FGLTFModel
   - Update bone transformations per frame
   - Handle animation blending

3. **Integrate with Renderer**
   - Pass glTF model data to hardware renderer
   - Render animated meshes instead of sprites
   - Apply PBR materials

## Testing the Build

Your BiasedDoom build has all the foundations in place:

```bash
cd /home/ericsonwillians/workspace/BiasedDoom/build
./neodoom

# The engine will:
# ✅ Load glTF ZScript mixin
# ✅ Compile ZScript without errors (if no custom ZSCRIPT.zs)
# ✅ Log native function calls to console
# ⚠️  Not yet render glTF models (stub implementation)
```

## Build Details

- **Executable**: `/home/ericsonwillians/workspace/BiasedDoom/build/neodoom` (242M)
- **glTF Files Compiled**: model_gltf.cpp, model_gltf_render.cpp, model_gltf_helpers.cpp, gltf_zscript.cpp
- **fastgltf Version**: 0.5.0#1
- **Build Type**: Debug with ENABLE_GLTF, NEODOOM_GLTF_SUPPORT

---

For questions or to contribute to the full implementation, see the BiasedDoom documentation.
