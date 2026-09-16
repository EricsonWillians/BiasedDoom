
varying vec4 vTexCoord;
varying vec4 vColor;
varying vec4 pixelpos;
varying vec3 glowdist;
varying vec3 gradientdist;
varying vec4 vWorldNormal;
varying vec4 vEyeNormal;

#ifdef NO_CLIPDISTANCE_SUPPORT
varying vec4 ClipDistanceA;
varying vec4 ClipDistanceB;
#endif


struct Material
{
	vec4 Base;
	vec4 Bright;
	vec4 Glow;
	vec3 Normal;
	vec3 Specular;
	float Glossiness;
	float SpecularLevel;
};

vec4 Process(vec4 color);
vec4 ProcessTexel();
Material ProcessMaterial(); // note that this is deprecated. Use SetupMaterial!
void SetupMaterial(inout Material mat);
vec4 ProcessLight(Material mat, vec4 color);
vec3 ProcessMaterialLight(Material material, vec3 color);
vec2 GetTexCoord();

// These get Or'ed into uTextureMode because it only uses its 3 lowermost bits.
//const int TEXF_Brightmap = 0x10000;
//const int TEXF_Detailmap = 0x20000;
//const int TEXF_Glowmap = 0x40000;


//===========================================================================
//
// Color to grayscale
//
//===========================================================================

