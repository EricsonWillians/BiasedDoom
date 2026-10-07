/*
** hardware.cpp
** Somewhat OS-independant interface to the screen, mouse, keyboard, and stick
**
**---------------------------------------------------------------------------
** Copyright 1998-2006 Randy Heit
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include <SDL2/SDL.h>
#include <signal.h>
#include <stdlib.h>

#include "c_console.h"
#include "c_dispatch.h"
#include "hardware.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "printf.h"
#include "v_text.h"
#include "nullvideo/null_video.h"

IVideo *Video;

void I_RestartRenderer();

void I_ShutdownGraphics ()
{
	if (screen)
	{
		DFrameBuffer *s = screen;
		screen = NULL;
		delete s;
	}
	if (Video)
		delete Video, Video = NULL;

	SDL_QuitSubSystem (SDL_INIT_VIDEO);
}

void I_InitGraphics ()
{
	const bool headless = I_IsHeadless();

	if (headless)
	{
		// SDL's "dummy" video driver keeps the event pump alive without
		// needing a display (X11/Wayland) or creating any window.
		setenv("SDL_VIDEODRIVER", "dummy", 1);
	}

#ifdef __APPLE__
	SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
#endif // __APPLE__
	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

	if (SDL_InitSubSystem (SDL_INIT_VIDEO) < 0)
	{
		I_FatalError ("Could not initialize SDL video:\n%s\n", SDL_GetError());
		return;
	}

	if (headless)
	{
		Printf("Headless mode: null video driver (no display, no rendering)\n");
		Video = new NullVideo();
	}
	else
	{
		const char *videoDriver = SDL_GetCurrentVideoDriver();
		// SDL may fall back to its offscreen backend when neither X11 nor
		// Wayland is usable. It can create a nominal window, but it has no
		// composed presentation surface and several OpenGL drivers stall while
		// creating the context. That is neither a usable interactive backend nor
		// a valid source for the video recorder; require an explicit -headless
		// run instead of appearing to start and then hanging.
		if (videoDriver != nullptr && stricmp(videoDriver, "offscreen") == 0)
		{
			I_FatalError("SDL selected its offscreen video driver. Start with a working display, or use -headless for a non-rendering run.\n");
			return;
		}

		Printf("Using video driver %s\n", videoDriver != nullptr ? videoDriver : "unknown");

		extern IVideo *gl_CreateVideo();
		Video = gl_CreateVideo();
	}

	if (Video == NULL)
		I_FatalError ("Failed to initialize display");
}

bool I_SupportsLiveBackendSwitch ()
{
	return !I_IsHeadless();
}

void I_RestartGraphics ()
{
	if (Video)
		delete Video, Video = NULL;

	// The Vulkan backend never destroys its window, so make sure it is gone
	// before the new IVideo creates one with different flags.
	extern void SDL_DestroyVideoWindow();
	SDL_DestroyVideoWindow();

	if (I_IsHeadless())
	{
		Video = new NullVideo();
	}
	else
	{
		extern IVideo *gl_CreateVideo();
		Video = gl_CreateVideo();
	}

	if (Video == NULL)
		I_FatalError ("Failed to initialize display");
}

void I_ShowGraphicsWindow ()
{
	extern void SDL_ShowVideoWindow();
	SDL_ShowVideoWindow();
}
