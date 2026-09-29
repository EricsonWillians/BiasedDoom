#pragma once

#include <math.h>
#include "matrix.h"
#include "hwrenderer/data/buffers.h"
#include "hw_renderstate.h"
#include "skyboxtexture.h"

class FGameTexture;
class FRenderState;
class IVertexBuffer;
struct HWSkyPortal;
struct HWDrawInfo;

// 57 world units roughly represent one sky texel for the glTranslate call.
const int skyoffsetfactor = 57;

// Atmospheric sky-fog parameters. The sky is geometry at infinite distance, so
// its fog veil is the limit of the same exponential fog model the level
// geometry uses (see FSkyVertexBuffer::UpdateFogDomeGradient): per-elevation
// transmittance to infinity through the exponential height fog, floored by the
// same minimum-visibility term, tinted by the same spatial gradient, and
// floored again by the map-authored 'skyfog' flat veil. Pure CPU-side data,
// POD so HWSkyInfo can stay memset/memcmp-clean.
struct FSkyFogParams
{
	PalEntry fogColor;         // resolved fog color (map fade or biased override/blend)
	PalEntry gradientColor;    // spatial gradient tint target (bd_fog_gradient_color)
	FVector3 gradientDir;      // normalized direction for directional gradients
	float sigma;               // effective density in shader units: density * (-log2(e) / 64000), <= 0
	float heightK;             // height falloff coefficient: bd_fog_height_falloff / 256
	float minVisibility;       // fog factor floor (bd_fog_min_visibility)
	float strength;            // master sky fog dial (bd_fog_sky_strength); 0 disables the analytic veil
	float gradientStrength;    // bd_fog_gradient_strength
	float gradientScale;       // bd_fog_gradient_scale
	float gradientMode;        // 0 = off, 1 = vertical, 2 = directional
	uint8_t flatAlpha;         // MAPINFO 'skyfog' flat veil floor

	bool IsActive() const { return flatAlpha > 0 || strength > 0.0f; }

	// Transmittance to infinity along a sky ray at the given elevation sine
	// through the exponential height fog: the d -> infinity limit of the
	// shader's optical depth sigma * d * heightIntegral(k*d*sinE) is
	// sigma / (k * ln2 * sinE). Approaches 0 at the horizon, so the veil
	// coverage there exactly equals the saturation coverage of far geometry.
	float Transmittance(float sinElev) const
	{
		if (sigma >= 0.0f) return 1.0f;
		const float LN2 = 0.6931471805599453f;
		const float s = fabsf(sinElev);
		if (s < 0.001f) return 0.0f;
		const float k = heightK > 0.0001f ? heightK : 0.0001f;
		const float od = sigma / (k * LN2 * s);  // sigma is negative
		return od < -64.0f ? 0.0f : exp2f(od);
	}

	// Veil coverage (alpha in 0..1) at the given elevation sine, including the
	// minimum-visibility floor and the master strength dial, but not 'skyfog'.
	float Coverage(float sinElev) const
	{
		const float t = Transmittance(sinElev);
		const float fogfactor = minVisibility + (1.0f - minVisibility) * t;
		return (1.0f - fogfactor) * strength;
	}

	// Representative coverage for the flat fallback veil and the skymist layer,
	// evaluated at 45 degrees elevation, with the 'skyfog' floor applied.
	uint8_t UniformAlpha() const
	{
		const int a = (int)(Coverage(0.70710678f) * 255.0f + 0.5f);
		const int clamped = a < 0 ? 0 : (a > 255 ? 255 : a);
		return (uint8_t)(flatAlpha > clamped ? flatAlpha : clamped);
	}

	bool operator==(const FSkyFogParams &o) const { return !memcmp(this, &o, sizeof(*this)); }
	bool operator!=(const FSkyFogParams &o) const { return !!memcmp(this, &o, sizeof(*this)); }
};

struct FSkyVertex
{
	float x, y, z, u, v, lu, lv, lindex;
	PalEntry color;

	void Set(float xx, float zz, float yy, float uu=0, float vv=0, PalEntry col=0xffffffff)
	{
		x = xx;
		z = zz;
		y = yy;
		u = uu;
		v = vv;
		lu = 0.0f;
		lv = 0.0f;
		lindex = -1.0f;
		color = col;
	}

	void SetXYZ(float xx, float yy, float zz, float uu = 0, float vv = 0, PalEntry col = 0xffffffff)
	{
		x = xx;
		y = yy;
		z = zz;
		u = uu;
		v = vv;
		lu = 0.0f;
		lv = 0.0f;
		lindex = -1.0f;
		color = col;
	}

};

class FSkyVertexBuffer
{
	friend struct HWSkyPortal;
public:
	static const int SKYHEMI_UPPER = 1;
	static const int SKYHEMI_LOWER = 2;

	enum
	{
		SKYMODE_MAINLAYER = 0,
		SKYMODE_SECONDLAYER = 1,
		SKYMODE_FOGLAYER = 2
	};

	IVertexBuffer *mVertexBuffer;

	TArray<FSkyVertex> mVertices;
	TArray<unsigned int> mPrimStartDoom;
	TArray<unsigned int> mPrimStartBuild;
	TArray<unsigned int> mPrimStartFog;
	TArray<float> mFogElevation;

	int mRows, mColumns;
	int mFogRows = 32;
	unsigned int mFogVertexStart = 0;
	FSkyFogParams mFogParams;
	bool mFogParamsValid = false;
	FTextureID mCapGradientTex;

	// indices for sky cubemap faces
	int mFaceStart[7];
	int mSideStart;

	void SkyVertexDoom(int r, int c, bool yflip);
	void SkyVertexBuild(int r, int c, bool yflip);
	void FogVertexDoom(int r, int c, bool yflip);
	void CreateSkyHemisphereDoom(int hemi);
	void CreateSkyHemisphereBuild(int hemi);
	void CreateFogHemisphere(int hemi);
	void UpdateFogDomeGradient(const FSkyFogParams &params);
	void UpdateCapGradient(const TArray<PalEntry>& upper, const TArray<PalEntry>& lower);
	void CreateDome();

public:

	FSkyVertexBuffer();
	~FSkyVertexBuffer();
	void SetupMatrices(FGameTexture *tex, float x_offset, float y_offset, bool mirror, int mode, VSMatrix &modelmatrix, VSMatrix &textureMatrix, bool tiled, float xscale = 0, float vertscale = 0);
	std::pair<IVertexBuffer *, IIndexBuffer *> GetBufferObjects() const
	{
		return std::make_pair(mVertexBuffer, nullptr);
	}

	int FaceStart(int i)
	{
		if (i >= 0 && i < 7) return mFaceStart[i];
		else return mSideStart;
	}

	void RenderRow(FRenderState& state, EDrawType prim, int row, TArray<unsigned int>& mPrimStart, bool apply = true);
	void DoRenderDome(FRenderState& state, FGameTexture* tex, int mode, bool which, PalEntry color = 0xffffffff);
	void RenderDome(FRenderState& state, FGameTexture* tex, float x_offset, float y_offset, bool mirror, int mode, bool tiled, float xscale = 0, float yscale = 0, PalEntry color = 0xffffffff);
	void RenderFogDome(FRenderState& state, const FSkyFogParams &params);
	void RenderBox(FRenderState& state, FSkyBox* tex, float x_offset, bool sky2, float stretch, const FVector3& skyrotatevector, const FVector3& skyrotatevector2, PalEntry color = 0xffffffff);

};
