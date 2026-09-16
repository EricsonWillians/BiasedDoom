
vec3 lightContribution(int i, vec3 normal)
{
	vec4 lightpos = lights[i];
	vec4 lightcolor = lights[i+1];
	vec4 lightspot1 = lights[i+2];
	vec4 lightspot2 = lights[i+3];

	float lightdistance = distance(lightpos.xyz, pixelpos.xyz);
	if (BiasedLightRadius(lightpos.w) < lightdistance)
		return vec3(0.0); // Early out lights touching surface but not this fragment

	vec3 lightdir = normalize(lightpos.xyz - pixelpos.xyz);
	float dotprod = dot(normal, lightdir);
	if (dotprod < -clamp(uDynLightWrap, 0.0, 0.95) - 0.0001) return vec3(0.0);	// light hits from the backside. This can happen with full sector light lists and must be rejected for all cases. Note that this can cause precision issues.

	float attenuation = BiasedLightAttenuation(lightdistance, lightpos.w);

	if (lightspot1.w == 1.0)
		attenuation *= spotLightAttenuation(lightpos, lightspot1.xyz, lightspot2.x, lightspot2.y);

	if (lightcolor.a < 0.0) // Sign bit is the attenuated light flag
	{
		attenuation *= BiasedNormalLightFactor(dotprod);
	}

	if (attenuation > 0.0) // Skip shadow map test if possible
	{
		attenuation *= shadowAttenuation(lightpos, lightcolor.a);
		return ApplyBiasedDynamicLight(lightcolor.rgb * attenuation);
	}
	else
	{
		return vec3(0.0);
	}
}

vec3 lightIndirectContribution(int i)
{
	if (uDynLightIndirect <= 0.0)
		return vec3(0.0);

	vec4 lightpos = lights[i];
	vec4 lightcolor = lights[i+1];
	vec4 lightspot1 = lights[i+2];
	vec4 lightspot2 = lights[i+3];

	float lightdistance = distance(lightpos.xyz, pixelpos.xyz);
	if (BiasedLightRadius(lightpos.w) < lightdistance)
		return vec3(0.0);

	float attenuation = BiasedLightAttenuation(lightdistance, lightpos.w);
	if (lightspot1.w == 1.0)
		attenuation *= spotLightAttenuation(lightpos, lightspot1.xyz, lightspot2.x, lightspot2.y);

	return ApplyBiasedIndirectLight(lightcolor.rgb * attenuation);
}

vec3 ProcessMaterialLight(Material material, vec3 color)
{
	vec4 dynlight = vec4(ApplyBiasedDynamicLight(uDynLightColor.rgb), uDynLightColor.a);
	vec3 normal = material.Normal;

	if (uLightIndex >= 0)
	{
		ivec4 lightRange = ivec4(lights[uLightIndex]) + ivec4(uLightIndex + 1);
		if (lightRange.z > lightRange.x)
		{
			// modulated lights
			for(int i=lightRange.x; i<lightRange.y; i+=4)
			{
				dynlight.rgb += lightContribution(i, normal);
				dynlight.rgb += lightIndirectContribution(i);
			}

			// subtractive lights
			for(int i=lightRange.y; i<lightRange.z; i+=4)
			{
				dynlight.rgb -= lightContribution(i, normal);
				dynlight.rgb -= lightIndirectContribution(i);
			}
		}
	}

	vec3 frag;

	if ( uLightBlendMode == 1 )
	{	// COLOR_CORRECT_CLAMPING 
	vec3 lightcolor = ApplyBiasedAmbientFloor(ApplyBiasedAmbientGradient(color + desaturate(dynlight).rgb + vec3(uGIAmbientStrength), normal));
		frag = material.Base.rgb * ((lightcolor / max(max(max(lightcolor.r, lightcolor.g), lightcolor.b), 1.4) * 1.4));
	}
	else if ( uLightBlendMode == 2 )
	{	// UNCLAMPED 
	frag = material.Base.rgb * ApplyBiasedAmbientFloor(ApplyBiasedAmbientGradient(color + desaturate(dynlight).rgb + vec3(uGIAmbientStrength), normal));
	}
	else
	{
	frag = material.Base.rgb * clamp(ApplyBiasedAmbientFloor(ApplyBiasedAmbientGradient(color + desaturate(dynlight).rgb + vec3(uGIAmbientStrength), normal)), 0.0, 1.4);
	}

	if (uLightIndex >= 0)
	{
		ivec4 lightRange = ivec4(lights[uLightIndex]) + ivec4(uLightIndex + 1);
		if (lightRange.w > lightRange.z)
		{
			vec4 addlight = vec4(0.0,0.0,0.0,0.0);

			// additive lights
			for(int i=lightRange.z; i<lightRange.w; i+=4)
			{
				addlight.rgb += lightContribution(i, normal);
				addlight.rgb += lightIndirectContribution(i);
			}

			frag = clamp(frag + desaturate(addlight).rgb, 0.0, 1.0);
		}
	}

	return frag;
}
