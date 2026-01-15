#!/bin/bash
#
# BiasedDoom glTF Model Replacement Generator
#
# Interactive script to create the complete structure for replacing
# Doom sprites with glTF 2.0 models (player, monsters, items, weapons, decorations)
#
# Usage: ./create-gltf-replacement.sh
#

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
NC='\033[0m' # No Color
BOLD='\033[1m'

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Banner
print_banner() {
    echo -e "${CYAN}${BOLD}"
    echo "╔════════════════════════════════════════════════════════════════╗"
    echo "║                                                                ║"
    echo "║           BiasedDoom glTF Model Replacement Generator             ║"
    echo "║                                                                ║"
    echo "║    Create 3D model replacements for Doom sprites (v2.0)       ║"
    echo "║                                                                ║"
    echo "╚════════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"
}

# Print section header
print_section() {
    echo -e "\n${BLUE}${BOLD}━━━ $1 ━━━${NC}"
}

# Print info message
print_info() {
    echo -e "${CYAN}ℹ${NC} $1"
}

# Print success message
print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

# Print warning message
print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

# Print error message
print_error() {
    echo -e "${RED}✗${NC} $1"
}

# Prompt for input with default value
prompt_input() {
    local prompt="$1"
    local default="$2"
    local var_name="$3"
    local input

    if [ -n "$default" ]; then
        echo -e -n "${MAGENTA}?${NC} ${prompt} ${BOLD}[${default}]${NC}: "
    else
        echo -e -n "${MAGENTA}?${NC} ${prompt}: "
    fi

    read -r input
    if [ -z "$input" ] && [ -n "$default" ]; then
        eval "$var_name=\"\$default\""
    else
        eval "$var_name=\"\$input\""
    fi
}

# Show selection menu
show_menu() {
    local title="$1"
    shift
    local options=("$@")

    echo -e "\n${BOLD}$title${NC}"
    for i in "${!options[@]}"; do
        echo -e "  ${GREEN}$((i+1))${NC}) ${options[$i]}"
    done
    echo -e "  ${RED}q${NC}) Quit"
}

# Get menu selection
get_selection() {
    local max=$1
    local selection

    while true; do
        echo -e -n "${MAGENTA}?${NC} ${BOLD}Select option [1-$max]:${NC} "
        read -r selection

        # Check for quit
        if [ "$selection" = "q" ] || [ "$selection" = "Q" ]; then
            echo -e "\n${YELLOW}Cancelled by user${NC}"
            exit 0
        fi

        # Validate numeric input
        if [[ "$selection" =~ ^[0-9]+$ ]]; then
            if [ "$selection" -ge 1 ] && [ "$selection" -le "$max" ]; then
                echo "$selection"
                return 0
            fi
        fi

        print_error "Invalid selection. Please enter a number between 1 and $max (or 'q' to quit)"
    done
}

# Validate class name (ASCII, no spaces)
validate_class_name() {
    local name="$1"
    if [[ ! "$name" =~ ^[A-Za-z][A-Za-z0-9_]*$ ]]; then
        print_error "Invalid class name. Must start with letter, contain only alphanumeric and underscore"
        return 1
    fi
    return 0
}

# Create directory structure
create_directories() {
    local base_path="$1"

    print_info "Creating directory structure..."

    if ! mkdir -p "$base_path/models" "$base_path/textures" "$base_path/zscript" "$base_path/docs" 2>/dev/null; then
        print_error "Failed to create directories. Check permissions."
        return 1
    fi

    print_success "Directories created at: $base_path"
    return 0
}