float grayscale(vec4 color)
{
	return dot(color.rgb, vec3(0.3, 0.56, 0.14));
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 dodesaturate(vec4 texel, float factor)
{
	if (factor != 0.0)
	{
		float gray = grayscale(texel);
		return mix (texel, vec4(gray,gray,gray,texel.a), factor);
	}
	else
	{
		return texel;
	}
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 desaturate(vec4 texel)
{
#if (DEF_DO_DESATURATE == 1)
	return dodesaturate(texel, uDesaturationFactor);
#else
	return texel;
#endif
}

vec3 ApplyBiasedLightTemperature(vec3 light)
{
	float warm = clamp(uLightTemperature, 0.0, 1.0);
	float cool = clamp(-uLightTemperature, 0.0, 1.0);
	vec3 warmTint = mix(vec3(1.0), vec3(1.12, 1.04, 0.88), warm);
	vec3 coolTint = mix(vec3(1.0), vec3(0.86, 0.96, 1.14), cool);
	return light * warmTint * coolTint;
}

vec3 ApplyBiasedDynamicLight(vec3 light)
{
	vec3 styled = light * max(uDynLightIntensity, 0.0);
	float gray = dot(styled, vec3(0.3, 0.56, 0.14));
	styled = mix(vec3(gray), styled, max(uDynLightSaturation, 0.0));
	styled = max(ApplyBiasedLightTemperature(styled), vec3(0.0));
	// S-curve contrast above 1.0, softening lift below 1.0.
	float contrast = clamp(uLightContrast, 0.0, 2.0);
	styled = mix(styled, styled * styled * (3.0 - 2.0 * styled), clamp(contrast - 1.0, 0.0, 1.0));
	styled = mix(styled, sqrt(max(styled, vec3(0.0))), clamp(1.0 - contrast, 0.0, 1.0));
	// Per-light flicker: no light position reaches this point, so hash the
	// attenuation-invariant chroma of the incoming light instead.
	if (uDynLightFlicker > 0.0)
	{
		vec3 chroma = light / max(dot(light, vec3(1.0)), 0.0001);
		float n = fract(sin(dot(chroma.xy + chroma.z, vec2(12.9898, 78.233)) + floor(uSceneTime * 8.0) * 0.37) * 43758.5453);
		styled *= 1.0 - uDynLightFlicker * 0.4 * n;
	}
	return styled;
}

float BiasedLightRadius(float radius)
{
	return max(radius * max(uDynLightRangeScale, 0.05), 0.0001);
}

float BiasedLightAttenuation(float lightdistance, float lightradius)
{
	float radius = BiasedLightRadius(lightradius);
	if (lightdistance >= radius)
		return 0.0;

	float n = lightdistance / radius;
	float linear = clamp(1.0 - n, 0.0, 1.0);
	float softness = clamp(uDynLightFalloffSoftness, 0.0, 1.0);
	float edgeFade = softness > 0.0 ? smoothstep(0.0, max(softness, 0.001), linear) : 1.0;

	if (uDynLightFalloffMode == 0)
		return linear * edgeFade;

	if (uDynLightFalloffMode == 1)
		return (1.0 / (1.0 + n * n * 4.0)) * edgeFade;

	return pow(linear, max(uDynLightFalloffExponent, 0.1)) * edgeFade;
}

float BiasedNormalLightFactor(float dotprod)
{
	float wrap = clamp(uDynLightWrap, 0.0, 0.95);
	return clamp((dotprod + wrap) / (1.0 + wrap), 0.0, 1.0);
}

vec3 ApplyBiasedIndirectLight(vec3 light)
{
	return ApplyBiasedDynamicLight(light * max(uDynLightIndirect, 0.0));
}

float ApplyBiasedShadow(float shadow)
{
	return mix(1.0, shadow, clamp(uDynLightShadowStrength, 0.0, 1.0));
}

vec3 ApplyBiasedAmbientFloor(vec3 light)
{
	return max(light, vec3(max(uLightAmbientFloor, 0.0)));
}

vec3 ApplyBiasedSpecularLight(vec3 light)
{
	return light * max(uLightSpecularScale, 0.0);
}

// View-dependent fresnel rim light, modulated by the local light level so it
// never glows in fully dark areas. vWorldNormal is not set on sprites (their
// vertex data has no normals, so it can be zero or even NaN after
// normalize()), so those fragments are skipped. Written as !(d > 0.5) because
// a NaN length would fail a plain <= comparison and let NaN leak through.
vec3 ApplyBiasedRimLight(vec3 color)
{
	float strength = clamp(uRimLightStrength, 0.0, 1.0);
	if (strength <= 0.0)
		return color;
	// Sprites and other geometry without normals can deliver a zero, NaN, or
	// infinite vWorldNormal. Only accept plausible interpolated normals; a
	// plain <= comparison would let NaN through, and a one-sided check would
	// let Inf through (normalize(Inf) is NaN).
	float normalLenSq = dot(vWorldNormal.xyz, vWorldNormal.xyz);
	if (!(normalLenSq > 0.25 && normalLenSq < 4.0))
		return color;
	vec3 normal = normalize(vWorldNormal.xyz);
	vec3 viewDir = normalize(uCameraPos.xyz - pixelpos.xyz);
	float rim = pow(1.0 - abs(dot(normal, viewDir)), max(uRimLightPower, 0.001));
	float lightlevel = clamp(dot(color, vec3(0.3, 0.56, 0.14)), 0.0, 1.0);
	return color + vec3(rim * strength * lightlevel);
}

// Height-tinted ambient: up-facing surfaces pick up the sky tint, down-facing
// surfaces lose a bit of ambient. Shader world space has Y as the height axis.
vec3 ApplyBiasedAmbientGradient(vec3 ambient, vec3 normal)
{
	float strength = clamp(uAmbientGradientStrength, 0.0, 1.0);
	if (strength <= 0.0)
		return ambient;
	float h = 0.5;
	if (dot(normal, normal) > 0.25)
		h = clamp(normalize(normal).y * 0.5 + 0.5, 0.0, 1.0);
	ambient += uAmbientGradientColor.rgb * (strength * h);
	return max(ambient - vec3(strength * (1.0 - h) * 0.25), vec3(0.0));
}

//===========================================================================
//
// Texture tinting code originally from JFDuke but with a few more options
//
//===========================================================================

const int Tex_Blend_Alpha = 1;
const int Tex_Blend_Screen = 2;
const int Tex_Blend_Overlay = 3;
const int Tex_Blend_Hardlight = 4;
 
 vec4 ApplyTextureManipulation(vec4 texel)
 {
	// Step 1: desaturate according to the material's desaturation factor. 
	texel = dodesaturate(texel, uTextureModulateColor.a);
	
	// Step 2: Invert if requested // TODO FIX
	//if ((blendflags & 8) != 0)
	//{
	//	texel.rgb = vec3(1.0 - texel.r, 1.0 - texel.g, 1.0 - texel.b);
	//}
	
	// Step 3: Apply additive color
	texel.rgb += uTextureAddColor.rgb;
	
	// Step 4: Colorization, including gradient if set.
	texel.rgb *= uTextureModulateColor.rgb;
	
	// Before applying the blend the value needs to be clamped to [0..1] range.
	texel.rgb = clamp(texel.rgb, 0.0, 1.0);
	
	// Step 5: Apply a blend. This may just be a translucent overlay or one of the blend modes present in current Build engines.
#if (DEF_BLEND_FLAGS != 0)
	
	vec3 tcol = texel.rgb * 255.0;	// * 255.0 to make it easier to reuse the integer math.
	vec4 tint = uTextureBlendColor * 255.0;

#if (DEF_BLEND_FLAGS == 1)
	
	tcol.b = tcol.b * (1.0 - uTextureBlendColor.a) + tint.b * uTextureBlendColor.a;
	tcol.g = tcol.g * (1.0 - uTextureBlendColor.a) + tint.g * uTextureBlendColor.a;
	tcol.r = tcol.r * (1.0 - uTextureBlendColor.a) + tint.r * uTextureBlendColor.a;

#elif (DEF_BLEND_FLAGS == 2) // Tex_Blend_Screen:
	tcol.b = 255.0 - (((255.0 - tcol.b) * (255.0 - tint.r)) / 256.0);
	tcol.g = 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 256.0);
	tcol.r = 255.0 - (((255.0 - tcol.r) * (255.0 - tint.b)) / 256.0);

#elif (DEF_BLEND_FLAGS == 3) // Tex_Blend_Overlay:
	
	tcol.b = tcol.b < 128.0? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
	tcol.g = tcol.g < 128.0? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
	tcol.r = tcol.r < 128.0? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);

#elif (DEF_BLEND_FLAGS == 4) // Tex_Blend_Hardlight:

	tcol.b = tint.b < 128.0 ? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
	tcol.g = tint.g < 128.0 ? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
	tcol.r = tint.r < 128.0 ? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);

