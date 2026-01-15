# BiasedDoom glTF Tools

This directory contains tools for working with glTF 2.0 models in BiasedDoom.

## create-gltf-replacement.sh

**Professional interactive script for creating 3D model replacements**

### Features

✨ **Automated Structure Generation**
- Creates complete directory structure for your mod
- Generates pre-configured ZScript code
- Includes comprehensive documentation
- Sets up proper model/texture directories

🎯 **Asset Type Support**
- **Players** - Third-person character models with full animation
- **Monsters** - Enemy replacements with AI-compatible animations
- **Items** - Pickup items with floating/rotation effects
- **Weapons** - First-person viewmodels with fire/reload animations
- **Decorations** - Static or animated environment props

🔧 **Smart Code Generation**
- Blender-friendly bone names (Spine_01, UpperArm_R, etc.)
- Procedural animation examples (head tracking, breathing)
- PBR material setup code
- Animation state management
- Attachment point support (muzzle flashes, effects)

📚 **Complete Documentation**
- Asset-specific animation requirements
- Blender export settings
- Installation instructions
- Customization guide
- Troubleshooting tips

### Usage

```bash
cd /path/to/BiasedDoom
./tools/create-gltf-replacement.sh
```

### Interactive Flow

The script will guide you through:

1. **Asset Type Selection**
   ```
   What type of asset do you want to create?
     1) Player Model (DoomPlayer replacement)
     2) Monster/Enemy (e.g., Imp, Baron, etc.)
     3) Item/Pickup (e.g., Health, Armor, Ammo)
     4) Weapon (First-person viewmodel)
     5) Decoration (Static or animated prop)
   ```

2. **Basic Information**
   ```
   ? Class name (e.g., GLTFDoomGuy, GLTFImp): GLTFCoolImp
   ? Model filename (without extension) [gltfcoolimp]: coolimp
   ? Short description: A cool 3D Imp replacement
   ```

3. **Base Class** (for replacements)
   ```
   Common Doom monster classes:
     - DoomImp, ZombieMan, ShotgunGuy, ChaingunGuy
     - Demon (Pinky), Spectre, LostSoul, Cacodemon
     ...
   ? Base class to replace [DoomImp]: DoomImp
   ```

4. **Output Location**
   ```
   ? Output directory [wadsrc/static/models_gltf/gltfcoolimp]:
   ```

5. **Confirmation**
   ```
   Configuration:
     Type:        Monster
     Class:       GLTFCoolImp
     Replaces:    DoomImp
     Model:       models/gltfcoolimp/coolimp.gltf
     Output:      wadsrc/static/models_gltf/gltfcoolimp

   ? Create this structure? [Y/n]:
   ```

### Generated Structure

```
output_directory/
├── models/
│   └── classname/
│       ├── .gitkeep
│       └── textures/
│           └── .gitkeep
├── zscript/
│   └── ClassName.zs          # Complete ZScript implementation
├── docs/
│   └── README.md             # Detailed documentation
├── modeldef.txt              # Optional MODELDEF
└── zscript_include.txt       # Include reference
```

### Example: Create Player Model Replacement

```bash
$ ./tools/create-gltf-replacement.sh

╔════════════════════════════════════════════════════════════════╗
║                                                                ║
║           BiasedDoom glTF Model Replacement Generator             ║
║                                                                ║
║    Create 3D model replacements for Doom sprites (v2.0)       ║
║                                                                ║
╚════════════════════════════════════════════════════════════════╝

━━━ Asset Type Selection ━━━

What type of asset do you want to create?
  1) Player Model (DoomPlayer replacement)
  2) Monster/Enemy (e.g., Imp, Baron, etc.)
  3) Item/Pickup (e.g., Health, Armor, Ammo)
  4) Weapon (First-person viewmodel)
  5) Decoration (Static or animated prop)
  q) Quit

? Select option [1-5]: 1

━━━ Basic Information ━━━

? Class name (e.g., GLTFDoomGuy, GLTFImp): GLTFMarineGuy
? Model filename (without extension) [gltfmarineguy]: marine
? Short description [glTF model replacement for Player]: Modern marine player

━━━ Output Location ━━━

? Output directory [wadsrc/static/models_gltf/gltfmarineguy]:

━━━ Summary ━━━

Configuration:
  Type:        Player
  Class:       GLTFMarineGuy
  Model:       models/gltfmarineguy/marine.gltf
  Output:      wadsrc/static/models_gltf/gltfmarineguy

? Create this structure? [Y/n]: y

━━━ Creating Files ━━━

ℹ Creating directory structure...
✓ Directories created at: wadsrc/static/models_gltf/gltfmarineguy
✓ Created ZScript: wadsrc/static/models_gltf/gltfmarineguy/zscript/GLTFMarineGuy.zs
✓ Created MODELDEF: wadsrc/static/models_gltf/gltfmarineguy/modeldef.txt
✓ Created README: wadsrc/static/models_gltf/gltfmarineguy/docs/README.md
✓ Created include reference: wadsrc/static/models_gltf/gltfmarineguy/zscript_include.txt
✓ Created model directory structure

━━━ Next Steps ━━━

✓ Structure created successfully!

To complete your model replacement:

1. Create your glTF model in Blender:
   - Export as: glTF 2.0 Separate (.gltf + .bin + textures)
   - Save to: wadsrc/static/models_gltf/gltfmarineguy/models/gltfmarineguy/
   - See: wadsrc/static/models_gltf/gltfmarineguy/docs/README.md for animation requirements

2. Add to your mod's ZScript:
   - Add this line to your zscript.txt:
     #include "zscript/GLTFMarineGuy.zs"

3. Package your mod:
   cd wadsrc/static/models_gltf/gltfmarineguy && zip -r GLTFMarineGuy.pk3 *

4. Test in BiasedDoom:
   neodoom -file GLTFMarineGuy.pk3

Documentation:
  - Model guide:   docs/BLENDER_GLTF_MODELING_GUIDE.md
  - ZScript API:   GLTF_ZSCRIPT_API.md
  - Local README:  wadsrc/static/models_gltf/gltfmarineguy/docs/README.md

✓ Happy modding! 🎮
```

