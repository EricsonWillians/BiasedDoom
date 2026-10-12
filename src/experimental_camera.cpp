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
#include "d_player.h"
#include "d_protocol.h"
#include "doomdef.h"
#include "doomstat.h"
#include "g_levellocals.h"
#include "p_local.h"
#include "r_utility.h"

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
};

FTopDownCameraState TopDownCamera;

constexpr double CmdToDegrees = 360.0 / 65536.0;

bool IsExperimentalCameraAllowed()
{
	return gamestate == GS_LEVEL && !netgame && !demoplayback && !demorecording
		&& consoleplayer >= 0 && consoleplayer < int(MAXPLAYERS)
		&& players[consoleplayer].mo != nullptr;
}

void ResetExperimentalCameras()
{
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
	TopDownCamera.Yaw = players[consoleplayer].mo->Angles.Yaw;
	TopDownCamera.Initialized = true;
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
	if (self == ECM_TopDown && gamestate == GS_LEVEL && !V_IsHardwareRenderer())
	{
		Printf("Top-Down Camera requires a hardware renderer.\n");
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

CUSTOM_CVAR(Float, cam_topdown_height, 320.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const float clampedHeight = clamp<float>(self, 64.0f, 1024.0f);
	if (self != clampedHeight) self = clampedHeight;
}

CUSTOM_CVAR(Float, cam_topdown_aim_radius, 128.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const float clampedRadius = clamp<float>(self, 48.0f, 320.0f);
	if (self != clampedRadius) self = clampedRadius;
}

CVAR(Bool, cam_topdown_aim_guide, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, cam_topdown_fov, 85.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const float clampedFov = clamp<float>(self, 20.0f, 120.0f);
	if (self != clampedFov) self = clampedFov;
}

bool ECamera_IsPhotoModeActive()
{
	return cam_experimental_mode == ECM_Photo && IsExperimentalCameraAllowed();
}

bool ECamera_IsTopDownModeActive()
{
	// The engine's orthographic world renderer is hardware-only. Declining to
	// enter this mode in the software renderer is preferable to presenting a
	// misleading perspective fallback with broken ceiling and aiming behavior.
	return cam_experimental_mode == ECM_TopDown && V_IsHardwareRenderer()
		&& IsExperimentalCameraAllowed();
}

bool ECamera_IsThirdPersonActive()
{
	return ECamera_IsPhotoModeActive() || ECamera_IsTopDownModeActive();
}

bool ECamera_HidesHUD()
{
	return ECamera_IsPhotoModeActive() && !cam_photo_showhud;
}

bool ECamera_GetTopDownAimOffset(double radius, double &x, double &y)
{
	if (!ECamera_IsTopDownModeActive())
	{
		return false;
	}

	const DAngle playerYaw = players[consoleplayer].mo->Angles.Yaw;
	const DAngle cameraYaw = TopDownCamera.Initialized
		? TopDownCamera.Yaw
		: playerYaw;
	const double relativeYaw = deltaangle(cameraYaw, playerYaw).Radians();

	// GZDoom projects a point at camera yaw to screen-up and a positive
	// yaw delta to screen-left. Keep the marker in that same frame so its
	// movement is an exact visual representation of horizontal weapon aim.
	x = -sin(relativeYaw) * radius;
	y = -cos(relativeYaw) * radius;
	return true;
}

double ECamera_GetFieldOfView(double normalFov)
{
	if (ECamera_IsPhotoModeActive())
	{
		return cam_photo_fov;
	}
	if (ECamera_IsTopDownModeActive())
	{
		return cam_topdown_fov;
	}
	return normalFov;
}

void ECamera_ConsumeTiccmd(usercmd_t& cmd)
{
	if (ECamera_IsTopDownModeActive())
	{
		InitializeTopDownCamera();
		if (!TopDownCamera.Initialized)
		{
			return;
		}

		AActor* const player = players[consoleplayer].mo;
		// Top-down is deliberately a classic horizontal-aim mode. Mouse/stick
		// yaw still turns and aims the player, while pitch and roll are removed
		// before the command reaches simulation so vertical aim cannot desync
		// the reticle from weapon fire.
		player->Angles.Pitch = nullAngle;
		cmd.pitch = 0;
		cmd.roll = 0;

		// Re-express movement in the player's basis after turning it into the
		// stable screen/map basis. W always means screen-up, S screen-down, and
		// A/D remain screen-left/right regardless of weapon facing.
		const DAngle playerYaw = player->Angles.Yaw
			+ DAngle::fromDeg(double(cmd.yaw) * CmdToDegrees);
		const DVector2 cameraForward = TopDownCamera.Yaw.ToVector();
		const DVector2 cameraRight = { cameraForward.Y, -cameraForward.X };
		const DVector2 requestedMove = cameraForward * double(cmd.forwardmove)
			+ cameraRight * double(cmd.sidemove);
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

	if (ECamera_IsTopDownModeActive())
	{
		InitializeTopDownCamera();
		const DAngle cameraYaw = TopDownCamera.Initialized
			? TopDownCamera.Yaw
			: players[consoleplayer].mo->Angles.Yaw;

		// This is a genuine orthographic overhead view, not a high chase camera.
		// It follows the player's XY position from outside map geometry, so low
		// ceilings can neither push it down nor obscure the floor. The renderer's
		// orthographic path also selects visible planes by view direction, which
		// naturally hides ceiling flats while looking straight down.
		const DVector3 focus = viewPoint.ActorPos;
		const double scale = double(cam_topdown_height);
		viewPoint.Pos = focus + DVector3(0.0, 0.0, scale);
		viewPoint.sector = viewPoint.ViewLevel->PointInRenderSubsector(focus)->sector;
		viewPoint.Angles = { cameraYaw, DAngle::fromDeg(90.0), nullAngle };
		viewPoint.bDoOob = true;
		viewPoint.bDoOrtho = true;
		viewPoint.bExperimentalOrtho = true;
		viewPoint.OrthoFocus = focus;
		viewPoint.OrthoDistance = scale;
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
	if (!V_IsHardwareRenderer())
	{
		Printf("Top-Down Camera requires a hardware renderer.\n");
		return;
	}
	cam_experimental_mode = cam_experimental_mode == ECM_TopDown ? ECM_Normal : ECM_TopDown;
	Printf("Top-Down Camera %s\n", cam_experimental_mode == ECM_TopDown ? "enabled" : "disabled");
}
