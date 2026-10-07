#pragma once

#include "hwrenderer/data/shaderuniforms.h"
#include "intrect.h"
#include <map>
#include <memory>

struct PostProcessShader;

typedef FRenderStyle PPBlendMode;
typedef IntRect PPViewport;

class PPTexture;
class PPShader;

enum class ETonemapMode : uint8_t {
  None = 0,
  Uncharted2 = 1,
  HejlDawson = 2,
  Reinhard = 3,
  Linear = 4,
  Palette = 5,
  Gothic = 6,
  GothicNoir = 7,
  Moonlit = 8,
  Candlelit = 9,
  Graveyard = 10,
  SilentHill = 11,
  BleachBypass = 12,
  Lottes = 13,
  ACES = 14,
  NumTonemapModes
};

enum class PPFilterMode { Nearest, Linear };
enum class PPWrapMode { Clamp, Repeat, Mirror };
enum class PPTextureType {
  CurrentPipelineTexture,
  NextPipelineTexture,
  PPTexture,
  SceneColor,
  SceneFog,
  SceneNormal,
  SceneDepth,
  SwapChain,
  ShadowMap
};

class PPTextureInput {
public:
  PPFilterMode Filter = PPFilterMode::Nearest;
  PPWrapMode Wrap = PPWrapMode::Clamp;
  PPTextureType Type = PPTextureType::CurrentPipelineTexture;
  PPTexture *Texture = nullptr;
};

class PPOutput {
public:
  PPTextureType Type = PPTextureType::NextPipelineTexture;
  PPTexture *Texture = nullptr;
};

class PPUniforms {
public:
  PPUniforms() {}

  PPUniforms(const PPUniforms &src) { Data = src.Data; }

  ~PPUniforms() { Clear(); }

  PPUniforms &operator=(const PPUniforms &src) {
    Data = src.Data;
    return *this;
  }

  void Clear() { Data.Clear(); }

  template <typename T> void Set(const T &v) {
    if (Data.Size() != (int)sizeof(T)) {
      Data.Resize(sizeof(T));
      memcpy(Data.Data(), &v, Data.Size());
    }
  }

  TArray<uint8_t> Data;
};

class PPRenderState {
public:
  virtual ~PPRenderState() = default;

  virtual void PushGroup(const FString &name) = 0;
  virtual void PopGroup() = 0;

  virtual void Draw() = 0;

  void Clear() {
    Shader = nullptr;
    Textures = TArray<PPTextureInput>();
    Uniforms = PPUniforms();
    Viewport = PPViewport();
    BlendMode = PPBlendMode();
    Output = PPOutput();
    ShadowMapBuffers = false;
  }

  void SetInputTexture(int index, PPTexture *texture,
                       PPFilterMode filter = PPFilterMode::Nearest,
                       PPWrapMode wrap = PPWrapMode::Clamp) {
    if ((int)Textures.Size() < index + 1)
      Textures.Resize(index + 1);
    auto &tex = Textures[index];
    tex.Filter = filter;
    tex.Wrap = wrap;
    tex.Type = PPTextureType::PPTexture;
    tex.Texture = texture;
  }

  void SetInputCurrent(int index, PPFilterMode filter = PPFilterMode::Nearest,
                       PPWrapMode wrap = PPWrapMode::Clamp) {
    SetInputSpecialType(index, PPTextureType::CurrentPipelineTexture, filter,
                        wrap);
  }

  void SetInputSceneColor(int index,
                          PPFilterMode filter = PPFilterMode::Nearest,
                          PPWrapMode wrap = PPWrapMode::Clamp) {
    SetInputSpecialType(index, PPTextureType::SceneColor, filter, wrap);
  }

  void SetInputSceneFog(int index, PPFilterMode filter = PPFilterMode::Nearest,
                        PPWrapMode wrap = PPWrapMode::Clamp) {
    SetInputSpecialType(index, PPTextureType::SceneFog, filter, wrap);
  }

  void SetInputSceneNormal(int index,
                           PPFilterMode filter = PPFilterMode::Nearest,
                           PPWrapMode wrap = PPWrapMode::Clamp) {
    SetInputSpecialType(index, PPTextureType::SceneNormal, filter, wrap);
  }

  void SetInputSceneDepth(int index,
                          PPFilterMode filter = PPFilterMode::Nearest,
                          PPWrapMode wrap = PPWrapMode::Clamp) {
    SetInputSpecialType(index, PPTextureType::SceneDepth, filter, wrap);
  }

  void SetInputSpecialType(int index, PPTextureType type,
                           PPFilterMode filter = PPFilterMode::Nearest,
                           PPWrapMode wrap = PPWrapMode::Clamp) {
    if ((int)Textures.Size() < index + 1)
      Textures.Resize(index + 1);
    auto &tex = Textures[index];
    tex.Filter = filter;
    tex.Wrap = wrap;
    tex.Type = type;
    tex.Texture = nullptr;
  }