### Generated ZScript Features

The script generates production-ready ZScript with:

#### Player Models
- Idle, Walk, Run, Jump, Fall animations
- Head tracking (looks at view direction)
- Breathing animation (chest movement)
- Weapon aiming (procedural arm rotation)
- Pain and death animations
- Animation speed based on velocity

#### Monster Models
- Idle, Walk, Attack, Pain, Death animations
- AI-compatible state transitions
- Random death animation selection
- Melee and ranged attack support
- Face target behavior
- XDeath (gibbed) support

#### Item Models
- Floating/bobbing animation
- Rotation effect
- Optional pulsing glow for special items
- Idle/Float animation support

#### Weapon Models
- Idle, Select, Deselect, Fire, Reload animations
- First-person positioning
- Muzzle flash attachment points
- Animation-timed events (ejection, reload sounds)
- Optional reload state

#### Decoration Models
- Idle animation (looped)
- Solid/non-solid option
- PBR material support

### Advanced Features

All generated ZScript includes:

✅ **Comprehensive Error Handling**
- Model load validation
- Animation existence checks
- Console error messages
- Developer mode logging

✅ **Blender-Compatible Bone Names**
- Spine_01, Spine_02, Spine_03
- UpperArm_R/L, LowerArm_R/L, Hand_R/L
- UpperLeg_R/L, LowerLeg_R/L, Foot_R/L
- Head, Jaw, Tail bones

✅ **Procedural Animation Examples**
- Head look-at constraints
- Breathing effects
- Weapon aiming
- Dynamic scaling
- Bone rotation overrides

✅ **PBR Material Support**
- SetupPBRMetal(), SetupPBRPlastic(), SetupPBRStone()
- Dynamic emissive glow
- Material animation examples

✅ **Animation State Management**
- Velocity-based animation selection
- Smooth blending between states
- Animation speed modulation
- State tracking

### Customization After Generation

The generated ZScript is well-commented and easy to customize:

```zscript
// Adjust model scale
SetModelScaleUniform(1.0); // Change to 0.8, 1.5, etc.

// Adjust position
SetModelOffset(0, 0, 0); // Shift model position

// Adjust animation speed
SetAnimationSpeed(1.2); // 120% speed

// Add custom procedural animations
if (BoneExists("Tail")) {
    double wag = sin(level.time * 0.2) * 15;
    Quat tailRot = Quat.FromEulerAngles(0, wag, 0);
    SetBoneRotation("Tail", tailRot, 1.0);
}
```

### Requirements

- Bash 4.0+
- Unix-like environment (Linux, macOS, WSL on Windows)
- Write permissions in output directory

### Validation

The script validates:
- ✅ Class names (alphanumeric + underscore, starts with letter)
- ✅ Directory write permissions
- ✅ User confirmation before creating files

### Error Handling

- Clear error messages with color coding
- Graceful cancellation (press 'q' or Ctrl+C)
- Validation of all user inputs
- Directory existence checks

### Tips

**Naming Conventions:**
- Use PascalCase for class names: `GLTFDoomGuy`, `GLTFCoolImp`
- Use lowercase for model filenames: `doomguy`, `coolimp`
- Avoid spaces and special characters

**Output Location:**
- Default: `wadsrc/static/models_gltf/classname/`
- Custom: Any writable directory
- Recommendation: Keep organized by mod/pack

**Base Classes:**
- Use exact Doom class names (case-sensitive)
- Check ZDoom wiki for complete class list
- Common classes listed in interactive prompts

### Known Limitations

- Requires manual glTF model creation in Blender
- ZScript must be manually included in main zscript.txt
- No automatic PK3 packaging (use zip command)
- Animation names must match Blender export exactly

### See Also

- [GLTF_QUICK_START.md](../GLTF_QUICK_START.md) - Quick start guide
- [GLTF_ZSCRIPT_API.md](../GLTF_ZSCRIPT_API.md) - Complete API reference
- [BLENDER_GLTF_MODELING_GUIDE.md](../docs/BLENDER_GLTF_MODELING_GUIDE.md) - Blender workflow
- [GLTF_IMPLEMENTATION.md](../GLTF_IMPLEMENTATION.md) - Technical details

---

**Version:** 2.0
**Author:** NeoDoom glTF Team
**License:** MIT
