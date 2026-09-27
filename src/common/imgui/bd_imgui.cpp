//===========================================================================
//
// bd_imgui.cpp
//
// Dear ImGui engine overlay layer for BiasedDoom.
//
// Rendering goes through the global F2DDrawer ("twod") exactly like the
// console and menus do, so every hardware backend (OpenGL, Vulkan, GLES,
// softpoly) renders the overlay without any backend-specific code. The
// translation of ImDrawData into F2DDrawer render commands is modeled on
// F2DDrawer::AddPoly(), extended with per-vertex colors.
//
//===========================================================================

#include "bd_imgui.h"

#include "c_cvars.h"
#include "c_console.h"
#include "c_dispatch.h"
#include "printf.h"

// The master switch is a normal cvar so it also exists (and is documented)
// in builds without ImGui; the stubs below simply ignore it.
CUSTOM_CVAR(Bool, py_imgui, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}

#ifdef BIASEDDOOM_IMGUI

#include <algorithm>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "imgui.h"

#include "d_eventbase.h"
#include "d_gui.h"
#include "v_2ddrawer.h"
#include "v_draw.h"
#include "textures.h"
#include "texturemanager.h"
#include "image.h"
#include "bitmap.h"
#include "renderstyle.h"
#include "palentry.h"
#include "python/python_runtime.h"

namespace BdImGui
{

static ImGuiContext* sContext = nullptr;
static FGameTexture* sFontTexture = nullptr;
static bool sDemoVisible = false;
static bool sFrameActive = false;
static bool sNavEnabled = true;	// mirrors io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard

// Runtime font registry. Source bytes are arena-owned (never freed), so
// atlas rebuilds can re-add fonts without dangling. Each atlas upload gets
// its own pixel block; old atlas textures stay alive in TexMan (draw data
// produced before a rebuild keeps referencing them), which makes every
// upload effectively process-lifetime memory. Growth is therefore capped at
// MaxAtlasUploads: once the cap is hit, further rebuilds and font-registry
// mutations are refused (loud warning, last valid atlas and font set
// retained).
struct BdFontEntry
{
	FString Name;
	ImFont* Font = nullptr;				// valid only while the current atlas lives
	TArray<uint8_t>* Data = nullptr;	// TTF bytes for script fonts, nullptr for built-ins
	float SizePixels = 0.0f;
	bool Bitmap = false;				// built-in: bitmap vs vector default data
	bool Permanent = false;				// engine-managed, refuses removal
};

static TArray<BdFontEntry> sFonts;
static TDeletingArray<TArray<uint8_t>*> sFontDataArena;
static TDeletingArray<TArray<uint8_t>*> sAtlasPixelArena;
static int sDefaultFontIndex = -1;
static bool sPendingAtlasRebuild = false;	// mutation arrived mid-frame
static bool sPreInitAtlasDirty = false;		// mutation before the first Frame()
static unsigned int sAtlasSerial = 0;
static unsigned int sAtlasUploadCount = 0;	// successful atlas texture uploads (process lifetime)
static bool sAtlasCapWarned = false;
enum { MaxAtlasUploads = 32 };	// generous bound; each upload owns its own pixel block forever
static float sUiScale = 1.0f;

//===========================================================================
//
// Runtime-created texture for the ImGui font atlas. Follows the same path
// as the engine's other generated textures: FImageSource subclass (arena
// allocated, no destructible members) -> FImageTexture -> MakeGameTexture
// -> TexMan.AddGameTexture, which transfers ownership to the texture
// manager for the remainder of the process lifetime.
//
// Every atlas upload owns its own pixel block (held by the layer, never
// reused) so that rebuilding the atlas under a new unique texture name
// cannot disturb older atlas textures that in-flight or cached draw data
// may still reference.
//
//===========================================================================

class FBdImGuiAtlasImage : public FImageSource
{
public:
	FBdImGuiAtlasImage(const TArray<uint8_t>* pixels, int width, int height) : Pixels(pixels)
	{
		Width = width;
		Height = height;
		bMasked = true;		// atlas alpha is coverage; holes everywhere
		bTranslucent = 1;
	}

