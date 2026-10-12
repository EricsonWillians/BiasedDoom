//-----------------------------------------------------------------------------
//
// Experimental local presentation cameras.
//
// These modes alter only the local renderer and locally-built user commands.
// They are intentionally unavailable in netgames and demos, where a camera
// that consumes input would make a deterministic stream misleading.
//
//-----------------------------------------------------------------------------

#include "experimental_camera.h"

#include "c_console.h"
#include "c_dispatch.h"
#include "d_main.h"
#include "d_event.h"
#include "d_player.h"
#include "d_protocol.h"
#include "doomdef.h"
#include "doomstat.h"
#include "g_levellocals.h"
#include "am_map.h"
#include "p_local.h"
#include "r_utility.h"

// Persist the old renderer-oriented controls so existing configs remain
// loadable. The textured-automap presentation intentionally does not read
// them anymore.
CVAR(Float, cam_topdown_height, 320.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, cam_topdown_pitch, 82.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, cam_topdown_wall_overlay, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, cam_topdown_fov, 85.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, cam_topdown_player_marker, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const int clampedMarker = clamp<int>(self, 0, 1);
	if (self != clampedMarker)
	{
		self = clampedMarker;
	}
}

CUSTOM_CVAR(Float, cam_topdown_player_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const float clampedScale = clamp<float>(self, 0.25f, 4.0f);
	if (self != clampedScale) self = clampedScale;
}

CUSTOM_CVAR(Float, cam_topdown_monster_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const float clampedScale = clamp<float>(self, 0.25f, 4.0f);
	if (self != clampedScale) self = clampedScale;
}

CUSTOM_CVAR(Float, cam_topdown_thing_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const float clampedScale = clamp<float>(self, 0.25f, 4.0f);
	if (self != clampedScale) self = clampedScale;
}

CVAR(Bool, cam_topdown_crosshair, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, cam_topdown_direct_aim, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, cam_topdown_freepan, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, cam_topdown_pan_speed, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const float clampedSpeed = clamp<float>(self, 0.25f, 4.0f);
	if (self != clampedSpeed) self = clampedSpeed;
}