  void SetShadowMapBuffers(bool enable) { ShadowMapBuffers = enable; }

  void SetOutputTexture(PPTexture *texture) {
    Output.Type = PPTextureType::PPTexture;
    Output.Texture = texture;
  }

  void SetOutputCurrent() {
    Output.Type = PPTextureType::CurrentPipelineTexture;
    Output.Texture = nullptr;
  }

  void SetOutputNext() {
    Output.Type = PPTextureType::NextPipelineTexture;
    Output.Texture = nullptr;
  }

  void SetOutputSceneColor() {
    Output.Type = PPTextureType::SceneColor;
    Output.Texture = nullptr;
  }

  void SetOutputSwapChain() {
    Output.Type = PPTextureType::SwapChain;
    Output.Texture = nullptr;
  }

  void SetOutputShadowMap() {
    Output.Type = PPTextureType::ShadowMap;
    Output.Texture = nullptr;
  }

  void SetNoBlend() {
    BlendMode.BlendOp = STYLEOP_Add;
    BlendMode.SrcAlpha = STYLEALPHA_One;
    BlendMode.DestAlpha = STYLEALPHA_Zero;
    BlendMode.Flags = 0;
  }

  void SetAdditiveBlend() {
    BlendMode.BlendOp = STYLEOP_Add;
    BlendMode.SrcAlpha = STYLEALPHA_One;
    BlendMode.DestAlpha = STYLEALPHA_One;
    BlendMode.Flags = 0;
  }

  void SetAlphaBlend() {
    BlendMode.BlendOp = STYLEOP_Add;
    BlendMode.SrcAlpha = STYLEALPHA_Src;
    BlendMode.DestAlpha = STYLEALPHA_InvSrc;
    BlendMode.Flags = 0;
  }

  PPShader *Shader;
  TArray<PPTextureInput> Textures;
  PPUniforms Uniforms;
  PPViewport Viewport;
  PPBlendMode BlendMode;
  PPOutput Output;
  bool ShadowMapBuffers = false;
};

enum class PixelFormat { Rgba8, Rgba16f, R32f, Rg16f, Rgba16_snorm };

class PPResource {
public:
  PPResource() { Link(); }

  PPResource(PPResource &&) noexcept { Link(); }

  PPResource(const PPResource &) = delete;

  PPResource &operator=(const PPResource &) = delete;

  PPResource &operator=(PPResource &&other) noexcept {
    if (this != &other) {
      Unlink();
      Link();
    }
    return *this;
  }

  virtual ~PPResource() { Unlink(); }

  static void ResetAll() {
    for (PPResource *cur = First; cur; cur = cur->Next)
      cur->ResetBackend();
  }

  virtual void ResetBackend() = 0;

 private:
  void Link() {
    Prev = nullptr;
    Next = First;
    First = this;
    if (Next)
      Next->Prev = this;
  }

  void Unlink() {
    if (Next)
      Next->Prev = Prev;
    if (Prev)
      Prev->Next = Next;
    else
      First = Next;

    Prev = nullptr;
    Next = nullptr;
  }

  static PPResource *First;
  PPResource *Prev = nullptr;
  PPResource *Next = nullptr;
};

class PPTextureBackend {
public:
  virtual ~PPTextureBackend() = default;
};

class PPTexture : public PPResource {
public:
  PPTexture() = default;
  PPTexture(int width, int height, PixelFormat format,
            std::shared_ptr<void> data = {})
      : Width(width), Height(height), Format(format), Data(data) {}

  void ResetBackend() override { Backend.reset(); }

  int Width;
  int Height;
  PixelFormat Format;
  // Most dynamic PP textures are only sampled and rendered to.  A small set
  // of backend-owned targets (continuous final-frame capture) also need to be
  // copied out by the GPU, so request that usage explicitly instead of making
  // every postprocess allocation depend on TRANSFER_SRC support.
  bool TransferSource = false;
  std::shared_ptr<void> Data;

  std::unique_ptr<PPTextureBackend> Backend;
};

class PPShaderBackend {
public:
  virtual ~PPShaderBackend() = default;
};

class PPShader : public PPResource {
public:
  PPShader() = default;
  PPShader(const FString &fragment, const FString &defines,
           const std::vector<UniformFieldDesc> &uniforms, int version = 330)
      : FragmentShader(fragment), Defines(defines), Uniforms(uniforms),
        Version(version) {}

  void ResetBackend() override { Backend.reset(); }

  FString VertexShader = "shaders/pp/screenquad.vp";
  FString FragmentShader;
  FString Defines;
  std::vector<UniformFieldDesc> Uniforms;
  int Version = 330;

  std::unique_ptr<PPShaderBackend> Backend;
};

/////////////////////////////////////////////////////////////////////////////

struct ExtractUniforms {
  FVector2 Scale;
  FVector2 Offset;
  float Threshold;
  float Knee;

