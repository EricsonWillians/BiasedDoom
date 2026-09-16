#pragma once

#include "r_defs.h"
#include "m_fixed.h"
#include "hwrenderer/scene/hw_clipper.h"
#include "r_utility.h"
#include "c_cvars.h"
#include <memory>

class FWrapperTexture;
class DCanvas;

class SWSceneDrawer
{
	FTexture *PaletteTexture;
	std::unique_ptr<FGameTexture> FBTexture[2];
	int FBTextureIndex = 0;
	bool FBIsTruecolor = false;
	std::unique_ptr<DCanvas> Canvas;

public:
	SWSceneDrawer();
	~SWSceneDrawer();

	sector_t *RenderView(player_t *player);

	// Drops the cached framebuffer textures, e.g. after a video backend
	// switch invalidated the hardware textures they were created with.
	void ResetFBTextures()
	{
		FBTexture[0].reset();
		FBTexture[1].reset();
	}
};

// Resets the cached software scene framebuffer textures, if any.
// Implemented in hw_entrypoint.cpp, where the SWSceneDrawer instance lives.
void ResetSWSceneFBTextures();