#endif
	
	texel.rgb = tcol / 255.0;
	
#endif

	return texel;
}

//===========================================================================
//
// This function is common for all (non-special-effect) fragment shaders
//
//===========================================================================

vec4 getTexel(vec2 st)
{
	vec4 texel = texture2D(tex, st);
	
#if (DEF_TEXTURE_MODE == 1)

	texel.rgb = vec3(1.0,1.0,1.0);
	
#elif (DEF_TEXTURE_MODE == 2)// TM_OPAQUE
	
	texel.a = 1.0;
				
#elif (DEF_TEXTURE_MODE == 3)// TM_INVERSE
	
	texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, texel.a);

#elif (DEF_TEXTURE_MODE == 4)// TM_ALPHATEXTURE

	float gray = grayscale(texel);
	texel = vec4(1.0, 1.0, 1.0, gray*texel.a);
			
#elif (DEF_TEXTURE_MODE == 5)// TM_CLAMPY
			
	if (st.t < 0.0 || st.t > 1.0)
	{
		texel.a = 0.0;
	}
			
#elif (DEF_TEXTURE_MODE == 6)// TM_OPAQUEINVERSE

	texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, 1.0);

			
#elif (DEF_TEXTURE_MODE == 7)//TM_FOGLAYER 
	
	return texel;
	
#endif
	
	// Apply the texture modification colors.
#if (DEF_BLEND_FLAGS != 0)	

	// only apply the texture manipulation if it contains something.
	texel = ApplyTextureManipulation(texel);

#endif

	// Apply the Doom64 style material colors on top of everything from the texture modification settings.
	// This may be a bit redundant in terms of features but the data comes from different sources so this is unavoidable.
	
	texel.rgb += uAddColor.rgb;

#if (DEF_USE_OBJECT_COLOR_2 == 1)
	texel *= mix(uObjectColor, uObjectColor2, gradientdist.z);
