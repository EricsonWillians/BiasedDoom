# Blender to BiasedDoom: Complete Player Replacement Tutorial

**A beginner-friendly guide to creating animated glTF player models for BiasedDoom**

---

## Table of Contents

1. [Introduction](#introduction)
2. [Prerequisites](#prerequisites)
3. [Part 1: Setting Up Your Character Model](#part-1-setting-up-your-character-model)
4. [Part 2: Creating the Armature (Skeleton)](#part-2-creating-the-armature-skeleton)
5. [Part 3: Rigging Your Character](#part-3-rigging-your-character)
6. [Part 4: Creating Animations](#part-4-creating-animations)
7. [Part 5: Texturing Your Model](#part-5-texturing-your-model)
8. [Part 6: Exporting to glTF](#part-6-exporting-to-gltf)
9. [Part 7: Creating the MODELDEF](#part-7-creating-the-modeldef)
10. [Part 8: Packaging Your Mod](#part-8-packaging-your-mod)
11. [Part 9: Testing in BiasedDoom](#part-9-testing-in-neodoom)
12. [Troubleshooting](#troubleshooting)
13. [Advanced Topics](#advanced-topics)

---

## Introduction

This tutorial will teach you how to create a complete, animated player character replacement for BiasedDoom using Blender. By the end, you'll have a fully functional 3D character that replaces the Doom Guy sprite with a modern glTF model supporting skeletal animation.

**What you'll learn:**
- Character modeling basics in Blender
- Skeletal rigging and skinning
- Creating game-ready animations
- PBR texturing workflow
- glTF export settings for BiasedDoom
- MODELDEF configuration
- Packaging mods as PK3 files

**Time estimate:** 3-6 hours for your first character

---

## Prerequisites

### Required Software

1. **Blender 3.6 or newer** (Download: https://www.blender.org/download/)
   - Why: Built-in glTF 2.0 export with full animation support

2. **BiasedDoom** (Latest build with glTF support)
   - Located at: `/home/ericsonwillians/workspace/BiasedDoom/build/neodoom`

3. **A Doom IWAD file** (doom.wad, doom2.wad, etc.)
   - Required to run BiasedDoom

### Recommended Skills

- Basic Blender navigation (viewport rotation, object selection)
- Understanding of 3D modeling concepts
- Familiarity with Doom gameplay

**Don't worry if you're a beginner!** This tutorial explains every step in detail.

---

## Part 1: Setting Up Your Character Model

### Step 1.1: Starting a New Blender Project

1. Open Blender
2. Delete the default cube (Select it, press `X`, confirm deletion)
3. Keep the default camera and light for reference
4. Set units to meters: `Properties Panel > Scene > Units > Metric`

### Step 1.2: Character Proportions for Doom

Doom Guy is approximately **56 units tall** in Doom, which translates to about **1.8 meters** in Blender.

**Recommended proportions:**
- **Height:** 1.8 meters (from feet to top of head)
- **Width (shoulders):** 0.5 meters
- **Depth (chest):** 0.3 meters

### Step 1.3: Modeling Your Character

You have three options:

#### Option A: Create a Simple Blocky Character (Beginner)

1. Add a cube: `Shift+A > Mesh > Cube`
2. Scale for body: `S` then type `0.25, 0.15, 0.4` (X, Y, Z)
3. Add another cube for head: `Shift+A > Mesh > Cube`
4. Scale head: `S` then type `0.2, 0.2, 0.25`
5. Position head above body: `G` then `Z` then type `0.5`
6. Add cubes for arms and legs similarly

**TIP:** Keep it simple for your first character! You can always create a more detailed model later.

#### Option B: Use a Humanoid Base Mesh

1. Add an Armature first (we'll do this in Part 2)
2. Generate a base mesh using Rigify's human meta-rig
3. Customize the proportions

#### Option C: Import an Existing Character Model

1. Import your model: `File > Import > FBX/OBJ/etc.`
2. Scale to Doom proportions (1.8m height)
3. Ensure the model is centered at world origin

### Step 1.4: Model Checklist

Before proceeding, verify:

- ✅ Model is centered at world origin (0, 0, 0)
- ✅ Model faces **+Y axis** (forward direction in Doom)
- ✅ Model is approximately 1.8 meters tall
- ✅ Model has proper scale: `Ctrl+A > Apply > Scale`
- ✅ Model is a single mesh object (or logically grouped)

---

## Part 2: Creating the Armature (Skeleton)

The armature is the "skeleton" that will drive your character's animations.

### Step 2.1: Basic Armature Structure

**Minimum bones required for Doom player:**

1. **Root** (pelvis/hip) - Main parent bone
2. **Spine** (chest/torso)
3. **Head** (neck/head)
4. **Upper Arms** (left & right)
5. **Lower Arms** (left & right)
6. **Upper Legs** (left & right)
7. **Lower Legs** (left & right)

### Step 2.2: Creating the Armature

1. **Add Armature:**
   - `Shift+A > Armature > Single Bone`
   - This creates one bone at the 3D cursor

2. **Enter Edit Mode:**
   - Select the armature
   - Press `Tab` to enter Edit Mode

3. **Create Root Bone:**
   - Position the first bone at the pelvis/hip area
   - Rename it to "Root": `Properties Panel > Bone Properties > Name: Root`

4. **Extrude Bones:**
   - Select the tip (head) of the bone
   - Press `E` to extrude
   - Move the new bone to create the spine
   - Rename: "Spine"

5. **Continue Building the Skeleton:**

```
Root (Pelvis)
├── Spine (Chest)
│   ├── Head
│   ├── UpperArm.L (Left Shoulder)
│   │   └── LowerArm.L (Left Forearm)
│   └── UpperArm.R (Right Shoulder)
│       └── LowerArm.R (Right Forearm)
├── UpperLeg.L (Left Thigh)
│   └── LowerLeg.L (Left Shin)
└── UpperLeg.R (Right Thigh)
    └── LowerLeg.R (Right Shin)
```

### Step 2.3: Bone Naming Conventions

**IMPORTANT:** Use Blender's `.L` and `.R` suffixes for symmetrical bones:

- `UpperArm.L` / `UpperArm.R`
- `LowerArm.L` / `LowerArm.R`
- `UpperLeg.L` / `UpperLeg.R`
- `LowerLeg.L` / `LowerLeg.R`

This enables Blender's symmetry tools!

### Step 2.4: Using Rigify (Advanced/Optional)

For a more sophisticated rig:

1. Enable Rigify addon: `Edit > Preferences > Add-ons > Search "Rigify" > Enable`
2. `Shift+A > Armature > Basic > Human`
3. Scale and adjust the meta-rig to match your character
4. Select the armature, click `Generate Rig` in properties panel

---

## Part 3: Rigging Your Character

Rigging connects your mesh to the armature so animations work.

### Step 3.1: Automatic Weights (Easiest Method)

1. **Select your character mesh** in Object Mode
2. **Shift-select the armature** (armature must be selected last)
3. **Parent with Automatic Weights:**
   - Press `Ctrl+P`
   - Choose `With Automatic Weights`

Blender will automatically calculate how each bone influences the mesh.

### Step 3.2: Testing the Rig

1. **Enter Pose Mode:**
   - Select the armature
   - Press `Ctrl+Tab` or switch mode to `Pose Mode`

2. **Test bone movements:**
   - Select a bone (e.g., UpperArm.L)
   - Press `R` to rotate
   - Verify the mesh deforms correctly

3. **Common issues:**
   - **Mesh doesn't move:** Check that mesh is parented to armature
   - **Weird deformations:** Adjust weight painting (see below)

### Step 3.3: Weight Painting (Fine-Tuning)

Weight painting controls how much each bone influences each vertex.

1. **Select the mesh**
2. **Enter Weight Paint Mode:** `Ctrl+Tab` or mode dropdown
3. **Select a bone:** Click on bones in the armature
4. **Paint weights:**
   - Red = Full influence (1.0)
   - Blue = No influence (0.0)
   - Green/Yellow = Partial influence

**Tips:**
- Shoulders should be influenced by UpperArm bones
- Elbows should blend between UpperArm and LowerArm
- Keep the torso influenced by Spine/Root

### Step 3.4: Weight Paint Checklist

Test each bone by rotating it in Pose Mode:

- ✅ Arms bend at elbows correctly
- ✅ Legs bend at knees correctly
- ✅ Head rotates without stretching neck
- ✅ Torso deforms naturally when spine rotates
- ✅ No vertices "left behind" when bones move

---

## Part 4: Creating Animations

Doom player needs several animations. Start with these essentials:

### Step 4.1: Animation Setup

1. **Switch to Animation Workspace:**
   - Top menu: `Animation` tab

2. **Create your first action:**
   - In Dope Sheet, change mode to `Action Editor`
   - Click `+ New` to create an action
   - Name it: `idle`

### Step 4.2: Essential Doom Player Animations

Create these animations as separate **Actions**:

| Animation Name | Frames | Description | Loop |
|---------------|--------|-------------|------|
| `idle` | 1-60 | Standing still, subtle breathing | Yes |
| `walk` | 1-30 | Walking forward | Yes |
| `run` | 1-20 | Running forward | Yes |
| `attack` | 1-15 | Shooting weapon | No |
| `pain` | 1-10 | Taking damage reaction | No |
| `death` | 1-40 | Death animation | No |
| `jump` | 1-20 | Jump pose | No |

### Step 4.3: Creating the Idle Animation

1. **Set frame range:** Timeline at bottom, set End Frame to `60`
2. **Enter Pose Mode** on armature
3. **Frame 1 - Starting Pose:**
   - Position bones in neutral standing pose
   - Select all bones: `A`
   - Insert keyframe: `I > Location & Rotation`

4. **Frame 30 - Breathing Pose:**
   - Select Spine bone
   - Rotate slightly forward: `R Y 5` (5 degrees Y-axis)
   - Move up slightly: `G Z 0.01`
   - Insert keyframe: `I > Location & Rotation`

5. **Frame 60 - Return to Start:**
   - Copy frame 1 pose
   - Insert keyframe: `I > Location & Rotation`

6. **Set to loop:**
   - Graph Editor > Channel > Extrapolation Mode > Make Cyclic (F-Modifier)

### Step 4.4: Creating the Walk Cycle

**Standard walk cycle = 20-30 frames**

1. **Create new action:** Name it `walk`
2. **Frame 1 - Contact Pose:**
   ```
   - Left leg forward, right leg back
   - Right arm forward, left arm back (opposite of legs!)
   - Slight spine rotation for natural movement
   ```
   - Select all bones: `A`
   - Insert keyframe: `I > Location & Rotation`

3. **Frame 8 - Passing Pose:**
   ```
   - Both legs nearly vertical
   - Body at highest point
   - Arms passing by torso
   ```
   - Insert keyframe: `I > Location & Rotation`

4. **Frame 15 - Contact Pose (Opposite):**
   ```
   - Right leg forward, left leg back
   - Left arm forward, right arm back
   - Mirror of frame 1
   ```
   - Insert keyframe: `I > Location & Rotation`

5. **Frame 23 - Passing Pose (Opposite):**
   - Mirror of frame 8
   - Insert keyframe: `I > Location & Rotation`

6. **Frame 30 - Return to Frame 1:**
   - Copy frame 1 pose
   - Insert keyframe: `I > Location & Rotation`

**TIP:** Use `Shift+E` to add walk cycle with Rigify, or use the NLA Editor to loop animations.

### Step 4.5: Creating the Attack Animation

1. **Create new action:** Name it `attack`
2. **Frame 1:** Neutral idle pose
3. **Frame 5:** Arms raised, weapon pointing forward (gun recoil)
4. **Frame 10:** Arms push back (recoil motion)
5. **Frame 15:** Return to neutral pose

### Step 4.6: Animation Checklist

For each animation:

- ✅ Animation name matches Doom action (idle, walk, attack, etc.)
- ✅ Root bone stays at origin (character doesn't "drift")
- ✅ Loop animations return smoothly to frame 1
- ✅ Keyframes are set on all relevant bones
- ✅ Movements look natural when played back

---

## Part 5: Texturing Your Model

NeoDoom supports PBR (Physically Based Rendering) materials via glTF.

### Step 5.1: Material Setup

1. **Switch to Shading Workspace:** Top menu: `Shading` tab
2. **Select your mesh**
3. **Add Material:**
   - Material Properties panel > `+ New`
   - Name it: `PlayerMaterial`

### Step 5.2: PBR Texture Workflow

PBR uses multiple texture maps:

- **Base Color:** The main color/texture
- **Metallic:** How metallic the surface is (0=non-metal, 1=metal)
- **Roughness:** How rough/shiny the surface is (0=shiny, 1=rough)
- **Normal:** Simulates surface detail without geometry
- **Emissive:** Self-illuminated areas (glowing parts)

### Step 5.3: Simple Colored Material (Quickstart)

If you don't have textures yet:

1. **In Shader Editor:**
   - Default setup has "Principled BSDF" node
2. **Set Base Color:**
   - Click the color next to "Base Color"
   - Choose your character's color (e.g., green for classic Doom armor)
3. **Adjust Roughness:** Set to `0.8` for cloth/armor
4. **Adjust Metallic:** Set to `0.0` for non-metallic surfaces

**This is what you saw with the test cube!** NeoDoom uses the Base Color to create a solid-colored texture.

### Step 5.4: Adding Image Textures

1. **Add Image Texture node:**
   - `Shift+A > Texture > Image Texture`
2. **Load your texture:**
   - Click `Open` button
   - Navigate to your texture file
3. **Connect to Base Color:**
   - Drag from Image Texture "Color" output to Principled BSDF "Base Color" input

### Step 5.5: UV Unwrapping (Required for Textures)

Before textures display correctly, you need UV coordinates:

1. **Select your mesh**
2. **Enter Edit Mode:** `Tab`
3. **Select all vertices:** `A`
4. **Unwrap:**
   - `U > Smart UV Project` (automatic, good for simple models)
   - OR `U > Unwrap` (more control, requires seams)

5. **View UVs:**
   - Switch to UV Editing workspace
   - Your model's UV layout appears in the UV Editor

### Step 5.6: PBR Texture Maps (Advanced)

For full PBR materials:

1. **Base Color Map:**
   ```
   Image Texture (BaseColor.png) -> Principled BSDF "Base Color"
   ```

2. **Metallic/Roughness Map:**
   ```
   Image Texture (MetallicRoughness.png) -> Separate RGB
   Blue channel -> Principled BSDF "Metallic"
   Green channel -> Principled BSDF "Roughness"
   ```

3. **Normal Map:**
   ```
   Image Texture (Normal.png, set to Non-Color) -> Normal Map node -> Principled BSDF "Normal"
   ```

4. **Emissive Map:**
   ```
   Image Texture (Emissive.png) -> Principled BSDF "Emission"
   Set Emission Strength to 1.0 or higher
   ```

### Step 5.7: Texturing Checklist

- ✅ Material is using Principled BSDF shader
- ✅ Base Color is set (solid color or texture)
- ✅ If using textures: Mesh is UV unwrapped
- ✅ Material properties are game-appropriate (roughness, metallic)
- ✅ Emissive areas glow correctly (optional)

---

## Part 6: Exporting to glTF

### Step 6.1: Pre-Export Checklist

Before exporting, verify:

1. **Apply Transforms:**
   - Select all objects: `A`
   - `Ctrl+A > All Transforms`
   - This bakes scale/rotation/location

2. **Check Armature Rest Pose:**
   - Select armature
   - `Pose > Clear Transform > All`
   - Ensure character is in T-pose or neutral pose

3. **Verify Animation Actions:**
   - Dope Sheet > Action Editor
   - Confirm all animations are saved as actions
   - Each action has a unique name

4. **Check Naming:**
   - Objects should have clear names (Character_Mesh, Character_Armature)
   - No special characters or spaces in names (use underscores)

### Step 6.2: Export Settings

1. **File > Export > glTF 2.0 (.glb)**

2. **In the export dialog, configure these settings:**

   **Include Section:**
   - ✅ Selected Objects (if you only selected your character)
   - ✅ Visible Objects (if exporting entire scene)
   - ✅ Custom Properties
   - ✅ Cameras (optional, for reference)
   - ✅ Punctual Lights (optional, for reference)

   **Transform Section:**
   - ✅ **+Y Up** (CRITICAL! Doom uses +Y as up axis)
   - Format: **glTF Binary (.glb)** (single file, easier to manage)

   **Geometry Section:**
   - ✅ UVs
   - ✅ Normals
   - ✅ Tangents (required for normal maps)
   - ✅ Vertex Colors (if you painted colors)
   - ✅ Materials: **Export**
   - ✅ Compression: **None** (for debugging, use Draco later for optimization)

   **Animation Section:**
   - ✅ **Use Current Frame** (unchecked)
   - ✅ **Animations** (checked)
   - ✅ **Limit to Playback Range** (checked)
   - ✅ **Always Sample Animations** (checked)
   - ✅ **Group by NLA Track** (unchecked for simple setups)
   - ✅ **Export Deformation Bones Only** (checked, reduces file size)
   - Animation Mode: **Actions** (exports all actions as separate animations)
   - ✅ **Optimize Animation Size** (checked)

   **Skinning Section:**
   - ✅ **Skinning** (checked)
   - ✅ **Include All Bone Influences** (checked for quality)

3. **Save location:**
   - Name your file: `player_model.glb`
   - Save to a temporary location (you'll move it to your mod folder later)

### Step 6.3: Export Verification

After exporting, verify the file:

1. **Check file size:**
   - Should be reasonable (100KB - 50MB depending on detail)
   - Very small (<10KB) = possible export issue
   - Very large (>100MB) = may need optimization

2. **Test in Blender:**
   - Import your exported glTF: `File > Import > glTF 2.0`
   - Check if model looks correct
   - Verify animations play

3. **Use glTF Validator (optional but recommended):**
   - Visit: https://github.khronos.org/glTF-Validator/
   - Upload your .glb file
   - Fix any errors reported

---

## Part 7: Creating the MODELDEF

MODELDEF tells NeoDoom how to use your glTF model.

### Step 7.1: Create MODELDEF File

1. **Create a text file named:** `MODELDEF`
2. **Add the following structure:**

```
Model DoomPlayer
{
    Path "models/player"
    Model 0 "player_model.glb"
    Scale 1.0 1.0 1.0

    // Map all Doom player sprite frames to your model
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

### Step 7.2: Understanding MODELDEF Syntax

**Line by line explanation:**

- `Model DoomPlayer` - Defines a model for the "DoomPlayer" class
- `Path "models/player"` - Where the model file is located in the PK3
- `Model 0 "player_model.glb"` - The actual glTF file to load
- `Scale 1.0 1.0 1.0` - Scale multiplier (X, Y, Z)
  - Increase if model is too small (e.g., `5.0 5.0 5.0`)
  - Decrease if model is too large (e.g., `0.5 0.5 0.5`)
- `FrameIndex PLAY A 0 0` - Maps Doom sprite frame to model frame
  - `PLAY` = Sprite name
  - `A` = Frame letter
  - First `0` = Model index (always 0 for single model)
  - Second `0` = Animation frame (0 for static, will use glTF animations)

### Step 7.3: Advanced MODELDEF Options

```
Model DoomPlayer
{
    Path "models/player"
    Model 0 "player_model.glb"

    // Scale the model (if it's too small/large in-game)
    Scale 2.0 2.0 2.0

    // Offset the model position (X, Y, Z)
    Offset 0.0 0.0 0.0

    // Rotation offset (Pitch, Yaw, Roll in degrees)
    AngleOffset 0.0 0.0 0.0

    // Animation mapping (advanced - for future use)
    // FrameIndex SPRITE FRAME MODEL_INDEX ANIMATION_FRAME
    FrameIndex PLAY A 0 0  // Standing
    FrameIndex PLAY B 0 1  // Walking frame 1
    FrameIndex PLAY C 0 2  // Walking frame 2
    FrameIndex PLAY D 0 3  // Walking frame 3

    // All other frames...
}
```

### Step 7.4: Sprite Frame Reference

Doom player sprite frames (for reference):

```
PLAY A-D : Walking frames (4 directions)
PLAY E-H : Walking (4 more directions for 8-directional)
PLAY I-P : Shooting/attack frames
PLAY Q-T : Pain/damage frames
PLAY U-W : Death frames
```

For now, map everything to frame 0 (static). Animation switching will be implemented later.

---

## Part 8: Packaging Your Mod

PK3 files are ZIP archives with a specific structure.

### Step 8.1: Create Directory Structure

Create this folder structure:

```
MyPlayerMod/
├── MODELDEF
└── models/
    └── player/
        └── player_model.glb
```

### Step 8.2: File Placement

1. **Create main folder:**
   ```bash
   mkdir -p MyPlayerMod/models/player
   ```

2. **Copy files:**
   - Copy `MODELDEF` to `MyPlayerMod/`
   - Copy `player_model.glb` to `MyPlayerMod/models/player/`

3. **Verify structure:**
   ```bash
   tree MyPlayerMod/
   ```
   Should show:
   ```
   MyPlayerMod/
   ├── MODELDEF
   └── models
       └── player
           └── player_model.glb
   ```

### Step 8.3: Create PK3 File

**On Linux:**
```bash
cd MyPlayerMod
zip -r ../MyPlayerMod.pk3 *
```

**On Windows:**
1. Select all files/folders inside `MyPlayerMod`
2. Right-click > Send to > Compressed (zipped) folder
3. Rename from `.zip` to `.pk3`

**On Mac:**
```bash
cd MyPlayerMod
zip -r ../MyPlayerMod.pk3 *
```

### Step 8.4: PK3 Checklist

Verify your PK3:

```bash
unzip -l MyPlayerMod.pk3
```

Should show:
```
Archive:  MyPlayerMod.pk3
  Length      Date    Time    Name
---------  ---------- -----   ----
     1234  2024-01-01 12:00   MODELDEF
        0  2024-01-01 12:00   models/
        0  2024-01-01 12:00   models/player/
    45678  2024-01-01 12:00   models/player/player_model.glb
```

✅ MODELDEF is in root
✅ models/player/player_model.glb path is correct
✅ No extra folders wrapping the contents

---

## Part 9: Testing in NeoDoom

### Step 9.1: Running Your Mod

1. **Basic test command:**
   ```bash
   ./build/neodoom \
     -file /path/to/MyPlayerMod.pk3 \
     -iwad ~/.local/share/games/doom/doom2.wad \
     +map E1M1
   ```

2. **With chase camera (to see your model):**
   ```bash
   ./build/neodoom \
     -file /path/to/MyPlayerMod.pk3 \
     -iwad ~/.local/share/games/doom/doom2.wad \
     +map E1M1 \
     +chase_cam 1
   ```

3. **Enable developer mode (for debugging):**
   ```bash
   ./build/neodoom \
     -file /path/to/MyPlayerMod.pk3 \
     -iwad ~/.local/share/games/doom/doom2.wad \
     +map E1M1 \
     +chase_cam 1 \
     +developer 2
   ```

### Step 9.2: In-Game Testing

Once the game loads:

1. **Enable chase camera (if not already):**
   - Press `~` to open console
   - Type: `chase_cam 1`
   - Press Enter

2. **Check if model appears:**
   - Look around - you should see your 3D model instead of sprites
   - Move around - model should move with you

3. **Toggle chase cam:**
   - Console command: `chase_cam 0` (disable)
   - Console command: `chase_cam 1` (enable)

### Step 9.3: Common First-Run Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| Model invisible | Scale too small | Increase Scale in MODELDEF (try 5.0 5.0 5.0) |
| Model too large | Scale too big | Decrease Scale in MODELDEF (try 0.5 0.5 0.5) |
| Model not loading | File path wrong | Check PK3 structure: models/player/file.glb |
| Model offset wrong | Origin not centered | Re-export from Blender with correct origin |
| Model backwards | Wrong axis orientation | Re-export with +Y Up setting |
| Animations not playing | Export settings | Ensure "Animations" checked in glTF export |

### Step 9.4: Adjusting Scale and Position

If your model appears but is the wrong size:

1. **Edit MODELDEF:**
   ```
   Scale 5.0 5.0 5.0  // Try different values
   ```

2. **Re-create PK3:**
   ```bash
   cd MyPlayerMod
   zip -r ../MyPlayerMod.pk3 *
   ```

3. **Test again**

**Scale guidelines:**
- Default Blender scale (1.8m character) ≈ `Scale 1.0` in MODELDEF
- If model is tiny ≈ `Scale 5.0` to `10.0`
- If model is huge ≈ `Scale 0.1` to `0.5`

---

## Troubleshooting

### Model Not Loading

**Symptoms:** Console shows "model not found" or no error but model doesn't appear

**Solutions:**

1. **Check console output:**
   ```bash
   ./build/neodoom -file MyPlayerMod.pk3 -iwad doom2.wad +developer 2 2>&1 | grep -i "model\|gltf"
   ```

2. **Verify PK3 structure:**
   ```bash
   unzip -l MyPlayerMod.pk3
   ```
   - MODELDEF must be in root
   - Model file must be at `models/player/player_model.glb`

3. **Check MODELDEF path:**
   - `Path "models/player"` must match actual folder in PK3
   - `Model 0 "player_model.glb"` must match actual filename

4. **Test with absolute path (debugging):**
   ```
   Model 0 "/full/path/to/player_model.glb"
   ```

### Model Invisible in Chase Cam

**Symptoms:** Model loads but nothing appears on screen

**Solutions:**

1. **Increase scale dramatically:**
   ```
   Scale 10.0 10.0 10.0
   ```

2. **Check model facing direction:**
   - Model should face +Y axis in Blender
   - Re-export with "+Y Up" setting

3. **Verify materials:**
   - Model must have at least Base Color set
   - Check that material isn't transparent (Alpha = 1.0)

4. **Check vertex data:**
   - Ensure mesh has vertices (not empty)
   - Look for console errors about vertex buffers

### Model Animation Not Playing

**Symptoms:** Model appears static, doesn't animate

**Current Status:** Animation playback is **work in progress** in NeoDoom

**Temporary workaround:**
- Model will use a single static pose
- Full animation support coming in future updates

**Preparation for animation support:**
1. Export with all animations as Actions
2. Name animations clearly: idle, walk, run, attack, etc.
3. When animation support is added, your model will work automatically

### Model Appears Distorted or Stretched

**Causes:**
- Unapplied transforms in Blender
- Incorrect bone rest pose
- Weight painting issues

**Solutions:**

1. **In Blender before export:**
   - Select all: `Ctrl+A > All Transforms`
   - Armature: `Pose > Clear Transform > All`

2. **Check bone orientations:**
   - All bones should point "downward" in Edit Mode
   - Use `Armature > Calculate > Calculate Roll > Global +Y Axis`

3. **Re-do weight painting:**
   - Select mesh > Weight Paint Mode
   - Test each bone by rotating in Pose Mode

### Model Has Wrong Colors/Textures

**Solutions:**

1. **Check material export:**
   - Blender Shading workspace
   - Ensure Principled BSDF is used
   - Verify Base Color is connected

2. **Check texture paths (if using textures):**
   - glTF embeds textures in .glb format (recommended)
   - OR pack textures: `File > External Data > Pack Resources`

3. **Test with simple color:**
   - Remove all textures
   - Set solid Base Color
   - Should render as solid color in NeoDoom

4. **Verify UV unwrap:**
   - UV Editing workspace
   - Check that UVs don't overlap badly
   - Re-unwrap: `U > Smart UV Project`

### Performance Issues

**Symptoms:** Game runs slowly with model loaded

**Solutions:**

1. **Reduce polygon count:**
   - Edit Mode: Select all
   - Mesh > Clean Up > Limited Dissolve
   - OR: Add Decimate modifier in Blender

2. **Optimize textures:**
   - Reduce texture resolution (2048x2048 → 1024x1024)
   - Use compressed formats

3. **Reduce bone count:**
   - Fewer bones = faster skinning
   - Merge unnecessary bones

4. **Enable Draco compression:**
   - glTF export: Enable "Draco mesh compression"
   - Requires NeoDoom Draco support (check documentation)

---

## Advanced Topics

### Animation State Machine (Future Feature)

When full animation support is implemented, you'll be able to map Doom states to glTF animations:

```
Model DoomPlayer
{
    Path "models/player"
    Model 0 "player_model.glb"

    // Future syntax (not yet implemented):
    Animation "idle" "idle"      // Map Doom idle state to glTF "idle" animation
    Animation "walk" "walk"      // Map walk state to "walk" animation
    Animation "run" "run"
    Animation "attack" "attack"
    Animation "pain" "pain"
    Animation "death" "death"
}
```

### Multiple LOD Models

For performance, you can define multiple detail levels:

```
Model DoomPlayer
{
    Path "models/player"
    Model 0 "player_high.glb"    // High detail
    Model 1 "player_medium.glb"  // Medium detail
    Model 2 "player_low.glb"     // Low detail

    Scale 1.0 1.0 1.0
}
```

### PBR Material Optimization

**Best practices for game-ready PBR:**

1. **Texture sizes:**
   - Base Color: 2048x2048 (or 1024x1024 for optimization)
   - Normal Map: 2048x2048
   - Metallic/Roughness: 1024x1024 (combined in single texture)
   - Emissive: 512x512 (if used)

2. **Texture formats:**
   - PNG or JPEG for base color
   - PNG for normal maps (don't compress)
   - PNG for metallic/roughness

3. **Material properties:**
   - Roughness: 0.3-0.8 for most surfaces
   - Metallic: 0.0 for non-metals, 1.0 for metals
   - Avoid values in between for Metallic (unless specific effect)

### Custom Weapon Models

The same workflow applies to weapon replacements:

1. Model weapon in Blender
2. Rig with simple armature (if animated)
3. Create firing animation
4. Export as glTF
5. Use MODELDEF with weapon class name

Example MODELDEF for pistol:
```
Model Pistol
{
    Path "models/weapons"
    Model 0 "pistol.glb"
    Scale 1.0 1.0 1.0

    FrameIndex PISG A 0 0
}
```

### Monster Replacements

Replace Doom monsters with glTF models:

```
Model ZombieMan
{
    Path "models/monsters"
    Model 0 "zombie.glb"
    Scale 1.5 1.5 1.5

    FrameIndex POSS AB 0 0    // Walking
    FrameIndex POSS C 0 1     // Alert
    FrameIndex POSS D 0 2     // Attack
    FrameIndex POSS E 0 3     // Pain
    FrameIndex POSS F 0 4     // Death
}
```

### Blender Add-ons for Game Development

Recommended add-ons:

1. **Rigify** (built-in)
   - Advanced rigging system
   - Human meta-rig for quick character setup

2. **Texel Density Checker**
   - Ensures consistent texture resolution
   - Download: Blender Market

3. **glTF Validator**
   - Validates exports before using in game
   - Built into Blender 3.6+

4. **Sketchfab Integration**
   - Test glTF files online
   - Community feedback

---

## Quick Reference Card

### Essential Blender Hotkeys

| Key | Action |
|-----|--------|
| `Tab` | Toggle Edit/Object Mode |
| `Ctrl+Tab` | Toggle Pose Mode (armature) |
| `G` | Move (Grab) |
| `R` | Rotate |
| `S` | Scale |
| `X/Y/Z` (after G/R/S) | Constrain to axis |
| `Shift+A` | Add object/bone |
| `I` | Insert keyframe (animation) |
| `Ctrl+P` | Parent objects |
| `A` | Select all |
| `Alt+A` | Deselect all |
| `E` | Extrude |
| `U` | UV unwrap menu |

### glTF Export Quick Settings

```
Format: glTF Binary (.glb)
Include: ✅ Custom Properties, ✅ Animations
Transform: ✅ +Y Up
Geometry: ✅ UVs, ✅ Normals, ✅ Tangents, ✅ Materials
Animation: ✅ Animations, ✅ Skinning, Mode: Actions
```

### MODELDEF Template

```
Model DoomPlayer
{
    Path "models/player"
    Model 0 "player_model.glb"
    Scale 1.0 1.0 1.0

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

### PK3 Structure

```
MyMod.pk3 (ZIP archive)
├── MODELDEF
├── models/
│   └── player/
│       └── player_model.glb
└── (optional) textures/
    └── (optional texture files)
```

### Testing Command

```bash
./build/neodoom \
  -file MyPlayerMod.pk3 \
  -iwad doom2.wad \
  +map E1M1 \
  +chase_cam 1 \
  +developer 2
```

---

## Next Steps

Now that you have a working player replacement:

1. **Refine your model:**
   - Add more detail to the mesh
   - Improve textures with proper PBR maps
   - Fine-tune animations

2. **Create weapon models:**
   - Apply the same workflow to Doom weapons
   - Replace pistol, shotgun, chaingun, etc.

3. **Replace monsters:**
   - Create glTF versions of Doom enemies
   - Build your own monster cast

4. **Share your mod:**
   - Upload to Doom community sites
   - Share screenshots and videos
   - Get feedback from players

5. **Learn advanced features:**
   - Implement full animation systems (when available)
   - Use Blender's NLA Editor for complex animations
   - Explore procedural texturing with Shader Nodes

---

## Additional Resources

### NeoDoom Documentation

- NeoDoom GitHub: https://github.com/your-neodoom-repo
- glTF Implementation Status: `/docs/GLTF_IMPLEMENTATION_STATUS.md`
- Troubleshooting Guide: `/docs/GLTF_COMPILATION_FIXES.md`

### Blender Learning

- Official Tutorials: https://www.blender.org/support/tutorials/
- Blender Fundamentals: https://www.youtube.com/playlist?list=PLa1F2ddGya_-UvuAqHAksYnB0qL9yWDO6
- Character Modeling: Search "Blender character modeling tutorial"
- Rigging Basics: Search "Blender rigging tutorial"

### glTF Specification

- glTF 2.0 Spec: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
- glTF Validator: https://github.khronos.org/glTF-Validator/
- glTF Sample Models: https://github.com/KhronosGroup/glTF-Sample-Models

### Doom Modding Community

- Doom Wiki: https://doomwiki.org/
- ZDoom Forums: https://forum.zdoom.org/
- /r/Doom: https://reddit.com/r/Doom

---

## Credits and License

This tutorial was created for the NeoDoom project.

**Tutorial Author:** Claude Code (with human guidance)
**NeoDoom Development:** NeoDoom Team
**Blender:** Blender Foundation
**Doom:** id Software

**License:** This tutorial is released under CC-BY-SA 4.0. You are free to share and adapt this content with attribution.

---

**Good luck with your first NeoDoom player model! Remember: start simple, test often, and have fun!**

If you get stuck, check the Troubleshooting section or ask for help in the NeoDoom community.

**Happy modding!** 🎮👾
