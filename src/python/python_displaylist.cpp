//---------------------------------------------------------------------------
//
// BiasedDoom persistent display list
//
// Python scripts register drawing primitives once and the engine re-renders
// them every HUD frame; scripts update an entry by re-calling a draw
// function with the same id. Screen-space items use normalized (0..1)
// coordinates. World-anchored items hold GC-safe actor references and are
// projected to screen space at render time.
//
//---------------------------------------------------------------------------

#include "python_displaylist.h"
#include "python_runtime.h"
#include "python_game_api.h"

#ifdef BIASEDDOOM_PYTHON

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#ifdef _PyCFunction_CAST
#define BD_DISPLAYLIST_KEYWORD_FUNCTION(function) _PyCFunction_CAST(function)
#else
#define BD_DISPLAYLIST_KEYWORD_FUNCTION(function) \
	reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(function))
#endif

#endif

#include "actor.h"
#include "d_main.h"
#include "d_player.h"
#include "dobjgc.h"
#include "doomstat.h"
#include "g_levellocals.h"
#include "i_net.h"
#include "p_local.h"
#include "r_translate.h"
#include "r_utility.h"
#include "texturemanager.h"
#include "v_2ddrawer.h"
#include "v_draw.h"
#include "v_font.h"
#include "v_video.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
enum class DrawKind : uint8_t
{
	Text,
	Rect,
	Line,
	Texture,
	WorldBar,
	WorldText,
	Circle,
	Frame,
	WorldTexture,
	WorldLine,
	WorldRing,
};

struct DrawItem
{
	DrawKind Kind = DrawKind::Text;
	FString Text;
	FFont* Font = nullptr;
	FGameTexture* Texture = nullptr;
	TObjPtr<AActor*> Actor = MakeObjPtr<AActor*>(nullptr);
	TObjPtr<AActor*> ActorB = MakeObjPtr<AActor*>(nullptr);
	FFont* LabelFont = nullptr;
	PalEntry Color = PalEntry(255, 255, 255, 255);
	PalEntry SecondaryColor = PalEntry(255, 20, 20, 20);
	PalEntry LabelColor = PalEntry(255, 255, 255, 255);
	EColorRange FontColor = CR_GOLD;
	EColorRange LabelFontColor = CR_GOLD;
	bool UseFontColor = false;
	bool LabelUseFontColor = false;
	bool Shadow = false;
	bool Outline = false;
	bool TrackHealth = false;
	bool FgExplicit = false;
	bool HasTint = false;
	bool Occlude = true;
	bool Label = false;
	bool Fill = false;
	bool HasColor2 = false;
	bool LineHasActorA = false;
	bool LineHasActorB = false;
	// ExpireTic: -1 = persistent, -2 = duration registered before any level
	// was loaded (resolve lazily at first Render), >= 0 = erase once
	// level.maptime reaches it.
	int64_t ExpireTic = -1;
	int64_t DurationTics = 0;
	uint32_t Seq = 0;
	int Layer = 0;
	int Align = 0; // 0 = left, 1 = center, 2 = right (screen-space text only)
	int Thickness = 0; // pixel border thickness (screen-space frame only)
	int Segments = 28; // polyline segment count (WorldRing only)
	double X = 0.0;
	double Y = 0.0;
	double X2 = 0.0;
	double Y2 = 0.0;
	double ScaleX = 1.0;
	double ScaleY = 1.0;
	// NormHeight: when > 0, text ignores ScaleX/ScaleY and is sized so one
	// text line occupies this normalized fraction of the screen height,
	// recomputed against the live drawer size every frame (resolution-
	// independent text; scale= stays raw pixels for compatibility).
	double NormHeight = 0.0;
	double Alpha = 1.0;
	double OffsetX = 0.0; // lateral world-unit offsets (WorldText only)
	double OffsetY = 0.0;
	double OffsetZ = 0.0;
	double BarWidth = 0.0;
	double BarHeight = 0.0;
	double MaxDistance = 2048.0;
	double Frac = 1.0;
	double LabelScale = 0.75;
	double Rotate = 0.0;
	double Size = 24.0;
	double Radius = 20.0; // world-unit ring radius (WorldRing only)
	DVector3 PointA = DVector3(0.0, 0.0, 0.0);
	DVector3 PointB = DVector3(0.0, 0.0, 0.0);
};

std::unordered_map<uint32_t, DrawItem> Items;
uint32_t NextSeq = 1; // monotonic insertion counter; tie-breaks the layer sort

bool IsWorldKind(DrawKind kind)
{
	return kind == DrawKind::WorldBar || kind == DrawKind::WorldText ||
		kind == DrawKind::WorldTexture || kind == DrawKind::WorldLine ||
		kind == DrawKind::WorldRing;
}

//---------------------------------------------------------------------------
// World-to-screen projection. Mirrors the software renderer's
// PointWorldToView/PointViewToScreen plus pitch, using the same view globals
// as the status bar's third-person crosshair projection.
//---------------------------------------------------------------------------

double ProjectionYAspectMul()
{
	double virtwidth = screen != nullptr ? screen->GetWidth() : viewwidth;
	double virtheight = screen != nullptr ? screen->GetHeight() : viewheight;

	if (virtwidth <= 0.0 || virtheight <= 0.0)
	{
		return 1.0;
	}

	if (AspectTallerThanWide(r_viewwindow.WidescreenRatio))
	{
		virtheight = virtheight * AspectMultiplier(r_viewwindow.WidescreenRatio) / 48.0;
	}
	else
	{
		virtwidth = virtwidth * AspectMultiplier(r_viewwindow.WidescreenRatio) / 48.0;
	}

	double pixelstretch = 1.2;
	if (r_viewpoint.ViewLevel != nullptr && r_viewpoint.ViewLevel->info != nullptr)
	{
		pixelstretch = r_viewpoint.ViewLevel->info->pixelstretch;
	}

	return 320.0 * virtheight / (r_Yaspect * virtwidth) * pixelstretch / 1.2;
}

bool ProjectWorldToScreen(const DVector3& worldPos, double& screenX, double& screenY, double& distance,
	double* outDepth = nullptr, double* outFocalX = nullptr, double* outFocalY = nullptr)
{
	if (r_viewwindow.FocalTangent <= 0.0 || r_viewwindow.centerx <= 0)
	{
		return false;
	}

	const DVector3 delta = worldPos - r_viewpoint.Pos;
	distance = delta.Length();
	const double side = delta.X * r_viewpoint.Sin - delta.Y * r_viewpoint.Cos;
	const double forward = delta.X * r_viewpoint.Cos + delta.Y * r_viewpoint.Sin;

	double up = delta.Z;
	double depth = forward;
	double centerY = r_viewwindow.centery;

	if (V_IsHardwareRenderer())
	{
		up = delta.Z * r_viewpoint.PitchCos + forward * r_viewpoint.PitchSin;
		depth = forward * r_viewpoint.PitchCos - delta.Z * r_viewpoint.PitchSin;
		centerY = viewheight * 0.5;
	}

	if (depth <= 1.0)
	{
		return false;
	}

	const double focalX = r_viewwindow.centerx / r_viewwindow.FocalTangent;
	const double focalY = focalX * ProjectionYAspectMul();

	screenX = viewwindowx + r_viewwindow.centerx + side / depth * focalX;
	screenY = viewwindowy + centerY - up / depth * focalY;
	if (outDepth != nullptr) *outDepth = depth;
	if (outFocalX != nullptr) *outFocalX = focalX;
	if (outFocalY != nullptr) *outFocalY = focalY;
	return true;
}

//---------------------------------------------------------------------------
// Per-kind rendering. All coordinates reaching this point are real drawer
// pixels.
//---------------------------------------------------------------------------

