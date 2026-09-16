

layout(location = 0) in vec4 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 pixelpos;
layout(location = 3) in vec3 glowdist;
layout(location = 4) in vec3 gradientdist;
layout(location = 5) in vec4 vWorldNormal;
layout(location = 6) in vec4 vEyeNormal;
layout(location = 9) in vec3 vLightmap;

#ifdef NO_CLIPDISTANCE_SUPPORT
layout(location = 7) in vec4 ClipDistanceA;
layout(location = 8) in vec4 ClipDistanceB;
#endif

layout(location=0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location=1) out vec4 FragFog;
layout(location=2) out vec4 FragNormal;
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
	float Metallic;
	float Roughness;
	float AO;
};

vec4 Process(vec4 color);
vec4 ProcessTexel();
Material ProcessMaterial(); // note that this is deprecated. Use SetupMaterial!
void SetupMaterial(inout Material mat);
vec4 ProcessLight(Material mat, vec4 color);
vec3 ProcessMaterialLight(Material material, vec3 color);
vec2 GetTexCoord();

// These get Or'ed into uTextureMode because it only uses its 3 lowermost bits.
const int TEXF_Brightmap = 0x10000;
const int TEXF_Detailmap = 0x20000;
const int TEXF_Glowmap = 0x40000;
const int TEXF_ClampY = 0x80000;

//===========================================================================
//
// RGB to HSV
//
//===========================================================================

vec3 rgb2hsv(vec3 c)
{
	vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
	vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

	float d = q.x - min(q.w, q.y);
	float e = 1.0e-10;
	return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

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
	return dodesaturate(texel, uDesaturationFactor);
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

 vec4 ApplyTextureManipulation(vec4 texel, int blendflags)
 {
	// Step 1: desaturate according to the material's desaturation factor. 
	texel = dodesaturate(texel, uTextureModulateColor.a);

	// Step 2: Invert if requested
	if ((blendflags & 8) != 0)
	{
		texel.rgb = vec3(1.0 - texel.r, 1.0 - texel.g, 1.0 - texel.b);
	}

	// Step 3: Apply additive color
	texel.rgb += uTextureAddColor.rgb;

	// Step 4: Colorization, including gradient if set.
	texel.rgb *= uTextureModulateColor.rgb;

	// Before applying the blend the value needs to be clamped to [0..1] range.
	texel.rgb = clamp(texel.rgb, 0.0, 1.0);

	// Step 5: Apply a blend. This may just be a translucent overlay or one of the blend modes present in current Build engines.
	if ((blendflags & 7) != 0)
	{
		vec3 tcol = texel.rgb * 255.0;	// * 255.0 to make it easier to reuse the integer math.
		vec4 tint = uTextureBlendColor * 255.0;

		switch (blendflags & 7)
		{
			default:
				tcol.b = tcol.b * (1.0 - uTextureBlendColor.a) + tint.b * uTextureBlendColor.a;
				tcol.g = tcol.g * (1.0 - uTextureBlendColor.a) + tint.g * uTextureBlendColor.a;
				tcol.r = tcol.r * (1.0 - uTextureBlendColor.a) + tint.r * uTextureBlendColor.a;
				break;
			// The following 3 are taken 1:1 from the Build engine
			case Tex_Blend_Screen:
				tcol.b = 255.0 - (((255.0 - tcol.b) * (255.0 - tint.r)) / 256.0);
				tcol.g = 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 256.0);
				tcol.r = 255.0 - (((255.0 - tcol.r) * (255.0 - tint.b)) / 256.0);
				break;
			case Tex_Blend_Overlay:
				tcol.b = tcol.b < 128.0? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tcol.g < 128.0? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tcol.r < 128.0? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
			case Tex_Blend_Hardlight:
				tcol.b = tint.b < 128.0 ? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tint.g < 128.0 ? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tint.r < 128.0 ? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
		}
		texel.rgb = tcol / 255.0;
	}
	return texel;
}

//===========================================================================
//
// This function is common for all (non-special-effect) fragment shaders
//
//===========================================================================

