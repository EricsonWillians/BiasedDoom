// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2003-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//

#include "doomtype.h"
#include "g_level.h"
#include "filesystem.h"
#include "r_state.h"
#include "r_utility.h"
#include "g_levellocals.h"
#include "hw_skydome.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_renderstate.h"
#include "skyboxtexture.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------
void HWSkyPortal::DrawContents(HWDrawInfo *di, FRenderState &state)
{
	bool drawBoth = false;
	auto &vp = di->Viewpoint;

	// We have no use for Doom lighting special handling here, so disable it for this function.
	auto oldlightmode = di->lightmode;
	if (isSoftwareLighting(oldlightmode))
	{
		di->SetFallbackLightMode();
	}
	// The sky is at infinity: it must never be darkened by the distance-based
	// lighting equation. This applies to every light mode, not just the
	// software ones, otherwise the dome inherits a stale sector light level
	// and turns almost black (most visible with a sky ceiling on huge maps).
	state.SetNoSoftLightLevel();


	state.ResetColor();
	state.EnableFog(false);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.SetRenderStyle(STYLE_Translucent);
	bool oldClamp = state.SetDepthClamp(true);

	// The sky dome/box is renderer-generated geometry, not a sector surface:
	// sampling the world-space bleed map across it paints sector-colored
	// vertical bands over the sky, so suppress the bleed for this pass. The
	// uniform block is re-uploaded by SetupView, and restored below so later
	// passes are unaffected.
	const float bleedStrength = di->VPUniforms.mSectorBleedParams.X;
	di->VPUniforms.mSectorBleedParams.X = 0.f;

	di->SetupView(state, 0, 0, 0, !!(mState->MirrorFlag & 1), !!(mState->PlaneMirrorFlag & 1));

	state.SetVertexBuffer(vertexBuffer);
	auto skybox = origin->texture[0] ? dynamic_cast<FSkyBox*>(origin->texture[0]->GetTexture()) : nullptr;
	if (skybox)
	{
		vertexBuffer->RenderBox(state, skybox, origin->x_offset[0], origin->sky2, di->Level->info->pixelstretch, di->Level->info->skyrotatevector, di->Level->info->skyrotatevector2);
	}
	else
	{
		if (origin->texture[0]==origin->texture[1] && origin->doublesky) origin->doublesky=false;	

		if (origin->texture[0])
		{
			state.SetTextureMode(TM_OPAQUE);
			vertexBuffer->RenderDome(state, origin->texture[0], origin->x_offset[0], origin->y_offset, origin->mirrored, FSkyVertexBuffer::SKYMODE_MAINLAYER, !!(di->Level->flags & LEVEL_FORCETILEDSKY));
			state.SetTextureMode(TM_NORMAL);
		}

		state.AlphaFunc(Alpha_Greater, 0.f);
		
		if (origin->doublesky && origin->texture[1])
		{
			vertexBuffer->RenderDome(state, origin->texture[1], origin->x_offset[1], origin->y_offset, false, FSkyVertexBuffer::SKYMODE_SECONDLAYER, !!(di->Level->flags & LEVEL_FORCETILEDSKY));
		}
	}

	if (origin->fogalpha > 0)
	{
		PalEntry FadeColor = origin->fadecolor;
		FadeColor.a = origin->fogalpha;

		if (di->Level->flags3 & LEVEL3_SKYMIST && origin->texture[2])
		{
			vertexBuffer->RenderDome(state, origin->texture[2], origin->x_offset[2], 0.f, false, FSkyVertexBuffer::SKYMODE_FOGLAYER, !!(di->Level->flags & LEVEL_FORCETILEDSKY), 0, 0, FadeColor);
		}
		else
		{
			state.EnableTexture(false);
			if (bd_fog_quality > 0 && bd_fog_sky_horizon > 0.0f)
				vertexBuffer->RenderFogDome(state, FadeColor, bd_fog_sky_horizon);
			else
			{
				state.SetObjectColor(FadeColor);
				state.Draw(DT_Triangles, 0, 12);
			}
			state.EnableTexture(true);
			state.SetObjectColor(0xffffffff);
		}
	}
	di->VPUniforms.mSectorBleedParams.X = bleedStrength;
	di->lightmode = oldlightmode;
	state.SetDepthClamp(oldClamp);
}

const char *HWSkyPortal::GetName() { return "Sky"; }
