# BiasedDoom glTF 2.0 Beginner's Tutorial
## Complete Step-by-Step Guide for Creating Custom Models

### 🎯 What You'll Learn
This tutorial will teach you how to create custom 3D models in Blender and use them in BiasedDoom to replace players, weapons, items, and monsters. We'll go from absolute basics to complete working mods, organized by difficulty.

**Important Note:** glTF support in BiasedDoom is currently under development (Phase 1 complete). This tutorial prepares you for when the feature is fully implemented, and provides workflows that will work with the current engine architecture.

---

## Table of Contents

1. [Understanding the Basics](#understanding-the-basics)
2. [Setting Up Your Tools](#setting-up-your-tools)
3. [Tutorial 1: Simple Prop (Easiest)](#tutorial-1-simple-prop-easiest)
4. [Tutorial 2: Animated Item (Easy)](#tutorial-2-animated-item-easy)
5. [Tutorial 3: Weapon Replacement (Medium)](#tutorial-3-weapon-replacement-medium)
6. [Tutorial 4: Monster Replacement (Advanced)](#tutorial-4-monster-replacement-advanced)
7. [Tutorial 5: Player Model (Expert)](#tutorial-5-player-model-expert)
8. [Creating PK3 Mods](#creating-pk3-mods)
9. [Troubleshooting Common Issues](#troubleshooting-common-issues)

---

## Understanding the Basics

### What is glTF?
glTF (GL Transmission Format) is a modern 3D file format that supports:
- **Geometry**: The shape of your 3D model
- **Materials**: How the model looks (colors, shininess, etc.)
- **Textures**: Images applied to the model's surface
- **Animations**: Movement and transformations
- **Skeletal Rigs**: Bones that control complex animations

### Why glTF for DOOM?
Traditional DOOM uses sprite-based graphics (2D images from different angles). BiasedDoom's glTF support allows you to use modern 3D models with:
- ✅ Smooth animations from any angle
- ✅ Modern lighting (PBR - Physically Based Rendering)
- ✅ Easy creation in Blender
- ✅ Professional game-ready workflows

### DOOM Units and Scale
- **1 DOOM unit = approximately 1 inch** (2.54 cm)
- **Player height**: ~56 units (about 4.5 feet tall)
- **Door height**: 128 units (about 10.5 feet)
- **In Blender**: Use 1 Blender unit = 1 DOOM unit for simplicity

---

## Setting Up Your Tools

### Required Software

#### 1. Blender 4.0 or Newer (Free)
Download from: https://www.blender.org/download/

**Why this version?** Blender 4.0+ has the best glTF 2.0 exporter with all the features we need.

#### 2. BiasedDoom with glTF Support
```bash
# Build BiasedDoom with glTF enabled
cd /path/to/BiasedDoom
ENABLE_GLTF=ON ENABLE_LTO=OFF ./build-arch.sh build
```

#### 3. Text Editor (Pick One)
- **VS Code** (Recommended): https://code.visualstudio.com/
- **Notepad++** (Windows): https://notepad-plus-plus.org/
- **Any text editor** that can edit plain text files

#### 4. Optional Helper Tools
```bash
# Install glTF validator (helps find errors)
npm install -g gltf-validator

# Or use the online validator:
# https://github.khronos.org/glTF-Validator/
```

### Setting Up Your Workspace

Create this folder structure for your mod:
```
MyFirstMod/
├── models/           # Your 3D models go here
│   ├── items/
│   ├── weapons/
│   ├── monsters/
│   └── players/
├── textures/         # Texture images
├── zscript/          # Game logic scripts
│   └── mymod.zs
├── ZSCRIPT          # Main ZScript file (tells DOOM to load your scripts)
├── sounds/           # Sound effects
└── MAPINFO          # Map and game configuration
```

---

## Tutorial 1: Simple Prop (Easiest)
### Creating a Spinning Keycard

**What You'll Learn:**
- Basic Blender modeling
- glTF export settings
- Simple animation
- Creating a PK3 mod

**Estimated Time:** 30-45 minutes

### Step 1: Create the Model in Blender

**⚠️ Before You Start:**
- Save your Blender file often (`Ctrl + S`)
- Work in a dedicated project folder
- Keep backups of working versions

1. **Open Blender** and delete the default cube:
   - Select it (click on it)
   - Press `X` → Delete
   - **Checkpoint:** Empty scene with camera and light only

2. **Add a simple shape**:
   - Press `Shift + A` → Mesh → Cube
   - This will be our keycard base
   - **Checkpoint:** Cube appears at origin (0, 0, 0)

3. **Scale it to keycard proportions**:
   - Press `S` (Scale) → `Z` (Z-axis only) → `0.1` → Enter
   - Press `S` → `X` → `0.6` → Enter
   - You now have a thin, card-like shape
   - **Checkpoint:** Properties panel shows scale still at (1, 1, 1) - we'll fix this next

4. **Apply the scale** (⚠️ VERY IMPORTANT):
   - Press `Ctrl + A` → "All Transforms"
   - **Why this matters:** Blender stores two types of scale:
     - Visual scale (what you see)
     - Data scale (what gets exported)
   - Applying transforms makes them match
   - **Checkpoint:** Properties panel (press `N`) shows Scale: X:1.0, Y:1.0, Z:1.0
   - **If it doesn't work:** You may still be in Edit Mode - press `Tab` to return to Object Mode

5. **Add some detail** (optional):
   - Tab into Edit Mode (`Tab` key)
   - Select the top face (`Alt + Click` on the top)
   - Press `I` (Inset) and move mouse slightly → Click
   - Press `E` (Extrude) → `0.05` → Enter
   - Tab back to Object Mode
   - **Checkpoint:** Keycard has raised detail on top

**🔍 Validation Checklist:**
- [ ] Object is in Object Mode (bottom left shows "Object Mode")
- [ ] Scale shows (1, 1, 1) in Properties panel
- [ ] Location shows (0, 0, 0)
- [ ] Rotation shows (0°, 0°, 0°)

### Step 2: Add a Simple Material

1. **Switch to Shading workspace**:
   - Click "Shading" at the top of Blender

2. **Add material**:
   - Select your keycard object
   - In the bottom shader editor, click "New"
   - You'll see a "Principled BSDF" node

3. **Set the color**:
   - Click the white "Base Color" box
   - Choose a bright color (e.g., red for a red keycard)

4. **Add shininess** (optional):
   - Set "Metallic" to 0.3
   - Set "Roughness" to 0.4

### Step 3: Create UV Mapping

1. **Enter Edit Mode**: Press `Tab`

2. **Select all**: Press `A` (selects all faces)

3. **Unwrap**:
   - Press `U` → "Smart UV Project" → OK

4. **Return to Object Mode**: Press `Tab`

### Step 4: Add a Simple Rotation Animation

1. **Set up the timeline**:
   - Look at the timeline at the bottom
   - Set end frame to 120 (4 seconds at 30 fps)

2. **Create the animation**:
   - Make sure your keycard is selected
   - Go to frame 1 (press `Home` or drag the blue timeline marker)
   - Press `I` → "Rotation" (this creates a keyframe)

3. **Set the end rotation**:
   - Drag timeline to frame 120
   - Press `R` → `Z` → `360` → Enter (rotate 360° on Z-axis)
   - Press `I` → "Rotation" (another keyframe)

4. **Name the animation**:
   - Open the Dope Sheet editor (top menu)
   - Click the animation name field and rename to "Spin"

### Step 5: Export as glTF

1. **File → Export → glTF 2.0**

2. **Critical Settings**:
   ```
   Format: glTF Separate (.gltf + .bin + textures)  ← MUST SELECT THIS

   Include:
   ✅ Selected Objects (if only keycard selected)
   ✅ UVs
   ✅ Normals
   ✅ Tangents
   ✅ Materials

   Transform:
   ✅ +Y Up

   Animation:
   ✅ Animations
   ✅ All Actions
   Sampling Rate: 30

   ❌ Draco Compression (DO NOT ENABLE)
   ❌ KTX2 Textures (DO NOT ENABLE)
   ```

3. **Export Location**:
   - Navigate to `MyFirstMod/models/items/`
   - Create folder: `redkey`
   - Save as: `redkey.gltf`

### Step 6: Create the ZScript Definition

Create: `MyFirstMod/zscript/items.zs`

```javascript
// Red Keycard Replacement
class ModernRedCard : RedCard replaces RedCard
{
    Default
    {
        // Model properties
        +DONTGIB
        +BRIGHT  // Makes it glow
    }

    States
    {
    Spawn:
        // When glTF is fully supported, this will use the model
        // For now, uses traditional sprites as fallback
        RKEY A -1 Bright;
        Stop;
    }
}

/*
   FUTURE glTF SYNTAX (when fully implemented):

   Default
   {
       Model.Path "models/items/redkey/redkey.gltf";
       Model.Animation "Spin";
       Model.AnimationSpeed 1.0;
       Model.Scale 8.0;  // Adjust to match DOOM's scale
   }
*/
```

Create: `MyFirstMod/ZSCRIPT`
```
version "4.10"

#include "zscript/items.zs"
```

### Step 7: Create the PK3 File

1. **Verify your structure**:
```
MyFirstMod/
├── models/
│   └── items/
│       └── redkey/
│           ├── redkey.gltf
│           └── redkey.bin
├── zscript/
│   └── items.zs
└── ZSCRIPT
```

2. **Create PK3** (it's just a ZIP file):
   - **Windows**: Select all files/folders → Right-click → Send to → Compressed folder
   - **Linux/Mac**: `zip -r MyFirstMod.pk3 models/ zscript/ ZSCRIPT`
   - **Important**: Rename the `.zip` to `.pk3`

3. **Test in NeoDoom**:
```bash
./neodoom -file MyFirstMod.pk3 -iwad doom2.wad
```

---

## Tutorial 2: Animated Item (Easy)
### Creating a Health Pack with Bobbing Animation

**What You'll Learn:**
- Texture painting basics
- Creating looping animations
- Using Blender's animation tools
- PBR materials

**Estimated Time:** 1-2 hours

### Step 1: Model the Health Pack

1. **Start fresh** in Blender (File → New → General)

2. **Create the base**:
   - Add Cube (`Shift + A` → Mesh → Cube)
   - Scale: `S` → `Z` → `0.4` → Enter (make it flatter)
   - Apply Transforms: `Ctrl + A` → All Transforms

3. **Add a cross detail**:
   - Tab into Edit Mode
   - Select the top face
   - `I` (Inset) → `0.2` → Enter
   - `E` (Extrude) → `0.1` → Enter

4. **Model a cross shape**:
   - In Edit Mode, add a vertical bar:
     - `Shift + A` → Mesh → Cube
     - `S` → `X` → `0.3` → `Z` → `1.5` → Enter
   - Add horizontal bar:
     - `Shift + D` (Duplicate) → `R` → `Z` → `90` → Enter
   - Join them: Select all (`A`) → `Ctrl + J`

### Step 2: UV Unwrapping

1. **Select your model** and enter Edit Mode (`Tab`)

2. **Mark seams** (where the texture will "split"):
   - Select edges where faces meet at 90° angles
   - Press `Ctrl + E` → "Mark Seam"
   - Do this for main edges of your box

3. **Unwrap**:
   - Select all (`A`)
   - Press `U` → "Unwrap"

4. **Check UV layout**:
   - Open UV Editor (top menu)
   - You should see your model's faces laid flat

### Step 3: Create a Texture

1. **Switch to Texture Paint workspace**

2. **Create new texture**:
   - Select your object
   - In Texture Paint mode: Image → New
   - Name: `healthpack_color`
   - Size: 1024 x 1024
   - ✅ 32-bit Float (for better quality)
   - Color: White
   - Click OK

3. **Paint the texture**:
   - Select a red color
   - Paint on the 3D model
   - The UV Editor shows where you're painting on the texture
   - Tips:
     - Use `F` to change brush size
     - `Shift` + click to draw straight lines
     - Paint a red cross and white background

4. **Save the texture**:
   - In UV Editor: Image → Save As
   - Save to: `MyFirstMod/models/items/healthpack/textures/healthpack_color.png`

### Step 4: Set Up PBR Material

1. **Switch to Shading workspace**

2. **Connect your texture**:
   - You'll see the Principled BSDF node
   - Press `Shift + A` → Texture → Image Texture
   - Click "Open" → Select your saved `healthpack_color.png`
   - Connect the "Color" output to "Base Color" input

3. **Adjust PBR properties**:
   - Metallic: 0.0 (not metal)
   - Roughness: 0.6 (slightly rough plastic)

4. **Optional - Add normal map for depth**:
   - Create another Image Texture node
   - Create new image: `healthpack_normal` (1024x1024, color: RGB 128,128,255)
   - Add "Normal Map" node (`Shift + A` → Vector → Normal Map)
   - Connect: Image → Normal Map → Principled BSDF "Normal"

### Step 5: Create Bobbing Animation

1. **Set animation length**:
   - End frame: 60 (2 seconds at 30 fps)

2. **Create up-down motion**:
   - Frame 1: Select object → `I` → Location
   - Frame 15: `G` → `Z` → `0.3` → Enter → `I` → Location
   - Frame 30: `G` → `Z` → `-0.3` → Enter → `I` → Location
   - Frame 45: `G` → `Z` → `0.3` → Enter → `I` → Location
   - Frame 60: `G` → `Z` → `0` → Enter → `I` → Location

3. **Make it loop smoothly**:
   - Open Graph Editor
   - Select all keyframes (`A`)
   - Press `T` → "Bezier"
   - Channel → Extrapolation Mode → "Make Cyclic"

4. **Name the animation**:
   - In Dope Sheet: Rename to "Float"

### Step 6: Export Settings

Same as Tutorial 1, but ensure:
```
✅ Tangents (for normal maps)
✅ All Actions
Sampling Rate: 30
```

Export to: `MyFirstMod/models/items/healthpack/healthpack.gltf`

### Step 7: ZScript for Health Pack

Create: `MyFirstMod/zscript/items.zs`

```javascript
class ModernHealthBonus : HealthBonus replaces HealthBonus
{
    Default
    {
        +COUNTITEM
        +INVENTORY.ALWAYSPICKUP
        Inventory.Amount 1;
        Inventory.MaxAmount 200;
        Inventory.PickupMessage "Picked up a health pack.";
    }

    States
    {
    Spawn:
        BON1 ABCDCB 6 Bright;
        Loop;
    }
}

/*
   FUTURE glTF VERSION:
   Default
   {
       Model.Path "models/items/healthpack/healthpack.gltf";
       Model.Animation "Float";
       Model.AnimationSpeed 1.0;
       Model.Scale 6.0;
   }
*/
```

### Testing Checklist
- [ ] Model exports without errors
- [ ] Textures are in correct folder
- [ ] glTF file size is reasonable (< 5MB)
- [ ] Animation plays in Blender
- [ ] PK3 structure is correct

---

## Tutorial 3: Weapon Replacement (Medium)
### Creating a Modern Pistol

**What You'll Learn:**
- More complex modeling
- Multiple animations (idle, fire, reload)
- Weapon positioning
- Muzzle flash attachment points

**Estimated Time:** 3-4 hours

### Step 1: Reference and Planning

Before modeling, plan your weapon:
- **DOOM pistol size**: Approximately 16 units long, 8 units tall
- **Animations needed**:
  - `Idle`: Slight breathing sway
  - `Fire`: Recoil and muzzle movement
  - `Reload`: Magazine swap (optional)
  - `Select`: Weapon raise
  - `Deselect`: Weapon lower

### Step 2: Model the Pistol

**Basic Pistol Shape:**

1. **Start with a cube**:
   - `Shift + A` → Mesh → Cube
   - `S` → `X` → `2` → `Z` → `0.6` → Enter
   - This is your pistol grip

2. **Add the barrel**:
   - In Edit Mode, select the front face
   - `E` (Extrude) → `1.5` → Enter
   - `S` → `0.7` → Enter (make it narrower)

3. **Add the trigger guard**:
   - `Shift + A` → Mesh → Cylinder
   - `R` → `Y` → `90` → Enter
   - `S` → `0.3` → Enter
   - Position below grip with `G` (grab/move)

4. **Add detail** (slide, sights, etc.):
   - Use extrude (`E`) and inset (`I`) for detail
   - Keep it simple - around 2000-5000 triangles total

5. **Mirror for symmetry**:
   - Add Mirror Modifier
   - Delete half the mesh
   - Modifier will create the other side

### Step 3: Create Armature (Bones) for Animation

**Why bones?** Even weapons benefit from skeletal animation for recoil, reload, etc.

1. **Add an Armature**:
   - In Object Mode: `Shift + A` → Armature → Single Bone
   - Name it "Weapon_Rig"

2. **Position the bone**:
   - Enter Edit Mode (`Tab` while armature selected)
   - Move the bone to align with the pistol grip
   - This will be the "Root" bone

3. **Add bones for moving parts**:
   - Select the bone tip
   - `E` (Extrude) → Move to create new bone
   - Create bones for:
     - `Bone_Root`: Main weapon bone
     - `Bone_Slide`: For slide movement on fire
     - `Bone_Grip`: For grip position
     - `Bone_Muzzle`: For muzzle flash attachment point

4. **Parent mesh to armature**:
   - In Object Mode: Select Pistol → Shift-select Armature
   - `Ctrl + P` → "With Automatic Weights"

5. **Paint weights** (tells which bone controls which part):
   - Select pistol → Tab → Weight Paint mode
   - Select each bone and paint influence
   - Example: Slide bone should control only the slide portion

### Step 4: UV Unwrap and Texture

1. **Unwrap the mesh**:
   - Edit Mode → Select All (`A`)
   - Smart UV Project (`U` → Smart UV Project)

2. **Create weapon texture**:
   - Use Texture Paint or external tool (GIMP, Photoshop)
   - Create: `pistol_color.png` (base color)
   - Create: `pistol_normal.png` (for detail bumps)
   - Create: `pistol_mr.png` (metallic + roughness combined)

3. **Set up material**:
   - Base Color: Connect `pistol_color.png` (set to sRGB)
   - Normal: Connect `pistol_normal.png` via Normal Map node (set to Non-Color)
   - Metallic: 0.8 (metal slide)
   - Roughness: 0.3 (somewhat shiny)

### Step 5: Create Animations

**Animation: Idle**

1. **Timeline**: Frames 1-60 (2 second loop)
2. **Create subtle sway**:
   - Frame 1: `I` → LocRot (keyframe location + rotation)
   - Frame 15: Rotate slightly up → `I` → LocRot
   - Frame 30: Return to center → `I` → LocRot
   - Frame 45: Rotate slightly down → `I` → LocRot
   - Frame 60: Return to start → `I` → LocRot

**Animation: Fire**

1. **New Action**:
   - In Action Editor: Click "New" → Name it "Fire"
   - Timeline: Frames 1-15 (0.5 seconds)

2. **Recoil animation**:
   - Frame 1: Starting position → `I` → LocRotScale
   - Frame 3:
     - Move slide back: Select Bone_Slide → `G` → `Y` → `-0.2` → Enter
     - Rotate weapon up slightly: `R` → `X` → `15` → Enter
     - Keyframe all: `I` → LocRotScale
   - Frame 8: Slide returns forward → `I` → LocRotScale
   - Frame 15: Return to start position → `I` → LocRotScale

**Animation: Reload**

1. **New Action**: "Reload", 60 frames (2 seconds)
2. **Create magazine drop animation**:
   - Lower weapon
   - Rotate magazine out (if you modeled separate mag)
   - Raise weapon back up
   - Keyframe at 10-frame intervals

**Animation: Select (Raise)**

1. **New Action**: "Select", 20 frames
2. **Animate from below screen**:
   - Frame 1: Move weapon down with `G` → `Z` → `-1.5`
   - Frame 20: Normal position
   - Keyframe both positions

**Animation: Deselect (Lower)**

1. **New Action**: "Deselect", 15 frames
2. **Reverse of Select**: Move weapon down and slightly rotate

### Step 6: Export for NeoDoom

**Export Settings**:
```
Format: glTF Separate (.gltf + .bin + textures)

Include:
✅ Selected Objects
✅ Apply Modifiers
✅ UVs
✅ Normals
✅ Tangents
✅ Materials
✅ Skinning (important for bones!)

Animation:
✅ All Actions
Sampling Rate: 30
✅ Optimize Animation Size

Transform:
✅ +Y Up
```

Export to: `MyFirstMod/models/weapons/pistol/pistol.gltf`

### Step 7: ZScript Weapon Definition

Create: `MyFirstMod/zscript/weapons.zs`

```javascript
class ModernPistol : Pistol replaces Pistol
{
    Default
    {
        Weapon.SelectionOrder 1900;
        Weapon.AmmoUse 1;
        Weapon.AmmoGive 20;
        Weapon.AmmoType "Clip";
        Obituary "$OB_MPPISTOL";
        +WEAPON.WIMPY_WEAPON
        Inventory.Pickupmessage "$PICKUP_PISTOL";
        Tag "$TAG_PISTOL";
    }

    States
    {
    Ready:
        PISG A 1 A_WeaponReady;
        Loop;

    Deselect:
        PISG A 1 A_Lower;
        Loop;

    Select:
        PISG A 1 A_Raise;
        Loop;

    Fire:
        PISG A 4;
        PISG B 6 A_FirePistol;
        PISG C 4;
        PISG B 5 A_ReFire;
        Goto Ready;
    }
}

/*
   FUTURE glTF VERSION:

   Default
   {
       Model.Path "models/weapons/pistol/pistol.gltf";
       Model.Scale 2.5;
       Model.Offset (0, 0, 28);  // Position in front of player view
       Model.AngleOffset 0;
       Model.RollOffset 0;
       Model.PitchOffset 0;
   }

   States
   {
   Ready:
       #### A 1
       {
           A_WeaponReady();
           Model.PlayAnimation("Idle", loop:true);
       }
       Loop;

   Select:
       #### A 0 Model.PlayAnimation("Select");
       #### A 20 A_Raise;
       Goto Ready;

   Fire:
       #### A 0 Model.PlayAnimation("Fire");
       #### A 4 A_FirePistol;
       #### A 10 A_ReFire;
       Goto Ready;
   }
*/
```

### Weapon Positioning Tips

When glTF is fully supported, you'll need to position the weapon correctly:

- **Offset**: How far from the player's view center
  - X: Left/Right (negative = left)
  - Y: Forward/Back (positive = forward)
  - Z: Up/Down (positive = up)

- **Typical pistol offset**: `(0, 15, 28)`
- **Typical shotgun offset**: `(0, 18, 25)`
- **Typical rifle offset**: `(0, 20, 30)`

### Testing Your Weapon

1. **Package as PK3**:
```
MyFirstMod.pk3/
├── models/
│   └── weapons/
│       └── pistol/
│           ├── pistol.gltf
│           ├── pistol.bin
│           └── textures/
│               ├── pistol_color.png
│               ├── pistol_normal.png
│               └── pistol_mr.png
├── zscript/
│   └── weapons.zs
└── ZSCRIPT
```

2. **Test**:
```bash
./neodoom -file MyFirstMod.pk3 -iwad doom2.wad +map map01 +give Pistol
```

---

## Tutorial 4: Monster Replacement (Advanced)
### Creating a Modern Demon

**What You'll Learn:**
- Character modeling
- Complex skeletal rigs
- Walk cycles and attack animations
- Sprite replacement techniques
- AI integration

**Estimated Time:** 8-12 hours

### Overview

Replacing a DOOM monster is complex because you need:
- Multiple animations (walk, attack, pain, death)
- Proper scaling to match original monster size
- Hitbox considerations
- Multiple texture variants (for different damage states)

### Step 1: Planning Your Monster

**Reference the Original**:
- DOOM Demon (Pinky):
  - Height: 56 units
  - Width: 60 units (radius: 30)
  - Speed: 10 units/tic
  - Health: 150
  - Melee attack range: 64 units

**Required Animations**:
1. **Idle**: Breathing, slight movement
2. **Walk**: 4-8 frame walk cycle
3. **Attack**: Bite/claw animation
4. **Pain**: Recoil from damage
5. **Death**: Falling/dying animation
6. **Raise** (optional): For Arch-Vile resurrection

### Step 2: Model the Demon

**Character Modeling Workflow**:

1. **Start with a base mesh**:
   - Use reference images (front, side, top views)
   - Start with UV Sphere or Cube
   - Model one half, use Mirror modifier

2. **Block out major forms**:
   - Head/skull: 8 units tall
   - Body: 30 units long, 20 units wide
   - Legs: 18 units long each
   - Arms/claws: 15 units

3. **Add detail progressively**:
   - Start with 500 polygons (basic shape)
   - Add subdivision for 2000 polygons (detail)
   - Final model: 5000-15000 polygons max

4. **Topology tips**:
   - Edge loops around joints (shoulders, hips, knees)
   - Quad faces (4 vertices) where possible
   - Triangles only in flat areas
   - No N-gons (5+ vertices)

### Step 3: Create the Armature (Skeleton)

**Bone Structure**:

1. **Create main chain** (spine):
   ```
   Root (pelvis)
   ├── Spine1
   ├── Spine2
   ├── Spine3
   └── Neck
       └── Head
   ```

2. **Add leg chains** (both sides):
   ```
   Leg_Upper.L/R
   ├── Leg_Lower.L/R
   └── Foot.L/R
       └── Toe.L/R
   ```

3. **Add arm chains**:
   ```
   Shoulder.L/R
   ├── Arm_Upper.L/R
   ├── Arm_Lower.L/R
   └── Hand.L/R
       ├── Finger1.L/R
       ├── Finger2.L/R
       └── Claw.L/R
   ```

4. **Total bones**: ~30-40 for a bipedal creature

**Setting Up IK (Inverse Kinematics)**:
- Helps animate legs and arms naturally
- Add IK targets for feet and hands
- Pole targets for knees and elbows

### Step 4: Weight Painting

**What is Weight Painting?**
Weight painting tells each vertex (point on the mesh) which bone controls it and how much influence that bone has.

1. **Automatic Weights first**:
   - Select Mesh → Shift-select Armature
   - `Ctrl + P` → "With Automatic Weights"
   - This gives you a starting point

2. **Fix problem areas**:
   - Enter Weight Paint mode
   - Select bone from list
   - Paint influence (red = 100%, blue = 0%)
   - Fix areas where wrong bones affect geometry

3. **Common problem zones**:
   - Shoulder/arm connection
   - Hip/leg connection
   - Neck/head transition
   - Elbow and knee joints

4. **Testing weights**:
   - Enter Pose Mode (`Ctrl + Tab`)
   - Move bones around
   - Check for unnatural deformation
   - Return to Weight Paint and fix

### Step 5: UV Mapping and Textures

1. **UV Unwrapping**:
   - Mark seams along natural body divisions
   - Typical seams:
     - Down the back
     - Inside of arms and legs
     - Around neck
   - `U` → "Unwrap"

2. **Optimize UV layout**:
   - Important areas (head, hands) get more texture space
   - Less visible areas (back, feet) get less space
   - Use `UV → Pack Islands` to organize

3. **Create textures**:
   - **Base Color** (Diffuse): Main appearance
     - Size: 2048x2048 recommended
     - Paint in Blender or external tool
     - Add demon skin, scales, details

   - **Normal Map**: Surface detail/bumps
     - Bake from high-poly sculpt, or
     - Generate from photos, or
     - Paint manually

   - **Metallic-Roughness**: Material properties
     - R channel: Occlusion (shadows)
     - G channel: Roughness (shininess)
     - B channel: Metallic (is it metal?)

### Step 6: Create Animations

**Animation 1: Idle (30 frames)**

1. **Pose Mode** → Select all bones (`A`)
2. **Create subtle breathing**:
   - Frame 1: Neutral pose → `I` → LocRotScale
   - Frame 8: Chest expands slightly
   - Frame 15: Return to neutral
   - Frame 22: Chest contracts slightly
   - Frame 30: Back to frame 1 pose

**Animation 2: Walk Cycle (24 frames)**

A walk cycle is the most important animation - spend time getting it right!

1. **Key poses**:
   - Frame 1: Contact (right foot forward)
   - Frame 6: Passing position (right leg passes left)
   - Frame 12: Contact (left foot forward)
   - Frame 18: Passing position (left leg passes right)
   - Frame 24: Return to frame 1

2. **Details to add**:
   - Opposite arm swings (right foot forward = left arm forward)
   - Spine rotates slightly with step
   - Head stays mostly level (counter-rotate neck)
   - Hips rotate and tilt
   - Center of mass shifts

3. **Walk cycle tips**:
   - Use "Copy Mirrored Pose" to mirror left/right sides
   - Keep feet planted (no sliding)
   - Add overlapping action (tail lags behind body)

**Animation 3: Attack (20 frames)**

1. **Wind-up** (frames 1-8):
   - Lean back
   - Raise arms/claws
   - Open mouth

2. **Strike** (frames 9-12):
   - Lunge forward
   - Swing claws down
   - Close mouth on bite

3. **Recovery** (frames 13-20):
   - Return to neutral stance
   - Prepare to return to Idle

**Animation 4: Pain (10 frames)**

1. **Impact** (frames 1-3):
   - Body jerks back
   - Arms go wide

2. **Recovery** (frames 4-10):
   - Return to stance
   - Ready to continue walking/attacking

**Animation 5: Death (40 frames)**

1. **Death Types** (make 2-3 variations):
   - **Death1**: Forward fall
     - Stumble forward (frames 1-15)
     - Fall to knees (frames 16-25)
     - Fall face-first (frames 26-40)

   - **Death2**: Backward fall
     - Stagger back (frames 1-10)
     - Spin slightly (frames 11-20)
     - Collapse backward (frames 21-40)

2. **Final pose**: Should work with floor collision

**Animation Best Practices**:
- Save each animation as a separate Action
- Name them clearly: `Demon_Idle`, `Demon_Walk`, etc.
- Use NLA Editor to organize multiple animations
- Test each animation loops smoothly
- Maintain consistent 30 FPS

### Step 7: Export Configuration

```
Format: glTF Separate (.gltf + .bin + textures)

Include:
✅ Selected Objects (if only exporting demon)
✅ Apply Modifiers
✅ UVs
✅ Normals
✅ Tangents
✅ Skinning
✅ Materials

Animation:
✅ All Actions
Sampling Rate: 30
✅ Optimize Animation Size
✅ Group by NLA Track

Compression:
❌ Draco (DO NOT USE)
```

Export to: `MyFirstMod/models/monsters/demon/demon.gltf`

### Step 8: ZScript Monster Definition

Create: `MyFirstMod/zscript/monsters.zs`

```javascript
class ModernDemon : Demon replaces Demon
{
    Default
    {
        Health 150;
        Radius 30;
        Height 56;
        Mass 400;
        Speed 10;
        PainChance 180;
        Monster;
        +FLOORCLIP
        SeeSound "demon/sight";
        AttackSound "demon/melee";
        PainSound "demon/pain";
        DeathSound "demon/death";
        ActiveSound "demon/active";
        Obituary "$OB_DEMONHIT";
        Tag "$FN_DEMON";
    }

    States
    {
    Spawn:
        SARG AB 10 A_Look;
        Loop;

    See:
        SARG AABBCCDD 2 A_Chase;
        Loop;

    Melee:
        SARG EF 8 A_FaceTarget;
        SARG G 8 A_SargAttack;
        Goto See;

    Pain:
        SARG H 2;
        SARG H 2 A_Pain;
        Goto See;

    Death:
        SARG I 8;
        SARG J 8 A_Scream;
        SARG K 4;
        SARG L 4 A_NoBlocking;
        SARG M 4;
        SARG N -1;
        Stop;

    Raise:
        SARG N 5;
        SARG MLKJI 5;
        Goto See;
    }
}

/*
   FUTURE glTF VERSION:

   Default
   {
       Model.Path "models/monsters/demon/demon.gltf";
       Model.Scale 1.0;  // Adjust to match original size
       Model.Offset (0, 0, 0);

       // Animation mappings
       Model.Animation "Idle" { State "Spawn"; Loop true; }
       Model.Animation "Walk" { State "See"; Loop true; }
       Model.Animation "Attack" { State "Melee"; }
       Model.Animation "Pain" { State "Pain"; }
       Model.Animation "Death1" { State "Death"; Random 0.5; }
       Model.Animation "Death2" { State "Death"; Random 0.5; }
   }
*/
```

### Step 9: Scale and Position Testing

**Scaling Issues**:
- Blender units don't always match DOOM units perfectly
- You may need to adjust `Model.Scale` in ZScript

**To Find the Right Scale**:
1. Place a cube in DOOM map that's 56 units tall (demon height)
2. Load your model
3. Compare sizes
4. Adjust `Model.Scale` value up or down
5. Typical range: 0.5 to 2.0

**Hitbox Alignment**:
- The model's origin point should be at the monster's feet
- In Blender: Set 3D cursor to ground level → Select model → `Shift + Ctrl + Alt + C` → "Origin to 3D Cursor"

---

## Tutorial 5: Player Model (Expert)
### Creating a Custom Player Character

**What You'll Learn:**
- First-person arm rig
- Third-person full body rig
- Weapon holding positions
- Player animation state machine
- View bobbing integration

**Estimated Time:** 15-20 hours

### Overview

Player models are the most complex because they need:
1. **First-person arms** (visible to player)
2. **Third-person body** (visible to others in multiplayer)
3. **Multiple weapon holding animations**
4. **Pain/death animations**
5. **Movement animations** (walk, run, strafe)

### Part 1: First-Person Arms

These are the arms the player sees holding weapons.

**Step 1: Model the Arms**

1. **Start with reference**:
   - Use photos of arms from player's perspective
   - DOOM arm proportions:
     - Upper arm: 15 units
     - Lower arm: 12 units
     - Hand: 8 units

2. **Model right arm only** (it's mirrored in DOOM):
   - Start from shoulder
   - Include part of torso (for realistic clothing)
   - Model to elbow
   - Model to wrist
   - Model hand with fingers

3. **Detail level**:
   - Arms: 2000-3000 polygons
   - Hands: 1000-1500 polygons each
   - Fingers need good topology for posing

**Step 2: Rig the Arms**

```
Armature hierarchy:
Shoulder
├── Upper_Arm
└── Lower_Arm
    └── Hand
        ├── Thumb_Base → Thumb_Mid → Thumb_Tip
        ├── Index_Base → Index_Mid → Index_Tip
        ├── Middle_Base → Middle_Mid → Middle_Tip
        ├── Ring_Base → Ring_Mid → Ring_Tip
        └── Pinky_Base → Pinky_Mid → Pinky_Tip
```

**Step 3: Create Weapon Grip Poses**

Create "rest pose" for each weapon type:
- **Pistol grip**: Trigger finger extended, others wrapped
- **Rifle grip**: Both hands (if creating two-handed weapons)
- **Melee grip**: Fist or weapon-appropriate grip

Save each as separate Action:
- `Arms_Pistol_Idle`
- `Arms_Pistol_Fire`
- `Arms_Shotgun_Idle`
- `Arms_Shotgun_Fire`
- etc.

### Part 2: Third-Person Full Body

**Step 1: Model the Full Character**

1. **Proportions** (DOOM Marine):
   - Total height: 56 units
   - Head: 8 units
   - Torso: 20 units
   - Legs: 28 units
   - Shoulders: 16 units wide

2. **Armor details**:
   - Model armor plates as separate layers
   - Or bake detail into normal map
   - Keep polygon count under 15,000

**Step 2: Create Full Body Rig**

```
Full body armature:
Root
├── Pelvis
│   ├── Spine_Lower
│   ├── Spine_Mid
│   ├── Spine_Upper
│   └── Neck
│       └── Head
├── Leg_Upper.L/R
│   ├── Leg_Lower.L/R
│   └── Foot.L/R
│       └── Toe.L/R
└── Shoulder.L/R
    ├── Arm_Upper.L/R
    ├── Arm_Lower.L/R
    └── Hand.L/R
        └── [5 finger chains]
```

**Step 3: Player Animations**

**Animation: Idle** (60 frames)
- Breathing motion
- Slight weapon sway
- Relaxed stance

**Animation: Walk** (24 frames)
- Standard walk cycle
- Arms swing opposite to legs
- Weapon moves with body

**Animation: Run** (20 frames)
- Faster walk cycle
- More exaggerated arm swing
- Leaning forward slightly

**Animation: Strafe_Left** (24 frames)
- Upper body faces forward
- Lower body sidesteps left
- Weapon stays aimed forward

**Animation: Strafe_Right** (24 frames)
- Mirror of strafe left

**Animation: Jump** (30 frames)
- Crouch (frames 1-8)
- Launch (frames 9-15)
- Air (frames 16-24)
- Land (frames 25-30)

**Animation: Pain** (10 frames)
- Recoil backward
- Arms react
- Return to idle

**Animation: Death** (40 frames)
- Multiple variations
- Fall forward, backward, left, right
- Final pose must work with ground

### Step 4: UV and Textures

**Texture Sets**:
1. **Skin/Face**: 1024x1024
   - Face details
   - Neck, hands
   - Use higher resolution for quality

2. **Armor/Body**: 2048x2048
   - Armor plates
   - Clothing
   - Equipment

3. **Normal Maps**: Same sizes as color maps
   - Armor detail
   - Fabric wrinkles
   - Surface damage

**Material Setup**:
- Armor: High metallic (0.7-0.9), low roughness (0.2-0.4)
- Fabric: Low metallic (0.0), higher roughness (0.6-0.8)
- Skin: No metallic (0.0), medium roughness (0.5)

### Step 5: Export

Export two separate files:
1. **FP Arms**: `models/players/marine/marine_arms.gltf`
2. **TP Body**: `models/players/marine/marine_body.gltf`

### Step 6: ZScript Player Definition

Create: `MyFirstMod/zscript/player.zs`

```javascript
class ModernMarinePlayer : PlayerPawn
{
    Default
    {
        Health 100;
        Radius 16;
        Height 56;
        Mass 100;
        PainChance 255;
        Speed 1;
        +NOSKIN
        +PICKUP
        Player.DisplayName "Modern Marine";
        Player.CrouchSprite "PLYC";
        Player.StartItem "Pistol";
        Player.StartItem "Fist";
        Player.StartItem "Clip", 50;
        Player.WeaponSlot 1, "Fist", "Chainsaw";
        Player.WeaponSlot 2, "Pistol";
        Player.WeaponSlot 3, "Shotgun", "SuperShotgun";
        Player.WeaponSlot 4, "Chaingun";
        Player.WeaponSlot 5, "RocketLauncher";
        Player.WeaponSlot 6, "PlasmaRifle";
        Player.WeaponSlot 7, "BFG9000";
        Player.ColorRange 112, 127;
        Player.Colorset 0, "Green", 0x70, 0x7F, 0x72;
        Player.Colorset 1, "Gray", 0x60, 0x6F, 0x62;
        Player.Colorset 2, "Brown", 0x40, 0x4F, 0x42;
        Player.Colorset 3, "Red", 0x20, 0x2F, 0x22;
    }

    States
    {
    Spawn:
        PLAY A -1;
        Stop;

    See:
        PLAY ABCD 4;
        Loop;

    Missile:
    Melee:
        PLAY E 12;
        Goto Spawn;

    Pain:
        PLAY F 4;
        PLAY F 4 A_Pain;
        Goto Spawn;

    Death:
        PLAY G 10;
        PLAY H 10 A_PlayerScream;
        PLAY I 10 A_NoBlocking;
        PLAY J 10;
        PLAY K -1;
        Stop;

    XDeath:
        PLAY L 5;
        PLAY M 5 A_XScream;
        PLAY N 5 A_NoBlocking;
        PLAY OPQRS 5;
        PLAY T -1;
        Stop;
    }
}

/*
   FUTURE glTF VERSION:

   Default
   {
       // First-person arms
       Model.FPArms "models/players/marine/marine_arms.gltf";
       Model.FPArmsScale 1.0;
       Model.FPArmsOffset (0, 10, 20);

       // Third-person body
       Model.TPBody "models/players/marine/marine_body.gltf";
       Model.TPBodyScale 1.0;

       // Animation state mapping
       Model.Animation "Idle" { States "Spawn"; Loop true; }
       Model.Animation "Walk" { States "See"; Loop true; Speed 1.0; }
       Model.Animation "Run" { States "See"; Loop true; Speed 1.5; }
       Model.Animation "Pain" { States "Pain"; }
       Model.Animation "Death1" { States "Death"; Random 0.33; }
       Model.Animation "Death2" { States "Death"; Random 0.33; }
       Model.Animation "Death3" { States "XDeath"; Random 0.34; }
   }
*/
```

### Testing Player Models

**In Singleplayer**:
```bash
./neodoom -file MyFirstMod.pk3 -iwad doom2.wad +map map01 +playerclass ModernMarinePlayer
```

**View Third-Person**:
- Open console (`~`)
- Type: `chase_cam 1`
- See your full body model

---

## Creating PK3 Mods

### What is a PK3?
A PK3 is just a **ZIP file** with a `.pk3` extension that contains all your mod files in a specific structure.

### Basic PK3 Structure

```
MyMod.pk3 (rename from MyMod.zip)
├── models/              # All 3D models
│   ├── items/
│   │   └── healthpack/
│   │       ├── healthpack.gltf
│   │       ├── healthpack.bin
│   │       └── textures/
│   │           ├── healthpack_color.png
│   │           └── healthpack_normal.png
│   ├── monsters/
│   ├── players/
│   └── weapons/
├── textures/            # Sprite textures, HUD graphics
├── sounds/              # Sound effects (.ogg, .wav)
├── music/               # Music files (.ogg, .mp3)
├── zscript/             # ZScript code files
│   ├── items.zs
│   ├── weapons.zs
│   ├── monsters.zs
│   └── player.zs
├── ZSCRIPT             # Main ZScript loader (no extension)
├── MAPINFO             # Map/game configuration (no extension)
├── SNDINFO             # Sound definitions (no extension)
├── DECORATE            # Legacy actor definitions (optional)
└── README.txt          # Mod documentation
```

### Essential Files

#### 1. ZSCRIPT (Main Loader)
```
version "4.10"

// Load all your ZScript files
#include "zscript/items.zs"
#include "zscript/weapons.zs"
#include "zscript/monsters.zs"
#include "zscript/player.zs"
```

#### 2. MAPINFO (Optional but Recommended)
```
GameInfo
{
    Name = "My Awesome Mod"
    PlayerClasses = "ModernMarinePlayer"
}
```

#### 3. SNDINFO (If Using Custom Sounds)
```
// Define custom sounds
weapons/pistol/fire     "sounds/pistol_fire.ogg"
weapons/shotgun/fire    "sounds/shotgun_fire.ogg"
monsters/demon/sight    "sounds/demon_see.ogg"
```

### Creating the PK3

#### Windows
1. Select all your mod folders and files
2. Right-click → "Send to" → "Compressed (zipped) folder"
3. Rename `MyMod.zip` to `MyMod.pk3`

#### Linux/Mac
```bash
cd MyModFolder
zip -r MyMod.pk3 models/ zscript/ textures/ sounds/ ZSCRIPT MAPINFO
```

#### Using 7-Zip (Windows, best quality)
1. Select files → Right-click → 7-Zip → "Add to archive"
2. Archive format: ZIP
3. Compression level: Normal
4. Save as: `MyMod.pk3`

### Testing Your PK3

**Basic test**:
```bash
./neodoom -file MyMod.pk3 -iwad doom2.wad
```

**Test with specific map**:
```bash
./neodoom -file MyMod.pk3 -iwad doom2.wad +map e1m1
```

**Test with console commands**:
```bash
./neodoom -file MyMod.pk3 -iwad doom2.wad +map map01 +give all
```

### Common PK3 Mistakes

❌ **Wrong folder structure**:
```
MyMod.pk3/
└── MyMod/          ← WRONG! Don't nest in another folder
    └── models/
```

✅ **Correct structure**:
```
MyMod.pk3/
├── models/         ← Files start at root of PK3
├── zscript/
└── ZSCRIPT
```

❌ **Case sensitivity** (Linux/Mac):
```
ZSCRIPT → loading "zscript/Items.zs"  ← Won't work if file is "items.zs"
```

✅ **Match case exactly**:
```
ZSCRIPT → loading "zscript/items.zs"  ← Matches actual filename
```

❌ **Missing required files**:
```
MyMod.pk3/
├── models/
└── zscript/
    └── weapons.zs
# Missing ZSCRIPT file! Mod won't load.
```

✅ **Include all required files**:
```
MyMod.pk3/
├── models/
├── zscript/
│   └── weapons.zs
└── ZSCRIPT         ← Required!
```

### Advanced: Load Order

If you have multiple PK3s, load order matters:
```bash
# Later files override earlier ones
./neodoom -file BaseWeapons.pk3 EnhancedWeapons.pk3 -iwad doom2.wad
```

---

## Mod Examples by Difficulty

### Level 1: Simple Replacements
**No animations, static models**

**Pickups**:
- Health packs
- Armor
- Keycards
- Ammo boxes

**What you need**:
- Basic model (500-2000 polygons)
- 1 texture (512x512 or 1024x1024)
- Simple ZScript replacement
- No animations required (or simple rotation)

### Level 2: Animated Items
**Simple looping animations**

**Examples**:
- Floating powerups (bobbing)
- Spinning items
- Torches with fire
- Animated decorations

**What you need**:
- Model with 1000-3000 polygons
- 1-2 textures
- 1 looping animation (30-60 frames)
- Basic material setup

### Level 3: Weapons
**Multiple animations, no AI**

**Examples**:
- All DOOM weapons
- Custom melee weapons
- Projectile weapons

**What you need**:
- Model with 2000-5000 polygons
- Optional bones for moving parts
- 3-5 animations (idle, fire, reload, select, deselect)
- 2-3 textures (color, normal, metallic-roughness)
- ZScript weapon states

### Level 4: Monsters
**Complex animations, AI integration**

**Examples**:
- Zombies (simple humanoid)
- Imps (humanoid with wings)
- Demons (quadruped)
- Cacodemons (floating)

**What you need**:
- Model with 5000-15000 polygons
- Full skeleton (20-40 bones)
- 6-8 animations (idle, walk, attack, pain, 2-3 deaths)
- 2-4 textures
- Advanced ZScript with AI states
- Proper hitbox setup

### Level 5: Player Characters
**Most complex, first and third person**

**What you need**:
- FP arms model (3000-5000 polygons)
- TP body model (10000-20000 polygons)
- Full skeleton (50+ bones)
- 10+ animations
- 4-6 textures
- Multiple weapon holding poses
- Complex ZScript player class

---

## Debugging Your glTF Models

### Console Commands for glTF Debugging

NeoDoom provides several console commands to help debug your models:

**Enable Developer Mode:**
```
developer 1    // Show warnings
developer 2    // Show detailed debug info
developer 3    // Show verbose logging
```

**Check Model Loading:**
```
summon MyGLTFActor
// Console will show:
// [glTF INFO] Loading model: models/myactor.gltf
// [glTF INFO] Found 5 animations
// [glTF INFO] Found 23 bones
```

**List Available Animations:**
```javascript
// In your actor's Spawn state, add:
TNT1 A 0 NoDelay
{
    if (InitGLTFModel("models/test.gltf"))
    {
        ListAnimations();  // Prints all available animations
    }
}
```

**List All Bones:**
```javascript
TNT1 A 0 NoDelay
{
    if (InitGLTFModel("models/test.gltf"))
    {
        ListBones();  // Prints all bone names
    }
}
```

**Get Model Info:**
```javascript
TNT1 A 1
{
    if (level.time % 35 == 0)  // Every second
    {
        Console.Printf(GetModelInfo());
    }
    UpdateGLTFModel();
}
```

### Step-by-Step Debugging Process

#### 1. **Model Won't Load**

**Symptoms:**
- Actor spawns but invisible
- Console shows "Error: Native model loading failed"

**Debug Steps:**

```javascript
class DebugGLTFActor : Actor
{
    mixin GLTFModel;

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            // Step 1: Check path
            String path = "models/test/test.gltf";
            Console.Printf("Attempting to load: %s", path);

            // Step 2: Try to initialize
            if (!InitGLTFModel(path))
            {
                Console.Printf("FAILED: Model did not initialize");
                Console.Printf("Check:");
                Console.Printf("1. File exists in PK3");
                Console.Printf("2. Path is correct (case-sensitive on Linux)");
                Console.Printf("3. File is valid glTF (test with online validator)");
                return;
            }

            Console.Printf("SUCCESS: Model initialized");

            // Step 3: Check model info
            Console.Printf(GetModelInfo());
        }
        TNT1 A -1;
        Stop;
    }
}
```

**Common Fixes:**
- ✅ Check PK3 structure: `models/` folder at root of PK3
- ✅ Verify file exists: `unzip -l MyMod.pk3 | grep test.gltf`
- ✅ Test glTF file: Use https://github.khronos.org/glTF-Validator/
- ✅ Check for typos: `test.gltf` vs `Test.gltf` vs `test.glb`

#### 2. **Animation Won't Play**

**Symptoms:**
- Model appears but doesn't animate
- Console shows "Animation 'Walk' not found"

**Debug Steps:**

```javascript
Spawn:
    TNT1 A 0 NoDelay
    {
        if (InitGLTFModel("models/demon/demon.gltf"))
        {
            // List all available animations
            Console.Printf("Available animations:");
            ListAnimations();

            // This will show something like:
            // Available animations for models/demon/demon.gltf:
            //   0: Idle
            //   1: Walk
            //   2: Attack
            //   3: Death

            // Try to play animation
            if (!PlayAnimation("Walk", loop: true))
            {
                Console.Printf("Failed to play 'Walk' animation");
                Console.Printf("Trying alternative names...");

                // Try common variations
                if (PlayAnimation("walk", loop: true))
                    Console.Printf("Found lowercase 'walk'");
                else if (PlayAnimation("WALK", loop: true))
                    Console.Printf("Found uppercase 'WALK'");
                else if (PlayAnimation("walk_cycle", loop: true))
                    Console.Printf("Found 'walk_cycle'");
                else
                    Console.Printf("Animation not found - check Blender Action names");
            }
        }
    }
    Loop;
```

**Common Fixes:**
- ✅ Check Blender Action names (must match exactly, case-sensitive)
- ✅ Ensure animations were exported (enable "All Actions" in glTF exporter)
- ✅ Verify animation has keyframes (not empty Action)
- ✅ Check animation is assigned to armature, not mesh

#### 3. **Model is Wrong Size**

**Symptoms:**
- Model appears too big or too small
- Model clips through floor or floats

**Debug with Visual Reference:**

```javascript
Spawn:
    TNT1 A 0 NoDelay
    {
        InitGLTFModel("models/demon/demon.gltf");

        // Try different scales and report
        double[] testScales = { 0.5, 0.75, 1.0, 1.5, 2.0 };
        double testScale = testScales[0];  // Start with first

        Console.Printf("Testing scale: %.2f", testScale);
        Console.Printf("Actor radius: %.2f, height: %.2f", radius, height);
        Console.Printf("DOOM Imp for reference: radius 20, height 56");

        SetModelScaleUniform(testScale);
    }
    TNT1 A 1
    {
        // Press keys to adjust scale in real-time
        // (This would require user input handling)
        UpdateGLTFModel();
    }
    Loop;
```

**Scale Finding Tool:**

```javascript
// Add to actor class
double debugScale = 1.0;

// In Spawn state
TNT1 A 1
{
    // Adjust with console: "puke 1" to increase, "puke 2" to decrease
    SetModelScaleUniform(debugScale);
    Console.Printf("Current scale: %.2f", debugScale);
    UpdateGLTFModel();
}
```

#### 4. **Bone Override Not Working**

**Symptoms:**
- AddBoneOverride() has no visible effect
- Console shows "Bone 'Head' not found"

**Debug Steps:**

```javascript
Spawn:
    TNT1 A 0 NoDelay
    {
        if (InitGLTFModel("models/character/character.gltf"))
        {
            // List all bones first
            Console.Printf("=== BONE LISTING ===");
            ListBones();

            // Test each bone
            String[] bonesToTest = { "Head", "head", "HEAD", "head_bone" };
            foreach (boneName : bonesToTest)
            {
                if (ValidateBoneName(boneName, "test"))
                {
                    Console.Printf("✓ Found bone: %s", boneName);
                    break;
                }
                else
                {
                    Console.Printf("✗ Not found: %s", boneName);
                }
            }
        }
    }
```

**Common Bone Names:**
- Humanoid: `"Spine"`, `"Chest"`, `"Neck"`, `"Head"`, `"Arm_L"`, `"Arm_R"`
- Weapon: `"Root"`, `"Slide"`, `"Muzzle"`, `"Magazine"`
- Check Blender's bone names in Pose Mode (exactly as shown)

#### 5. **Performance Issues / Lag**

**Symptoms:**
- Game slows down with glTF models
- Frame rate drops significantly

**Performance Profiling:**

```javascript
class PerformanceTestActor : Actor
{
    mixin GLTFModel;

    int frameCount;
    double totalTime;

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            InitGLTFModel("models/complex/complex.gltf");
            frameCount = 0;
            totalTime = 0;
        }
        TNT1 A 1
        {
            double startTime = MSTime();  // Get current time in ms
            UpdateGLTFModel();
            double endTime = MSTime();

            double deltaTime = endTime - startTime;
            totalTime += deltaTime;
            frameCount++;

            if (frameCount % 35 == 0)  // Every second
            {
                double avgTime = totalTime / frameCount;
                Console.Printf("Avg update time: %.2f ms/frame", avgTime);

                if (avgTime > 1.0)
                    Console.Printf("WARNING: Update is slow!");
            }
        }
        Loop;
    }
}
```

**Performance Fixes:**
- ✅ Reduce polygon count in Blender (< 15,000 triangles)
- ✅ Use smaller textures (2048x2048 max, prefer 1024x1024)
- ✅ Limit bone overrides to essential bones only
- ✅ Don't call UpdateGLTFModel() multiple times per frame
- ✅ Use LOD (Level of Detail) for distant models

### Common Error Messages Explained

**"Error: Empty model path"**
```
Cause: Called InitGLTFModel("") or InitGLTFModel(null)
Fix: Provide valid path string
```

**"Error: Failed to create model definition"**
```
Cause: Out of memory or internal allocation failure
Fix: Reduce model complexity, check system resources
```

**"Warning: Bone 'Head' not found"**
```
Cause: Bone name doesn't match Blender bone name
Fix: Use ListBones() to see available bones, match exactly
```

**"Error: Failed to start animation 'Fire'"**
```
Cause: Animation name not in glTF file
Fix: Use ListAnimations() to see available animations
```

**"Warning: Negative blend time -0.5, clamping to 0"**
```
Cause: Passed negative value to PlayAnimation()
Fix: Use positive blend time values (0.0 to 1.0 typical)
```

### Validation Workflow

**Before asking for help, complete this checklist:**

1. **File Validation:**
   ```bash
   # Validate glTF file
   gltf-validator models/test/test.gltf
   # Should show: "No errors found"
   ```

2. **PK3 Structure:**
   ```bash
   # Check PK3 contents
   unzip -l MyMod.pk3
   # Verify: models/ at root, not nested in subfolder
   ```

3. **Blender Export:**
   - [ ] Format: glTF Separate (.gltf + .bin + textures)
   - [ ] Include → Normals: ON
   - [ ] Include → Tangents: ON
   - [ ] Include → Materials: ON
   - [ ] Animations → All Actions: ON
   - [ ] Compression: OFF (no Draco)

4. **ZScript Code:**
   - [ ] mixin GLTFModel added to actor
   - [ ] InitGLTFModel() called with NoDelay
   - [ ] UpdateGLTFModel() called every tic
   - [ ] Using TNT1 sprite (not regular sprites)

5. **Console Output:**
   ```
   developer 2  // Enable detailed logging
   summon MyGLTFActor
   // Read console output carefully
   ```

6. **Test with Simple Model:**
   - If your complex model fails, test with a simple cube
   - Gradually add complexity (animations, bones, textures)
   - Identifies where the problem starts

## Troubleshooting Common Issues

### Blender Issues

**Problem: Model exports but looks wrong in-game**
```
Cause: Unapplied transforms
Fix:
1. Select object in Blender
2. Ctrl + A → "All Transforms"
3. Re-export
```

**Problem: Textures are black/missing**
```
Cause: Wrong color space or missing files
Fix:
1. Check Image Texture nodes
2. Base Color: sRGB
3. Normal/Metallic/Roughness: Non-Color
4. Ensure textures are in correct folder
```

**Problem: Animation doesn't export**
```
Cause: Action not selected for export
Fix:
1. In export dialog
2. Animation section
3. ✅ Enable "All Actions" or "NLA Strips"
```

**Problem: Model is wrong size**
```
Cause: Scale not applied before export
Fix:
1. In Blender: Object Mode
2. Select model
3. Ctrl + A → "Scale"
4. Check dimensions (N key → Dimensions panel)
```

**Problem: Bones don't work / model doesn't skin**
```
Cause: No weight painting or wrong vertex groups
Fix:
1. Select mesh → Shift-select armature
2. Ctrl + P → "With Automatic Weights"
3. Enter Weight Paint mode
4. Check each bone has influence
```

### Export Issues

**Problem: Export fails with error**
```
Error: "No JSON object could be decoded"
Cause: File path has special characters
Fix:
- Use only letters, numbers, underscore (_), hyphen (-)
- No spaces in file paths
- No special characters (!, @, #, etc.)
```

**Problem: Exported file is huge (>50MB)**
```
Cause: Embedded textures or high poly count
Fix:
1. Use glTF Separate (not GLB)
2. Reduce texture sizes (max 2048x2048)
3. Decimate high-poly models
4. Don't embed textures
```

**Problem: Normals are flipped/backwards**
```
Cause: Incorrect face orientation
Fix:
1. Edit Mode → Select All (A)
2. Shift + N (Recalculate Outside)
3. Re-export
```

### NeoDoom / ZScript Issues

**Problem: Model doesn't appear in-game**
```
Cause: Model path incorrect or glTF not enabled
Fix:
1. Check path in ZScript: "models/items/thing/thing.gltf"
2. Ensure NeoDoom built with ENABLE_GLTF=ON
3. Check console for errors (~ key)
```

**Problem: Model appears but animations don't play**
```
Cause: Animation names don't match or not exported
Fix:
1. In Blender: Check Action names
2. Export with "All Actions" enabled
3. In ZScript: Match animation names exactly (case-sensitive)
```

**Problem: Model is too bright/dark**
```
Cause: Emissive material or wrong lighting
Fix:
1. Check Principled BSDF → Emissive = 0
2. Adjust Emissive Strength
3. Or add lights in map for better lighting
```

**Problem: Mod doesn't load**
```
Cause: PK3 structure wrong or missing files
Fix:
1. Open PK3 with 7-Zip or WinRAR
2. Files should be at root (not in subfolder)
3. Check ZSCRIPT file exists at root
4. Check all #include paths are correct
```

**Problem: "Script error" on startup**
```
Cause: ZScript syntax error
Fix:
1. Read the error message (tells you line number)
2. Common errors:
   - Missing semicolon (;)
   - Mismatched braces { }
   - Wrong capitalization
   - Missing comma in lists
3. Use text editor with syntax highlighting
```

### Performance Issues

**Problem: Game lags with custom model**
```
Cause: Too many polygons or large textures
Fix:
- Reduce polygon count (< 15000 for monsters)
- Reduce texture size (2048x2048 max)
- Use simpler materials
- Check topology for n-gons
```

**Problem: Animation is choppy**
```
Cause: Too few keyframes or wrong interpolation
Fix:
1. In Blender Graph Editor
2. Select all keyframes
3. Key → Interpolation Mode → Bezier
4. Or add more keyframes
```

---

## Best Practices Summary

### Modeling
- ✅ Keep polygon count reasonable (see guidelines above)
- ✅ Use quads where possible, triangles in flat areas
- ✅ Apply all transforms before export
- ✅ Use Mirror modifier for symmetry
- ✅ Keep mesh manifold (no holes or loose vertices)

### Textures
- ✅ Use power-of-two sizes (512, 1024, 2048)
- ✅ Set correct color spaces (sRGB for color, Non-Color for data)
- ✅ Keep textures in separate folder next to .gltf
- ✅ Use PNG for lossless, JPEG only for photos
- ✅ Don't exceed 2048x2048 unless necessary

### Animation
- ✅ Name actions clearly and consistently
- ✅ Bake at 30 FPS for consistency
- ✅ Make loops seamless (first and last frame match)
- ✅ Test in Blender before export
- ✅ Keep animations smooth (enough keyframes)

### Rigging
- ✅ Limit to 4 bone weights per vertex
- ✅ Normalize weights
- ✅ Test pose before exporting
- ✅ Name bones clearly (L/R for left/right)
- ✅ Keep bone count reasonable (< 100)

### Export
- ✅ Always use glTF Separate format
- ✅ Enable Normals and Tangents
- ✅ Include all required animations
- ✅ Don't use Draco or KTX2 compression
- ✅ Validate with glTF Validator

### Organization
- ✅ Keep clean folder structure
- ✅ Use consistent naming (lowercase, no spaces)
- ✅ Document your mod (README.txt)
- ✅ Version control (Git) recommended
- ✅ Test frequently during development

---

## Next Steps

### Learning More

**Blender Resources**:
- Official Blender Manual: https://docs.blender.org/
- Blender Guru YouTube tutorials
- Grant Abbitt (game asset tutorials)

**DOOM Modding**:
- ZDoom Wiki: https://zdoom.org/wiki/
- DOOM Builder tutorials
- /r/Doom community

**glTF Specification**:
- Khronos glTF Guide: https://www.khronos.org/gltf/
- glTF Viewer for testing

### Practice Projects

**Beginner**:
1. Replace all keycards with custom models
2. Create a custom health pack variant
3. Make a simple spinning decoration

**Intermediate**:
1. Create a full weapon replacement pack
2. Replace one monster type completely
3. Make a themed decoration set (tech, hell, etc.)

**Advanced**:
1. Create a completely new monster with custom AI
2. Full player model with all animations
3. Total conversion mod with all assets

---

## Conclusion

Creating glTF models for NeoDoom combines modern 3D modeling with classic DOOM gameplay. Start small, practice each skill, and gradually work up to more complex projects.

**Remember**:
- glTF support is still under development - features may change
- Test frequently to catch issues early
- Join the community for help and feedback
- Have fun creating!

### Getting Help

**Community Resources**:
- NeoDoom GitHub: Issues and discussions
- ZDoom Forums: Modding help
- Blender Stack Exchange: Modeling questions
- Discord communities for DOOM modding

**Debugging Steps**:
1. Check console for errors (~ key in NeoDoom)
2. Validate glTF file with online validator
3. Test model in glTF viewer before using in game
4. Compare working examples
5. Ask community with specific error messages

Happy modding! 🎮