// Pixel width of the widest line of a (possibly multiline) string.
double MeasureTextWidth(FFont* font, const char* text)
{
	double widest = 0.0;
	const char* line = text;
	while (line != nullptr)
	{
		const char* newline = strchr(line, '\n');
		const FString part = newline != nullptr ? FString(line, static_cast<size_t>(newline - line)) : FString(line);
		widest = std::max(widest, static_cast<double>(font->StringWidth(part)));
		line = newline != nullptr ? newline + 1 : nullptr;
	}
	return widest;
}

int CountTextLines(const char* text)
{
	int lines = 1;
	for (const char* p = text; *p != '\0'; ++p)
	{
		if (*p == '\n') ++lines;
	}
	return lines;
}

// Renders text through a (lazily created, deduplicated per color)
// luminance-ramp palette translation: CR_NATIVEPAL + DTA_TranslationIndex
// REMAPS the font glyph's brightness ramp to the requested color; the old
// CR_UNTRANSLATED + DTA_Color path merely multiplied the glyph's natural red
// texture by the color, which rendered every tuple color as dark red.
// The translation is created on demand at render time, so nothing needs
// savegame serialization: after a load the script re-registers its items
// and the table is rebuilt.
void DrawTranslatedText(F2DDrawer* drawer, FFont* font, const PalEntry& color, const char* text,
	double x, double y, double alpha, double scaleX, double scaleY)
{
	const FTranslationID translation = CreateFontColorTranslation(color);
	DrawText(drawer, font, CR_NATIVEPAL, x, y, text,
		DTA_TranslationIndex, translation.index(),
		DTA_Alpha, alpha,
		DTA_ScaleX, scaleX,
		DTA_ScaleY, scaleY,
		TAG_DONE);
}

void DrawTextPixelsEx(F2DDrawer* drawer, FFont* font, bool useFontColor, EColorRange fontColor,
	const PalEntry& color, const char* text, double x, double y, double alpha,
	double scaleX, double scaleY, bool shadow, bool outline)
{
	// Outline/shadow offsets track the text size: a fixed 1px outline
	// vanishes next to large resolution-independent text.
	const double offset = std::max(1.0, scaleY * 0.5);
	// DTA_Shadow is dead in this fork (parms.shadowColor is never consumed),
	// so shadow and outline are drawn manually as extra translated passes
	// before the main pass.
	if (shadow)
	{
		DrawTranslatedText(drawer, font, PalEntry(255, 20, 20, 20), text, x + offset * 2.0, y + offset * 2.0, alpha, scaleX, scaleY);
	}
	if (outline)
	{
		const PalEntry black(255, 0, 0, 0);
		DrawTranslatedText(drawer, font, black, text, x - offset, y, alpha, scaleX, scaleY);
		DrawTranslatedText(drawer, font, black, text, x + offset, y, alpha, scaleX, scaleY);
		DrawTranslatedText(drawer, font, black, text, x, y - offset, alpha, scaleX, scaleY);
		DrawTranslatedText(drawer, font, black, text, x, y + offset, alpha, scaleX, scaleY);
	}
	if (useFontColor)
	{
		DrawText(drawer, font, fontColor, x, y, text,
			DTA_Alpha, alpha,
			DTA_ScaleX, scaleX,
			DTA_ScaleY, scaleY,
			TAG_DONE);
	}
	else
	{
		DrawTranslatedText(drawer, font, color, text, x, y, alpha, scaleX, scaleY);
	}
}

void DrawTextPixels(F2DDrawer* drawer, const DrawItem& item, double x, double y,
	double scaleX, double scaleY)
{
	DrawTextPixelsEx(drawer, item.Font, item.UseFontColor, item.FontColor, item.Color,
		item.Text.GetChars(), x, y, item.Alpha, scaleX, scaleY, item.Shadow, item.Outline);
}

// Effective pixel scale for a text item: NormHeight (> 0) sizes one text
// line to that fraction of the live drawer height; otherwise the stored
// raw pixel scale is used unchanged.
void EffectiveTextScale(const DrawItem& item, double drawerHeight, double& scaleX, double& scaleY)
{
	if (item.NormHeight > 0.0 && item.Font != nullptr && item.Font->GetHeight() > 0)
	{
		scaleX = scaleY = item.NormHeight * drawerHeight / item.Font->GetHeight();
	}
	else
	{
		scaleX = item.ScaleX;
		scaleY = item.ScaleY;
	}
}

void RenderRectItem(F2DDrawer* drawer, const DrawItem& item, double width, double height)
{
	const int w = static_cast<int>(item.X2 * width);
	const int h = static_cast<int>(item.Y2 * height);
	if (w <= 0 || h <= 0) return;
	PalEntry color = item.Color;
	color.a = static_cast<uint8_t>(std::clamp(item.Alpha, 0.0, 1.0) * 255.0);
	const int x = static_cast<int>(item.X * width);
	const int y = static_cast<int>(item.Y * height);
	if (item.HasColor2)
	{
		// Vertical gradient: color at the top edge, color2 at the bottom edge.
		PalEntry color2 = item.SecondaryColor;
		color2.a = color.a;
		drawer->AddColorOnlyGradientQuad(x, y, w, h, color, color2);
	}
	else
	{
		drawer->AddColorOnlyQuad(x, y, w, h, color);
	}
}

void RenderLineItem(F2DDrawer* drawer, const DrawItem& item, double width, double height)
{
	const DVector2 from(item.X * width, item.Y * height);
	const DVector2 to(item.X2 * width, item.Y2 * height);
	const uint8_t alpha = static_cast<uint8_t>(std::clamp(item.Alpha, 0.0, 1.0) * 255.0);
	drawer->AddLine(from, to, nullptr, static_cast<uint32_t>(item.Color) & 0xffffffu, alpha);
}

void RenderCircleItem(F2DDrawer* drawer, const DrawItem& item, double width, double height)
{
	const double cx = item.X * width;
	const double cy = item.Y * height;
	// The radius is X-normalized: it is scaled by the drawer width only, so a
	// circle stays circular regardless of the aspect ratio.
	const double r = item.X2 * width;
	if (r <= 0.0) return;
	const uint8_t alpha = static_cast<uint8_t>(std::clamp(item.Alpha, 0.0, 1.0) * 255.0);
	const uint32_t rgb = static_cast<uint32_t>(item.Color) & 0xffffffu;
	if (!item.Fill)
	{
		// 32-segment polyline loop.
		constexpr int segments = 32;
		constexpr double twoPi = 6.28318530717958647692;
		DVector2 prev(cx + r, cy);
		for (int i = 1; i <= segments; ++i)
		{
			const double angle = i * (twoPi / segments);
			const DVector2 point(cx + r * cos(angle), cy + r * sin(angle));
			drawer->AddLine(prev, point, nullptr, rgb, alpha);
			prev = point;
		}
	}
	else
	{
		// Filled: 48 horizontal chord scanlines. Kept as plain AddLine calls to
		// avoid texture/polygon machinery; gaps may show for radii under ~24px.
		constexpr int segments = 48;
		for (int i = 0; i < segments; ++i)
		{
			const double dy = -r + 2.0 * r * (i + 0.5) / segments;
			const double half = sqrt(std::max(0.0, r * r - dy * dy));
			drawer->AddLine(DVector2(cx - half, cy + dy), DVector2(cx + half, cy + dy), nullptr, rgb, alpha);
		}
	}
}

