/*
** gltf_examples.zs
**
** Example actor implementations using glTF models
** These demonstrate best practices for different asset types
**
**---------------------------------------------------------------------------
**
** Copyright 2025 BiasedDoom Contributors
** All rights reserved.
**
*/

// ============================================================================
// EXAMPLE 1: Simple Item with Rotation Animation
// ============================================================================

class GLTFHealthPack : HealthBonus
{
    mixin GLTFModel;

    Default
    {
        +COUNTITEM
        +INVENTORY.ALWAYSPICKUP
        Inventory.Amount 1;
        Inventory.MaxAmount 200;
        Inventory.PickupMessage "Picked up a health pack.";
        Scale 0.15; // Adjust to match DOOM scale
    }

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            // Initialize glTF model
            InitGLTFModel("models/items/healthpack/healthpack.gltf");

            // Configure model appearance
            SetModelScaleUniform(6.0); // Scale to appropriate DOOM size
            SetModelOffset(0, 0, 4);   // Slight vertical offset

            // Start floating animation
            PlayAnimation("Float", loop: true);
            SetAnimationSpeed(1.0);
        }
        TNT1 A 1
        {
            UpdateGLTFModel(); // Update model state each tic
        }
        Wait;
    }
}

// ============================================================================
// EXAMPLE 2: Weapon with Multiple Animations
// ============================================================================

class GLTFPistol : Pistol
{
    mixin GLTFModel;

    Default
    {
        Weapon.SelectionOrder 1900;
        Weapon.AmmoUse 1;
        Weapon.AmmoGive 20;
        Weapon.AmmoType "Clip";
        Inventory.Pickupmessage "Picked up a modern pistol.";
        Tag "Modern Pistol";
    }

    States
    {
    Ready:
        TNT1 A 0
        {
            // Initialize weapon model if not already done
            if (!HasGLTFModel())
            {
                InitGLTFModel("models/weapons/pistol/pistol.gltf");
                SetModelScaleUniform(2.5);
                SetModelOffset(0, 15, 28); // Position in front of player view
            }

            // Play idle animation
            PlayAnimation("Idle", loop: true, blendTime: 0.15);
        }
        TNT1 A 1
        {
            A_WeaponReady();
            UpdateGLTFModel();
        }
        Loop;

    Deselect:
        TNT1 A 0
        {
            PlayAnimation("Deselect", loop: false, blendTime: 0.1);
        }
        TNT1 A 1
        {
            A_Lower();
            UpdateGLTFModel();
        }
        Loop;

    Select:
        TNT1 A 0
        {
            PlayAnimation("Select", loop: false, blendTime: 0.1);
        }
        TNT1 A 1
        {
            A_Raise();
            UpdateGLTFModel();
        }
        Loop;

    Fire:
        TNT1 A 0
        {
            PlayAnimation("Fire", loop: false, blendTime: 0.05);
        }
        TNT1 A 4
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_FireBullets(5.6, 0, 1, 5);
        TNT1 A 0 A_PlaySound("weapons/pistol", CHAN_WEAPON);
        TNT1 A 6
        {
            UpdateGLTFModel();
        }
        TNT1 A 0
        {
            // Return to idle
            PlayAnimation("Idle", loop: true, blendTime: 0.2);
        }
        TNT1 A 5
        {
            A_ReFire();
            UpdateGLTFModel();
        }
        Goto Ready;
    }
}

// ============================================================================
// EXAMPLE 3: Monster with Full Animation Set
// ============================================================================

