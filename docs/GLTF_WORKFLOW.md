# NeoDoom GLTF Character Guide
## Complete Tutorial: Blender to NeoDoom Workflow

**Version**: 1.0  
**Last Updated**: November 2025  
**Author**: NeoDoom Team

---

## Table of Contents

1. [Introduction](#introduction)
2. [Blender Character Setup](#blender-character-setup)
3. [Animation Mapping](#animation-mapping)
4. [Materials and Textures](#materials-and-textures)
5. [Export Settings](#export-settings)
6. [MODELDEF Reference](#modeldef-reference)
7. [PK3 Package Structure](#pk3-package-structure)
8. [Testing and Debugging](#testing-and-debugging)
9. [Advanced Topics](#advanced-topics)
10. [Troubleshooting](#troubleshooting)

---

## Introduction

NeoDoom's GLTF support allows you to replace classic DOOM sprites with fully animated 3D models exported from Blender. This guide covers the complete workflow from character creation to in-game testing.

### What You'll Learn
- How to rig a character for DOOM animations
- Map animations to DOOM player states
- Set up PBR materials for proper rendering
- Package everything into a working PK3 mod

### Prerequisites
- Blender 3.0+ (4.0+ recommended)
- Basic Blender knowledge (modeling, rigging, animation)
- NeoDoom with GLTF support enabled
- Text editor for MODELDEF files

---

## Blender Character Setup

### 1. Character Model Requirements

**Polygon Count Guidelines**:
- **Low**: 500-2000 tris (recommended for multiplayer)
- **Medium**: 2000-8000 tris (single player)
- **High**: 8000-20000 tris (showcase/cutscenes)

**Scale Reference**:
- DoomGuy height: ~56 units (in-game)
- Your model in Blender: 2-4 Blender units tall
- Use MODELDEF Scale to adjust in-game size

### 2. Armature Setup

Create a simple humanoid rig:

```
Root
├── Hips
│   ├── Spine
│   │   ├── Chest
│   │   │   ├── Neck
│   │   │   │   └── Head
│   │   │   ├── Shoulder.L
│   │   │   │   ├── UpperArm.L
│   │   │   │   │   ├── LowerArm.L
│   │   │   │   │   │   └── Hand.L
│   │   │   └── Shoulder.R
│   │   │       ├── UpperArm.R
│   │   │       │   ├── LowerArm.R
│   │   │       │   │   └── Hand.R
│   ├── UpperLeg.L
│   │   ├── LowerLeg.L
│   │   │   └── Foot.L
│   └── UpperLeg.R
│       ├── LowerLeg.R
│       │   └── Foot.L
```

**Critical Setup Steps**:

1. **Armature at World Origin**:
   ```
   Location: (0, 0, 0)
   Rotation: (0, 0, 0)
   Scale: (1, 1, 1)
   ```

2. **Character Feet on Ground Plane**:
   - In Edit Mode, position character so feet are at Z=0
   - This ensures proper ground contact in-game

3. **Apply All Transforms**:
   - Select mesh: `Ctrl+A` → Apply → All Transforms
   - Select armature: `Ctrl+A` → Apply → All Transforms

4. **Skin Weights**:
   - Use Automatic Weights: Select mesh → Shift+Select armature → `Ctrl+P` → With Automatic Weights
   - Clean up weights in Weight Paint mode

### 3. Animation Timeline Setup

**Frame Rate**: Set to 35 FPS (DOOM's TICRATE)
```
Render Properties → Frame Rate → 35 fps
```

**Timeline Markers**:
Mark animation boundaries with markers (`M` key):
```
Frame 1: Idle Start
Frame 35: Idle End (1 second loop)
Frame 36: Walk Start
Frame 70: Walk End
... etc
```

---

## Animation Mapping

### DoomGuy Animation States

DOOM uses sprite-based animations with specific naming conventions. Here's the complete mapping:

#### Player Sprite Frames (PLAY prefix)

| Frame | State | Animation Needed | Typical Duration |
|-------|-------|------------------|------------------|
| A | Standing Idle | Idle loop | 1-2 seconds |
| B | Running Frame 1 | Run cycle frame 1 | 4 frames |
| C | Running Frame 2 | Run cycle frame 2 | 4 frames |
| D | Running Frame 3 | Run cycle frame 3 | 4 frames |
| E | Running Frame 4 | Run cycle frame 4 | 4 frames |
| F | Firing Frame 1 | Shoot start | 2-4 frames |
| G | Firing Frame 2 | Shoot recoil | 2-4 frames |
| H | Pain | Hit reaction | 4-6 frames |
| I | Death Frame 1 | Death start | 5 frames |
| J | Death Frame 2 | Falling | 5 frames |
| K | Death Frame 3 | Hit ground | 5 frames |
| L | Death Frame 4 | Dead pose | Hold |
| M | Death Frame 5 | Dead (alt) | Hold |
| N | XDeath Frame 1 | Gib death 1 | 5 frames |
| O | XDeath Frame 2 | Gib death 2 | 5 frames |
| P | XDeath Frame 3 | Gib death 3 | 5 frames |
| Q | XDeath Frame 4 | Gib death 4 | 5 frames |
| R | XDeath Frame 5 | Gibbed | Hold |
| S | XDeath Frame 6 | Gibbed (alt) | Hold |
| T | XDeath Frame 7 | Gibbed (alt 2) | Hold |
| U | XDeath Frame 8 | Gibbed (alt 3) | Hold |
| V | XDeath Frame 9 | Gibbed (alt 4) | Hold |
| W | Raise (respawn) | Stand up | 10-15 frames |

### Creating Animations in Blender

#### Method 1: Multiple Animations (Recommended)

Create separate animation actions in Blender's Action Editor:

1. **Action Editor** → **New Action**
2. Name it exactly as it will appear in MODELDEF (e.g., "Idle", "Run", "Shoot")
3. Animate within the action
4. Repeat for each animation

**Example Action Setup**:
```
Actions:
- Idle (0-35 frames, loops)
- Run (0-16 frames, loops)
- Shoot (0-8 frames, one-shot)
- Pain (0-6 frames, one-shot)
- Death (0-25 frames, one-shot)
```

#### Method 2: Single Timeline with Markers

Use NLA Editor to create animation strips:

1. Create all animations in one timeline
2. Use markers to denote boundaries
3. Push animations to NLA strips
4. Each NLA strip becomes a GLTF animation

### Animation Best Practices

**Looping Animations** (Idle, Run):
- First frame should match last frame
- Use cyclic F-Curves: Graph Editor → Channel → Extrapolation Mode → Make Cyclic

**One-Shot Animations** (Shoot, Pain, Death):
- Clear start and end poses
- Exaggerate keyframes for visibility at distance
- Hold final pose for death animations

**Movement Animations** (Run, Walk):
- Root bone should NOT translate (use in-place animation)
- DOOM handles movement via game logic
- Focus on limb motion only

---

## Materials and Textures

### PBR Material Setup

NeoDoom supports GLTF 2.0 PBR (Physically Based Rendering):

#### Basic Material Setup

1. **Shader Editor** → Add **Principled BSDF**
2. Set up texture nodes:

```
Material: "PlayerSkin"
├── Base Color
│   └── Image Texture → player_diffuse.png
├── Metallic: 0.0 (non-metallic)
├── Roughness: 0.8 (slightly rough)
├── Normal Map
│   └── Image Texture → player_normal.png
│   └── Normal Map node
└── Emission (optional)
    └── Image Texture → player_emissive.png
```

#### Texture Map Types

**Supported Maps**:
- **Base Color** (Required): RGB texture, main appearance
- **Normal Map**: RGB, surface detail without geometry
- **Metallic/Roughness**: Combined texture (Metal=R, Rough=G)
- **Occlusion**: Grayscale, ambient shadows
- **Emissive**: RGB, glowing parts

**Texture Specifications**:
```
Format: PNG (recommended) or JPG
Size: Power of 2 (512x512, 1024x1024, 2048x2048)
Color Space:
  - Base Color: sRGB
  - Normal: Non-Color
  - Metallic/Roughness: Non-Color
  - Emissive: sRGB
```

#### Material Using Only Colors

For simple models without textures:

```python
# In Blender
mat = bpy.data.materials.new(name="SolidGreen")
mat.use_nodes = True
bsdf = mat.node_tree.nodes["Principled BSDF"]
bsdf.inputs['Base Color'].default_value = (0, 0.8, 0, 1)  # RGBA
bsdf.inputs['Metallic'].default_value = 0.0
bsdf.inputs['Roughness'].default_value = 0.9
```

NeoDoom will automatically generate a solid color texture from `baseColorFactor`.

### Texture Packing

**Embed Textures** (Recommended for PK3):
- File → External Data → Pack Resources
- Textures will be embedded in GLB file

**External Textures** (Advanced):
- Export textures separately
- Place in `textures/` folder in PK3
- GLTF will reference external files

---

## Export Settings

### GLTF Export Configuration

**File** → **Export** → **glTF 2.0 (.glb/.gltf)**

#### Essential Settings

**Format**:
```
Format: glTF Binary (.glb)  ← RECOMMENDED
  - Single file, easy to package
  - Embedded textures and animations
```

**Include**:
```
☑ Selected Objects (if only exporting character)
☑ Custom Properties
☑ Cameras (optional, usually off)
☑ Punctual Lights (optional, usually off)
```

**Transform**:
```
+Y Up: YES ← CRITICAL
  Blender uses Z-up, GLTF uses Y-up
  This setting handles conversion automatically
```

**Geometry**:
```
☑ Apply Modifiers
☑ UVs
☑ Normals
☑ Tangents (for normal maps)
☑ Vertex Colors (if used)
☐ Loose Edges (off)
☐ Loose Points (off)
```

**Animation**:
```
☑ Use Current Frame (for static exports)
☑ Animations
☑ Limit to Playback Range (if using timeline markers)
☐ NLA Strips (use if you have NLA setup)
☑ Force Sample Animations
Sampling Rate: 30  ← Match DOOM framerate
☑ Export Deformation Bones Only
```

**Skinning**:
```
☑ Include All Bone Influences (not just 4)
☐ Export all bone influences (leave off for performance)
```

**Compression** (Blender 4.0+):
```
☐ Compress (off for debugging)
  Turn on later for final release
```

### Pre-Export Checklist

- [ ] All transforms applied (Ctrl+A)
- [ ] Character feet at Z=0 in Blender
- [ ] Armature at origin
- [ ] All animations created as separate Actions
- [ ] Textures embedded or paths correct
- [ ] Materials use Principled BSDF
- [ ] Frame rate set to 35 fps
- [ ] No non-manifold geometry
- [ ] Bone weights normalized

### Example Export Workflow

```python
# Blender Python script for batch export
import bpy

# Select only character and armature
bpy.ops.object.select_all(action='DESELECT')
bpy.data.objects['CharacterMesh'].select_set(True)
bpy.data.objects['Armature'].select_set(True)

# Export
bpy.ops.export_scene.gltf(
    filepath="/path/to/output/character.glb",
    export_format='GLB',
    export_selected=True,
    export_animations=True,
    export_force_sampling=True,
    export_apply=True
)
```

---

## MODELDEF Reference

### Basic Syntax

MODELDEF files define how GLTF models map to DOOM sprites.

**Location**: `MODELDEF` or `MODELDEF.txt` in PK3 root

#### Simple Example

```c
Model DoomPlayer
{
    Path "models/player"
    Model 0 "marine.glb"
    Scale 10.0 10.0 10.0
    Offset 0 0 0
    
    // Map PLAY sprite frames to animation 0 (Idle)
    FrameIndex PLAY A 0 0
    FrameIndex PLAY B 0 0
    FrameIndex PLAY C 0 0
    FrameIndex PLAY D 0 0
    // ... repeat for all frames
}
```

### MODELDEF Commands

#### Model Definition

```c
Model <ClassName>
{
    // Model commands here
}
```

**ClassName**: Must match DOOM actor class:
- `DoomPlayer` - Player character
- `ZombieMan` - Former Human
- `Imp` - DOOM Imp
- `BaronOfHell` - Baron
- etc.

#### Path Command

```c
Path "models/subfolder"
```

Sets the base path for model files (relative to PK3 root).

#### Model Command

```c
Model <index> "filename.glb"
```

- `<index>`: Usually 0 for single model
- Multiple models can be used for different parts (weapon, body, etc.)

#### Scale Command

```c
Scale <x> <y> <z>
```

Scales the model in each axis. Use uniform scale unless you need stretching:
```c
Scale 10.0 10.0 10.0    // Standard scale
Scale 8.5 8.5 8.5       // Smaller
Scale 15.0 15.0 15.0    // Larger
```

**Finding the Right Scale**:
1. Start with 10.0
2. Test in-game
3. Adjust by factors of 0.5-2.0
4. DoomGuy is typically 10-12 units

#### Offset Command

```c
Offset <x> <y> <z>
```

Offsets model position. Useful for fixing ground alignment:
```c
Offset 0 0 0      // Default
Offset 0 0 -10    // Move down 10 units
Offset 0 0 28     // Move up 28 units
```

**Finding the Right Offset**:
- If character is underground: Use positive Z
- If character floats: Use negative Z
- Typical range: -20 to 50

#### FrameIndex Command

```c
FrameIndex <sprite> <frame> <model> <animation>
```

Maps sprite frames to model animations:

- `<sprite>`: 4-letter sprite name (PLAY, POSS, TROO, etc.)
- `<frame>`: Letter A-Z representing frame
- `<model>`: Model index (usually 0)
- `<animation>`: Animation index from GLTF file

**Examples**:
```c
// All frames use animation 0
FrameIndex PLAY A 0 0
FrameIndex PLAY B 0 0
FrameIndex PLAY C 0 0

// Different animations for different actions
FrameIndex PLAY A 0 0   // Idle (animation 0)
FrameIndex PLAY B 0 1   // Run cycle (animation 1)
FrameIndex PLAY F 0 2   // Shoot (animation 2)
FrameIndex PLAY H 0 3   // Pain (animation 3)
FrameIndex PLAY I 0 4   // Death (animation 4)
```

### Advanced MODELDEF

#### Multiple Models (Weapon + Body)

```c
Model DoomPlayer
{
    Path "models/player"
    Model 0 "body.glb"    // Body
    Model 1 "weapon.glb"  // Weapon attachment
    Scale 10.0 10.0 10.0
    
    // Body animations
    FrameIndex PLAY A 0 0
    FrameIndex PLAY B 0 1
    
    // Weapon animations  
    FrameIndex PLAY F 1 0  // Model 1 (weapon), anim 0
}
```

#### Skin Overrides

```c
Model DoomPlayer
{
    Path "models/player"
    Model 0 "marine.glb"
    Scale 10.0 10.0 10.0
    
    SurfaceSkin 0 0 "textures/marine_red.png"    // Skin override
    
    FrameIndex PLAY A 0 0
}
```

#### Rotation and Offset Per Frame

```c
Model DoomPlayer
{
    Path "models/player"
    Model 0 "marine.glb"
    
    // Per-frame transformations
    FrameIndex PLAY A 0 0
    {
        Offset 0 0 2
        Rotation 0 15 0
    }
}
```

### Complete MODELDEF Example

```c
// Complete player character replacement
Model DoomPlayer
{
    Path "models/player"
    Model 0 "neomarine.glb"
    
    Scale 11.0 11.0 11.0
    Offset 0 0 0
    
    // === STANDING / IDLE ===
    FrameIndex PLAY A 0 0    // Idle loop
    
    // === RUNNING ===
    FrameIndex PLAY B 0 1    // Run cycle
    FrameIndex PLAY C 0 1
    FrameIndex PLAY D 0 1
    FrameIndex PLAY E 0 1
    
    // === ATTACK ===
    FrameIndex PLAY F 0 2    // Shoot
    FrameIndex PLAY G 0 2
    
    // === PAIN ===
    FrameIndex PLAY H 0 3    // Hit reaction
    
    // === DEATH ===
    FrameIndex PLAY I 0 4    // Normal death
    FrameIndex PLAY J 0 4
    FrameIndex PLAY K 0 4
    FrameIndex PLAY L 0 4
    FrameIndex PLAY M 0 4
    
    // === EXTREME DEATH ===
    FrameIndex PLAY N 0 5    // Gib death
    FrameIndex PLAY O 0 5
    FrameIndex PLAY P 0 5
    FrameIndex PLAY Q 0 5
    FrameIndex PLAY R 0 5
    FrameIndex PLAY S 0 5
    FrameIndex PLAY T 0 5
    FrameIndex PLAY U 0 5
    FrameIndex PLAY V 0 5
    
    // === RESPAWN ===
    FrameIndex PLAY W 0 0    // Rise from dead (use idle)
}
```

---

## PK3 Package Structure

### Directory Layout

A PK3 is a ZIP file with specific structure:

```
MyCharacterMod.pk3/
├── MODELDEF                    ← Model definitions
├── models/
│   └── player/
│       ├── character.glb       ← Your exported model
│       └── weapon.glb          ← Optional weapon model
├── textures/                   ← Optional external textures
│   └── player/
│       ├── diffuse.png
│       └── normal.png
├── sounds/                     ← Optional custom sounds
│   └── player/
│       └── death.ogg
└── README.txt                  ← Mod documentation
```

### Creating a PK3

#### Method 1: Command Line (Linux/Mac)

```bash
cd MyCharacterMod/
zip -r ../MyCharacterMod.pk3 *
```

#### Method 2: GUI (Windows/Linux/Mac)

1. Select all files/folders
2. Right-click → "Compress" or "Send to → Compressed folder"
3. Rename `.zip` to `.pk3`

#### Method 3: Automated Script

```bash
#!/bin/bash
# build_pk3.sh

MOD_NAME="NeoMarine"
BUILD_DIR="build"

# Create directory structure
mkdir -p "$BUILD_DIR/models/player"
mkdir -p "$BUILD_DIR/textures"

# Copy files
cp MODELDEF "$BUILD_DIR/"
cp models/player/*.glb "$BUILD_DIR/models/player/"
cp textures/*.png "$BUILD_DIR/textures/" 2>/dev/null

# Create PK3
cd "$BUILD_DIR"
zip -r "../${MOD_NAME}.pk3" *
cd ..

echo "Created ${MOD_NAME}.pk3"
```

### MODELDEF File

Create `MODELDEF` (no extension) in the PK3 root:

```c
// models/player/character.glb
Model DoomPlayer
{
    Path "models/player"
    Model 0 "character.glb"
    Scale 10.0 10.0 10.0
    Offset 0 0 0
    
    FrameIndex PLAY A 0 0
    FrameIndex PLAY B 0 0
    FrameIndex PLAY C 0 0
    FrameIndex PLAY D 0 0
    FrameIndex PLAY E 0 0
    FrameIndex PLAY F 0 0
    FrameIndex PLAY G 0 0
    FrameIndex PLAY H 0 0
    FrameIndex PLAY I 0 0
    FrameIndex PLAY J 0 0
    FrameIndex PLAY K 0 0
    FrameIndex PLAY L 0 0
    FrameIndex PLAY M 0 0
    FrameIndex PLAY N 0 0
    FrameIndex PLAY O 0 0
    FrameIndex PLAY P 0 0
    FrameIndex PLAY Q 0 0
    FrameIndex PLAY R 0 0
    FrameIndex PLAY S 0 0
    FrameIndex PLAY T 0 0
    FrameIndex PLAY U 0 0
    FrameIndex PLAY V 0 0
    FrameIndex PLAY W 0 0
}
```

### Optional: DECORATE for Custom Behavior

If you want to modify player properties:

`actors/player.dec`:
```c
ACTOR NeoMarine : DoomPlayer replaces DoomPlayer
{
    Health 150        // More health
    Speed 1.2         // Faster movement
    States
    {
    Spawn:
        PLAY A -1
        Stop
    }
}
```

### Testing Your PK3

```bash
# Linux/Mac
./neodoom -file MyCharacterMod.pk3

# With specific IWAD
./neodoom -iwad doom2.wad -file MyCharacterMod.pk3

# With map
./neodoom -file MyCharacterMod.pk3 +map map01
```

---

## Testing and Debugging

### Initial Test

1. **Load the mod**:
   ```bash
   ./neodoom -file YourMod.pk3 +map map01
   ```

2. **Check console output** (press `~`):
   ```
   GLTF: Auto-selected animation 0 'Idle' (duration=1.00s)
   GLTF Anim: time=0.42/1.00 bones=15
   ```

3. **Verify model appears**:
   - Character should be visible
   - At correct height (feet on ground)
   - Proper scale (not giant or tiny)

### Common Issues

#### Model Underground
**Symptom**: Can only see top of head
**Solution**: Increase Z offset in MODELDEF
```c
Offset 0 0 30    // Try increasing
```

#### Model Too Large/Small
**Symptom**: Model clips through walls or is tiny
**Solution**: Adjust scale
```c
Scale 8.0 8.0 8.0    // Smaller
Scale 15.0 15.0 15.0  // Larger
```

#### Animation Not Playing
**Console Check**:
```
GLTF: Auto-selected animation 0 'Idle' (duration=0.00s)  ← BAD (no duration)
GLTF: Auto-selected animation 0 'Idle' (duration=1.43s)  ← GOOD
```

**Fixes**:
- Verify animation exported (check in Blender)
- Ensure "Animations" checked in export settings
- Check animation has keyframes

#### Model Appears Black
**Symptom**: Model visible but completely black
**Causes**:
1. No textures (intentional baseColorFactor)
2. Missing normals
3. Material issues

**Solutions**:
```c
// In Blender, ensure:
- Principled BSDF is used
- Base Color is set (texture or color)
- Normals are included in export
```

#### Model Not Appearing At All
**Console Check**:
```
// Look for errors:
Error: Failed to load glTF model: [error message]
```

**Common Causes**:
- File path wrong in MODELDEF
- GLB file corrupted
- Missing from PK3

**Debug Steps**:
1. Extract PK3 and verify GLB exists
2. Check MODELDEF path matches file location
3. Try opening GLB in Blender to verify it's valid

### Debug Console Commands

```c
// Show model info
modelinfo

// Reload models
r_drawmodels 0  // Disable
r_drawmodels 1  // Re-enable

// Toggle model rendering
r_drawmodels 0  // Sprites only
r_drawmodels 1  // Models (default)

// Frame rate display
vid_fps 1
```

### Performance Testing

Monitor frame rate with different settings:

```bash
# Low detail test
./neodoom -file YourMod.pk3 -width 1280 -height 720

# High detail test
./neodoom -file YourMod.pk3 -width 1920 -height 1080
```

**Target**: 60+ FPS
**Acceptable**: 35+ FPS
**Poor**: <30 FPS (reduce poly count or textures)

---

## Advanced Topics

### Animation Blending

NeoDoom currently doesn't support animation blending, but you can create smooth transitions:

```c
// In Blender:
1. Create transition animations between states
2. Export as separate actions
3. Map to intermediate frames in MODELDEF
```

### LOD (Level of Detail)

Create multiple versions of your model:

```
models/player/
├── marine_high.glb    // 15,000 tris
├── marine_med.glb     // 5,000 tris
└── marine_low.glb     // 1,500 tris
```

MODELDEF (future support):
```c
Model DoomPlayer
{
    Path "models/player"
    Model 0 "marine_high.glb"
    Model 1 "marine_med.glb"   // Future LOD support
    Model 2 "marine_low.glb"
    
    LODDistances 500 1000 2000
}
```

### Custom Shaders (Future)

NeoDoom may support custom shaders:

```c
Material "PlayerSkin"
{
    Shader "shaders/player_pbr.glsl"
    Texture BaseColor "textures/player_diffuse.png"
    Texture Normal "textures/player_normal.png"
}
```

### Bone Overrides (Advanced)

Dynamically adjust specific bones:

```c
// Example: Look at target
ACTOR SmartMarine : DoomPlayer
{
    BoneOverride "Head" LookAtTarget
    BoneOverride "Spine" PartialLookAtTarget 0.3
}
```

---

## Troubleshooting

### Problem: "Failed to load glTF model"

**Check**:
1. File path in MODELDEF matches actual file location
2. GLB file is valid (open in Blender to verify)
3. File is actually in the PK3 (extract and check)

### Problem: Model appears but animation frozen

**Check console**:
```
GLTF Anim: time=0.00/1.00 bones=1  ← Frozen (time not increasing)
```

**Causes**:
- Animation duration is 0
- No keyframes in animation
- Animation not exported

**Fix**:
- Re-export with "Animations" enabled
- Verify animations exist in Blender Action Editor
- Check "Force Sample Animations" in export

### Problem: Model is white/untextured

**Cause**: Missing or invalid materials

**Fix**:
1. Verify materials use Principled BSDF
2. Check Base Color has value/texture
3. In export settings, ensure "Materials" is enabled
4. Pack textures before export

### Problem: Model faces wrong direction

**Fix in Blender**:
1. Rotate character 180° around Z-axis in Edit Mode
2. Apply rotation: Object Mode → `Ctrl+A` → Rotation
3. Re-export

**Fix in MODELDEF**:
```c
Model DoomPlayer
{
    ...
    Rotation 0 180 0  // Rotate 180° around Y-axis
    ...
}
```

### Problem: Animations play too fast/slow

**Cause**: Frame rate mismatch

**Fix**:
1. Blender frame rate should be 35 fps
2. Animation sampling rate in export: 30-35
3. Adjust animation speed in Blender before export

### Problem: Model appears stretched/squashed

**Cause**: Non-uniform scale applied

**Fix**:
1. In Blender: `Ctrl+A` → Apply Scale
2. Verify scale is (1, 1, 1) before export
3. Use uniform scale in MODELDEF: `Scale 10 10 10`

---

## Example Workflow: Complete Character Mod

### Step-by-Step: "Neo Marine" Character

#### 1. Blender Setup (Day 1-2)

```python
# Blender script: setup_character.py
import bpy

# Set project frame rate
bpy.context.scene.render.fps = 35

# Create armature
bpy.ops.object.armature_add()
arm = bpy.context.object
arm.name = "MarineRig"

# Position at origin
arm.location = (0, 0, 0)

# Create character mesh (or import)
# ... modeling work ...

# Parent mesh to armature
# Select mesh, Shift+Select armature, Ctrl+P → With Automatic Weights
```

#### 2. Create Animations (Day 3-5)

```
Action "Idle":
  - 35 frames (1 second loop)
  - Subtle breathing motion
  - Weight shift

Action "Run":
  - 16 frames (fast loop)
  - Arms swinging
  - Legs cycling

Action "Shoot":
  - 8 frames (one-shot)
  - Recoil motion
  - Return to idle pose

Action "Death":
  - 25 frames (one-shot)
  - Fall backward
  - Hit ground
  - Final rest pose
```

#### 3. Materials (Day 1)

```
Material "MarineArmor":
  - Base Color: marine_diffuse.png (2048x2048)
  - Normal Map: marine_normal.png
  - Metallic: 0.2
  - Roughness: 0.7
  - Emissive: marine_glow.png (visor/lights)
```

#### 4. Export

```
File → Export → glTF 2.0
Format: GLB
Include: Selected Objects
Transform: +Y Up
Animation: Force Sample, 30 fps
Output: ~/Desktop/NeoMarine/models/player/marine.glb
```

#### 5. Create MODELDEF

`~/Desktop/NeoMarine/MODELDEF`:
```c
Model DoomPlayer
{
    Path "models/player"
    Model 0 "marine.glb"
    Scale 11.5 11.5 11.5
    Offset 0 0 2
    
    FrameIndex PLAY A 0 0  // Idle
    FrameIndex PLAY B 0 1  // Run
    FrameIndex PLAY C 0 1
    FrameIndex PLAY D 0 1
    FrameIndex PLAY E 0 1
    FrameIndex PLAY F 0 2  // Shoot
    FrameIndex PLAY G 0 2
    FrameIndex PLAY H 0 3  // Pain
    FrameIndex PLAY I 0 4  // Death
    FrameIndex PLAY J 0 4
    FrameIndex PLAY K 0 4
    FrameIndex PLAY L 0 4
    FrameIndex PLAY M 0 4
    // ... rest of frames
}
```

#### 6. Build PK3

```bash
cd ~/Desktop/NeoMarine
zip -r NeoMarine.pk3 MODELDEF models/
```

#### 7. Test

```bash
./neodoom -file ~/Desktop/NeoMarine.pk3 +map map01
```

#### 8. Iterate

- Adjust Scale and Offset based on in-game appearance
- Tweak animations if needed
- Optimize textures for performance
- Test in multiplayer

#### 9. Release

```
NeoMarine_v1.0.pk3
├── MODELDEF
├── README.txt
├── CREDITS.txt
├── models/player/marine.glb
└── screenshots/
    ├── ingame1.png
    └── ingame2.png
```

---

## Quick Reference Card

### Essential MODELDEF Template

```c
Model DoomPlayer
{
    Path "models/player"
    Model 0 "YOURMODEL.glb"
    Scale 10.0 10.0 10.0
    Offset 0 0 0
    
    FrameIndex PLAY A 0 0
    FrameIndex PLAY B 0 0
    FrameIndex PLAY C 0 0
    FrameIndex PLAY D 0 0
    FrameIndex PLAY E 0 0
    FrameIndex PLAY F 0 0
    FrameIndex PLAY G 0 0
    FrameIndex PLAY H 0 0
    FrameIndex PLAY I 0 0
    FrameIndex PLAY J 0 0
    FrameIndex PLAY K 0 0
    FrameIndex PLAY L 0 0
    FrameIndex PLAY M 0 0
    FrameIndex PLAY N 0 0
    FrameIndex PLAY O 0 0
    FrameIndex PLAY P 0 0
    FrameIndex PLAY Q 0 0
    FrameIndex PLAY R 0 0
    FrameIndex PLAY S 0 0
    FrameIndex PLAY T 0 0
    FrameIndex PLAY U 0 0
    FrameIndex PLAY V 0 0
    FrameIndex PLAY W 0 0
}
```

### Blender Export Checklist

- [ ] Frame rate: 35 fps
- [ ] +Y Up enabled
- [ ] Animations included
- [ ] Force Sample Animations
- [ ] All transforms applied
- [ ] Character feet at Z=0
- [ ] Format: GLB
- [ ] Textures packed/embedded

### PK3 Structure

```
YourMod.pk3/
├── MODELDEF
└── models/
    └── player/
        └── yourmodel.glb
```

### Testing Command

```bash
./neodoom -file YourMod.pk3 +map map01
```

---

## Additional Resources

### Community
- **NeoDoom Forums**: [Link TBD]
- **Discord**: [Link TBD]
- **GitHub Issues**: Report bugs and request features

### Tools
- **Blender**: https://www.blender.org/
- **glTF Viewer**: https://gltf-viewer.donmccurdy.com/
- **Texture Tools**: GIMP, Krita, Substance Painter

### Learning Resources
- Blender Character Modeling: Blender Cloud tutorials
- PBR Texturing: Substance Academy
- DOOM Modding: ZDoom Wiki
- glTF Specification: https://www.khronos.org/gltf/

---

## Changelog

**v1.0 (November 2025)**
- Initial release
- Complete workflow documentation
- MODELDEF reference
- Troubleshooting guide

---

**Happy Modding!**

For questions, issues, or to share your creations, visit the NeoDoom community forums.

