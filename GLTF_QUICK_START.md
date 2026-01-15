# BiasedDoom glTF Quick Start Guide

**Get started with 3D model replacements in under 10 minutes!**

## Prerequisites

- BiasedDoom with glTF support enabled
- Blender 3.0+ (for creating/exporting models)
- Basic familiarity with Doom modding (PK3 files, ZScript)

## Quick Start: Replace a Monster with a 3D Model

### Step 1: Generate the Structure (2 minutes)

Use the interactive script to generate all necessary files:

```bash
cd /path/to/BiasedDoom
./tools/create-gltf-replacement.sh
```

**Interactive prompts example:**
```
? What type of asset: Monster/Enemy
? Class name: GLTFCoolImp
? Model filename: coolimp
? Base class to replace: DoomImp
? Output directory: [default]
```

This creates:
```
wadsrc/static/models_gltf/gltfcoolimp/
├── models/
│   └── gltfcoolimp/
│       └── textures/
├── zscript/
│   └── GLTFCoolImp.zs      # Pre-configured ZScript
├── docs/
│   └── README.md            # Detailed instructions
├── modeldef.txt             # Optional MODELDEF
└── zscript_include.txt      # Include reference
```

### Step 2: Create Model in Blender (5 minutes)

#### Required Animations (name exactly as shown):
- **Idle** - Standing idle (looped)
- **Walk** - Movement (looped)
- **Attack_Melee** or **Attack_Ranged** - Attack animation
- **Pain** - Taking damage
- **Death1** - Death animation

#### Export Settings:
1. **File → Export → glTF 2.0**
2. **Format:** glTF Separate (.gltf + .bin + textures)
3. **Include:**
   - ✅ Apply Modifiers
   - ✅ UVs, Normals, Tangents
   - ✅ Animations
4. **Save to:** `models/gltfcoolimp/coolimp.gltf`

### Step 3: Package and Test (3 minutes)

```bash
cd wadsrc/static/models_gltf/gltfcoolimp

# Add ZScript include to your mod
echo '#include "zscript/GLTFCoolImp.zs"' >> ../../zscript.txt

# Package as PK3
zip -r GLTFCoolImp.pk3 models/ zscript/

# Test in BiasedDoom
neodoom -file GLTFCoolImp.pk3
```

**Done!** Your Imp is now a 3D model with smooth animations.

---

## Common Use Cases

### Replace Player Model (Third-Person)

```bash
./tools/create-gltf-replacement.sh
# Select: Player Model
# Class name: GLTFDoomGuy
```

**Required animations:**
- Idle, Walk, Run, Jump, Fall, Pain, Death1

**Blender bones (optional but recommended):**
- Head, Spine_01-03, UpperArm_L/R, LowerArm_L/R, Hand_L/R
- UpperLeg_L/R, LowerLeg_L/R, Foot_L/R

### Replace Weapon (First-Person)

```bash
./tools/create-gltf-replacement.sh
# Select: Weapon
# Class name: GLTFPistol
# Base class: Pistol
```

**Required animations:**
- Idle, Select, Deselect, Fire, Reload (optional)

**Blender bones (optional):**
- Muzzle (for muzzle flash attachment)
- Slide, Magazine, Bolt, Eject

### Replace Item/Pickup

```bash
./tools/create-gltf-replacement.sh
# Select: Item/Pickup
# Class name: GLTFMedikit
# Base class: Medikit
```

**Required animations:**
- Idle or Float (looped, rotating/bobbing)

### Add Custom Decoration

```bash
./tools/create-gltf-replacement.sh
# Select: Decoration
# Class name: GLTFTorch
# Is solid: no
```

**Required animations:**
- Idle (looped flame animation)

---

## Advanced Features

### Procedural Bone Animation

The generated ZScript includes examples of procedural animation. Customize in the ZScript file:

```zscript
// Head tracking (looks at target)
if (target) {
    AddBoneLookAt("Head", target.pos + (0, 0, 40), (0, 0, 1), 0.8);
}

// Breathing effect
double breathScale = 1.0 + sin(level.time * 0.03) * 0.03;
SetBoneScale("Spine_02", (1.0, breathScale, breathScale), 0.5);
```

### Dynamic Animation Blending

```zscript
// Blend walk to run based on speed
double speedFactor = vel.Length() / maxRunSpeed;
BlendAnimations("Walk", "Run", speedFactor, true);
```

### Material Customization

```zscript
// Setup PBR materials
SetupPBRMetal(0.9, 0.2);      // Shiny metal
SetupPBRPlastic(0.6);          // Matte plastic
SetupPBRStone(0.9);            // Rough stone

// Animated glow
PulseEmissive(Color(255, 255, 0, 0), 1.5, 0.0, 3.0);
```

---

## Blender Tips

### Correct Scale
- Model in real-world scale (1 unit = 1 meter in Blender)
- Doom player height: ~1.78m (56 units), radius: 0.5m (16 units)
- Apply scale before export: **Ctrl+A → Scale**

### Bone Naming Convention
Use Blender's standard naming (BiasedDoom's API recognizes these):
```
Head
Spine_01, Spine_02, Spine_03
UpperArm_R, LowerArm_R, Hand_R
UpperArm_L, LowerArm_L, Hand_L
UpperLeg_R, LowerLeg_R, Foot_R
UpperLeg_L, LowerLeg_L, Foot_L
```

### Animation Frame Rate
- **30 FPS** or **60 FPS** (consistent throughout project)
- Sample animation every frame for smooth playback