vec4 getTexel(vec2 st)
{
	vec4 texel = texture(tex, st);

	//
	// Apply texture modes
	//
	switch (uTextureMode & 0xffff)
	{
		case 1:	// TM_STENCIL
			texel.rgb = vec3(1.0,1.0,1.0);
			break;

		case 2:	// TM_OPAQUE
			texel.a = 1.0;
			break;

		case 3:	// TM_INVERSE
			texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, texel.a);
			break;

		case 4:	// TM_ALPHATEXTURE
		{
			float gray = grayscale(texel);
			texel = vec4(1.0, 1.0, 1.0, gray*texel.a);
			break;
		}

		case 5:	// TM_CLAMPY
			if (st.t < 0.0 || st.t > 1.0)
			{
				texel.a = 0.0;
			}
			break;

		case 6: // TM_OPAQUEINVERSE
			texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, 1.0);
			break;

		case 7: //TM_FOGLAYER 
			return texel;

	}

	if ((uTextureMode & TEXF_ClampY) != 0)
	{
		if (st.t < 0.0 || st.t > 1.0)
		{
			texel.a = 0.0;
		}
	}

	// Apply the texture modification colors.
	int blendflags = int(uTextureAddColor.a);	// this alpha is unused otherwise
	if (blendflags != 0)	
	{
		// only apply the texture manipulation if it contains something.
		texel = ApplyTextureManipulation(texel, blendflags);
	}

	// Apply the Doom64 style material colors on top of everything from the texture modification settings.
	// This may be a bit redundant in terms of features but the data comes from different sources so this is unavoidable.
	texel.rgb += uAddColor.rgb;
	if (uObjectColor2.a == 0.0) texel *= uObjectColor;
	else texel *= mix(uObjectColor, uObjectColor2, gradientdist.z);

	// Last but not least apply the desaturation from the sector's light.
	return desaturate(texel);
}

//===========================================================================
//
// Vanilla Doom wall colormap equation
//
//===========================================================================
float R_WallColormap(float lightnum, float z, vec3 normal)
{
	// R_ScaleFromGlobalAngle calculation
	float projection = 160.0; // projection depends on SCREENBLOCKS!! 160 is the fullscreen value
	vec2 line_v1 = pixelpos.xz; // in vanilla this is the first curline vertex
	vec2 line_normal = normal.xz;
	float texscale = projection * clamp(dot(normalize(uCameraPos.xz - line_v1), line_normal), 0.0, 1.0) / z;

	float lightz = clamp(16.0 * texscale, 0.0, 47.0);

	// scalelight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	return startmap - lightz * 0.5;
}

