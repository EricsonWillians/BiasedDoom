/*
** null_video.h
** Headless ("null") video driver: an IVideo/DFrameBuffer pair that never
** touches a display, GL context or Vulkan device. Used by -headless /
** BIASEDDOOM_HEADLESS=1 so the engine can boot and run the game loop in
** CI environments without X11/xvfb.
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

#pragma once

#include "i_video.h"
#include "v_video.h"

class NullFrameBuffer : public DFrameBuffer
{
public:
	NullFrameBuffer(int width, int height);

	void InitializeState() override {}
	bool IsFullscreen() override { return false; }
	void ToggleFullscreen(bool) override {}
	int GetClientWidth() override { return GetWidth(); }
	int GetClientHeight() override { return GetHeight(); }

	// There is no presentation target; the base Update() would try to resize
	// hardware vertex buffers that do not exist here.
	void Update() override {}
};

class NullVideo : public IVideo
{
public:
	DFrameBuffer *CreateFrameBuffer() override;
	void DumpAdapters() override;
};