  static std::vector<UniformFieldDesc> Desc() {
    return {{"Scale", UniformType::Vec2, offsetof(ExtractUniforms, Scale)},
            {"Offset", UniformType::Vec2, offsetof(ExtractUniforms, Offset)},
            {"Threshold", UniformType::Float, offsetof(ExtractUniforms, Threshold)},
            {"Knee", UniformType::Float, offsetof(ExtractUniforms, Knee)}};
  }
};

static_assert(sizeof(ExtractUniforms) <= 128,
              "Bloom extraction uniforms exceed Vulkan's guaranteed push constant size");

struct BloomCompositeUniforms {
  float Intensity;

  static std::vector<UniformFieldDesc> Desc() {
    return {{"Intensity", UniformType::Float, offsetof(BloomCompositeUniforms, Intensity)}};
  }
};

static_assert(sizeof(BloomCompositeUniforms) <= 128,
              "Bloom composite uniforms exceed Vulkan's guaranteed push constant size");

struct BlurUniforms {
  float SampleWeights[8];
  float RadiusScale;

  static std::vector<UniformFieldDesc> Desc() {
    return {
        {"SampleWeights0", UniformType::Float,
         offsetof(BlurUniforms, SampleWeights[0])},
        {"SampleWeights1", UniformType::Float,
         offsetof(BlurUniforms, SampleWeights[1])},
        {"SampleWeights2", UniformType::Float,
         offsetof(BlurUniforms, SampleWeights[2])},
        {"SampleWeights3", UniformType::Float,
         offsetof(BlurUniforms, SampleWeights[3])},
        {"SampleWeights4", UniformType::Float,
         offsetof(BlurUniforms, SampleWeights[4])},
        {"SampleWeights5", UniformType::Float,
         offsetof(BlurUniforms, SampleWeights[5])},
        {"SampleWeights6", UniformType::Float,
         offsetof(BlurUniforms, SampleWeights[6])},
        {"SampleWeights7", UniformType::Float,
         offsetof(BlurUniforms, SampleWeights[7])},
        {"RadiusScale", UniformType::Float,
         offsetof(BlurUniforms, RadiusScale)},
    };
  }
};

enum { NumBloomLevels = 4 };

class PPBlurLevel {
public:
  PPViewport Viewport;
  PPTexture VTexture;
  PPTexture HTexture;
};

class PPBloom {
public:
  void RenderBloom(PPRenderState *renderstate, int sceneWidth, int sceneHeight,
                   int fixedcm);
  void RenderBlur(PPRenderState *renderstate, int sceneWidth, int sceneHeight,
                  float gameinfobluramount);

private:
  void BlurStep(PPRenderState *renderstate, const BlurUniforms &blurUniforms,
                PPTexture &input, PPTexture &output, PPViewport viewport,
                bool vertical);
  void UpdateTextures(int width, int height);

  static float ComputeBlurGaussian(float n, float theta);
  static void ComputeBlurSamples(int sampleCount, float blurAmount,
                                 float *sampleWeights);

  PPBlurLevel levels[NumBloomLevels];
  int lastWidth = 0;
  int lastHeight = 0;

  PPShader BloomCombine = {"shaders/pp/bloomcombine.fp", "", {}};
  PPShader BloomFinal = {"shaders/pp/bloomcombine.fp", "#define BLOOM_FINAL\n",
                         BloomCompositeUniforms::Desc()};
  PPShader BloomExtract = {"shaders/pp/bloomextract.fp", "",
                           ExtractUniforms::Desc()};
  PPShader BlurVertical = {"shaders/pp/blur.fp", "#define BLUR_VERTICAL\n",
                           BlurUniforms::Desc()};
  PPShader BlurHorizontal = {"shaders/pp/blur.fp", "#define BLUR_HORIZONTAL\n",
                             BlurUniforms::Desc()};
};

/////////////////////////////////////////////////////////////////////////////

struct LensUniforms {
  float AspectRatio;
  float Scale;
  float Padding0, Padding1;
  FVector4 LensDistortionCoefficient;
  FVector4 CubicDistortionValue;

  static std::vector<UniformFieldDesc> Desc() {
    return {{"Aspect", UniformType::Float, offsetof(LensUniforms, AspectRatio)},
            {"Scale", UniformType::Float, offsetof(LensUniforms, Scale)},
            {"Padding0", UniformType::Float, offsetof(LensUniforms, Padding0)},
            {"Padding1", UniformType::Float, offsetof(LensUniforms, Padding1)},
            {"k", UniformType::Vec4,
             offsetof(LensUniforms, LensDistortionCoefficient)},
            {"kcube", UniformType::Vec4,
             offsetof(LensUniforms, CubicDistortionValue)}};
  }
};