//===========================================================================
//
// Vanilla Doom plane colormap equation
//
//===========================================================================
float R_PlaneColormap(float lightnum, float z)
{
	float lightz = clamp(z / 16.0f, 0.0, 127.0);

	// zlight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	float scale = 160.0 / (lightz + 1.0);
	return startmap - scale * 0.5;
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

float R_DoomColormap(float light, float z)
{
	if ((uPalLightLevels >> 16) == 16) // gl_lightmode 16
	{
		float lightnum = clamp(light * 15.0, 0.0, 15.0);

		if (dot(vWorldNormal.xyz, vWorldNormal.xyz) > 0.5)
		{
			vec3 normal = normalize(vWorldNormal.xyz);
			return mix(R_WallColormap(lightnum, z, normal), R_PlaneColormap(lightnum, z), abs(normal.y));
		}
		else // vWorldNormal is not set on sprites
		{
			return R_PlaneColormap(lightnum, z);
		}
	}
	else
	{
		return R_ZDoomColormap(light, z);
	}
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
	if (((uPalLightLevels >> 8)  & 0xff) == 2)
	{
		z = distance(pixelpos.xyz, uCameraPos.xyz);
	}
	else 
	{
		z = pixelpos.w;
	}

	if ((uPalLightLevels >> 16) == 5) // gl_lightmode 5: Build software lighting emulation.
	{
		// This is a lot more primitive than Doom's lighting...
		float numShades = float(uPalLightLevels & 255);
		float curshade = (1.0 - light) * (numShades - 1.0);
		float visibility = max(uGlobVis * uLightFactor * z, 0.0);
		float shade = clamp((curshade + visibility), 0.0, numShades - 1.0);
		return clamp(shade * uLightDist, 0.0, 1.0);
	}

	float colormap = R_DoomColormap(light, z);

	if ((uPalLightLevels & 0xff) != 0)
		colormap = floor(colormap) + 0.5;

	// Result is the normalized colormap index (0 bright .. 1 dark)
	return clamp(colormap, 0.0, 31.0) / 32.0;
}

//===========================================================================
//
// Check if light is in shadow
//
//===========================================================================

#ifdef SUPPORTS_RAYTRACING

bool traceHit(vec3 origin, vec3 direction, float dist)
{
	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TopLevelAS, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin, 0.01f, direction, dist);
	while(rayQueryProceedEXT(rayQuery)) { }
	return rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

vec2 softshadow[9 * 3] = vec2[](
	vec2( 0.0, 0.0),
	vec2(-2.0,-2.0),
	vec2( 2.0, 2.0),
	vec2( 2.0,-2.0),
	vec2(-2.0, 2.0),
	vec2(-1.0,-1.0),
	vec2( 1.0, 1.0),
	vec2( 1.0,-1.0),
	vec2(-1.0, 1.0),

	vec2( 0.0, 0.0),
	vec2(-1.5,-1.5),
	vec2( 1.5, 1.5),
	vec2( 1.5,-1.5),
	vec2(-1.5, 1.5),
	vec2(-0.5,-0.5),
	vec2( 0.5, 0.5),
	vec2( 0.5,-0.5),
	vec2(-0.5, 0.5),

	vec2( 0.0, 0.0),
	vec2(-1.25,-1.75),
	vec2( 1.75, 1.25),
	vec2( 1.25,-1.75),
	vec2(-1.75, 1.75),
	vec2(-0.75,-0.25),
	vec2( 0.25, 0.75),
	vec2( 0.75,-0.25),
	vec2(-0.25, 0.75)
);

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	float shadowIndex = abs(lightcolorA) - 1.0;
	if (shadowIndex >= 1024.0)
		return 1.0; // Don't cast rays for this light

	vec3 origin = pixelpos.xzy;
	vec3 target = lightpos.xzy + 0.01; // nudge light position slightly as Doom maps tend to have their lights perfectly aligned with planes

	vec3 direction = normalize(target - origin);
	float dist = distance(origin, target);

	if (uShadowmapFilter <= 0)
	{
		return ApplyBiasedShadow(traceHit(origin, direction, dist) ? 0.0 : 1.0);
	}
	else
	{
		vec3 v = (abs(direction.x) > abs(direction.y)) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		vec3 xdir = normalize(cross(direction, v));
		vec3 ydir = cross(direction, xdir);

		float sum = 0.0;
		int step_count = uShadowmapFilter * 9;
		for (int i = 0; i <= step_count; i++)
		{
			vec3 pos = target + xdir * softshadow[i].x + ydir * softshadow[i].y;
			sum += traceHit(origin, normalize(pos - origin), dist) ? 0.0 : 1.0;
		}
		return ApplyBiasedShadow(sum / step_count);
	}
}

#else
#ifdef SUPPORTS_SHADOWMAPS

float shadowDirToU(vec2 dir)
{
	if (abs(dir.y) > abs(dir.x))
	{
		float x = dir.x / dir.y * 0.125;
		if (dir.y >= 0.0)
			return 0.125 + x;
		else
			return (0.50 + 0.125) + x;
	}
	else
	{
		float y = dir.y / dir.x * 0.125;
		if (dir.x >= 0.0)
			return (0.25 + 0.125) - y;
		else
			return (0.75 + 0.125) - y;
	}
}

vec2 shadowUToDir(float u)
{
	u *= 4.0;
	vec2 raydir;
	switch (int(u))
	{
	case 0: raydir = vec2(u * 2.0 - 1.0, 1.0); break;
	case 1: raydir = vec2(1.0, 1.0 - (u - 1.0) * 2.0); break;
	case 2: raydir = vec2(1.0 - (u - 2.0) * 2.0, -1.0); break;
	case 3: raydir = vec2(-1.0, (u - 3.0) * 2.0 - 1.0); break;
	}
	return raydir;
}