namespace
{
enum EExperimentalCameraMode
{
	ECM_Normal,
	ECM_Photo,
	ECM_TopDown,
};

struct FPhotoCameraState
{
	bool Initialized = false;
	FLevelLocals* Level = nullptr;
	DVector3 Position = {};
	DRotator Angles = { nullAngle, nullAngle, nullAngle };
};

FPhotoCameraState PhotoCamera;

struct FTopDownCameraState
{
	bool Initialized = false;
	FLevelLocals* Level = nullptr;
	DAngle Yaw = nullAngle;
	bool AutomapClaimed = false;
	FLevelLocals* AutomapLevel = nullptr;
	bool RestoreAutomapActive = false;
	bool RestoreViewActive = true;
};

FTopDownCameraState TopDownCamera;

constexpr double CmdToDegrees = 360.0 / 65536.0;
// Keep this in lockstep with PlayerPawn.TURN180_TICKS. This is used only to
// express this tic's screen-relative movement in the basis that the player
// pawn will use when it processes the command.
constexpr int Turn180Tics = (TICRATE / 4) + 1;

bool IsExperimentalCameraAllowed()
{
	return gamestate == GS_LEVEL && !netgame && !demoplayback && !demorecording
		&& consoleplayer >= 0 && consoleplayer < int(MAXPLAYERS)
		&& players[consoleplayer].mo != nullptr;
}

void ReleaseTopDownAutomap()
{
	if (!TopDownCamera.AutomapClaimed)
	{
		return;
	}

	if (TopDownCamera.AutomapLevel != nullptr && TopDownCamera.AutomapLevel->automap != nullptr)
	{
		TopDownCamera.AutomapLevel->automap->SetTopDownCameraPan({});
		TopDownCamera.AutomapLevel->automap->SetTopDownCamera(false, false);
	}

	automapactive = TopDownCamera.RestoreAutomapActive;
	viewactive = TopDownCamera.RestoreViewActive;
	TopDownCamera.AutomapClaimed = false;
	TopDownCamera.AutomapLevel = nullptr;
}

void ResetExperimentalCameras()
{
	ReleaseTopDownAutomap();
	PhotoCamera.Initialized = false;
	PhotoCamera.Level = nullptr;
	TopDownCamera.Initialized = false;
	TopDownCamera.Level = nullptr;
	R_ResetViewInterpolation();
}

bool IsMainPlayerView(const FRenderViewpoint& viewPoint)
{
	return consoleplayer >= 0 && consoleplayer < int(MAXPLAYERS)
		&& viewPoint.camera != nullptr
		&& viewPoint.camera == players[consoleplayer].camera;
}

void InitializePhotoCameraFromPlayer()
{
	if (PhotoCamera.Level != players[consoleplayer].mo->Level)
	{
		PhotoCamera.Initialized = false;
	}

	if (PhotoCamera.Initialized || !IsExperimentalCameraAllowed())
	{
		return;
	}

	const player_t& player = players[consoleplayer];
	PhotoCamera.Position = { player.mo->Pos().XY(), player.viewz };
	PhotoCamera.Angles = player.mo->Angles;
	PhotoCamera.Level = player.mo->Level;
	PhotoCamera.Initialized = true;
}

void InitializePhotoCameraFromView(const FRenderViewpoint& viewPoint)
{
	if (PhotoCamera.Initialized)
	{
		return;
	}

	PhotoCamera.Position = viewPoint.Pos;
	PhotoCamera.Angles = viewPoint.Angles;
	PhotoCamera.Level = viewPoint.ViewLevel;
	PhotoCamera.Initialized = true;
}

void InitializeTopDownCamera()
{
	if (TopDownCamera.Level != players[consoleplayer].mo->Level)
	{
		TopDownCamera.Initialized = false;
	}

	if (TopDownCamera.Initialized || !IsExperimentalCameraAllowed())
	{
		return;
	}

	TopDownCamera.Level = players[consoleplayer].mo->Level;
	// The automap is north-up. Yaw 90 maps W to screen-up in Doom's XY basis.
	TopDownCamera.Yaw = DAngle::fromDeg(90.0);
	TopDownCamera.Initialized = true;
}

DAngle GetPlayerYawForTopDownMovement(const AActor* player, const usercmd_t& cmd)
{
	const player_t& playerInfo = players[consoleplayer];
	const bool startsTurn180 = (cmd.buttons & BT_TURN180) != 0
		&& (playerInfo.oldbuttons & BT_TURN180) == 0;

	// PlayerPawn::HandleMovement starts a 180-degree turn before it calls
	// MovePlayer. MovePlayer then ignores cmd.yaw and advances the yaw by one
	// fixed turn step. Projecting against cmd.yaw during that transition makes
	// the movement basis briefly point to the wrong side of the north-up map.
	if (playerInfo.turnticks != 0 || startsTurn180)
	{
		return player->Angles.Yaw + DAngle::fromDeg(180.0 / Turn180Tics);
	}

	return player->Angles.Yaw + DAngle::fromDeg(double(cmd.yaw) * CmdToDegrees);
}

void EnsureTopDownAutomap()
{
	if (!IsExperimentalCameraAllowed())
	{
		ReleaseTopDownAutomap();
		return;
	}

	FLevelLocals* const level = players[consoleplayer].mo->Level;
	if (TopDownCamera.AutomapClaimed && TopDownCamera.AutomapLevel != level)
	{
		ReleaseTopDownAutomap();
	}

	if (primaryLevel == nullptr || primaryLevel->automap == nullptr)
	{
		return;
	}

	if (!TopDownCamera.AutomapClaimed)
	{
		TopDownCamera.RestoreAutomapActive = automapactive;
		TopDownCamera.RestoreViewActive = viewactive;
		TopDownCamera.AutomapClaimed = true;
		TopDownCamera.AutomapLevel = primaryLevel;
		primaryLevel->automap->startDisplay();
	}

	// A full-screen automap is the complete presentation in this mode. It
	// keeps every floor, wall, player, and marker in the same map transform.
	automapactive = true;
	viewactive = false;
	primaryLevel->automap->SetTopDownCamera(true, cam_topdown_player_marker != 0);
}

bool CanToggleCamera(const char* modeName)
{
	if (IsExperimentalCameraAllowed())
	{
		return true;
	}

	Printf("%s is only available in a local single-player level (not in demos or netgames).\n", modeName);
	return false;
}
}