class PPLensDistort {
public:
  void Render(PPRenderState *renderstate);

private:
  PPShader Lens = {"shaders/pp/lensdistortion.fp", "", LensUniforms::Desc()};
};

/////////////////////////////////////////////////////////////////////////////

struct FXAAUniforms {
  FVector2 ReciprocalResolution;
  float Padding0, Padding1;

  static std::vector<UniformFieldDesc> Desc() {
    return {{"ReciprocalResolution", UniformType::Vec2,
             offsetof(FXAAUniforms, ReciprocalResolution)},
            {"Padding0", UniformType::Float, offsetof(FXAAUniforms, Padding0)},
            {"Padding1", UniformType::Float, offsetof(FXAAUniforms, Padding1)}};
  }
};

class PPFXAA {
public:
  void Render(PPRenderState *renderstate);

private:
  void CreateShaders();
  int GetMaxVersion();
  FString GetDefines();

  PPShader FXAALuma;
  PPShader FXAA;
  int LastQuality = -1;
};

/////////////////////////////////////////////////////////////////////////////

struct ExposureExtractUniforms {
  FVector2 Scale;
  FVector2 Offset;

  static std::vector<UniformFieldDesc> Desc() {
    return {
        {"Scale", UniformType::Vec2, offsetof(ExposureExtractUniforms, Scale)},
        {"Offset", UniformType::Vec2,
         offsetof(ExposureExtractUniforms, Offset)}};
  }
};

struct ExposureCombineUniforms {
  float ExposureBase;
  float ExposureMin;
  float ExposureScale;
  float ExposureSpeed;

  static std::vector<UniformFieldDesc> Desc() {
    return {{"ExposureBase", UniformType::Float,
             offsetof(ExposureCombineUniforms, ExposureBase)},
            {"ExposureMin", UniformType::Float,
             offsetof(ExposureCombineUniforms, ExposureMin)},
            {"ExposureScale", UniformType::Float,
             offsetof(ExposureCombineUniforms, ExposureScale)},
            {"ExposureSpeed", UniformType::Float,
             offsetof(ExposureCombineUniforms, ExposureSpeed)}};
  }
};

class PPExposureLevel {
public:
  PPViewport Viewport;
  PPTexture Texture;
};

class PPCameraExposure {
public:
  void Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight);

  PPTexture CameraTexture = {1, 1, PixelFormat::R32f};

private:
  void UpdateTextures(int width, int height);

  std::vector<PPExposureLevel> ExposureLevels;
  bool FirstExposureFrame = true;

  PPShader ExposureExtract = {"shaders/pp/exposureextract.fp", "",
                              ExposureExtractUniforms::Desc()};
  PPShader ExposureAverage = {"shaders/pp/exposureaverage.fp", "", {}, 400};
  PPShader ExposureCombine = {"shaders/pp/exposurecombine.fp", "",
                              ExposureCombineUniforms::Desc()};
};

/////////////////////////////////////////////////////////////////////////////

struct ColormapUniforms {
  FVector4 MapStart;
  FVector4 MapRange;

  static std::vector<UniformFieldDesc> Desc() {
    return {
        {"uFixedColormapStart", UniformType::Vec4,
         offsetof(ColormapUniforms, MapStart)},
        {"uFixedColormapRange", UniformType::Vec4,
         offsetof(ColormapUniforms, MapRange)},
    };
  }
};

class PPColormap {
public:
  void Render(PPRenderState *renderstate, int fixedcm, float flash);

private:
  PPShader Colormap = {"shaders/pp/colormap.fp", "", ColormapUniforms::Desc()};
};

/////////////////////////////////////////////////////////////////////////////

class PPTonemap {
public:
  void SetTonemapMode(ETonemapMode tm) { level_tonemap = tm; }
  void Render(PPRenderState *renderstate);
  void ClearTonemapPalette() { PaletteTexture = {}; }

private:
  void UpdateTextures();

  PPTexture PaletteTexture;

  PPShader LinearShader = {"shaders/pp/tonemap.fp", "#define LINEAR\n", {}};
  PPShader ReinhardShader = {"shaders/pp/tonemap.fp", "#define REINHARD\n", {}};
  PPShader HejlDawsonShader = {
      "shaders/pp/tonemap.fp", "#define HEJLDAWSON\n", {}};
  PPShader Uncharted2Shader = {
      "shaders/pp/tonemap.fp", "#define UNCHARTED2\n", {}};
  PPShader PaletteShader = {"shaders/pp/tonemap.fp", "#define PALETTE\n", {}};
  PPShader GothicShader = {"shaders/pp/tonemap.fp", "#define GOTHIC\n", {}};
  PPShader GothicNoirShader = {
      "shaders/pp/tonemap.fp", "#define GOTHIC_NOIR\n", {}};
  PPShader MoonlitShader = {"shaders/pp/tonemap.fp", "#define MOONLIT\n", {}};
  PPShader CandlelitShader = {
      "shaders/pp/tonemap.fp", "#define CANDLELIT\n", {}};
  PPShader GraveyardShader = {
      "shaders/pp/tonemap.fp", "#define GRAVEYARD\n", {}};
  PPShader SilentHillShader = {
      "shaders/pp/tonemap.fp", "#define SILENT_HILL\n", {}};
  PPShader BleachBypassShader = {
      "shaders/pp/tonemap.fp", "#define BLEACH_BYPASS\n", {}};
  PPShader LottesShader = {"shaders/pp/tonemap.fp", "#define LOTTES\n", {}};
  PPShader ACESShader = {"shaders/pp/tonemap.fp", "#define ACES\n", {}};
  ETonemapMode level_tonemap = ETonemapMode::None;
};