void RenderTextureItem(F2DDrawer* drawer, const DrawItem& item, double width, double height)
{
	if (item.Texture == nullptr || !item.Texture->isValid()) return;
	const double x = item.X * width;
	const double y = item.Y * height;
	if (item.HasTint)
	{
		if (item.Rotate != 0.0)
		{
			DrawTexture(drawer, item.Texture, x, y,
				DTA_Alpha, item.Alpha,
				DTA_ScaleX, item.ScaleX,
				DTA_ScaleY, item.ScaleY,
				DTA_FillColor, static_cast<int>(static_cast<uint32_t>(item.Color) & 0xffffffu),
				DTA_Rotate, item.Rotate,
				TAG_DONE);
		}
		else
		{
			DrawTexture(drawer, item.Texture, x, y,
				DTA_Alpha, item.Alpha,
				DTA_ScaleX, item.ScaleX,
				DTA_ScaleY, item.ScaleY,
				DTA_FillColor, static_cast<int>(static_cast<uint32_t>(item.Color) & 0xffffffu),
				TAG_DONE);
		}
	}
	else if (item.Rotate != 0.0)
	{
		DrawTexture(drawer, item.Texture, x, y,
			DTA_Alpha, item.Alpha,
			DTA_ScaleX, item.ScaleX,
			DTA_ScaleY, item.ScaleY,
			DTA_Rotate, item.Rotate,
			TAG_DONE);
	}
	else
	{
		DrawTexture(drawer, item.Texture, x, y,
			DTA_Alpha, item.Alpha,
			DTA_ScaleX, item.ScaleX,
			DTA_ScaleY, item.ScaleY,
			TAG_DONE);
	}
}

void RenderFrameItem(F2DDrawer* drawer, const DrawItem& item, double width, double height)
{
	// Hollow rectangle drawn as four AddColorOnlyQuad edges INSIDE the given
	// rect. The engine's DrawFrame (v_draw.cpp) was not used: it draws its
	// border outside the area and special-cases thickness -1, both surprising
	// for a persistent normalized-coordinate API.
	const int x = static_cast<int>(item.X * width);
	const int y = static_cast<int>(item.Y * height);
	const int w = static_cast<int>(item.X2 * width);
	const int h = static_cast<int>(item.Y2 * height);
	if (w <= 0 || h <= 0 || item.Thickness <= 0) return;
	PalEntry color = item.Color;
	color.a = static_cast<uint8_t>(std::clamp(item.Alpha, 0.0, 1.0) * 255.0);
	const int edgeH = std::min(item.Thickness, h);
	const int edgeW = std::min(item.Thickness, w);
	drawer->AddColorOnlyQuad(x, y, w, edgeH, color);
	drawer->AddColorOnlyQuad(x, y + h - edgeH, w, edgeH, color);
	drawer->AddColorOnlyQuad(x, y, edgeW, h, color);
	drawer->AddColorOnlyQuad(x + w - edgeW, y, edgeW, h, color);
}

bool ProjectAnchor(F2DDrawer* drawer, const DrawItem& item, AActor* actor, double& screenX, double& screenY,
	double* outDistance = nullptr, double* outDepth = nullptr, double* outFocalX = nullptr, double* outFocalY = nullptr)
{
	double distance = 0.0;
	const DVector3 pos = actor->Pos();
	if (!ProjectWorldToScreen(DVector3(pos.X + item.OffsetX, pos.Y + item.OffsetY, actor->Top() + item.OffsetZ), screenX, screenY, distance,
		outDepth, outFocalX, outFocalY))
	{
		return false;
	}
	if (distance > item.MaxDistance)
	{
		return false;
	}
	const double width = drawer->GetWidth();
	const double height = drawer->GetHeight();
	if (screenX < 0.0 || screenX >= width || screenY < 0.0 || screenY >= height)
	{
		return false;
	}
	if (outDistance != nullptr) *outDistance = distance;
	return true;
}

// Fades alpha linearly to zero over the last 20% of the MaxDistance range.
double DistanceFade(const DrawItem& item, double distance)
{
	const double fadeStart = item.MaxDistance * 0.8;
	if (distance <= fadeStart || item.MaxDistance <= fadeStart)
	{
		return 1.0;
	}
	return std::clamp((item.MaxDistance - distance) / (item.MaxDistance - fadeStart), 0.0, 1.0);
}

void RenderWorldBarItem(F2DDrawer* drawer, const DrawItem& item, AActor* actor)
{
	double screenX = 0.0;
	double screenY = 0.0;
	double distance = 0.0;
	if (!ProjectAnchor(drawer, item, actor, screenX, screenY, &distance)) return;
	const double fade = DistanceFade(item, distance);
	if (fade <= 0.0) return;

	double fraction = item.Frac;
	if (item.TrackHealth)
	{
		const int spawnHealth = actor->GetDefault()->health;
		fraction = spawnHealth > 0 ? static_cast<double>(actor->health) / spawnHealth : 0.0;
	}
	fraction = std::clamp(fraction, 0.0, 1.0);

	// Health-tracked bars without an explicit fg get a green/yellow/red
	// gradient driven by the current fraction.
	PalEntry fg = item.Color;
	if (item.TrackHealth && !item.FgExplicit)
	{
		if (fraction > 0.6) fg = PalEntry(255, 60, 200, 60);
		else if (fraction > 0.3) fg = PalEntry(255, 230, 200, 40);
		else fg = PalEntry(255, 230, 40, 30);
	}

	const int w = static_cast<int>(item.BarWidth * drawer->GetWidth());
	const int h = std::max(1, static_cast<int>(item.BarHeight * drawer->GetHeight()));
	if (w <= 0) return;
	const int x = static_cast<int>(screenX - w * 0.5);
	const int y = static_cast<int>(screenY - h);

	// Outer border: fully opaque black at the full bar size.
	constexpr int border = 2;
	PalEntry borderColor(255, 0, 0, 0);
	borderColor.a = static_cast<uint8_t>(255.0 * fade);
	drawer->AddColorOnlyQuad(x, y, w, h, borderColor);
	const int innerW = w - 2 * border;
	const int innerH = h - 2 * border;
	if (innerW > 0 && innerH > 0)
	{
		// Background: stored bg color inset by the border at ~85% opacity.
		PalEntry bg = item.SecondaryColor;
		bg.a = static_cast<uint8_t>(bg.a * 0.85 * fade);
		drawer->AddColorOnlyQuad(x + border, y + border, innerW, innerH, bg);
		const int fill = static_cast<int>(innerW * fraction);
		if (fill > 0)
		{
			fg.a = static_cast<uint8_t>(fg.a * fade);
			drawer->AddColorOnlyQuad(x + border, y + border, fill, innerH, fg);
		}
	}

	// Name label above the bar; inherits the bar's distance fade.
	if (item.Label && item.LabelFont != nullptr)
	{
		const char* name = actor->GetTag();
		const double labelWidth = item.LabelFont->StringWidth(name) * item.LabelScale;
		const double labelHeight = item.LabelFont->GetHeight() * item.LabelScale;
		DrawTextPixelsEx(drawer, item.LabelFont, item.LabelUseFontColor, item.LabelFontColor,
			item.LabelColor, name, screenX - labelWidth * 0.5, y - labelHeight - 2.0,
			fade, item.LabelScale, item.LabelScale, false, false);
	}
}

void RenderWorldTextItem(F2DDrawer* drawer, const DrawItem& item, AActor* actor)
{
	double screenX = 0.0;
	double screenY = 0.0;
	if (!ProjectAnchor(drawer, item, actor, screenX, screenY)) return;
	// height= sizes the label by screen-height fraction (resolution-
	// independent); scale= stays raw pixels for compatibility.
	double scaleX, scaleY;
	EffectiveTextScale(item, drawer->GetHeight(), scaleX, scaleY);
	const double textWidth = MeasureTextWidth(item.Font, item.Text.GetChars()) * scaleX;
	DrawTextPixels(drawer, item, screenX - textWidth * 0.5, screenY, scaleX, scaleY);
}