# Generate ZScript actor replacement
generate_zscript_player() {
    local file="$1"
    local class_name="$2"
    local model_path="$3"
    local description="$4"

    cat > "$file" << 'ZSCRIPT_PLAYER_EOF'
/*
** %FILENAME%
**
** ZScript: %CLASS_NAME% - glTF Player Model Replacement
** %DESCRIPTION%
**
** This replaces the player sprite with a glTF 2.0 model for third-person view.
** For first-person arms, see separate weapon models.
**
**---------------------------------------------------------------------------
*/

class %CLASS_NAME% : DoomPlayer
{
    mixin GLTFModel;

    // Animation state tracking
    private double lastVelocity;
    private bool wasOnGround;
    private bool wasAttacking;

    Default
    {
        Player.DisplayName "%DISPLAY_NAME%";
        Player.ColorRange 112, 127; // Green range (customize as needed)
        Player.StartItem "Pistol";
        Player.StartItem "Fist";
        Player.StartItem "Clip", 50;

        // Player properties
        Health 100;
        Radius 16;
        Height 56;
        Mass 100;
        Speed 1;

        +PICKUP
        +NOTDMATCH // Remove if you want this in deathmatch
    }

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            // Initialize glTF model
            if (!InitGLTFModel("%MODEL_PATH%"))
            {
                Console.Printf("ERROR: Failed to load player model: %MODEL_PATH%");
                return;
            }

            // Configure model appearance
            SetModelScaleUniform(1.0); // Adjust scale to match Doom player size
            SetModelOffset(0, 0, 0);   // Center model at player position

            // Enable PBR for realistic materials
            SetPBREnabled(true);

            // Start with idle animation
            PlayAnimation("Idle", loop: true);

            // Initialize state
            lastVelocity = 0;
            wasOnGround = true;
            wasAttacking = false;

            if (developer >= 1)
                Console.Printf("Player model loaded: %CLASS_NAME%");
        }
        Goto Active;

    Active:
        TNT1 A 1
        {
            // Update model
            UpdateGLTFModel();

            // Determine player state and play appropriate animation
            UpdatePlayerAnimation();

            // Update procedural animations (head tracking, breathing, etc.)
            UpdateProceduralAnimations();
        }
        Loop;

    Death:
        TNT1 A 0
        {
            // Random death animation
            if (random(0, 1))
                PlayAnimation("Death1", loop: false, blendTime: 0.1);
            else
                PlayAnimation("Death2", loop: false, blendTime: 0.1);
        }
        TNT1 A 10
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_PlayerScream;
        TNT1 A 10
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_NoBlocking;
        TNT1 A 30
        {
            UpdateGLTFModel();
        }
        TNT1 A -1
        {
            UpdateGLTFModel();
        }
        Stop;

    XDeath: // Gibbed death
        TNT1 A 0
        {
            PlayAnimation("DeathGib", loop: false, blendTime: 0.0);
        }
        TNT1 A 5 A_PlayerScream;
        TNT1 A 0 A_NoBlocking;
        TNT1 A 5 A_XScream;
        TNT1 A 5
        {
            UpdateGLTFModel();
        }
        TNT1 A -1
        {
            UpdateGLTFModel();
        }
        Stop;
    }

    // Update animation based on player state
    private void UpdatePlayerAnimation()
    {
        if (health <= 0) return; // Dead, animation handled in Death state

        // Get current velocity
        double currentVel = vel.Length();
        bool onGround = (pos.z <= floorz || bONMOBJ);

        // Determine animation based on state
        String desiredAnim = "Idle";
        double animSpeed = 1.0;

        // Jump/Fall
        if (!onGround && wasOnGround)
        {
            desiredAnim = "Jump";
        }
        else if (!onGround)
        {
            if (vel.z < -8)
                desiredAnim = "Fall";
            else if (vel.z > 8)
                desiredAnim = "Jump";
            else
                desiredAnim = "Float";
        }
        // Ground movement
        else if (onGround && currentVel > 0.1)
        {
            if (currentVel > 12) // Running
            {
                desiredAnim = "Run";
                animSpeed = currentVel / 12.0; // Speed up animation with movement
            }
            else if (currentVel > 4) // Walking
            {
                desiredAnim = "Walk";
                animSpeed = currentVel / 8.0;
            }
            else // Slow walk
            {
                desiredAnim = "Walk";
                animSpeed = 0.5;
            }
        }
        else if (onGround && currentVel <= 0.1)
        {
            desiredAnim = "Idle";
        }

        // Override with pain animation if recently hurt
        if (InStateSequence(CurState, ResolveState("Pain")))
        {
            desiredAnim = "Pain";
        }

        // Play animation if different from current
        if (GetCurrentAnimation() != desiredAnim)
        {
            PlayAnimation(desiredAnim, loop: (desiredAnim != "Jump" && desiredAnim != "Pain"), blendTime: 0.2);
        }

        // Update animation speed
        if (animSpeed != 1.0)
        {
            SetAnimationSpeed(animSpeed);
        }

        // Store state for next frame
        lastVelocity = currentVel;
        wasOnGround = onGround;
    }

    // Procedural animations (head tracking, breathing, etc.)
    private void UpdateProceduralAnimations()
    {
        // Head looks at view direction (for third-person)
        if (player && player.camera == self)
        {
            // Calculate look direction from player pitch/angle
            double lookPitch = player.mo.pitch;
            double lookYaw = player.mo.angle;

            // Apply to head bone with limits
            Quat headRot = Quat.FromEulerAngles(
                Clamp(lookPitch, -45, 45),  // Limit head pitch
                Clamp(lookYaw - angle, -60, 60), // Limit head yaw relative to body
                0
            );

            SetBoneRotation("Head", headRot, 0.3); // 30% influence for subtle look
        }

        // Breathing animation (subtle chest movement)
        double breathPhase = level.time * 0.03; // Slow breathing
        double breathScale = 1.0 + sin(breathPhase) * 0.03; // 3% expansion

        if (BoneExists("Spine_02") || BoneExists("Chest"))
        {
            String chestBone = BoneExists("Spine_02") ? "Spine_02" : "Chest";
            SetBoneScale(chestBone, (1.0, breathScale, breathScale), 0.5);
        }

        // Weapon aim (if weapon visible in third person)
        if (player && player.ReadyWeapon && BoneExists("UpperArm_R"))
        {
            // Calculate aim direction
            Vector3 aimDir = (
                cos(angle) * cos(pitch),
                sin(angle) * cos(pitch),
                -sin(pitch)
            );

            // Apply to arm bones
            Quat aimRot = QuatLookRotation(aimDir, (0, 0, 1));
            SetBoneRotation("UpperArm_R", aimRot, 0.5);
            SetBoneRotation("LowerArm_R", aimRot, 0.3);
        }
    }

    // Debug: List available animations
    void ListAvailableAnimations()
    {
        if (!HasGLTFModel()) return;

        Console.Printf("=== Available animations for %s ===", GetClassName());
        ListAnimations();
        Console.Printf("=== Available bones ===");
        ListBones();
    }
}
ZSCRIPT_PLAYER_EOF

    # Replace placeholders (use different delimiter to avoid path issues)
    local filename_basename
    filename_basename=$(basename "$file")

    sed -i.bak \
        -e "s|%FILENAME%|${filename_basename}|g" \
        -e "s|%CLASS_NAME%|${class_name}|g" \
        -e "s|%MODEL_PATH%|${model_path}|g" \
        -e "s|%DESCRIPTION%|${description}|g" \
        -e "s|%DISPLAY_NAME%|${class_name}|g" \
        "$file" && rm -f "${file}.bak"

    if [ $? -ne 0 ]; then
        print_error "Failed to process template file: $file"
        return 1
    fi

    return 0
}