/////////////////////////////////////////////////////////////////////////////

struct LinearDepthUniforms {
  int SampleIndex;
  float LinearizeDepthA;
  float LinearizeDepthB;
  float InverseDepthRangeA;
  float InverseDepthRangeB;
  float Padding0, Padding1, Padding2;
  FVector2 Scale;
  FVector2 Offset;

  static std::vector<UniformFieldDesc> Desc() {
    return {
        {"SampleIndex", UniformType::Int,
         offsetof(LinearDepthUniforms, SampleIndex)},
        {"LinearizeDepthA", UniformType::Float,
         offsetof(LinearDepthUniforms, LinearizeDepthA)},
        {"LinearizeDepthB", UniformType::Float,
         offsetof(LinearDepthUniforms, LinearizeDepthB)},
        {"InverseDepthRangeA", UniformType::Float,
         offsetof(LinearDepthUniforms, InverseDepthRangeA)},
        {"InverseDepthRangeB", UniformType::Float,
         offsetof(LinearDepthUniforms, InverseDepthRangeB)},
        {"Padding0", UniformType::Float,
         offsetof(LinearDepthUniforms, Padding0)},
        {"Padding1", UniformType::Float,
         offsetof(LinearDepthUniforms, Padding1)},
        {"Padding2", UniformType::Float,
         offsetof(LinearDepthUniforms, Padding2)},
        {"Scale", UniformType::Vec2, offsetof(LinearDepthUniforms, Scale)},
        {"Offset", UniformType::Vec2, offsetof(LinearDepthUniforms, Offset)}};
  }
};

struct SSAOUniforms {
  FVector2 UVToViewA;
  FVector2 UVToViewB;
  FVector2 InvFullResolution;
  float NDotVBias;
  float NegInvR2;
  float RadiusToScreen;
  float AOMultiplier;
  float AOStrength;
  int SampleIndex;
  float Padding0, Padding1;
  FVector2 Scale;
  FVector2 Offset;

  static std::vector<UniformFieldDesc> Desc() {
    return {
        {"UVToViewA", UniformType::Vec2, offsetof(SSAOUniforms, UVToViewA)},
        {"UVToViewB", UniformType::Vec2, offsetof(SSAOUniforms, UVToViewB)},
        {"InvFullResolution", UniformType::Vec2,
         offsetof(SSAOUniforms, InvFullResolution)},
        {"NDotVBias", UniformType::Float, offsetof(SSAOUniforms, NDotVBias)},
        {"NegInvR2", UniformType::Float, offsetof(SSAOUniforms, NegInvR2)},
        {"RadiusToScreen", UniformType::Float,
         offsetof(SSAOUniforms, RadiusToScreen)},
        {"AOMultiplier", UniformType::Float,
         offsetof(SSAOUniforms, AOMultiplier)},
        {"AOStrength", UniformType::Float, offsetof(SSAOUniforms, AOStrength)},
        {"SampleIndex", UniformType::Int, offsetof(SSAOUniforms, SampleIndex)},
        {"Padding0", UniformType::Float, offsetof(SSAOUniforms, Padding0)},
        {"Padding1", UniformType::Float, offsetof(SSAOUniforms, Padding1)},
        {"Scale", UniformType::Vec2, offsetof(SSAOUniforms, Scale)},
        {"Offset", UniformType::Vec2, offsetof(SSAOUniforms, Offset)},
    };
  }
};

struct DepthBlurUniforms {
  float BlurSharpness;
  float PowExponent;
  float Padding0, Padding1;

  static std::vector<UniformFieldDesc> Desc() {
    return {
        {"BlurSharpness", UniformType::Float,
         offsetof(DepthBlurUniforms, BlurSharpness)},
        {"PowExponent", UniformType::Float,
         offsetof(DepthBlurUniforms, PowExponent)},
        {"Padding0", UniformType::Float, offsetof(DepthBlurUniforms, Padding0)},
        {"Padding1", UniformType::Float,
         offsetof(DepthBlurUniforms, Padding1)}};
  }
};

struct AmbientCombineUniforms {
  int SampleCount;
  int DebugMode, Padding1, Padding2;
  FVector2 Scale;
  FVector2 Offset;

