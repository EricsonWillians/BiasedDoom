
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D InputTexture;
layout(binding=1) uniform sampler2D DitherTexture;

vec4 ApplyGamma(vec4 c)
{
	c.rgb = min(c.rgb, vec3(2.0)); // for HDR mode - prevents stacked translucent sprites (such as plasma) producing way too bright light

	vec3 valgray;
	if (GrayFormula == 0)
		valgray = vec3(c.r + c.g + c.b) * (1 - Saturation) / 3 + c.rgb * Saturation;
	else if (GrayFormula == 2)	// new formula
		valgray = mix(vec3(pow(dot(pow(vec3(c), vec3(2.2)), vec3(0.2126, 0.7152, 0.0722)), 1.0/2.2)), c.rgb, Saturation);
	else
		valgray = mix(vec3(dot(c.rgb, vec3(0.3,0.56,0.14))), c.rgb, Saturation);
	vec3 val = valgray * Contrast - (Contrast - 1.0) * 0.5;
	val += Brightness * 0.5;
	val = pow(max(val, vec3(0.0)), vec3(InvGamma));
	return vec4(val, c.a);
}

vec4 Dither(vec4 c)
{
	if (ColorScale == 0.0)
		return c;
	vec2 texSize = vec2(textureSize(DitherTexture, 0));
	float threshold = texture(DitherTexture, gl_FragCoord.xy / texSize).r;
	return vec4(floor(c.rgb * ColorScale + threshold) / ColorScale, c.a);
}

vec3 sRGBtoLinear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 sRGBtoscRGBLinear(vec3 c)
{
	return pow(c, vec3(2.2)) * 1.1;
}

vec4 ApplyHdrMode(vec4 c)
{
	if (HdrMode == 0)
		return c;
	else
		return vec4(sRGBtoscRGBLinear(c.rgb), c.a);
}

