/*
** null_video.cpp
** Headless ("null") video driver implementation. See null_video.h.
**
**---------------------------------------------------------------------------
** Copyright 2025 BiasedDoom Maintainers and Contributors
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

#include <stdlib.h>

#include "nullvideo/null_video.h"
#include "c_cvars.h"
#include "m_argv.h"
#include "printf.h"

EXTERN_CVAR(Int, vid_defwidth)
EXTERN_CVAR(Int, vid_defheight)

// -1 = not determined yet, 0 = normal video, 1 = headless.
static int headlessState = -1;

//==========================================================================
//
// I_IsHeadless
//
// True when the engine was asked to run without a display, either via the
// -headless command line parameter or the BIASEDDOOM_HEADLESS environment
// variable (any value other than "", 0, "false" or "no" enables it).
//
//==========================================================================

bool I_IsHeadless()
{
	if (headlessState < 0)
	{
		bool headless = false;
		if (const char *env = getenv("BIASEDDOOM_HEADLESS"))
		{
			headless = env[0] != '\0' && stricmp(env, "0") != 0 &&
				stricmp(env, "false") != 0 && stricmp(env, "no") != 0;
		}
		if (!headless && Args != nullptr)
		{
			headless = !!Args->CheckParm("-headless");
		}
		headlessState = headless ? 1 : 0;
	}
	return headlessState == 1;
}

//==========================================================================
//
// NullFrameBuffer
//
//==========================================================================

NullFrameBuffer::NullFrameBuffer(int width, int height)
	: DFrameBuffer(0, 0)
{
	// Keep the reported size exact, like DDummyFrameBuffer does.
	SetVirtualSize(width, height);
}

//==========================================================================
//
// NullVideo
//
//==========================================================================

DFrameBuffer *NullVideo::CreateFrameBuffer()
{
	int width = vid_defwidth;
	int height = vid_defheight;

	// Honor a remembered window size if one is stored in the config.
	if (win_w > 0 && win_h > 0)
	{
		width = win_w;
		height = win_h;
	}
	if (width <= 0) width = 640;
	if (height <= 0) height = 480;

	return new NullFrameBuffer(width, height);
}

void NullVideo::DumpAdapters()
{
	Printf("Headless mode: no display adapters.\n");
}
