#pragma once

struct FRenderViewpoint;
struct usercmd_t;

// Local-only presentation cameras. They deliberately never create or move an
// actor, so enabling one cannot affect saves, demos, or network simulation.
bool ECamera_IsPhotoModeActive();
bool ECamera_IsTopDownModeActive();
bool ECamera_IsThirdPersonActive();
bool ECamera_HidesHUD();

// Returns the pixel offset for the top-down aim marker. It is calculated from
// the player's gameplay yaw relative to the stable top-down camera yaw.
bool ECamera_GetTopDownAimOffset(double radius, double &x, double &y);

// Applies the selected experimental camera after the normal view has been
// interpolated. Returns true when it replaced the regular player view.
bool ECamera_ApplyView(FRenderViewpoint& viewPoint);

// Consumes local input while the free camera is active. This is called after a
// complete usercmd is assembled, before it can enter player simulation.
void ECamera_ConsumeTiccmd(usercmd_t& cmd);

// Lets photo and top-down modes supply their framing FOV without changing the
// player's persistent FOV preference.
double ECamera_GetFieldOfView(double normalFov);