float Hash(vec2 p)
{
	return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

float VignetteMask(vec2 uv)
{
	vec2 scale = max(abs(UVScale), vec2(0.0001));
	vec2 local = clamp((uv - UVOffset) / scale, 0.0, 1.0);
	vec2 centered = abs(local * 2.0 - 1.0);
	float box = max(centered.x, centered.y);
	float radial = length(centered) * 0.70710678;
	float shape = mix(box, radial, 0.35);
	float strength = clamp(VignetteStrength, 0.0, 1.0);
	float inner = mix(0.78, 0.42, strength);
	float outer = mix(1.12, 0.92, strength);
	float edge = smoothstep(inner, outer, shape);
	return max(1.0 - edge * strength * 0.88, 0.02);
}

vec3 ApplyVibrance(vec3 rgb)
{
	float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
	float maxc = max(rgb.r, max(rgb.g, rgb.b));
	float minc = min(rgb.r, min(rgb.g, rgb.b));
	float sat = maxc - minc;
	float amount = clamp(VibranceStrength, 0.0, 1.0) * (1.0 - sat);
	return clamp(mix(vec3(luma), rgb, 1.0 + amount), 0.0, 1.0);
}

vec3 ApplyWhiteBalance(vec3 rgb)
{
	float temp = clamp(WhiteBalanceTemperature, -1.0, 1.0);
	float tint = clamp(WhiteBalanceTint, -1.0, 1.0);
	vec3 balanced = rgb;
	balanced.r *= 1.0 + 0.12 * temp;
	balanced.b *= 1.0 - 0.12 * temp;
	balanced.g *= 1.0 + 0.08 * tint;
	balanced.r *= 1.0 - 0.04 * tint;
	balanced.b *= 1.0 - 0.04 * tint;
	return clamp(balanced, 0.0, 1.0);
}

vec3 ApplyLiftGammaGain(vec3 rgb)
{
	vec3 gradeGain = vec3(GradeGainR, GradeGainG, GradeGainB);
	vec3 gradeLift = vec3(GradeLiftR, GradeLiftG, GradeLiftB);
	vec3 gradeGamma = vec3(GradeGammaR, GradeGammaG, GradeGammaB);
	vec3 lifted = rgb * gradeGain + gradeLift;
	vec3 invGamma = vec3(1.0) / max(gradeGamma, vec3(0.05));
	return pow(max(lifted, vec3(0.0)), invGamma);
}

vec3 ApplyHueShift(vec3 rgb)
{
	float angle = radians(HueShiftDegrees);
	vec3 axis = vec3(0.57735027);
	float c = cos(angle);
	float s = sin(angle);
	return clamp(rgb * c + cross(axis, rgb) * s + axis * dot(axis, rgb) * (1.0 - c), 0.0, 1.0);
}

vec3 ApplyColorgrade(vec3 rgb)
{
	if (ColorgradeMode <= 0 && ColorgradeLut <= 0 || ColorgradeStrength <= 0.0)
		return rgb;

	vec3 graded = rgb;

	if (ColorgradeMode == 1)
	{
		// Warm
		graded = graded * vec3(1.10, 1.03, 0.94) + vec3(0.010, 0.004, 0.0);
	}
	else if (ColorgradeMode == 2)
	{
		// Cool
		graded = graded * vec3(0.94, 1.02, 1.12) + vec3(0.0, 0.004, 0.012);
	}
	else if (ColorgradeMode == 3)
	{
		// Filmic + muted contrast
		float lum = dot(graded, vec3(0.299, 0.587, 0.114));
		graded = mix(vec3(lum), graded, 0.65);
		graded = vec3(graded.r * 1.03, graded.g * 0.99, graded.b * 1.04);
	}
	else if (ColorgradeMode == 4)
	{
		// Bleach bypass
		float lum = dot(graded, vec3(0.2126, 0.7152, 0.0722));
		vec3 silver = mix(vec3(lum), graded, 0.34);
		graded = mix(graded, (silver - 0.5) * 1.42 + 0.5, 0.78);
	}
	else if (ColorgradeMode == 5)
	{
		// Sickly green-gray horror grade
		float lum = dot(graded, vec3(0.299, 0.587, 0.114));
		graded = mix(vec3(lum), graded, 0.28) * vec3(0.92, 1.02, 0.82) + vec3(0.020, 0.026, 0.006);
		graded = pow(clamp(graded, 0.0, 1.0), vec3(1.18, 1.10, 1.28));
	}
	else if (ColorgradeMode == 6)
	{
		// Dream decay
		float lum = dot(graded, vec3(0.2126, 0.7152, 0.0722));
		vec3 veil = mix(graded, vec3(lum) * vec3(1.08, 1.02, 0.92), 0.42);
		graded = pow(clamp(veil + vec3(0.026, 0.020, 0.012), 0.0, 1.0), vec3(0.92, 0.96, 1.08));
	}
	else if (ColorgradeMode == 7)
	{
		// Neon split
		float lum = dot(graded, vec3(0.2126, 0.7152, 0.0722));
		vec3 neon = graded * vec3(1.16, 0.88, 1.32) + vec3(0.018, 0.0, 0.035);
		graded = mix(vec3(lum) * vec3(0.75, 0.90, 1.15), neon, 0.78);
	}
	else if (ColorgradeMode == 8)
	{
		// Oxidized rust
		float lum = dot(graded, vec3(0.299, 0.587, 0.114));
		vec3 rust = vec3(lum) * vec3(1.28, 0.76, 0.46);
		graded = mix(graded * vec3(1.10, 0.92, 0.74), rust, 0.42);
	}

	if (ColorgradeLut == 1)
	{
		// Soft teal-cyan punch
		graded = vec3(
			pow(graded.r, 0.98),
			graded.g * 1.02 + 0.01,
			graded.b * 1.08 + 0.02
		);
	}
	else if (ColorgradeLut == 2)
	{
		// Warm paper emulation
		graded = vec3(
			graded.r * 1.06 + 0.02,
			graded.g * 0.97,
			graded.b * 0.94 - 0.01
		);
	}
	else if (ColorgradeLut == 3)
	{
		// High-contrast contrast-pass emulation
		float lum = dot(graded, vec3(0.2126, 0.7152, 0.0722));
		graded = mix(vec3(lum), graded, 0.72);
		graded = pow(graded, vec3(0.96));
	}
	else if (ColorgradeLut == 4)
	{
		// Faded institutional green
		float lum = dot(graded, vec3(0.299, 0.587, 0.114));
		graded = mix(vec3(lum) * vec3(0.86, 1.04, 0.78), graded, 0.38) + vec3(0.010, 0.018, 0.0);
	}
	else if (ColorgradeLut == 5)
	{
		// Cold blue print
		float lum = dot(graded, vec3(0.2126, 0.7152, 0.0722));
		graded = mix(graded, vec3(lum) * vec3(0.70, 0.86, 1.20), 0.46);
	}
	else if (ColorgradeLut == 6)
	{
		// Copper rust
		graded = graded * vec3(1.18, 0.88, 0.62) + vec3(0.024, 0.006, 0.0);
	}
	else if (ColorgradeLut == 7)
	{
		// Sodium vapor
		float lum = dot(graded, vec3(0.299, 0.587, 0.114));
		graded = mix(graded, vec3(lum) * vec3(1.38, 0.96, 0.42), 0.58);
	}
	else if (ColorgradeLut == 8)
	{
		// Silver retention
		float lum = dot(graded, vec3(0.2126, 0.7152, 0.0722));
		graded = mix(vec3(lum), graded, 0.48);
		graded = (graded - 0.5) * 1.18 + 0.5;
	}

	return mix(rgb, clamp(graded, 0.0, 1.0), ColorgradeStrength);
}

void main()
{
	vec2 uv = UVOffset + TexCoord * UVScale;
	vec2 texSize = vec2(textureSize(InputTexture, 0));

	// CRT Distortion (Pincushion)
	if (CrtMode > 0)
	{
		vec2 center = UVOffset + 0.5 * UVScale; 
		vec2 rel = TexCoord - 0.5;
		float r2 = dot(rel, rel);
		float dist = 1.0 + r2 * (CrtDistortion * 2.0); 

		// Apply Zoom (Overscan) here
		// If Zoom > 1.0, we divide the relative coordinate effectively "sampling" a smaller area
		float zoomEffect = max(CrtZoom, 0.01); // Prevent div by zero
		
		vec2 distortedTexCoord = 0.5 + (rel * dist) / zoomEffect;
		
		// If we are outside valid range, mask it?
		if (distortedTexCoord.x < 0.0 || distortedTexCoord.x > 1.0 || distortedTexCoord.y < 0.0 || distortedTexCoord.y > 1.0)
		{
			FragColor = vec4(0.0, 0.0, 0.0, 1.0);
			return;
		}

		uv = UVOffset + distortedTexCoord * UVScale;
	}

	if (RetroPixelScale > 1.0)
	{
		vec2 localUv = (uv - UVOffset) / UVScale;
		localUv = clamp(localUv, 0.0, 1.0);
		vec2 pixelStep = vec2(1.0) / texSize;
		vec2 blockSize = pixelStep * RetroPixelScale;
		localUv = floor(localUv / blockSize) * blockSize + blockSize * 0.5;
		uv = UVOffset + localUv * UVScale;
	}

	vec4 res = ApplyHdrMode(ApplyGamma(texture(InputTexture, uv)));

	// God rays (radial bright-pass blur toward a light origin above screen top-center)
	if (GodRaysStrength > 0.0)
	{
		vec2 origin = vec2(0.5, 1.15);
		vec2 delta = (origin - uv) * (0.06 * clamp(GodRaysLength, 0.25, 2.0));
		float thresh = clamp(GodRaysThreshold, 0.0, 1.0);
		vec2 suv = uv;
		vec3 rays = vec3(0.0);
		float w = 1.0;
		for (int i = 0; i < 14; i++)
		{
			suv += delta;
			vec3 s = texture(InputTexture, clamp(suv, vec2(0.001), vec2(0.999))).rgb;
			float lum = max(dot(s, vec3(0.2126, 0.7152, 0.0722)) - thresh, 0.0);
			rays += lum * w * vec3(1.0, 0.96, 0.88);
			w *= 0.86;
		}
		res.rgb += rays * GodRaysStrength * 0.12;
	}

	// Posterize (retro palette quantization)
	if (PosterizeLevels >= 2.0)
	{
		float levels = clamp(PosterizeLevels, 2.0, 16.0);
		res.rgb = clamp(floor(res.rgb * (levels - 1.0) + 0.5) / (levels - 1.0), 0.0, 1.0);
	}

	if (VignetteStrength > 0.0)
	{
		res.rgb *= VignetteMask(uv);
	}

	// NTSC / Signal Noise
	if (NtscMode > 0)
	{
		float fringe = 0.003 * CrtDistortion; 
		vec3 fringeColor;
		fringeColor.r = texture(InputTexture, uv + vec2(fringe, 0.0)).r;
		fringeColor.g = res.g;
		fringeColor.b = texture(InputTexture, uv - vec2(fringe, 0.0)).b;
		res.rgb = mix(res.rgb, fringeColor, 0.5);
	}

	// Chromatic aberration
	if (ChromaticStrength > 0.0)
	{
		vec2 texel = vec2(1.0) / texSize;
		float shift = ChromaticStrength * 1.5 * texel.x;
		vec3 chroma;
		chroma.r = texture(InputTexture, uv + vec2(shift, 0.0)).r;
		chroma.g = texture(InputTexture, uv).g;
		chroma.b = texture(InputTexture, uv - vec2(shift, 0.0)).b;
		res.rgb = mix(res.rgb, chroma, 0.7 * ChromaticStrength);
	}

	// Sharpen
	if (SharpenStrength > 0.0)
	{
		vec2 texel = vec2(1.0) / texSize;
		vec3 center = res.rgb;
		vec3 n = texture(InputTexture, uv + vec2(0.0, texel.y)).rgb;
		vec3 s = texture(InputTexture, uv - vec2(0.0, texel.y)).rgb;
		vec3 e = texture(InputTexture, uv + vec2(texel.x, 0.0)).rgb;
		vec3 w = texture(InputTexture, uv - vec2(texel.x, 0.0)).rgb;
		vec3 sharpened = center * (1.0 + 4.0 * SharpenStrength) - (n + s + e + w) * SharpenStrength;
		res.rgb = clamp(mix(center, sharpened, SharpenStrength), 0.0, 1.0);
	}

	// Clarity (local contrast from 4-tap blurred luma)
	if (ClarityStrength > 0.0)
	{
		vec2 texel3 = vec2(3.0) / texSize;
		float la = dot(texture(InputTexture, uv + vec2(texel3.x, texel3.y)).rgb, vec3(0.299, 0.587, 0.114));
		float lb = dot(texture(InputTexture, uv - vec2(texel3.x, texel3.y)).rgb, vec3(0.299, 0.587, 0.114));
		float lc = dot(texture(InputTexture, uv + vec2(texel3.x, -texel3.y)).rgb, vec3(0.299, 0.587, 0.114));
		float ld = dot(texture(InputTexture, uv + vec2(-texel3.x, texel3.y)).rgb, vec3(0.299, 0.587, 0.114));
		float avg = (la + lb + lc + ld) * 0.25;
		res.rgb = clamp(res.rgb + (res.rgb - vec3(avg)) * ClarityStrength * 0.6, 0.0, 1.0);
	}

	// Edge glow / cel outline (4-tap luma edge detect)
	if (EdgeGlowStrength > 0.0)
	{
		vec2 texel = vec2(1.0) / texSize;
		float lumaL = dot(texture(InputTexture, uv - vec2(texel.x, 0.0)).rgb, vec3(0.299, 0.587, 0.114));
		float lumaR = dot(texture(InputTexture, uv + vec2(texel.x, 0.0)).rgb, vec3(0.299, 0.587, 0.114));
		float lumaB = dot(texture(InputTexture, uv - vec2(0.0, texel.y)).rgb, vec3(0.299, 0.587, 0.114));
		float lumaT = dot(texture(InputTexture, uv + vec2(0.0, texel.y)).rgb, vec3(0.299, 0.587, 0.114));
		float edge = abs(lumaR - lumaL) + abs(lumaT - lumaB);
		float threshold = clamp(EdgeGlowThreshold, 0.0, 1.0);
		res.rgb *= 1.0 - clamp(EdgeGlowStrength, 0.0, 1.0) * smoothstep(threshold, threshold * 3.0, edge);
	}

	// Film grain (time-invariant hash noise)
	if (FilmgrainStrength > 0.0)
	{
		float scale = max(FilmgrainScale, 1.0);
		float grain = Hash((TexCoord * texSize) * scale) * 2.0 - 1.0;
		res.rgb = clamp(res.rgb + grain * FilmgrainStrength * 0.08, 0.0, 1.0);
	}

	if (VhsStrength > 0.0)
	{
		float scanLine = floor(uv.y * texSize.y);
		float fieldPhase = floor(VhsTime * 29.97);
		float lineNoise = Hash(vec2(scanLine, fieldPhase));
		float lineNoisePrev = Hash(vec2(scanLine - 1.0, fieldPhase));
		float lineNoiseNext = Hash(vec2(scanLine + 1.0, fieldPhase));
		float correlatedLineNoise = (lineNoise * 0.5 + lineNoisePrev * 0.25 + lineNoiseNext * 0.25) * 2.0 - 1.0;
		float capstan = sin(VhsTime * 1.7 + uv.y * 6.28318) * 0.45 + sin(VhsTime * 0.37 + uv.y * 17.0) * 0.25;
		float creaseBand = smoothstep(0.74, 0.98, Hash(vec2(floor(uv.y * 26.0), floor(VhsTime * 7.0) + 41.0)));
		float trackingBand = smoothstep(0.78, 1.0, fract(uv.y + VhsTime * (0.055 + VhsEvil * 0.10)));
		float headSwitch = smoothstep(0.88, 1.0, uv.y) * (0.45 + 0.55 * Hash(vec2(fieldPhase, 9.0)));
		float timebaseError = correlatedLineNoise * (0.0015 + 0.0060 * VhsJitter);
		timebaseError += capstan * (0.0010 + 0.0065 * VhsTracking);
		timebaseError += (creaseBand + trackingBand + headSwitch) * (0.004 + 0.026 * VhsTracking + 0.018 * VhsEvil);
		timebaseError *= VhsStrength;

		float verticalRoll = sin(VhsTime * 0.31 + fieldPhase * 0.07) * 0.006 * VhsTracking;
		verticalRoll += (Hash(vec2(floor(VhsTime * 1.3), 19.0)) * 2.0 - 1.0) * 0.016 * VhsEvil;
		vec2 vhsUV = clamp(uv + vec2(timebaseError, verticalRoll), vec2(0.0), vec2(1.0));

		vec2 texel = vec2(1.0) / texSize;
		vec3 c0 = texture(InputTexture, vhsUV).rgb;
		vec3 cL1 = texture(InputTexture, clamp(vhsUV - vec2(texel.x * 1.5, 0.0), vec2(0.0), vec2(1.0))).rgb;
		vec3 cR1 = texture(InputTexture, clamp(vhsUV + vec2(texel.x * 1.5, 0.0), vec2(0.0), vec2(1.0))).rgb;
		vec3 cL3 = texture(InputTexture, clamp(vhsUV - vec2(texel.x * 4.5, 0.0), vec2(0.0), vec2(1.0))).rgb;
		vec3 cR3 = texture(InputTexture, clamp(vhsUV + vec2(texel.x * 4.5, 0.0), vec2(0.0), vec2(1.0))).rgb;
		float luma = dot(c0, vec3(0.299, 0.587, 0.114));
		vec3 lowBand = c0 * 0.34 + (cL1 + cR1) * 0.23 + (cL3 + cR3) * 0.10;
		float lowLuma = dot(lowBand, vec3(0.299, 0.587, 0.114));
		vec3 chroma = lowBand - vec3(lowLuma);
		vec3 composite = vec3(luma) + chroma * (0.50 - 0.16 * VhsEvil);
		res.rgb = mix(res.rgb, composite, clamp(0.48 + 0.42 * VhsStrength, 0.0, 1.0));

		float ghostShift = (0.002 + 0.018 * VhsGhosting + 0.008 * VhsEvil) * (1.0 + headSwitch);
		vec3 ghostA = texture(InputTexture, clamp(vhsUV - vec2(ghostShift, 0.0), vec2(0.0), vec2(1.0))).rgb;
		vec3 ghostB = texture(InputTexture, clamp(vhsUV - vec2(ghostShift * 2.7, 0.0), vec2(0.0), vec2(1.0))).rgb;
		res.rgb += ghostA * (0.10 + 0.26 * VhsGhosting) * VhsStrength;
		res.rgb += ghostB * (0.03 + 0.15 * VhsGhosting) * (0.5 + VhsEvil);

		float chromaPhase = sin(scanLine * 3.14159 + fieldPhase * 0.73);
		float chromaShift = (0.001 + 0.008 * VhsGhosting + 0.006 * VhsEvil) * chromaPhase * VhsStrength;
		vec3 bleed;
		bleed.r = texture(InputTexture, clamp(vhsUV + vec2(chromaShift * 0.6, 0.0), vec2(0.0), vec2(1.0))).r;
		bleed.g = texture(InputTexture, vhsUV).g;
		bleed.b = texture(InputTexture, clamp(vhsUV - vec2(chromaShift * 1.4, 0.0), vec2(0.0), vec2(1.0))).b;
		res.rgb = mix(res.rgb, bleed, 0.24 * VhsStrength + 0.28 * VhsGhosting);

		float scanCarrier = sin(scanLine * 3.14159 * 2.0 + sin(VhsTime * 2.1) * 0.7);
		float scanEnvelope = 1.0 - VhsScanline * (0.22 + 0.28 * VhsStrength) * (0.5 + 0.5 * scanCarrier);
		res.rgb *= mix(vec3(1.0), vec3(scanEnvelope), 0.85);

		float rfNoise = Hash(vec2(gl_FragCoord.x + fieldPhase * 17.0, scanLine * 3.0)) * 2.0 - 1.0;
		float chromaNoise = Hash(vec2(floor(gl_FragCoord.x * 0.5), scanLine + fieldPhase * 5.0)) * 2.0 - 1.0;
		float snow = smoothstep(0.965 - 0.12 * VhsNoise, 1.0, Hash(vec2(gl_FragCoord.xy + fieldPhase * 13.0)));
		res.rgb += rfNoise * (0.018 + 0.090 * VhsNoise) * (0.65 + 0.35 * creaseBand);
		res.rgb += vec3(chromaNoise * 0.04, -chromaNoise * 0.015, chromaNoise * 0.055) * VhsNoise * VhsStrength;
		res.rgb += snow * vec3(0.10, 0.11, 0.09) * (0.4 + VhsNoise);

		float dropoutSeed = Hash(vec2(floor(uv.y * 95.0) + fieldPhase * 3.0, floor(VhsTime * 11.0)));
		float dropout = smoothstep(0.86 - 0.08 * VhsEvil, 1.0, dropoutSeed) * (0.45 + 0.55 * trackingBand);
		res.rgb *= 1.0 - dropout * (0.22 * VhsTracking + 0.34 * VhsEvil);
		res.rgb = mix(res.rgb, vec3(dot(res.rgb, vec3(0.299, 0.587, 0.114))), dropout * (0.35 + 0.35 * VhsEvil));

		float sickLum = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		vec3 nightVisionRot = vec3(sickLum * 0.58, sickLum * (0.86 + 0.20 * trackingBand), sickLum * 0.54);
		res.rgb = mix(res.rgb, nightVisionRot, 0.13 * VhsEvil + 0.08 * VhsNoise);
		res.rgb *= 1.0 - (0.08 + 0.16 * headSwitch) * VhsEvil;
		res.rgb += vec3(0.015, 0.028, 0.010) * (trackingBand + creaseBand) * VhsTracking;

		if (VhsPanicEnable > 0)
		{
			float panicPhase = floor(VhsTime * (4.0 + 12.0 * VhsEvil));
			float panicGate = smoothstep(0.64, 0.96, Hash(vec2(panicPhase, 113.0)));
			float rollBand = fract(uv.y + VhsTime * (0.22 + 0.95 * VhsEvil));
			float rollPulse = smoothstep(0.78, 1.0, rollBand);
			float hardTear = step(0.82, Hash(vec2(floor(uv.y * 18.0) + panicPhase, 211.0)));
			float panicShift = (Hash(vec2(panicPhase, floor(uv.y * 11.0))) * 2.0 - 1.0) *
				(0.020 + 0.065 * VhsEvil) * panicGate;

			vec2 panicUV = clamp(vhsUV + vec2(panicShift + hardTear * 0.045 * VhsEvil, rollPulse * 0.028 * VhsEvil), vec2(0.0), vec2(1.0));
			vec3 panicSample = texture(InputTexture, panicUV).rgb;
			res.rgb = mix(res.rgb, panicSample, panicGate * (0.42 + 0.40 * VhsEvil));

			float mono = dot(res.rgb, vec3(0.299, 0.587, 0.114));
			float panicMono = panicGate * (0.45 + 0.48 * VhsEvil);
			res.rgb = mix(res.rgb, vec3(mono), panicMono);

			float panicFlash = step(0.88, Hash(vec2(panicPhase, 17.0))) * (0.12 + 0.24 * VhsEvil);
			res.rgb += panicFlash * vec3(0.65, 0.72, 0.58);
			res.rgb *= 1.0 - rollPulse * (0.18 + 0.34 * VhsEvil) * panicGate;
		}

		res.rgb = clamp(res.rgb, 0.0, 1.0);
	}

	// Lens flare ghosts (mirrored bright-pass taps + halo tint)
	if (LensFlareStrength > 0.0)
	{
		vec2 center = vec2(0.5);
		vec3 flare = vec3(0.0);
		vec3 g1 = texture(InputTexture, center + (center - uv) * 0.5).rgb;
		vec3 g2 = texture(InputTexture, center + (center - uv) * 1.0).rgb;
		vec3 g3 = texture(InputTexture, center + (center - uv) * 1.5).rgb;
		flare += max(g1 - 0.8, vec3(0.0)) * vec3(0.9, 0.7, 0.5) * 0.20;
		flare += max(g2 - 0.8, vec3(0.0)) * vec3(0.5, 0.7, 0.9) * 0.12;
		flare += max(g3 - 0.8, vec3(0.0)) * vec3(0.6, 0.5, 0.9) * 0.08;
		res.rgb += flare * LensFlareStrength;
	}

	// Lift / Gamma / Gain
	if (GradeEnable > 0)
	{
		res.rgb = ApplyLiftGammaGain(res.rgb);
	}

	res.rgb = ApplyColorgrade(res.rgb);

	// White balance
	if (abs(WhiteBalanceTemperature) + abs(WhiteBalanceTint) > 0.0001)
	{
		res.rgb = ApplyWhiteBalance(res.rgb);
	}

	// Hue shift
	if (HueShiftDegrees != 0.0)
	{
		res.rgb = ApplyHueShift(res.rgb);
	}

	// Vibrance
	if (VibranceStrength > 0.0)
	{
		res.rgb = ApplyVibrance(res.rgb);
	}

	vec3 atmosphereBase = res.rgb;

	if (AtmosphereMode == 1) // Gothic
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		res.rgb = mix(vec3(gray), res.rgb, 0.2); 
		res.rgb = (res.rgb - 0.5) * 1.2 + 0.5; 
		res.rgb *= vec3(0.9, 0.9, 1.0); 
	}
	else if (AtmosphereMode == 2) // Blood
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		res.rgb = vec3(gray * 1.5, gray * 0.2, gray * 0.2); 
		res.rgb = (res.rgb - 0.5) * 1.3 + 0.5; 
	}
	else if (AtmosphereMode == 3) // Sepia
	{
		vec3 sepia;
		sepia.r = dot(res.rgb, vec3(0.393, 0.769, 0.189));
		sepia.g = dot(res.rgb, vec3(0.349, 0.686, 0.168));
		sepia.b = dot(res.rgb, vec3(0.272, 0.534, 0.131));
		res.rgb = sepia;
	}
	else if (AtmosphereMode == 4) // Toxic
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		vec3 toxic = vec3(gray * 0.8, gray * 1.4, gray * 0.4); 
		res.rgb = mix(res.rgb, toxic, 0.9);
		res.rgb = (res.rgb - 0.5) * 1.3 + 0.5; 
	}
	else if (AtmosphereMode == 5) // Hellfire
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		vec3 fire = vec3(gray * 1.6, gray * 0.6, gray * 0.1); 
		res.rgb = mix(res.rgb, fire, 0.95);
		res.rgb = (res.rgb - 0.5) * 1.4 + 0.5; 
	}
	else if (AtmosphereMode == 6) // Cyberpunk
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		res.rgb = mix(vec3(gray), res.rgb, 1.5); 
		res.rgb *= vec3(1.1, 0.8, 1.4); 
		res.rgb = pow(res.rgb, vec3(0.9)); 
	}
	else if (AtmosphereMode == 7) // Fogbound
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		vec3 fog = vec3(gray) * vec3(0.90, 0.98, 0.84) + vec3(0.030, 0.036, 0.018);
		res.rgb = mix(res.rgb, fog, 0.72);
		res.rgb = (res.rgb - 0.5) * 0.82 + 0.5;
	}
	else if (AtmosphereMode == 8) // Bleak Blue
	{
		float gray = dot(res.rgb, vec3(0.2126, 0.7152, 0.0722));
		vec3 bleak = mix(vec3(gray) * vec3(0.58, 0.72, 1.05), res.rgb * vec3(0.72, 0.86, 1.16), 0.38);
		res.rgb = pow(clamp(bleak, 0.0, 1.0), vec3(1.18, 1.08, 0.96));
	}
	else if (AtmosphereMode == 9) // Otherworld
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		vec3 rust = vec3(gray) * vec3(1.35, 0.58, 0.34);
		res.rgb = mix(res.rgb * vec3(1.12, 0.78, 0.62), rust, 0.64);
		res.rgb = (res.rgb - 0.5) * 1.28 + 0.5;
	}
	else if (AtmosphereMode == 10) // Sodium Vapor
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		vec3 sodium = vec3(gray) * vec3(1.42, 0.94, 0.36);
		res.rgb = mix(res.rgb, sodium, 0.62);
	}

	// Apply configurable intensity (blend between original and effect)
	if (AtmosphereMode > 0)
	{
		res.rgb = mix(atmosphereBase, res.rgb, AtmosphereIntensity);
		res.rgb = (res.rgb - 0.5) * AtmosphereContrast + 0.5;
	}

	// CRT Scanlines and Masks
	if (CrtMode > 0)
	{
		// Sharpness-tuned Scanlines
		float scanlineCount = textureSize(InputTexture, 0).y * CrtScanlineDensity;
		if (scanlineCount < 50.0) scanlineCount = 200.0 * CrtScanlineDensity;

		float sc = uv.y * scanlineCount * 3.14159 * 2.0;
		float scanline = sin(sc);
		
		// Apply Sharpness: Scale sine range and clamp
		// Sharpness 1.0 (Standard) -> -1 to 1 sin wave
		// Sharpness > 1.0 -> Squares off the wave
		if (CrtScanlineSharpness > 1.0)
		{
			scanline = clamp(scanline * CrtScanlineSharpness, -1.0, 1.0);
		}
		
		vec3 scanlineColor = vec3(0.5 + 0.5 * scanline);
		res.rgb *= mix(vec3(1.0), scanlineColor, 0.5 * CrtScanline); // Intensity

		// Phosphor / Shadow Masks based on Mode
		// Mode 1: Standard (No Mask, just scanlines)
		// Mode 2: Aperture Grille (Vertical stripes)
		if (CrtMode == 2)
		{
			float pixelX = uv.x * textureSize(InputTexture, 0).x * 2.0; // Higher density horizontal
			float mask = 0.5 + 0.5 * sin(pixelX * 3.14159);
			// RGB subsampling?
			vec3 maskColor = vec3(mask);
			res.rgb *= mix(vec3(1.0), maskColor, CrtMaskIntensity);
		}
		// Mode 3: Shadow Mask (Dots)
		else if (CrtMode >= 3)
		{
			float pixelX = uv.x * textureSize(InputTexture, 0).x * 3.0; // Dense dots
			float pixelY = uv.y * textureSize(InputTexture, 0).y * 1.5;
			
			float maskX = sin(pixelX * 3.14159);
			float maskY = sin(pixelY * 3.14159);
			float mask = 0.5 + 0.5 * (maskX * maskY);
			
			res.rgb *= mix(vec3(1.0), vec3(mask), CrtMaskIntensity);
		}
	}

	FragColor = Dither(res);
}
