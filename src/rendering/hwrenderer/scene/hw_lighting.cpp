// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2018 Christoph Oelckers
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
/*
** gl_light.cpp
** Light level / fog management / dynamic lights
**
**/

#include "c_cvars.h"
#include "r_sky.h"
#include "g_levellocals.h"
#include "hw_lighting.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"

// externally settable lighting properties
static float distfogtable[2][256];	// light to fog conversion table for black fog

CVAR(Int, gl_weaponlight, 8, CVAR_ARCHIVE);
CVAR(Bool, gl_enhanced_nightvision, true, CVAR_ARCHIVE|CVAR_NOINITCALL)

bool IsBiasedGlobalFogActive()
{
	return bd_fog_mode == 1 && bd_fog_density > 0.0f && bd_sector_fog_scale > 0.0f;
}

// Distance-based visibility (light diminishing and biased fog) is tuned for
// classically sized maps. On very large maps everything lies beyond the fog
// and light falloff range and renders solid black, so scale those ranges with
// the map's bounding box diagonal. Returns 1.0 for normally sized maps.
float BiasedVisibilityScale(FLevelLocals *Level)
{
	static const FLevelLocals *cachedLevel = nullptr;
	static bool cachedEnabled = false;
	static float cachedReference = -1.0f;
	static float cachedMax = -1.0f;
	static float cachedScale = 1.0f;

	const bool enabled = bd_vis_autoscale;
	const float reference = (float)bd_vis_autoscale_reference;
	const float maxscale = (float)bd_vis_autoscale_max;
	if (Level == cachedLevel && enabled == cachedEnabled && reference == cachedReference && maxscale == cachedMax)
	{
		return cachedScale;
	}

	float scale = 1.0f;
	if (enabled && reference > 0.0f && Level != nullptr && Level->vertexes.Size() > 1)
	{
		double minx = Level->vertexes[0].p.X, maxx = minx;
		double miny = Level->vertexes[0].p.Y, maxy = miny;
		for (auto &v : Level->vertexes)
		{
			minx = min(minx, v.p.X);
			maxx = max(maxx, v.p.X);
			miny = min(miny, v.p.Y);
			maxy = max(maxy, v.p.Y);
		}
		const double diagonal = sqrt((maxx - minx) * (maxx - minx) + (maxy - miny) * (maxy - miny));
		scale = (float)clamp(diagonal / reference, 1.0, (double)maxscale);
	}

	cachedLevel = Level;
	cachedEnabled = enabled;
	cachedReference = reference;
	cachedMax = maxscale;
	cachedScale = scale;
	return scale;
}

static uint8_t BlendFogChannel(uint8_t from, uint8_t to, float amount)
{
	return (uint8_t)clamp<int>((int)(from + (to - from) * amount + 0.5f), 0, 255);
}

PalEntry GetBiasedFogColor(PalEntry fogcolor, bool forcecustomcolor)
{
	if (bd_fog_mode == 0) return fogcolor;

	PalEntry custom((uint32_t)bd_fog_color & 0xffffff);
	custom.a = 0;
	const bool hasmapcolor = (fogcolor.d & 0xffffff) != 0;

	if (forcecustomcolor || bd_fog_color_mode == 1 || (bd_fog_mode == 1 && !hasmapcolor))
	{
		return custom;
	}

	if (bd_fog_color_mode == 2 && hasmapcolor)
	{
		float amount = clamp<float>(bd_fog_color_strength, 0.0f, 1.0f);
		return PalEntry(0,
			BlendFogChannel(fogcolor.r, custom.r, amount),
			BlendFogChannel(fogcolor.g, custom.g, amount),
			BlendFogChannel(fogcolor.b, custom.b, amount));
	}

	return (bd_fog_mode == 1) ? custom : fogcolor;
}

//==========================================================================
//
// Sets up the fog tables
//
//==========================================================================