# Generate ZScript monster replacement
generate_zscript_monster() {
    local file="$1"
    local class_name="$2"
    local base_class="$3"
    local model_path="$4"
    local description="$5"

    cat > "$file" << 'ZSCRIPT_MONSTER_EOF'
/*
** %FILENAME%
**
** ZScript: %CLASS_NAME% - glTF Monster Replacement
** %DESCRIPTION%
**
** Replaces %BASE_CLASS% with a glTF 2.0 model
**
**---------------------------------------------------------------------------
*/

class %CLASS_NAME% : %BASE_CLASS% replaces %BASE_CLASS%
{
    mixin GLTFModel;

    Default
    {
        // Inherit properties from base class
        // Add custom properties here if needed
        Tag "%DISPLAY_NAME%";
    }

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            // Initialize glTF model
            if (!InitGLTFModel("%MODEL_PATH%"))
            {
                Console.Printf("ERROR: Failed to load monster model: %MODEL_PATH%");
                return;
            }

            // Configure model
            SetModelScaleUniform(1.0); // Adjust to match original monster size
            SetModelOffset(0, 0, 0);

            // Enable PBR materials
            SetPBREnabled(true);

            // Start idle animation
            PlayAnimation("Idle", loop: true);

            if (developer >= 1)
                Console.Printf("Monster model loaded: %CLASS_NAME%");
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
        }
        TNT1 A 3
        {
            A_Chase();
            UpdateGLTFModel();
        }
        Loop;

    Melee:
        TNT1 A 0
        {
            A_FaceTarget();
            PlayAnimation("Attack_Melee", loop: false, blendTime: 0.1);
        }
        TNT1 A 8
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_CustomMeleeAttack(damage * random(1, 8));
        TNT1 A 8
        {
            UpdateGLTFModel();
        }
        Goto See;

    Missile:
        TNT1 A 0
        {
            A_FaceTarget();
            PlayAnimation("Attack_Ranged", loop: false, blendTime: 0.1);
        }
        TNT1 A 10
        {
            UpdateGLTFModel();
        }
        TNT1 A 0 A_SpawnProjectile("DoomImpBall"); // Replace with appropriate projectile
        TNT1 A 10
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
            // Random death animation if multiple exist
            if (random(0, 1) && HasAnimation("Death2"))
                PlayAnimation("Death2", loop: false, blendTime: 0.1);
            else
                PlayAnimation("Death1", loop: false, blendTime: 0.1);
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

    XDeath: // Gibbed death
        TNT1 A 0
        {
            if (HasAnimation("DeathGib"))
                PlayAnimation("DeathGib", loop: false);
            else
                PlayAnimation("Death1", loop: false); // Fallback
        }
        TNT1 A 5 A_XScream();
        TNT1 A 5 A_NoBlocking();
        TNT1 A 5
        {
            UpdateGLTFModel();
        }
        TNT1 A -1
        {
            UpdateGLTFModel();
        }
        Stop;
    }

    // Check if animation exists
    private bool HasAnimation(String animName)
    {
        // Try to find animation (this would need native support)
        // For now, always return false and add animations as needed
        return false;
    }
}
ZSCRIPT_MONSTER_EOF

    # Replace placeholders
    local filename_basename
    filename_basename=$(basename "$file")

    sed -i.bak \
        -e "s|%FILENAME%|${filename_basename}|g" \
        -e "s|%CLASS_NAME%|${class_name}|g" \
        -e "s|%BASE_CLASS%|${base_class}|g" \
        -e "s|%MODEL_PATH%|${model_path}|g" \
        -e "s|%DESCRIPTION%|${description}|g" \
        -e "s|%DISPLAY_NAME%|${class_name}|g" \
        "$file" && rm -f "${file}.bak"

    if [ $? -ne 0 ]; then
        print_error "Failed to process template file: $file"
        return 1
    fi

    return 0
}