CUSTOM_CVAR(Int, cam_experimental_mode, ECM_Normal, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const int clampedMode = clamp<int>(self, ECM_Normal, ECM_TopDown);
	if (self != clampedMode)
	{
		self = clampedMode;
		return;
	}
	if (self != ECM_Normal && gamestate == GS_LEVEL && !IsExperimentalCameraAllowed())
	{
		Printf("Experimental cameras are unavailable in demos and netgames.\n");
		self = ECM_Normal;
		return;
	}
	ResetExperimentalCameras();
}

CUSTOM_CVAR(Float, cam_photo_speed, 20.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const float clampedSpeed = clamp<float>(self, 2.0f, 96.0f);
	if (self != clampedSpeed) self = clampedSpeed;
}

CUSTOM_CVAR(Float, cam_photo_fov, 75.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const float clampedFov = clamp<float>(self, 20.0f, 120.0f);
	if (self != clampedFov) self = clampedFov;
}

CVAR(Bool, cam_photo_showhud, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Retained for configuration compatibility with the superseded 3D top-down
// renderer. The native aim reticle uses the actual line trace instead.
CUSTOM_CVAR(Float, cam_topdown_aim_radius, 128.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const float clampedRadius = clamp<float>(self, 48.0f, 320.0f);
	if (self != clampedRadius) self = clampedRadius;
}

CVAR(Bool, cam_topdown_aim_guide, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

bool ECamera_IsPhotoModeActive()
{
	return cam_experimental_mode == ECM_Photo && IsExperimentalCameraAllowed();
}

bool ECamera_IsTopDownModeActive()
{
	return cam_experimental_mode == ECM_TopDown && IsExperimentalCameraAllowed();
}

bool ECamera_UsesTopDownDirectAim(const AActor* actor)
{
	return cam_topdown_direct_aim && ECamera_IsTopDownModeActive()
		&& actor != nullptr && actor == players[consoleplayer].mo;
}

bool ECamera_IsThirdPersonActive()
{
	return ECamera_IsPhotoModeActive();
}

bool ECamera_HidesHUD()
{
	return ECamera_IsPhotoModeActive() && !cam_photo_showhud;
}

double ECamera_GetFieldOfView(double normalFov)
{
	if (ECamera_IsPhotoModeActive())
	{
		return cam_photo_fov;
	}
	return normalFov;
}

void ECamera_ConsumeTiccmd(usercmd_t& cmd)
{
	if (ECamera_IsTopDownModeActive())
	{
		InitializeTopDownCamera();
		EnsureTopDownAutomap();
		if (!TopDownCamera.Initialized)
		{
			return;
		}

		AActor* const player = players[consoleplayer].mo;
		// The full-screen map is north-up, while mouse, right-stick, and gyro
		// look remain unmodified gameplay aim. They all reach this command before
		// the movement remap below, so Top-Down never needs a device-specific
		// control path.

		// Re-express movement in the player's basis after turning it into the
		// stable screen/map basis. W always means screen-up, S screen-down, and
		// A/D remain screen-left/right regardless of weapon facing.
		const DAngle playerYaw = GetPlayerYawForTopDownMovement(player, cmd);
		const DVector2 cameraForward = TopDownCamera.Yaw.ToVector();
		const DVector2 cameraRight = { cameraForward.Y, -cameraForward.X };
		const DVector2 requestedMove = cameraForward * double(cmd.forwardmove)
			+ cameraRight * double(cmd.sidemove);
		if (primaryLevel != nullptr && primaryLevel->automap != nullptr)
		{
			primaryLevel->automap->SetTopDownCameraPan(cam_topdown_freepan ? requestedMove : DVector2{});
		}

		if (cam_topdown_freepan)
		{
			// W/A/S/D becomes north-up camera movement while yaw remains live for aiming.
			cmd.forwardmove = cmd.sidemove = cmd.upmove = 0;
			return;
		}
		const DVector2 playerForward = playerYaw.ToVector();
		const DVector2 playerRight = { playerForward.Y, -playerForward.X };
		cmd.forwardmove = short(round(requestedMove | playerForward));
		cmd.sidemove = short(round(requestedMove | playerRight));
		return;
	}

	if (!ECamera_IsPhotoModeActive())
	{
		return;
	}

	InitializePhotoCameraFromPlayer();
	if (!PhotoCamera.Initialized)
	{
		return;
	}

	PhotoCamera.Angles.Yaw = (PhotoCamera.Angles.Yaw + DAngle::fromDeg(cmd.yaw * CmdToDegrees)).Normalized180();
	PhotoCamera.Angles.Pitch = DAngle::fromDeg(clamp<double>(
		PhotoCamera.Angles.Pitch.Degrees() - cmd.pitch * CmdToDegrees, -89.0, 89.0));

	const DVector2 forward2D = PhotoCamera.Angles.Yaw.ToVector();
	const double pitchCos = PhotoCamera.Angles.Pitch.Cos();
	const DVector3 forward = { forward2D.X * pitchCos, forward2D.Y * pitchCos, -PhotoCamera.Angles.Pitch.Sin() };
	// Doom's positive sidemove thrust is yaw - 90 degrees. Mirror that
	// convention so A/D and controller strafing keep their normal direction.
	const DVector3 right = { forward2D.Y, -forward2D.X, 0.0 };
	DVector3 movement = forward * double(cmd.forwardmove)
		+ right * double(cmd.sidemove)
		+ DVector3(0.0, 0.0, double(cmd.upmove));

	const double movementLength = movement.Length();
	if (movementLength > 0.0)
	{
		// A full run command is 50. Preserve the normal walk/run distinction
		// while preventing diagonal motion from being faster than straight motion.
		movement *= min<double>(1.0, 50.0 / movementLength);
		PhotoCamera.Position += movement * (double(cam_photo_speed) / 50.0);
		PhotoCamera.Position.X = clamp<double>(PhotoCamera.Position.X, -MAX_MAP_COORD, MAX_MAP_COORD);
		PhotoCamera.Position.Y = clamp<double>(PhotoCamera.Position.Y, -MAX_MAP_COORD, MAX_MAP_COORD);
		PhotoCamera.Position.Z = clamp<double>(PhotoCamera.Position.Z, -MAX_MAP_COORD, MAX_MAP_COORD);
	}

	// Keep the player completely inert. The camera remains a renderer-only
	// object, so no gameplay state or network command is changed by the mode.
	cmd.buttons = 0;
	cmd.pitch = cmd.yaw = cmd.roll = 0;
	cmd.forwardmove = cmd.sidemove = cmd.upmove = 0;
}

bool ECamera_ApplyView(FRenderViewpoint& viewPoint)
{
	if (!IsMainPlayerView(viewPoint))
	{
		return false;
	}

	if (ECamera_IsTopDownModeActive())
	{
		EnsureTopDownAutomap();
		return false;
	}

	if (ECamera_IsPhotoModeActive())
	{
		if (PhotoCamera.Level != viewPoint.ViewLevel)
		{
			PhotoCamera.Initialized = false;
		}
		InitializePhotoCameraFromView(viewPoint);
		viewPoint.Pos = PhotoCamera.Position;
		viewPoint.ActorPos = PhotoCamera.Position;
		viewPoint.Angles = PhotoCamera.Angles;
		viewPoint.sector = viewPoint.ViewLevel->PointInRenderSubsector(viewPoint.Pos)->sector;
		viewPoint.showviewer = true;
		viewPoint.bForceNoViewer = false;
		return true;
	}

	return false;
}

CCMD(togglephotomode)
{
	if (!CanToggleCamera("Photo Mode / Free Camera"))
	{
		return;
	}
	cam_experimental_mode = cam_experimental_mode == ECM_Photo ? ECM_Normal : ECM_Photo;
	Printf("Photo Mode / Free Camera %s\n", cam_experimental_mode == ECM_Photo ? "enabled" : "disabled");
}

CCMD(toggletopdowncamera)
{
	if (!CanToggleCamera("Top-Down Camera"))
	{
		return;
	}
	cam_experimental_mode = cam_experimental_mode == ECM_TopDown ? ECM_Normal : ECM_TopDown;
	Printf("Top-Down Camera %s\n", cam_experimental_mode == ECM_TopDown ? "enabled" : "disabled");
}

CCMD(toggletopdownpan)
{
	if (!ECamera_IsTopDownModeActive())
	{
		Printf("Top-Down Free Pan is available only while Top-Down Camera is active.\n");
		return;
	}

	cam_topdown_freepan = !cam_topdown_freepan;
	if (!cam_topdown_freepan && primaryLevel != nullptr && primaryLevel->automap != nullptr)
	{
		primaryLevel->automap->SetTopDownCameraPan({});
	}
	Printf("Top-Down Free Pan %s\n", cam_topdown_freepan ? "enabled (W/A/S/D move the camera)" : "disabled (following player)");
}