void RenderWorldTextureItem(F2DDrawer* drawer, const DrawItem& item, AActor* actor)
{
	if (item.Texture == nullptr || !item.Texture->isValid()) return;
	double screenX = 0.0;
	double screenY = 0.0;
	double distance = 0.0;
	double depth = 0.0;
	double focalX = 0.0;
	double focalY = 0.0;
	if (!ProjectAnchor(drawer, item, actor, screenX, screenY, &distance, &depth, &focalX, &focalY)) return;
	const double fade = DistanceFade(item, distance);
	if (fade <= 0.0) return;

	// World-unit size to pixels: px = worldUnits x focal / depth.
	const double pixelW = item.Size * focalX / depth;
	const double pixelH = item.Size * focalY / depth;
	const double texW = item.Texture->GetDisplayWidth();
	const double texH = item.Texture->GetDisplayHeight();
	if (pixelW <= 0.0 || pixelH <= 0.0 || texW <= 0.0 || texH <= 0.0) return;
	const double alpha = item.Alpha * fade;
	const double x = screenX - pixelW * 0.5;
	const double y = screenY - pixelH * 0.5;
	if (item.HasTint)
	{
		DrawTexture(drawer, item.Texture, x, y,
			DTA_Alpha, alpha,
			DTA_ScaleX, pixelW / texW,
			DTA_ScaleY, pixelH / texH,
			DTA_FillColor, static_cast<int>(static_cast<uint32_t>(item.Color) & 0xffffffu),
			TAG_DONE);
	}
	else
	{
		DrawTexture(drawer, item.Texture, x, y,
			DTA_Alpha, alpha,
			DTA_ScaleX, pixelW / texW,
			DTA_ScaleY, pixelH / texH,
			TAG_DONE);
	}
}

// Resolves one world-line endpoint: actor center (follows movement each
// frame) or the stored static point.
DVector3 LineEndpointPos(const TObjPtr<AActor*>& actor, bool hasActor, const DVector3& point)
{
	if (!hasActor) return point;
	const DVector3 pos = actor->Pos();
	return DVector3(pos.X, pos.Y, pos.Z + actor->Height * 0.5);
}

void RenderWorldLineItem(F2DDrawer* drawer, const DrawItem& item)
{
	const DVector3 a = LineEndpointPos(item.Actor, item.LineHasActorA, item.PointA);
	const DVector3 b = LineEndpointPos(item.ActorB, item.LineHasActorB, item.PointB);
	double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0, distance = 0.0;
	// Skip the whole line when either endpoint is behind the camera.
	if (!ProjectWorldToScreen(a, ax, ay, distance)) return;
	if (!ProjectWorldToScreen(b, bx, by, distance)) return;
	const uint8_t alpha = static_cast<uint8_t>(std::clamp(item.Alpha, 0.0, 1.0) * 255.0);
	drawer->AddLine(DVector2(ax, ay), DVector2(bx, by), nullptr,
		static_cast<uint32_t>(item.Color) & 0xffffffu, alpha);
}

// Flat ground ring around the actor's feet (Diablo-style affix aura): a
// Segments-sided polyline in the actor's floor plane, projected per
// segment; segments with an endpoint behind the camera are dropped.
void RenderWorldRingItem(F2DDrawer* drawer, const DrawItem& item, AActor* actor)
{
	if (item.Radius <= 0.0 || item.Segments < 3) return;
	const DVector3 pos = actor->Pos();
	const DVector3 center(pos.X, pos.Y, pos.Z + item.OffsetZ);
	double centerX = 0.0, centerY = 0.0, distance = 0.0;
	if (!ProjectWorldToScreen(center, centerX, centerY, distance)) return;
	if (distance > item.MaxDistance) return;
	const double fade = DistanceFade(item, distance);
	if (fade <= 0.0) return;
	const uint8_t alpha = static_cast<uint8_t>(std::clamp(item.Alpha, 0.0, 1.0) * fade * 255.0);
	const uint32_t rgb = static_cast<uint32_t>(item.Color) & 0xffffffu;
	const double step = 2.0 * M_PI / item.Segments;
	double prevX = 0.0, prevY = 0.0, segDistance = 0.0;
	bool prevValid = false;
	for (int segment = 0; segment <= item.Segments; ++segment)
	{
		const double angle = segment * step;
		const DVector3 point(center.X + cos(angle) * item.Radius,
			center.Y + sin(angle) * item.Radius, center.Z);
		double px = 0.0, py = 0.0;
		const bool valid = ProjectWorldToScreen(point, px, py, segDistance);
		if (valid && prevValid)
		{
			drawer->AddLine(DVector2(prevX, prevY), DVector2(px, py), nullptr, rgb, alpha);
		}
		prevX = px;
		prevY = py;
		prevValid = valid;
	}
}

} // namespace