# Generate ZScript item replacement
generate_zscript_item() {
    local file="$1"
    local class_name="$2"
    local base_class="$3"
    local model_path="$4"
    local description="$5"

    cat > "$file" << 'ZSCRIPT_ITEM_EOF'
/*
** %FILENAME%
**
** ZScript: %CLASS_NAME% - glTF Item Replacement
** %DESCRIPTION%
**
** Replaces %BASE_CLASS% with a glTF 2.0 model
**
**---------------------------------------------------------------------------
*/

class %CLASS_NAME% : %BASE_CLASS% replaces %BASE_CLASS%
{
    mixin GLTFModel;

    // Animation state
    private double rotationPhase;
    private double bobPhase;

    Default
    {
        // Inherit properties from base class
        Tag "%DISPLAY_NAME%";
    }

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            // Initialize glTF model
            if (!InitGLTFModel("%MODEL_PATH%"))
            {
                Console.Printf("ERROR: Failed to load item model: %MODEL_PATH%");
                return;
            }

            // Configure model appearance
            SetModelScaleUniform(0.5); // Items are usually smaller
            SetModelOffset(0, 0, 0);

            // Enable PBR
            SetPBREnabled(true);

            // Start floating/idle animation
            if (HasAnimation("Float"))
                PlayAnimation("Float", loop: true);
            else
                PlayAnimation("Idle", loop: true);

            // Initialize animation state
            rotationPhase = frandom(0, 360);
            bobPhase = frandom(0, 360);

            if (developer >= 1)
                Console.Printf("Item model loaded: %CLASS_NAME%");
        }
        TNT1 A 1
        {
            UpdateGLTFModel();

            // Procedural floating animation
            UpdateFloatingAnimation();
        }
        Wait;
    }

    // Procedural floating animation for pickups
    private void UpdateFloatingAnimation()
    {
        // Slow rotation
        rotationPhase += 1.0;
        if (rotationPhase >= 360) rotationPhase -= 360;

        // Bobbing motion
        bobPhase += 2.0;
        if (bobPhase >= 360) bobPhase -= 360;

        double bobOffset = sin(bobPhase) * 4.0;

        // Apply rotation to model
        SetModelRotation(0, rotationPhase, 0);

        // Apply bobbing (adjust Z offset)
        SetModelOffset(0, 0, bobOffset);

        // Optional: Pulsing glow for special items
        if (bCOUNTITEM) // Special items glow
        {
            double glowPhase = sin(level.time * 0.1);
            double glowStrength = 0.5 + glowPhase * 0.5;

            // Apply subtle emissive glow
            SetEmissive(Color(255, 100, 150, 200), glowStrength);
        }
    }

    // Check if animation exists
    private bool HasAnimation(String animName)
    {
        // Simplified check - expand as needed
        return false;
    }
}
ZSCRIPT_ITEM_EOF

    # Replace placeholders
    local filename_basename
    filename_basename=$(basename "$file")

    sed -i.bak \
        -e "s|%FILENAME%|${filename_basename}|g" \
        -e "s|%CLASS_NAME%|${class_name}|g" \
        -e "s|%BASE_CLASS%|${base_class}|g" \
        -e "s|%MODEL_PATH%|${model_path}|g" \
        -e "s|%DESCRIPTION%|${description}|g" \
        -e "s|%DISPLAY_NAME%|${class_name}|g" \
        "$file" && rm -f "${file}.bak"

    if [ $? -ne 0 ]; then
        print_error "Failed to process template file: $file"
        return 1
    fi

    return 0
}