class GLTFDemon : Demon
{
    mixin GLTFModel;

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
        Tag "Modern Demon";
    }

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            // Initialize model
            InitGLTFModel("models/monsters/demon/demon.gltf");
            SetModelScaleUniform(1.0);
            SetModelOffset(0, 0, 0);

            // Enable PBR for realistic materials
            SetPBREnabled(true);

            // Start idle animation
            PlayAnimation("Idle", loop: true);
        }
        TNT1 A 10
        {
            A_Look();
            UpdateGLTFModel();
        }
        Loop;

    See:
        TNT1 A 0
        {
            PlayAnimation("Walk", loop: true, blendTime: 0.3);
            SetAnimationSpeed(1.2); // Slightly faster for movement
        }
        TNT1 A 2
        {
            A_Chase();
            UpdateGLTFModel();
        }
        Loop;

    Melee:
        TNT1 A 0
        {
            A_FaceTarget();
            PlayAnimation("Attack", loop: false, blendTime: 0.1);
        }
        TNT1 A 8
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_CustomMeleeAttack(10 * random(1, 8));
        TNT1 A 8
        {
            UpdateGLTFModel();
        }
        Goto See;

    Pain:
        TNT1 A 0
        {
            PlayAnimation("Pain", loop: false, blendTime: 0.05);
        }
        TNT1 A 2
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_Pain();
        TNT1 A 8
        {
            UpdateGLTFModel();
        }
        Goto See;

    Death:
        TNT1 A 0
        {
            // Randomly choose death animation
            if (random(0, 1))
                PlayAnimation("Death1", loop: false, blendTime: 0.1);
            else
                PlayAnimation("Death2", loop: false, blendTime: 0.1);
        }
        TNT1 A 8
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_Scream();
        TNT1 A 8
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_NoBlocking();
        TNT1 A 24
        {
            UpdateGLTFModel();
        }
        TNT1 A -1
        {
            UpdateGLTFModel();
        }
        Stop;
    }
}

// ============================================================================
// EXAMPLE 4: Player Model with First and Third Person
// ============================================================================

class GLTFMarinePlayer : PlayerPawn
{
    mixin GLTFModel;

    // Separate model for first-person arms
    GLTFModelDef fpArmsModel;
    GLTFAnimationState fpAnimState;

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
        Player.StartItem "Pistol";
        Player.StartItem "Fist";
        Player.StartItem "Clip", 50;
        Player.ColorRange 112, 127;
    }

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            // Initialize third-person body model
            InitGLTFModel("models/players/marine/marine_body.gltf");
            SetModelScaleUniform(1.0);

            // Initialize first-person arms (stored separately)
            fpArmsModel = GLTFModelDef.Create("models/players/marine/marine_arms.gltf");
            fpArmsModel.scale = (1.0, 1.0, 1.0);
            fpArmsModel.offset = (0, 10, 20);
            fpAnimState = GLTFAnimationState.Create();

            PlayAnimation("Idle", loop: true);
        }
        TNT1 A 1
        {
            UpdateGLTFModel();
        }
        Loop;

    See:
        TNT1 A 0
        {
            // Switch between walk/run based on speed
            if (vel.Length() > 8)
                PlayAnimation("Run", loop: true, blendTime: 0.2);
            else
                PlayAnimation("Walk", loop: true, blendTime: 0.2);
        }
        TNT1 A 1
        {
            UpdateGLTFModel();
        }
        Loop;

    Pain:
        TNT1 A 0
        {
            PlayAnimation("Pain", loop: false, blendTime: 0.05);
        }
        TNT1 A 4
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_Pain();
        Goto Spawn;

    Death:
        TNT1 A 0
        {
            PlayAnimation("Death1", loop: false);
        }
        TNT1 A 10
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_PlayerScream();
        TNT1 A 10
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_NoBlocking();
        TNT1 A 30
        {
            UpdateGLTFModel();
        }
        TNT1 A -1
        {
            UpdateGLTFModel();
        }
        Stop;
    }
}

// ============================================================================
// EXAMPLE 5: Procedural Animation with Bone Overrides
// ============================================================================

class GLTFFloatingSkull : Lost_Soul
{
    mixin GLTFModel;

    double bobPhase;
    double wobblePhase;

    Default
    {
        Health 100;
        Radius 16;
        Height 56;
        Mass 50;
        Speed 8;
        Damage 3;
        PainChance 256;
        Monster;
        +FLOAT
        +NOGRAVITY
        +MISSILEMORE
        +DONTFALL
        +NOICEDEATH
        SeeSound "skull/active";
        AttackSound "skull/melee";
        PainSound "skull/pain";
        DeathSound "skull/death";
        ActiveSound "skull/active";
    }

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            InitGLTFModel("models/monsters/skull/skull.gltf");
            SetModelScaleUniform(0.8);