	int CopyPixels(FBitmap* bmp, int conversion, int frame) override
	{
		bmp->CopyPixelDataRGB(0, 0, Pixels->Data(), Width, Height, 4, Width * 4, 0, CF_RGBA);
		return 0;
	}

private:
	const TArray<uint8_t>* Pixels;
};

// Uploads an atlas pixel set as a new uniquely named game texture and binds
// it as the ImGui font texture. Returns the texture, or nullptr on failure.
static FGameTexture* UploadAtlasTexture(const char* name, const unsigned char* pixels, int width, int height)
{
	if (pixels == nullptr || width <= 0 || height <= 0) return nullptr;

	// Keep the RGBA data as-is: RGB is white, A is glyph coverage. The 2D
	// path multiplies texture color with the per-vertex color, which is
	// exactly what ImGui expects.
	TArray<uint8_t>* owned = new TArray<uint8_t>();
	owned->Resize(width * height * 4);
	memcpy(owned->Data(), pixels, (size_t)width * (size_t)height * 4);
	sAtlasPixelArena.Push(owned);

	auto img = new FBdImGuiAtlasImage(owned, width, height);
	auto tex = MakeGameTexture(new FImageTexture(img), name, ETextureType::MiscPatch);
	TexMan.AddGameTexture(tex);
	sAtlasUploadCount++;
	return tex;
}

// Initial atlas build at context creation: build the stock default font,
// upload it and register it in the font registry as "Default".
static void CreateAtlasTexture()
{
	ImGuiIO& io = ImGui::GetIO();
	unsigned char* pixels = nullptr;
	int width = 0, height = 0;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

	FString name;
	name.Format("imgui_font_atlas_%u", sAtlasSerial++);
	FGameTexture* tex = UploadAtlasTexture(name.GetChars(), pixels, width, height);
	if (tex == nullptr) return;
	sFontTexture = tex;
	io.Fonts->SetTexID((ImTextureID)(intptr_t)tex);

	if (io.Fonts->Fonts.Size > 0)
	{
		ImFont* font = io.Fonts->Fonts[0];
		BdFontEntry entry;
		entry.Name = "Default";
		entry.Font = font;
		entry.SizePixels = font->LegacySize;
		entry.Permanent = true;
		// AddFontDefault() picks ProggyClean (bitmap) or ProggyForever
		// (vector) by expected context size; recover the choice from the
		// baked source name so RebuildAtlas() re-adds the same kind.
		entry.Bitmap = font->Sources.Size > 0 && strstr(font->Sources[0]->Name, "ProggyClean") != nullptr;
		sFonts.Push(std::move(entry));
		sDefaultFontIndex = (int)sFonts.Size() - 1;
	}
}

//===========================================================================
//
// Font registry. Mutations re-add every registered font on a cleared atlas
// and upload a fresh texture. Must not run while the atlas is locked
// (inside NewFrame..Render); MarkAtlasDirty() routes those cases to a
// pending rebuild at the end of Frame().
//
//===========================================================================

// Returns true once the atlas upload cap is reached, logging the one-time
// loud warning on the first refusal. Past the cap the registry must not
// diverge from the last valid atlas/font set, so the Python-facing mutation
// APIs refuse as well (SetDefaultFont stays allowed: it only re-selects an
// already-valid font).
static bool AtlasCapReached()
{
	if (sAtlasUploadCount < MaxAtlasUploads) return false;
	if (!sAtlasCapWarned)
	{
		sAtlasCapWarned = true;
		Printf(TEXTCOLOR_RED "bd.imgui: font atlas upload cap (%d) reached; further font changes are ignored\n"
			TEXTCOLOR_RED "and the last valid atlas/font set stays active. Restart the engine to load more fonts.\n",
			MaxAtlasUploads);
	}
	return true;
}

// Rebuilds the atlas from the registry and uploads a new texture. The
// caller must guarantee the atlas is unlocked (outside the frame).
static void RebuildAtlas()
{
	if (sContext == nullptr) return;
	ImGui::SetCurrentContext(sContext);
	ImFontAtlas* atlas = ImGui::GetIO().Fonts;
	if (atlas->Locked)
	{
		sPendingAtlasRebuild = true;
		return;
	}

	// Refuse to grow texture memory without bound: every upload owns its pixel
	// block for the process lifetime, so cap the number of uploads and keep
	// serving the last valid atlas/fonts past that point.
	if (AtlasCapReached())
		return;

	atlas->Clear();
	for (BdFontEntry& entry : sFonts)
	{
		if (entry.Data != nullptr)
		{
			ImFontConfig cfg;
			cfg.FontDataOwnedByAtlas = false;	// the layer owns the bytes
			entry.Font = atlas->AddFontFromMemoryTTF(entry.Data->Data(), (int)entry.Data->Size(),
				entry.SizePixels, &cfg);
		}
		else if (entry.Bitmap)
		{
			ImFontConfig cfg;
			cfg.SizePixels = entry.SizePixels;
			entry.Font = atlas->AddFontDefaultBitmap(&cfg);
		}
		else
		{
			ImFontConfig cfg;
			cfg.SizePixels = entry.SizePixels;
			entry.Font = atlas->AddFontDefaultVector(&cfg);
		}
		if (entry.Font == nullptr)
			Printf(TEXTCOLOR_YELLOW "bd.imgui: font '%s' failed to load; it renders as the atlas default.\n",
				entry.Name.GetChars());
	}

	unsigned char* pixels = nullptr;
	int width = 0, height = 0;
	atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
	FString name;
	name.Format("imgui_font_atlas_%u", sAtlasSerial++);
	FGameTexture* tex = UploadAtlasTexture(name.GetChars(), pixels, width, height);
	if (tex == nullptr)
	{
		Printf(TEXTCOLOR_RED "bd.imgui: font atlas rebuild failed to upload a texture.\n");
		return;
	}
	sFontTexture = tex;
	atlas->SetTexID((ImTextureID)(intptr_t)tex);
}

// Applies a registry mutation: rebuild immediately when idle, defer to the
// end of Frame() when called from inside a frame, or remember for Init()
// when the context does not exist yet.
static void MarkAtlasDirty()
{
	if (sContext == nullptr)
	{
		sPreInitAtlasDirty = true;
		return;
	}
	if (sFrameActive)
	{
		sPendingAtlasRebuild = true;
		return;
	}
	RebuildAtlas();
}

static int FindFontIndex(const char* name)
{
	for (int i = 0; i < (int)sFonts.Size(); i++)
		if (sFonts[i].Name.CompareNoCase(name) == 0) return i;
	return -1;
}

bool AddFontTTF(const char* name, const void* data, size_t dataSize, float sizePixels)
{
	if (name == nullptr || name[0] == '\0')
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: add_font_ttf rejected an empty font name.\n");
		return false;
	}
	if (data == nullptr || dataSize == 0)
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: add_font_ttf('%s') rejected empty font data.\n", name);
		return false;
	}
	if (FindFontIndex(name) >= 0)
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: font '%s' already exists; duplicate registration is not allowed.\n", name);
		return false;
	}
	if (AtlasCapReached())
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: add_font_ttf('%s') refused: the font atlas upload cap is reached.\n", name);
		return false;
	}

	BdFontEntry entry;
	entry.Name = name;
	entry.SizePixels = (float)std::clamp((double)sizePixels, 4.0, 96.0);
	TArray<uint8_t>* block = new TArray<uint8_t>();
	block->Resize((int)dataSize);
	memcpy(block->Data(), data, dataSize);
	sFontDataArena.Push(block);
	entry.Data = block;
	sFonts.Push(std::move(entry));
	MarkAtlasDirty();
	return true;
}