#else
	texel *= uObjectColor;
#endif

	// Last but not least apply the desaturation from the sector's light.
	return desaturate(texel);
}




//===========================================================================
//
// Doom software lighting equation
//
//===========================================================================

#define DOOMLIGHTFACTOR 232.0

float R_DoomLightingEquation_OLD(float light)
{
	// z is the depth in view space, positive going into the screen
	float z = pixelpos.w;

	
		/* L in the range 0 to 63 */
	float L = light * 63.0/31.0;

	float min_L = clamp(36.0/31.0 - L, 0.0, 1.0);

	// Fix objects getting totally black when close.
	if (z < 0.0001)
		z = 0.0001;

	float scale = 1.0 / z;
	float index = (59.0/31.0 - L) - (scale * DOOMLIGHTFACTOR/31.0 - DOOMLIGHTFACTOR/31.0);

	// Result is the normalized colormap index (0 bright .. 1 dark)
	return clamp(index, min_L, 1.0) / 32.0;
}


//===========================================================================
//
// zdoom colormap equation
//
//===========================================================================
float R_ZDoomColormap(float light, float z)
{
	float L = light * 255.0;
	float vis = min(uGlobVis / z, 24.0 / 32.0);
	float shade = 2.0 - (L + 12.0) / 128.0;
	float lightscale = shade - vis;
	return lightscale * 31.0;
}

//===========================================================================
//
// Doom software lighting equation
//
//===========================================================================
float R_DoomLightingEquation(float light)
{
	// z is the depth in view space, positive going into the screen
	float z;

#if (DEF_FOG_RADIAL == 1)
	z = distance(pixelpos.xyz, uCameraPos.xyz);
#else
	z = pixelpos.w;
#endif

#if (DEF_BUILD_LIGHTING == 1) // gl_lightmode 5: Build software lighting emulation.
	// This is a lot more primitive than Doom's lighting...
	float numShades = float(uPalLightLevels);
	float curshade = (1.0 - light) * (numShades - 1.0);
	float visibility = max(uGlobVis * uLightFactor * abs(z), 0.0);
	float shade = clamp((curshade + visibility), 0.0, numShades - 1.0);
	return clamp(shade * uLightDist, 0.0, 1.0);
#endif

	float colormap = R_ZDoomColormap(light, z); // ONLY Software mode, vanilla not yet working

#if (DEF_BANDED_SW_LIGHTING == 1) 
	colormap = floor(colormap) + 0.5;
#endif

	// Result is the normalized colormap index (0 bright .. 1 dark)
	return clamp(colormap, 0.0, 31.0) / 32.0;
}


float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	return 1.0;
}


float spotLightAttenuation(vec4 lightpos, vec3 spotdir, float lightCosInnerAngle, float lightCosOuterAngle)
{
	vec3 lightDirection = normalize(lightpos.xyz - pixelpos.xyz);
	float cosDir = dot(lightDirection, spotdir);
	return smoothstep(lightCosOuterAngle, lightCosInnerAngle, cosDir);
}

vec3 ApplyNormalMap(vec2 texcoord)
{
	return normalize(vWorldNormal.xyz);
}

//===========================================================================
//
// Sets the common material properties.
//
//===========================================================================

void SetMaterialProps(inout Material material, vec2 texCoord)
{

#ifdef NPOT_EMULATION

#if (DEF_NPOT_EMULATION == 1)
		float period = floor(texCoord.t / uNpotEmulation.y);
		texCoord.s += uNpotEmulation.x * floor(mod(texCoord.t, uNpotEmulation.y));
		texCoord.t = period + mod(texCoord.t, uNpotEmulation.y);
#endif

#endif

	material.Base = getTexel(texCoord.st);
	material.Normal = ApplyNormalMap(texCoord.st);

	#if (DEF_TEXTURE_FLAGS & 0x1)
		material.Bright = texture2D(brighttexture, texCoord.st);
	#endif

	#if (DEF_TEXTURE_FLAGS & 0x2)
	{
		vec4 Detail = texture2D(detailtexture, texCoord.st * uDetailParms.xy) * uDetailParms.z;
		material.Base *= Detail;
	}
	#endif

	#if (DEF_TEXTURE_FLAGS & 0x4)
	{
		material.Glow = texture2D(glowtexture, texCoord.st);
	}
	#endif

}