            PlayAnimation("Idle", loop: true);
            bobPhase = 0;
            wobblePhase = 0;
        }
        TNT1 A 1
        {
            // Procedural floating motion using bone overrides
            bobPhase += 0.05;
            wobblePhase += 0.03;

            double bobOffset = sin(bobPhase) * 4.0;
            double wobbleRot = sin(wobblePhase) * 10.0;

            // Override jaw bone for breathing effect
            Quat jawRot = Quat.FromEulerAngles(sin(bobPhase * 2) * 5, 0, 0);
            AddBoneOverride("Jaw", (0, 0, 0), jawRot, 1.0);

            // Add glow effect
            SetEmissive(Color(255, 200, 100, 100), 1.5);

            A_Look();
            UpdateGLTFModel();
        }
        Loop;

    See:
        TNT1 A 0
        {
            PlayAnimation("Fly", loop: true);
        }
        TNT1 A 2
        {
            A_Chase();
            UpdateGLTFModel();

            // Continue procedural animation
            bobPhase += 0.08;
            double tilt = sin(bobPhase) * 15.0;
            Quat tiltRot = Quat.FromEulerAngles(0, 0, tilt);
            AddBoneOverride("Root", (0, 0, 0), tiltRot, 0.5);
        }
        Loop;
    }
}

// ============================================================================
// EXAMPLE 6: Attachment Points (Weapon with Muzzle Flash)
// ============================================================================

class GLTFRifle : Weapon
{
    mixin GLTFModel;

    Actor muzzleFlash;

    Default
    {
        Weapon.SelectionOrder 700;
        Weapon.AmmoUse 1;
        Weapon.AmmoGive 30;
        Weapon.AmmoType "Clip";
        Inventory.PickupMessage "Picked up an assault rifle.";
    }

    States
    {
    Ready:
        TNT1 A 0
        {
            if (!HasGLTFModel())
            {
                InitGLTFModel("models/weapons/rifle/rifle.gltf");
                SetModelScaleUniform(2.0);
                SetModelOffset(0, 18, 30);
            }
            PlayAnimation("Idle", loop: true);
        }
        TNT1 A 1
        {
            A_WeaponReady();
            UpdateGLTFModel();
        }
        Loop;

    Fire:
        TNT1 A 0
        {
            PlayAnimation("Fire", loop: false);

            // Create muzzle flash and attach to weapon
            if (!muzzleFlash)
            {
                muzzleFlash = Spawn("GLTFMuzzleFlash", pos);
                AttachActorToBone(muzzleFlash, "Muzzle", (0, 8, 0));
            }
        }
        TNT1 A 2
        {
            A_FireBullets(3.0, 3.0, 1, 10);
            A_PlaySound("weapons/rifle", CHAN_WEAPON);
            UpdateGLTFModel();
        }
        TNT1 A 3
        {
            if (muzzleFlash)
            {
                muzzleFlash.Destroy();
                muzzleFlash = null;
            }
            UpdateGLTFModel();
        }
        TNT1 A 0
        {
            PlayAnimation("Idle", loop: true, blendTime: 0.15);
        }
        TNT1 A 3 A_ReFire();
        Goto Ready;
    }
}

// ============================================================================
// EXAMPLE 7: Dynamic Material Properties
// ============================================================================

class GLTFBarrel : ExplosiveBarrel
{
    mixin GLTFModel;

    double heatLevel;

    Default
    {
        Health 20;
        Radius 10;
        Height 42;
        +SOLID
        +SHOOTABLE
        +NOBLOOD
        +ACTIVATEMCROSS
        +DONTGIB
        +NOICEDEATH
        DeathSound "world/barrelx";
    }

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            InitGLTFModel("models/props/barrel/barrel.gltf");
            SetModelScaleUniform(1.2);

            // Configure PBR for metal barrel
            SetPBREnabled(true);
            SetMetallicFactor(0.9);  // Very metallic
            SetRoughnessFactor(0.4); // Slightly worn metal
            SetEmissive(Color(0, 0, 0, 0), 0.0);

            heatLevel = 0;
        }
        TNT1 A 1
        {
            UpdateGLTFModel();

            // Gradually heat up when taking damage
            if (health < 20)
            {
                heatLevel = 1.0 - (health / 20.0);

                // Glow red when damaged
                int redGlow = int(heatLevel * 255);
                SetEmissive(Color(255, redGlow, 0, 0), heatLevel * 2.0);
            }
        }
        Loop;

    Death:
        TNT1 A 0
        {
            // Full glow before explosion
            SetEmissive(Color(255, 255, 100, 0), 5.0);
        }
        TNT1 A 5 Bright
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_Scream();
        TNT1 A 0 A_Explode();
        TNT1 A 10 Bright
        {
            UpdateGLTFModel();
        }
        TNT1 A -1;
        Stop;
    }
}