bool AddFontDefault(const char* name, float sizePixels, bool bitmap)
{
	if (name == nullptr || name[0] == '\0')
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: add_font_default rejected an empty font name.\n");
		return false;
	}
	if (FindFontIndex(name) >= 0)
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: font '%s' already exists; duplicate registration is not allowed.\n", name);
		return false;
	}
	if (AtlasCapReached())
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: add_font_default('%s') refused: the font atlas upload cap is reached.\n", name);
		return false;
	}

	BdFontEntry entry;
	entry.Name = name;
	entry.SizePixels = (float)std::clamp((double)sizePixels, 4.0, 96.0);
	entry.Bitmap = bitmap;
	sFonts.Push(std::move(entry));
	MarkAtlasDirty();
	return true;
}

bool RemoveFont(const char* name)
{
	const int index = FindFontIndex(name);
	if (index < 0)
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: remove_font: unknown font '%s'.\n", name == nullptr ? "" : name);
		return false;
	}
	if (sFonts[index].Permanent)
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: the built-in 'Default' font cannot be removed.\n");
		return false;
	}
	if (AtlasCapReached())
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: remove_font('%s') refused: the font atlas upload cap is reached.\n", name);
		return false;
	}
	sFonts.Delete(index);
	if (sDefaultFontIndex == index || sDefaultFontIndex >= (int)sFonts.Size())
	{
		// Fall back to the permanent "Default" entry.
		sDefaultFontIndex = -1;
		for (int i = 0; i < (int)sFonts.Size(); i++)
			if (sFonts[i].Permanent) { sDefaultFontIndex = i; break; }
	}
	else if (sDefaultFontIndex > index) sDefaultFontIndex--;
	MarkAtlasDirty();
	return true;
}

