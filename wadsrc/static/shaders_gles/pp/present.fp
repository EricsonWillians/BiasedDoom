
varying vec2 TexCoord;

uniform sampler2D InputTexture;
uniform sampler2D DitherTexture;

vec4 ApplyGamma(vec4 c)
{
	c.rgb = min(c.rgb, vec3(2.0));

	vec3 valgray;
	if (GrayFormula == 0)
		valgray = vec3(c.r + c.g + c.b) * (1 - Saturation) / 3 + c.rgb * Saturation;
	else if (GrayFormula == 2)
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
	float threshold = texture2D(DitherTexture, gl_FragCoord.xy / texSize).r;
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
		graded = vec3(graded.r * 1.08, graded.g * 1.02, graded.b * 0.96);
		graded = pow(graded, vec3(1.0 - 0.02 * ColorgradeStrength));
	}
	else if (ColorgradeMode == 2)
	{
		graded = vec3(graded.r * 0.96, graded.g * 1.02, graded.b * 1.08);
	}
	else
	{
		float lum = dot(graded, vec3(0.299, 0.587, 0.114));
		graded = mix(vec3(lum), graded, 0.65);
		graded = vec3(graded.r * 1.03, graded.g * 0.99, graded.b * 1.04);
	}

	if (ColorgradeLut == 1)
	{
		graded = vec3(
			pow(graded.r, 0.98),
			graded.g * 1.02 + 0.01,
			graded.b * 1.08 + 0.02
		);
	}
	else if (ColorgradeLut == 2)
	{
		graded = vec3(
			graded.r * 1.06 + 0.02,
			graded.g * 0.97,
			graded.b * 0.94 - 0.01
		);
	}
	else if (ColorgradeLut == 3)
	{
		float lum = dot(graded, vec3(0.2126, 0.7152, 0.0722));
		graded = mix(vec3(lum), graded, 0.72);
		graded = pow(graded, vec3(0.96));
	}

	return mix(rgb, clamp(graded, 0.0, 1.0), ColorgradeStrength);
}