namespace PythonDisplayList
{
bool HasItems()
{
	return !Items.empty();
}

void Render(F2DDrawer* drawer, int hudState)
{
	(void)hudState; // items are drawn for every HUD state, like RenderOverlay
	if (drawer == nullptr || Items.empty()) return;
	const double width = drawer->GetWidth();
	const double height = drawer->GetHeight();
	if (width <= 0.0 || height <= 0.0) return;

	// The sight test below runs on the main thread during HUD draw, like the
	// other playsim reads in this function (P_CheckSight uses the global
	// validcount/sightcounts state and is not thread-safe).
	AActor* viewer = players[consoleplayer].mo;

	// With no level loaded the map clock reads as zero: durations registered
	// before a level exists (ExpireTic == -2) stay pending and timed items
	// simply do not expire.
	const int64_t nowTic = primaryLevel != nullptr ? primaryLevel->maptime : 0;

	// Collect the surviving items, then draw them in painter's order sorted
	// by (layer, insertion sequence).
	std::vector<DrawItem*> sorted;
	sorted.reserve(Items.size());
	for (auto it = Items.begin(); it != Items.end(); )
	{
		DrawItem& item = it->second;
		if (item.ExpireTic == -2 && primaryLevel != nullptr)
		{
			item.ExpireTic = nowTic + item.DurationTics;
		}
		if (item.ExpireTic >= 0 && nowTic >= item.ExpireTic)
		{
			it = Items.erase(it);
			continue;
		}
		if (IsWorldKind(item.Kind))
		{
			const auto gone = [](const TObjPtr<AActor*>& actor)
			{
				return actor.Get() == nullptr || (actor->ObjectFlags & OF_EuthanizeMe);
			};
			bool anchorGone = false;
			if (item.Kind == DrawKind::WorldLine)
			{
				anchorGone = (item.LineHasActorA && gone(item.Actor)) ||
					(item.LineHasActorB && gone(item.ActorB));
			}
			else
			{
				anchorGone = gone(item.Actor);
			}
			if (anchorGone)
			{
				it = Items.erase(it);
				continue;
			}
		}
		sorted.push_back(&item);
		++it;
	}
	std::sort(sorted.begin(), sorted.end(), [](const DrawItem* a, const DrawItem* b)
	{
		if (a->Layer != b->Layer) return a->Layer < b->Layer;
		return a->Seq < b->Seq;
	});

	for (DrawItem* itemPtr : sorted)
	{
		DrawItem& item = *itemPtr;
		if (IsWorldKind(item.Kind))
		{
			if (item.Kind == DrawKind::WorldLine)
			{
				// World lines skip the occlusion, health and distance gates: the
				// endpoints already decide visibility (behind camera -> skipped).
				RenderWorldLineItem(drawer, item);
				continue;
			}
			AActor* actor = item.Actor.Get();
			// Dead actors keep their entry (they may be revived) but draw
			// nothing — except transient text (registered with duration=),
			// which plays out over the corpse: the killing blow is exactly
			// when combat feedback matters.
			const bool transientText = item.Kind == DrawKind::WorldText &&
				item.ExpireTic != -1;
			if (actor->health > 0 || transientText)
			{
				// v1 limitation: with chase-cam the occlusion test still runs
				// from the player actor, not from the chase camera.
				const bool occluded = item.Occlude &&
					(viewer == nullptr || !P_CheckSight(viewer, actor, 0));
				if (!occluded)
				{
					if (item.Kind == DrawKind::WorldBar) RenderWorldBarItem(drawer, item, actor);
					else if (item.Kind == DrawKind::WorldText) RenderWorldTextItem(drawer, item, actor);
					else if (item.Kind == DrawKind::WorldRing) RenderWorldRingItem(drawer, item, actor);
					else RenderWorldTextureItem(drawer, item, actor);
				}
			}
		}
		else if (item.Kind == DrawKind::Text)
		{
			double scaleX, scaleY;
			EffectiveTextScale(item, height, scaleX, scaleY);
			double x = item.X * width;
			if (item.Align != 0 && item.Font != nullptr)
			{
				const double textWidth = MeasureTextWidth(item.Font, item.Text.GetChars()) * scaleX;
				x -= item.Align == 1 ? textWidth * 0.5 : textWidth;
			}
			DrawTextPixels(drawer, item, x, item.Y * height, scaleX, scaleY);
		}
		else if (item.Kind == DrawKind::Rect)
		{
			RenderRectItem(drawer, item, width, height);
		}
		else if (item.Kind == DrawKind::Line)
		{
			RenderLineItem(drawer, item, width, height);
		}
		else if (item.Kind == DrawKind::Circle)
		{
			RenderCircleItem(drawer, item, width, height);
		}
		else if (item.Kind == DrawKind::Frame)
		{
			RenderFrameItem(drawer, item, width, height);
		}
		else
		{
			RenderTextureItem(drawer, item, width, height);
		}
	}
}

void PurgeWorldItems()
{
	for (auto it = Items.begin(); it != Items.end(); )
	{
		if (IsWorldKind(it->second.Kind)) it = Items.erase(it);
		else ++it;
	}
}

#ifdef BIASEDDOOM_PYTHON

namespace
{
bool ParseItemId(PyObject* object, uint32_t& id)
{
	if (object == nullptr)
	{
		PyErr_SetString(PyExc_TypeError, "id is required");
		return false;
	}
	const long long value = PyLong_AsLongLong(object);
	if (value == -1 && PyErr_Occurred()) return false;
	if (value < 0 || value > 0xffffffffLL)
	{
		PyErr_SetString(PyExc_ValueError, "id must be between 0 and 2**32 - 1");
		return false;
	}
	id = static_cast<uint32_t>(value);
	return true;
}

bool ParseColor(PyObject* object, const char* argument, PalEntry& color)
{
	PyObject* sequence = PySequence_Fast(object, "color must be an (r, g, b) sequence");
	if (sequence == nullptr) return false;
	if (PySequence_Fast_GET_SIZE(sequence) != 3)
	{
		Py_DECREF(sequence);
		PyErr_Format(PyExc_ValueError, "%s must contain exactly three components (r, g, b)", argument);
		return false;
	}
	uint8_t components[3];
	for (int index = 0; index < 3; ++index)
	{
		const long value = PyLong_AsLong(PySequence_Fast_GET_ITEM(sequence, index));
		if (value == -1 && PyErr_Occurred())
		{
			Py_DECREF(sequence);
			return false;
		}
		components[index] = static_cast<uint8_t>(std::clamp(value, 0L, 255L));
	}
	Py_DECREF(sequence);
	color = PalEntry(255, components[0], components[1], components[2]);
	return true;
}

bool ParseOptionalColor(PyObject* object, const char* argument, PalEntry fallback, PalEntry& color)
{
	if (object == nullptr || object == Py_None)
	{
		color = fallback;
		return true;
	}
	return ParseColor(object, argument, color);
}

bool ParseTextColorValue(PyObject* object, PalEntry& color, EColorRange& fontColor, bool& useFontColor)
{
	if (object == nullptr || object == Py_None)
	{
		useFontColor = false;
		color = PalEntry(255, 255, 255, 255);
		return true;
	}
	if (PyUnicode_Check(object))
	{
		const char* name = PyUnicode_AsUTF8(object);
		if (name == nullptr) return false;
		EColorRange range = V_FindFontColor(FName(name));
		if (range == CR_UNTRANSLATED) range = CR_GOLD;
		useFontColor = true;
		fontColor = range;
		return true;
	}
	PalEntry rgb;
	if (!ParseColor(object, "color", rgb)) return false;
	useFontColor = false;
	color = rgb;
	return true;
}

bool ParseTextColor(PyObject* object, DrawItem& item)
{
	return ParseTextColorValue(object, item.Color, item.FontColor, item.UseFontColor);
}

bool ParseScale(PyObject* object, double fallback, double& scaleX, double& scaleY)
{
	if (object == nullptr || object == Py_None)
	{
		scaleX = fallback;
		scaleY = fallback;
		return true;
	}
	if (PyFloat_Check(object) || PyLong_Check(object))
	{
		const double value = PyFloat_AsDouble(object);
		if (PyErr_Occurred()) return false;
		scaleX = value;
		scaleY = value;
		return true;
	}
	PyObject* sequence = PySequence_Fast(object, "scale must be a number or an (sx, sy) sequence");
	if (sequence == nullptr) return false;
	if (PySequence_Fast_GET_SIZE(sequence) != 2)
	{
		Py_DECREF(sequence);
		PyErr_SetString(PyExc_ValueError, "scale must be a number or an (sx, sy) sequence");
		return false;
	}
	scaleX = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sequence, 0));
	scaleY = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sequence, 1));
	const bool valid = !PyErr_Occurred();
	Py_DECREF(sequence);
	return valid;
}

FFont* ResolveFont(const char* name)
{
	FFont* font = V_GetFont(name);
	if (font == nullptr)
	{
		PyErr_Format(PyExc_ValueError, "unknown font '%s'", name);
	}
	return font;
}

// duration is seconds (float) or None for persistent items. When a level is
// loaded the expiry tic is computed immediately; otherwise the item is marked
// 'resolve on first render' (ExpireTic -2) and converted once a level exists.
bool ParseDuration(PyObject* object, DrawItem& item)
{
	if (object == nullptr || object == Py_None)
	{
		item.ExpireTic = -1;
		return true;
	}
	const double seconds = PyFloat_AsDouble(object);
	if (seconds == -1.0 && PyErr_Occurred()) return false;
	if (seconds < 0.0)
	{
		PyErr_SetString(PyExc_ValueError, "duration must be non-negative or None");
		return false;
	}
	const int64_t tics = static_cast<int64_t>(seconds * TICRATE + 0.5);
	if (primaryLevel != nullptr)
	{
		item.ExpireTic = primaryLevel->maptime + tics;
	}
	else
	{
		item.ExpireTic = -2;
		item.DurationTics = tics;
	}
	return true;
}

bool ParseAlign(const char* name, DrawItem& item)
{
	if (strcmp(name, "left") == 0) item.Align = 0;
	else if (strcmp(name, "center") == 0) item.Align = 1;
	else if (strcmp(name, "right") == 0) item.Align = 2;
	else
	{
		PyErr_Format(PyExc_ValueError, "unknown align '%s' (expected 'left', 'center' or 'right')", name);
		return false;
	}
	return true;
}

