//-----------------------------------------------------------------------------
//
// Copyright 1993-1996 id Software
// Copyright 1999-2016 Randy Heit
// Copyright 2002-2016 Christoph Oelckers
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//-----------------------------------------------------------------------------
//


#ifndef __M_MISC__
#define __M_MISC__

#include "basics.h"
#include "zstring.h"

class FConfigFile;
class FGameConfigFile;
class FIWadManager;

extern FGameConfigFile *GameConfig;

void M_FindResponseFile (void);

// [RH] M_ScreenShot now accepts a filename parameter.
//		Pass a NULL to get the original behavior.
void M_ScreenShot (const char *filename);
void M_RequestScreenShot(const char *filename);
void M_ProcessPendingScreenShot();

// Shared destination helpers for the recording UI. A blank configured folder
// resolves to the per-user screenshots/captures directory.
FString M_GetCaptureExportPath();
FString M_MakeCaptureFileName(const char *requestedName, const char *extension, const char *defaultStem);

// Lossless final-frame video capture. Capture is completed before graphics
// shutdown so a stopped or normally exited recording remains playable.
bool M_StartVideoRecording(const char *requestedName = nullptr);
void M_StopVideoRecording();
bool M_IsVideoRecording();

void M_LoadDefaults ();

bool M_SaveDefaults (const char *filename);
void M_SaveCustomKeys (FConfigFile *config, char *section, char *subsection, size_t sublen);


#include "i_specialpaths.h"
#endif
