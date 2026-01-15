## BiasedDoom glTF ZScript API Reference

**Version 2.0 - Enhanced Blender Integration**

Complete API documentation for using glTF 2.0 models with skeletal animation in ZScript. This version includes comprehensive Blender-friendly bone manipulation and advanced animation control.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [GLTFModel Mixin](#gltfmodel-mixin)
3. [Core Classes](#core-classes)
4. [Animation System](#animation-system)
5. [Bone Manipulation](#bone-manipulation)
6. [Blender-Friendly Bone API](#blender-friendly-bone-api) ⭐ NEW
7. [Advanced Animation Blending](#advanced-animation-blending) ⭐ NEW
8. [Attachment System](#attachment-system)
9. [PBR Material Control](#pbr-material-control)
10. [Quaternion Math Helpers](#quaternion-math-helpers) ⭐ NEW
11. [Complete Examples](#complete-examples)
12. [Best Practices](#best-practices)
13. [Troubleshooting](#troubleshooting)

---

## Quick Start

### Minimal Example

```javascript
class MyGLTFActor : Actor
{
    mixin GLTFModel;  // Add glTF support

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            // Initialize model
            InitGLTFModel("models/myactor/myactor.gltf");
            SetModelScaleUniform(1.0);
            PlayAnimation("Idle", loop: true);
        }
        TNT1 A 1
        {
            UpdateGLTFModel();  // Update every tic
        }
        Loop;
    }
}
```

### Required Steps

1. **Add mixin**: `mixin GLTFModel;`
2. **Initialize**: Call `InitGLTFModel()` once
3. **Update**: Call `UpdateGLTFModel()` every tic
4. **Use TNT1 sprite**: glTF models replace sprites entirely

---

## GLTFModel Mixin

The `GLTFModel` mixin adds glTF support to any actor class.

### Adding to Your Actor

```javascript
class MyActor : Actor
{
    mixin GLTFModel;  // Adds all glTF functionality

    // Your actor definition...
}
```

### What It Provides

- Model initialization and management
- Animation playback system
- Bone manipulation (procedural animation)
- Attachment points for child actors
- PBR material control
- Automatic update management

---

## Core Classes

### GLTFModelDef

Stores model configuration and properties.

#### Properties

```javascript
String modelPath;           // Path to .gltf/.glb file
String modelName;           // Friendly name

// Transform
Vector3 scale;              // Scale multiplier (X, Y, Z)
Vector3 offset;             // Position offset from origin
Vector3 rotation;           // Rotation offset (pitch, yaw, roll)

// Flags
bool inheritActorScale;     // Use actor's scale property
bool inheritActorAngle;     // Use actor's angle
bool inheritActorPitch;     // Use actor's pitch
bool inheritActorRoll;      // Use actor's roll

// PBR
bool enablePBR;             // Enable PBR rendering
double metallicFactor;      // Metallic multiplier (0.0-1.0)
double roughnessFactor;     // Roughness multiplier (0.0-1.0)
Color emissiveColor;        // Emissive tint
double emissiveStrength;    // Emissive intensity

// Animation
String defaultAnimation;    // Default animation name
double animationSpeed;      // Global speed multiplier
bool loopAnimation;         // Default loop behavior

// Rendering
bool castShadows;           // Cast dynamic shadows
bool receiveShadows;        // Receive shadows
int shadowQuality;          // 0=low, 1=med, 2=high
```

#### Creation

```javascript
// Automatic (via InitGLTFModel)
InitGLTFModel("path/to/model.gltf");

// Manual
GLTFModelDef def = GLTFModelDef.Create("path/to/model.gltf");
```

### GLTFAnimationState

Tracks current animation playback.

#### Properties

```javascript
String animationName;       // Current animation
double currentTime;         // Playback time (seconds)
double duration;            // Total duration
double playbackSpeed;       // Speed multiplier
bool isLooping;             // Is looping
bool isPlaying;             // Is playing
bool isPaused;              // Is paused
double blendTime;           // Blend duration
double blendProgress;       // Current blend (0.0-1.0)
```

#### Callbacks

```javascript
void OnAnimationStart();    // Called when animation starts
void OnAnimationEnd();      // Called when animation ends
void OnAnimationLoop();     // Called on loop
```

---

## Animation System

### Initialization

#### InitGLTFModel()

Initialize glTF model for this actor.

```javascript
void InitGLTFModel(String modelPath)
```

**Parameters:**
- `modelPath`: Path to .gltf or .glb file (relative to game root)

**Example:**
```javascript
InitGLTFModel("models/monsters/demon/demon.gltf");
```

**Notes:**
- Call once, typically in `Spawn` state with `NoDelay`
- Path is relative to game directory or PK3 root
- Supports both .gltf (JSON) and .glb (binary) formats

#### HasGLTFModel()

Check if model is initialized.

```javascript
bool HasGLTFModel()
```

**Returns:** `true` if model is initialized and ready

**Example:**
```javascript
if (!HasGLTFModel())
{
    InitGLTFModel("models/item.gltf");
}
```

### Playback Control

#### PlayAnimation()

Play animation by name with optional looping and blending.

```javascript
void PlayAnimation(String animName, bool loop = true, double blendTime = 0.2)
```

**Parameters:**
- `animName`: Name of animation (from Blender Action name)
- `loop`: Should animation loop (default: true)
- `blendTime`: Seconds to blend from previous animation (default: 0.2)

**Examples:**
```javascript
// Simple loop
PlayAnimation("Idle", loop: true);

// One-shot animation
PlayAnimation("Fire", loop: false);

// Quick blend for combat
PlayAnimation("Attack", loop: false, blendTime: 0.1);

// Slow cinematic blend
PlayAnimation("Death", loop: false, blendTime: 0.5);
```

**Notes:**
- Animation names are case-sensitive
- Must match Blender Action names exactly
- If same animation is already playing, does nothing
- Use short blend times (0.05-0.15) for snappy actions
- Use longer blend times (0.3-0.5) for smooth transitions

#### StopAnimation()

Stop current animation.

```javascript
void StopAnimation()
```

**Example:**
```javascript
StopAnimation();  // Freeze at current frame
```

#### PauseAnimation() / ResumeAnimation()

Pause/resume animation playback.

```javascript
void PauseAnimation()
void ResumeAnimation()
```

**Example:**
```javascript
// Freeze frame effect
PauseAnimation();
// ... wait ...
ResumeAnimation();
```

#### SetAnimationSpeed()

Set playback speed multiplier.

```javascript
void SetAnimationSpeed(double speed)
```

**Parameters:**
- `speed`: Speed multiplier (1.0 = normal, 2.0 = double speed, 0.5 = half speed)

**Examples:**
```javascript
SetAnimationSpeed(1.0);   // Normal speed
SetAnimationSpeed(2.0);   // Fast (running, panic)
SetAnimationSpeed(0.5);   // Slow (wounded, dramatic)
SetAnimationSpeed(0.0);   // Frozen (same as Pause)
```

### Animation State Queries

#### GetCurrentAnimation()

Get name of currently playing animation.

```javascript
String GetCurrentAnimation()
```

**Returns:** Current animation name, or empty string if none

**Example:**
```javascript
String current = GetCurrentAnimation();
if (current == "Idle")
{
    PlayAnimation("Walk", loop: true);
}
```

#### IsAnimationPlaying()

Check if animation is currently playing.

```javascript
bool IsAnimationPlaying()
```

**Returns:** `true` if animation is playing and not paused

**Example:**
```javascript
if (!IsAnimationPlaying())
{
    PlayAnimation("Idle", loop: true);
}
```

#### GetAnimationProgress()

Get animation progress as percentage.

```javascript
double GetAnimationProgress()
```

**Returns:** Value from 0.0 to 1.0 (0% to 100%)

**Example:**
```javascript
double progress = GetAnimationProgress();
if (progress > 0.5)  // Halfway through
{
    // Trigger event (e.g., muzzle flash at 50% of fire animation)
    SpawnMuzzleFlash();
}
```

---

## Transform Control

### SetModelScale()

Set model scale on each axis independently.

```javascript
void SetModelScale(double x, double y, double z)
```

**Parameters:**
- `x`: X-axis scale
- `y`: Y-axis scale
- `z`: Z-axis scale

**Example:**
```javascript
SetModelScale(1.0, 1.0, 1.5);  // Stretch vertically
```

### SetModelScaleUniform()

Set uniform scale (same on all axes).

```javascript
void SetModelScaleUniform(double scale)
```

**Parameters:**
- `scale`: Uniform scale multiplier

**Example:**
```javascript
SetModelScaleUniform(2.0);  // Double size
SetModelScaleUniform(0.5);  // Half size
```

**Common scales:**
- Items/pickups: 4.0 - 8.0
- Weapons: 2.0 - 3.0
- Monsters: 0.8 - 1.2
- Props: 1.0 - 2.0

### SetModelOffset()

Set positional offset from actor origin.

```javascript
void SetModelOffset(double x, double y, double z)
```

**Parameters:**
- `x`: Left/right offset (negative = left)
- `y`: Forward/back offset (positive = forward)
- `z`: Up/down offset (positive = up)

**Examples:**
```javascript
// Weapon positioning
SetModelOffset(0, 15, 28);  // Pistol
SetModelOffset(0, 18, 25);  // Shotgun

// Floating item
SetModelOffset(0, 0, 8);    // Hover above ground

// Centered monster
SetModelOffset(0, 0, 0);    // At feet
```

### SetModelRotation()

Set rotational offset.

```javascript
void SetModelRotation(double pitch, double yaw, double roll)
```

**Parameters:**
- `pitch`: Up/down rotation (degrees)
- `yaw`: Left/right rotation (degrees)
- `roll`: Tilt rotation (degrees)

**Example:**
```javascript
SetModelRotation(0, 90, 0);  // Rotate 90° right
```

---

## Bone Manipulation

Advanced procedural animation by directly controlling bones.

### AddBoneOverride()

Override bone transformation for procedural effects.

```javascript
void AddBoneOverride(String boneName, Vector3 posOffset, Quat rotOffset, double influence = 1.0)
```

**Parameters:**
- `boneName`: Name of bone to control
- `posOffset`: Position offset from bind pose
- `rotOffset`: Rotation offset (quaternion)
- `influence`: Blend factor 0.0-1.0 (1.0 = full override)

**Example:**
```javascript
// Head look-at
Quat headRot = Quat.FromEulerAngles(pitch, yaw, 0);
AddBoneOverride("Head", (0,0,0), headRot, 0.5);  // 50% influence

// Breathing effect
double breathe = sin(level.time * 0.05) * 0.2;
Vector3 chestOffset = (0, 0, breathe);
AddBoneOverride("Chest", chestOffset, Quat.Identity(), 1.0);

// Weapon recoil
Quat recoil = Quat.FromEulerAngles(-15, 0, 0);  // Kick up
AddBoneOverride("Weapon_Root", (0, -0.5, 0), recoil, 1.0);
```

### RemoveBoneOverride()

Remove bone override by name.

```javascript
void RemoveBoneOverride(String boneName)
```

**Example:**
```javascript
RemoveBoneOverride("Head");  // Stop overriding head
```

### ClearBoneOverrides()

Remove all bone overrides.

```javascript
void ClearBoneOverrides()
```

**Example:**
```javascript
ClearBoneOverrides();  // Return all bones to animation
```

### GetBoneWorldPosition()

Get bone's world-space position.

```javascript
Vector3 GetBoneWorldPosition(String boneName)
```

**Returns:** World position of bone

**Example:**
```javascript
Vector3 handPos = GetBoneWorldPosition("Hand_R");
SpawnProjectile(handPos);  // Spawn at hand
```

### GetBoneWorldRotation()

Get bone's world-space rotation as quaternion.

```javascript
Quat GetBoneWorldRotation(String boneName)
```

**Returns:** World rotation of bone

**Example:**
```javascript
Quat handRot = GetBoneWorldRotation("Hand_R");
// Use for orienting spawned actors
```

---

## Blender-Friendly Bone API

⭐ **NEW in v2.0**: Enhanced bone manipulation API designed to work seamlessly with Blender's bone naming and transform system.

### FindBoneIndex()

Find bone index by name (supports Blender bone names).

```javascript
int FindBoneIndex(String boneName)
```

**Returns:** Bone index, or -1 if not found

**Example:**
```javascript
int spineIndex = FindBoneIndex("Spine_01");
if (spineIndex >= 0) {
    Console.Printf("Found spine bone at index %d", spineIndex);
}
```

### GetBoneCount()

Get total number of bones in the armature.

```javascript
int GetBoneCount()
```

**Returns:** Number of bones

**Example:**
```javascript
int boneCount = GetBoneCount();
Console.Printf("Model has %d bones", boneCount);

// Iterate through all bones
for (int i = 0; i < boneCount; i++) {
    String boneName = GetBoneName(i);
    Console.Printf("Bone %d: %s", i, boneName);
}
```

### GetBoneName()

Get bone name by index (useful for iteration).

```javascript
String GetBoneName(int index)
```

**Returns:** Bone name, or empty string if invalid

**Example:**
```javascript
String headBoneName = GetBoneName(5);
```

### BoneExists()

Check if bone exists by name.

```javascript
bool BoneExists(String boneName)
```

**Returns:** true if bone exists

**Example:**
```javascript
if (BoneExists("UpperArm_R")) {
    SetBoneRotation("UpperArm_R", aimRotation);
}
```

### SetBoneRotation()

Set bone rotation only (common for look-at and aiming).

```javascript
void SetBoneRotation(String boneName, Quat rotation, double influence = 1.0)
```

**Parameters:**
- `boneName`: Blender bone name
- `rotation`: Target rotation as quaternion
- `influence`: Blend factor (0.0-1.0)

**Example:**
```javascript
// Aim weapon with right arm
Quat aimRot = Quat.FromEulerAngles(aimPitch, aimYaw, 0);
SetBoneRotation("UpperArm_R", aimRot, 1.0);
SetBoneRotation("LowerArm_R", aimRot, 0.5);
```

### SetBonePosition()

Set bone position only (common for IK targets).

```javascript
void SetBonePosition(String boneName, Vector3 position, double influence = 1.0)
```

**Example:**
```javascript
// Move hand to target position
Vector3 targetPos = GetTarget().pos;
SetBonePosition("Hand_R", targetPos, 1.0);
```

### SetBoneScale()

Set bone scale (for dynamic size changes).

```javascript
void SetBoneScale(String boneName, Vector3 scale, double influence = 1.0)
```

**Example:**
```javascript
// Inflate chest when breathing
double breathScale = 1.0 + sin(level.time * 0.05) * 0.1;
SetBoneScale("Spine_02", (1.0, breathScale, breathScale), 1.0);
```

### AddBoneLookAt()

Create look-at constraint for bone (Blender track-to constraint equivalent).

```javascript
void AddBoneLookAt(String boneName, Vector3 targetWorldPos, Vector3 upAxis = (0, 0, 1), double influence = 1.0)
```

**Parameters:**
- `boneName`: Bone to rotate
- `targetWorldPos`: World position to look at
- `upAxis`: Up vector for roll control
- `influence`: Constraint strength

**Example:**
```javascript
// Head tracks player
if (target) {
    AddBoneLookAt("Head", target.pos + (0, 0, 40), (0, 0, 1), 0.8);
}

// Weapon muzzle tracks enemy
if (target) {
    Vector3 aimPos = target.pos + (0, 0, target.height * 0.5);
    AddBoneLookAt("Muzzle", aimPos, (0, 1, 0), 1.0);
}
```

### BlendBonePose()

Blend between two bone poses (useful for Blender action blending).

```javascript
void BlendBonePose(String boneName, Quat rot1, Quat rot2, double blendFactor)
```

**Example:**
```javascript
// Blend between idle and attack pose
Quat idleRot = Quat.FromEulerAngles(0, 0, 0);
Quat attackRot = Quat.FromEulerAngles(45, 0, 20);
BlendBonePose("UpperArm_R", idleRot, attackRot, attackProgress);
```

### GetBoneLocalTransform()

Get bone local transform (for Blender edit bone data).

```javascript
bool GetBoneLocalTransform(String boneName, out Vector3 position, out Quat rotation, out Vector3 scale)
```

**Returns:** true on success

**Example:**
```javascript
Vector3 pos, scale;
Quat rot;
if (GetBoneLocalTransform("Hand_R", pos, rot, scale)) {
    Console.Printf("Hand position: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
}
```

### TransformBoneHierarchy()

Hierarchical bone transformation (affects children).

```javascript
void TransformBoneHierarchy(String rootBone, Vector3 posOffset, Quat rotOffset, bool recursive = true)
```

**Example:**
```javascript
// Rotate entire arm chain
Quat armRot = Quat.FromEulerAngles(0, 45, 0);
TransformBoneHierarchy("Shoulder_R", (0, 0, 0), armRot, true);
```

### AddMultipleBoneOverrides()

Batch bone overrides for efficiency (Blender IK-friendly).

```javascript
void AddMultipleBoneOverrides(Array<String> boneNames, Array<Vector3> positions, Array<Quat> rotations, double influence = 1.0)
```

**Example:**
```javascript
Array<String> bones;
Array<Vector3> positions;
Array<Quat> rotations;

bones.Push("Spine_01");
bones.Push("Spine_02");
bones.Push("Spine_03");

// Calculate positions and rotations for spine curve
for (int i = 0; i < 3; i++) {
    double bendAngle = 10.0 * i;
    positions.Push((0, 0, 0));
    rotations.Push(Quat.FromEulerAngles(bendAngle, 0, 0));
}

AddMultipleBoneOverrides(bones, positions, rotations, 1.0);
```

### AnimateBoneWithCurve()

Apply procedural animation curve to bone (Blender F-Curve equivalent).

```javascript
void AnimateBoneWithCurve(String boneName, double time, Array<double> keyTimes, Array<Quat> keyRotations)
```

**Example:**
```javascript
// Create custom rotation curve
Array<double> times;
Array<Quat> rotations;

times.Push(0.0);
rotations.Push(Quat.FromEulerAngles(0, 0, 0));

times.Push(0.5);
rotations.Push(Quat.FromEulerAngles(45, 0, 0));

times.Push(1.0);
rotations.Push(Quat.FromEulerAngles(0, 0, 0));

// Animate based on time
double animTime = fmod(level.time * 0.1, 1.0);
AnimateBoneWithCurve("Tail", animTime, times, rotations);
```

### CopyBoneTransform()

Copy bone transform from another actor (useful for ragdolls/attachments).

```javascript
void CopyBoneTransform(String sourceBone, Actor sourceActor, String destBone, double influence = 1.0)
```

**Example:**
```javascript
// Mirror animation from master actor
CopyBoneTransform("Hand_R", masterActor, "Hand_R", 1.0);
```

---

## Advanced Animation Blending

⭐ **NEW in v2.0**: NLA-style animation control for complex animation systems.

### BlendAnimations()

Blend between two animations at runtime.

```javascript
void BlendAnimations(String anim1, String anim2, double blendFactor, bool loop = true)
```

**Parameters:**
- `anim1`: First animation name
- `anim2`: Second animation name
- `blendFactor`: Blend amount (0.0 = anim1, 1.0 = anim2)
- `loop`: Loop the blended result

**Example:**
```javascript
// Blend from walk to run based on speed
double speedFactor = vel.Length() / maxRunSpeed;
BlendAnimations("Walk", "Run", speedFactor, true);
```

### QueueAnimation()

Queue animation to play after current finishes (Blender action strips).

```javascript
void QueueAnimation(String animName, bool loop = true, double blendTime = 0.2)
```

**Example:**
```javascript
// Chain attack animations
PlayAnimation("Attack_Windup", loop: false);
QueueAnimation("Attack_Strike", loop: false, blendTime: 0.1);
QueueAnimation("Attack_Recovery", loop: false, blendTime: 0.15);
```

### GetAnimationTime()

Get animation time (current playback position in seconds).

```javascript
double GetAnimationTime()
```

**Returns:** Current animation time

**Example:**
```javascript
double animTime = GetAnimationTime();
if (animTime > 0.5 && animTime < 0.6) {
    // Trigger effect at specific point in animation
    SpawnEffect();
}
```

### SetAnimationTime()

Set animation time (scrub timeline, Blender-style).

```javascript
void SetAnimationTime(double time)
```

**Example:**
```javascript
// Jump to middle of animation
SetAnimationTime(animState.duration * 0.5);

// Sync multiple actors
for (let actor : syncedActors) {
    actor.SetAnimationTime(masterTime);
}
```

### IsAnimationNearEnd()

Check if animation is near end (for transition logic).

```javascript
bool IsAnimationNearEnd(double threshold = 0.1)
```

**Returns:** true if within threshold seconds of end

**Example:**
```javascript
if (IsAnimationNearEnd(0.2)) {
    // Prepare next animation
    QueueAnimation("Idle", loop: true);
}
```

---

## Attachment System

Attach actors to model bones (e.g., muzzle flash, effects).

### AttachActorToBone()

Attach actor to follow bone movement.

```javascript
void AttachActorToBone(Actor actor, String boneName, Vector3 offset = (0,0,0))
```

**Parameters:**
- `actor`: Actor to attach
- `boneName`: Bone to attach to
- `offset`: Local offset from bone origin

**Examples:**
```javascript
// Muzzle flash on weapon
Actor flash = Spawn("MuzzleFlash", pos);
AttachActorToBone(flash, "Muzzle", (0, 8, 0));

// Particle effect on hand
Actor particles = Spawn("MagicParticles", pos);
AttachActorToBone(particles, "Hand_L", (0, 0, 2));

// Hat on character
Actor hat = Spawn("FancyHat", pos);
AttachActorToBone(hat, "Head", (0, 0, 8));
```

**Common bones:**
- Weapons: `"Muzzle"`, `"Barrel"`, `"Magazine"`, `"Eject"`
- Characters: `"Head"`, `"Hand_L"`, `"Hand_R"`, `"Foot_L"`, `"Foot_R"`
- Monsters: `"Mouth"`, `"Eyes"`, `"Tail_Tip"`

### DetachActor()

Detach actor from bone.

```javascript
void DetachActor(Actor actor)
```

**Example:**
```javascript
DetachActor(flash);  // Detach muzzle flash
flash.Destroy();     // Clean up
```

---

## PBR Material Control

Control Physically Based Rendering properties at runtime.

### SetPBREnabled()

Enable/disable PBR rendering.

```javascript
void SetPBREnabled(bool enable)
```

**Example:**
```javascript
SetPBREnabled(true);   // Use PBR materials
SetPBREnabled(false);  // Use standard materials
```

### SetMetallicFactor()

Set how metallic the surface is.

```javascript
void SetMetallicFactor(double metallic)
```

**Parameters:**
- `metallic`: 0.0-1.0 (0.0 = dielectric, 1.0 = pure metal)

**Examples:**
```javascript
SetMetallicFactor(0.0);   // Plastic, cloth, skin
SetMetallicFactor(0.3);   // Worn metal, alloy
SetMetallicFactor(0.9);   // Polished metal, chrome
SetMetallicFactor(1.0);   // Perfect metal mirror
```

### SetRoughnessFactor()

Set surface roughness/smoothness.

```javascript
void SetRoughnessFactor(double roughness)
```

**Parameters:**
- `roughness`: 0.0-1.0 (0.0 = mirror smooth, 1.0 = rough/matte)

**Examples:**
```javascript
SetRoughnessFactor(0.0);   // Mirror, glass
SetRoughnessFactor(0.2);   // Polished metal
SetRoughnessFactor(0.5);   // Satin, worn metal
SetRoughnessFactor(0.9);   // Rough stone, concrete
SetRoughnessFactor(1.0);   // Clay, chalk, matte
```

### SetEmissive()

Set emissive (glowing) properties.

```javascript
void SetEmissive(Color color, double strength = 1.0)
```

**Parameters:**
- `color`: Emissive color
- `strength`: Emission intensity multiplier

**Examples:**
```javascript
// Red glow (danger)
SetEmissive(Color(255, 255, 0, 0), 1.5);

// Blue energy
SetEmissive(Color(255, 50, 150, 255), 2.0);

// Pulsing effect
double pulse = abs(sin(level.time * 0.1));
SetEmissive(Color(255, 255, 100, 0), pulse * 3.0);

// Turn off glow
SetEmissive(Color(0, 0, 0, 0), 0.0);
```

### Blender-Friendly PBR Presets

⭐ **NEW in v2.0**: Quick PBR setup matching common Blender Principled BSDF configurations.

#### SetupPBRMetal()

Configure for metallic surfaces.

```javascript
void SetupPBRMetal(double metallic = 0.9, double roughness = 0.3)
```

**Example:**
```javascript
// Polished steel weapon
SetupPBRMetal(0.9, 0.2);

// Worn brass
SetupPBRMetal(0.8, 0.5);

// Chrome finish
SetupPBRMetal(1.0, 0.1);
```

#### SetupPBRPlastic()

Configure for plastic/dielectric surfaces.

```javascript
void SetupPBRPlastic(double roughness = 0.6)
```

**Example:**
```javascript
// Glossy plastic armor
SetupPBRPlastic(0.3);

// Matte plastic casing
SetupPBRPlastic(0.8);
```

#### SetupPBRStone()

Configure for rough stone/concrete surfaces.

```javascript
void SetupPBRStone(double roughness = 0.9)
```

**Example:**
```javascript
// Rough concrete
SetupPBRStone(0.95);

// Polished marble
SetupPBRStone(0.3);
```

#### PulseEmissive()

Animated pulsing emissive effect.

```javascript
void PulseEmissive(Color baseColor, double pulseSpeed = 1.0, double minStrength = 0.0, double maxStrength = 2.0)
```

**Example:**
```javascript
// Pulsing red danger light
PulseEmissive(Color(255, 255, 0, 0), 1.0, 0.0, 3.0);

// Slow breathing glow
PulseEmissive(Color(255, 100, 150, 255), 0.3, 0.5, 1.5);

// Fast warning beacon
PulseEmissive(Color(255, 255, 200, 0), 2.5, 0.0, 5.0);
```

---

## Quaternion Math Helpers

⭐ **NEW in v2.0**: Complete quaternion mathematics for smooth bone rotations, compatible with Blender's rotation system.

### Quat Struct

Quaternion representation for 3D rotations.

```javascript
struct Quat
{
    double x, y, z, w;
}
```

### Quat.Identity()

Create identity quaternion (no rotation).

```javascript
static Quat Quat.Identity()
```

**Example:**
```javascript
Quat noRotation = Quat.Identity();
```

### Quat.FromEulerAngles()

Create quaternion from Euler angles (pitch, yaw, roll).

```javascript
static Quat Quat.FromEulerAngles(double pitch, double yaw, double roll)
```

**Example:**
```javascript
// 45 degree pitch
Quat rot = Quat.FromEulerAngles(45, 0, 0);

// Combined rotation
Quat complexRot = Quat.FromEulerAngles(30, 45, 15);
```

### Quat.ToEulerAngles()

Convert quaternion to Euler angles.

```javascript
Vector3 ToEulerAngles()
```

**Returns:** Vector3 with (pitch, yaw, roll)

**Example:**
```javascript
Quat rot = GetBoneWorldRotation("Head");
Vector3 euler = rot.ToEulerAngles();
Console.Printf("Head rotation: P=%.1f, Y=%.1f, R=%.1f", euler.x, euler.y, euler.z);
```

### SlerpQuat()

Spherical linear interpolation between quaternions (smooth rotation blending).

```javascript
Quat SlerpQuat(Quat a, Quat b, double t)
```

**Parameters:**
- `a`: Start rotation
- `b`: End rotation
- `t`: Interpolation factor (0.0-1.0)

**Example:**
```javascript
Quat start = Quat.FromEulerAngles(0, 0, 0);
Quat end = Quat.FromEulerAngles(90, 0, 0);

// Smooth interpolation at 50%
Quat middle = SlerpQuat(start, end, 0.5);
SetBoneRotation("Arm", middle);

// Animation curve
double t = fmod(level.time * 0.1, 1.0);
Quat animated = SlerpQuat(start, end, t);
```

### QuatLookRotation()

Create look-at rotation (Blender track-to constraint).

```javascript
Quat QuatLookRotation(Vector3 forward, Vector3 up)
```

**Parameters:**
- `forward`: Direction to look
- `up`: Up vector for roll control

**Example:**
```javascript
// Look at target
Vector3 direction = target.pos - pos;
Quat lookRot = QuatLookRotation(direction, (0, 0, 1));
SetBoneRotation("Head", lookRot);

// Look along velocity
if (vel.Length() > 0) {
    Quat moveRot = QuatLookRotation(vel, (0, 0, 1));
    SetBoneRotation("Spine", moveRot, 0.5);
}
```

### CrossProduct()

Vector cross product helper.

```javascript
Vector3 CrossProduct(Vector3 a, Vector3 b)
```

**Example:**
```javascript
Vector3 forward = (1, 0, 0);
Vector3 up = (0, 0, 1);
Vector3 right = CrossProduct(forward, up);
```

### Quat Methods

Additional quaternion operations:

```javascript
// Quaternion multiplication
Quat Multiply(Quat other)

// Dot product
double Dot(Quat other)

// Length/magnitude
double Length()

// Normalize
Quat Normalize()

// Rotate vector by quaternion
Vector3 RotateVector(Vector3 v)
```

**Example:**
```javascript
// Combine rotations
Quat rot1 = Quat.FromEulerAngles(45, 0, 0);
Quat rot2 = Quat.FromEulerAngles(0, 30, 0);
Quat combined = rot1.Multiply(rot2);

// Apply rotation to vector
Vector3 point = (10, 0, 0);
Vector3 rotated = combined.RotateVector(point);
```

---

## Update System

### UpdateGLTFModel()

Update model state - **MUST** call every tic.

```javascript
void UpdateGLTFModel()
```

**Call in every state:**
```javascript
States
{
Spawn:
    TNT1 A 1
    {
        UpdateGLTFModel();  // Required!
    }
    Loop;
}
```

**What it does:**
- Advances animation time
- Updates bone transforms
- Updates attached actors
- Syncs with renderer

---

## Complete Examples

### Example 1: Animated Powerup

```javascript
class GLTFInvulnerability : InvulnerabilitySphere
{
    mixin GLTFModel;

    double rotationPhase;
    double bobPhase;

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            InitGLTFModel("models/items/invulnerability/invuln.gltf");
            SetModelScaleUniform(8.0);

            // Floating effect
            PlayAnimation("Float", loop: true);

            // PBR settings for magical artifact
            SetPBREnabled(true);
            SetMetallicFactor(0.8);
            SetRoughnessFactor(0.2);

            rotationPhase = 0;
            bobPhase = 0;
        }
        TNT1 A 1
        {
            // Procedural effects
            rotationPhase += 2;
            bobPhase += 0.1;

            // Rotate around vertical axis
            SetModelRotation(0, rotationPhase, 0);

            // Bob up and down
            double bobOffset = sin(bobPhase) * 4;
            SetModelOffset(0, 0, bobOffset);

            // Pulsing glow
            double pulse = (sin(level.time * 0.05) + 1.0) * 0.5;
            SetEmissive(Color(255, 100, 255, 100), pulse * 3.0);

            UpdateGLTFModel();
        }
        Loop;
    }
}
```

### Example 2: Weapon with Recoil

```javascript
class GLTFShotgun : Shotgun
{
    mixin GLTFModel;

    double recoilAmount;

    States
    {
    Ready:
        TNT1 A 0
        {
            if (!HasGLTFModel())
            {
                InitGLTFModel("models/weapons/shotgun/shotgun.gltf");
                SetModelScaleUniform(2.8);
                SetModelOffset(0, 18, 26);
                PlayAnimation("Idle", loop: true);
            }

            recoilAmount = 0;
        }
        TNT1 A 1
        {
            A_WeaponReady();

            // Smooth recoil recovery
            if (recoilAmount > 0)
            {
                recoilAmount *= 0.8;  // Decay
                Quat recoil = Quat.FromEulerAngles(-recoilAmount, 0, 0);
                AddBoneOverride("Weapon_Root", (0, 0, 0), recoil, 1.0);
            }

            UpdateGLTFModel();
        }
        Loop;

    Fire:
        TNT1 A 0
        {
            PlayAnimation("Fire", loop: false, blendTime: 0.05);
            recoilAmount = 25.0;  // Strong recoil

            // Muzzle flash
            Actor flash = Spawn("GLTFMuzzleFlash", pos);
            AttachActorToBone(flash, "Muzzle", (0, 12, 0));
        }
        TNT1 A 3
        {
            A_FireBullets(5.6, 5.6, 7, 5);
            A_PlaySound("weapons/shotgun", CHAN_WEAPON);
            UpdateGLTFModel();
        }
        TNT1 A 7
        {
            UpdateGLTFModel();
        }
        TNT1 A 0
        {
            PlayAnimation("Idle", loop: true, blendTime: 0.3);
        }
        TNT1 A 5 A_ReFire();
        Goto Ready;
    }
}
```

### Example 3: Boss with Phase Transitions

```javascript
class GLTFBossDemon : Actor
{
    mixin GLTFModel;

    int phase;
    double angerLevel;

    Default
    {
        Health 5000;
        Radius 64;
        Height 128;
        Mass 2000;
        Speed 12;
        Monster;
        +BOSS
        +DONTMORPH
        +NOICEDEATH
    }

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            InitGLTFModel("models/monsters/bossdemon/boss.gltf");
            SetModelScaleUniform(2.0);

            SetPBREnabled(true);
            SetMetallicFactor(0.3);
            SetRoughnessFactor(0.7);

            phase = 1;
            angerLevel = 0.0;

            PlayAnimation("Idle", loop: true);
        }
        TNT1 A 10
        {
            A_Look();

            // Breathing effect
            double breathe = sin(level.time * 0.03) * 2.0;
            Vector3 chestMove = (0, 0, breathe);
            AddBoneOverride("Chest", chestMove, Quat.Identity(), 0.3);

            UpdateGLTFModel();
        }
        Loop;

    See:
        TNT1 A 0
        {
            // Check health for phase transitions
            if (health < 3000 && phase == 1)
            {
                phase = 2;
                SetState(FindState("PhaseTransition2"));
                return;
            }
            if (health < 1500 && phase == 2)
            {
                phase = 3;
                SetState(FindState("PhaseTransition3"));
                return;
            }

            PlayAnimation("Walk", loop: true);
        }
        TNT1 A 2
        {
            A_Chase();
            UpdateGLTFModel();
        }
        Loop;

    PhaseTransition2:
        TNT1 A 0
        {
            PlayAnimation("Enrage", loop: false);

            // Visual transformation
            SetEmissive(Color(255, 255, 50, 0), 1.0);
            A_PlaySound("boss/enrage", CHAN_VOICE);
        }
        TNT1 A 60
        {
            // Intensify glow during transition
            angerLevel += 1.0 / 60.0;
            SetEmissive(Color(255, 255, 50, 0), angerLevel * 3.0);

            UpdateGLTFModel();
        }
        TNT1 A 0
        {
            // Phase 2: Faster, stronger
            Speed = 16;
            PlayAnimation("Walk", loop: true);
            SetAnimationSpeed(1.5);
        }
        Goto See;

    PhaseTransition3:
        TNT1 A 0
        {
            PlayAnimation("Berserk", loop: false);

            // Full rage mode
            SetEmissive(Color(255, 255, 0, 0), 5.0);
            SetMetallicFactor(0.9);  // Burning metal appearance
            A_PlaySound("boss/berserk", CHAN_VOICE);
        }
        TNT1 A 90
        {
            // Screen shake, particles, etc.
            UpdateGLTFModel();
        }
        TNT1 A 0
        {
            // Phase 3: Maximum power
            Speed = 20;
            PlayAnimation("Walk", loop: true);
            SetAnimationSpeed(2.0);
        }
        Goto See;

    Melee:
        TNT1 A 0
        {
            A_FaceTarget();

            // Different attacks per phase
            if (phase == 1)
                PlayAnimation("Attack1", loop: false);
            else if (phase == 2)
                PlayAnimation("Attack2", loop: false);
            else
                PlayAnimation("Attack3", loop: false);
        }
        TNT1 A 12
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_CustomMeleeAttack(20 * random(1, 8) * phase);
        TNT1 A 12
        {
            UpdateGLTFModel();
        }
        Goto See;

    Death:
        TNT1 A 0
        {
            PlayAnimation("Death", loop: false);

            // Fade out glow
            for (int i = 0; i < 60; i++)
            {
                double fade = 1.0 - (i / 60.0);
                // Gradual fade handled in state loop
            }
        }
        TNT1 A 10
        {
            A_Scream();
            UpdateGLTFModel();
        }
        TNT1 A 10
        {
            A_NoBlocking();
            UpdateGLTFModel();
        }
        TNT1 A 40
        {
            // Fade emissive
            double fade = GetAnimationProgress();
            SetEmissive(Color(255, 255, 0, 0), (1.0 - fade) * 5.0);

            UpdateGLTFModel();
        }
        TNT1 A -1;
        Stop;
    }
}
```

---

## Best Practices

### Performance

1. **Call UpdateGLTFModel() once per tic**
   ```javascript
   // Good
   TNT1 A 1 { UpdateGLTFModel(); }

   // Bad - multiple updates waste CPU
   TNT1 A 1 { UpdateGLTFModel(); UpdateGLTFModel(); }
   ```

2. **Reuse bone overrides instead of creating new ones**
   ```javascript
   // Good - update existing override
   AddBoneOverride("Head", newPos, newRot, 1.0);

   // Bad - don't clear and re-add every frame
   ClearBoneOverrides();
   AddBoneOverride("Head", newPos, newRot, 1.0);
   ```

3. **Clean up attachments**
   ```javascript
   // Always detach before destroying
   DetachActor(flash);
   flash.Destroy();
   ```

### Animation

1. **Use appropriate blend times**
   - Combat: 0.05 - 0.15 seconds (snappy)
   - Movement: 0.2 - 0.3 seconds (smooth)
   - Cinematics: 0.4 - 0.6 seconds (dramatic)

2. **Match animation speed to actor speed**
   ```javascript
   double walkSpeed = vel.Length() / 8.0;  // Normalize
   SetAnimationSpeed(walkSpeed);
   ```

3. **Don't restart same animation**
   ```javascript
   if (GetCurrentAnimation() != "Idle")
   {
       PlayAnimation("Idle", loop: true);
   }
   ```

### Scaling

1. **Test scale values iteratively**
   ```javascript
   // Start with 1.0, adjust up or down
   SetModelScaleUniform(1.0);
   // Too small? Try 1.5
   // Too big? Try 0.7
   ```

2. **Common scale ranges by type:**
   - Pickups/items: 4.0 - 8.0
   - Weapons: 2.0 - 3.5
   - Monsters: 0.8 - 1.5
   - Players: 0.9 - 1.1
   - Props: 0.5 - 3.0

### Materials

1. **Set PBR once on spawn**
   ```javascript
   TNT1 A 0 NoDelay
   {
       InitGLTFModel("...");
       SetPBREnabled(true);
       SetMetallicFactor(0.8);
       SetRoughnessFactor(0.3);
   }
   ```

2. **Use emissive for dynamic effects**
   ```javascript
   // Damage indicator
   double damageGlow = (1.0 - (health / default.health)) * 2.0;
   SetEmissive(Color(255, 255, 0, 0), damageGlow);
   ```

---

## Troubleshooting

### Model doesn't appear

**Problem:** Actor spawns but no model visible.

**Solutions:**
1. Check model path is correct
2. Ensure `InitGLTFModel()` is called with `NoDelay`
3. Verify model file exists in PK3
4. Check console for error messages

```javascript
// Debug: Print model path
TNT1 A 0 NoDelay
{
    InitGLTFModel("models/test.gltf");
    Console.Printf("Loaded: %s", modelDef.modelPath);
}
```

### Animation doesn't play

**Problem:** Model appears but doesn't animate.

**Solutions:**
1. Check animation name matches Blender Action exactly (case-sensitive)
2. Ensure `UpdateGLTFModel()` is called every tic
3. Verify `PlayAnimation()` is called after `InitGLTFModel()`

```javascript
// Debug: Check animation state
Console.Printf("Current: %s, Playing: %d",
    GetCurrentAnimation(), IsAnimationPlaying());
```

### Model is wrong size

**Problem:** Model too big or too small.

**Solutions:**
1. Adjust scale incrementally
2. Check Blender model scale was applied before export
3. Use uniform scale first, then adjust axes if needed

```javascript
// Try different scales
SetModelScaleUniform(0.5);  // Half size
SetModelScaleUniform(1.0);  // Original
SetModelScaleUniform(2.0);  // Double size
```

### Performance issues

**Problem:** Game lags with glTF models.

**Solutions:**
1. Reduce polygon count in Blender
2. Use smaller textures (1024x1024 instead of 4096x4096)
3. Limit bone overrides to essential bones only
4. Don't call `UpdateGLTFModel()` multiple times per tic

### Bones don't respond to overrides

**Problem:** `AddBoneOverride()` has no effect.

**Solutions:**
1. Check bone name exactly matches Blender bone name
2. Ensure influence is not 0.0
3. Verify bone exists in model (check Blender)

```javascript
// List all bones (if API available)
for (int i = 0; i < model.GetBoneCount(); i++)
{
    Console.Printf("Bone %d: %s", i, model.GetBoneName(i));
}
```

### Attached actors don't follow bone

**Problem:** `AttachActorToBone()` doesn't work.

**Solutions:**
1. Ensure `UpdateGLTFModel()` is called (updates attachments)
2. Check bone name is correct
3. Verify actor is not destroyed prematurely

```javascript
// Keep reference and check validity
if (attachedFlash && !attachedFlash.bDestroyed)
{
    // Actor still exists
}
```

---

## Additional Resources

- [Blender glTF Modeling Guide](BLENDER_GLTF_MODELING_GUIDE.md)
- [glTF Beginner Tutorial](GLTF_BEGINNER_TUTORIAL.md)
- [glTF Implementation Status](GLTF_IMPLEMENTATION.md)
- [NeoDoom Development Guide](CLAUDE.md)

---

**Happy modding with glTF!** 🎮
