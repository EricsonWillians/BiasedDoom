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
** gl_dynlight1.cpp
** dynamic light application
**
**/

#include <algorithm>

#include "actorinlines.h"
#include "a_dynlight.h"
#include "hw_dynlightdata.h"
#include"hw_cvars.h"
#include "v_video.h"
#include "r_utility.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"

// If we want to share the array to avoid constant allocations it needs to be thread local unless it'd be littered with expensive synchronization.
thread_local FDynLightData lightdata;

//==========================================================================
//
// Light related CVARs
//
//==========================================================================

// These shouldn't be called 'gl...' anymore...
CVAR (Bool, gl_light_sprites, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR (Bool, gl_light_particles, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);


//==========================================================================
//
// Performance culling: skip lights too far away from the current viewpoint.
// Uses squared distances so no square root is needed per light.
//
//==========================================================================
static bool LightBeyondCullDistance(const DVector3 &pos)
{
	if (bd_dynlight_cull_distance <= 0.0f) return false;
	double maxdist = (double)bd_dynlight_cull_distance;
	DVector3 delta = pos - r_viewpoint.Pos;
	return delta.LengthSquared() > maxdist * maxdist;
}

//==========================================================================
//
// Sets up the parameters to render one dynamic light onto one plane
//
//==========================================================================
bool GetLight(FDynLightData& dld, int group, Plane & p, FDynamicLight * light, bool checkside)
{
	DVector3 pos = light->PosRelative(group);
	float radius = (light->GetRadius());

	auto dist = fabs(p.DistToPoint((float)pos.X, (float)pos.Z, (float)pos.Y));

	if (radius <= 0.f) return false;
	if (dist > radius) return false;
	if (LightBeyondCullDistance(pos)) return false;
	if (checkside && p.PointOnSide((float)pos.X, (float)pos.Z, (float)pos.Y))
	{
		return false;
	}

	AddLightToList(dld, group, light, false);
	return true;
}

//==========================================================================
//
// Add one dynamic light to the light data list
//
//==========================================================================
void AddLightToList(FDynLightData &dld, int group, FDynamicLight * light, bool forceAttenuate)
{
	int i = 0;

	DVector3 pos = light->PosRelative(group);
	float radius = light->GetRadius();

	if (LightBeyondCullDistance(pos)) return;

	float cs;
	if (light->IsAdditive()) 
	{
		cs = 0.2f;
		i = 2;
	}
	else 
	{
		cs = 1.0f;
	}

	if (light->target && (light->target->renderflags2 & RF2_LIGHTMULTALPHA))
		cs *= (float)light->target->Alpha;

	// Multiply intensity from GLDEFS
	cs *= (float)light->GetLightDefIntensity();

	float r = light->GetRed() / 255.0f * cs;
	float g = light->GetGreen() / 255.0f * cs;
	float b = light->GetBlue() / 255.0f * cs;

	if (light->IsSubtractive())
	{
		DVector3 v(r, g, b);
		float length = (float)v.Length();
		
		r = length - r;
		g = length - g;
		b = length - b;
		i = 1;
	}

	float shadowIndex;
	if (screen->mShadowMap.Enabled()) // note: with shadowmaps switched off, we cannot rely on properly set indices anymore.
	{
		shadowIndex = light->mShadowmapIndex + 1.0f;
	}
	else shadowIndex = 1025.f;
	// Store attenuate flag in the sign bit of the float.
	if (light->IsAttenuated() || forceAttenuate) shadowIndex = -shadowIndex;

	float lightType = 0.0f;
	float spotInnerAngle = 0.0f;
	float spotOuterAngle = 0.0f;
	float spotDirX = 0.0f;
	float spotDirY = 0.0f;
	float spotDirZ = 0.0f;
	if (light->IsSpot())
	{
		lightType = 1.0f;
		spotInnerAngle = (float)light->pSpotInnerAngle->Cos();
		spotOuterAngle = (float)light->pSpotOuterAngle->Cos();

		DAngle negPitch = -*light->pPitch;
		DAngle Angle = light->target->Angles.Yaw;
		double xzLen = negPitch.Cos();
		spotDirX = float(-Angle.Cos() * xzLen);
		spotDirY = float(-negPitch.Sin());
		spotDirZ = float(-Angle.Sin() * xzLen);
	}

	float *data = &dld.arrays[i][dld.arrays[i].Reserve(16)];
	data[0] = float(pos.X);
	data[1] = float(pos.Z);
	data[2] = float(pos.Y);
	data[3] = radius;
	data[4] = r;
	data[5] = g;
	data[6] = b;
	data[7] = shadowIndex;
	data[8] = spotDirX;
	data[9] = spotDirY;
	data[10] = spotDirZ;
	data[11] = lightType;
	data[12] = spotInnerAngle;
	data[13] = spotOuterAngle;
	data[14] = 0.0f; // unused
	data[15] = 0.0f; // unused
}

//==========================================================================
//
// Truncates each light group to the maxLights strongest entries.
// Strength is estimated from the stored light record: radius first
// (data[3], larger reach usually means a stronger contribution), then
// color intensity (data[4..6]) as tie breaker.
//
//==========================================================================
void FDynLightData::LimitPerSurface(int maxLights)
{
	if (maxLights <= 0) return;

	for (int group = 0; group < 3; group++)
	{
		auto &arr = arrays[group];
		int count = (int)arr.Size() / 16;
		if (count <= maxLights) continue;

		TArray<int> order(count, true);
		for (int i = 0; i < count; i++) order[i] = i;
		std::sort(order.begin(), order.end(), [&](int a, int b)
		{
			float ra = arr[a * 16 + 3];
			float rb = arr[b * 16 + 3];
			if (ra != rb) return ra > rb;
			float ia = arr[a * 16 + 4] + arr[a * 16 + 5] + arr[a * 16 + 6];
			float ib = arr[b * 16 + 4] + arr[b * 16 + 5] + arr[b * 16 + 6];
			return ia > ib;
		});

		TArray<float> kept(maxLights * 16, true);
		for (int i = 0; i < maxLights; i++)
			memcpy(&kept[i * 16], &arr[order[i] * 16], 16 * sizeof(float));
		arr = std::move(kept);
	}
}

