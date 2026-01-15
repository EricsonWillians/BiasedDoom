
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

void main()
{
	vec4 res = ApplyHdrMode(ApplyGamma(texture(InputTexture, UVOffset + TexCoord * UVScale)));
	vec3 original = res.rgb;

	if (AtmosphereMode == 1) // Gothic
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		res.rgb = mix(vec3(gray), res.rgb, 0.2); // Highly desaturated
		res.rgb = (res.rgb - 0.5) * 1.2 + 0.5; // Contrast
		res.rgb *= vec3(0.9, 0.9, 1.0); // Slight blue tint
	}
	else if (AtmosphereMode == 2) // Blood
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		res.rgb = vec3(gray * 1.5, gray * 0.2, gray * 0.2); // Strong red
		res.rgb = (res.rgb - 0.5) * 1.3 + 0.5; // High contrast
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
		vec3 toxic = vec3(gray * 0.8, gray * 1.4, gray * 0.4); // Radioactive green/yellow
		res.rgb = mix(res.rgb, toxic, 0.9);
		res.rgb = (res.rgb - 0.5) * 1.3 + 0.5; // High contrast
	}
	else if (AtmosphereMode == 5) // Hellfire
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		vec3 fire = vec3(gray * 1.6, gray * 0.6, gray * 0.1); // Deep red/orange
		res.rgb = mix(res.rgb, fire, 0.95);
		res.rgb = (res.rgb - 0.5) * 1.4 + 0.5; // Very high contrast
	}
	else if (AtmosphereMode == 6) // Cyberpunk
	{
		float gray = dot(res.rgb, vec3(0.299, 0.587, 0.114));
		res.rgb = mix(vec3(gray), res.rgb, 1.5); // Boost saturation
		res.rgb *= vec3(1.1, 0.8, 1.4); // Shift towards purple/pink/blue
		res.rgb = pow(res.rgb, vec3(0.9)); // Slight gamma shift for "glow" feel
	}

	// Apply configurable intensity (blend between original and effect)
	if (AtmosphereMode > 0)
	{
		res.rgb = mix(original, res.rgb, AtmosphereIntensity);
		
		// Apply configurable contrast
		res.rgb = (res.rgb - 0.5) * AtmosphereContrast + 0.5;
	}

	FragColor = Dither(res);
}
