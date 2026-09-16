
const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = max(dot(N, H), 0.0);
	float NdotH2 = NdotH*NdotH;

	float nom = a2;
	float denom = (NdotH2 * (a2 - 1.0) + 1.0);
	denom = PI * denom * denom;

	return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
	float r = (roughness + 1.0);
	float k = (r * r) / 8.0;

	float nom = NdotV;
	float denom = NdotV * (1.0 - k) + k;

	return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
	float NdotV = max(dot(N, V), 0.0);
	float NdotL = max(dot(N, L), 0.0);
	float ggx2 = GeometrySchlickGGX(NdotV, roughness);
	float ggx1 = GeometrySchlickGGX(NdotL, roughness);
	return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
	return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
	return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - cosTheta, 5.0);
}

float quadraticDistanceAttenuation(vec4 lightpos)
{
	vec3 distVec = lightpos.xyz - pixelpos.xyz;
	float lightdistance = length(distVec);
	if (lightdistance >= BiasedLightRadius(lightpos.w))
		return 0.0;

	float attenuation = 1.0 / (1.0 + (lightdistance / BiasedLightRadius(lightpos.w)));
	return attenuation;
}

float DynamicLightAttenuation(float lightdistance, float lightradius)
{
	return BiasedLightAttenuation(lightdistance, lightradius);
}

float linearDistanceAttenuation(vec4 lightpos)
{
	float lightdistance = distance(lightpos.xyz, pixelpos.xyz);
	return DynamicLightAttenuation(lightdistance, lightpos.w);
}

