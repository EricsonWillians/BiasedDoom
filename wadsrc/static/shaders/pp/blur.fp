
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SourceTexture;

void main()
{
	vec2 texel = 1.0 / vec2(textureSize(SourceTexture, 0));
#if defined(BLUR_HORIZONTAL)
	vec2 stepDir = vec2(1.0, 0.0) * texel * RadiusScale;
#else
	vec2 stepDir = vec2(0.0, 1.0) * texel * RadiusScale;
#endif
	FragColor =
		texture(SourceTexture, TexCoord) * SampleWeights0 +
		texture(SourceTexture, TexCoord + stepDir * 1.0) * SampleWeights1 +
		texture(SourceTexture, TexCoord - stepDir * 1.0) * SampleWeights2 +
		texture(SourceTexture, TexCoord + stepDir * 2.0) * SampleWeights3 +
		texture(SourceTexture, TexCoord - stepDir * 2.0) * SampleWeights4 +
		texture(SourceTexture, TexCoord + stepDir * 3.0) * SampleWeights5 +
		texture(SourceTexture, TexCoord - stepDir * 3.0) * SampleWeights6;
}