float sampleShadowmap(vec3 planePoint, float v)
{
	float bias = 1.0;
	float negD = dot(vWorldNormal.xyz, planePoint);

	vec3 ray = planePoint;

	vec2 isize = textureSize(ShadowMap, 0);
	float scale = float(isize.x) * 0.25;

	// Snap to shadow map texel grid
	if (abs(ray.z) > abs(ray.x))
	{
		ray.y = ray.y / abs(ray.z);
		ray.x = ray.x / abs(ray.z);
		ray.x = (floor((ray.x + 1.0) * 0.5 * scale) + 0.5) / scale * 2.0 - 1.0;
		ray.z = sign(ray.z);
	}
	else
	{
		ray.y = ray.y / abs(ray.x);
		ray.z = ray.z / abs(ray.x);
		ray.z = (floor((ray.z + 1.0) * 0.5 * scale) + 0.5) / scale * 2.0 - 1.0;
		ray.x = sign(ray.x);
	}

	float t = negD / dot(vWorldNormal.xyz, ray) - bias;
	vec2 dir = ray.xz * t;

	float u = shadowDirToU(dir);
	float dist2 = dot(dir, dir);
	return step(dist2, texture(ShadowMap, vec2(u, v)).x);
}

float sampleShadowmapPCF(vec3 planePoint, float v)
{
	float bias = 1.0;
	float negD = dot(vWorldNormal.xyz, planePoint);

	vec3 ray = planePoint;

	if (abs(ray.z) > abs(ray.x))
		ray.y = ray.y / abs(ray.z);
	else
		ray.y = ray.y / abs(ray.x);

	vec2 isize = textureSize(ShadowMap, 0);
	float scale = float(isize.x);
	float texelPos = floor(shadowDirToU(ray.xz) * scale);

	float sum = 0.0;
	float step_count = uShadowmapFilter;

	texelPos -= step_count + 0.5;
	for (float x = -step_count; x <= step_count; x++)
	{
		float u = fract(texelPos / scale);
		vec2 dir = shadowUToDir(u);

		ray.x = dir.x;
		ray.z = dir.y;
		float t = negD / dot(vWorldNormal.xyz, ray) - bias;
		dir = ray.xz * t;

		float dist2 = dot(dir, dir);
		sum += step(dist2, texture(ShadowMap, vec2(u, v)).x);
		texelPos++;
	}
	return sum / (uShadowmapFilter * 2.0 + 1.0);
}

float shadowmapAttenuation(vec4 lightpos, float shadowIndex)
{
	if (shadowIndex >= 1024.0)
		return 1.0; // No shadowmap available for this light

	vec3 planePoint = pixelpos.xyz - lightpos.xyz;
	planePoint += 0.01; // nudge light position slightly as Doom maps tend to have their lights perfectly aligned with planes

	if (dot(planePoint.xz, planePoint.xz) < 1.0)
		return 1.0; // Light is too close

	float v = (shadowIndex + 0.5) / 1024.0;

	if (uShadowmapFilter <= 0)
	{
		return sampleShadowmap(planePoint, v);
	}
	else
	{
		return sampleShadowmapPCF(planePoint, v);
	}
}

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	float shadowIndex = abs(lightcolorA) - 1.0;
	return ApplyBiasedShadow(shadowmapAttenuation(lightpos, shadowIndex));
}

#else

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	return 1.0;
}

#endif
#endif

float spotLightAttenuation(vec4 lightpos, vec3 spotdir, float lightCosInnerAngle, float lightCosOuterAngle)
{
	vec3 lightDirection = normalize(lightpos.xyz - pixelpos.xyz);
	float cosDir = dot(lightDirection, spotdir);
	return smoothstep(lightCosOuterAngle, lightCosInnerAngle, cosDir);
}

//===========================================================================
//
// Adjust normal vector according to the normal map
//
//===========================================================================