void ClearFonts()
{
	if (AtlasCapReached())
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: clear_fonts refused: the font atlas upload cap is reached.\n");
		return;
	}
	sDefaultFontIndex = -1;
	for (int i = (int)sFonts.Size() - 1; i >= 0; i--)
	{
		if (sFonts[i].Permanent)	// "Default" is permanent
		{
			sDefaultFontIndex = i;
			continue;
		}
		sFonts.Delete(i);
	}
	MarkAtlasDirty();
}

void ListFonts(TArray<FontInfo>& out)
{
	for (const BdFontEntry& entry : sFonts)
	{
		FontInfo info;
		info.Name = entry.Name;
		info.SizePixels = entry.SizePixels;
		info.Bitmap = entry.Bitmap;
		info.BuiltIn = entry.Permanent;
		out.Push(std::move(info));
	}
}

int FontCount()
{
	return (int)sFonts.Size();
}

ImFont* FindFont(const char* name)
{
	const int index = FindFontIndex(name);
	return index >= 0 ? sFonts[index].Font : nullptr;
}

bool SetDefaultFont(const char* name)
{
	const int index = FindFontIndex(name);
	if (index < 0)
	{
		Printf(TEXTCOLOR_YELLOW "bd.imgui: set_default_font: unknown font '%s'.\n", name == nullptr ? "" : name);
		return false;
	}
	sDefaultFontIndex = index;
	return true;
}

FString GetDefaultFontName()
{
	if (sDefaultFontIndex >= 0 && sDefaultFontIndex < (int)sFonts.Size())
		return sFonts[sDefaultFontIndex].Name;
	return FString();
}

bool SetUiScale(float factor)
{
	factor = (float)std::clamp((double)factor, 0.5, 4.0);
	if (factor == sUiScale) return true;
	const float previous = sUiScale;
	sUiScale = factor;
	if (sContext != nullptr)
	{
		ImGui::SetCurrentContext(sContext);
		ImGuiStyle& style = ImGui::GetStyle();
		style.FontScaleMain = factor;
		style.ScaleAllSizes(factor / previous);
	}
	return true;
}

float GetUiScale()
{
	return sUiScale;
}

//===========================================================================
//
// Color conversion. ImU32 is 0xAABBGGRR (R in the lowest byte); PalEntry
// stores 0xAARRGGBB in .d on little-endian targets.
//
//===========================================================================

static inline PalEntry ImGuiColorToPalEntry(ImU32 col)
{
	return PalEntry((uint8_t)(col >> 24), (uint8_t)(col & 0xff), (uint8_t)((col >> 8) & 0xff), (uint8_t)((col >> 16) & 0xff));
}

