#pragma once

struct FRenderViewpoint;
struct usercmd_t;
class AActor;

// Local-only experimental camera modes. Photo Mode is presentation-only;
// Top-Down maps local gameplay input to its visible cursor. Both are disabled
// for demos and netgames.
bool ECamera_IsPhotoModeActive();
bool ECamera_IsTopDownModeActive();
// Top-Down is a planar gameplay mode. The local pawn therefore uses the
// classic vertical autoaim model: its view pitch stays level while weapons
// acquire vertical slope to targets at the selected horizontal heading.
bool ECamera_UsesTopDownClassicAim(const AActor* actor);
// True only for the local player while the opt-in Top-Down pointer aim model
// is active. Weapon helpers use this to keep their shot ray on that cursor.
bool ECamera_UsesTopDownDirectAim(const AActor* actor);
// Raw mouse/controller inputs are routed here while Top-Down pointer aim is
// active. A true return means the normal first-person look path should not
// consume that input too.
bool ECamera_AddTopDownMouseMotion(float right, float down);
bool ECamera_AddTopDownControllerMotion(float right, float down);
bool ECamera_AddTopDownGyroMotion(float rightDegrees, float downDegrees);
bool ECamera_IsThirdPersonActive();
bool ECamera_HidesHUD();

// Applies the selected experimental camera after the normal view has been
// interpolated. Returns true when it replaced the regular player view.
bool ECamera_ApplyView(FRenderViewpoint& viewPoint);

// Consumes local input while the free camera is active. This is called after a
// complete usercmd is assembled, before it can enter player simulation.
void ECamera_ConsumeTiccmd(usercmd_t& cmd);

// Lets photo mode supply its framing FOV without changing the player's
// persistent FOV preference.
double ECamera_GetFieldOfView(double normalFov);