  static std::vector<UniformFieldDesc> Desc() {
    return {
        {"SampleCount", UniformType::Int,
         offsetof(AmbientCombineUniforms, SampleCount)},
        {"DebugMode", UniformType::Int,
         offsetof(AmbientCombineUniforms, DebugMode)},
        {"Padding1", UniformType::Int,
         offsetof(AmbientCombineUniforms, Padding1)},
        {"Padding2", UniformType::Int,
         offsetof(AmbientCombineUniforms, Padding2)},
        {"Scale", UniformType::Vec2, offsetof(AmbientCombineUniforms, Scale)},
        {"Offset", UniformType::Vec2,
         offsetof(AmbientCombineUniforms, Offset)}};
  }
};

class PPAmbientOcclusion {
public:
  PPAmbientOcclusion();
  void Render(PPRenderState *renderstate, float m5, int sceneWidth,
              int sceneHeight);

private:
  void CreateShaders();
  void UpdateTextures(int width, int height);

  enum Quality { Off, LowQuality, MediumQuality, HighQuality, NumQualityModes };

  int AmbientWidth = 0;
  int AmbientHeight = 0;

  int LastQuality = -1;
  int LastWidth = 0;
  int LastHeight = 0;

  PPShader LinearDepth;
  PPShader LinearDepthMS;
  PPShader AmbientOcclude;
  PPShader AmbientOccludeMS;
  PPShader BlurVertical;
  PPShader BlurHorizontal;
  PPShader Combine;
  PPShader CombineMS;

  PPTexture LinearDepthTexture;
  PPTexture Ambient0;
  PPTexture Ambient1;

  enum { NumAmbientRandomTextures = 3 };
  PPTexture AmbientRandomTexture[NumAmbientRandomTextures];
};

struct PresentUniforms {
  float InvGamma;
  float Contrast;
  float Brightness;
  float Saturation;
  int GrayFormula;
  int WindowPositionParity; // top-of-window might not be top-of-screen
  FVector2 Scale;
  FVector2 Offset;
  float ColorScale;
  int HdrMode;
  int AtmosphereMode;
  float AtmosphereIntensity;
  float AtmosphereContrast;
  // Vulkan feeds this struct through push constants. The Vulkan spec only
  // guarantees a 128 byte push constant range, but desktop GPUs commonly
  // expose 256; vk_renderdevice rejects devices below sizeof(PresentUniforms)
  // at init. Most effects gate on
  // a neutral parameter value instead of a dedicated enable int (strength 0,
  // scale 1.0, levels 0, degrees 0, neutral white balance); only effects with
  // no neutral-capable parameter keep an explicit enable.
  float VignetteStrength;
  float ChromaticStrength;
  float FilmgrainStrength;
  float FilmgrainScale;
  float SharpenStrength;
  float RetroPixelScale;
  int ColorgradeMode;
  float ColorgradeStrength;
  int ColorgradeLut;
  float VhsStrength;
  float VhsScanline;
  float VhsJitter;
  float VhsTime;
  float VhsTracking;
  float VhsGhosting;
  float VhsNoise;
  float VhsEvil;
  int VhsPanicEnable;
  int CrtMode;
  float CrtDistortion;
  float CrtZoom;
  float CrtScanline;
  float CrtScanlineDensity;
  float CrtScanlineSharpness;
  float CrtMaskIntensity;
  int NtscMode;
  int GradeEnable;
  float VibranceStrength;
  float WhiteBalanceTemperature;
  float WhiteBalanceTint;
  float HueShiftDegrees;
  float PosterizeLevels;
  float EdgeGlowStrength;
  float EdgeGlowThreshold;
  float GodRaysStrength;
  float GodRaysLength;
  float GodRaysThreshold;
  float LensFlareStrength;
  float ClarityStrength;
  float GradeLiftR;
  float GradeLiftG;
  float GradeLiftB;
  float GradeGammaR;
  float GradeGammaG;
  float GradeGammaB;
  float GradeGainR;
  float GradeGainG;
  float GradeGainB;