//===========================================================================
//
// ImDrawData -> F2DDrawer translation. Modeled on F2DDrawer::AddPoly()
// (src/common/2d/v_2ddrawer.cpp) but with per-vertex colors.
//
// Batching: consecutive ImDrawCmds that share texture + scissor rect are
// merged into a single F2DDrawer RenderCommand. No explicit merging pass is
// needed here: F2DDrawer::AddCommand() already merges a new command into
// the previous one whenever RenderCommand::isCompatible() matches, and
// isCompatible() compares exactly the state ImGui varies between commands
// (mTexture, mScissor, mRenderStyle, mFlags, transform). The merge is exact
// for our DrawTypeTriangles commands because every command's indices are
// appended contiguously to twod->mIndices and the hardware 2D path draws
// via DrawIndexed(mIndexIndex, mIndexCount) only (see hw_draw2d.cpp), so a
// merged command references a contiguous index range; mVertCount is only
// read by the DT_Lines/DT_Points paths, which ImGui never emits. A native
// ImGui backend would gain nothing further: merging cannot deduplicate the
// index buffer because each ImDrawCmd owns a distinct element range.
// Measured effect of the existing merge: a typical overlay frame (menu bar
// + panel window + demo window) emits ~60-90 ImDrawCmds that collapse to
// roughly a quarter as many RenderCommands, since text runs inside one
// window share the font texture and clip rect.
//
//===========================================================================

static void RenderDrawData(ImDrawData* drawData)
{
	if (drawData == nullptr || drawData->TotalVtxCount <= 0) return;

	const ImVec2 displayPos = drawData->DisplayPos;

	for (int n = 0; n < drawData->CmdListsCount; n++)
	{
		const ImDrawList* drawList = drawData->CmdLists[n];
		if (drawList->VtxBuffer.Size <= 0) continue;

		// Upload the whole vertex buffer of the list once; individual
		// commands then index into it.
		const int vertexBase = (int)twod->mVertices.Reserve(drawList->VtxBuffer.Size);
		F2DDrawer::TwoDVertex* out = &twod->mVertices[vertexBase];
		for (int i = 0; i < drawList->VtxBuffer.Size; i++)
		{
			const ImDrawVert& in = drawList->VtxBuffer[i];
			out[i].Set((double)in.pos.x - displayPos.x, (double)in.pos.y - displayPos.y, 0.0,
				(double)in.uv.x, (double)in.uv.y, ImGuiColorToPalEntry(in.col));
		}

		for (int c = 0; c < drawList->CmdBuffer.Size; c++)
		{
			const ImDrawCmd& cmd = drawList->CmdBuffer[c];
			if (cmd.UserCallback != nullptr) continue;	// not supported; ImGui core emits none
			if (cmd.ElemCount == 0) continue;

			FGameTexture* texture = (FGameTexture*)(intptr_t)cmd.GetTexID();
			if (texture == nullptr) texture = sFontTexture;
			if (texture == nullptr || !texture->isValid()) continue;

			F2DDrawer::RenderCommand dg;
			dg.mType = F2DDrawer::DrawTypeTriangles;

			// ClipRect is (x1, y1, x2, y2) in display coordinates; clamp to
			// the 2D surface and honor the drawer's offset like AddPoly does.
			const int clipL = (int)floorf(cmd.ClipRect.x - displayPos.x);
			const int clipT = (int)floorf(cmd.ClipRect.y - displayPos.y);
			const int clipR = (int)ceilf(cmd.ClipRect.z - displayPos.x);
			const int clipB = (int)ceilf(cmd.ClipRect.w - displayPos.y);
			dg.mScissor[0] = clipL < 0 ? 0 : clipL;
			dg.mScissor[1] = clipT < 0 ? 0 : clipT;
			dg.mScissor[2] = clipR > twod->GetWidth() ? twod->GetWidth() : clipR;
			dg.mScissor[3] = clipB > twod->GetHeight() ? twod->GetHeight() : clipB;
			if (dg.mScissor[2] <= dg.mScissor[0] || dg.mScissor[3] <= dg.mScissor[1]) continue;
			dg.mFlags |= F2DDrawer::DTF_Scissor;

			dg.mTexture = texture;
			dg.mVertIndex = vertexBase + (int)cmd.VtxOffset;
			dg.mVertCount = drawList->VtxBuffer.Size - (int)cmd.VtxOffset;
			dg.mRenderStyle = LegacyRenderStyles[STYLE_Translucent];

			dg.mIndexIndex = (int)twod->mIndices.Size();
			dg.mIndexCount = (int)cmd.ElemCount;
			twod->mIndices.Reserve(cmd.ElemCount);
			for (unsigned int e = 0; e < cmd.ElemCount; e++)
			{
				twod->mIndices[dg.mIndexIndex + e] = dg.mVertIndex + drawList->IdxBuffer[cmd.IdxOffset + e];
			}

			dg.useTransform = true;
			dg.transform = twod->transform;
			dg.transform.Cells[0][2] += twod->offset.X;
			dg.transform.Cells[1][2] += twod->offset.Y;
			twod->AddCommand(&dg);
		}
	}
}

