#pragma once

//===========================================================================
//
// bd_imgui.h
//
// Dear ImGui engine overlay layer. Renders through the backend-agnostic
// F2DDrawer 2D path, so it works identically on OpenGL, Vulkan, GLES and
// softpoly. All entry points degrade to no-ops when the build has no ImGui
// support (BIASEDDOOM_IMGUI undefined).
//
//===========================================================================

#include "tarray.h"
#include "zstring.h"

struct event_t;
struct ImGuiContext;
struct ImFont;

namespace BdImGui
{
	// True when the overlay was compiled in (BIASEDDOOM_IMGUI).
	bool Available();

	// Lazy initialization, performed on the first Frame() call: creates the
	// ImGui context and uploads the font atlas as a runtime game texture.
	void Init();
	void Shutdown();

	// Per-frame cycle, called from DrawOverlays() while the 2D drawer is open.
	// deltaSeconds is wall-clock frame time, pre-clamped by the caller.
	void Frame(double deltaSeconds);

	// Responder hook between the console and the menu in D_ProcessEvents.
	// Returns true when ImGui consumed the event.
	bool HandleEvent(const event_t* ev);

	// OR'ed into the platform layer's GUI capture decision so that the engine
	// starts posting EV_GUI_* events while ImGui wants input.
	bool WantsGuiCapture();

	// True while an ImGui text-editing widget owns text input. Platform input
	// paths use this to avoid diverting printable shortcut keys from the field.
	bool WantsTextInput();

	// Master visibility switch, backed by the py_imgui cvar.
	bool MasterVisible();
	void SetMasterVisible(bool on);

	// Keyboard navigation flag (ImGuiConfigFlags_NavEnableKeyboard), enabled by
	// default. Config state, not frame state: callable any time; a toggle made
	// before the first Frame() is applied when the context is created.
	bool NavEnabled();
	void SetNavEnabled(bool on);

	// Toggled by the py_imgui_demo console command.
	bool DemoVisible();
	void SetDemoVisible(bool on);

	// Raw context for native modules (e.g. the bd.imgui Python submodule)
	// that submit ImGui calls directly. nullptr until the first Frame() call.
	ImGuiContext* Context();

	// True between ImGui::NewFrame() and ImGui::Render(), i.e. exactly while
	// imgui_frame handlers run and ImGui widget calls are legal.
	bool FrameActive();

	// Runtime font registry. Fonts are registered by name; every mutation
	// marks the atlas dirty and rebuilds it (outside an active frame:
	// immediately, inside a frame: after Render()), uploading a fresh atlas
	// texture under a unique name. Font source bytes are copied into
	// never-freed layer-owned blocks so atlas rebuilds cannot dangle.
	// The engine-registered built-in font is named "Default" and cannot be
	// removed. All of these are config-state calls, not frame calls: they
	// are callable from any event, and also before the first Frame() (the
	// pending rebuild is applied when the context is created).

	// Description of one registered font, filled by ListFonts().
	struct FontInfo
	{
		FString Name;
		float SizePixels;
		bool Bitmap;	// built-in pixel font (ProggyClean) vs vector/TTF
		bool BuiltIn;	// engine-managed font ("Default")
	};

	// Copies the TTF/OTF bytes (layer-owned afterwards) and registers the
	// font. size_pixels is clamped to 4..96. Empty or duplicate names and
	// empty data are rejected with a bd-side warning. Returns false on
	// rejection.
	bool AddFontTTF(const char* name, const void* data, size_t dataSize, float sizePixels);
	// Registers a font from ImGui's embedded default data. bitmap selects
	// AddFontDefaultBitmap() vs AddFontDefaultVector(). Same name/size rules.
	bool AddFontDefault(const char* name, float sizePixels, bool bitmap);
	// Removes a script-registered font. The built-in "Default" font is
	// permanent and rejects removal. Returns false on unknown name.
	bool RemoveFont(const char* name);
	// Removes all script-registered fonts, keeping "Default".
	void ClearFonts();
	// Appends one FontInfo per registered font to out.
	void ListFonts(TArray<FontInfo>& out);
	// Number of registered fonts (stable indices for ListFonts).
	int FontCount();
	// Resolves a registered name to the current ImFont (nullptr when the
	// atlas has not been built yet or the name is unknown).
	ImFont* FindFont(const char* name);
	// Selects the font Frame() pushes around imgui_frame handlers. Unknown
	// names are rejected with a bd-side warning. Returns false on rejection.
	bool SetDefaultFont(const char* name);
	FString GetDefaultFontName();

	// Global UI scale (style.FontScaleMain plus a compensating
	// ScaleAllSizes() so spacing tracks the font size). The factor is
	// clamped to [0.5, 4.0]; every call rescales spacing by the ratio
	// between the new and the previous factor, so the calls compose.
	bool SetUiScale(float factor);
	float GetUiScale();
}