  static std::vector<UniformFieldDesc> Desc() {
    return {
        {"InvGamma", UniformType::Float, offsetof(PresentUniforms, InvGamma)},
        {"Contrast", UniformType::Float, offsetof(PresentUniforms, Contrast)},
        {"Brightness", UniformType::Float,
         offsetof(PresentUniforms, Brightness)},
        {"Saturation", UniformType::Float,
         offsetof(PresentUniforms, Saturation)},
        {"GrayFormula", UniformType::Int,
         offsetof(PresentUniforms, GrayFormula)},
        {"WindowPositionParity", UniformType::Int,
         offsetof(PresentUniforms, WindowPositionParity)},
        {"UVScale", UniformType::Vec2, offsetof(PresentUniforms, Scale)},
        {"UVOffset", UniformType::Vec2, offsetof(PresentUniforms, Offset)},
        {"ColorScale", UniformType::Float,
         offsetof(PresentUniforms, ColorScale)},
        {"HdrMode", UniformType::Int, offsetof(PresentUniforms, HdrMode)},
        {"AtmosphereMode", UniformType::Int,
         offsetof(PresentUniforms, AtmosphereMode)},
        {"AtmosphereIntensity", UniformType::Float,
         offsetof(PresentUniforms, AtmosphereIntensity)},
        {"AtmosphereContrast", UniformType::Float,
         offsetof(PresentUniforms, AtmosphereContrast)},
        {"VignetteStrength", UniformType::Float,
         offsetof(PresentUniforms, VignetteStrength)},
        {"ChromaticStrength", UniformType::Float,
         offsetof(PresentUniforms, ChromaticStrength)},
        {"FilmgrainStrength", UniformType::Float,
         offsetof(PresentUniforms, FilmgrainStrength)},
        {"FilmgrainScale", UniformType::Float,
         offsetof(PresentUniforms, FilmgrainScale)},
        {"SharpenStrength", UniformType::Float,
         offsetof(PresentUniforms, SharpenStrength)},
        {"RetroPixelScale", UniformType::Float,
         offsetof(PresentUniforms, RetroPixelScale)},
        {"ColorgradeMode", UniformType::Int,
         offsetof(PresentUniforms, ColorgradeMode)},
        {"ColorgradeStrength", UniformType::Float,
         offsetof(PresentUniforms, ColorgradeStrength)},
        {"ColorgradeLut", UniformType::Int,
         offsetof(PresentUniforms, ColorgradeLut)},
        {"VhsStrength", UniformType::Float,
         offsetof(PresentUniforms, VhsStrength)},
        {"VhsScanline", UniformType::Float,
         offsetof(PresentUniforms, VhsScanline)},
        {"VhsJitter", UniformType::Float, offsetof(PresentUniforms, VhsJitter)},
        {"VhsTime", UniformType::Float, offsetof(PresentUniforms, VhsTime)},
        {"VhsTracking", UniformType::Float,
         offsetof(PresentUniforms, VhsTracking)},
        {"VhsGhosting", UniformType::Float,
         offsetof(PresentUniforms, VhsGhosting)},
        {"VhsNoise", UniformType::Float, offsetof(PresentUniforms, VhsNoise)},
        {"VhsEvil", UniformType::Float, offsetof(PresentUniforms, VhsEvil)},
        {"VhsPanicEnable", UniformType::Int,
         offsetof(PresentUniforms, VhsPanicEnable)},
        {"CrtMode", UniformType::Int, offsetof(PresentUniforms, CrtMode)},
        {"CrtDistortion", UniformType::Float,
         offsetof(PresentUniforms, CrtDistortion)},
        {"CrtZoom", UniformType::Float, offsetof(PresentUniforms, CrtZoom)},
        {"CrtScanline", UniformType::Float,
         offsetof(PresentUniforms, CrtScanline)},
        {"CrtScanlineDensity", UniformType::Float,
         offsetof(PresentUniforms, CrtScanlineDensity)},
        {"CrtScanlineSharpness", UniformType::Float,
         offsetof(PresentUniforms, CrtScanlineSharpness)},
        {"CrtMaskIntensity", UniformType::Float,
         offsetof(PresentUniforms, CrtMaskIntensity)},
        {"NtscMode", UniformType::Int, offsetof(PresentUniforms, NtscMode)},
        {"GradeEnable", UniformType::Int,
         offsetof(PresentUniforms, GradeEnable)},
        {"VibranceStrength", UniformType::Float,
         offsetof(PresentUniforms, VibranceStrength)},
        {"WhiteBalanceTemperature", UniformType::Float,
         offsetof(PresentUniforms, WhiteBalanceTemperature)},
        {"WhiteBalanceTint", UniformType::Float,
         offsetof(PresentUniforms, WhiteBalanceTint)},
        {"HueShiftDegrees", UniformType::Float,
         offsetof(PresentUniforms, HueShiftDegrees)},
        {"PosterizeLevels", UniformType::Float,
         offsetof(PresentUniforms, PosterizeLevels)},
        {"EdgeGlowStrength", UniformType::Float,
         offsetof(PresentUniforms, EdgeGlowStrength)},
        {"EdgeGlowThreshold", UniformType::Float,
         offsetof(PresentUniforms, EdgeGlowThreshold)},
        {"GodRaysStrength", UniformType::Float,
         offsetof(PresentUniforms, GodRaysStrength)},
        {"GodRaysLength", UniformType::Float,
         offsetof(PresentUniforms, GodRaysLength)},
        {"GodRaysThreshold", UniformType::Float,
         offsetof(PresentUniforms, GodRaysThreshold)},
        {"LensFlareStrength", UniformType::Float,
         offsetof(PresentUniforms, LensFlareStrength)},
        {"ClarityStrength", UniformType::Float,
         offsetof(PresentUniforms, ClarityStrength)},
        {"GradeLiftR", UniformType::Float,
         offsetof(PresentUniforms, GradeLiftR)},
        {"GradeLiftG", UniformType::Float,
         offsetof(PresentUniforms, GradeLiftG)},
        {"GradeLiftB", UniformType::Float,
         offsetof(PresentUniforms, GradeLiftB)},
        {"GradeGammaR", UniformType::Float,
         offsetof(PresentUniforms, GradeGammaR)},
        {"GradeGammaG", UniformType::Float,
         offsetof(PresentUniforms, GradeGammaG)},
        {"GradeGammaB", UniformType::Float,
         offsetof(PresentUniforms, GradeGammaB)},
        {"GradeGainR", UniformType::Float,
         offsetof(PresentUniforms, GradeGainR)},
        {"GradeGainG", UniformType::Float,
         offsetof(PresentUniforms, GradeGainG)},
        {"GradeGainB", UniformType::Float,
         offsetof(PresentUniforms, GradeGainB)}};
  }
};