//===========================================================================
//
// Input translation. The engine's platform layer posts EV_GUI_* events
// while GUI capture is active; GK_* codes and toupper'd ASCII arrive in
// data1, modifiers in data3.
//
//===========================================================================

static ImGuiKey MapGuiKey(int code)
{
	switch (code)
	{
	case GK_TAB:		return ImGuiKey_Tab;
	case GK_LEFT:		return ImGuiKey_LeftArrow;
	case GK_RIGHT:		return ImGuiKey_RightArrow;
	case GK_UP:			return ImGuiKey_UpArrow;
	case GK_DOWN:		return ImGuiKey_DownArrow;
	case GK_PGUP:		return ImGuiKey_PageUp;
	case GK_PGDN:		return ImGuiKey_PageDown;
	case GK_HOME:		return ImGuiKey_Home;
	case GK_END:		return ImGuiKey_End;
	case GK_DEL:		return ImGuiKey_Delete;
	case GK_BACKSPACE:	return ImGuiKey_Backspace;
	case GK_RETURN:		return ImGuiKey_Enter;
	case GK_ESCAPE:		return ImGuiKey_Escape;
	case ' ':			return ImGuiKey_Space;
	case '\'':			return ImGuiKey_Apostrophe;
	case ',':			return ImGuiKey_Comma;
	case '-':			return ImGuiKey_Minus;
	case '.':			return ImGuiKey_Period;
	case '/':			return ImGuiKey_Slash;
	case ';':			return ImGuiKey_Semicolon;
	case '=':			return ImGuiKey_Equal;
	case '[':			return ImGuiKey_LeftBracket;
	case '\\':			return ImGuiKey_Backslash;
	case ']':			return ImGuiKey_RightBracket;
	case '`':			return ImGuiKey_GraveAccent;
	default:			break;
	}
	if (code >= GK_F1 && code <= GK_F12) return (ImGuiKey)(ImGuiKey_F1 + (code - GK_F1));
	if (code >= 'A' && code <= 'Z') return (ImGuiKey)(ImGuiKey_A + (code - 'A'));
	if (code >= '0' && code <= '9') return (ImGuiKey)(ImGuiKey_0 + (code - '0'));
	return ImGuiKey_None;
}

static void ApplyModifiers(ImGuiIO& io, int data3)
{
	io.AddKeyEvent(ImGuiMod_Ctrl, (data3 & GKM_CTRL) != 0);
	io.AddKeyEvent(ImGuiMod_Shift, (data3 & GKM_SHIFT) != 0);
	io.AddKeyEvent(ImGuiMod_Alt, (data3 & GKM_ALT) != 0);
}

bool Available()
{
	return true;
}

bool MasterVisible()
{
	return py_imgui;
}

void SetMasterVisible(bool on)
{
	py_imgui = on;
}

bool NavEnabled()
{
	return sNavEnabled;
}

void SetNavEnabled(bool on)
{
	sNavEnabled = on;
	if (sContext == nullptr) return;	// applied from sNavEnabled in Init()
	ImGui::SetCurrentContext(sContext);
	ImGuiIO& io = ImGui::GetIO();
	if (on) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
}

bool DemoVisible()
{
	return sDemoVisible;
}

void SetDemoVisible(bool on)
{
	sDemoVisible = on;
}

ImGuiContext* Context()
{
	return sContext;
}

bool FrameActive()
{
	return sFrameActive;
}