CUSTOM_CVAR(Int, gl_distfog, 70, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	for (int i = 0; i < 256; i++)
	{

		if (i < 164)
		{
			distfogtable[0][i] = (float)((gl_distfog >> 1) + (gl_distfog)*(164 - i) / 164);
		}
		else if (i < 230)
		{
			distfogtable[0][i] = (float)((gl_distfog >> 1) - (gl_distfog >> 1)*(i - 164) / (230 - 164));
		}
		else distfogtable[0][i] = 0;

		if (i < 128)
		{
			distfogtable[1][i] = 6.f + (float)((gl_distfog >> 1) + (gl_distfog)*(128 - i) / 48);
		}
		else if (i < 216)
		{
			distfogtable[1][i] = (216.f - i) / ((216.f - 128.f)) * gl_distfog / 10;
		}
		else distfogtable[1][i] = 0;
	}
}

//==========================================================================
//
// Get current light level
//
//==========================================================================

int CalcLightLevel(ELightMode lightmode, int lightlevel, int rellight, bool weapon, int blendfactor, bool weaponPureLightLevel)
{
	int light;

	if (lightlevel <= 0) return 0;

	bool darklightmode = (isDarkLightMode(lightmode)) || (isSoftwareLighting(lightmode) && blendfactor > 0);

	if ((darklightmode && lightlevel < 192 && !weapon) || (weapon && weaponPureLightLevel))
	{
		if (lightlevel > 100)
		{
			light = xs_CRoundToInt(192.f - (192 - lightlevel)* 1.87f);
			if (light + rellight < 20)
			{
				light = 20 + (light + rellight - 20) / 5;
			}
			else
			{
				light += rellight;
			}
		}
		else
		{
			light = (lightlevel + rellight) / 5;
		}

	}
	else
	{
		light=lightlevel+rellight;
	}

	// Fake contrast should never turn a positive value into 0.
	return clamp(light, 1, 255);
}

//==========================================================================
//
// Get current light color
//
//==========================================================================

PalEntry CalcLightColor(ELightMode lightmode, int light, PalEntry pe, int blendfactor)
{
	int r,g,b;

	if (blendfactor == 0)
	{
		if (isSoftwareLighting(lightmode))
		{
			return pe;
		}

		r = pe.r * light / 255;
		g = pe.g * light / 255;
		b = pe.b * light / 255;
	}
	else
	{
		// This is what Legacy does with colored light in 3D volumes. No, it doesn't really make sense...
		// It also doesn't translate well to software style lighting.
		int mixlight = light * (255 - blendfactor);

		r = (mixlight + pe.r * blendfactor) / 255;
		g = (mixlight + pe.g * blendfactor) / 255;
		b = (mixlight + pe.b * blendfactor) / 255;
	}
	return PalEntry(255, uint8_t(r), uint8_t(g), uint8_t(b));
}

//==========================================================================
//
// calculates the current fog density
//
//	Rules for fog:
//
//  1. If bit 4 of gl_lightmode is set always use the level's fog density. 
//     This is what Legacy's GL render does.
//	2. black fog means no fog and always uses the distfogtable based on the level's fog density setting
//	3. If outside fog is defined and the current fog color is the same as the outside fog
//	   the engine always uses the outside fog density to make the fog uniform across the Level->
//	   If the outside fog's density is undefined it uses the level's fog density and if that is
//	   not defined it uses a default of 70.
//	4. If a global fog density is specified it is being used for all fog on the level
//	5. If none of the above apply fog density is based on the light level as for the software renderer.
//
//==========================================================================

