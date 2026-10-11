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
// DESCRIPTION:
//	 Duh.
// 
//-----------------------------------------------------------------------------


#ifndef __G_GAME__
#define __G_GAME__

struct event_t;

#include <functional>
#include <string>

#include "dobjgc.h"
#include "name.h"
#include "gamestate.h"


// wipegamestate can be set to -1
//	to force a wipe on the next draw
extern gamestate_t wipegamestate;



class AActor;
struct FLevelLocals;
namespace BdOnline { struct FOnlineSnapshotBundle; }

//
// GAME
//
enum
{
	PPS_FORCERANDOM			= 1,
	PPS_NOBLOCKINGCHECK		= 2,
};

void G_DeferedPlayDemo (const char* demo);

// Can be called by the startup code or M_Responder,
// calls P_SetupLevel or W_EnterWorld.
void G_LoadGame (const char* name, bool hidecon=false);

void G_DoLoadGame (void);

// Called by M_Responder.
void G_SaveGame (const char *filename, const char *description, bool quick = false);
// Called by messagebox
void G_DoQuickSave ();

// Only called by startup code.
void G_RecordDemo (const char* name);

void G_BeginRecording (const char *startmap);
void G_StopDemoRecording();
// A menu-driven recorddemo request can be cancelled before the ticker begins
// it. Once it reaches ga_recordgame, its queued mode snapshot is no longer
// eligible for restoration.
void G_CommitQueuedDemoRecording();

void G_PlayDemo (char* name);
void G_TimeDemo (const char* name);
bool G_CheckDemoStatus (void);

void G_Ticker (void);
bool G_Responder (event_t*	ev);

enum
{
	FSTATE_EndingGame = 0,
	FSTATE_ChangingLevel = 1,
	FSTATE_InLevel = 2,
	FSTATE_InLevelNoWipe = 3
};

void G_ScreenShot (const char* filename);
void G_StartSlideshow(FLevelLocals *Level, FName whichone, int state);

class FSerializer;
bool G_CheckSaveGameWads (FSerializer &arc, bool printwarn);

enum EFinishLevelType
{
	FINISH_SameHub,
	FINISH_NextHub,
	FINISH_NoHub
};

void G_PlayerFinishLevel (int player, EFinishLevelType mode, int flags);

void G_DoPlayerPop(int playernum);

// Captures the serializer state a future online join-in-progress bridge needs
// before it begins an authenticated transfer. This deliberately has no file
// output and does not mutate hub/savegame snapshot ownership.
bool G_BuildOnlineSnapshotBundle(BdOnline::FOnlineSnapshotBundle& bundle, std::string& error);
// Restores a fully authenticated online snapshot into this process using the
// ordinary savegame lifecycle. Callers must keep the joining client offline
// until replay catches it up to the live tic boundary.
bool G_RestoreOnlineSnapshotBundle(const BdOnline::FOnlineSnapshotBundle& bundle, std::string& error);

// Adds pitch to consoleplayer's viewpitch and clamps it
void G_AddViewPitch (int look, bool mouse = false);

// Adds to consoleplayer's viewangle if allowed
void G_AddViewAngle (int yaw, bool mouse = false);

class FBaseCVar;
FBaseCVar* G_GetUserCVar(int playernum, const char* cvarname);

class DIntermissionController;
struct level_info_t;
void RunIntermission(level_info_t* oldlevel, level_info_t* newlevel, DIntermissionController* intermissionScreen, DObject* statusScreen, bool ending, std::function<void(bool)> completionf);

extern const AActor *SendItemUse, *SendItemDrop;
extern int SendItemDropAmount;


#endif