void StoreItem(uint32_t id, DrawItem& item)
{
	item.Seq = NextSeq++;
	Items[id] = std::move(item);
}

PyObject* PyDrawText(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* text = nullptr;
	PyObject* idObject = nullptr;
	double x = 0.0;
	double y = 0.0;
	const char* fontName = "smallfont";
	PyObject* colorObject = nullptr;
	PyObject* scaleObject = nullptr;
	double alpha = 1.0;
	int shadow = 0;
	int outline = 0;
	const char* alignName = "left";
	int layer = 0;
	double normHeight = 0.0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "text", "id", "x", "y", "font", "color", "scale", "alpha", "shadow", "outline", "align", "layer", "height", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|$OddsOOdppsidO:draw_text", const_cast<char**>(keywords),
		&text, &idObject, &x, &y, &fontName, &colorObject, &scaleObject, &alpha, &shadow, &outline, &alignName, &layer, &normHeight, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	FFont* font = ResolveFont(fontName);
	if (font == nullptr) return nullptr;
	if (normHeight < 0.0 || normHeight > 1.0)
	{
		PyErr_SetString(PyExc_ValueError, "height must be a normalized 0..1 screen-height fraction");
		return nullptr;
	}
	DrawItem item;
	item.Kind = DrawKind::Text;
	item.Text = text;
	item.Font = font;
	item.X = x;
	item.Y = y;
	if (!ParseTextColor(colorObject, item)) return nullptr;
	if (!ParseScale(scaleObject, 1.0, item.ScaleX, item.ScaleY)) return nullptr;
	item.NormHeight = normHeight;
	item.Alpha = alpha;
	item.Shadow = shadow != 0;
	item.Outline = outline != 0;
	if (!ParseAlign(alignName, item)) return nullptr;
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

PyObject* PyDrawRect(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* idObject = nullptr;
	double x = 0.0;
	double y = 0.0;
	double w = 0.0;
	double h = 0.0;
	PyObject* colorObject = nullptr;
	double alpha = 0.75;
	PyObject* color2Object = nullptr;
	int layer = 0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "id", "x", "y", "w", "h", "color", "alpha", "color2", "layer", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|$OddddOdOiO:draw_rect", const_cast<char**>(keywords),
		&idObject, &x, &y, &w, &h, &colorObject, &alpha, &color2Object, &layer, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	DrawItem item;
	item.Kind = DrawKind::Rect;
	item.X = x;
	item.Y = y;
	item.X2 = w;
	item.Y2 = h;
	if (!ParseOptionalColor(colorObject, "color", PalEntry(255, 255, 255, 255), item.Color)) return nullptr;
	item.Alpha = alpha;
	if (color2Object != nullptr && color2Object != Py_None)
	{
		if (!ParseColor(color2Object, "color2", item.SecondaryColor)) return nullptr;
		item.HasColor2 = true;
	}
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

PyObject* PyDrawLine(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* idObject = nullptr;
	double x1 = 0.0;
	double y1 = 0.0;
	double x2 = 0.0;
	double y2 = 0.0;
	PyObject* colorObject = nullptr;
	double alpha = 1.0;
	int layer = 0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "id", "x1", "y1", "x2", "y2", "color", "alpha", "layer", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|$OddddOdiO:draw_line", const_cast<char**>(keywords),
		&idObject, &x1, &y1, &x2, &y2, &colorObject, &alpha, &layer, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	DrawItem item;
	item.Kind = DrawKind::Line;
	item.X = x1;
	item.Y = y1;
	item.X2 = x2;
	item.Y2 = y2;
	if (!ParseOptionalColor(colorObject, "color", PalEntry(255, 255, 255, 255), item.Color)) return nullptr;
	item.Alpha = alpha;
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

PyObject* PyDrawTexture(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* name = nullptr;
	PyObject* idObject = nullptr;
	double x = 0.0;
	double y = 0.0;
	PyObject* scaleObject = nullptr;
	double alpha = 1.0;
	PyObject* tintObject = nullptr;
	double rotate = 0.0;
	int layer = 0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "name", "id", "x", "y", "scale", "alpha", "tint", "rotate", "layer", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|$OddOdOdiO:draw_texture", const_cast<char**>(keywords),
		&name, &idObject, &x, &y, &scaleObject, &alpha, &tintObject, &rotate, &layer, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	FGameTexture* texture = TexMan.FindGameTexture(name, ETextureType::MiscPatch, FTextureManager::TEXMAN_TryAny);
	if (texture == nullptr || !texture->isValid())
	{
		PyErr_Format(PyExc_ValueError, "unknown texture '%s'", name);
		return nullptr;
	}
	DrawItem item;
	item.Kind = DrawKind::Texture;
	item.Texture = texture;
	item.X = x;
	item.Y = y;
	if (!ParseScale(scaleObject, 1.0, item.ScaleX, item.ScaleY)) return nullptr;
	item.Alpha = alpha;
	if (tintObject != nullptr && tintObject != Py_None)
	{
		if (!ParseColor(tintObject, "tint", item.Color)) return nullptr;
		item.HasTint = true;
	}
	item.Rotate = rotate;
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

AActor* ResolveAnchorActor(PyObject* object)
{
	if (object == nullptr)
	{
		PyErr_SetString(PyExc_TypeError, "actor is required");
		return nullptr;
	}
	return PythonRuntime::GameApi::ActorFromHandle(object);
}

PyObject* PyDrawWorldBar(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* actorObject = nullptr;
	PyObject* idObject = nullptr;
	double offsetZ = 0.0;
	double barWidth = 0.06;
	double barHeight = 0.008;
	PyObject* trackObject = nullptr;
	PyObject* fracObject = nullptr;
	PyObject* fgObject = nullptr;
	PyObject* bgObject = nullptr;
	double maxDistance = 2048.0;
	int occlude = 1;
	int label = 0;
	PyObject* labelColorObject = nullptr;
	double labelScale = 1.5;
	const char* labelFontName = "smallfont";
	int layer = 0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "actor", "id", "offset_z", "width", "height", "track", "frac", "fg", "bg", "max_distance", "occlude", "label", "label_color", "label_scale", "label_font", "layer", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$OdddOOOOdppOdsiO:draw_world_bar", const_cast<char**>(keywords),
		&actorObject, &idObject, &offsetZ, &barWidth, &barHeight, &trackObject, &fracObject, &fgObject, &bgObject, &maxDistance,
		&occlude, &label, &labelColorObject, &labelScale, &labelFontName, &layer, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	AActor* actor = ResolveAnchorActor(actorObject);
	if (actor == nullptr) return nullptr;
	DrawItem item;
	item.Kind = DrawKind::WorldBar;
	item.Actor = actor;
	item.OffsetZ = offsetZ;
	item.BarWidth = barWidth;
	item.BarHeight = barHeight;
	item.MaxDistance = maxDistance;
	if (trackObject == nullptr)
	{
		item.TrackHealth = true;
	}
	else if (PyUnicode_Check(trackObject))
	{
		const char* track = PyUnicode_AsUTF8(trackObject);
		if (track == nullptr) return nullptr;
		if (FString(track).CompareNoCase("health") != 0)
		{
			PyErr_Format(PyExc_ValueError, "unknown track '%s' (expected 'health' or None)", track);
			return nullptr;
		}
		item.TrackHealth = true;
	}
	else if (trackObject == Py_None)
	{
		if (fracObject == nullptr || fracObject == Py_None)
		{
			PyErr_SetString(PyExc_ValueError, "frac is required when track is None");
			return nullptr;
		}
		const double frac = PyFloat_AsDouble(fracObject);
		if (frac == -1.0 && PyErr_Occurred()) return nullptr;
		if (frac < 0.0 || frac > 1.0)
		{
			PyErr_SetString(PyExc_ValueError, "frac must be between 0 and 1");
			return nullptr;
		}
		item.TrackHealth = false;
		item.Frac = frac;
	}
	else
	{
		PyErr_SetString(PyExc_TypeError, "track must be 'health' or None");
		return nullptr;
	}
	item.FgExplicit = fgObject != nullptr && fgObject != Py_None;
	if (!ParseOptionalColor(fgObject, "fg", PalEntry(255, 220, 40, 40), item.Color)) return nullptr;
	if (!ParseOptionalColor(bgObject, "bg", PalEntry(255, 20, 20, 20), item.SecondaryColor)) return nullptr;
	item.Occlude = occlude != 0;
	if (label != 0)
	{
		FFont* labelFont = ResolveFont(labelFontName);
		if (labelFont == nullptr) return nullptr;
		item.Label = true;
		item.LabelFont = labelFont;
		item.LabelScale = labelScale;
		if (!ParseTextColorValue(labelColorObject, item.LabelColor, item.LabelFontColor, item.LabelUseFontColor)) return nullptr;
	}
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

PyObject* PyDrawWorldText(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* actorObject = nullptr;
	PyObject* idObject = nullptr;
	const char* text = nullptr;
	double offsetX = 0.0;
	double offsetY = 0.0;
	double offsetZ = 0.0;
	const char* fontName = "smallfont";
	PyObject* colorObject = nullptr;
	PyObject* scaleObject = nullptr;
	double alpha = 1.0;
	double maxDistance = 2048.0;
	int occlude = 1;
	int shadow = 0;
	int outline = 0;
	int layer = 0;
	double normHeight = 0.0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "actor", "id", "text", "offset_x", "offset_y", "offset_z", "font", "color", "scale", "alpha", "max_distance", "occlude", "shadow", "outline", "layer", "height", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$OsdddsOOddpppidO:draw_world_text", const_cast<char**>(keywords),
		&actorObject, &idObject, &text, &offsetX, &offsetY, &offsetZ, &fontName, &colorObject, &scaleObject, &alpha, &maxDistance, &occlude,
		&shadow, &outline, &layer, &normHeight, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	if (text == nullptr)
	{
		PyErr_SetString(PyExc_TypeError, "text is required");
		return nullptr;
	}
	AActor* actor = ResolveAnchorActor(actorObject);
	if (actor == nullptr) return nullptr;
	FFont* font = ResolveFont(fontName);
	if (font == nullptr) return nullptr;
	DrawItem item;
	item.Kind = DrawKind::WorldText;
	item.Actor = actor;
	item.Text = text;
	item.Font = font;
	item.OffsetX = offsetX;
	item.OffsetY = offsetY;
	item.OffsetZ = offsetZ;
	item.MaxDistance = maxDistance;
	if (!ParseTextColor(colorObject, item)) return nullptr;
	if (!ParseScale(scaleObject, 0.75, item.ScaleX, item.ScaleY)) return nullptr;
	if (normHeight < 0.0 || normHeight > 1.0)
	{
		PyErr_SetString(PyExc_ValueError, "height must be a normalized 0..1 screen-height fraction");
		return nullptr;
	}
	item.NormHeight = normHeight;
	item.Alpha = alpha;
	item.Occlude = occlude != 0;
	item.Shadow = shadow != 0;
	item.Outline = outline != 0;
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

PyObject* PyMeasureText(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* text = nullptr;
	const char* fontName = "smallfont";
	PyObject* scaleObject = nullptr;
	static const char* keywords[] = { "text", "font", "scale", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|$sO:measure_text", const_cast<char**>(keywords),
		&text, &fontName, &scaleObject)) return nullptr;
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	FFont* font = ResolveFont(fontName);
	if (font == nullptr) return nullptr;
	double scaleX = 1.0;
	double scaleY = 1.0;
	if (!ParseScale(scaleObject, 1.0, scaleX, scaleY)) return nullptr;
	// Dimensions are returned in PIXELS (unambiguous; no drawer exists at bind
	// time): width = widest line, height = line count x font line height.
	const double w = MeasureTextWidth(font, text) * scaleX;
	const double h = CountTextLines(text) * font->GetHeight() * scaleY;
	return Py_BuildValue("(dd)", w, h);
}

PyObject* PyDrawCircle(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* idObject = nullptr;
	double x = 0.0;
	double y = 0.0;
	double radius = 0.0;
	PyObject* colorObject = nullptr;
	double alpha = 1.0;
	int fill = 0;
	int layer = 0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "id", "x", "y", "radius", "color", "alpha", "fill", "layer", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|$OdddOdpiO:draw_circle", const_cast<char**>(keywords),
		&idObject, &x, &y, &radius, &colorObject, &alpha, &fill, &layer, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	DrawItem item;
	item.Kind = DrawKind::Circle;
	item.X = x;
	item.Y = y;
	item.X2 = radius; // X-normalized radius (scaled by the drawer width only)
	if (!ParseOptionalColor(colorObject, "color", PalEntry(255, 255, 255, 255), item.Color)) return nullptr;
	item.Alpha = alpha;
	item.Fill = fill != 0;
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

PyObject* PyDrawFrame(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* idObject = nullptr;
	double x = 0.0;
	double y = 0.0;
	double w = 0.0;
	double h = 0.0;
	PyObject* colorObject = nullptr;
	int thickness = 2;
	double alpha = 1.0;
	int layer = 0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "id", "x", "y", "w", "h", "color", "thickness", "alpha", "layer", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|$OddddOidiO:draw_frame", const_cast<char**>(keywords),
		&idObject, &x, &y, &w, &h, &colorObject, &thickness, &alpha, &layer, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	DrawItem item;
	item.Kind = DrawKind::Frame;
	item.X = x;
	item.Y = y;
	item.X2 = w;
	item.Y2 = h;
	if (!ParseOptionalColor(colorObject, "color", PalEntry(255, 255, 255, 255), item.Color)) return nullptr;
	item.Thickness = thickness;
	item.Alpha = alpha;
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

PyObject* PyDrawWorldTexture(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* actorObject = nullptr;
	const char* name = nullptr;
	PyObject* idObject = nullptr;
	double offsetZ = 0.0;
	double size = 24.0;
	double alpha = 1.0;
	PyObject* tintObject = nullptr;
	int occlude = 1;
	double maxDistance = 2048.0;
	int layer = 0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "actor", "name", "id", "offset_z", "size", "alpha", "tint", "occlude", "max_distance", "layer", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Os|$OdddOpdiO:draw_world_texture", const_cast<char**>(keywords),
		&actorObject, &name, &idObject, &offsetZ, &size, &alpha, &tintObject, &occlude, &maxDistance, &layer, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	AActor* actor = ResolveAnchorActor(actorObject);
	if (actor == nullptr) return nullptr;
	FGameTexture* texture = TexMan.FindGameTexture(name, ETextureType::MiscPatch, FTextureManager::TEXMAN_TryAny);
	if (texture == nullptr || !texture->isValid())
	{
		PyErr_Format(PyExc_ValueError, "unknown texture '%s'", name);
		return nullptr;
	}
	DrawItem item;
	item.Kind = DrawKind::WorldTexture;
	item.Actor = actor;
	item.Texture = texture;
	item.OffsetZ = offsetZ;
	item.Size = size;
	item.Alpha = alpha;
	item.MaxDistance = maxDistance;
	item.Occlude = occlude != 0;
	if (tintObject != nullptr && tintObject != Py_None)
	{
		if (!ParseColor(tintObject, "tint", item.Color)) return nullptr;
		item.HasTint = true;
	}
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

// Parses one world-line endpoint: an actor handle/TID (anchored at the
// actor's center, followed per frame) or an (x, y, z) sequence (static).
bool ParseLineEndpoint(PyObject* object, const char* argument, DrawItem& item, bool isB)
{
	if (object == nullptr)
	{
		PyErr_Format(PyExc_TypeError, "%s is required", argument);
		return false;
	}
	if (PyTuple_Check(object) || PyList_Check(object))
	{
		PyObject* sequence = PySequence_Fast(object, "endpoint must be an actor or an (x, y, z) sequence");
		if (sequence == nullptr) return false;
		if (PySequence_Fast_GET_SIZE(sequence) != 3)
		{
			Py_DECREF(sequence);
			PyErr_Format(PyExc_ValueError, "%s must contain exactly three components (x, y, z)", argument);
			return false;
		}
		double components[3];
		for (int index = 0; index < 3; ++index)
		{
			components[index] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sequence, index));
			if (components[index] == -1.0 && PyErr_Occurred())
			{
				Py_DECREF(sequence);
				return false;
			}
		}
		Py_DECREF(sequence);
		if (isB)
		{
			item.PointB = DVector3(components[0], components[1], components[2]);
			item.LineHasActorB = false;
		}
		else
		{
			item.PointA = DVector3(components[0], components[1], components[2]);
			item.LineHasActorA = false;
		}
		return true;
	}
	AActor* actor = PythonRuntime::GameApi::ActorFromHandle(object);
	if (actor == nullptr) return false;
	if (isB)
	{
		item.ActorB = actor;
		item.LineHasActorB = true;
	}
	else
	{
		item.Actor = actor;
		item.LineHasActorA = true;
	}
	return true;
}

PyObject* PyDrawWorldLine(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* aObject = nullptr;
	PyObject* bObject = nullptr;
	PyObject* idObject = nullptr;
	PyObject* colorObject = nullptr;
	double alpha = 1.0;
	int layer = 0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "a", "b", "id", "color", "alpha", "layer", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|$OOdiO:draw_world_line", const_cast<char**>(keywords),
		&aObject, &bObject, &idObject, &colorObject, &alpha, &layer, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	DrawItem item;
	item.Kind = DrawKind::WorldLine;
	if (!ParseLineEndpoint(aObject, "a", item, false)) return nullptr;
	if (!ParseLineEndpoint(bObject, "b", item, true)) return nullptr;
	if (!ParseOptionalColor(colorObject, "color", PalEntry(255, 255, 255, 255), item.Color)) return nullptr;
	item.Alpha = alpha;
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

PyObject* PyDrawWorldRing(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* actorObject = nullptr;
	PyObject* idObject = nullptr;
	double radius = 20.0;
	PyObject* colorObject = nullptr;
	double alpha = 1.0;
	double offsetZ = 2.0;
	int segments = 28;
	double maxDistance = 2048.0;
	int occlude = 1;
	int layer = 0;
	PyObject* durationObject = nullptr;
	static const char* keywords[] = { "actor", "id", "radius", "color", "alpha", "offset_z", "segments", "max_distance", "occlude", "layer", "duration", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$OdOddidpiO:draw_world_ring", const_cast<char**>(keywords),
		&actorObject, &idObject, &radius, &colorObject, &alpha, &offsetZ, &segments, &maxDistance,
		&occlude, &layer, &durationObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	AActor* actor = ResolveAnchorActor(actorObject);
	if (actor == nullptr) return nullptr;
	if (radius <= 0.0)
	{
		PyErr_SetString(PyExc_ValueError, "radius must be positive (world units)");
		return nullptr;
	}
	if (segments < 3 || segments > 128)
	{
		PyErr_SetString(PyExc_ValueError, "segments must be between 3 and 128");
		return nullptr;
	}
	DrawItem item;
	item.Kind = DrawKind::WorldRing;
	item.Actor = actor;
	item.Radius = radius;
	item.OffsetZ = offsetZ;
	item.Segments = segments;
	item.MaxDistance = maxDistance;
	if (!ParseOptionalColor(colorObject, "color", PalEntry(255, 255, 255, 255), item.Color)) return nullptr;
	item.Alpha = alpha;
	item.Occlude = occlude != 0;
	item.Layer = layer;
	if (!ParseDuration(durationObject, item)) return nullptr;
	StoreItem(id, item);
	Py_RETURN_NONE;
}

PyObject* PyDrawClear(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* idObject = nullptr;
	static const char* keywords[] = { "id", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:draw_clear", const_cast<char**>(keywords), &idObject)) return nullptr;
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	uint32_t id = 0;
	if (!ParseItemId(idObject, id)) return nullptr;
	Items.erase(id);
	Py_RETURN_NONE;
}

PyObject* PyDrawClearAll(PyObject*, PyObject*)
{
	if (!PythonRuntime::CheckLocalPresentation()) return nullptr;
	Items.clear();
	Py_RETURN_NONE;
}

void MarkRoots()
{
	for (auto& pair : Items)
	{
		// Only root live actors from the primary level: cross-level and dying
		// (OF_EuthanizeMe) actors must not be kept alive by display items.
		AActor* actor = pair.second.Actor.ForceGet();
		if (actor != nullptr && !(actor->ObjectFlags & OF_EuthanizeMe) && actor->Level == primaryLevel)
		{
			GC::Mark(pair.second.Actor);
		}
		actor = pair.second.ActorB.ForceGet();
		if (actor != nullptr && !(actor->ObjectFlags & OF_EuthanizeMe) && actor->Level == primaryLevel)
		{
			GC::Mark(pair.second.ActorB);
		}
	}
}

PyMethodDef DisplayListMethods[] = {
	{ "draw_text", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawText), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent screen-space text item." },
	{ "draw_rect", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawRect), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent screen-space filled rectangle." },
	{ "draw_line", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawLine), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent screen-space line." },
	{ "draw_texture", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawTexture), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent screen-space texture." },
	{ "draw_world_bar", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawWorldBar), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent actor-anchored bar (health tracking by default)." },
	{ "draw_world_text", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawWorldText), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent actor-anchored text label." },
	{ "draw_circle", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawCircle), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent screen-space circle (radius is X-normalized)." },
	{ "draw_frame", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawFrame), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent screen-space hollow rectangle (border inside the rect)." },
	{ "draw_world_texture", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawWorldTexture), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent actor-anchored world-space texture (sprite/icon)." },
	{ "draw_world_line", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawWorldLine), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent 1px world-space line between two actors or static points." },
	{ "draw_world_ring", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawWorldRing), METH_VARARGS | METH_KEYWORDS, "Register or replace a persistent actor-anchored ground ring (affix aura)." },
	{ "measure_text", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyMeasureText), METH_VARARGS | METH_KEYWORDS, "Measure a string for a font and scale; returns (w, h) in pixels." },
	{ "draw_clear", BD_DISPLAYLIST_KEYWORD_FUNCTION(PyDrawClear), METH_VARARGS | METH_KEYWORDS, "Remove the display-list item with the given id." },
	{ "draw_clear_all", PyDrawClearAll, METH_NOARGS, "Remove every display-list item." },
	{ nullptr, nullptr, 0, nullptr },
};

bool MarkerRegistered = false;
} // namespace

bool AddFunctions(_object* module)
{
	if (!MarkerRegistered)
	{
		GC::AddMarkerFunc(MarkRoots);
		MarkerRegistered = true;
	}
	return PyModule_AddFunctions(reinterpret_cast<PyObject*>(module), DisplayListMethods) == 0;
}

#else

bool AddFunctions(_object*)
{
	return false;
}

#endif
} // namespace PythonDisplayList