float GetFogDensity(FLevelLocals* Level, ELightMode lightmode, int lightlevel, PalEntry fogcolor, int sectorfogdensity, int blendfactor)
{
	float density;

	auto oldlightmode = lightmode;
	if (isSoftwareLighting(lightmode) && blendfactor > 0) lightmode = ELightMode::Doom;	// The blendfactor feature does not work with software-style lighting.

	if (lightmode == ELightMode::DoomLegacy)
	{
		// uses approximations of Legacy's default settings.
		density = Level->fogdensity ? (float)Level->fogdensity : 18;
	}
	else if (sectorfogdensity != 0)
	{
		// case 1: Sector has an explicit fog density set.
		density = (float)sectorfogdensity;
	}
	else if ((fogcolor.d & 0xffffff) == 0)
	{
		// case 2: black fog
		if ((!isDoomSoftwareLighting(lightmode) || blendfactor > 0) && !(Level->flags3 & LEVEL3_NOLIGHTFADE))
		{
			density = distfogtable[lightmode != ELightMode::LinearStandard][hw_ClampLight(lightlevel)];
		}
		else
		{
			density = 0;
		}
	}
	else if (Level->outsidefogdensity != 0 && APART(Level->info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (Level->info->outsidefog & 0xffffff))
	{
		// case 3. outsidefogdensity has already been set as needed
		density = (float)Level->outsidefogdensity;
	}
	else  if (Level->fogdensity != 0)
	{
		// case 4: level has fog density set
		density = (float)Level->fogdensity;
	}
	else if (lightlevel < 248)
	{
		// case 5: use light level
		density = (float)clamp<int>(255 - lightlevel, 30, 255);
	}
	else
	{
		density = 0.f;
	}

	if (bd_fog_mode == 2)
	{
		float scale = max(0.0f, (float)bd_sector_fog_scale);
		if (scale <= 0.0f)
		{
			return 0.0f;
		}

		float darkness = clamp<float>((255.0f - lightlevel) / 255.0f, 0.0f, 1.0f);
		float sectorFloor = max(0.0f, (float)bd_fog_density) * (0.55f + darkness * 0.45f);
		density = max(density, sectorFloor);
		density *= scale;
	}

	if (IsBiasedGlobalFogActive())
	{
		density = max(density, max(0.0f, (float)bd_fog_density));
		density *= max(0.0f, (float)bd_sector_fog_scale);
	}

	// Distance-based light diminishing and fog are tuned for classically sized
	// maps: on very large maps everything lies beyond the falloff range and the
	// far field renders solid black, hiding both the architecture and the sky
	// above it. Scale every density path with the map's size, not just the
	// biased-fog ones; this is a no-op for normally sized maps. Scaling last is
	// equivalent to scaling each contribution since the scale is positive.
	density /= BiasedVisibilityScale(Level);

	// Script-set densities (ACS/FraggleScript/ZScript setters) and a negative
	// gl_distfog bypass the MAPINFO/UDMF parse clamps; when no biased-fog path
	// above floored them, a negative density would reach FRenderState::SetFog
	// (which ignores d < 0, leaving a stale density) and exp() in the fuzz
	// sprite path. Negative fog has no meaning, so clamp it to "no fog".
	return max(density, 0.0f);
}

//==========================================================================
//
// Check if the current linedef is a candidate for a fog boundary
//
// Requirements for a fog boundary:
// - front sector has no fog
// - back sector has fog
// - at least one of both does not have a sky ceiling.
//
//==========================================================================

bool CheckFog(FLevelLocals* Level, sector_t *frontsector, sector_t *backsector, ELightMode lightmode)
{
	if (frontsector == backsector) return false;	// there can't be a boundary if both sides are in the same sector.
	if (IsBiasedGlobalFogActive()) return false;

	// Check for fog boundaries. This needs a few more checks for the sectors

	PalEntry fogcolor = frontsector->Colormap.FadeColor;

	if ((fogcolor.d & 0xffffff) == 0)
	{
		return false;
	}
	else if (fogcolor.a != 0)
	{
	}
	else if (Level->outsidefogdensity != 0 && APART(Level->info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (Level->info->outsidefog & 0xffffff))
	{
	}
	else  if (Level->fogdensity!=0 || lightmode == ELightMode::DoomLegacy)
	{
		// case 3: level has fog density set
	}
	else 
	{
		// case 4: use light level
		if (frontsector->lightlevel >= 248) return false;
	}

	fogcolor = backsector->Colormap.FadeColor;

	if ((fogcolor.d & 0xffffff) == 0)
	{
	}
	else if (Level->outsidefogdensity != 0 && APART(Level->info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (Level->info->outsidefog & 0xffffff))
	{
		return false;
	}
	else  if (Level->fogdensity!=0 || lightmode == ELightMode::DoomLegacy)
	{
		// case 3: level has fog density set
		return false;
	}
	else 
	{
		// case 4: use light level
		if (backsector->lightlevel < 248) return false;
	}

	// in all other cases this might create more problems than it solves.
	return ((frontsector->GetTexture(sector_t::ceiling)!=skyflatnum || 
			 backsector->GetTexture(sector_t::ceiling)!=skyflatnum));
}