# Generate ZScript weapon replacement
generate_zscript_weapon() {
    local file="$1"
    local class_name="$2"
    local base_class="$3"
    local model_path="$4"
    local description="$5"

    cat > "$file" << 'ZSCRIPT_WEAPON_EOF'
/*
** %FILENAME%
**
** ZScript: %CLASS_NAME% - glTF Weapon Replacement
** %DESCRIPTION%
**
** Replaces %BASE_CLASS% with a glTF 2.0 model (first-person viewmodel)
**
**---------------------------------------------------------------------------
*/

class %CLASS_NAME% : %BASE_CLASS% replaces %BASE_CLASS%
{
    mixin GLTFModel;

    // Weapon state
    private bool modelInitialized;

    Default
    {
        // Inherit weapon properties
        Tag "%DISPLAY_NAME%";
    }

    States
    {
    Ready:
        TNT1 A 0
        {
            // Initialize model once
            if (!modelInitialized)
            {
                if (!InitGLTFModel("%MODEL_PATH%"))
                {
                    Console.Printf("ERROR: Failed to load weapon model: %MODEL_PATH%");
                    return;
                }

                // Position for first-person view
                SetModelScaleUniform(2.5); // Larger for FP view
                SetModelOffset(0, 15, 28);  // Position in front of camera
                SetModelRotation(0, 0, 0);

                // Enable PBR
                SetPBREnabled(true);

                modelInitialized = true;

                if (developer >= 1)
                    Console.Printf("Weapon model loaded: %CLASS_NAME%");
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
        TNT1 A 0 A_FireBullets(5.6, 0, 1, 5); // Adjust for weapon type
        TNT1 A 0 A_PlaySound("weapons/pistol", CHAN_WEAPON); // Replace sound
        TNT1 A 6
        {
            UpdateGLTFModel();

            // Spawn muzzle flash at muzzle bone if it exists
            if (BoneExists("Muzzle"))
            {
                Vector3 muzzlePos = GetBoneWorldPosition("Muzzle");
                // Spawn muzzle flash actor at bone position
                // Actor flash = Spawn("BulletPuff", muzzlePos);
            }
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

    Reload: // Optional reload state
        TNT1 A 0
        {
            PlayAnimation("Reload", loop: false, blendTime: 0.15);
        }
        TNT1 A 30 // Duration matches reload animation
        {
            UpdateGLTFModel();

            // Trigger events at specific animation times
            double animTime = GetAnimationTime();

            // Magazine eject at 30% through animation
            if (animTime >= 0.3 && animTime < 0.35)
            {
                A_PlaySound("weapons/reload_eject", CHAN_AUTO);
            }

            // Magazine insert at 70% through animation
            if (animTime >= 0.7 && animTime < 0.75)
            {
                A_PlaySound("weapons/reload_insert", CHAN_AUTO);
            }
        }
        TNT1 A 0
        {
            PlayAnimation("Idle", loop: true, blendTime: 0.2);
        }
        Goto Ready;
    }
}
ZSCRIPT_WEAPON_EOF

    # Replace placeholders
    local filename_basename
    filename_basename=$(basename "$file")

    sed -i.bak \
        -e "s|%FILENAME%|${filename_basename}|g" \
        -e "s|%CLASS_NAME%|${class_name}|g" \
        -e "s|%BASE_CLASS%|${base_class}|g" \
        -e "s|%MODEL_PATH%|${model_path}|g" \
        -e "s|%DESCRIPTION%|${description}|g" \
        -e "s|%DISPLAY_NAME%|${class_name}|g" \
        "$file" && rm -f "${file}.bak"

    if [ $? -ne 0 ]; then
        print_error "Failed to process template file: $file"
        return 1
    fi

    return 0
}

# Generate ZScript decoration replacement
generate_zscript_decoration() {
    local file="$1"
    local class_name="$2"
    local model_path="$3"
    local description="$4"
    local is_solid="$5"

    cat > "$file" << 'ZSCRIPT_DECORATION_EOF'
/*
** %FILENAME%
**
** ZScript: %CLASS_NAME% - glTF Decoration
** %DESCRIPTION%
**
** Static or animated decoration using glTF 2.0 model
**
**---------------------------------------------------------------------------
*/

class %CLASS_NAME% : Actor
{
    mixin GLTFModel;

    Default
    {
        Radius 16;
        Height 64;
        %SOLID_FLAG%
        +NOBLOOD
        Tag "%DISPLAY_NAME%";
    }

    States
    {
    Spawn:
        TNT1 A 0 NoDelay
        {
            // Initialize glTF model
            if (!InitGLTFModel("%MODEL_PATH%"))
            {
                Console.Printf("ERROR: Failed to load decoration model: %MODEL_PATH%");
                return;
            }

            // Configure model
            SetModelScaleUniform(1.0);
            SetModelOffset(0, 0, 0);

            // Enable PBR
            SetPBREnabled(true);

            // Start animation (if model has animation)
            PlayAnimation("Idle", loop: true);

            if (developer >= 1)
                Console.Printf("Decoration model loaded: %CLASS_NAME%");
        }
        TNT1 A 1
        {
            UpdateGLTFModel();
        }
        Wait;
    }
}
ZSCRIPT_DECORATION_EOF

    # Replace placeholders
    local filename_basename
    filename_basename=$(basename "$file")
    local solid_flag

    if [ "$is_solid" = "yes" ]; then
        solid_flag="+SOLID"
    else
        solid_flag="-SOLID"
    fi

    sed -i.bak \
        -e "s|%FILENAME%|${filename_basename}|g" \
        -e "s|%CLASS_NAME%|${class_name}|g" \
        -e "s|%MODEL_PATH%|${model_path}|g" \
        -e "s|%DESCRIPTION%|${description}|g" \
        -e "s|%DISPLAY_NAME%|${class_name}|g" \
        -e "s|%SOLID_FLAG%|${solid_flag}|g" \
        "$file" && rm -f "${file}.bak"

    if [ $? -ne 0 ]; then
        print_error "Failed to process template file: $file"
        return 1
    fi

    return 0
}

# Generate MODELDEF
generate_modeldef() {
    local file="$1"
    local class_name="$2"
    local model_path="$3"

    cat > "$file" << EOF
// MODELDEF for $class_name
//
// This file is OPTIONAL with glTF models managed via ZScript.
// Use MODELDEF for simple sprite-to-model replacements without animation control.

Model $class_name
{
    Path "$model_path"
    Model 0 "$(basename $model_path)"
    Scale 1.0 1.0 1.0
    Offset 0.0 0.0 0.0

    // Animation frames (if using MODELDEF method)
    // FrameIndex SPRT A 0 0

    // PBR flags (if supported)
    // USEACTORPITCH
    // USEACTORROLL
}
EOF
}

# Generate README documentation
generate_readme() {
    local file="$1"
    local class_name="$2"
    local asset_type="$3"
    local base_class="$4"
    local model_path="$5"

    cat > "$file" << EOF
# $class_name - glTF Model Replacement

**Type:** $asset_type
**Base Class:** ${base_class:-N/A}
**Model Path:** $model_path

## Overview

This package replaces the Doom sprite for \`${base_class:-$class_name}\` with a glTF 2.0 3D model, providing:
- Skeletal animation support
- PBR materials (metallic-roughness workflow)
- Dynamic bone manipulation
- Procedural animation capabilities

## Directory Structure

\`\`\`
.
├── models/           # glTF model files (.gltf + .bin + textures)
│   └── ${class_name,,}/
│       ├── ${class_name,,}.gltf
│       ├── ${class_name,,}.bin
│       └── textures/
│           ├── basecolor.png
│           ├── normal.png
│           └── metallic_roughness.png
├── zscript/          # ZScript actor definitions
│   └── ${class_name}.zs
├── modeldef.txt      # Optional MODELDEF (not needed with ZScript method)
└── docs/
    └── README.md     # This file
\`\`\`

## Required Blender Animations

The glTF model should include these animations (names are case-sensitive):

EOF

    case "$asset_type" in
        "Player")
            cat >> "$file" << EOF
- **Idle**: Standing idle (looped)
- **Walk**: Walking animation (looped)
- **Run**: Running animation (looped)
- **Jump**: Jump start
- **Fall**: Falling through air
- **Float**: Floating/hovering (optional)
- **Pain**: Taking damage
- **Death1**: Primary death animation
- **Death2**: Alternative death (optional)
- **DeathGib**: Gibbed/extreme death (optional)

### Recommended Bone Names (Blender)
- Head
- Spine_01, Spine_02, Spine_03
- UpperArm_L, UpperArm_R
- LowerArm_L, LowerArm_R
- Hand_L, Hand_R
- UpperLeg_L, UpperLeg_R
- LowerLeg_L, LowerLeg_R
- Foot_L, Foot_R
EOF
            ;;
        "Monster")
            cat >> "$file" << EOF
- **Idle**: Standing idle (looped)
- **Walk**: Movement animation (looped)
- **Attack_Melee**: Melee attack
- **Attack_Ranged**: Ranged attack (if applicable)
- **Pain**: Taking damage
- **Death1**: Primary death animation
- **Death2**: Alternative death (optional)
- **DeathGib**: Gibbed death (optional)

### Recommended Bone Names
- Head
- Spine/Body bones
- Limb bones (Arms, Legs, Wings, etc.)
- Jaw (for monsters with mouth attacks)
- Tail bones (if applicable)
EOF
            ;;
        "Item")
            cat >> "$file" << EOF
- **Idle** or **Float**: Floating/rotating idle (looped)

### Bone Names
Items typically don't need bones unless they have special animations.
EOF
            ;;
        "Weapon")
            cat >> "$file" << EOF
- **Idle**: Weapon idle (looped)
- **Select**: Weapon raise/equip
- **Deselect**: Weapon lower/holster
- **Fire**: Primary fire animation
- **Reload**: Reload animation (optional)
- **AltFire**: Alternative fire (optional)

### Recommended Bone Names
- Muzzle: Muzzle flash attachment point
- Magazine: Magazine for reload animations
- Slide: Pistol slide (if applicable)
- Bolt: Rifle bolt (if applicable)
- Eject: Shell ejection point
EOF
            ;;
        "Decoration")
            cat >> "$file" << EOF
- **Idle**: Looping animation (optional for static decorations)

### Bone Names
Decorations may not need bones unless animated (e.g., torches, fans).
EOF
            ;;
    esac

    cat >> "$file" << EOF

## Installation

1. **Copy model files:**
   \`\`\`bash
   cp -r models/${class_name,,}/ <your-pk3>/models/
   \`\`\`

2. **Add ZScript file:**
   \`\`\`bash
   cp zscript/${class_name}.zs <your-pk3>/zscript/
   \`\`\`

3. **Update ZScript include:**
   Add to your \`zscript.txt\`:
   \`\`\`
   #include "zscript/${class_name}.zs"
   \`\`\`

4. **Build PK3:**
   \`\`\`bash
   cd <your-pk3>
   zip -r MyMod.pk3 *
   \`\`\`

5. **Test in NeoDoom:**
   \`\`\`bash
   neodoom -file MyMod.pk3
   \`\`\`

## Blender Export Settings

When exporting from Blender:

1. **File → Export → glTF 2.0 (.gltf/.glb)**
2. **Format:** glTF Separate (.gltf + .bin + textures)
3. **Include:**
   - [x] Cameras unchecked
   - [x] Selected Objects (if selecting specific)
   - [x] Custom Properties unchecked
4. **Transform:**
   - [x] +Y Up (default)
5. **Geometry:**
   - [x] Apply Modifiers
   - [x] UVs
   - [x] Normals
   - [x] Tangents
   - [x] Vertex Colors (optional)
6. **Animation:**
   - [x] Use Current Frame unchecked
   - [x] Animations
   - [x] Limit to Playback Range unchecked
   - [x] Sampling Rate: 30 or 60 FPS
   - [x] Always Sample Animations
   - [x] Group by NLA Track (if using NLA)
   - [x] Export Deformation Bones Only unchecked
   - [x] Optimize Animation Size

## Customization

### Adjusting Scale
In the ZScript file, modify:
\`\`\`zscript
SetModelScaleUniform(1.0); // Change this value
\`\`\`

### Adjusting Position
\`\`\`zscript
SetModelOffset(0, 0, 0); // X, Y, Z offset
\`\`\`

### Animation Speed
\`\`\`zscript
SetAnimationSpeed(1.5); // 1.5x speed
\`\`\`

### PBR Materials
\`\`\`zscript
SetupPBRMetal(0.9, 0.3);      // Metallic surface
SetupPBRPlastic(0.6);          // Plastic surface
SetupPBRStone(0.9);            // Rough stone
\`\`\`

## Troubleshooting

### Model doesn't appear
- Check console for error messages
- Verify model path in ZScript matches actual file location
- Ensure glTF format is "Separate" (.gltf + .bin), not GLB
- Check that textures are in correct location with relative paths

### Animations don't play
- Verify animation names in Blender match ZScript exactly (case-sensitive)
- Check that animations were exported (Animation checkbox in Blender)
- Enable \`developer 1\` in console and check for animation errors

### Model is wrong size
- Adjust \`SetModelScaleUniform()\` value in ZScript
- In Blender: Apply scale before export (Ctrl+A → Scale)

### Textures look wrong
- Ensure texture color spaces are correct in Blender:
  - Base Color: sRGB
  - Normal, Metallic-Roughness: Non-Color
- Check texture paths are relative in glTF file

### Performance issues
- Reduce model polygon count in Blender
- Use smaller texture resolutions (1024x1024 or 512x512)
- Limit bone count to <100 per model

## References

- [NeoDoom glTF Documentation](../../../GLTF_IMPLEMENTATION.md)
- [ZScript API Reference](../../../GLTF_ZSCRIPT_API.md)
- [Blender Modeling Guide](../../../docs/BLENDER_GLTF_MODELING_GUIDE.md)
- [glTF 2.0 Specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)

## License

[Add your license information here]

---

Generated by NeoDoom glTF Model Replacement Generator v2.0
EOF
}

# Generate load order file
generate_zscript_include() {
    local file="$1"
    local class_name="$2"

    cat > "$file" << EOF
// ZScript Load Order for $class_name
//
// Add this line to your main zscript.txt file:

#include "zscript/${class_name}.zs"
EOF
}

# Main script
main() {
    # Trap errors and interrupts
    trap 'echo -e "\n${RED}Script interrupted${NC}"; exit 130' INT TERM
    trap 'if [ $? -ne 0 ]; then echo -e "${RED}Script failed${NC}"; fi' EXIT

    clear
    print_banner

    # Check if we're in the right directory
    if [ ! -f "$PROJECT_ROOT/CMakeLists.txt" ]; then
        print_warning "Not in NeoDoom project root. Script will create files in current directory."
        PROJECT_ROOT="$(pwd)"
    fi

    print_section "Asset Type Selection"

    # Asset type menu
    local asset_types=(
        "Player Model (DoomPlayer replacement)"
        "Monster/Enemy (e.g., Imp, Baron, etc.)"
        "Item/Pickup (e.g., Health, Armor, Ammo)"
        "Weapon (First-person viewmodel)"
        "Decoration (Static or animated prop)"
    )

    show_menu "What type of asset do you want to create?" "${asset_types[@]}"

    local selection
    selection=$(get_selection ${#asset_types[@]})

    if [ $? -ne 0 ]; then
        print_error "Failed to get selection"
        exit 1
    fi

    local asset_type=""
    case "$selection" in
        1) asset_type="Player" ;;
        2) asset_type="Monster" ;;
        3) asset_type="Item" ;;
        4) asset_type="Weapon" ;;
        5) asset_type="Decoration" ;;
        *)
            print_error "Invalid selection: $selection"
            exit 1
            ;;
    esac

    print_info "Selected: ${asset_types[$((selection-1))]}"

    # Get basic information
    print_section "Basic Information"

    local class_name=""
    while true; do
        prompt_input "Class name (e.g., GLTFDoomGuy, GLTFImp)" "" class_name
        if [ -z "$class_name" ]; then
            print_error "Class name cannot be empty"
            continue
        fi
        if validate_class_name "$class_name"; then
            break
        fi
    done

    local model_filename=""
    local class_name_lower="${class_name,,}"
    prompt_input "Model filename (without extension)" "$class_name_lower" model_filename

    if [ -z "$model_filename" ]; then
        model_filename="$class_name_lower"
    fi

    local description=""
    prompt_input "Short description" "glTF model replacement for $asset_type" description

    if [ -z "$description" ]; then
        description="glTF model replacement for $asset_type"
    fi

    # Base class (for replacements)
    local base_class=""
    if [ "$asset_type" != "Player" ] && [ "$asset_type" != "Decoration" ]; then
        print_section "Base Class"

        case "$asset_type" in
            "Monster")
                print_info "Common Doom monster classes:"
                echo "  - DoomImp, ZombieMan, ShotgunGuy, ChaingunGuy"
                echo "  - Demon (Pinky), Spectre, LostSoul, Cacodemon"
                echo "  - BaronOfHell, HellKnight, Arachnotron, PainElemental"
                echo "  - Revenant, Mancubus, Archvile, SpiderMastermind, Cyberdemon"
                prompt_input "Base class to replace" "DoomImp" base_class
                ;;
            "Item")
                print_info "Common Doom item classes:"
                echo "  - HealthBonus, Stimpack, Medikit"
                echo "  - ArmorBonus, GreenArmor, BlueArmor"
                echo "  - Clip, ClipBox, Shell, ShellBox, RocketAmmo, RocketBox, Cell, CellPack"
                echo "  - Berserk, Soulsphere, Megasphere, InvulnerabilitySphere"
                prompt_input "Base class to replace" "Medikit" base_class
                ;;
            "Weapon")
                print_info "Common Doom weapon classes:"
                echo "  - Fist, Chainsaw, Pistol, Shotgun, SuperShotgun"
                echo "  - Chaingun, RocketLauncher, PlasmaRifle, BFG9000"
                prompt_input "Base class to replace" "Pistol" base_class
                ;;
        esac

        if [ -z "$base_class" ]; then
            print_error "Base class cannot be empty"
            exit 1
        fi
    fi

    # Output location
    print_section "Output Location"

    local class_name_lower="${class_name,,}"
    local default_output="$PROJECT_ROOT/wadsrc/static/models_gltf/${class_name_lower}"
    local output_dir=""

    prompt_input "Output directory" "$default_output" output_dir

    if [ -z "$output_dir" ]; then
        output_dir="$default_output"
    fi

    # Validate output directory parent exists
    local output_parent
    output_parent=$(dirname "$output_dir")
    if [ ! -d "$output_parent" ]; then
        print_warning "Parent directory does not exist: $output_parent"
        print_info "It will be created automatically"
    fi

    # Confirm
    print_section "Summary"
    echo ""
    echo -e "${BOLD}Configuration:${NC}"
    echo -e "  Type:        $asset_type"
    echo -e "  Class:       $class_name"
    if [ -n "$base_class" ]; then
        echo -e "  Replaces:    $base_class"
    fi
    echo -e "  Model:       models/${class_name_lower}/${model_filename}.gltf"
    echo -e "  Output:      $output_dir"
    echo ""

    local confirm=""
    echo -e -n "${MAGENTA}?${NC} ${BOLD}Create this structure? [Y/n]:${NC} "
    read -r confirm

    if [[ "$confirm" =~ ^[Nn] ]]; then
        print_warning "Cancelled by user"
        exit 0
    fi

    # Create structure
    print_section "Creating Files"

    if ! create_directories "$output_dir"; then
        print_error "Failed to create directory structure"
        exit 1
    fi

    # Model path for ZScript
    local model_path="models/${class_name_lower}/${model_filename}.gltf"

    # Generate ZScript based on type
    local zscript_file="$output_dir/zscript/${class_name}.zs"

    case "$asset_type" in
        "Player")
            if ! generate_zscript_player "$zscript_file" "$class_name" "$model_path" "$description"; then
                print_error "Failed to generate player ZScript"
                exit 1
            fi
            ;;
        "Monster")
            if ! generate_zscript_monster "$zscript_file" "$class_name" "$base_class" "$model_path" "$description"; then
                print_error "Failed to generate monster ZScript"
                exit 1
            fi
            ;;
        "Item")
            if ! generate_zscript_item "$zscript_file" "$class_name" "$base_class" "$model_path" "$description"; then
                print_error "Failed to generate item ZScript"
                exit 1
            fi
            ;;
        "Weapon")
            if ! generate_zscript_weapon "$zscript_file" "$class_name" "$base_class" "$model_path" "$description"; then
                print_error "Failed to generate weapon ZScript"
                exit 1
            fi
            ;;
        "Decoration")
            local is_solid=""
            prompt_input "Is decoration solid? (blocks movement) [yes/no]" "no" is_solid
            if [ -z "$is_solid" ]; then
                is_solid="no"
            fi
            if ! generate_zscript_decoration "$zscript_file" "$class_name" "$model_path" "$description" "$is_solid"; then
                print_error "Failed to generate decoration ZScript"
                exit 1
            fi
            ;;
    esac

    print_success "Created ZScript: $zscript_file"

    # Generate MODELDEF (optional)
    local modeldef_file="$output_dir/modeldef.txt"
    if ! generate_modeldef "$modeldef_file" "$class_name" "$model_path"; then
        print_warning "Failed to generate MODELDEF (optional file)"
    else
        print_success "Created MODELDEF: $modeldef_file"
    fi

    # Generate README
    local readme_file="$output_dir/docs/README.md"
    if ! generate_readme "$readme_file" "$class_name" "$asset_type" "$base_class" "$model_path"; then
        print_warning "Failed to generate README"
    else
        print_success "Created README: $readme_file"
    fi

    # Generate ZScript include reference
    local include_file="$output_dir/zscript_include.txt"
    if ! generate_zscript_include "$include_file" "$class_name"; then
        print_warning "Failed to generate include reference"
    else
        print_success "Created include reference: $include_file"
    fi

    # Create placeholder for model
    if ! mkdir -p "$output_dir/models/${class_name_lower}/textures" 2>/dev/null; then
        print_warning "Failed to create model directories"
    else
        touch "$output_dir/models/${class_name_lower}/.gitkeep" 2>/dev/null || true
        touch "$output_dir/models/${class_name_lower}/textures/.gitkeep" 2>/dev/null || true
        print_success "Created model directory structure"
    fi

    # Final instructions
    print_section "Next Steps"
    echo ""
    echo -e "${GREEN}✓${NC} Structure created successfully!"
    echo ""
    echo -e "${BOLD}To complete your model replacement:${NC}"
    echo ""
    echo -e "1. ${CYAN}Create your glTF model in Blender:${NC}"
    echo -e "   - Export as: glTF 2.0 Separate (.gltf + .bin + textures)"
    echo -e "   - Save to: ${YELLOW}$output_dir/models/${class_name,,}/${NC}"
    echo -e "   - See: ${YELLOW}$readme_file${NC} for animation requirements"
    echo ""
    echo -e "2. ${CYAN}Add to your mod's ZScript:${NC}"
    echo -e "   - Add this line to your ${YELLOW}zscript.txt${NC}:"
    echo -e "     ${GREEN}#include \"zscript/${class_name}.zs\"${NC}"
    echo ""
    echo -e "3. ${CYAN}Package your mod:${NC}"
    echo -e "   ${YELLOW}cd $output_dir && zip -r ${class_name}.pk3 *${NC}"
    echo ""
    echo -e "4. ${CYAN}Test in NeoDoom:${NC}"
    echo -e "   ${YELLOW}neodoom -file ${class_name}.pk3${NC}"
    echo ""
    echo -e "${BOLD}Documentation:${NC}"
    echo -e "  - Model guide:   ${BLUE}$PROJECT_ROOT/docs/BLENDER_GLTF_MODELING_GUIDE.md${NC}"
    echo -e "  - ZScript API:   ${BLUE}$PROJECT_ROOT/GLTF_ZSCRIPT_API.md${NC}"
    echo -e "  - Local README:  ${BLUE}$readme_file${NC}"
    echo ""
    print_success "Happy modding! 🎮"
}

# Run main function
main "$@"