void Init()
{
	if (sContext != nullptr) return;
	sContext = ImGui::CreateContext();
	ImGui::SetCurrentContext(sContext);
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;	// no imgui.ini persistence inside the engine
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	if (!sNavEnabled) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
#ifdef IMGUI_HAS_DOCK
	// Docking branch: allow windows to dock into dock spaces submitted from
	// Python (bd.imgui.dock_space*). Viewports are deliberately NOT enabled:
	// multi-viewport needs platform windows, and the overlay renders through
	// the single F2DDrawer canvas, so detached-platform windows cannot work.
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif
	if (sUiScale != 1.0f)
	{
		ImGuiStyle& style = ImGui::GetStyle();
		style.FontScaleMain = sUiScale;
		style.ScaleAllSizes(sUiScale);
	}
	CreateAtlasTexture();
	// Fonts registered by scripts before the first frame are folded in now.
	if (sPreInitAtlasDirty)
	{
		sPreInitAtlasDirty = false;
		RebuildAtlas();
	}
}

void Shutdown()
{
	if (sContext != nullptr)
	{
		ImGui::SetCurrentContext(sContext);
		ImGui::DestroyContext(sContext);
		sContext = nullptr;
	}
	// The atlas textures are owned by TexMan and die with it.
	sFontTexture = nullptr;
	sDemoVisible = false;
	sFrameActive = false;
	sPendingAtlasRebuild = false;
	sFonts.Clear();
	sDefaultFontIndex = -1;
}

void Frame(double deltaSeconds)
{
	if (sContext == nullptr) Init();
	if (sContext == nullptr) return;

	ImGui::SetCurrentContext(sContext);
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)twod->GetWidth(), (float)twod->GetHeight());
	io.DeltaTime = (float)deltaSeconds;

	// NewFrame/Render are always paired to keep the context state sane, even
	// when the overlay is hidden. Window state persists across hidden frames.
	ImGui::NewFrame();
	sFrameActive = true;

	if (MasterVisible())
	{
		// Start every script frame on the registered default font so a
		// leftover PushFont from a previous handler cannot leak across.
		// Push/Pop stay symmetric even if the runtime reports an error.
		ImFont* defaultFont = (sDefaultFontIndex >= 0 && sDefaultFontIndex < (int)sFonts.Size())
			? sFonts[sDefaultFontIndex].Font : nullptr;
		if (defaultFont != nullptr)
			ImGui::PushFont(defaultFont, 0.0f);
		PythonRuntime::OnImguiFrame();
		if (sDemoVisible)
			ImGui::ShowDemoWindow(&sDemoVisible);
		if (defaultFont != nullptr)
			ImGui::PopFont();
	}

	ImGui::Render();
	sFrameActive = false;

	// A font mutation that arrived from inside imgui_frame is applied now,
	// after Render() unlocked the atlas; draw data already produced keeps
	// referencing the previous atlas texture, which stays alive in TexMan.
	if (sPendingAtlasRebuild)
	{
		sPendingAtlasRebuild = false;
		RebuildAtlas();
	}

	if (MasterVisible() && twod->HasBegun2D())
		RenderDrawData(ImGui::GetDrawData());
}