vec3 ProcessMaterialLight(Material material, vec3 ambientLight)
{
	vec3 worldpos = pixelpos.xyz;

	vec3 albedo = pow(material.Base.rgb, vec3(2.2)); // sRGB to linear
	ambientLight = pow(ambientLight, vec3(2.2));
	ambientLight = ApplyBiasedAmbientFloor(ambientLight + vec3(uGIAmbientStrength));

	float metallic = material.Metallic;
	float roughness = clamp(material.Roughness / max(uSpecularPowerScale, 0.25), 0.045, 1.0);
	float ao = material.AO;

	vec3 N = material.Normal;
	vec3 V = normalize(uCameraPos.xyz - worldpos);

	ambientLight = ApplyBiasedAmbientGradient(ambientLight, N);

	vec3 F0 = mix(vec3(0.04), albedo, metallic);

	vec3 Lo = ApplyBiasedDynamicLight(uDynLightColor.rgb);

	if (uLightIndex >= 0)
	{
		ivec4 lightRange = ivec4(lights[uLightIndex]) + ivec4(uLightIndex + 1);
		if (lightRange.z > lightRange.x)
		{
			//
			// modulated lights
			//
			for(int i=lightRange.x; i<lightRange.y; i+=4)
			{
				vec4 lightpos = lights[i];
				vec4 lightcolor = lights[i+1];
				vec4 lightspot1 = lights[i+2];
				vec4 lightspot2 = lights[i+3];

				vec3 L = normalize(lightpos.xyz - worldpos);
				vec3 H = normalize(V + L);

				float lightdistance = distance(lightpos.xyz, worldpos);
				float attenuation = DynamicLightAttenuation(lightdistance, lightpos.w);
				float indirect = attenuation * max(uDynLightIndirect, 0.0);
				if (lightspot1.w == 1.0)
				{
					attenuation *= spotLightAttenuation(lightpos, lightspot1.xyz, lightspot2.x, lightspot2.y);
					indirect *= spotLightAttenuation(lightpos, lightspot1.xyz, lightspot2.x, lightspot2.y);
				}
				if (lightcolor.a < 0.0)
					attenuation *= BiasedNormalLightFactor(dot(N, L)); // Sign bit is the attenuated light flag

				if (attenuation > 0.0)
				{
					attenuation *= shadowAttenuation(lightpos, lightcolor.a);

					vec3 radiance = ApplyBiasedDynamicLight(lightcolor.rgb * attenuation);

					// cook-torrance brdf
					float NDF = DistributionGGX(N, H, roughness);
					float G = GeometrySmith(N, V, L, roughness);
					vec3 F = fresnelSchlick(clamp(dot(H, V), 0.0, 1.0), F0);

					vec3 kS = F;
					vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

					vec3 nominator = NDF * G * F;
					float denominator = 4.0 * clamp(dot(N, V), 0.0, 1.0) * clamp(dot(N, L), 0.0, 1.0);
					vec3 specular = (nominator / max(denominator, 0.001)) * uSpecularTintColor.rgb;

					Lo += (kD * albedo / PI + ApplyBiasedSpecularLight(specular)) * radiance;
				}

				if (indirect > 0.0)
					Lo += albedo * ApplyBiasedDynamicLight(lightcolor.rgb * indirect) * (1.0 / PI);
			}
			//
			// subtractive lights
			//
			for(int i=lightRange.y; i<lightRange.z; i+=4)
			{
				vec4 lightpos = lights[i];
				vec4 lightcolor = lights[i+1];
				vec4 lightspot1 = lights[i+2];
				vec4 lightspot2 = lights[i+3];

				vec3 L = normalize(lightpos.xyz - worldpos);
				vec3 H = normalize(V + L);

				float lightdistance = distance(lightpos.xyz, worldpos);
				float attenuation = DynamicLightAttenuation(lightdistance, lightpos.w);
				float indirect = attenuation * max(uDynLightIndirect, 0.0);
				if (lightspot1.w == 1.0)
				{
					attenuation *= spotLightAttenuation(lightpos, lightspot1.xyz, lightspot2.x, lightspot2.y);
					indirect *= spotLightAttenuation(lightpos, lightspot1.xyz, lightspot2.x, lightspot2.y);
				}
				if (lightcolor.a < 0.0)
					attenuation *= BiasedNormalLightFactor(dot(N, L)); // Sign bit is the attenuated light flag

				if (attenuation > 0.0)
				{
					attenuation *= shadowAttenuation(lightpos, lightcolor.a);

					vec3 radiance = ApplyBiasedDynamicLight(lightcolor.rgb * attenuation);

					// cook-torrance brdf
					float NDF = DistributionGGX(N, H, roughness);
					float G = GeometrySmith(N, V, L, roughness);
					vec3 F = fresnelSchlick(clamp(dot(H, V), 0.0, 1.0), F0);

					vec3 kS = F;
					vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

					vec3 nominator = NDF * G * F;
					float denominator = 4.0 * clamp(dot(N, V), 0.0, 1.0) * clamp(dot(N, L), 0.0, 1.0);
					vec3 specular = (nominator / max(denominator, 0.001)) * uSpecularTintColor.rgb;

					Lo -= (kD * albedo / PI + ApplyBiasedSpecularLight(specular)) * radiance;
				}

				if (indirect > 0.0)
					Lo -= albedo * ApplyBiasedDynamicLight(lightcolor.rgb * indirect) * (1.0 / PI);
			}
		}
	}

	// Pretend we sampled the sector light level from an irradiance map

	vec3 F = fresnelSchlickRoughness(clamp(dot(N, V), 0.0, 1.0), F0, roughness);

	vec3 kS = F;
	vec3 kD = 1.0 - kS;

	vec3 irradiance = ambientLight; // texture(irradianceMap, N).rgb
	vec3 diffuse = irradiance * albedo;

	//kD *= 1.0 - metallic;
	//const float MAX_REFLECTION_LOD = 4.0;
	//vec3 prefilteredColor = textureLod(prefilterMap, R,  roughness * MAX_REFLECTION_LOD).rgb;
	//vec2 envBRDF = texture(brdfLUT, vec2(clamp(dot(N, V), 0.0, 1.0), roughness)).rg;
	//vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);

	//vec3 ambient = (kD * diffuse + specular) * ao;
	vec3 ambient = (kD * diffuse) * ao;

	vec3 color = max(ambient + Lo, vec3(0.0));

	// Tonemap (reinhard) and apply sRGB gamma
	//color = color / (color + vec3(1.0));
	return pow(color, vec3(1.0 / 2.2));
}