#if defined(NORMALMAP)
mat3 cotangent_frame(vec3 n, vec3 p, vec2 uv)
{
	// get edge vectors of the pixel triangle
	vec3 dp1 = dFdx(p);
	vec3 dp2 = dFdy(p);
	vec2 duv1 = dFdx(uv);
	vec2 duv2 = dFdy(uv);

	// solve the linear system
	vec3 dp2perp = cross(n, dp2); // cross(dp2, n);
	vec3 dp1perp = cross(dp1, n); // cross(n, dp1);
	vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;

	// construct a scale-invariant frame
	float invmax = inversesqrt(max(dot(t,t), dot(b,b)));
	return mat3(t * invmax, b * invmax, n);
}

vec3 ApplyNormalMap(vec2 texcoord)
{
	#define WITH_NORMALMAP_UNSIGNED
	#define WITH_NORMALMAP_GREEN_UP
	//#define WITH_NORMALMAP_2CHANNEL

	vec3 interpolatedNormal = normalize(vWorldNormal.xyz);

	vec3 map = texture(normaltexture, texcoord).xyz;
	#if defined(WITH_NORMALMAP_UNSIGNED)
	map = map * 255./127. - 128./127.; // Math so "odd" because 0.5 cannot be precisely described in an unsigned format
	#endif
	#if defined(WITH_NORMALMAP_2CHANNEL)
	map.z = sqrt(1 - dot(map.xy, map.xy));
	#endif
	#if defined(WITH_NORMALMAP_GREEN_UP)
	map.y = -map.y;
	#endif

	mat3 tbn = cotangent_frame(interpolatedNormal, pixelpos.xyz, vTexCoord.st);
	vec3 bumpedNormal = normalize(tbn * map);
	return bumpedNormal;
}
#else
vec3 ApplyNormalMap(vec2 texcoord)
{
	return normalize(vWorldNormal.xyz);
}
#endif

//===========================================================================
//
// Sets the common material properties.
//
//===========================================================================