//===========================================================================
//
// Calculate light
//
// It is important to note that the light color is not desaturated
// due to ZDoom's implementation weirdness. Everything that's added
// on top of it, e.g. dynamic lights and glows are, though, because
// the objects emitting these lights are also.
//
// This is making this a bit more complicated than it needs to
// because we can't just desaturate the final fragment color.
// 
//===========================================================================

vec4 getLightColor(Material material, float fogdist, float fogfactor)
{
	vec4 color = vColor;

#if (DEF_USE_U_LIGHT_LEVEL == 1)
	{
		float newlightlevel = 1.0 - R_DoomLightingEquation(uLightLevel);
		color.rgb *= newlightlevel;
	}
#else
	{

		#if (DEF_FOG_ENABLED == 1) && (DEF_FOG_COLOURED == 0)
		{
			// brightening around the player for light mode 2
			if (fogdist < uLightDist)
			{
				color.rgb *= uLightFactor - (fogdist / uLightDist) * (uLightFactor - 1.0);
			}
		
			//
			// apply light diminishing through fog equation
			//
			color.rgb = mix(vec3(0.0, 0.0, 0.0), color.rgb, fogfactor);
		}
		#endif
	}
#endif	

#ifndef LEGACY_USER_SHADER
	if (uSectorBleedParams.x > 0.0 && abs(vWorldNormal.z) > 0.5)
	{
		vec2 bleedUV = (pixelpos.xy - uSectorBleedBounds.xy) * uSectorBleedBounds.zw;
		vec3 bleedTarget = texture2D(SectorBleed, bleedUV).rgb;
		vec3 localLightScale = clamp(color.rgb / max(vColor.rgb, vec3(1.0 / 255.0)), vec3(0.0), vec3(4.0));
		color.rgb = mix(color.rgb, bleedTarget * localLightScale, uSectorBleedParams.x);
	}
#endif


	//
	// handle glowing walls
	//
#if (DEF_USE_GLOW_TOP_COLOR)	
	if (glowdist.x < uGlowTopColor.a)
	{
		color.rgb += desaturate(uGlowTopColor * (1.0 - glowdist.x / uGlowTopColor.a)).rgb;
	}
#endif


#if (DEF_USE_GLOW_BOTTOM_COLOR)	
	if (glowdist.y < uGlowBottomColor.a)
	{
		color.rgb += desaturate(uGlowBottomColor * (1.0 - glowdist.y / uGlowBottomColor.a)).rgb;
	}
#endif

	color = min(color, 1.0);

	// these cannot be safely applied by the legacy format where the implementation cannot guarantee that the values are set.
#ifndef LEGACY_USER_SHADER
	//
	// apply glow 
	//
	color.rgb = mix(color.rgb, material.Glow.rgb, material.Glow.a);

	//
	// apply brightmaps 
	//
	color.rgb = min(color.rgb + material.Bright.rgb * (1.0 + uEmissiveBoost), 1.0);
#endif
	
	//
	// apply other light manipulation by custom shaders, default is a NOP.
	//
	color = ProcessLight(material, color);

	//
	// apply dynamic lights
	//
	return vec4(ApplyBiasedRimLight(ProcessMaterialLight(material, color.rgb)), material.Base.a * vColor.a);
}

//===========================================================================
//
// Applies colored fog
//
//===========================================================================

