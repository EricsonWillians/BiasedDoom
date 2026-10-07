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
// Advance only an already-stopping recorder without servicing a pending
// ordinary screenshot or starting a new GPU readback. The display admission
// gate uses this while it deliberately skips presentation after a
// capture-backpressure failure, so recorder finalization can retire or
// abandon its last GPU readback.
void M_PollStoppingVideoRecording();
void M_ProcessPendingScreenShot();

// Shared destination helpers for the recording UI. A blank configured folder
// resolves to the per-user screenshots/captures directory.
FString M_GetCaptureExportPath();
FString M_MakeCaptureFileName(const char *requestedName, const char *extension, const char *defaultStem);

// A presentation snapshot for the video recorder. Active takes update their
// elapsed time from a wall clock; stopping and completed takes retain the
// elapsed time of the captured material. Completed and failed snapshots are
// intentionally short-lived so the caller can provide immediate feedback
// without leaving a permanent HUD message behind.
enum EVideoRecordingState
{
	VRS_None,
	VRS_Recording,
	VRS_Stopping,
	VRS_Finalized,
	VRS_Failed,
};

struct FVideoRecordingStatus
{
	EVideoRecordingState State = VRS_None;
	uint64_t ElapsedMilliseconds = 0;
	FString Detail;
};

// Lossless final-frame video capture. Capture is completed before graphics
// shutdown so a stopped or normally exited recording remains playable.
bool M_StartVideoRecording(const char *requestedName = nullptr);
void M_StopVideoRecording();
// Stop a take because a backend could no longer safely make forward progress.
// Completed frames are still finalized, but the final status remains failed so
// the player knows why the recorder detached from the render loop.
void M_FailVideoRecording(const char *reason);
// Used during engine teardown after an interactive stop request has already
// detached capture from the render loop.
void M_FinishVideoRecording();
bool M_IsVideoRecording();
bool M_GetVideoRecordingStatus(FVideoRecordingStatus &status);

void M_LoadDefaults ();

bool M_SaveDefaults (const char *filename);
void M_SaveCustomKeys (FConfigFile *config, char *section, char *subsection, size_t sublen);


#include "i_specialpaths.h"
#endif
