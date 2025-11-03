# NeoDoom GLTF Quick Start Guide

## 60-Second Checklist

### 1. Blender Setup (5 min)
- [ ] Set frame rate to 35 fps
- [ ] Character feet at Z=0
- [ ] Armature at origin (0,0,0)
- [ ] Apply all transforms (`Ctrl+A`)

### 2. Create Animation (10 min)
- [ ] Action Editor → New Action → "Idle"
- [ ] Animate 35 frames (1 second loop)
- [ ] First frame matches last frame

### 3. Export (1 min)
```
File → Export → glTF 2.0 (.glb)
Format: GLB
Transform: +Y Up ✓
Animations: ✓
Force Sample: ✓
```

### 4. Create MODELDEF
```c
Model DoomPlayer
{
    Path "models/player"
    Model 0 "character.glb"
    Scale 10.0 10.0 10.0
    Offset 0 0 40
    
    FrameIndex PLAY A 0 0
    FrameIndex PLAY B 0 0
    // ... (copy template from full guide)
}
```

### 5. Package PK3
```bash
MyMod.pk3/
├── MODELDEF
└── models/player/character.glb
```

```bash
zip -r MyMod.pk3 MODELDEF models/
```

### 6. Test
```bash
./neodoom -file MyMod.pk3 +map map01
```

## Expected Output
```
GLTF: Auto-selected animation 0 'Idle' (duration=1.00s)
GLTF Anim: time=0.42/1.00 bones=15
```

## Common Fixes

**Underground**: Increase offset
```c
Offset 0 0 50  // Try higher values
```

**Too big/small**: Adjust scale
```c
Scale 15.0 15.0 15.0  // Bigger
Scale 8.0 8.0 8.0     // Smaller
```

**No animation**: Check Blender Action Editor has animations

## Full Documentation
See `docs/GLTF_WORKFLOW.md` for complete guide

