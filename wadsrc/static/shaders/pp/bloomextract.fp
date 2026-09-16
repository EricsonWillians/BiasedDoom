
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;
layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D ExposureTexture;

void main()
{
	float exposureAdjustment = texture(ExposureTexture, vec2(0.5)).x;
	vec4 color = texture(SceneTexture, Offset + TexCoord * Scale);
	vec3 exposed = max(color.rgb * exposureAdjustment, vec3(0.0));
	float luminance = dot(exposed, vec3(0.2126, 0.7152, 0.0722));

	// A configurable soft photographic knee preserves highlight energy without
	// blooming midtones.
	float threshold = max(Threshold, 0.0);
	float knee = max(Knee, 0.01);
	float shoulder = max(luminance - threshold + knee, 0.0);
	float softKnee = shoulder * shoulder / max(4.0 * knee, 0.0001);
	float contribution = max(luminance - threshold, softKnee);
	float bloomScale = contribution / max(luminance, 0.0001);

	vec3 spectralScatter = mix(vec3(1.03, 0.99, 0.94), vec3(0.92, 0.97, 1.08), clamp(luminance - threshold, 0.0, 1.0));
	FragColor = vec4(exposed * bloomScale * spectralScatter, 1.0);
}
