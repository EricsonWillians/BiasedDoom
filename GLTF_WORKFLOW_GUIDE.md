# BiasedDoom glTF 2.0 Professional Workflow Guide

## Executive Summary

This comprehensive guide establishes professional workflows for integrating Blender-exported glTF 2.0 models with skeletal animations into BiasedDoom. It covers complete asset replacement strategies for players, monsters, items, weapons, and environmental objects while maintaining compatibility with existing DOOM gameplay mechanics and mod frameworks.

---

## Table of Contents

1. [Prerequisites & Environment Setup](#prerequisites--environment-setup)
2. [Blender glTF Export Workflow](#blender-gltf-export-workflow)
3. [Asset Category Integration](#asset-category-integration)
4. [Animation System Integration](#animation-system-integration)
5. [Material & Texture Workflow](#material--texture-workflow)
6. [Performance Optimization](#performance-optimization)
7. [Quality Assurance & Testing](#quality-assurance--testing)
8. [Deployment & Distribution](#deployment--distribution)
9. [Troubleshooting & Best Practices](#troubleshooting--best-practices)
10. [Advanced Techniques](#advanced-techniques)

---

## Prerequisites & Environment Setup

### Required Software Stack

#### Primary Tools
- **Blender 4.0+** - Latest LTS recommended for glTF 2.0 export
- **BiasedDoom** - With glTF support enabled (`NEODOOM_ENABLE_GLTF=ON`)
- **Git** - For version control and asset management
- **Text Editor** - VS Code, Sublime Text, or similar for ZScript editing

#### Asset Pipeline Tools
- **glTF Validator** - Khronos Group official validator
- **glTF Viewer** - For asset verification and debugging
- **Texture Compressor** - For optimized texture delivery
- **Model Optimizer** - For mesh optimization and LOD generation

#### Development Environment
```bash
# Verify BiasedDoom glTF support
./neodoom --version | grep "glTF Support: Enabled"

# Install asset pipeline tools
npm install -g gltf-validator
pip install gltf-viewer
```

### Directory Structure

```
MyMod/
├── assets/
│   ├── models/
│   │   ├── players/
│   │   ├── monsters/
│   │   ├── weapons/
│   │   ├── items/
│   │   └── environment/
│   ├── textures/
│   │   ├── diffuse/
│   │   ├── normal/
│   │   ├── metallic-roughness/
│   │   └── emissive/
│   └── animations/
│       ├── player/
│       ├── monsters/
│       └── weapons/
├── zscript/
│   ├── actors/
│   ├── weapons/
│   └── items/
├── maps/
├── sounds/
└── scripts/
    ├── validate_assets.sh
    ├── optimize_textures.sh
    └── deploy_mod.sh
```

---

## Blender glTF Export Workflow

### 1. Model Preparation

#### Scene Setup Guidelines
```python
# Blender Python script for scene validation
import bpy

def validate_scene():
    issues = []

    # Check for non-applied transforms
    for obj in bpy.context.scene.objects:
        if obj.type == 'MESH':
            if any(obj.location) or any(obj.rotation_euler) or any(s != 1.0 for s in obj.scale):
                issues.append(f"Object {obj.name} has unapplied transforms")

    # Validate armature hierarchy
    armatures = [obj for obj in bpy.context.scene.objects if obj.type == 'ARMATURE']
    for armature in armatures:
        if len(armature.data.bones) > 128:  # BiasedDoom bone limit
            issues.append(f"Armature {armature.name} exceeds 128 bone limit")

    return issues
```

#### Essential Pre-Export Checklist
- [ ] **Apply All Transforms** - `Ctrl+A → Apply All Transforms`
- [ ] **Validate Bone Count** - Maximum 128 bones per armature
- [ ] **Check Bone Hierarchy** - No disconnected bone chains
- [ ] **Verify UV Mapping** - All meshes have proper UV coordinates
- [ ] **Material Assignment** - All materials use Principled BSDF
- [ ] **Animation Validation** - All animations baked and properly keyed

### 2. Armature & Rigging Standards

#### Bone Naming Convention
```
// Player Character Bones
Root
├── Hips
│   ├── Spine_01
│   ├── Spine_02
│   ├── Spine_03
│   ├── Neck
│   └── Head
├── UpperLeg_L / UpperLeg_R
├── LowerLeg_L / LowerLeg_R
├── Foot_L / Foot_R
├── UpperArm_L / UpperArm_R
├── LowerArm_L / LowerArm_R
└── Hand_L / Hand_R
    ├── Thumb_01_L / Thumb_01_R
    ├── Index_01_L / Index_01_R
    ├── Middle_01_L / Middle_01_R
    ├── Ring_01_L / Ring_01_R
    └── Pinky_01_L / Pinky_01_R

// Weapon Attachment Points
Weapon_Attach_R
Weapon_Attach_L
Weapon_Muzzle
Weapon_Eject
```

#### Rigging Best Practices
- **Bone Constraints**: Use only export-compatible constraints
- **Weight Painting**: Maximum 4 influences per vertex
- **Bone Orientation**: Consistent local axis alignment
- **IK Chains**: Bake IK to FK before export

### 3. Animation Production Standards

#### Standard Animation Set Requirements

**Player Character Animations (Required)**
```yaml
locomotion:
  - idle: 30fps, 2-4 seconds, seamless loop
  - walk: 30fps, 1 second cycle, loop
  - run: 30fps, 0.8 second cycle, loop
  - crouch_idle: 30fps, 2 seconds, loop
  - crouch_walk: 30fps, 1.2 second cycle, loop

combat:
  - attack_melee: 30fps, 0.5-1 second, one-shot
  - attack_ranged: 30fps, 0.3-0.8 second, one-shot
  - reload: 30fps, 1-3 seconds, one-shot
  - draw_weapon: 30fps, 0.5-1 second, one-shot
  - holster_weapon: 30fps, 0.5-1 second, one-shot

reactions:
  - hit_light: 30fps, 0.3 seconds, one-shot
  - hit_heavy: 30fps, 0.5 seconds, one-shot
  - death: 30fps, 1-2 seconds, one-shot
  - pain: 30fps, 0.4 seconds, one-shot

special:
  - use_item: 30fps, 0.8 seconds, one-shot
  - jump: 30fps, 0.6 seconds, one-shot
  - land: 30fps, 0.3 seconds, one-shot
```

**Monster Animations (Category-Specific)**
```yaml
basic_monster:
  - idle: 30fps, 2-5 seconds, loop
  - walk: 30fps, 1-1.5 second cycle, loop
  - attack: 30fps, 0.8-1.5 seconds, one-shot
  - pain: 30fps, 0.4 seconds, one-shot
  - death: 30fps, 1-3 seconds, one-shot

flying_monster:
  - hover: 30fps, 2-3 seconds, loop
  - fly_forward: 30fps, 1 second cycle, loop
  - attack_ranged: 30fps, 0.6-1 second, one-shot

boss_monster:
  - idle_threat: 30fps, 3-5 seconds, loop
  - special_attack_01: 30fps, 2-4 seconds, one-shot
  - special_attack_02: 30fps, 2-4 seconds, one-shot
  - phase_transition: 30fps, 2-3 seconds, one-shot
```

### 4. glTF Export Configuration

#### Recommended Export Settings
```json
{
  "format": "GLB",
  "export_settings": {
    "export_format": "GLB",
    "export_copyright": "Your Studio Name",
    "export_image_format": "AUTO",
    "export_texture_dir": "",
    "export_keep_originals": false,
    "export_texcoords": true,
    "export_normals": true,
    "export_draco_mesh_compression_enable": false,
    "export_draco_mesh_compression_level": 6,
    "export_draco_position_quantization": 14,
    "export_draco_normal_quantization": 10,
    "export_draco_texcoord_quantization": 12,
    "export_draco_color_quantization": 10,
    "export_draco_generic_quantization": 12,
    "export_tangents": true,
    "export_materials": "EXPORT",
    "export_colors": true,
    "export_cameras": false,
    "export_selected": false,
    "use_selection": false,
    "export_extras": false,
    "export_yup": true,
    "export_apply": true,
    "export_animations": true,
    "export_frame_range": true,
    "export_frame_step": 1,
    "export_force_sampling": true,
    "export_nla_strips": true,
    "export_def_bones": false,
    "optimize_animation_size": true,
    "export_current_frame": false,
    "export_skins": true,
    "export_all_influences": false,
    "export_morph": true,
    "export_morph_normal": true,
    "export_morph_tangent": false,
    "export_lights": false,
    "will_save_settings": false
  }
}
```

#### Validation Script
```python
# Post-export validation
import json
import os

def validate_gltf_export(filepath):
    """Validate exported glTF file meets BiasedDoom requirements"""

    # File size check
    file_size = os.path.getsize(filepath)
    if file_size > 50 * 1024 * 1024:  # 50MB limit
        print(f"WARNING: File size {file_size/1024/1024:.2f}MB exceeds recommended 50MB")

    # Run official validator
    os.system(f"gltf_validator {filepath}")

    # Custom BiasedDoom checks
    with open(filepath, 'rb') as f:
        # Check for GLB magic number
        magic = f.read(4)
        if magic != b'glTF':
            print("ERROR: Invalid GLB magic number")
            return False

    print("✓ glTF export validation passed")
    return True
```

---

## Asset Category Integration

### 1. Player Character Replacement

#### ZScript Integration
```zscript
// zscript/actors/player_gltf.zs
class GLTFDoomPlayer : DoomPlayer
{
    Default
    {
        // Visual properties
        Model.Path "models/players/marine.glb";
        Model.Scale 1.0;
        Model.Animation "idle";
        Model.PBREnabled true;

        // Hitbox adjustments for new model
        Height 56;
        Radius 16;

        // Animation state management
        +Model.AnimationInterpolation;
        Model.AnimationSpeed 1.0;
    }

    States
    {
    Spawn:
        GPLY A -1 NoDelay
        {
            // Set initial animation based on player state
            if (vel.length() > 0)
            {
                if (player.crouchfactor < 1.0)
                    A_SetModelAnimation("crouch_walk");
                else if (vel.length() > 8.0)
                    A_SetModelAnimation("run");
                else
                    A_SetModelAnimation("walk");
            }
            else
            {
                if (player.crouchfactor < 1.0)
                    A_SetModelAnimation("crouch_idle");
                else
                    A_SetModelAnimation("idle");
            }
        }
        Loop;

    Melee:
        GPLY B 4 A_SetModelAnimation("attack_melee");
        GPLY C 8 A_Punch;
        GPLY D 4;
        Goto Spawn;

    Missile:
        GPLY E 12
        {
            A_SetModelAnimation("attack_ranged");
            A_GunFlash;
        }
        GPLY F 6 A_FireBullets(5.6, 0, 1, 5, "BulletPuff");
        GPLY E 4;
        Goto Spawn;

    Pain:
        GPLY G 4 A_SetModelAnimation("pain");
        GPLY G 4 A_Pain;
        Goto Spawn;

    Death:
        GPLY H 10 A_SetModelAnimation("death");
        GPLY I 10 A_Scream;
        GPLY J 10 A_NoBlocking;
        GPLY K -1;
        Stop;
    }
}
```

#### Animation State Management
```zscript
// Advanced player animation controller
class GLTFPlayerAnimationController : Actor
{
    private String currentAnimation;
    private double animationStartTime;
    private bool isLooping;

    void UpdatePlayerAnimation(DoomPlayer player)
    {
        String newAnimation = DetermineAnimation(player);

        if (newAnimation != currentAnimation)
        {
            SetPlayerAnimation(player, newAnimation);
        }
    }

    private String DetermineAnimation(DoomPlayer player)
    {
        // Priority-based animation selection
        if (player.health <= 0)
            return "death";

        if (player.player.ReadyWeapon && player.player.ReadyWeapon.bAltFire)
            return "reload";

        if (player.vel.length() > 0)
        {
            if (player.player.crouchfactor < 1.0)
                return "crouch_walk";
            else if (player.vel.length() > 8.0)
                return "run";
            else
                return "walk";
        }

        if (player.player.crouchfactor < 1.0)
            return "crouch_idle";

        return "idle";
    }

    private void SetPlayerAnimation(DoomPlayer player, String animName)
    {
        player.A_SetModelAnimation(animName);
        currentAnimation = animName;
        animationStartTime = level.time;

        // Set looping based on animation type
        isLooping = (animName ~== "idle" ||
                    animName ~== "walk" ||
                    animName ~== "run" ||
                    animName ~== "crouch_idle" ||
                    animName ~== "crouch_walk");
    }
}
```

### 2. Monster Replacement System

#### Base Monster Class
```zscript
// zscript/actors/base_gltf_monster.zs
class BaseGLTFMonster : Actor
{
    protected String idleAnimation;
    protected String walkAnimation;
    protected String attackAnimation;
    protected String painAnimation;
    protected String deathAnimation;

    Default
    {
        Monster;
        +SOLID;
        +SHOOTABLE;
        +COUNTKILL;
        +Model.AnimationInterpolation;
        Model.PBREnabled true;
        Model.AnimationSpeed 1.0;
    }

    virtual void SetupAnimations()
    {
        idleAnimation = "idle";
        walkAnimation = "walk";
        attackAnimation = "attack";
        painAnimation = "pain";
        deathAnimation = "death";
    }

    override void PostBeginPlay()
    {
        Super.PostBeginPlay();
        SetupAnimations();
        A_SetModelAnimation(idleAnimation);
    }

    void A_MonsterAttackAnimation()
    {
        A_SetModelAnimation(attackAnimation);
    }

    void A_MonsterWalkAnimation()
    {
        A_SetModelAnimation(walkAnimation);
    }

    void A_MonsterIdleAnimation()
    {
        A_SetModelAnimation(idleAnimation);
    }
}
```

#### Specific Monster Implementation
```zscript
// Example: Cyberdemon replacement
class GLTFCyberdemon : BaseGLTFMonster
{
    Default
    {
        Health 4000;
        Radius 40;
        Height 110;
        Mass 1000;
        Speed 16;
        PainChance 20;
        SeeSound "cyber/sight";
        PainSound "cyber/pain";
        DeathSound "cyber/death";
        ActiveSound "cyber/active";

        Model.Path "models/monsters/cyberdemon.glb";
        Model.Scale 1.2;

        // Boss-specific properties
        +BOSS;
        +MISSILEMORE;
        +FLOORCLIP;
        +NORADIUSDMG;
        +DONTMORPH;
        +BOSSDEATH;
    }

    override void SetupAnimations()
    {
        Super.SetupAnimations();
        idleAnimation = "idle_threat";
        walkAnimation = "walk_heavy";
        attackAnimation = "rocket_attack";
        painAnimation = "pain_heavy";
        deathAnimation = "death_explosive";
    }

    States
    {
    Spawn:
        CYBR A 10 A_MonsterIdleAnimation();
        Loop;

    See:
        CYBR A 3 A_MonsterWalkAnimation();
        CYBR A 3 A_Chase;
        Loop;

    Missile:
        CYBR E 6 A_FaceTarget;
        CYBR F 12
        {
            A_MonsterAttackAnimation();
            A_CyberAttack;
        }
        CYBR E 12 A_FaceTarget;
        CYBR F 12
        {
            A_MonsterAttackAnimation();
            A_CyberAttack;
        }
        CYBR E 12 A_FaceTarget;
        CYBR F 12
        {
            A_MonsterAttackAnimation();
            A_CyberAttack;
        }
        Goto See;

    Pain:
        CYBR G 10
        {
            A_SetModelAnimation(painAnimation);
            A_Pain;
        }
        Goto See;

    Death:
        CYBR H 10 A_SetModelAnimation(deathAnimation);
        CYBR I 10 A_Scream;
        CYBR JKL 10;
        CYBR M 10 A_NoBlocking;
        CYBR NO 10;
        CYBR P 30;
        CYBR P -1 A_BossDeath;
        Stop;
    }
}
```

### 3. Weapon Integration System

#### First-Person Weapon Models
```zscript
// zscript/weapons/gltf_pistol.zs
class GLTFPistol : Pistol
{
    Default
    {
        Weapon.SelectionOrder 1900;
        Weapon.AmmoUse 1;
        Weapon.AmmoGive 20;
        Weapon.AmmoType "Clip";

        // First-person model
        Model.Path "models/weapons/pistol_fp.glb";
        Model.Scale 0.8;
        Model.Offset (0, 0, 0);
        Model.AngleOffset (0, 0, 0);
        Model.PBREnabled true;

        // Third-person model for dropped weapon
        DropItem.Model "models/weapons/pistol_world.glb";
        DropItem.Scale 1.0;
    }

    States
    {
    Ready:
        PISG A 1
        {
            A_SetModelAnimation("idle");
            A_WeaponReady;
        }
        Loop;

    Deselect:
        PISG A 1
        {
            A_SetModelAnimation("holster");
            A_Lower;
        }
        Loop;

    Select:
        PISG A 1
        {
            A_SetModelAnimation("draw");
            A_Raise;
        }
        Loop;

    Fire:
        PISG A 4
        {
            A_SetModelAnimation("fire");
            A_FireBullets(5.6, 0, 1, 5, "BulletPuff");
            A_PlaySound("weapons/pistol", CHAN_WEAPON);
        }
        PISG B 6 A_SetModelAnimation("recoil");
        PISG C 4 A_SetModelAnimation("recover");
        PISG B 5 A_ReFire;
        Goto Ready;

    Reload:
        PISG D 5 A_SetModelAnimation("reload_start");
        PISG E 10 A_PlaySound("weapons/pistol/magout", CHAN_WEAPON);
        PISG F 10 A_PlaySound("weapons/pistol/magin", CHAN_WEAPON);
        PISG G 5 A_SetModelAnimation("reload_end");
        Goto Ready;
    }
}
```

#### Advanced Weapon Animation System
```zscript
class WeaponAnimationHandler : EventHandler
{
    override void WorldThingSpawned(WorldEvent e)
    {
        if (e.Thing is "Weapon")
        {
            let weapon = Weapon(e.Thing);
            InitializeWeaponAnimations(weapon);
        }
    }

    void InitializeWeaponAnimations(Weapon weapon)
    {
        // Set up weapon-specific animation bindings
        switch (weapon.GetClassName())
        {
            case "GLTFPistol":
                SetupPistolAnimations(weapon);
                break;
            case "GLTFShotgun":
                SetupShotgunAnimations(weapon);
                break;
            case "GLTFChaingun":
                SetupChaingunAnimations(weapon);
                break;
        }
    }

    void SetupPistolAnimations(Weapon weapon)
    {
        // Bind audio cues to animation events
        weapon.A_SetModelAnimationCallback("fire", "OnFireAnimation");
        weapon.A_SetModelAnimationCallback("reload_start", "OnReloadStart");
        weapon.A_SetModelAnimationCallback("reload_end", "OnReloadEnd");
    }
}
```

### 4. Item & Pickup Integration

#### Animated Pickup Items
```zscript
// zscript/items/gltf_health_bonus.zs
class GLTFHealthBonus : HealthBonus
{
    Default
    {
        Model.Path "models/items/health_bonus.glb";
        Model.Scale 0.6;
        Model.Animation "idle_float";
        Model.PBREnabled true;

        // Pickup effects
        +COUNTITEM;
        +INVENTORY.AUTOACTIVATE;
        +FLOATBOB;
        FloatBobStrength 0.5;
    }

    States
    {
    Spawn:
        GLTF A 6 Bright A_SetModelAnimation("idle_float");
        Loop;

    Pickup:
        GLTF B 6
        {
            A_SetModelAnimation("pickup_shine");
            A_PlaySound("misc/health_pickup");
        }
        Stop;
    }
}
```

#### Interactive Objects
```zscript
// Interactive switch with animations
class GLTFSwitch : SwitchableDecoration
{
    bool isActivated;

    Default
    {
        Model.Path "models/environment/tech_switch.glb";
        Model.Scale 1.0;
        Model.Animation "off";
        Model.PBREnabled true;

        +SOLID;
        +USESPECIAL;
        Radius 16;
        Height 32;
    }

    override bool Use(Actor user)
    {
        isActivated = !isActivated;

        if (isActivated)
        {
            A_SetModelAnimation("activate");
            A_PlaySound("switches/normbutn");
        }
        else
        {
            A_SetModelAnimation("deactivate");
            A_PlaySound("switches/exitbutn");
        }

        return Super.Use(user);
    }

    States
    {
    Spawn:
        GLTF A -1 A_SetModelAnimation(isActivated ? "on" : "off");
        Stop;
    }
}
```

### 5. Environmental Asset Integration

#### Animated Environment Objects
```zscript
// Ambient animated machinery
class GLTFTechMachinery : Actor
{
    Default
    {
        Model.Path "models/environment/tech_machinery.glb";
        Model.Scale 1.5;
        Model.Animation "idle_working";
        Model.PBREnabled true;

        +SOLID;
        +NOBLOCKMAP;
        Radius 64;
        Height 128;
    }

    override void PostBeginPlay()
    {
        Super.PostBeginPlay();

        // Random animation start time for variety
        A_SetModelAnimationTime(Random(0, 100) * 0.01);

        // Ambient sound loop
        A_PlaySound("ambient/machinery", CHAN_AUTO, 0.3, true);
    }

    States
    {
    Spawn:
        GLTF A -1 A_SetModelAnimation("idle_working");
        Stop;
    }
}
```

---

## Animation System Integration

### 1. Animation State Management

#### Core Animation Controller
```zscript
class GLTFAnimationController : Thinker
{
    private Actor target;
    private String currentState;
    private String queuedState;
    private double transitionTime;
    private double stateStartTime;
    private bool isTransitioning;

    // Animation state definitions
    enum AnimationState
    {
        STATE_IDLE,
        STATE_WALK,
        STATE_RUN,
        STATE_ATTACK,
        STATE_PAIN,
        STATE_DEATH,
        STATE_SPECIAL
    };

    static GLTFAnimationController Create(Actor target)
    {
        let controller = new("GLTFAnimationController");
        controller.target = target;
        controller.currentState = "idle";
        controller.stateStartTime = level.time;
        return controller;
    }

    void RequestStateChange(String newState, double blendTime = 0.2)
    {
        if (newState == currentState) return;

        queuedState = newState;
        transitionTime = blendTime;
        isTransitioning = true;
    }

    override void Tick()
    {
        if (!target || target.health <= 0) return;

        if (isTransitioning)
        {
            ProcessTransition();
        }

        UpdateAnimationState();
    }

    private void ProcessTransition()
    {
        if (!queuedState.Length()) return;

        // Implement blend transition
        target.A_SetModelAnimation(queuedState, transitionTime);
        currentState = queuedState;
        queuedState = "";
        isTransitioning = false;
        stateStartTime = level.time;
    }

    private void UpdateAnimationState()
    {
        // Check for state-specific updates
        switch (currentState)
        {
            case "attack":
                if (level.time - stateStartTime > 35) // 1 second at 35 tics/sec
                    RequestStateChange("idle");
                break;

            case "pain":
                if (level.time - stateStartTime > 15) // 0.4 seconds
                    RequestStateChange("idle");
                break;
        }
    }
}
```

### 2. Animation Event System

#### Event-Driven Animation Callbacks
```zscript
// Animation event handler for synchronized effects
class AnimationEventHandler : EventHandler
{
    // Global animation event registry
    private Map<String, Array<AnimationCallback>> eventCallbacks;

    struct AnimationCallback
    {
        String eventName;
        Actor target;
        String functionName;
        Array<String> parameters;
    }

    void RegisterAnimationEvent(Actor target, String animationName, String eventName, String callback)
    {
        AnimationCallback cb;
        cb.eventName = eventName;
        cb.target = target;
        cb.functionName = callback;

        String key = String.Format("%p_%s_%s", target, animationName, eventName);
        eventCallbacks[key].Push(cb);
    }

    void TriggerAnimationEvent(Actor target, String animationName, String eventName)
    {
        String key = String.Format("%p_%s_%s", target, animationName, eventName);
        if (eventCallbacks.CheckKey(key))
        {
            Array<AnimationCallback> callbacks = eventCallbacks[key];
            for (int i = 0; i < callbacks.Size(); i++)
            {
                ExecuteCallback(callbacks[i]);
            }
        }
    }

    private void ExecuteCallback(AnimationCallback cb)
    {
        // Execute the callback function on the target actor
        switch (cb.functionName)
        {
            case "PlaySound":
                if (cb.parameters.Size() > 0)
                    cb.target.A_PlaySound(cb.parameters[0]);
                break;

            case "SpawnEffect":
                if (cb.parameters.Size() > 0)
                    cb.target.A_SpawnItemEx(cb.parameters[0]);
                break;

            case "TakeDamage":
                if (cb.parameters.Size() > 0)
                    cb.target.DamageMobj(cb.target, cb.target, cb.parameters[0].ToInt(), "Normal");
                break;
        }
    }
}
```

### 3. Weapon Animation Synchronization

#### Precise Timing System
```zscript
class WeaponAnimationSync : Weapon
{
    private double lastFireTime;
    private String currentWeaponAnimation;
    private bool isReloading;
    private int reloadFrame;

    // Weapon-specific animation timings (in tics)
    const FIRE_ANIMATION_DURATION = 12;
    const RELOAD_ANIMATION_DURATION = 70;
    const DRAW_ANIMATION_DURATION = 25;

    override void DoEffect()
    {
        Super.DoEffect();
        UpdateWeaponAnimation();
    }

    void UpdateWeaponAnimation()
    {
        // Synchronize animation with weapon state
        if (isReloading)
        {
            UpdateReloadAnimation();
        }
        else if (level.time - lastFireTime < FIRE_ANIMATION_DURATION)
        {
            UpdateFireAnimation();
        }
        else
        {
            SetWeaponAnimation("idle");
        }
    }

    void UpdateReloadAnimation()
    {
        reloadFrame++;

        // Trigger specific reload events
        switch (reloadFrame)
        {
            case 20: // Magazine out
                A_PlaySound("weapons/reload/magout");
                break;

            case 45: // Magazine in
                A_PlaySound("weapons/reload/magin");
                break;

            case RELOAD_ANIMATION_DURATION: // Reload complete
                isReloading = false;
                reloadFrame = 0;
                break;
        }
    }

    void UpdateFireAnimation()
    {
        int fireFrame = level.time - lastFireTime;

        switch (fireFrame)
        {
            case 1: // Muzzle flash
                A_FireBullets(0, 0, 1, 0, "BulletPuff");
                A_GunFlash;
                break;

            case 4: // Shell eject
                if (GetClassName() ~== "GLTFShotgun")
                    A_SpawnItemEx("ShotgunShell", 16, 8, 32,
                                random(-2,2), random(-2,2), random(4,8));
                break;
        }
    }

    void StartReload()
    {
        isReloading = true;
        reloadFrame = 0;
        SetWeaponAnimation("reload");
    }

    void FireWeapon()
    {
        lastFireTime = level.time;
        SetWeaponAnimation("fire");
    }

    void SetWeaponAnimation(String animName)
    {
        if (currentWeaponAnimation != animName)
        {
            A_SetModelAnimation(animName);
            currentWeaponAnimation = animName;
        }
    }
}
```

---

## Material & Texture Workflow

### 1. PBR Material Setup

#### Blender Material Configuration
```python
# Blender script for PBR material setup
import bpy

def setup_pbr_material(material_name):
    # Create new material
    mat = bpy.data.materials.new(name=material_name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links

    # Clear default nodes
    nodes.clear()

    # Add Principled BSDF
    principled = nodes.new(type='ShaderNodeBsdfPrincipled')
    principled.location = (0, 0)

    # Add Output node
    output = nodes.new(type='ShaderNodeOutputMaterial')
    output.location = (300, 0)

    # Connect Principled BSDF to Output
    links.new(principled.outputs['BSDF'], output.inputs['Surface'])

    # Add texture nodes
    tex_coord = nodes.new(type='ShaderNodeTexCoord')
    tex_coord.location = (-800, 0)

    mapping = nodes.new(type='ShaderNodeMapping')
    mapping.location = (-600, 0)
    links.new(tex_coord.outputs['UV'], mapping.inputs['Vector'])

    # Base Color texture
    base_color_tex = nodes.new(type='ShaderNodeTexImage')
    base_color_tex.location = (-400, 200)
    base_color_tex.name = "BaseColor"
    links.new(mapping.outputs['Vector'], base_color_tex.inputs['Vector'])
    links.new(base_color_tex.outputs['Color'], principled.inputs['Base Color'])

    # Normal map
    normal_tex = nodes.new(type='ShaderNodeTexImage')
    normal_tex.location = (-400, 0)
    normal_tex.name = "Normal"
    normal_tex.image.colorspace_settings.name = 'Non-Color'

    normal_map = nodes.new(type='ShaderNodeNormalMap')
    normal_map.location = (-200, 0)
    links.new(mapping.outputs['Vector'], normal_tex.inputs['Vector'])
    links.new(normal_tex.outputs['Color'], normal_map.inputs['Color'])
    links.new(normal_map.outputs['Normal'], principled.inputs['Normal'])

    # Metallic/Roughness texture (ORM format)
    orm_tex = nodes.new(type='ShaderNodeTexImage')
    orm_tex.location = (-400, -200)
    orm_tex.name = "ORM"
    orm_tex.image.colorspace_settings.name = 'Non-Color'

    separate_rgb = nodes.new(type='ShaderNodeSeparateRGB')
    separate_rgb.location = (-200, -200)
    links.new(mapping.outputs['Vector'], orm_tex.inputs['Vector'])
    links.new(orm_tex.outputs['Color'], separate_rgb.inputs['Image'])

    # Connect ORM channels
    links.new(separate_rgb.outputs['R'], principled.inputs['Roughness'])
    links.new(separate_rgb.outputs['G'], principled.inputs['Metallic'])
    # Blue channel (Occlusion) to AO input if available

    return mat

# Batch material setup
def setup_all_materials():
    material_types = [
        "Marine_Body",
        "Marine_Armor",
        "Weapon_Metal",
        "Weapon_Plastic",
        "Monster_Skin",
        "Monster_Carapace",
        "Environment_Metal",
        "Environment_Concrete"
    ]

    for mat_type in material_types:
        setup_pbr_material(mat_type)
        print(f"Created material: {mat_type}")
```

#### Texture Naming Convention
```
// Texture file naming standard
{AssetName}_{MaterialType}_{TextureType}.{Format}

Examples:
- marine_body_basecolor.png
- marine_body_normal.png
- marine_body_orm.png      // Occlusion-Roughness-Metallic
- marine_armor_basecolor.png
- marine_armor_normal.png
- marine_armor_orm.png
- cyberdemon_skin_basecolor.png
- cyberdemon_skin_normal.png
- cyberdemon_skin_orm.png
- pistol_metal_basecolor.png
- pistol_metal_normal.png
- pistol_metal_orm.png
```

### 2. Texture Optimization Pipeline

#### Automated Texture Processing
```bash
#!/bin/bash
# scripts/optimize_textures.sh

set -e

TEXTURE_DIR="assets/textures"
OUTPUT_DIR="optimized_textures"
TEMP_DIR="temp_textures"

# Create directories
mkdir -p "$OUTPUT_DIR"
mkdir -p "$TEMP_DIR"

echo "🎨 Starting texture optimization pipeline..."

# Function to optimize texture
optimize_texture() {
    local input_file="$1"
    local output_file="$2"
    local texture_type="$3"

    echo "Processing: $input_file"

    case "$texture_type" in
        "basecolor")
            # Base color: sRGB, BC7 compression for high quality
            magick "$input_file" \
                -resize 1024x1024 \
                -quality 95 \
                -format PNG \
                "$output_file"
            ;;

        "normal")
            # Normal maps: Linear, BC5 compression
            magick "$input_file" \
                -colorspace RGB \
                -resize 1024x1024 \
                -quality 100 \
                -format PNG \
                "$output_file"
            ;;

        "orm")
            # ORM maps: Linear, BC4 per channel or BC7
            magick "$input_file" \
                -colorspace RGB \
                -resize 1024x1024 \
                -quality 95 \
                -format PNG \
                "$output_file"
            ;;

        "emissive")
            # Emissive: sRGB, high quality
            magick "$input_file" \
                -resize 512x512 \
                -quality 90 \
                -format PNG \
                "$output_file"
            ;;
    esac
}

# Process all textures
find "$TEXTURE_DIR" -name "*.png" -o -name "*.jpg" -o -name "*.tga" | while read file; do
    filename=$(basename "$file")
    name_without_ext="${filename%.*}"

    # Determine texture type from filename
    if [[ $filename == *"basecolor"* ]] || [[ $filename == *"diffuse"* ]]; then
        texture_type="basecolor"
    elif [[ $filename == *"normal"* ]]; then
        texture_type="normal"
    elif [[ $filename == *"orm"* ]] || [[ $filename == *"roughness"* ]] || [[ $filename == *"metallic"* ]]; then
        texture_type="orm"
    elif [[ $filename == *"emissive"* ]] || [[ $filename == *"emission"* ]]; then
        texture_type="emissive"
    else
        texture_type="basecolor"  # Default
    fi

    output_file="$OUTPUT_DIR/${name_without_ext}.png"
    optimize_texture "$file" "$output_file" "$texture_type"
done

echo "✅ Texture optimization complete!"
echo "📊 Generating texture report..."

# Generate texture usage report
{
    echo "# Texture Optimization Report"
    echo "Generated: $(date)"
    echo ""
    echo "## Processed Textures"
    echo ""

    total_original_size=0
    total_optimized_size=0

    find "$TEXTURE_DIR" -name "*.png" -o -name "*.jpg" -o -name "*.tga" | while read original_file; do
        filename=$(basename "$original_file")
        name_without_ext="${filename%.*}"
        optimized_file="$OUTPUT_DIR/${name_without_ext}.png"

        if [[ -f "$optimized_file" ]]; then
            original_size=$(stat -f%z "$original_file" 2>/dev/null || stat -c%s "$original_file")
            optimized_size=$(stat -f%z "$optimized_file" 2>/dev/null || stat -c%s "$optimized_file")

            savings=$((original_size - optimized_size))
            savings_percent=$((savings * 100 / original_size))

            echo "- **$filename**: $(numfmt --to=iec $original_size) → $(numfmt --to=iec $optimized_size) (${savings_percent}% reduction)"

            total_original_size=$((total_original_size + original_size))
            total_optimized_size=$((total_optimized_size + optimized_size))
        fi
    done

    total_savings=$((total_original_size - total_optimized_size))
    total_savings_percent=$((total_savings * 100 / total_original_size))

    echo ""
    echo "## Summary"
    echo "- **Total Original Size**: $(numfmt --to=iec $total_original_size)"
    echo "- **Total Optimized Size**: $(numfmt --to=iec $total_optimized_size)"
    echo "- **Total Savings**: $(numfmt --to=iec $total_savings) (${total_savings_percent}%)"

} > texture_optimization_report.md

echo "📄 Report saved to: texture_optimization_report.md"
```

### 3. Runtime Material Configuration

#### ZScript Material Manager
```zscript
class GLTFMaterialManager : Thinker
{
    // Material property cache
    private Map<String, PBRMaterialProperties> materialCache;

    struct PBRMaterialProperties
    {
        Vector4 baseColorFactor;
        double metallicFactor;
        double roughnessFactor;
        double normalScale;
        Vector3 emissiveFactor;
        double alphaCutoff;
        bool doubleSided;

        String baseColorTexture;
        String normalTexture;
        String ormTexture;
        String emissiveTexture;
    }

    static GLTFMaterialManager GetInstance()
    {
        // Singleton pattern
        static GLTFMaterialManager instance;
        if (!instance)
        {
            instance = new("GLTFMaterialManager");
        }
        return instance;
    }

    void RegisterMaterial(String name, PBRMaterialProperties props)
    {
        materialCache[name] = props;
    }

    PBRMaterialProperties GetMaterial(String name)
    {
        if (materialCache.CheckKey(name))
            return materialCache[name];

        // Return default material
        PBRMaterialProperties defaultMat;
        defaultMat.baseColorFactor = (1.0, 1.0, 1.0, 1.0);
        defaultMat.metallicFactor = 0.0;
        defaultMat.roughnessFactor = 0.8;
        defaultMat.normalScale = 1.0;
        defaultMat.emissiveFactor = (0.0, 0.0, 0.0);
        defaultMat.alphaCutoff = 0.5;
        defaultMat.doubleSided = false;

        return defaultMat;
    }

    void InitializeStandardMaterials()
    {
        // Marine materials
        PBRMaterialProperties marineMetal;
        marineMetal.baseColorFactor = (0.7, 0.7, 0.8, 1.0);
        marineMetal.metallicFactor = 0.9;
        marineMetal.roughnessFactor = 0.2;
        RegisterMaterial("marine_armor", marineMetal);

        // Monster materials
        PBRMaterialProperties demonSkin;
        demonSkin.baseColorFactor = (0.8, 0.4, 0.3, 1.0);
        demonSkin.metallicFactor = 0.0;
        demonSkin.roughnessFactor = 0.9;
        RegisterMaterial("demon_skin", demonSkin);

        // Weapon materials
        PBRMaterialProperties weaponMetal;
        weaponMetal.baseColorFactor = (0.3, 0.3, 0.3, 1.0);
        weaponMetal.metallicFactor = 0.95;
        weaponMetal.roughnessFactor = 0.1;
        RegisterMaterial("weapon_metal", weaponMetal);
    }
}
```

---

## Performance Optimization

### 1. Level-of-Detail (LOD) System

#### Automatic LOD Management
```zscript
class GLTFLODManager : Thinker
{
    struct LODLevel
    {
        String modelPath;
        double distance;
        int maxVertices;
        bool enableAnimations;
        bool enablePBR;
    }

    // LOD configurations per asset type
    private Map<String, Array<LODLevel>> lodConfigurations;

    void InitializeLODSystem()
    {
        // Player LOD configuration
        Array<LODLevel> playerLODs;

        LODLevel playerHigh;
        playerHigh.modelPath = "models/players/marine_high.glb";
        playerHigh.distance = 512.0;
        playerHigh.maxVertices = 8000;
        playerHigh.enableAnimations = true;
        playerHigh.enablePBR = true;
        playerLODs.Push(playerHigh);

        LODLevel playerMed;
        playerMed.modelPath = "models/players/marine_med.glb";
        playerMed.distance = 1024.0;
        playerMed.maxVertices = 4000;
        playerMed.enableAnimations = true;
        playerMed.enablePBR = true;
        playerLODs.Push(playerMed);

        LODLevel playerLow;
        playerLow.modelPath = "models/players/marine_low.glb";
        playerLow.distance = 2048.0;
        playerLow.maxVertices = 1500;
        playerLow.enableAnimations = false;
        playerLow.enablePBR = false;
        playerLODs.Push(playerLow);

        lodConfigurations["player"] = playerLODs;

        // Monster LOD configuration
        SetupMonsterLODs();

        // Weapon LOD configuration
        SetupWeaponLODs();
    }

    String GetOptimalModel(Actor target, Actor viewer)
    {
        String actorType = DetermineActorType(target);
        double distance = target.Distance3D(viewer);

        Array<LODLevel> lods = lodConfigurations[actorType];

        for (int i = 0; i < lods.Size(); i++)
        {
            if (distance <= lods[i].distance)
            {
                return lods[i].modelPath;
            }
        }

        // Return lowest LOD if beyond all distances
        if (lods.Size() > 0)
            return lods[lods.Size() - 1].modelPath;

        return "";
    }

    void UpdateActorLOD(Actor target)
    {
        if (!target.bIsMonster && !target.player) return;

        Actor viewer = players[consoleplayer].mo;
        if (!viewer) return;

        String optimalModel = GetOptimalModel(target, viewer);

        // Only update if model changed
        if (target.GetModelPath() != optimalModel)
        {
            target.A_SetModel(optimalModel);

            // Adjust animation and PBR based on LOD
            LODLevel currentLOD = GetLODLevel(target, viewer);
            target.A_SetModelAnimation(currentLOD.enableAnimations ? "idle" : "");
            target.SetPBREnabled(currentLOD.enablePBR);
        }
    }

    override void Tick()
    {
        // Update LODs every 10 tics to reduce overhead
        if (level.time % 10 != 0) return;

        ThinkerIterator it = ThinkerIterator.Create("Actor");
        Actor mo;

        while (mo = Actor(it.Next()))
        {
            if (mo.bCorpse || mo.health <= 0) continue;
            UpdateActorLOD(mo);
        }
    }
}
```

### 2. Animation Culling & Optimization

#### Smart Animation System
```zscript
class AnimationCullingSystem : Thinker
{
    private Map<Actor, double> lastUpdateTimes;
    private double cullDistance = 2048.0;
    private double reduceDistance = 1024.0;

    void SetCullingDistances(double cull, double reduce)
    {
        cullDistance = cull;
        reduceDistance = reduce;
    }

    override void Tick()
    {
        Actor viewer = players[consoleplayer].mo;
        if (!viewer) return;

        ThinkerIterator it = ThinkerIterator.Create("Actor");
        Actor mo;

        while (mo = Actor(it.Next()))
        {
            if (!mo.bIsMonster && !mo.player) continue;
            if (mo.health <= 0) continue;

            double distance = mo.Distance3D(viewer);

            // Cull animations beyond cull distance
            if (distance > cullDistance)
            {
                mo.A_SetModelAnimation("");
                continue;
            }

            // Reduce animation frequency beyond reduce distance
            if (distance > reduceDistance)
            {
                double lastUpdate = lastUpdateTimes.CheckKey(mo) ? lastUpdateTimes[mo] : 0;
                if (level.time - lastUpdate < 10) // Update every 10 tics instead of every tic
                    continue;
            }

            // Normal animation update
            UpdateActorAnimation(mo);
            lastUpdateTimes[mo] = level.time;
        }
    }

    void UpdateActorAnimation(Actor mo)
    {
        // Determine appropriate animation based on actor state
        String animation = "idle";

        if (mo.vel.length() > 0)
        {
            animation = (mo.vel.length() > 8.0) ? "run" : "walk";
        }

        if (mo.target && mo.CheckSight(mo.target))
        {
            animation = "alert";
        }

        mo.A_SetModelAnimation(animation);
    }
}
```

### 3. Memory Management

#### Asset Streaming System
```zscript
class GLTFAssetStreamer : Thinker
{
    private Map<String, double> assetUsageTimes;
    private Map<String, int> assetReferenceCounts;
    private double maxIdleTime = 180.0; // 3 minutes in seconds

    void RegisterAssetUsage(String modelPath)
    {
        assetUsageTimes[modelPath] = level.time;

        if (assetReferenceCounts.CheckKey(modelPath))
            assetReferenceCounts[modelPath]++;
        else
            assetReferenceCounts[modelPath] = 1;
    }

    void UnregisterAssetUsage(String modelPath)
    {
        if (assetReferenceCounts.CheckKey(modelPath))
        {
            assetReferenceCounts[modelPath]--;
            if (assetReferenceCounts[modelPath] <= 0)
            {
                assetReferenceCounts.Remove(modelPath);
                // Asset can be unloaded after idle time
            }
        }
    }

    override void Tick()
    {
        // Check for unused assets every 5 seconds
        if (level.time % (35 * 5) != 0) return;

        double currentTime = level.time;
        Array<String> assetsToUnload;

        MapIterator<String, double> it;
        it.Init(assetUsageTimes);

        while (it.Next())
        {
            String modelPath = it.GetKey();
            double lastUsed = it.GetValue();

            // Check if asset is unused and idle
            if (!assetReferenceCounts.CheckKey(modelPath) || assetReferenceCounts[modelPath] == 0)
            {
                if (currentTime - lastUsed > maxIdleTime * 35) // Convert to tics
                {
                    assetsToUnload.Push(modelPath);
                }
            }
        }

        // Unload idle assets
        for (int i = 0; i < assetsToUnload.Size(); i++)
        {
            UnloadAsset(assetsToUnload[i]);
            assetUsageTimes.Remove(assetsToUnload[i]);
        }
    }

    void UnloadAsset(String modelPath)
    {
        // Request asset unload from engine
        Console.Printf("Unloading unused asset: %s", modelPath);
        // A_UnloadModel(modelPath); // Hypothetical engine function
    }
}
```

---

## Quality Assurance & Testing

### 1. Automated Asset Validation

#### Comprehensive Validation Suite
```bash
#!/bin/bash
# scripts/validate_assets.sh

set -e

ASSETS_DIR="assets"
MODELS_DIR="$ASSETS_DIR/models"
TEXTURES_DIR="$ASSETS_DIR/textures"
REPORTS_DIR="validation_reports"

mkdir -p "$REPORTS_DIR"

echo "🔍 Starting comprehensive asset validation..."

# Function to validate glTF model
validate_gltf_model() {
    local model_file="$1"
    local report_file="$2"

    echo "Validating: $model_file" | tee -a "$report_file"

    # File size check
    local file_size=$(stat -f%z "$model_file" 2>/dev/null || stat -c%s "$model_file")
    local size_mb=$((file_size / 1024 / 1024))

    if [ $size_mb -gt 50 ]; then
        echo "❌ WARNING: File size ${size_mb}MB exceeds 50MB recommendation" | tee -a "$report_file"
    else
        echo "✅ File size: ${size_mb}MB (within limits)" | tee -a "$report_file"
    fi

    # Official glTF validation
    if command -v gltf_validator &> /dev/null; then
        echo "Running official glTF validator..." | tee -a "$report_file"
        if gltf_validator "$model_file" >> "$report_file" 2>&1; then
            echo "✅ glTF validation passed" | tee -a "$report_file"
        else
            echo "❌ glTF validation failed" | tee -a "$report_file"
            return 1
        fi
    else
        echo "⚠️  glTF validator not available" | tee -a "$report_file"
    fi

    # NeoDoom-specific checks
    echo "Running NeoDoom-specific validation..." | tee -a "$report_file"

    # Extract and analyze glTF content
    if [[ "$model_file" == *.glb ]]; then
        # GLB file - need to extract JSON
        local temp_json=$(mktemp)
        python3 -c "
import struct
import json
import sys

with open('$model_file', 'rb') as f:
    magic = f.read(4)
    if magic != b'glTF':
        sys.exit(1)

    version = struct.unpack('<I', f.read(4))[0]
    length = struct.unpack('<I', f.read(4))[0]

    chunk_length = struct.unpack('<I', f.read(4))[0]
    chunk_type = struct.unpack('<I', f.read(4))[0]

    if chunk_type == 0x4E4F534A:  # JSON
        json_data = f.read(chunk_length).decode('utf-8')
        gltf = json.loads(json_data)

        # Check bone count
        if 'skins' in gltf:
            for skin in gltf['skins']:
                bone_count = len(skin.get('joints', []))
                if bone_count > 128:
                    print(f'❌ Bone count {bone_count} exceeds 128 limit')
                    sys.exit(1)
                else:
                    print(f'✅ Bone count: {bone_count} (within limits)')

        # Check animation count
        if 'animations' in gltf:
            anim_count = len(gltf['animations'])
            print(f'ℹ️  Animation count: {anim_count}')

            for i, anim in enumerate(gltf['animations']):
                name = anim.get('name', f'Animation_{i}')
                print(f'  - {name}')

        # Check material count
        if 'materials' in gltf:
            mat_count = len(gltf['materials'])
            print(f'ℹ️  Material count: {mat_count}')

        # Check texture resolution
        if 'images' in gltf:
            print(f'ℹ️  Texture count: {len(gltf[\"images\"])}')
" >> "$report_file" 2>&1

        rm -f "$temp_json"
    fi

    echo "----------------------------------------" | tee -a "$report_file"
    return 0
}

# Validate all glTF models
echo "📁 Scanning for glTF models..."
model_count=0
failed_count=0

find "$MODELS_DIR" -name "*.glb" -o -name "*.gltf" | while read model_file; do
    model_count=$((model_count + 1))

    relative_path=${model_file#$ASSETS_DIR/}
    report_file="$REPORTS_DIR/$(echo "$relative_path" | tr '/' '_').txt"

    echo "=== Validation Report for $relative_path ===" > "$report_file"
    echo "Generated: $(date)" >> "$report_file"
    echo "" >> "$report_file"

    if ! validate_gltf_model "$model_file" "$report_file"; then
        failed_count=$((failed_count + 1))
    fi
done

# Validate texture consistency
echo "🖼️  Validating texture consistency..."
texture_report="$REPORTS_DIR/texture_validation.txt"

{
    echo "=== Texture Validation Report ==="
    echo "Generated: $(date)"
    echo ""

    # Check for missing textures referenced in models
    echo "## Missing Texture Analysis"
    echo ""

    missing_count=0

    find "$MODELS_DIR" -name "*.glb" | while read model_file; do
        echo "Checking textures for: ${model_file#$ASSETS_DIR/}"

        # Extract texture references (simplified - would need proper glTF parsing)
        if strings "$model_file" | grep -E '\.(png|jpg|jpeg|tga)' | while read texture_ref; do
            texture_path="$TEXTURES_DIR/$texture_ref"
            if [[ ! -f "$texture_path" ]]; then
                echo "❌ Missing texture: $texture_ref (referenced in ${model_file#$ASSETS_DIR/})"
                missing_count=$((missing_count + 1))
            fi
        done
    done

    echo ""
    echo "## Texture Size Analysis"
    echo ""

    find "$TEXTURES_DIR" -name "*.png" -o -name "*.jpg" -o -name "*.tga" | while read texture_file; do
        if command -v identify &> /dev/null; then
            dimensions=$(identify -format "%wx%h" "$texture_file" 2>/dev/null)
            size_kb=$(stat -f%z "$texture_file" 2>/dev/null || stat -c%s "$texture_file")
            size_kb=$((size_kb / 1024))

            echo "- ${texture_file#$TEXTURES_DIR/}: ${dimensions} (${size_kb}KB)"

            # Check for power-of-2 dimensions
            width=$(echo "$dimensions" | cut -d'x' -f1)
            height=$(echo "$dimensions" | cut -d'x' -f2)

            if ! [[ $width =~ ^(32|64|128|256|512|1024|2048|4096)$ ]] ||
               ! [[ $height =~ ^(32|64|128|256|512|1024|2048|4096)$ ]]; then
                echo "  ⚠️  Non-power-of-2 dimensions detected"
            fi

            # Check for oversized textures
            if [ $size_kb -gt 2048 ]; then
                echo "  ⚠️  Large texture file (>2MB)"
            fi
        fi
    done

} > "$texture_report"

# Generate summary report
summary_report="$REPORTS_DIR/validation_summary.md"

{
    echo "# Asset Validation Summary"
    echo ""
    echo "**Generated:** $(date)"
    echo ""
    echo "## Overview"
    echo ""
    echo "- **Total Models Validated:** $model_count"
    echo "- **Failed Validations:** $failed_count"
    echo "- **Success Rate:** $(( (model_count - failed_count) * 100 / model_count ))%"
    echo ""

    if [ $failed_count -gt 0 ]; then
        echo "## Failed Validations"
        echo ""
        find "$REPORTS_DIR" -name "*.txt" -exec grep -l "❌\|failed" {} \; | while read report; do
            model_name=$(basename "$report" .txt)
            echo "- **$model_name**: See detailed report"
        done
        echo ""
    fi

    echo "## Recommendations"
    echo ""

    if [ $failed_count -gt 0 ]; then
        echo "- 🔧 **Fix Failed Models:** Review detailed reports for specific issues"
    fi

    echo "- 🎨 **Optimize Textures:** Run \`scripts/optimize_textures.sh\` for better performance"
    echo "- 🧪 **Test In-Game:** Load models in NeoDoom for runtime validation"
    echo "- 📊 **Monitor Performance:** Use glTF debug tools for performance analysis"

} > "$summary_report"

echo ""
echo "✅ Asset validation complete!"
echo "📊 Summary: $model_count models validated, $failed_count failed"
echo "📄 Detailed reports available in: $REPORTS_DIR/"
echo "📋 Summary report: $summary_report"

if [ $failed_count -gt 0 ]; then
    exit 1
fi
```

### 2. In-Game Testing Framework

#### Automated Test Suite
```zscript
// Test framework for glTF asset validation
class GLTFTestSuite : EventHandler
{
    private Array<TestCase> testCases;
    private int currentTestIndex;
    private double testStartTime;
    private bool testsRunning;

    struct TestCase
    {
        String name;
        String description;
        String modelPath;
        String expectedAnimation;
        double timeout;
        bool passed;
        String errorMessage;
    }

    override void OnRegister()
    {
        SetupTestCases();
    }

    void SetupTestCases()
    {
        // Player model tests
        AddTestCase("Player Model Load", "Test basic player model loading",
                   "models/players/marine.glb", "idle", 5.0);
        AddTestCase("Player Animation", "Test player animation playback",
                   "models/players/marine.glb", "walk", 10.0);

        // Monster model tests
        AddTestCase("Cyberdemon Load", "Test cyberdemon model loading",
                   "models/monsters/cyberdemon.glb", "idle_threat", 5.0);
        AddTestCase("Imp Animation", "Test imp attack animation",
                   "models/monsters/imp.glb", "attack", 8.0);

        // Weapon model tests
        AddTestCase("Pistol Model", "Test pistol first-person model",
                   "models/weapons/pistol_fp.glb", "idle", 3.0);
        AddTestCase("Shotgun Reload", "Test shotgun reload animation",
                   "models/weapons/shotgun_fp.glb", "reload", 15.0);
    }

    void AddTestCase(String name, String desc, String model, String anim, double timeout)
    {
        TestCase test;
        test.name = name;
        test.description = desc;
        test.modelPath = model;
        test.expectedAnimation = anim;
        test.timeout = timeout;
        test.passed = false;
        test.errorMessage = "";

        testCases.Push(test);
    }

    void StartTests()
    {
        if (testsRunning) return;

        Console.Printf("🧪 Starting glTF Test Suite (%d tests)", testCases.Size());
        testsRunning = true;
        currentTestIndex = 0;
        RunNextTest();
    }

    void RunNextTest()
    {
        if (currentTestIndex >= testCases.Size())
        {
            FinishTests();
            return;
        }

        TestCase test = testCases[currentTestIndex];
        Console.Printf("Running test: %s", test.name);

        testStartTime = level.time;

        // Create test actor
        Actor testActor = Actor.Spawn("TestGLTFActor", (0, 0, 0));
        if (testActor)
        {
            testActor.A_SetModel(test.modelPath);
            testActor.A_SetModelAnimation(test.expectedAnimation);

            // Schedule test completion check
            SetDelay(int(test.timeout * 35), "CheckTestCompletion");
        }
        else
        {
            testCases[currentTestIndex].passed = false;
            testCases[currentTestIndex].errorMessage = "Failed to spawn test actor";
            currentTestIndex++;
            CallDelay(1, "RunNextTest");
        }
    }

    void CheckTestCompletion()
    {
        TestCase test = testCases[currentTestIndex];

        // Find test actor and validate
        ThinkerIterator it = ThinkerIterator.Create("TestGLTFActor");
        Actor testActor = Actor(it.Next());

        if (testActor)
        {
            // Check if model loaded successfully
            if (testActor.GetModelPath() == test.modelPath)
            {
                // Check if animation is playing
                if (testActor.GetCurrentAnimation() == test.expectedAnimation)
                {
                    testCases[currentTestIndex].passed = true;
                    Console.Printf("✅ Test passed: %s", test.name);
                }
                else
                {
                    testCases[currentTestIndex].passed = false;
                    testCases[currentTestIndex].errorMessage =
                        String.Format("Animation mismatch: expected '%s', got '%s'",
                                    test.expectedAnimation,
                                    testActor.GetCurrentAnimation());
                    Console.Printf("❌ Test failed: %s - %s", test.name,
                                 testCases[currentTestIndex].errorMessage);
                }
            }
            else
            {
                testCases[currentTestIndex].passed = false;
                testCases[currentTestIndex].errorMessage = "Model failed to load";
                Console.Printf("❌ Test failed: %s - Model failed to load", test.name);
            }

            // Clean up test actor
            testActor.Destroy();
        }
        else
        {
            testCases[currentTestIndex].passed = false;
            testCases[currentTestIndex].errorMessage = "Test actor not found";
            Console.Printf("❌ Test failed: %s - Test actor not found", test.name);
        }

        currentTestIndex++;
        CallDelay(35, "RunNextTest"); // Wait 1 second between tests
    }

    void FinishTests()
    {
        testsRunning = false;

        int passedCount = 0;
        int failedCount = 0;

        Console.Printf("");
        Console.Printf("🧪 glTF Test Suite Results:");
        Console.Printf("================================");

        for (int i = 0; i < testCases.Size(); i++)
        {
            TestCase test = testCases[i];
            if (test.passed)
            {
                Console.Printf("✅ %s", test.name);
                passedCount++;
            }
            else
            {
                Console.Printf("❌ %s - %s", test.name, test.errorMessage);
                failedCount++;
            }
        }

        Console.Printf("================================");
        Console.Printf("Results: %d passed, %d failed", passedCount, failedCount);

        if (failedCount == 0)
        {
            Console.Printf("🎉 All tests passed!");
        }
        else
        {
            Console.Printf("⚠️  %d tests failed - check asset files", failedCount);
        }
    }
}

// Test actor class
class TestGLTFActor : Actor
{
    Default
    {
        +NOBLOCKMAP;
        +NOGRAVITY;
        +NOTELEPORT;
        RenderStyle "None"; // Invisible during testing
    }

    String GetCurrentAnimation()
    {
        // Return current animation name (engine function)
        return ""; // Placeholder
    }

    String GetModelPath()
    {
        // Return current model path (engine function)
        return ""; // Placeholder
    }
}
```

### 3. Performance Profiling

#### Runtime Performance Monitor
```zscript
class GLTFPerformanceMonitor : Thinker
{
    struct PerformanceMetrics
    {
        int totalModelsLoaded;
        int activeAnimations;
        double averageFrameTime;
        int memoryUsageMB;
        int texturesLoaded;
        double loadTime;
    }

    private PerformanceMetrics currentMetrics;
    private Array<double> frameTimes;
    private double lastReportTime;
    private const REPORT_INTERVAL = 350; // 10 seconds at 35 FPS

    override void Tick()
    {
        UpdateMetrics();

        if (level.time - lastReportTime >= REPORT_INTERVAL)
        {
            GenerateReport();
            lastReportTime = level.time;
        }
    }

    void UpdateMetrics()
    {
        // Reset counters
        currentMetrics.totalModelsLoaded = 0;
        currentMetrics.activeAnimations = 0;
        currentMetrics.texturesLoaded = 0;

        // Count active glTF assets
        ThinkerIterator it = ThinkerIterator.Create("Actor");
        Actor mo;

        while (mo = Actor(it.Next()))
        {
            String modelPath = mo.GetModelPath();
            if (modelPath.Length() > 0 &&
                (modelPath.Right(4) ~== ".glb" || modelPath.Right(5) ~== ".gltf"))
            {
                currentMetrics.totalModelsLoaded++;

                if (mo.GetCurrentAnimation().Length() > 0)
                {
                    currentMetrics.activeAnimations++;
                }
            }
        }

        // Track frame time
        double currentFrameTime = GetFrameTime(); // Engine function
        frameTimes.Push(currentFrameTime);

        // Keep only last 60 frame times (2 seconds at 30 FPS)
        if (frameTimes.Size() > 60)
        {
            frameTimes.Delete(0);
        }

        // Calculate average frame time
        if (frameTimes.Size() > 0)
        {
            double total = 0;
            for (int i = 0; i < frameTimes.Size(); i++)
            {
                total += frameTimes[i];
            }
            currentMetrics.averageFrameTime = total / frameTimes.Size();
        }
    }

    void GenerateReport()
    {
        Console.Printf("📊 glTF Performance Report:");
        Console.Printf("  Models Loaded: %d", currentMetrics.totalModelsLoaded);
        Console.Printf("  Active Animations: %d", currentMetrics.activeAnimations);
        Console.Printf("  Average Frame Time: %.2fms", currentMetrics.averageFrameTime);
        Console.Printf("  FPS: %.1f", 1000.0 / currentMetrics.averageFrameTime);

        // Performance warnings
        if (currentMetrics.averageFrameTime > 33.33) // Below 30 FPS
        {
            Console.Printf("⚠️  Performance Warning: Frame rate below 30 FPS");

            if (currentMetrics.totalModelsLoaded > 50)
            {
                Console.Printf("  - Consider enabling LOD system");
            }

            if (currentMetrics.activeAnimations > 20)
            {
                Console.Printf("  - Consider animation culling");
            }
        }

        if (currentMetrics.totalModelsLoaded > 100)
        {
            Console.Printf("ℹ️  High model count - monitor memory usage");
        }
    }

    double GetFrameTime()
    {
        // Engine function to get current frame time in milliseconds
        return 16.67; // Placeholder - 60 FPS
    }
}
```

---

## Deployment & Distribution

### 1. Mod Packaging System

#### Professional Packaging Script
```bash
#!/bin/bash
# scripts/deploy_mod.sh

set -e

MOD_NAME="NeoDoomGLTFPack"
VERSION="1.0.0"
BUILD_DIR="build"
DIST_DIR="dist"
TEMP_DIR="temp_build"

echo "📦 Starting mod packaging for $MOD_NAME v$VERSION"

# Clean previous builds
rm -rf "$BUILD_DIR" "$DIST_DIR" "$TEMP_DIR"
mkdir -p "$BUILD_DIR" "$DIST_DIR" "$TEMP_DIR"

# Validate assets before packaging
echo "🔍 Validating assets..."
if ! ./scripts/validate_assets.sh; then
    echo "❌ Asset validation failed. Please fix issues before packaging."
    exit 1
fi

# Optimize textures
echo "🎨 Optimizing textures..."
./scripts/optimize_textures.sh

# Create mod structure
echo "📁 Creating mod structure..."
MOD_DIR="$TEMP_DIR/$MOD_NAME"
mkdir -p "$MOD_DIR"

# Copy core files
cp -r assets/ "$MOD_DIR/"
cp -r zscript/ "$MOD_DIR/"
cp -r maps/ "$MOD_DIR/" 2>/dev/null || true
cp -r sounds/ "$MOD_DIR/" 2>/dev/null || true

# Copy optimized textures
if [ -d "optimized_textures" ]; then
    echo "Using optimized textures..."
    rm -rf "$MOD_DIR/assets/textures"
    cp -r optimized_textures "$MOD_DIR/assets/textures"
fi

# Generate mod info
cat > "$MOD_DIR/modinfo.txt" << EOF
Name "$MOD_NAME"
Author "NeoDoom Development Team"
Version "$VERSION"
Description "Professional glTF 2.0 asset pack for NeoDoom featuring high-quality models, animations, and PBR materials."
Website "https://neodoom.example.com"
EOF

# Generate ZScript loader
cat > "$MOD_DIR/zscript.zs" << 'EOF'
version "4.11"

// Core glTF actor definitions
#include "zscript/actors/base_gltf_monster.zs"
#include "zscript/actors/player_gltf.zs"

// Monster replacements
#include "zscript/actors/monsters/gltf_cyberdemon.zs"
#include "zscript/actors/monsters/gltf_imp.zs"
#include "zscript/actors/monsters/gltf_baron.zs"
#include "zscript/actors/monsters/gltf_cacodemon.zs"

// Weapon replacements
#include "zscript/weapons/gltf_pistol.zs"
#include "zscript/weapons/gltf_shotgun.zs"
#include "zscript/weapons/gltf_chaingun.zs"
#include "zscript/weapons/gltf_rocket_launcher.zs"

// Item replacements
#include "zscript/items/gltf_health_bonus.zs"
#include "zscript/items/gltf_armor_bonus.zs"
#include "zscript/items/gltf_powerups.zs"

// System components
#include "zscript/systems/animation_controller.zs"
#include "zscript/systems/lod_manager.zs"
#include "zscript/systems/performance_monitor.zs"
EOF

# Generate MAPINFO
cat > "$MOD_DIR/mapinfo.txt" << 'EOF'
gameinfo
{
    playerclasses = "GLTFDoomPlayer"
    statusbarclass = "DoomStatusBar"
    menufontcolor_title = "Red"
    menufontcolor_label = "Red"
    menufontcolor_value = "Gray"
    menufontcolor_action = "Red"
    menufontcolor_header = "Gold"
    menufontcolor_highlight = "Yellow"
    menufontcolor_selection = "Brick"

    // Enable glTF features
    defaultrespawntime = 30
    defaultdropstyle = 1
}

// Default replacement classes
DoomPlayer = GLTFDoomPlayer
Cyberdemon = GLTFCyberdemon
DoomImp = GLTFImp
BaronOfHell = GLTFBaron
Cacodemon = GLTFCacodemon

Pistol = GLTFPistol
Shotgun = GLTFShotgun
Chaingun = GLTFChaingun
RocketLauncher = GLTFRocketLauncher

HealthBonus = GLTFHealthBonus
ArmorBonus = GLTFArmorBonus
EOF

# Generate readme
cat > "$MOD_DIR/README.md" << EOF
# $MOD_NAME v$VERSION

Professional glTF 2.0 asset pack for NeoDoom featuring high-quality models, skeletal animations, and PBR materials.

## Features

- **Complete Asset Replacement**: Players, monsters, weapons, and items
- **High-Quality Models**: Professional 3D assets with PBR materials
- **Skeletal Animations**: Smooth, realistic character animations
- **Performance Optimized**: LOD system and intelligent culling
- **Blender Compatible**: Assets created with industry-standard workflow

## Installation

1. Ensure you have NeoDoom with glTF support installed
2. Extract this mod to your NeoDoom mods directory
3. Load the mod with: \`neodoom -file $MOD_NAME.pk3\`

## System Requirements

- NeoDoom with glTF support enabled
- OpenGL 3.3 or Vulkan support
- 4GB RAM minimum, 8GB recommended
- 2GB free disk space

## Performance Notes

- Models automatically switch detail based on distance
- Animations are culled beyond view distance
- PBR materials can be disabled in settings for older hardware

## Credits

- Development Team: NeoDoom Contributors
- Asset Creation: Professional 3D Artists
- Engine Integration: NeoDoom glTF Team

## License

This mod is released under the same license as NeoDoom.
Assets are provided under Creative Commons Attribution 4.0.

## Support

For issues or questions:
- GitHub: https://github.com/neodoom/gltf-pack
- Discord: https://discord.gg/neodoom
- Forum: https://forum.neodoom.com

---

Generated: $(date)
Version: $VERSION
EOF

# Generate asset manifest
echo "📋 Generating asset manifest..."
{
    echo "# Asset Manifest for $MOD_NAME v$VERSION"
    echo "Generated: $(date)"
    echo ""
    echo "## Models"
    find "$MOD_DIR/assets/models" -name "*.glb" -o -name "*.gltf" | while read model; do
        rel_path=${model#$MOD_DIR/}
        size=$(stat -f%z "$model" 2>/dev/null || stat -c%s "$model")
        size_mb=$(echo "scale=2; $size/1024/1024" | bc)
        echo "- $rel_path (${size_mb}MB)"
    done

    echo ""
    echo "## Textures"
    find "$MOD_DIR/assets/textures" -name "*.png" -o -name "*.jpg" -o -name "*.tga" | while read texture; do
        rel_path=${texture#$MOD_DIR/}
        if command -v identify &> /dev/null; then
            dimensions=$(identify -format "%wx%h" "$texture" 2>/dev/null)
            echo "- $rel_path ($dimensions)"
        else
            echo "- $rel_path"
        fi
    done

} > "$MOD_DIR/MANIFEST.md"

# Create PK3 archive
echo "🗜️  Creating PK3 archive..."
cd "$TEMP_DIR"
zip -r "../$DIST_DIR/$MOD_NAME-v$VERSION.pk3" "$MOD_NAME/" -x "*.DS_Store" "*/.*"
cd ..

# Create additional distribution formats
echo "📚 Creating additional distribution formats..."

# Create development package (with sources)
zip -r "$DIST_DIR/$MOD_NAME-v$VERSION-dev.zip" \
    assets/ zscript/ maps/ sounds/ scripts/ \
    *.md *.sh *.txt \
    -x "*.DS_Store" "*/.*" "build/*" "dist/*" "temp_build/*"

# Create installer script
cat > "$DIST_DIR/install.sh" << 'EOF'
#!/bin/bash
# NeoDoom glTF Pack Installer

set -e

MOD_FILE="$1"
NEODOOM_DIR="$2"

if [ -z "$MOD_FILE" ] || [ -z "$NEODOOM_DIR" ]; then
    echo "Usage: $0 <mod_file.pk3> <neodoom_directory>"
    echo "Example: $0 NeoDoomGLTFPack-v1.0.0.pk3 /usr/local/games/neodoom"
    exit 1
fi

if [ ! -f "$MOD_FILE" ]; then
    echo "Error: Mod file '$MOD_FILE' not found"
    exit 1
fi

if [ ! -d "$NEODOOM_DIR" ]; then
    echo "Error: NeoDoom directory '$NEODOOM_DIR' not found"
    exit 1
fi

MODS_DIR="$NEODOOM_DIR/mods"
mkdir -p "$MODS_DIR"

echo "Installing $(basename "$MOD_FILE") to $MODS_DIR..."
cp "$MOD_FILE" "$MODS_DIR/"

echo "✅ Installation complete!"
echo ""
echo "To run NeoDoom with this mod:"
echo "  cd $NEODOOM_DIR"
echo "  ./neodoom -file mods/$(basename "$MOD_FILE")"
EOF

chmod +x "$DIST_DIR/install.sh"

# Generate distribution report
{
    echo "# Distribution Package Report"
    echo ""
    echo "**Generated:** $(date)"
    echo "**Version:** $VERSION"
    echo ""
    echo "## Package Contents"
    echo ""

    for file in "$DIST_DIR"/*; do
        if [ -f "$file" ]; then
            filename=$(basename "$file")
            size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file")
            size_mb=$(echo "scale=2; $size/1024/1024" | bc)
            echo "- **$filename**: ${size_mb}MB"
        fi
    done

    echo ""
    echo "## Installation Instructions"
    echo ""
    echo "### Standard Installation"
    echo "1. Download \`$MOD_NAME-v$VERSION.pk3\`"
    echo "2. Place in your NeoDoom mods directory"
    echo "3. Load with: \`neodoom -file $MOD_NAME-v$VERSION.pk3\`"
    echo ""
    echo "### Automated Installation (Linux/macOS)"
    echo "1. Download both \`$MOD_NAME-v$VERSION.pk3\` and \`install.sh\`"
    echo "2. Run: \`./install.sh $MOD_NAME-v$VERSION.pk3 /path/to/neodoom\`"
    echo ""
    echo "### Development Installation"
    echo "1. Download \`$MOD_NAME-v$VERSION-dev.zip\`"
    echo "2. Extract to your development directory"
    echo "3. Modify and rebuild as needed"

} > "$DIST_DIR/README.md"

# Calculate and display final statistics
echo ""
echo "✅ Packaging complete!"
echo ""
echo "📊 Distribution Statistics:"

total_size=0
for file in "$DIST_DIR"/*; do
    if [ -f "$file" ]; then
        filename=$(basename "$file")
        size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file")
        size_mb=$(echo "scale=2; $size/1024/1024" | bc)
        echo "  $filename: ${size_mb}MB"
        total_size=$(echo "$total_size + $size" | bc)
    fi
done

total_mb=$(echo "scale=2; $total_size/1024/1024" | bc)
echo "  Total: ${total_mb}MB"

echo ""
echo "📁 Distribution files created in: $DIST_DIR/"
echo ""
echo "🚀 Ready for distribution!"

# Clean up temporary files
rm -rf "$TEMP_DIR"

echo "🧹 Cleanup complete"
```

### 2. Version Control & Asset Management

#### Git LFS Configuration
```bash
# .gitattributes for proper asset management
# 3D Models
*.glb filter=lfs diff=lfs merge=lfs -text
*.gltf filter=lfs diff=lfs merge=lfs -text

# Textures
*.png filter=lfs diff=lfs merge=lfs -text
*.jpg filter=lfs diff=lfs merge=lfs -text
*.jpeg filter=lfs diff=lfs merge=lfs -text
*.tga filter=lfs diff=lfs merge=lfs -text
*.tiff filter=lfs diff=lfs merge=lfs -text

# Audio
*.wav filter=lfs diff=lfs merge=lfs -text
*.ogg filter=lfs diff=lfs merge=lfs -text
*.mp3 filter=lfs diff=lfs merge=lfs -text

# Video
*.mp4 filter=lfs diff=lfs merge=lfs -text
*.avi filter=lfs diff=lfs merge=lfs -text

# Blender files
*.blend filter=lfs diff=lfs merge=lfs -text

# Archives
*.zip filter=lfs diff=lfs merge=lfs -text
*.pk3 filter=lfs diff=lfs merge=lfs -text
*.wad filter=lfs diff=lfs merge=lfs -text

# Exclude from LFS (text files)
*.txt text
*.md text
*.zs text
*.cfg text
*.ini text
*.json text
*.xml text
*.yml text
*.yaml text
```

### 3. Continuous Integration

#### GitHub Actions Workflow
```yaml
# .github/workflows/asset-validation.yml
name: Asset Validation and Packaging

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main ]
  release:
    types: [ created ]

jobs:
  validate-assets:
    runs-on: ubuntu-latest

    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
      with:
        lfs: true

    - name: Setup Node.js
      uses: actions/setup-node@v4
      with:
        node-version: '18'

    - name: Install validation tools
      run: |
        npm install -g gltf-validator
        sudo apt-get update
        sudo apt-get install -y imagemagick bc

    - name: Validate glTF assets
      run: |
        chmod +x scripts/validate_assets.sh
        ./scripts/validate_assets.sh

    - name: Upload validation reports
      uses: actions/upload-artifact@v4
      if: always()
      with:
        name: validation-reports
        path: validation_reports/
        retention-days: 30

  package-mod:
    needs: validate-assets
    runs-on: ubuntu-latest
    if: github.event_name == 'release'

    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
      with:
        lfs: true

    - name: Setup build environment
      run: |
        sudo apt-get update
        sudo apt-get install -y imagemagick bc zip

    - name: Build and package mod
      run: |
        chmod +x scripts/deploy_mod.sh
        ./scripts/deploy_mod.sh

    - name: Upload distribution packages
      uses: actions/upload-artifact@v4
      with:
        name: distribution-packages
        path: dist/
        retention-days: 90

    - name: Attach to release
      uses: softprops/action-gh-release@v1
      if: github.event_name == 'release'
      with:
        files: |
          dist/*.pk3
          dist/*.zip
          dist/README.md
      env:
        GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}

  performance-test:
    needs: validate-assets
    runs-on: ubuntu-latest

    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
      with:
        lfs: true

    - name: Build NeoDoom with glTF support
      run: |
        # This would build NeoDoom if source is available
        echo "Performance testing would require NeoDoom build"

    - name: Run performance benchmarks
      run: |
        # Run automated performance tests
        echo "Performance benchmarks would run here"

    - name: Generate performance report
      run: |
        echo "Performance report generation"
```

---

## Troubleshooting & Best Practices

### 1. Common Issues & Solutions

#### glTF Export Problems

**Issue: Animations not exporting correctly**
```python
# Blender script to fix animation export issues
import bpy

def fix_animation_export():
    # Ensure all animations are baked
    for action in bpy.data.actions:
        # Set action as active
        if bpy.context.object and bpy.context.object.animation_data:
            bpy.context.object.animation_data.action = action

            # Bake animation
            bpy.ops.nla.bake(
                frame_start=int(action.frame_range[0]),
                frame_end=int(action.frame_range[1]),
                only_selected=False,
                visual_keying=True,
                clear_constraints=False,
                use_current_action=True,
                bake_types={'POSE'}
            )
```

**Issue: Bone count exceeds limits**
```python
def optimize_bone_hierarchy():
    armature = bpy.context.active_object
    if armature and armature.type == 'ARMATURE':
        # Remove unused bones
        bpy.ops.object.mode_set(mode='EDIT')

        bones_to_remove = []
        for bone in armature.data.edit_bones:
            # Check if bone has any vertex weights
            has_weights = False
            for obj in bpy.context.scene.objects:
                if obj.type == 'MESH':
                    for group in obj.vertex_groups:
                        if group.name == bone.name:
                            has_weights = True
                            break

            if not has_weights and len(bone.children) == 0:
                bones_to_remove.append(bone)

        for bone in bones_to_remove:
            armature.data.edit_bones.remove(bone)

        bpy.ops.object.mode_set(mode='OBJECT')
        print(f"Removed {len(bones_to_remove)} unused bones")
```

#### Performance Issues

**Issue: Frame rate drops with many glTF models**
```zscript
// Implement aggressive LOD system
class AggressiveLODManager : GLTFLODManager
{
    override void UpdateActorLOD(Actor target)
    {
        Actor viewer = players[consoleplayer].mo;
        if (!viewer) return;

        double distance = target.Distance3D(viewer);

        // More aggressive culling distances
        if (distance > 768.0) // Reduced from 2048
        {
            target.A_SetModel(""); // Completely disable model
            return;
        }

        if (distance > 384.0) // Reduced from 1024
        {
            target.A_SetModelAnimation(""); // Disable animations
        }

        Super.UpdateActorLOD(target);
    }
}
```

**Issue: Memory usage too high**
```zscript
// Implement texture streaming
class TextureStreamer : Thinker
{
    private Map<String, double> textureLastUsed;
    private const TEXTURE_TIMEOUT = 120.0; // 2 minutes

    void UnloadUnusedTextures()
    {
        double currentTime = level.time / 35.0; // Convert to seconds

        Array<String> texturesToUnload;
        MapIterator<String, double> it;
        it.Init(textureLastUsed);

        while (it.Next())
        {
            if (currentTime - it.GetValue() > TEXTURE_TIMEOUT)
            {
                texturesToUnload.Push(it.GetKey());
            }
        }

        for (int i = 0; i < texturesToUnload.Size(); i++)
        {
            // Unload texture from GPU memory
            Console.Printf("Unloading texture: %s", texturesToUnload[i]);
            textureLastUsed.Remove(texturesToUnload[i]);
        }
    }
}
```

### 2. Optimization Guidelines

#### Model Optimization Checklist
- [ ] **Polygon Count**: Under 5000 triangles for characters, 2000 for weapons
- [ ] **Bone Count**: Maximum 128 bones per armature
- [ ] **Texture Resolution**: 1024x1024 maximum for characters, 512x512 for weapons
- [ ] **Animation Length**: Keep under 10 seconds per animation
- [ ] **Material Count**: Maximum 4 materials per model

#### Performance Targets
```yaml
target_performance:
  frame_rate:
    minimum: 30fps
    target: 60fps
    ideal: 120fps

  memory_usage:
    models: 500MB maximum
    textures: 1GB maximum
    animations: 200MB maximum

  loading_times:
    model_load: 100ms maximum
    animation_start: 50ms maximum
    texture_load: 200ms maximum
```

### 3. Quality Standards

#### Visual Quality Requirements
- **Texture Quality**: Minimum 512x512, prefer 1024x1024
- **Normal Maps**: Always include for surface detail
- **PBR Materials**: Use full metallic-roughness workflow
- **Animation Quality**: 30fps minimum, 60fps preferred
- **UV Mapping**: Clean, non-overlapping UVs with proper padding

#### Technical Standards
- **File Formats**: GLB preferred over glTF+bin
- **Compression**: No Draco compression (compatibility issues)
- **Validation**: Must pass official glTF validator
- **Naming**: Consistent naming convention throughout
- **Documentation**: All assets documented in manifest

---

## Advanced Techniques

### 1. Dynamic Model Swapping

#### Context-Aware Model System
```zscript
class DynamicModelManager : Thinker
{
    struct ModelVariant
    {
        String basePath;
        String condition;
        int priority;
    }

    private Map<String, Array<ModelVariant>> modelVariants;

    void RegisterModelVariant(String actorClass, String modelPath, String condition, int priority)
    {
        ModelVariant variant;
        variant.basePath = modelPath;
        variant.condition = condition;
        variant.priority = priority;

        modelVariants[actorClass].Push(variant);

        // Sort by priority
        Array<ModelVariant> variants = modelVariants[actorClass];
        variants.Sort("CompareVariantPriority");
        modelVariants[actorClass] = variants;
    }

    String GetOptimalModel(Actor target)
    {
        String actorClass = target.GetClassName();

        if (!modelVariants.CheckKey(actorClass))
            return "";

        Array<ModelVariant> variants = modelVariants[actorClass];

        for (int i = 0; i < variants.Size(); i++)
        {
            if (EvaluateCondition(target, variants[i].condition))
            {
                return variants[i].basePath;
            }
        }

        return "";
    }

    bool EvaluateCondition(Actor target, String condition)
    {
        // Parse and evaluate condition string
        if (condition ~== "low_health")
            return target.health < target.default.health * 0.3;

        if (condition ~== "combat")
            return target.target != null;

        if (condition ~== "distance_close")
        {
            Actor player = players[consoleplayer].mo;
            return player && target.Distance3D(player) < 512.0;
        }

        if (condition ~== "night")
            return level.lightlevel < 128;

        return condition ~== "default";
    }

    void InitializeVariants()
    {
        // Marine variants
        RegisterModelVariant("DoomPlayer", "models/players/marine_default.glb", "default", 0);
        RegisterModelVariant("DoomPlayer", "models/players/marine_combat.glb", "combat", 10);
        RegisterModelVariant("DoomPlayer", "models/players/marine_injured.glb", "low_health", 20);

        // Demon variants
        RegisterModelVariant("DoomImp", "models/monsters/imp_default.glb", "default", 0);
        RegisterModelVariant("DoomImp", "models/monsters/imp_aggressive.glb", "combat", 10);
        RegisterModelVariant("DoomImp", "models/monsters/imp_stealth.glb", "night", 5);
    }
}
```

### 2. Procedural Animation Blending

#### Advanced Animation Controller
```zscript
class ProceduralAnimationController : Thinker
{
    struct AnimationBlend
    {
        String primaryAnimation;
        String secondaryAnimation;
        double blendWeight;
        double blendSpeed;
        String trigger;
    }

    private Map<Actor, Array<AnimationBlend>> activeBlends;

    void StartAnimationBlend(Actor target, String primary, String secondary,
                            double weight, double speed, String trigger)
    {
        AnimationBlend blend;
        blend.primaryAnimation = primary;
        blend.secondaryAnimation = secondary;
        blend.blendWeight = weight;
        blend.blendSpeed = speed;
        blend.trigger = trigger;

        activeBlends[target].Push(blend);
    }

    override void Tick()
    {
        MapIterator<Actor, Array<AnimationBlend>> it;
        it.Init(activeBlends);

        while (it.Next())
        {
            Actor target = it.GetKey();
            Array<AnimationBlend> blends = it.GetValue();

            for (int i = blends.Size() - 1; i >= 0; i--)
            {
                UpdateBlend(target, blends[i], i);
            }
        }
    }

    void UpdateBlend(Actor target, AnimationBlend blend, int index)
    {
        // Check if blend trigger is still active
        bool triggerActive = EvaluateTrigger(target, blend.trigger);

        if (triggerActive)
        {
            // Increase blend weight
            blend.blendWeight = min(1.0, blend.blendWeight + blend.blendSpeed * (1.0/35.0));
        }
        else
        {
            // Decrease blend weight
            blend.blendWeight = max(0.0, blend.blendWeight - blend.blendSpeed * (1.0/35.0));
        }

        // Apply blend
        if (blend.blendWeight > 0.0)
        {
            target.A_SetModelAnimationBlend(
                blend.primaryAnimation,
                blend.secondaryAnimation,
                blend.blendWeight
            );
        }
        else
        {
            // Remove blend when weight reaches zero
            target.A_SetModelAnimation(blend.primaryAnimation);
            Array<AnimationBlend> blends = activeBlends[target];
            blends.Delete(index);
            activeBlends[target] = blends;
        }

        // Update the blend in the array
        Array<AnimationBlend> blends = activeBlends[target];
        blends[index] = blend;
        activeBlends[target] = blends;
    }

    bool EvaluateTrigger(Actor target, String trigger)
    {
        switch (trigger)
        {
            case "moving":
                return target.vel.length() > 0;

            case "aiming":
                return target.target != null && target.CheckSight(target.target);

            case "reloading":
                if (target.player && target.player.ReadyWeapon)
                    return target.player.ReadyWeapon.bAltFire; // Simplified check
                return false;

            case "injured":
                return target.health < target.default.health * 0.5;
        }

        return false;
    }
}
```

### 3. Intelligent Texture Streaming

#### Smart Texture Manager
```zscript
class SmartTextureManager : Thinker
{
    struct TextureUsageData
    {
        String texturePath;
        double lastAccess;
        int accessCount;
        double totalDistance;
        bool isLoaded;
        int priority;
    }

    private Map<String, TextureUsageData> textureDatabase;
    private const MAX_LOADED_TEXTURES = 200;
    private const PREDICTION_DISTANCE = 1024.0;

    void PredictTextureUsage()
    {
        Actor player = players[consoleplayer].mo;
        if (!player) return;

        // Find all actors within prediction distance
        ThinkerIterator it = ThinkerIterator.Create("Actor");
        Actor mo;

        while (mo = Actor(it.Next()))
        {
            if (mo.Distance3D(player) <= PREDICTION_DISTANCE)
            {
                String modelPath = mo.GetModelPath();
                if (modelPath.Length() > 0)
                {
                    PredictModelTextures(modelPath, mo.Distance3D(player));
                }
            }
        }

        // Preload high-priority textures
        PreloadPriorityTextures();
    }

    void PredictModelTextures(String modelPath, double distance)
    {
        // Get texture list for model (would need engine support)
        Array<String> modelTextures; // = GetModelTextures(modelPath);

        for (int i = 0; i < modelTextures.Size(); i++)
        {
            String texturePath = modelTextures[i];

            if (!textureDatabase.CheckKey(texturePath))
            {
                TextureUsageData data;
                data.texturePath = texturePath;
                data.lastAccess = 0;
                data.accessCount = 0;
                data.totalDistance = 0;
                data.isLoaded = false;
                data.priority = 0;
                textureDatabase[texturePath] = data;
            }

            TextureUsageData data = textureDatabase[texturePath];
            data.accessCount++;
            data.totalDistance += distance;
            data.lastAccess = level.time;

            // Calculate priority based on proximity and usage
            double avgDistance = data.totalDistance / data.accessCount;
            data.priority = int((PREDICTION_DISTANCE - avgDistance) * data.accessCount);

            textureDatabase[texturePath] = data;
        }
    }

    void PreloadPriorityTextures()
    {
        // Sort textures by priority
        Array<String> textureList;
        Array<int> priorities;

        MapIterator<String, TextureUsageData> it;
        it.Init(textureDatabase);

        while (it.Next())
        {
            textureList.Push(it.GetKey());
            priorities.Push(it.GetValue().priority);
        }

        // Simple bubble sort by priority (could be optimized)
        for (int i = 0; i < textureList.Size() - 1; i++)
        {
            for (int j = 0; j < textureList.Size() - i - 1; j++)
            {
                if (priorities[j] < priorities[j + 1])
                {
                    // Swap textures
                    String tempTexture = textureList[j];
                    textureList[j] = textureList[j + 1];
                    textureList[j + 1] = tempTexture;

                    // Swap priorities
                    int tempPriority = priorities[j];
                    priorities[j] = priorities[j + 1];
                    priorities[j + 1] = tempPriority;
                }
            }
        }

        // Load top priority textures
        int loadedCount = 0;
        for (int i = 0; i < textureList.Size() && loadedCount < MAX_LOADED_TEXTURES; i++)
        {
            String texturePath = textureList[i];
            TextureUsageData data = textureDatabase[texturePath];

            if (!data.isLoaded && data.priority > 0)
            {
                LoadTexture(texturePath);
                data.isLoaded = true;
                textureDatabase[texturePath] = data;
                loadedCount++;
            }
        }
    }

    void LoadTexture(String texturePath)
    {
        // Engine function to preload texture
        Console.Printf("Preloading texture: %s", texturePath);
        // A_PreloadTexture(texturePath);
    }
}
```

---

## Conclusion

This comprehensive workflow guide establishes professional standards for glTF 2.0 integration in NeoDoom, covering everything from Blender export to final distribution. The system provides:

- **Professional Asset Pipeline**: Industry-standard workflows for creating high-quality glTF models
- **Complete Integration**: Seamless replacement of all DOOM asset categories
- **Performance Optimization**: Intelligent LOD, culling, and streaming systems
- **Quality Assurance**: Comprehensive validation and testing frameworks
- **Professional Deployment**: Automated packaging and distribution systems

By following these workflows, developers can create professional-quality NeoDoom modifications that leverage the full power of modern 3D graphics while maintaining compatibility with the classic DOOM gameplay experience.

The integration of glTF 2.0 support transforms NeoDoom into a modern game engine capable of supporting contemporary 3D assets while preserving the performance and gameplay characteristics that make DOOM timeless.