// The Vulkan backend uploads PresentUniforms as fragment stage push constants
// (vk_renderpass.cpp AddPushConstantRange). Desktop GPUs commonly expose 256
// bytes, but the Vulkan spec only guarantees 128 — devices below
// sizeof(PresentUniforms) are rejected at VulkanRenderDevice::InitializeState
// before any postprocess pipeline is created. This assert bounds the struct to
// what the common desktop 256 byte range can hold.
static_assert(sizeof(PresentUniforms) <= 256,
              "PresentUniforms must fit within the 256 byte push constant range common on desktop GPUs");

class PPPresent {
 public:
  PPPresent();

  PPTexture Dither;

  PPShader Present = {"shaders/pp/present.fp", "", PresentUniforms::Desc()};
  PPShader Checker3D = {"shaders/pp/present_checker3d.fp", "",
                        PresentUniforms::Desc()};
  PPShader Column3D = {"shaders/pp/present_column3d.fp", "",
                       PresentUniforms::Desc()};
  PPShader Row3D = {"shaders/pp/present_row3d.fp", "", PresentUniforms::Desc()};
};

struct ShadowMapUniforms {
  float ShadowmapQuality;
  int NodesCount;
  float Padding0, Padding1;

  static std::vector<UniformFieldDesc> Desc() {
    return {
        {"ShadowmapQuality", UniformType::Float,
         offsetof(ShadowMapUniforms, ShadowmapQuality)},
        {"NodesCount", UniformType::Int,
         offsetof(ShadowMapUniforms, NodesCount)},
        {"Padding0", UniformType::Float, offsetof(ShadowMapUniforms, Padding0)},
        {"Padding1", UniformType::Float, offsetof(ShadowMapUniforms, Padding1)},
    };
  }
};

class PPCustomShaderInstance {
public:
  PPCustomShaderInstance(PostProcessShader *desc);

  void Run(PPRenderState *renderstate);

  PostProcessShader *Desc = nullptr;

private:
  void AddUniformField(size_t &offset, const FString &name, UniformType type,
                       size_t fieldsize, size_t alignment = 0);
  void SetTextures(PPRenderState *renderstate);
  void SetUniforms(PPRenderState *renderstate);

  PPShader Shader;
  int UniformStructSize = 0;
  std::vector<UniformFieldDesc> Fields;
  std::vector<std::unique_ptr<FString>> FieldNames;
  std::map<FTexture *, std::unique_ptr<PPTexture>> Textures;
  std::map<FString, size_t> FieldOffset;
};

class PPCustomShaders {
public:
  void Run(PPRenderState *renderstate, FString target);

private:
  void CreateShaders();

  std::vector<std::unique_ptr<PPCustomShaderInstance>> mShaders;
};

class PPShadowMap {
public:
  void Update(PPRenderState *renderstate);

private:
  PPShader ShadowMap = {"shaders/pp/shadowmap.fp", "",
                        ShadowMapUniforms::Desc()};
};

/////////////////////////////////////////////////////////////////////////////

class Postprocess {
public:
  PPBloom bloom;
  PPLensDistort lens;
  PPFXAA fxaa;
  PPCameraExposure exposure;
  PPColormap colormap;
  PPTonemap tonemap;
  PPAmbientOcclusion ssao;
  PPPresent present;
  PPShadowMap shadowmap;
  PPCustomShaders customShaders;

  void SetTonemapMode(ETonemapMode tm) { tonemap.SetTonemapMode(tm); }
  void Pass1(PPRenderState *state, int fixedcm, int sceneWidth,
             int sceneHeight);
  void Pass2(PPRenderState *state, int fixedcm, float flash, int sceneWidth,
             int sceneHeight);
};

extern Postprocess hw_postprocess;