### Texture Settings
- **Base Color:** sRGB color space, 1024x1024 or 2048x2048
- **Normal Map:** Non-Color, tangent space
- **Metallic-Roughness:** Non-Color, packed texture
  - R: Unused/Occlusion
  - G: Roughness
  - B: Metallic

---

## Troubleshooting

### "Model not found" error
- ✅ Check path: `models/yourmodel/yourmodel.gltf`
- ✅ Ensure format is "glTF Separate" (.gltf), not GLB
- ✅ Check textures are in `models/yourmodel/textures/`

### Animations don't play
- ✅ Animation names must match exactly (case-sensitive)
- ✅ Enable "Animations" checkbox in Blender export
- ✅ Check console with `developer 1` for errors

### Model is wrong size
- ✅ In Blender: Apply scale (Ctrl+A → Scale)
- ✅ In ZScript: Adjust `SetModelScaleUniform(1.0)` value

### Textures missing or wrong colors
- ✅ Check texture color spaces in Blender
- ✅ Ensure textures are external files (not embedded)
- ✅ Use relative paths in glTF

### Performance issues
- ✅ Reduce polygon count (<50k vertices recommended)
- ✅ Use smaller textures (1024x1024 instead of 4096x4096)
- ✅ Limit bone count (<100 per model)

---

## File Paths Reference

### Doom Asset Types → Common Base Classes

**Players:**
- `DoomPlayer` - Base player class

**Monsters:**
- `ZombieMan`, `ShotgunGuy`, `ChaingunGuy`
- `DoomImp`, `Demon`, `Spectre`, `Cacodemon`, `LostSoul`
- `BaronOfHell`, `HellKnight`, `Mancubus`, `Arachnotron`
- `PainElemental`, `Revenant`, `Archvile`
- `SpiderMastermind`, `Cyberdemon`

**Items:**
- Health: `HealthBonus`, `Stimpack`, `Medikit`, `Soulsphere`, `Megasphere`
- Armor: `ArmorBonus`, `GreenArmor`, `BlueArmor`
- Ammo: `Clip`, `Shell`, `RocketAmmo`, `Cell`
- Powerups: `Berserk`, `InvulnerabilitySphere`, `Infrared`, `AllMap`

**Weapons:**
- `Fist`, `Chainsaw`, `Pistol`, `Shotgun`, `SuperShotgun`
- `Chaingun`, `RocketLauncher`, `PlasmaRifle`, `BFG9000`

**Decorations:**
- Lamps: `Column`, `TallGreenColumn`, `ShortGreenColumn`
- Torches: `RedTorch`, `GreenTorch`, `BlueTorch`
- Barrels: `ExplosiveBarrel`, `BurningBarrel`
- Tech: `TechPillar`, `TechLamp`, `TechLamp2`

---

## Example Workflow (Full Imp Replacement)

```bash
# 1. Generate structure
./tools/create-gltf-replacement.sh
# → Select Monster, name it GLTFImp, replace DoomImp

# 2. Create in Blender
# → Model your Imp (~56 units tall)
# → Add armature with bones: Root, Spine, Head, Arms, Legs
# → Create animations: Idle, Walk, Attack_Melee, Pain, Death1
# → Export: File → Export → glTF 2.0 Separate
# → Save to: wadsrc/static/models_gltf/gltfimp/models/gltfimp/gltfimp.gltf

# 3. Add to mod
cd wadsrc/static/models_gltf/gltfimp
echo '#include "zscript/GLTFImp.zs"' >> ../../zscript.txt

# 4. Customize ZScript (optional)
# Edit zscript/GLTFImp.zs:
#   - Adjust scale: SetModelScaleUniform(0.9)
#   - Add procedural animation
#   - Customize attack behavior

# 5. Test
zip -r GLTFImp.pk3 models/ zscript/
neodoom -iwad doom2.wad -file GLTFImp.pk3

# 6. Iterate
# → Tweak model/animations in Blender
# → Re-export (overwrites previous)
# → Re-zip and test
```

---

## API Quick Reference

### Model Management
```zscript
InitGLTFModel("path/to/model.gltf")  // Initialize
UpdateGLTFModel()                     // Update each tic
SetModelScaleUniform(1.0)             // Scale
SetModelOffset(0, 0, 0)               // Position offset
```

### Animation Control
```zscript
PlayAnimation("Idle", loop: true, blendTime: 0.2)
StopAnimation()
SetAnimationSpeed(1.5)
GetAnimationProgress()  // Returns 0.0-1.0
```

### Bone Manipulation
```zscript
SetBoneRotation("Head", rotation, 1.0)
SetBonePosition("Hand_R", pos, 1.0)
AddBoneLookAt("Head", targetPos, upAxis, 0.8)
BlendBonePose("Arm_R", rot1, rot2, factor)
```

### PBR Materials
```zscript
SetupPBRMetal(0.9, 0.3)
SetupPBRPlastic(0.6)
SetupPBRStone(0.9)
PulseEmissive(color, speed, min, max)
```

---

## Next Steps

- 📖 **Full ZScript API:** [GLTF_ZSCRIPT_API.md](GLTF_ZSCRIPT_API.md)
- 🎨 **Blender Guide:** [docs/BLENDER_GLTF_MODELING_GUIDE.md](docs/BLENDER_GLTF_MODELING_GUIDE.md)
- 🔧 **Implementation Details:** [GLTF_IMPLEMENTATION.md](GLTF_IMPLEMENTATION.md)
- 🚀 **Advanced Features:** [GLTF_V2_IMPROVEMENTS.md](GLTF_V2_IMPROVEMENTS.md)

---

**Happy Modding!** 🎮

For questions and support, see the [NeoDoom glTF documentation](GLTF_IMPLEMENTATION.md).
