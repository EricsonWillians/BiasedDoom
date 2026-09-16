
vec3 ProcessMaterialLight(Material material, vec3 color)
{
	return material.Base.rgb * clamp(ApplyBiasedAmbientFloor(ApplyBiasedAmbientGradient(color + desaturate(vec4(ApplyBiasedDynamicLight(uDynLightColor.rgb), uDynLightColor.a)).rgb + vec3(uGIAmbientStrength), material.Normal)), 0.0, 1.4);
}