void main()
{
	vec2 uv = UVOffset + TexCoord * UVScale;
	vec2 texSize = vec2(textureSize(InputTexture, 0));

	if (CrtMode > 0)
	{
		vec2 center = UVOffset + 0.5 * UVScale;
		vec2 rel = TexCoord - 0.5;
		float r2 = dot(rel, rel);
		float dist = 1.0 + r2 * (CrtDistortion * 2.0);
		float zoomEffect = max(CrtZoom, 0.01);
		vec2 distortedTexCoord = 0.5 + (rel * dist) / zoomEffect;

		if (distortedTexCoord.x < 0.0 || distortedTexCoord.x > 1.0 || distortedTexCoord.y < 0.0 || distortedTexCoord.y > 1.0)
		{
			gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
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

	vec4 res = ApplyHdrMode(ApplyGamma(texture2D(InputTexture, uv)));

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
			vec3 s = texture2D(InputTexture, clamp(suv, vec2(0.001), vec2(0.999))).rgb;
			float lum = max(dot(s, vec3(0.2126, 0.7152, 0.0722)) - thresh, 0.0);
			rays += lum * w * vec3(1.0, 0.96, 0.88);
			w *= 0.86;
		}
		res.rgb += rays * GodRaysStrength * 0.12;
	}

	vec3 original = res.rgb;

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

	if (NtscMode > 0)
	{
		float fringe = 0.003 * CrtDistortion;
		vec3 fringeColor;
		fringeColor.r = texture2D(InputTexture, uv + vec2(fringe, 0.0)).r;
		fringeColor.g = res.g;
		fringeColor.b = texture2D(InputTexture, uv - vec2(fringe, 0.0)).b;
		res.rgb = mix(res.rgb, fringeColor, 0.5);
	}

	if (ChromaticStrength > 0.0)
	{
		vec2 texel = vec2(1.0) / texSize;
		float shift = ChromaticStrength * 1.5 * texel.x;
		vec3 chroma;
		chroma.r = texture2D(InputTexture, uv + vec2(shift, 0.0)).r;
		chroma.g = texture2D(InputTexture, uv).g;
		chroma.b = texture2D(InputTexture, uv - vec2(shift, 0.0)).b;
		res.rgb = mix(res.rgb, chroma, 0.7 * ChromaticStrength);
	}

	if (SharpenStrength > 0.0)
	{
		vec2 texel = vec2(1.0) / texSize;
		vec3 center = res.rgb;
		vec3 n = texture2D(InputTexture, uv + vec2(0.0, texel.y)).rgb;
		vec3 s = texture2D(InputTexture, uv - vec2(0.0, texel.y)).rgb;
		vec3 e = texture2D(InputTexture, uv + vec2(texel.x, 0.0)).rgb;
		vec3 w = texture2D(InputTexture, uv - vec2(texel.x, 0.0)).rgb;
		vec3 sharpened = center * (1.0 + 4.0 * SharpenStrength) - (n + s + e + w) * SharpenStrength;
		res.rgb = clamp(mix(center, sharpened, SharpenStrength), 0.0, 1.0);
	}

	// Clarity (local contrast from 4-tap blurred luma)
	if (ClarityStrength > 0.0)
	{
		vec2 texel3 = vec2(3.0) / texSize;
		float la = dot(texture2D(InputTexture, uv + vec2(texel3.x, texel3.y)).rgb, vec3(0.299, 0.587, 0.114));
		float lb = dot(texture2D(InputTexture, uv - vec2(texel3.x, texel3.y)).rgb, vec3(0.299, 0.587, 0.114));
		float lc = dot(texture2D(InputTexture, uv + vec2(texel3.x, -texel3.y)).rgb, vec3(0.299, 0.587, 0.114));
		float ld = dot(texture2D(InputTexture, uv + vec2(-texel3.x, texel3.y)).rgb, vec3(0.299, 0.587, 0.114));
		float avg = (la + lb + lc + ld) * 0.25;
		res.rgb = clamp(res.rgb + (res.rgb - vec3(avg)) * ClarityStrength * 0.6, 0.0, 1.0);
	}

	// Edge glow / cel outline (4-tap luma edge detect)
	if (EdgeGlowStrength > 0.0)
	{
		vec2 texel = vec2(1.0) / texSize;
		float lumaL = dot(texture2D(InputTexture, uv - vec2(texel.x, 0.0)).rgb, vec3(0.299, 0.587, 0.114));
		float lumaR = dot(texture2D(InputTexture, uv + vec2(texel.x, 0.0)).rgb, vec3(0.299, 0.587, 0.114));
		float lumaB = dot(texture2D(InputTexture, uv - vec2(0.0, texel.y)).rgb, vec3(0.299, 0.587, 0.114));
		float lumaT = dot(texture2D(InputTexture, uv + vec2(0.0, texel.y)).rgb, vec3(0.299, 0.587, 0.114));
		float edge = abs(lumaR - lumaL) + abs(lumaT - lumaB);
		float threshold = clamp(EdgeGlowThreshold, 0.0, 1.0);
		res.rgb *= 1.0 - clamp(EdgeGlowStrength, 0.0, 1.0) * smoothstep(threshold, threshold * 3.0, edge);
	}

	if (FilmgrainStrength > 0.0)
	{
		float scale = max(FilmgrainScale, 1.0);
		float grain = Hash((TexCoord * texSize) * scale) * 2.0 - 1.0;
		res.rgb = clamp(res.rgb + grain * FilmgrainStrength * 0.08, 0.0, 1.0);
	}

	if (VhsStrength > 0.0)
	{
		float scanLine = floor(uv.y * texSize.y);
		float tapeWave = sin(uv.y * 24.0 + VhsTime * 3.0) * 0.5 + 0.5;
		float rowJitter = Hash(vec2(scanLine, floor(VhsTime * 18.0))) * 2.0 - 1.0;
		float trackingBand = smoothstep(0.82, 1.0, fract(uv.y + VhsTime * (0.07 + VhsEvil * 0.08)));
		float holdDrift = sin(VhsTime * (0.9 + VhsEvil * 2.4) + uv.y * (8.0 + VhsTracking * 20.0));
		float horizontalRip = sin(uv.y * (120.0 + VhsTracking * 220.0) + VhsTime * (22.0 + VhsEvil * 35.0));
		float jitter = rowJitter * VhsJitter * 0.004 * VhsStrength;
		jitter += holdDrift * VhsTracking * 0.010 * VhsStrength;
		jitter += horizontalRip * trackingBand * (0.008 + 0.024 * VhsTracking) * VhsStrength;
		jitter += (Hash(vec2(floor(uv.y * 48.0), floor(VhsTime * 4.0))) * 2.0 - 1.0) * VhsEvil * 0.016 * VhsStrength;

		float verticalRoll = (Hash(vec2(floor(VhsTime * 1.6), 19.0)) * 2.0 - 1.0) * 0.018 * VhsEvil;
		vec2 vhsUV = clamp(uv + vec2(jitter, verticalRoll), vec2(0.0), vec2(1.0));
		vec4 vhs = texture2D(InputTexture, vhsUV);
		res.rgb = mix(res.rgb, vhs.rgb, 0.40 + 0.30 * VhsStrength);

		float ghostShiftA = (0.0015 + 0.010 * VhsGhosting) * (1.0 + 0.8 * VhsStrength);
		float ghostShiftB = ghostShiftA * (1.8 + 1.5 * VhsEvil);
		vec3 ghostA = texture2D(InputTexture, clamp(vhsUV - vec2(ghostShiftA, 0.0), vec2(0.0), vec2(1.0))).rgb;
		vec3 ghostB = texture2D(InputTexture, clamp(vhsUV - vec2(ghostShiftB, 0.0), vec2(0.0), vec2(1.0))).rgb;
		res.rgb = mix(res.rgb, ghostA, 0.18 * VhsGhosting + 0.10 * VhsStrength);
		res.rgb += ghostB * (0.10 * VhsGhosting * (0.6 + 0.8 * VhsEvil));

		float chromaShift = (0.0015 + 0.006 * VhsGhosting + 0.003 * VhsEvil) * VhsStrength;
		vec3 bleed;
		bleed.r = texture2D(InputTexture, clamp(vhsUV + vec2(chromaShift, 0.0), vec2(0.0), vec2(1.0))).r;
		bleed.g = texture2D(InputTexture, clamp(vhsUV + vec2(chromaShift * 0.25, 0.0), vec2(0.0), vec2(1.0))).g;
		bleed.b = texture2D(InputTexture, clamp(vhsUV - vec2(chromaShift * 1.35, 0.0), vec2(0.0), vec2(1.0))).b;
		res.rgb = mix(res.rgb, bleed, 0.25 * VhsStrength + 0.35 * VhsGhosting);

		float vhsScan = 1.0 - (VhsScanline * (0.35 + 0.45 * VhsEvil)) *
			(0.5 + 0.5 * sin(uv.y * texSize.y * 3.14159 * 2.0 + VhsTime * (8.0 + 10.0 * VhsTracking)));
		res.rgb *= mix(vec3(1.0), vec3(vhsScan), 0.75 + 0.20 * VhsStrength);

		float staticNoise = Hash((TexCoord * texSize) * (1.0 + VhsNoise * 3.0) + vec2(floor(VhsTime * 90.0), floor(VhsTime * 53.0))) * 2.0 - 1.0;
		float snowStripe = smoothstep(0.92, 1.0, Hash(vec2(floor(uv.y * 120.0), floor(VhsTime * 8.0) + 31.0)));
		res.rgb += staticNoise * (0.03 + 0.14 * VhsNoise) * (0.5 + 0.5 * tapeWave);
		res.rgb += snowStripe * (0.04 + 0.16 * VhsNoise) * vec3(1.0, 1.0, 1.0);

		float dropout = smoothstep(0.84, 1.0, Hash(vec2(floor(uv.y * 60.0) + floor(VhsTime * 12.0), 77.0)));
		res.rgb *= 1.0 - dropout * (0.18 * VhsTracking + 0.24 * VhsEvil) * trackingBand;

		float sickLum = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		vec3 haunted = vec3(sickLum * 0.72, sickLum * (0.92 + 0.20 * tapeWave), sickLum * 0.68);
		res.rgb = mix(res.rgb, haunted, 0.08 * VhsEvil + 0.10 * VhsNoise);
		res.rgb *= 1.0 - 0.10 * VhsEvil;
		res.rgb += vec3(0.02, 0.03, 0.01) * trackingBand * VhsTracking;

		if (VhsPanicEnable > 0)
		{
			float panicPhase = floor(VhsTime * (5.0 + 10.0 * VhsEvil));
			float panicGate = smoothstep(0.72, 0.98, Hash(vec2(panicPhase, 113.0)));
			float rollBand = fract(uv.y + VhsTime * (0.35 + 0.65 * VhsEvil));
			float rollPulse = smoothstep(0.82, 1.0, rollBand);
			float hardTear = step(0.88, Hash(vec2(floor(uv.y * 16.0) + panicPhase, 211.0)));
			float panicShift = (Hash(vec2(panicPhase, floor(uv.y * 9.0))) * 2.0 - 1.0) *
				(0.015 + 0.045 * VhsEvil) * panicGate;

			vec2 panicUV = clamp(vhsUV + vec2(panicShift + hardTear * 0.03 * VhsEvil, rollPulse * 0.02 * VhsEvil), vec2(0.0), vec2(1.0));
			vec3 panicSample = texture2D(InputTexture, panicUV).rgb;
			res.rgb = mix(res.rgb, panicSample, panicGate * (0.35 + 0.35 * VhsEvil));

			float mono = dot(res.rgb, vec3(0.299, 0.587, 0.114));
			float panicMono = panicGate * (0.35 + 0.45 * VhsEvil);
			res.rgb = mix(res.rgb, vec3(mono), panicMono);

			float panicFlash = step(0.90, Hash(vec2(panicPhase, 17.0))) * (0.18 + 0.32 * VhsEvil);
			res.rgb += panicFlash * vec3(0.9, 0.95, 1.0);
			res.rgb *= 1.0 - rollPulse * (0.12 + 0.28 * VhsEvil) * panicGate;
		}

		res.rgb = clamp(res.rgb, 0.0, 1.0);
	}

	// Lens flare ghosts (mirrored bright-pass taps + halo tint)
	if (LensFlareStrength > 0.0)
	{
		vec2 center = vec2(0.5);
		vec3 flare = vec3(0.0);
		vec3 g1 = texture2D(InputTexture, center + (center - uv) * 0.5).rgb;
		vec3 g2 = texture2D(InputTexture, center + (center - uv) * 1.0).rgb;
		vec3 g3 = texture2D(InputTexture, center + (center - uv) * 1.5).rgb;
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

	if (AtmosphereMode == 1)
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		res.rgb = mix(vec3(gray), res.rgb, 0.2);
		res.rgb = (res.rgb - 0.5) * 1.2 + 0.5;
		res.rgb *= vec3(0.9, 0.9, 1.0);
	}
	else if (AtmosphereMode == 2)
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		res.rgb = vec3(gray * 1.5, gray * 0.2, gray * 0.2);
		res.rgb = (res.rgb - 0.5) * 1.3 + 0.5;
	}
	else if (AtmosphereMode == 3)
	{
		vec3 sepia;
		sepia.r = dot(res.rgb, vec3(0.393, 0.769, 0.189));
		sepia.g = dot(res.rgb, vec3(0.349, 0.686, 0.168));
		sepia.b = dot(res.rgb, vec3(0.272, 0.534, 0.131));
		res.rgb = sepia;
	}
	else if (AtmosphereMode == 4)
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		vec3 toxic = vec3(gray * 0.8, gray * 1.4, gray * 0.4);
		res.rgb = mix(res.rgb, toxic, 0.9);
		res.rgb = (res.rgb - 0.5) * 1.3 + 0.5;
	}
	else if (AtmosphereMode == 5)
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		vec3 fire = vec3(gray * 1.6, gray * 0.6, gray * 0.1);
		res.rgb = mix(res.rgb, fire, 0.95);
		res.rgb = (res.rgb - 0.5) * 1.4 + 0.5;
	}
	else if (AtmosphereMode == 6)
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		res.rgb = mix(vec3(gray), res.rgb, 1.5);
		res.rgb *= vec3(1.1, 0.8, 1.4);
		res.rgb = pow(res.rgb, vec3(0.9));
	}

	if (AtmosphereMode > 0)
	{
		res.rgb = mix(original, res.rgb, AtmosphereIntensity);
		res.rgb = (res.rgb - 0.5) * AtmosphereContrast + 0.5;
	}

	if (CrtMode > 0)
	{
		float scanlineCount = textureSize(InputTexture, 0).y * CrtScanlineDensity;
		if (scanlineCount < 50.0) scanlineCount = 200.0 * CrtScanlineDensity;

		float sc = uv.y * scanlineCount * 3.14159 * 2.0;
		float scanline = sin(sc);
		if (CrtScanlineSharpness > 1.0)
		{
			scanline = clamp(scanline * CrtScanlineSharpness, -1.0, 1.0);
		}

		vec3 scanlineColor = vec3(0.5 + 0.5 * scanline);
		res.rgb *= mix(vec3(1.0), scanlineColor, 0.5 * CrtScanline);

		if (CrtMode == 2)
		{
			float pixelX = uv.x * textureSize(InputTexture, 0).x * 2.0;
			float mask = 0.5 + 0.5 * sin(pixelX * 3.14159);
			vec3 maskColor = vec3(mask);
			res.rgb *= mix(vec3(1.0), maskColor, CrtMaskIntensity);
		}
		else if (CrtMode >= 3)
		{
			float pixelX = uv.x * textureSize(InputTexture, 0).x * 3.0;
			float pixelY = uv.y * textureSize(InputTexture, 0).y * 1.5;
			float maskX = sin(pixelX * 3.14159);
			float maskY = sin(pixelY * 3.14159);
			float mask = 0.5 + 0.5 * (maskX * maskY);
			res.rgb *= mix(vec3(1.0), vec3(mask), CrtMaskIntensity);
		}
	}

	gl_FragColor = Dither(res);
}