vec3 getFogColor(float spatialWeight)
{
	vec3 fogcolor = uFogColor.rgb;
	// Spatial fog color modulation fades out as the fog saturates so the far
	// field converges to a uniform atmosphere on any map size.
	float strength = clamp(uFogGradientColor.a, 0.0, 1.0) * spatialWeight;
	float mode = uFogGradientDirection.w;
	float scale = length(uFogGradientDirection.xyz);
	if (mode > 0.5 && strength > 0.0 && scale > 0.0001)
	{
		vec3 direction = uFogGradientDirection.xyz / scale;
		float coord;
		if (mode < 1.5)
		{
			coord = pixelpos.y - uCameraPos.y;
		}
		else
		{
			coord = dot(pixelpos.xyz - uCameraPos.xyz, direction);
		}
		float gradient = clamp(coord * scale / 1024.0 + 0.5, 0.0, 1.0);
		fogcolor = mix(fogcolor, uFogGradientColor.rgb, gradient * strength);
	}
	return fogcolor;
}

vec4 applyFog(vec4 frag, float fogfactor)
{
	return vec4(mix(getFogColor(clamp(fogfactor * 2.0, 0.0, 1.0)), frag.rgb, fogfactor), frag.a);
}

// Sin-free hash: sin() loses all precision in fp32 at the large world
// coordinates of huge maps, which turned the noise into blocky, squary
// artifacts there.
float fogHash(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

float fogNoise(vec3 p)
{
	vec3 cell = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float n00 = mix(fogHash(cell), fogHash(cell + vec3(1.0, 0.0, 0.0)), f.x);
	float n10 = mix(fogHash(cell + vec3(0.0, 1.0, 0.0)), fogHash(cell + vec3(1.0, 1.0, 0.0)), f.x);
	float n01 = mix(fogHash(cell + vec3(0.0, 0.0, 1.0)), fogHash(cell + vec3(1.0, 0.0, 1.0)), f.x);
	float n11 = mix(fogHash(cell + vec3(0.0, 1.0, 1.0)), fogHash(cell + vec3(1.0, 1.0, 1.0)), f.x);
	return mix(mix(n00, n10, f.y), mix(n01, n11, f.y), f.z);
}

// The second octave is sampled in a rotated frame so neither octave's value
// grid aligns with the world axes; this keeps the fog organic instead of
// squary. Same octave count as before, so no extra cost.
const mat3 fogNoiseRot = mat3(
	0.8762, -0.4156,  0.2389,
	0.4156,  0.9045,  0.1005,
	-0.2389,  0.1005,  0.9659);

// Branchless pseudo-random unit gradient for the Perlin path.
vec3 fogGradDir(vec3 cell)
{
	float a = fogHash(cell) * 6.2831853;
	float b = fogHash(cell + 31.416) * 2.0 - 1.0;
	float s = sqrt(max(0.0, 1.0 - b * b));
	return vec3(s * cos(a), s * sin(a), b);
}

// 3D gradient (Perlin) noise with quintic fade: visibly smoother and free of
// the pillowy blobs of plain value noise. Used by the high-quality fog path.
float fogPerlin(vec3 p)
{
	vec3 cell = floor(p);
	vec3 f = fract(p);
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

	float n000 = dot(fogGradDir(cell), f);
	float n100 = dot(fogGradDir(cell + vec3(1.0, 0.0, 0.0)), f - vec3(1.0, 0.0, 0.0));
	float n010 = dot(fogGradDir(cell + vec3(0.0, 1.0, 0.0)), f - vec3(0.0, 1.0, 0.0));
	float n110 = dot(fogGradDir(cell + vec3(1.0, 1.0, 0.0)), f - vec3(1.0, 1.0, 0.0));
	float n001 = dot(fogGradDir(cell + vec3(0.0, 0.0, 1.0)), f - vec3(0.0, 0.0, 1.0));
	float n101 = dot(fogGradDir(cell + vec3(1.0, 0.0, 1.0)), f - vec3(1.0, 0.0, 1.0));
	float n011 = dot(fogGradDir(cell + vec3(0.0, 1.0, 1.0)), f - vec3(0.0, 1.0, 1.0));
	float n111 = dot(fogGradDir(cell + vec3(1.0, 1.0, 1.0)), f - vec3(1.0, 1.0, 1.0));

	return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
	           mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

float fogTurbulenceNoise(vec3 p)
{
	float n = fogPerlin(p) * 0.65 + fogPerlin(fogNoiseRot * p * 2.13 + 17.31) * 0.35;
	return clamp(n * 1.4 + 0.5, 0.0, 1.0);
}

float getEnhancedFogDistance(float fogdist)
{
	if (uFogQuality.x < 0.5)
	{
		if (uThickFogDistance > 0.0 && fogdist > uThickFogDistance)
			fogdist += uThickFogMultiplier * (fogdist - uThickFogDistance);
		return fogdist;
	}

	if (uThickFogDistance > 0.0)
	{
		float excess = max(fogdist - uThickFogDistance, 0.0);
		float transition = max(48.0, uThickFogDistance * 0.30);
		fogdist += uThickFogMultiplier * excess * smoothstep(0.0, transition, excess);
	}

	return max(fogdist, 16.0);
}

float getEnhancedFogFactor(float fogdist)
{
	float dist = getEnhancedFogDistance(fogdist);
	// Analytic exponential height fog (Beer-Lambert). The medium's extinction
	// coefficient decays exponentially with height, sigma(z) = sigma0 * 2^(-k *
	// (z - camz)), and integrates in closed form along the view ray:
	//     od = sigma0 * d * (1 - 2^(-k*dz)) / (k*dz*ln2)
	// uFogDensity already carries sigma0 in base-2 log units (see
	// FRenderState::SetFog), and k = falloff/256 keeps the
	// bd_fog_height_falloff tuning intuition: at falloff 1.0 the density doubles
	// every 256 units below the camera. falloff 0 makes heightIntegral exactly
	// 1, reducing to plain distance fog. All inputs are coordinate differences,
	// so this stays fp32-stable at the world coordinates of huge maps, and the
	// continuous integral cannot produce brightness steps between surfaces at
	// different heights (e.g. a far wall against the floor it meets).
	float k = uFogQuality.y * (1.0 / 256.0);
	float x = clamp(k * (pixelpos.z - uCameraPos.z), -64.0, 64.0);
	float heightIntegral = (abs(x) < 0.001) ? 1.0 : (1.0 - exp2(-x)) / (x * 0.6931471805599453);
	float od = uFogDensity * dist * heightIntegral;
	// Turbulence is a *density* fluctuation (Beer-Lambert): it modulates the
	// optical depth, never the transmittance directly. exp2() compresses the
	// response, so thin fog on nearby surfaces stays clean (no veins or harsh
	// lines across textures), mid distances drift organically, and the
	// saturated far field stays fogged instead of being punched through.
	// Skip where the fog is too thin (od > -0.03) or too saturated (od < -6)
	// for noise to be visible; on huge maps the saturated far field is most of
	// the screen, and the branch is spatially coherent. Quality 0 skips
	// turbulence altogether.
	if (uFogQuality.z > 0.0 && uFogQuality.x > 0.5 && od < -0.03 && od > -6.0)
	{
		// Wrap into a large periodic domain so hash inputs stay small: bounded
		// fp32 error no matter how large the map coordinates get. 1024 cells *
		// 1/scale world units per tile (128k units at the default scale), far
		// beyond fogged visibility, so the tiling is never perceptible.
		vec3 noisePos = mod(pixelpos.xyz * uFogQuality.w, 1024.0);
		float noiseValue = (uFogQuality.x > 1.5) ? fogTurbulenceNoise(noisePos) : fogNoise(noisePos);
		od *= clamp(1.0 + (noiseValue - 0.5) * 2.0 * uFogQuality.z, 0.5, 1.5);
	}
	float fogfactor = exp2(od);
	if (uFogQuality.x > 0.5)
	{
		float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
		fogfactor = clamp(fogfactor + dither / 255.0, 0.0, 1.0);
	}
	// Smooth minimum-visibility floor: rescale [0,1] -> [minVis,1] instead of a
	// hard max(). A clamp has a derivative discontinuity that shows up as a
	// visible contour line where distant geometry hits the floor; the rescale
	// is C1-smooth everywhere and still guarantees the same lower bound.
	return uFogMinVisibility + (1.0 - uFogMinVisibility) * fogfactor;
}

//===========================================================================
//
// Main shader routine
//
//===========================================================================

void main()
{

	//if (ClipDistanceA.x < 0.0 || ClipDistanceA.y < 0.0 || ClipDistanceA.z < 0.0 || ClipDistanceA.w < 0.0 || ClipDistanceB.x < 0.0) discard;

#ifndef LEGACY_USER_SHADER
	Material material;
	
	material.Base = vec4(0.0);
	material.Bright = vec4(0.0);
	material.Glow = vec4(0.0);
	material.Normal = vec3(0.0);
	material.Specular = vec3(0.0);
	material.Glossiness = 0.0;
	material.SpecularLevel = 0.0;
	SetupMaterial(material);
#else
	Material material = ProcessMaterial();
#endif
	vec4 frag = material.Base;

#ifndef NO_ALPHATEST
	if (frag.a <= uAlphaThreshold) discard;
#endif

#ifdef DITHERTRANS
	int index = (int(pixelpos.x) % 2) * 2 + int(pixelpos.y) % 2;
	if (index != 2) discard;
#endif

	#if (DEF_FOG_2D == 0)	// check for special 2D 'fog' mode.
	{
		float fogdist = 0.0;
		float fogfactor = 0.0;
		
		//
		// calculate fog factor
		//
		#if (DEF_FOG_ENABLED == 1)
		{
			#if (DEF_FOG_RADIAL == 0)
				fogdist = max(16.0, pixelpos.w);
			#else
				fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
			#endif

			fogfactor = getEnhancedFogFactor(fogdist);
		}
		#endif

		#if (DEF_TEXTURE_MODE != 7)
		{
			frag = getLightColor(material, fogdist, fogfactor);

			//
			// colored fog
			//
			#if (DEF_FOG_ENABLED == 1) && (DEF_FOG_COLOURED == 1)
			{
				frag = applyFog(frag, fogfactor);
			}
			#endif
		}
		#else
		{
			frag = vec4(getFogColor(clamp(fogfactor * 2.0, 0.0, 1.0)), (1.0 - fogfactor) * frag.a * 0.75 * vColor.a);
		}
		#endif
	}	
	#else
	{
		#if (DEF_TEXTURE_MODE == 7)
		{
			float gray = grayscale(frag);
			vec4 cm = (uObjectColor + gray * (uAddColor - uObjectColor)) * 2.0;
			frag = vec4(clamp(cm.rgb, 0.0, 1.0), frag.a);
		}		
		#endif
	
		frag = frag * ProcessLight(material, vColor);
		frag.rgb = frag.rgb + uFogColor.rgb;
	}
	#endif  // (DEF_2D_FOG == 0)
	
#if (DEF_USE_COLOR_MAP == 1) // This mostly works but doesn't look great because of the blending.
	{
		frag.rgb = clamp(pow(frag.rgb, vec3(uFixedColormapStart.a)), 0.0, 1.0);
		if (uFixedColormapRange.a == 0.0)
		{
			float gray = (frag.r * 0.3 + frag.g * 0.56 + frag.b * 0.14);	
			vec4 cm = uFixedColormapStart + gray * uFixedColormapRange;
			frag.rgb = clamp(cm.rgb, 0.0, 1.0);
		} 
	}
#endif

	// Aerial perspective: fade distant fragments toward a desaturated haze.
	// fogdist is block-scoped above, so recompute the radial distance here.
	if (uAerialStrength > 0.0)
	{
		float aerialDist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
		float aerial = clamp(aerialDist / max(uAerialDistance, 64.0), 0.0, 1.0) * uAerialStrength;
		float lum = dot(frag.rgb, vec3(0.2126, 0.7152, 0.0722));
		vec3 haze = mix(uFogColor.rgb, uAmbientGradientColor.rgb, 0.5);
		vec3 target = mix(vec3(lum), haze, 0.35) * (0.85 + 0.15 * lum);
		frag.rgb = mix(frag.rgb, target, aerial * 0.6);
	}

	gl_FragColor = frag;

	//gl_FragColor = vec4(0.8, 0.2, 0.5, 1);

}