void SetMaterialProps(inout Material material, vec2 texCoord)
{
#ifdef NPOT_EMULATION
	if (uNpotEmulation.y != 0.0)
	{
		float period = floor(texCoord.t / uNpotEmulation.y);
		texCoord.s += uNpotEmulation.x * floor(mod(texCoord.t, uNpotEmulation.y));
		texCoord.t = period + mod(texCoord.t, uNpotEmulation.y);
	}
#endif	
	material.Base = getTexel(texCoord.st); 
	material.Normal = ApplyNormalMap(texCoord.st);

// OpenGL doesn't care, but Vulkan pukes all over the place if these texture samplings are included in no-texture shaders, even though never called.
#ifndef NO_LAYERS
	if ((uTextureMode & TEXF_Brightmap) != 0)
		material.Bright = desaturate(texture(brighttexture, texCoord.st));

	if ((uTextureMode & TEXF_Detailmap) != 0)
	{
		vec4 Detail = texture(detailtexture, texCoord.st * uDetailParms.xy) * uDetailParms.z;
		material.Base.rgb *= Detail.rgb;
	}

	if ((uTextureMode & TEXF_Glowmap) != 0)
		material.Glow = desaturate(texture(glowtexture, texCoord.st));
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

	if (uLightLevel >= 0.0)
	{
		float newlightlevel = 1.0 - R_DoomLightingEquation(uLightLevel);
		color.rgb *= newlightlevel;
	}
	else if (uFogEnabled > 0)
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

#if !defined LEGACY_USER_SHADER && !defined NO_LAYERS
	// Smooth sector lighting only over horizontal world surfaces. Derive the
	// local sector-light/fog scale from vColor, then apply it to the blended
	// target so software and hardware light modes retain their normal shaping.
	if (uSectorBleedParams.x > 0.0 && abs(vWorldNormal.z) > 0.5)
	{
		vec2 bleedUV = (pixelpos.xy - uSectorBleedBounds.xy) * uSectorBleedBounds.zw;
		vec3 bleedTarget = texture(SectorBleed, bleedUV).rgb;
		vec3 localLightScale = clamp(color.rgb / max(vColor.rgb, vec3(1.0 / 255.0)), vec3(0.0), vec3(4.0));
		color.rgb = mix(color.rgb, bleedTarget * localLightScale, uSectorBleedParams.x);
	}
#endif

	//
	// handle glowing walls
	//
	if (uGlowTopColor.a > 0.0 && glowdist.x < uGlowTopColor.a)
	{
		color.rgb += desaturate(uGlowTopColor * (1.0 - glowdist.x / uGlowTopColor.a)).rgb;
	}
	if (uGlowBottomColor.a > 0.0 && glowdist.y < uGlowBottomColor.a)
	{
		color.rgb += desaturate(uGlowBottomColor * (1.0 - glowdist.y / uGlowBottomColor.a)).rgb;
	}
	color = min(color, 1.0);

	// these cannot be safely applied by the legacy format where the implementation cannot guarantee that the values are set.
#if !defined LEGACY_USER_SHADER && !defined NO_LAYERS
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
	// apply lightmaps
	//
	if (vLightmap.z >= 0.0)
	{
		color.rgb += texture(LightMap, vLightmap).rgb;
	}

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

// Stable world-space value noise keeps fog from looking like a perfectly
// uniform color sheet. It intentionally does not depend on time, so geometry
// does not shimmer while the camera moves. The hash is sin-free: sin() loses
// all precision in fp32 at the large world coordinates of huge maps, which
// turned the noise into blocky, squary artifacts there.
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

	// Ease into dense distance fog instead of exposing a hard circular wall.
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
// The color of the fragment if it is fully occluded by ambient lighting
//
//===========================================================================

// Takes the fog factor already computed for this fragment in main() so the
// G-buffer FragFog output does not have to evaluate the full fog function
// (turbulence included) a second time.
vec3 AmbientOcclusionColor(float fogfactor)
{
	return mix(getFogColor(clamp(fogfactor * 2.0, 0.0, 1.0)), vec3(0.0), fogfactor);
}

// Legacy self-computing variant, used only when main() legitimately skips the
// fog computation (fog disabled / 2D mode), to keep the old FragFog semantics.
vec3 AmbientOcclusionColorLegacy()
{
	float fogdist = (uFogEnabled == -1) ? max(16.0, pixelpos.w) : max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
	return AmbientOcclusionColor(getEnhancedFogFactor(fogdist));
}

//===========================================================================
//
// Main shader routine
//
//===========================================================================

void main()
{
#ifdef NO_CLIPDISTANCE_SUPPORT
	if (ClipDistanceA.x < 0 || ClipDistanceA.y < 0 || ClipDistanceA.z < 0 || ClipDistanceA.w < 0 || ClipDistanceB.x < 0) discard;
#endif

#ifndef LEGACY_USER_SHADER
	Material material;

	material.Base = vec4(0.0);
	material.Bright = vec4(0.0);
	material.Glow = vec4(0.0);
	material.Normal = vec3(0.0);
	material.Specular = vec3(0.0);
	material.Glossiness = 0.0;
	material.SpecularLevel = 0.0;
	material.Metallic = 0.0;
	material.Roughness = 0.0;
	material.AO = 0.0;
	SetupMaterial(material);
#else
	Material material = ProcessMaterial();
#endif
	vec4 frag = material.Base;

#ifndef NO_ALPHATEST
	if (frag.a <= uAlphaThreshold) discard;
#endif

	float fogdist = 0.0;
	float fogfactor = 0.0;

	if (uFogEnabled != -3)	// check for special 2D 'fog' mode.
	{
		//
		// calculate fog factor
		//
		if (uFogEnabled != 0)
		{
			if (uFogEnabled == 1 || uFogEnabled == -1) 
			{
				fogdist = max(16.0, pixelpos.w);
			}
			else 
			{
				fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
			}
			fogfactor = getEnhancedFogFactor(fogdist);
		}

		if ((uTextureMode & 0xffff) != 7)
		{
			frag = getLightColor(material, fogdist, fogfactor);

			//
			// colored fog
			//
			if (uFogEnabled < 0) 
			{
				frag = applyFog(frag, fogfactor);
			}
		}
		else
		{
			frag = vec4(getFogColor(clamp(fogfactor * 2.0, 0.0, 1.0)), (1.0 - fogfactor) * frag.a * 0.75 * vColor.a);
		}
	}
	else // simple 2D (uses the fog color to add a color overlay)
	{
		if ((uTextureMode & 0xffff) == 7)
		{
			float gray = grayscale(frag);
			vec4 cm = (uObjectColor + gray * (uAddColor - uObjectColor)) * 2;
			frag = vec4(clamp(cm.rgb, 0.0, 1.0), frag.a);
		}
			frag = frag * ProcessLight(material, vColor);
		frag.rgb = frag.rgb + uFogColor.rgb;
	}
	// Aerial perspective: fade distant fragments toward a desaturated haze.
	// fogdist stays 0.0 when fog is disabled, so fall back to a fresh radial
	// distance in that case.
	if (uAerialStrength > 0.0)
	{
		float aerialDist = (fogdist > 0.0) ? fogdist : max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
		float aerial = clamp(aerialDist / max(uAerialDistance, 64.0), 0.0, 1.0) * uAerialStrength;
		float lum = dot(frag.rgb, vec3(0.2126, 0.7152, 0.0722));
		vec3 haze = mix(uFogColor.rgb, uAmbientGradientColor.rgb, 0.5);
		vec3 target = mix(vec3(lum), haze, 0.35) * (0.85 + 0.15 * lum);
		frag.rgb = mix(frag.rgb, target, aerial * 0.6);
	}
	FragColor = frag;

#ifdef DITHERTRANS
	int index = (int(pixelpos.x) % 8) * 8 + int(pixelpos.y) % 8;
	const float DITHER_THRESHOLDS[64] =
	float[64](
		1.0 / 65.0, 33.0 / 65.0, 9.0 / 65.0, 41.0 / 65.0, 3.0 / 65.0, 35.0 / 65.0, 11.0 / 65.0, 43.0 / 65.0,
		49.0 / 65.0, 17.0 / 65.0, 57.0 / 65.0, 25.0 / 65.0, 51.0 / 65.0, 19.0 / 65.0, 59.0 / 65.0, 27.0 / 65.0,
		13.0 / 65.0, 45.0 / 65.0, 5.0 / 65.0, 37.0 / 65.0, 15.0 / 65.0, 47.0 / 65.0, 7.0 / 65.0, 39.0 / 65.0,
		61.0 / 65.0, 29.0 / 65.0, 53.0 / 65.0, 21.0 / 65.0, 63.0 / 65.0, 31.0 / 65.0, 55.0 / 65.0, 23.0 / 65.0,
		4.0 / 65.0, 36.0 / 65.0, 12.0 / 65.0, 44.0 / 65.0, 2.0 / 65.0, 34.0 / 65.0, 10.0 / 65.0, 42.0 / 65.0,
		52.0 / 65.0, 20.0 / 65.0, 60.0 / 65.0, 28.0 / 65.0, 50.0 / 65.0, 18.0 / 65.0, 58.0 / 65.0, 26.0 / 65.0,
		16.0 / 65.0, 48.0 / 65.0, 8.0 / 65.0, 40.0 / 65.0, 14.0 / 65.0, 46.0 / 65.0, 6.0 / 65.0, 38.0 / 65.0,
		64.0 / 65.0, 32.0 / 65.0, 56.0 / 65.0, 24.0 / 65.0, 62.0 / 65.0, 30.0 / 65.0, 54.0 / 65.0, 22.0 /65.0
	);

	vec3 fragHSV = rgb2hsv(FragColor.rgb);
	float brightness = clamp(1.5*fragHSV.z, 0.1, 1.0);
	if (DITHER_THRESHOLDS[index] < brightness) discard;
	else FragColor *= 0.5;
#endif

#ifdef GBUFFER_PASS
	if (uFogEnabled == 0 || uFogEnabled == -3)
		FragFog = vec4(AmbientOcclusionColorLegacy(), 1.0);
	else
		FragFog = vec4(AmbientOcclusionColor(fogfactor), 1.0);
	FragNormal = vec4(vEyeNormal.xyz * 0.5 + 0.5, 1.0);
#endif
}
