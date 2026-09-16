#pragma once

#include "matrix.h"

struct HWDrawInfo;

enum class ELightBlendMode : uint8_t
{
	CLAMP = 0,
	CLAMP_COLOR = 1,
	NOCLAMP = 2,

	DEFAULT = CLAMP,
};

struct HWViewpointUniforms
{
	VSMatrix mProjectionMatrix;
	VSMatrix mViewMatrix;
	VSMatrix mNormalViewMatrix;
	FVector4 mCameraPos;
	FVector4 mClipLine;

	float mGlobVis = 1.f;
	int mPalLightLevels = 0;
	int mViewHeight = 0;
	float mClipHeight = 0.f;
	float mClipHeightDirection = 0.f;
	int mShadowmapFilter = 1;

	int mLightBlendMode = 0;

	float mThickFogDistance = -1.f;
	float mThickFogMultiplier = 30.f;
	int mDynLightFalloffMode = 0;
	float mDynLightFalloffExponent = 2.f;
	float mDynLightIntensity = 1.f;
	float mDynLightSaturation = 1.f;
	float mLightTemperature = 0.f;
	float mLightAmbientFloor = 0.f;
	float mLightSpecularScale = 1.f;
	float mDynLightRangeScale = 1.f;
	float mDynLightFalloffSoftness = 0.f;
	float mDynLightWrap = 0.f;
	float mDynLightIndirect = 0.f;
	float mDynLightShadowStrength = 1.f;
	float mEmissiveBoost = 0.f;
	float mGIAmbientStrength = 0.f;
	float mLightStylePadding = 0.f;
	FVector4 mFogGradientColor = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mFogGradientDirection = { 0.f, 1.f, 0.f, 0.f };
	// quality, relative height falloff, turbulence strength, turbulence scale
	FVector4 mFogQuality = { 0.f, 0.f, 0.f, 0.f };
	// Lower bound for the fog visibility factor so distant geometry can never
	// be swallowed completely by fog.
	float mFogMinVisibility = 0.f;
	float mLightContrast = 1.f;
	float mSpecularPowerScale = 1.f;
	float mRimLightStrength = 0.f;
	float mRimLightPower = 3.f;
	float mAmbientGradientStrength = 0.f;
	float mLightStylePadding2 = 0.f;
	float mLightStylePadding3 = 0.f;
	FVector4 mAmbientGradientColor = { 0.f, 0.f, 0.f, 0.f };
	float mSceneTime = 0.f;
	float mDynLightFlicker = 0.f;
	float mAerialStrength = 0.f;
	float mAerialDistance = 2048.f;
	FVector4 mSpecularTintColor = { 1.f, 1.f, 1.f, 0.f };
	// World-space sector-light bleed map: minimum XY and reciprocal extent.
	FVector4 mSectorBleedBounds = { 0.f, 0.f, 0.f, 0.f };
	// Blend strength in X; the remaining components are reserved.
	FVector4 mSectorBleedParams = { 0.f, 0.f, 0.f, 0.f };

	void CalcDependencies()
	{
		mNormalViewMatrix.computeNormalMatrix(mViewMatrix);
	}
};