bool HandleEvent(const event_t* ev)
{
	if (ev->type != EV_GUI_Event) return false;
	if (!MasterVisible() || sContext == nullptr) return false;

	ImGui::SetCurrentContext(sContext);
	ImGuiIO& io = ImGui::GetIO();

	switch (ev->subtype)
	{
	case EV_GUI_MouseMove:
		io.AddMousePosEvent((float)ev->data1, (float)ev->data2);
		return io.WantCaptureMouse;

	case EV_GUI_LButtonDown:
	case EV_GUI_LButtonUp:
		io.AddMouseButtonEvent(0, ev->subtype == EV_GUI_LButtonDown);
		return io.WantCaptureMouse;
	case EV_GUI_RButtonDown:
	case EV_GUI_RButtonUp:
		io.AddMouseButtonEvent(1, ev->subtype == EV_GUI_RButtonDown);
		return io.WantCaptureMouse;
	case EV_GUI_MButtonDown:
	case EV_GUI_MButtonUp:
		io.AddMouseButtonEvent(2, ev->subtype == EV_GUI_MButtonDown);
		return io.WantCaptureMouse;
	case EV_GUI_BackButtonDown:
	case EV_GUI_BackButtonUp:
		io.AddMouseButtonEvent(3, ev->subtype == EV_GUI_BackButtonDown);
		return io.WantCaptureMouse;
	case EV_GUI_FwdButtonDown:
	case EV_GUI_FwdButtonUp:
		io.AddMouseButtonEvent(4, ev->subtype == EV_GUI_FwdButtonDown);
		return io.WantCaptureMouse;

	case EV_GUI_WheelUp:
		io.AddMouseWheelEvent(0.0f, 1.0f);
		return io.WantCaptureMouse;
	case EV_GUI_WheelDown:
		io.AddMouseWheelEvent(0.0f, -1.0f);
		return io.WantCaptureMouse;
	case EV_GUI_WheelRight:
		io.AddMouseWheelEvent(1.0f, 0.0f);
		return io.WantCaptureMouse;
	case EV_GUI_WheelLeft:
		io.AddMouseWheelEvent(-1.0f, 0.0f);
		return io.WantCaptureMouse;

	case EV_GUI_KeyDown:
	case EV_GUI_KeyRepeat:
	case EV_GUI_KeyUp:
		ApplyModifiers(io, ev->data3);
		{
			const ImGuiKey key = MapGuiKey(ev->data1);
			if (key != ImGuiKey_None)
				io.AddKeyEvent(key, ev->subtype != EV_GUI_KeyUp);
		}
		return io.WantCaptureKeyboard;

	case EV_GUI_Char:
		// SDL_TEXTINPUT already delivered a decoded codepoint (BMP only).
		if (ev->data1 > 0)
			io.AddInputCharacter((unsigned int)ev->data1);
		return io.WantTextInput;

	default:
		return false;
	}
}

bool WantsGuiCapture()
{
	if (!MasterVisible() || sContext == nullptr) return false;
	ImGui::SetCurrentContext(sContext);
	ImGuiIO& io = ImGui::GetIO();
	return io.WantCaptureMouse || io.WantCaptureKeyboard || sDemoVisible;
}

} // namespace BdImGui

CCMD(py_imgui_demo)
{
	if (!BdImGui::Available())
	{
		Printf("This executable was built without Dear ImGui support.\n");
		return;
	}
	if (!BdImGui::MasterVisible())
	{
		Printf("The py_imgui master switch is off; enable it first.\n");
		return;
	}
	BdImGui::SetDemoVisible(!BdImGui::DemoVisible());
	Printf("Dear ImGui demo window %s.\n", BdImGui::DemoVisible() ? "opened" : "closed");
}

#else // !BIASEDDOOM_IMGUI

namespace BdImGui
{
	bool Available() { return false; }
	void Init() {}
	void Shutdown() {}
	void Frame(double) {}
	bool HandleEvent(const event_t*) { return false; }
	bool WantsGuiCapture() { return false; }
	bool MasterVisible() { return false; }
	void SetMasterVisible(bool) {}
	bool NavEnabled() { return false; }
	void SetNavEnabled(bool) {}
	bool DemoVisible() { return false; }
	void SetDemoVisible(bool) {}
	ImGuiContext* Context() { return nullptr; }
	bool FrameActive() { return false; }
	bool AddFontTTF(const char*, const void*, size_t, float) { return false; }
	bool AddFontDefault(const char*, float, bool) { return false; }
	bool RemoveFont(const char*) { return false; }
	void ClearFonts() {}
	void ListFonts(TArray<FontInfo>&) {}
	int FontCount() { return 0; }
	ImFont* FindFont(const char*) { return nullptr; }
	bool SetDefaultFont(const char*) { return false; }
	FString GetDefaultFontName() { return FString(); }
	bool SetUiScale(float) { return false; }
	float GetUiScale() { return 1.0f; }
}

CCMD(py_imgui_demo)
{
	Printf("This executable was built without Dear ImGui support.\n");
}

#endif // BIASEDDOOM_IMGUI
