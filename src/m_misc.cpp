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
//		Default Config File.
//		Screenshots.
//
//-----------------------------------------------------------------------------


#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "r_defs.h"

#include "version.h"

#if defined(_WIN32)
#include <io.h>
#else
#endif


#include "m_swap.h"
#include "m_argv.h"

#include "filesystem.h"

#include "c_cvars.h"
#include "c_dispatch.h"
#include "c_bind.h"

#include "i_video.h"
#include "v_video.h"
#include "i_system.h"
#include "g_input.h"
#include "fs_findfile.h"

// Data.
#include "m_misc.h"
#include "m_png.h"
#include "video_capture_audio.h"

#include "cmdlib.h"

#include "g_game.h"
#include "gi.h"

#include "gameconfigfile.h"
#include "gstrings.h"
#include "vm.h"

FGameConfigFile *GameConfig;

CVAR(Bool, screenshot_quiet, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);
CVAR(String, screenshot_type, "png", CVAR_ARCHIVE|CVAR_GLOBALCONFIG);
CVAR(String, screenshot_dir, "", CVAR_ARCHIVE|CVAR_GLOBALCONFIG);

// Recording deliberately shares one destination with demos and video captures.
// A blank value is resolved to screenshots/captures, which keeps old screenshot
// preferences untouched while still giving captures a predictable home.
CVAR(String, capture_export_dir, "", CVAR_ARCHIVE|CVAR_GLOBALCONFIG);
CVAR(String, vid_record_name, "", CVAR_ARCHIVE|CVAR_GLOBALCONFIG);
CUSTOM_CVAR(Int, vid_record_fps, 60, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	if (self < 1) self = 1;
	else if (self > 240) self = 240;
}
CVAR(Int, vid_record_format, 0, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);
// Video frames are normally a source for a later edit/export, where spending
// CPU on a high-deflate PNG is rarely worthwhile. Keep screenshots at their
// quality-oriented setting and give recording its own fast, lossless default.
CUSTOM_CVAR(Int, vid_record_png_level, 1, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	if (self < 0) self = 0;
	else if (self > 9) self = 9;
}
EXTERN_CVAR(Float, png_gamma);
EXTERN_CVAR(Bool, longsavemessages);

static FString PendingScreenShotName;
static bool PendingScreenShot = false;

static size_t ParseCommandLine (const char *args, int *argc, char **argv);


//---------------------------------------------------------------------------
//
// PROC M_FindResponseFile
//
//---------------------------------------------------------------------------

void M_FindResponseFile (void)
{
	const int limit = 100;	// avoid infinite recursion
	int added_stuff = 0;
	int i = 1;

	while (i < Args->NumArgs())
	{
		if (Args->GetArg(i)[0] != '@')
		{
			i++;
		}
		else
		{
			char	**argv;
			FileSys::FileData file;
			int		argc = 0;
			size_t	argsize = 0;
			int 	index;

			// Any more response files after the limit will be removed from the
			// command line.
			if (added_stuff < limit)
			{
				// READ THE RESPONSE FILE INTO MEMORY
				FileReader fr;
				if (!fr.OpenFile(Args->GetArg(i) + 1))
				{ // [RH] Make this a warning, not an error.
					Printf ("No such response file (%s)!\n", Args->GetArg(i) + 1);
				}
				else
				{
					Printf ("Found response file %s!\n", Args->GetArg(i) + 1);
					file = fr.ReadPadded(1);
					argsize = ParseCommandLine (file.string(), &argc, nullptr);
				}
			}
			else
			{
				Printf ("Ignored response file %s.\n", Args->GetArg(i) + 1);
			}

			if (argc != 0)
			{
				argv = (char **)M_Malloc (argc*sizeof(char *) + argsize);
				argv[0] = (char *)argv + argc*sizeof(char *);
				ParseCommandLine (file.string(), nullptr, argv);

				// Create a new argument vector
				FArgs *newargs = new FArgs;

				// Copy parameters before response file.
				for (index = 0; index < i; ++index)
					newargs->AppendArg(Args->GetArg(index));

				// Copy parameters from response file.
				for (index = 0; index < argc; ++index)
					newargs->AppendArg(argv[index]);

				// Copy parameters after response file.
				for (index = i + 1; index < Args->NumArgs(); ++index)
					newargs->AppendArg(Args->GetArg(index));

				// Use the new argument vector as the global Args object.
				delete Args;
				Args = newargs;
				if (++added_stuff == limit)
				{
					Printf("Response file limit of %d hit.\n", limit);
				}
			}
			else
			{
				// Remove the response file from the Args object
				Args->RemoveArg(i);
			}
		}
	}
	if (added_stuff > 0)
	{
		// DISPLAY ARGS
		Printf ("Added %d response file%s, now have %d command-line args:\n",
			added_stuff, added_stuff > 1 ? "s" : "", Args->NumArgs ());
		for (int k = 1; k < Args->NumArgs (); k++)
			Printf ("%s\n", Args->GetArg (k));
	}
}

// ParseCommandLine
//
// This is just like the version in c_dispatch.cpp, except it does not
// do cvar expansion.

static size_t ParseCommandLine (const char *args, int *argc, char **argv)
{
	int count;
	char* buffstart;
	char *buffplace;

	count = 0;
	buffstart = NULL;
	if (argv != NULL)
	{
		buffstart = argv[0];
	}
	buffplace = buffstart;

	for (;;)
	{
		while (*args <= ' ' && *args)
		{ // skip white space
			args++;
		}
		if (*args == 0)
		{
			break;
		}
		else if (*args == '\"')
		{ // read quoted string
			char stuff;
			if (argv != NULL)
			{
				argv[count] = buffplace;
			}
			count++;
			args++;
			do
			{
				stuff = *args++;
				if (stuff == '\\' && *args == '\"')
				{
					stuff = '\"', args++;
				}
				else if (stuff == '\"')
				{
					stuff = 0;
				}
				else if (stuff == 0)
				{
					args--;
				}
				if (argv != NULL)
				{
					*buffplace = stuff;
				}
				buffplace++;
			} while (stuff);
		}
		else
		{ // read unquoted string
			const char *start = args++, *end;

			while (*args && *args > ' ' && *args != '\"')
				args++;
			end = args;
			if (argv != NULL)
			{
				argv[count] = buffplace;
				while (start < end)
					*buffplace++ = *start++;
				*buffplace++ = 0;
			}
			else
			{
				buffplace += end - start + 1;
			}
			count++;
		}
	}
	if (argc != NULL)
	{
		*argc = count;
	}
	return (buffplace - buffstart);
}


//
// M_SaveDefaults
//

bool M_SaveDefaults (const char *filename)
{
	FString oldpath;
	bool success;

	if (GameConfig == nullptr) return true;
	if (filename != nullptr)
	{
		oldpath = GameConfig->GetPathName();
		GameConfig->ChangePathName (filename);
	}
	GameConfig->ArchiveGlobalData ();
	if (gameinfo.ConfigName.IsNotEmpty())
	{
		GameConfig->ArchiveGameData (gameinfo.ConfigName.GetChars());
	}
	success = GameConfig->WriteConfigFile ();
	if (filename != nullptr)
	{
		GameConfig->ChangePathName (filename);
	}
	return success;
}

void M_SaveDefaultsFinal ()
{
	if (GameConfig == nullptr) return;
	while (!M_SaveDefaults (nullptr) && I_WriteIniFailed (GameConfig->GetPathName()))
	{
		/* Loop until the config saves or I_WriteIniFailed() returns false */
	}
	delete GameConfig;
	GameConfig = nullptr;
}

UNSAFE_CCMD (writeini)
{
	const char *filename = (argv.argc() == 1) ? NULL : argv[1];
	if (!M_SaveDefaults (filename))
	{
		Printf ("Writing config failed: %s\n", strerror(errno));
	}
	else
	{
		Printf ("Config saved.\n");
	}
}

CCMD(openconfig)
{
	M_SaveDefaults(nullptr);
	I_OpenShellFolder(ExtractFilePath(GameConfig->GetPathName()).GetChars());
}

//
// M_LoadDefaults
//

void M_LoadDefaults ()
{
	GameConfig = new FGameConfigFile;
	GameConfig->DoGlobalSetup ();
}


//
// SCREEN SHOTS
//


struct pcx_t
{
	int8_t				manufacturer;
	int8_t				version;
	int8_t				encoding;
	int8_t				bits_per_pixel;

	uint16_t			xmin;
	uint16_t			ymin;
	uint16_t			xmax;
	uint16_t			ymax;
	
	uint16_t			hdpi;
	uint16_t			vdpi;

	uint8_t				palette[48];
	
	int8_t				reserved;
	int8_t				color_planes;
	uint16_t			bytes_per_line;
	uint16_t			palette_type;
	
	int8_t				filler[58];
};


inline void putc(unsigned char chr, FileWriter *file)
{
	file->Write(&chr, 1);
}

//
// WritePCXfile
//
void WritePCXfile (FileWriter *file, const uint8_t *buffer, const PalEntry *palette,
				   ESSType color_type, int width, int height, int pitch)
{
	TArray<uint8_t> temprow_storage(width * 3, true);
	uint8_t *temprow = &temprow_storage[0];
	const uint8_t *data;
	int x, y;
	int runlen;
	int bytes_per_row_minus_one;
	uint8_t color;
	pcx_t pcx;

	pcx.manufacturer = 10;				// PCX id
	pcx.version = 5;					// 256 (or more) colors
	pcx.encoding = 1;
	pcx.bits_per_pixel = 8;				// 256 (or more) colors
	pcx.xmin = 0;
	pcx.ymin = 0;
	pcx.xmax = LittleShort((unsigned short)(width-1));
	pcx.ymax = LittleShort((unsigned short)(height-1));
	pcx.hdpi = LittleShort((unsigned short)75);
	pcx.vdpi = LittleShort((unsigned short)75);
	memset (pcx.palette, 0, sizeof(pcx.palette));
	pcx.reserved = 0;
	pcx.color_planes = (color_type == SS_PAL) ? 1 : 3;	// chunky image
	pcx.bytes_per_line = width + (width & 1);
	pcx.palette_type = 1;				// not a grey scale
	memset (pcx.filler, 0, sizeof(pcx.filler));

	file->Write(&pcx, 128);

	bytes_per_row_minus_one = ((color_type == SS_PAL) ? width : width * 3) - 1;

	// pack the image
	for (y = height; y > 0; y--)
	{
		switch (color_type)
		{
		case SS_PAL:
			data = buffer;
			break;

		case SS_RGB:
			// Unpack RGB into separate planes.
			for (int i = 0; i < width; ++i)
			{
				temprow[i            ] = buffer[i*3];
				temprow[i + width    ] = buffer[i*3 + 1];
				temprow[i + width * 2] = buffer[i*3 + 2];
			}
			data = temprow;
			break;

		case SS_BGRA:
			// Unpack RGB into separate planes, discarding A.
			for (int i = 0; i < width; ++i)
			{
				temprow[i            ] = buffer[i*4 + 2];
				temprow[i + width    ] = buffer[i*4 + 1];
				temprow[i + width * 2] = buffer[i*4];
			}
			data = temprow;
			break;

		default:
			// Should never happen.
			return;
		}
		buffer += pitch;

		color = *data++;
		runlen = 1;

		for (x = bytes_per_row_minus_one; x > 0; x--)
		{
			if (*data == color)
			{
				runlen++;
			}
			else
			{
				if (runlen > 1 || color >= 0xc0)
				{
					while (runlen > 63)
					{
						putc (0xff, file);
						putc (color, file);
						runlen -= 63;
					}
					if (runlen > 0)
					{
						putc (0xc0 + runlen, file);
					}
				}
				if (runlen > 0)
				{
					putc (color, file);
				}
				runlen = 1;
				color = *data;
			}
			data++;
		}

		if (runlen > 1 || color >= 0xc0)
		{
			while (runlen > 63)
			{
				putc (0xff, file);
				putc (color, file);
				runlen -= 63;
			}
			if (runlen > 0)
			{
				putc (0xc0 + runlen, file);
			}
		}
		if (runlen > 0)
		{
			putc (color, file);
		}

		if (width & 1)
			putc (0, file);
	}

	// write the palette
	if (color_type == SS_PAL)
	{
		putc (12, file);		// palette ID byte
		for (x = 0; x < 256; x++, palette++)
		{
			putc (palette->r, file);
			putc (palette->g, file);
			putc (palette->b, file);
		}
	}
}

//
// WritePNGfile
//
void WritePNGfile (FileWriter *file, const uint8_t *buffer, const PalEntry *palette,
				   ESSType color_type, int width, int height, int pitch, float gamma)
{
	char software[100];
	mysnprintf(software, countof(software), GAMENAME " %s", GetVersionString());
	if (!M_CreatePNG (file, buffer, palette, color_type, width, height, pitch, gamma) ||
		!M_AppendPNGText (file, "Software", software) ||
		!M_FinishPNG (file))
	{
		Printf ("%s\n", GStrings.GetString("TXT_SCREENSHOTERR"));
	}
}


//
// M_ScreenShot
//
static bool FindFreeName (FString &fullname, const char *extension)
{
	FString lbmname;
	int i;

	for (i = 0; i <= 9999; i++)
	{
		const char *gamename = gameinfo.ConfigName.GetChars();

		time_t now;
		tm *tm;

		time(&now);
		tm = localtime(&now);

		if (tm == NULL)
		{
			lbmname.Format ("%sScreenshot_%s_%04d.%s", fullname.GetChars(), gamename, i, extension);
		}
		else if (i == 0)
		{
			lbmname.Format ("%sScreenshot_%s_%04d%02d%02d_%02d%02d%02d.%s", fullname.GetChars(), gamename,
				tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
				tm->tm_hour, tm->tm_min, tm->tm_sec,
				extension);
		}
		else
		{
			lbmname.Format ("%sScreenshot_%s_%04d%02d%02d_%02d%02d%02d_%02d.%s", fullname.GetChars(), gamename,
				tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
				tm->tm_hour, tm->tm_min, tm->tm_sec,
				i, extension);
		}

		if (!FileExists (lbmname.GetChars()))
		{
			fullname = lbmname;
			return true;		// file doesn't exist
		}
	}
	return false;
}

void M_ScreenShot (const char *filename)
{
	FileWriter *file;
	FString autoname;
	bool writepcx = (stricmp (screenshot_type, "pcx") == 0);	// PNG is the default

	// find a file name to save it to
	if (filename == NULL || filename[0] == '\0')
	{
		size_t dirlen;
		autoname = Args->CheckValue("-shotdir");
		if (autoname.IsEmpty())
		{
			autoname = screenshot_dir;
		}
		dirlen = autoname.Len();
		if (dirlen == 0)
		{
			autoname = M_GetScreenshotsPath();
			dirlen = autoname.Len();
		}
		if (dirlen > 0)
		{
			if (autoname[dirlen-1] != '/' && autoname[dirlen-1] != '\\')
			{
				autoname += '/';
			}
		}
		autoname = NicePath(autoname.GetChars());
		CreatePath(autoname.GetChars());
		if (!FindFreeName (autoname, writepcx ? "pcx" : "png"))
		{
			Printf ("M_ScreenShot: Delete some screenshots\n");
			return;
		}
	}
	else
	{
		autoname = filename;
		DefaultExtension (autoname, writepcx ? ".pcx" : ".png");
		// An explicit destination may point into a directory that does not
		// exist yet; create it instead of failing with "Could not open".
		ptrdiff_t slash = autoname.LastIndexOfAny(":/\\");
		if (slash > 0)
		{
			FString dir = autoname.Left(slash + 1);
			CreatePath(dir.GetChars());
		}
	}

	// save the screenshot
	int pitch;
	ESSType color_type;
	float gamma;

	auto buffer = screen->GetScreenshotBuffer(pitch, color_type, gamma);
	if (buffer.Size() > 0)
	{
		file = FileWriter::Open(autoname.GetChars());
		if (file == NULL)
		{
			Printf ("Could not open %s\n", autoname.GetChars());
			return;
		}
		if (writepcx)
		{
			WritePCXfile(file, buffer.Data(), nullptr, color_type,
				screen->GetWidth(), screen->GetHeight(), pitch);
		}
		else
		{
			WritePNGfile(file, buffer.Data(), nullptr, color_type,
				screen->GetWidth(), screen->GetHeight(), pitch, gamma);
		}
		delete file;

		if (!screenshot_quiet)
		{
			ptrdiff_t slash = -1;
			if (!longsavemessages) slash = autoname.LastIndexOfAny(":/\\");
			Printf ("Captured %s\n", autoname.GetChars()+slash+1);
		}
	}
	else
	{
		if (!screenshot_quiet)
		{
			Printf ("Could not create screenshot.\n");
		}
	}
}

//---------------------------------------------------------------------------
//
// Recording export paths
//
//---------------------------------------------------------------------------

static FString MakeCapturePathUnique(FString filename)
{
	if (!FileExists(filename.GetChars()))
	{
		return filename;
	}

	const ptrdiff_t slash = filename.LastIndexOfAny(":/\\");
	const ptrdiff_t dot = filename.LastIndexOf('.');
	const FString stem = dot > slash ? filename.Left(dot) : filename;
	const FString extension = dot > slash ? filename.Mid(dot) : "";

	for (unsigned int index = 1; index <= 9999; ++index)
	{
		FString candidate;
		candidate.Format("%s_%03u%s", stem.GetChars(), index, extension.GetChars());
		if (!FileExists(candidate.GetChars()))
		{
			return candidate;
		}
	}
	return FString();
}

FString M_GetCaptureExportPath()
{
	FString path;
	path = capture_export_dir;
	path.StripLeftRight();
	if (path.IsEmpty())
	{
		path = M_GetScreenshotsPath();
		path += "captures";
	}
	path = NicePath(path.GetChars());
	if (path.IsNotEmpty() && path.Back() != '/' && path.Back() != '\\')
	{
		path += '/';
	}
	CreatePath(path.GetChars());
	return path;
}

FString M_MakeCaptureFileName(const char *requestedName, const char *extension, const char *defaultStem)
{
	FString filename = requestedName ? requestedName : "";
	filename.StripLeftRight();
	if (filename.IsEmpty())
	{
		time_t now;
		time(&now);
		tm *local = localtime(&now);
		if (local != nullptr)
		{
			filename.Format("%s%s_%s_%04d%02d%02d_%02d%02d%02d%s",
				M_GetCaptureExportPath().GetChars(), defaultStem, gameinfo.ConfigName.GetChars(),
				local->tm_year + 1900, local->tm_mon + 1, local->tm_mday,
				local->tm_hour, local->tm_min, local->tm_sec, extension);
		}
		else
		{
			filename.Format("%s%s_%s%s", M_GetCaptureExportPath().GetChars(),
				defaultStem, gameinfo.ConfigName.GetChars(), extension);
		}
	}
	else
	{
		filename = NicePath(filename.GetChars());
		if (filename.LastIndexOfAny(":/\\") < 0)
		{
			filename = M_GetCaptureExportPath() + filename;
		}
	}

	const ptrdiff_t slash = filename.LastIndexOfAny(":/\\");
	const ptrdiff_t dot = filename.LastIndexOf('.');
	if (dot > slash)
	{
		filename.Truncate(dot);
	}
	filename += extension;
	if (slash > 0)
	{
		CreatePath(filename.Left(slash + 1).GetChars());
	}
	return MakeCapturePathUnique(filename);
}

//---------------------------------------------------------------------------
//
// Lossless final-frame video capture
//
//---------------------------------------------------------------------------

namespace
{
	enum EVideoRecordingFormat
	{
		VIDEO_RECORDING_PNG_SEQUENCE = 0,
		VIDEO_RECORDING_RGB_AVI = 1,
	};

	// Recording rate is a delivery property, not a gameplay simulation property.
	// I_nsTime() deliberately follows i_timescale, so use a raw monotonic clock
	// here to keep a 60 FPS capture at 60 frames per wall-clock second even when
	// the player slows down or fast-forwards the game.
	static uint64_t CaptureWallClockNS()
	{
		using namespace std::chrono;
		return (uint64_t)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
	}

	static FString IndexedCaptureStem(const FString &original, unsigned int index)
	{
		if (index == 0)
		{
			return original;
		}
		FString candidate;
		candidate.Format("%s_%03u", original.GetChars(), index);
		return candidate;
	}

	static bool CaptureFamilyHasFiles(const FString &stem, const char *prefixSuffix, const char *extension)
	{
		FString directory = ExtractFilePath(stem.GetChars());
		if (directory.IsEmpty())
		{
			directory = ".";
		}
		FString prefix = ExtractFileBase(stem.GetChars(), true);
		prefix += prefixSuffix;
		const size_t extensionLength = strlen(extension);
		FileSys::FileList matches;
		// This is an explicit collision-safety scan, so include dotfiles as well:
		// a user may intentionally name a capture family ".take".
		if (!FileSys::ScanDirectory(matches, directory.GetChars(), "*", true, true))
		{
			return false;
		}
		for (const auto &entry : matches)
		{
			const size_t length = entry.FileName.size();
			if (!entry.isDirectory && length >= prefix.Len() + extensionLength &&
				strnicmp(entry.FileName.c_str(), prefix.GetChars(), prefix.Len()) == 0 &&
				strnicmp(entry.FileName.c_str() + length - extensionLength, extension, extensionLength) == 0)
			{
				return true;
			}
		}
		return false;
	}

	static bool SequenceStemAvailable(const FString &stem)
	{
		FString baseFile = stem;
		baseFile += ".png";
		FString timelineFile = stem;
		timelineFile += ".ffconcat";
		FString audioFile = stem;
		audioFile += "_audio.wav";
		return !FileExists(baseFile.GetChars()) &&
			!FileExists(timelineFile.GetChars()) &&
			!FileExists(audioFile.GetChars()) &&
			!CaptureFamilyHasFiles(stem, "_frame", ".png") &&
			!CaptureFamilyHasFiles(stem, "_part", ".png") &&
			!CaptureFamilyHasFiles(stem, "_part", ".ffconcat") &&
			!CaptureFamilyHasFiles(stem, "_audio", ".wav");
	}

	static bool AviStemAvailable(const FString &stem)
	{
		FString baseFile = stem;
		baseFile += ".avi";
		return !FileExists(baseFile.GetChars()) && !CaptureFamilyHasFiles(stem, "_part", ".avi");
	}

	// RIFF and idx1 offsets are 32-bit, and FileWriter uses the platform C
	// stream seek API. Leave enough headroom for the header/index and signed
	// 32-bit seek implementations before continuing in a sibling part.
	constexpr uint64_t AVI_PART_LIMIT = 1900ull * 1024ull * 1024ull;

	// Video capture runs beside the renderer, so a slow disk must never turn
	// into an ever-growing amount of render-thread work or memory. The writer
	// owns a small, byte-bounded queue; when it cannot keep up, producer frames
	// are intentionally skipped rather than backfilled later.
	constexpr unsigned int VIDEO_QUEUE_MAX_FRAMES = 3;
	constexpr uint64_t VIDEO_QUEUE_MAX_BYTES = 64ull * 1024ull * 1024ull;
	constexpr uint64_t VIDEO_MAX_SINGLE_FRAME_BYTES = 128ull * 1024ull * 1024ull;
	// Finalization normally drains every PBO that has already been submitted.
	// A wedged driver must not, however, leave the recorder in FINALIZING (and a
	// queued restart blocked) forever. This is a no-wait wall-clock budget: the
	// normal path keeps zero-polling and retains every ready tail frame; expiry
	// abandons only work that the GPU has still not completed.
	constexpr uint64_t VIDEO_STOP_READBACK_DRAIN_MAX_NS = 2ull * 1000000000ull;
	// AVI stores one 16-byte idx1 entry per raw frame. At very small
	// resolutions the payload limit alone would permit an unnecessarily large
	// in-memory index, so roll into a sibling part before that can happen.
	constexpr uint64_t AVI_INDEX_MAX_BYTES = 64ull * 1024ull * 1024ull;
	// Raw AVI is deliberately available as a reference format, but at modern
	// resolutions it can fill a disk in seconds. Keep a large system reserve,
	// cap one take, and re-check the destination regularly. The session cap is
	// also useful on filesystems whose free-space report lags buffered IO.
	constexpr uint64_t VIDEO_DISK_RESERVE_BYTES = 4ull * 1024ull * 1024ull * 1024ull;
	constexpr uint64_t VIDEO_SESSION_MAX_BYTES = 32ull * 1024ull * 1024ull * 1024ull;
	constexpr unsigned int VIDEO_MAX_FREE_SPACE_FRACTION = 2;
	// A free-space query can acquire filesystem-wide locks or become an RPC on a
	// network/FUSE capture destination. The strict per-take budget still gates
	// every write; probing at this interval bounds any external-disk-use race
	// without making the encoder issue statfs once per captured frame.
	constexpr uint64_t VIDEO_FREE_SPACE_CHECK_INTERVAL = 64ull * 1024ull * 1024ull;
	constexpr uint64_t VIDEO_PNG_ESTIMATE_OVERHEAD = 1024ull * 1024ull;
	// A PNG take carries a small ffconcat sidecar which preserves the actual
	// sparse capture timeline. Reserve one safely oversized entry per frame so
	// sidecar writes do not need a filesystem-space query of their own.
	constexpr uint64_t VIDEO_PNG_TIMELINE_RESERVE = 8ull * 1024ull;
	// PNG compression is CPU-bound on modern GPUs. A recording may therefore
	// use a few persistent encoders, but it must never turn a slow destination
	// into an unbounded worker or raw-frame backlog. The count is deliberately
	// capped by the same admission limit that bounds queued capture data.
	constexpr unsigned int VIDEO_PNG_MAX_ENCODER_WORKERS = VIDEO_QUEUE_MAX_FRAMES;
	constexpr uint64_t VIDEO_AVI_HEADER_RESERVE = 64ull * 1024ull;
	constexpr uint64_t VIDEO_AVI_FINALIZATION_RESERVE = 64ull * 1024ull;

	struct FAviIndexEntry
	{
		char ChunkID[4];
		uint32_t Flags;
		uint32_t Offset;
		uint32_t Size;
	};

	struct FQueuedVideoFrame
	{
		// GPU backends may hand us native RGBA rows. Keep them intact until the
		// writer owns the frame: packing/flipping every sampled image on the
		// render thread was the remaining large CPU cost of recording.
		TArray<uint8_t> Pixels;
		int Width = 0;
		int Height = 0;
		int Pitch = 0;
		ESSType ColorType = SS_RGB;
		bool BottomUp = false;
		float Gamma = 1.0f;
		// StorageBytes bounds queue memory. OutputBytes is the RGB payload size
		// used by disk-budget estimates, irrespective of a native RGBA readback.
		uint64_t StorageBytes = 0;
		uint64_t OutputBytes = 0;
		// This is the wall-clock moment at which the composed frame was
		// requested, never the later writer completion time. It lets an AVI
		// retain real elapsed time when the bounded recorder deliberately
		// skips frames under pressure.
		uint64_t CaptureTimeNS = 0;
	};

	// The coordinator assigns names and reservations before handing a frame to
	// a worker. Workers own only image compression and their individual file;
	// ordering, output accounting, and ffconcat writes remain serial.
	struct FPngEncodeTask
	{
		FQueuedVideoFrame Frame;
		FString Filename;
		uint64_t FrameNumber = 0;
		uint64_t EstimateBytes = 0;
		uint64_t ReservationBytes = 0;
		// Only true after this worker has created the pathname. An outer worker
		// exception must never remove a pre-existing file merely because it was
		// the task's intended output name.
		bool OutputOpened = false;
	};

	struct FPngEncodeResult
	{
		FString Filename;
		FString Error;
		// Worker-side error recovery must not allocate: an allocation failure is
		// exactly the condition in which this result still has to reach the
		// serial coordinator. Dynamic error text remains available for normal
		// results, while this fixed pointer covers worker exception boundaries.
		const char *StaticError = nullptr;
		uint64_t FrameNumber = 0;
		uint64_t CaptureTimeNS = 0;
		uint64_t StorageBytes = 0;
		uint64_t EstimateBytes = 0;
		uint64_t ReservationBytes = 0;
		bool Succeeded = false;
	};

	struct FPngWorkerState
	{
		// There can never be more tasks in this coordinator than the global
		// raw-frame admission limit. Keeping both hand-off lists in fixed slots
		// is intentional: dispatch and completion must not allocate after a
		// queued frame has been removed and counted as in-flight.
		std::array<FPngEncodeTask, VIDEO_PNG_MAX_ENCODER_WORKERS> Pending;
		std::array<FPngEncodeResult, VIDEO_PNG_MAX_ENCODER_WORKERS> Completed;
		unsigned int PendingCount = 0;
		unsigned int CompletedCount = 0;
		unsigned int EncodingFrames = 0;
		bool StopWorkers = false;
		// Set only when a worker could not even convert its exception into a
		// per-frame completion. The coordinator observes it under mQueueMutex
		// and turns it into the same orderly terminal path as an encode failure.
		bool WorkerFailed = false;
	};

	static unsigned int VideoPngEncoderWorkerCount()
	{
		const unsigned int hardwareThreads = std::thread::hardware_concurrency();
		if (hardwareThreads == 0 || hardwareThreads <= 3)
		{
			return 1;
		}
		if (hardwareThreads <= 5)
		{
			return 2;
		}
		return VIDEO_PNG_MAX_ENCODER_WORKERS;
	}

	static int VideoBytesPerPixel(ESSType colorType)
	{
		switch (colorType)
		{
		case SS_RGB: return 3;
		case SS_BGRA:
		case SS_RGBA: return 4;
		default: return 0;
		}
	}

	enum EVideoWriterResult
	{
		VIDEO_WRITER_COMPLETE,
		VIDEO_WRITER_SPACE_LIMIT,
		VIDEO_WRITER_FAILURE,
	};

	enum EQueuedCaptureResult
	{
		CAPTURE_FRAME_QUEUED,
		CAPTURE_FRAME_QUEUE_FULL,
		CAPTURE_FRAME_INVALID,
	};

	static bool QueryCaptureFreeSpace(const FString &filename, uint64_t &available)
	{
		FString directory = ExtractFilePath(filename.GetChars());
		if (directory.IsEmpty())
		{
			directory = ".";
		}
		std::error_code error;
		const auto info = std::filesystem::space(std::filesystem::u8path(directory.GetChars()), error);
		if (error)
		{
			return false;
		}
		const auto maximum = std::numeric_limits<uint64_t>::max();
		available = info.available > maximum ? maximum : (uint64_t)info.available;
		return true;
	}

	class FVideoRecorder
	{
	public:
		~FVideoRecorder()
		{
			// D_Cleanup normally stops recording before renderer teardown. This
			// backstop makes static destruction safe if an earlier startup failure
			// bypasses that normal path. The queue is intentionally tiny and
			// byte-bounded, so shutdown may safely drain already captured frames
			// instead of silently shortening a normally ending take.
			// Mirror the normal stop boundary even during abnormal shutdown: the
			// writer may still need the completed audio snapshot while it drains,
			// but stream callbacks must stop appending before that finalization.
			const uint64_t stopTimeNS = RequestWriterStop(true);
			I_StopVideoRecordingAudio(stopTimeNS);
			JoinWriter();
			I_DiscardVideoRecordingAudio();
			DiscardAviPart();
			DiscardPngTimeline();
		}

		bool Start(const char *requestedName)
		{
			// A completed background writer may not have been reaped yet if the
			// player stopped on a menu or paused the simulation. Reap it here so
			// a new console command need not wait for a later capture callback.
			if (mActive && JoinWriterIfFinished())
			{
				CompleteStopAndRestartIfRequested();
			}
			if (mActive)
			{
				// Never reuse the recorder's output state while the old writer owns
				// it. Instead, remember one requested take and arm it as soon as the
				// old file has been finalized. This keeps an immediate
				// stopvideorecording/startvideorecording (or toggle) responsive even
				// when a slow filesystem is still flushing the previous capture.
					if (StopRequested() || mStopCaptureDrainPending)
				{
					if (mRestartPending)
					{
						Printf("Video recording restart is already pending.\n");
						return false;
					}
					mRestartPending = true;
					mPendingStartName = requestedName ? requestedName : "";
					Printf("Video recording will start after the previous take finishes.\n");
					return true;
				}
				Printf("Video recording is already active.\n");
				return false;
			}
			if (I_IsHeadless())
			{
				Printf("Video recording is unavailable in headless mode.\n");
				PublishStartFailure("Video capture needs an active renderer");
				return false;
			}
			// A new take must never inherit a PBO/staging request from a previous
			// recording or renderer change. The backend release is bounded and
			// does not wait for the GPU.
			if (screen != nullptr)
			{
				screen->ResetVideoCapture();
			}

			mFormat = vid_record_format == VIDEO_RECORDING_RGB_AVI ? VIDEO_RECORDING_RGB_AVI : VIDEO_RECORDING_PNG_SEQUENCE;
			mFrameRate = vid_record_fps;
			if (mFrameRate < 1) mFrameRate = 1;
			if (mFrameRate > 240) mFrameRate = 240;
			mCapturePeriodNS = 1000000000ull / (uint64_t)mFrameRate;
			if (mCapturePeriodNS == 0) mCapturePeriodNS = 1;
			mPngCompressionLevel = vid_record_png_level;
			if (mPngCompressionLevel < 0) mPngCompressionLevel = 0;
			else if (mPngCompressionLevel > 9) mPngCompressionLevel = 9;
			mPngGammaOverride = png_gamma;
			FString configuredName;
			configuredName = vid_record_name;
			const char *name = requestedName != nullptr && requestedName[0] != '\0' ? requestedName : configuredName.GetChars();
			mBaseFile = M_MakeCaptureFileName(name, mFormat == VIDEO_RECORDING_RGB_AVI ? ".avi" : ".png", "Video");
			if (mBaseFile.IsEmpty())
			{
				Printf("Could not find an unused video capture filename.\n");
				PublishStartFailure("Could not find an unused capture filename");
				return false;
			}

			mSequenceStem = StripExtension(mBaseFile);
			if (mFormat == VIDEO_RECORDING_PNG_SEQUENCE)
			{
				if (!SelectUnusedSequenceStem())
				{
					Printf("Could not find an unused PNG sequence name.\n");
					PublishStartFailure("Could not find an unused PNG sequence name");
					return false;
				}
			}
			else if (!SelectUnusedAviFamily())
			{
				Printf("Could not find an unused AVI recording name.\n");
				PublishStartFailure("Could not find an unused AVI recording name");
				return false;
			}
			if (!InitializeOutputBudget())
			{
				PublishStartFailure("Could not reserve safe capture storage");
				return false;
			}

			mPart = 1;
			mNextCaptureTime = 0;
			mTotalFrames = 0;
				mFinalizedAviFrames = 0;
				mFinalizedAviParts = 0;
				mAviNextSlot = 0;
				mAviDuplicatedFrames = 0;
				mAviHavePackedFrame = false;
				mAviTimelineOriginNS = 0;
				mAviPartFirstSlot = 0;
			mAviPartAudioFrames = 0;
			mEstimatedOutputBytes = 0;
			mPngReservedOutputBytes = 0;
			mOutputOpen = false;
			mDroppedFrames = 0;
				mReadbackDeferredFrames = 0;
				mDropNoticeShown = false;
				mReadbackNoticeShown = false;
				mStopNoticeShown = false;
				mStopDrainQueueWaitNoticeShown = false;
				mStopCaptureDrainPending = false;
				mStopCaptureDrainTimeNS = 0;
				mStopCaptureDrainStartedNS = 0;
				mStopReadbackAbandoned = false;
				mReadbackAbandonedForBackpressure = false;
			mProducerStopReason = "";
			mCurrentFile = "";
			mPngTimelinePath = "";
			mPngTimelineLastFile = "";
			mPngTimelineLastCaptureTimeNS = 0;
			mPngTimelineFailed = false;
			mAudioOutputPath = "";
			mBgrFrame.Reset();
			mAviIndex.Reset();
			mWriterFailureReason = "";
			mWriterSpaceLimited = false;
			mTakeStartTimeNS = CaptureWallClockNS();
			const FString audioStem = mFormat == VIDEO_RECORDING_PNG_SEQUENCE ?
				mSequenceStem : StripExtension(mBaseFile);
			I_StartVideoRecordingAudio(audioStem, mTakeStartTimeNS);
			{
				std::lock_guard<std::mutex> lock(mQueueMutex);
				mQueue.clear();
				mQueuedBytes = 0;
				mOutstandingFrames = 0;
				mStopRequested = false;
				mWriterAborting = false;
				mStopTimeNS = 0;
				mWriterFinished = false;
				mWriterResult = VIDEO_WRITER_COMPLETE;
				mWriterResultMessage = "";
				mWriterPngPoolFallback = false;
			}
			try
			{
				mWriterThread = std::thread(&FVideoRecorder::WriterMain, this);
			}
			catch (...)
			{
				// A worker-pool allocation failure is not allowed to terminate the
				// recording coordinator. No task has been dispatched yet, so the
				// established single bounded writer remains a safe fallback.
				Printf("Could not start the video capture writer thread.\n");
				I_DiscardVideoRecordingAudio();
				ResetAfterStop();
				PublishStartFailure("Could not start the capture writer");
				return false;
			}
			mActive = true;
			PublishRecordingStatus(mFormat == VIDEO_RECORDING_PNG_SEQUENCE ?
				"PNG capture" : "RGB AVI capture");

			if (mFormat == VIDEO_RECORDING_PNG_SEQUENCE)
			{
				Printf("Lossless PNG recording armed at %d FPS: %s_frame000001.png\n", mFrameRate, mSequenceStem.GetChars());
			}
			else
			{
				Printf("Lossless RGB AVI recording armed at %d FPS: %s\n", mFrameRate, mBaseFile.GetChars());
			}
			return true;
		}

			void Stop()
			{
				if (!mActive)
			{
				Printf("Video recording is not active.\n");
				return;
			}

			// A stop request is a responsiveness boundary: do not join the worker
			// here. It is still important to preserve every frame the render thread
			// has already handed off, though. The queue is capped at three frames
			// and a byte limit, so asking the background worker to drain it cannot
			// create an unbounded stop backlog or stall gameplay.
				if (mRestartPending)
			{
				mRestartPending = false;
					mPendingStartName = "";
					Printf("Pending video recording restart canceled.\n");
					}
			if (mStopCaptureDrainPending || StopRequested())
			{
				return;
			}
					const uint64_t stopTimeNS = CaptureWallClockNS();
					I_StopVideoRecordingAudio(stopTimeNS);
					mStopCaptureDrainTimeNS = stopTimeNS;
					mStopCaptureDrainStartedNS = stopTimeNS;
					PublishStoppingStatus(stopTimeNS, "Finalizing captured frames");
				if (screen != nullptr && screen->HasPendingVideoCapture())
				{
						// Keep the writer alive while the backend zero-polls work already
						// submitted before the stop. Drain mode never issues a replacement
						// transfer and never waits for the GPU.
						mStopCaptureDrainPending = true;
						PublishStoppingStatus(stopTimeNS, "Finalizing final GPU readback");
						return;
				}
				RequestWriterStopAfterCaptureDrain();
				CompleteInteractiveStop();
		}

		void Fail(const char *reason)
		{
			if (!mActive)
			{
				return;
			}
			if (mProducerStopReason.IsEmpty())
			{
				mProducerStopReason = reason != nullptr && reason[0] != '\0' ? reason :
					"the capture backend could not make forward progress";
			}
			Stop();
		}

			void Finish()
		{
			if (!mActive)
			{
				return;
			}
			// Engine teardown must never arm a take that was requested while a
			// prior writer was still finalizing.
			mRestartPending = false;
			mPendingStartName = "";
			if (!StopRequested() && !mStopCaptureDrainPending)
			{
				Stop();
			}
			if (!mActive)
			{
				return;
			}
			if (mStopCaptureDrainPending)
			{
				// Teardown has no subsequent presentation callback in which to
				// keep polling. Take one zero-wait poll for a frame that is already
				// ready, then release any remaining backend work rather than making
				// application exit wait on the GPU.
				DrainStoppingReadback();
				if (mStopCaptureDrainPending)
				{
					if (screen != nullptr)
					{
						screen->ResetVideoCapture();
					}
					RequestWriterStopAfterCaptureDrain();
				}
			}
			// Stop() normally captured this exact boundary. Keep Finish idempotent
			// for teardown paths that reached the writer-stop state by another route.
			const uint64_t audioStopTimeNS = GetStopTimeNS();
			I_StopVideoRecordingAudio(audioStopTimeNS != 0 ? audioStopTimeNS : CaptureWallClockNS());
			JoinWriter();
			CompleteStop();
		}

		bool IsActive() const
		{
			return mActive;
		}

		bool GetStatus(FVideoRecordingStatus &status) const
		{
			const uint64_t now = CaptureWallClockNS();
			std::lock_guard<std::mutex> lock(mStatusMutex);
			if (mRecordingStatus.State == VRS_None ||
				((mRecordingStatus.State == VRS_Finalized || mRecordingStatus.State == VRS_Failed) &&
					mStatusVisibleUntilNS != 0 && now >= mStatusVisibleUntilNS))
			{
				return false;
			}
			status = mRecordingStatus;
			if (status.State == VRS_Recording && mStatusTakeStartNS != 0 && now >= mStatusTakeStartNS)
			{
				status.ElapsedMilliseconds = (now - mStatusTakeStartNS) / 1000000ull;
			}
			return true;
		}

		void Toggle(const char *requestedName)
		{
			// A second toggle while a restart is pending cancels it. A toggle
			// after a normal stop queues the next take without waiting for a slow
			// close on the old one.
				if (mRestartPending || (mActive && !StopRequested() && !mStopCaptureDrainPending))
			{
				Stop();
			}
			else
			{
				Start(requestedName);
			}
		}

		void CaptureFrame()
		{
			if (!mActive || screen == nullptr)
			{
				return;
			}

			if (mStopCaptureDrainPending)
			{
				DrainStoppingReadback();
				return;
			}

			if (StopRequested())
			{
					if (JoinWriterIfFinished())
					{
						CompleteStopAndRestartIfRequested();
					}
					return;
				}

			if (WriterNeedsStop())
			{
				Stop();
				return;
			}

			const uint64_t now = CaptureWallClockNS();
			if (mNextCaptureTime != 0 && now < mNextCaptureTime)
			{
				return;
			}
			// Never replay missed wall-clock slots. A capture that is slow for a
			// moment must recover immediately, rather than writing an ever-larger
			// duplicate-frame backlog on this render update.

					int width = 0;
					int height = 0;
					screen->GetVideoCaptureDimensions(width, height);
				uint64_t expectedOutputBytes = 0;
				uint64_t maximumReadbackBytes = 0;
				if (!CalculateCaptureFrameBounds(width, height, expectedOutputBytes, maximumReadbackBytes))
			{
				mProducerStopReason = "the active resolution exceeds the safe capture-frame limit";
				Stop();
				return;
			}
			// Reserve for the largest supported native format before issuing any
			// GPU request. A full queue therefore skips the sample without asking
			// the renderer to start work that the writer cannot own.
			if (!CanQueueFrame(maximumReadbackBytes))
			{
				// A full CPU queue must not leave an older asynchronous GPU request
				// active indefinitely. In particular, OpenGL suppresses its legacy
				// finish while a capture PBO is pending; waiting for the writer to
				// free a slot before polling that PBO can therefore make its bounded
				// frame-fence gate freeze presentation. Drop only the unusable GPU
				// tail once per congestion spell; the backend retains any idle
				// storage for the next accepted sample.
				if (!mReadbackAbandonedForBackpressure)
				{
					screen->AbandonPendingVideoCaptureReadbacks();
					mReadbackAbandonedForBackpressure = true;
				}
				ScheduleNextCapture(now);
				NoteDroppedFrame();
				return;
			}
			mReadbackAbandonedForBackpressure = false;

			int pitch = 0;
			int captureWidth = width;
			int captureHeight = height;
			ESSType colorType = SS_RGB;
			float gamma = 1.0f;
			uint64_t captureTimeNS = now;
			bool readbackPending = false;
			bool bottomUp = false;
			auto screenshot = screen->GetVideoCaptureBuffer(captureWidth, captureHeight, pitch, colorType, gamma,
				now, captureTimeNS, readbackPending, bottomUp);
			if (screenshot.Size() == 0 && readbackPending)
			{
				// The asynchronous backend deliberately has no ready frame this
				// update. Treat that as a skipped sampling opportunity, not a
				// renderer failure or a reason to wait on the GPU.
				NoteReadbackDeferredFrame();
				ScheduleNextCapture(now);
				return;
			}
				const EQueuedCaptureResult queued = QueueCapturedFrame(std::move(screenshot), captureWidth, captureHeight,
					pitch, colorType, gamma, captureTimeNS, bottomUp);
				if (queued == CAPTURE_FRAME_INVALID)
				{
					mProducerStopReason = "the active renderer could not provide a valid final frame";
					Stop();
					return;
				}
				if (queued == CAPTURE_FRAME_QUEUE_FULL)
				{
					// GetVideoCaptureBuffer() may have refilled an asynchronous slot
					// just before a concurrent writer state change rejects this frame.
					// It has no useful consumer now, so apply the same one-shot release
					// policy as the preflight path.
					if (!mReadbackAbandonedForBackpressure)
					{
						screen->AbandonPendingVideoCaptureReadbacks();
						mReadbackAbandonedForBackpressure = true;
					}
					NoteDroppedFrame();
				}
			ScheduleNextCapture(now);
		}

		// Advance only an already-stopping recorder. This is intentionally not a
		// general capture callback: the frame-admission gate may invoke it after
		// deciding that no new rendering work is safe, so it must never ask the
		// backend to issue another readback.
		void PollStopping()
		{
			if (!mActive)
			{
				return;
			}
			if (mStopCaptureDrainPending)
			{
				DrainStoppingReadback();
				return;
			}
			if (StopRequested() && JoinWriterIfFinished())
			{
				CompleteStopAndRestartIfRequested();
			}
		}

	private:
		static constexpr uint64_t VIDEO_STATUS_NOTICE_NS = 8ull * 1000000000ull;

		bool CalculateCaptureFrameBounds(int width, int height, uint64_t &expectedOutputBytes,
			uint64_t &maximumReadbackBytes) const
		{
			expectedOutputBytes = 0;
			maximumReadbackBytes = 0;
			if (width <= 0 || height <= 0)
			{
				return false;
			}
			const uint64_t outputRowBytes = (uint64_t)width * 3ull;
			const uint64_t readbackRowBytes = (uint64_t)width * 4ull;
			const uint64_t rows = (uint64_t)height;
			// Bound before multiplying rows so a malformed or future extreme
			// video mode cannot wrap this calculation into an apparently small
			// allocation. Native hardware readback can be RGBA even though the
			// on-disk frame is RGB.
			if (readbackRowBytes > VIDEO_MAX_SINGLE_FRAME_BYTES / rows)
			{
				return false;
			}
			maximumReadbackBytes = readbackRowBytes * rows;
			expectedOutputBytes = outputRowBytes * rows;
			return expectedOutputBytes != 0 && maximumReadbackBytes != 0 &&
				expectedOutputBytes <= 0xffffffffull && maximumReadbackBytes <= VIDEO_MAX_SINGLE_FRAME_BYTES;
		}

		EQueuedCaptureResult QueueCapturedFrame(TArray<uint8_t> &&screenshot, int captureWidth, int captureHeight,
			int pitch, ESSType colorType, float gamma, uint64_t captureTimeNS, bool bottomUp)
		{
			const int bytesPerPixel = VideoBytesPerPixel(colorType);
			const uint64_t sourceBytes = bytesPerPixel > 0 && captureWidth > 0 && captureHeight > 0 &&
				pitch >= captureWidth * bytesPerPixel ? (uint64_t)pitch * (uint64_t)captureHeight : 0;
			const uint64_t outputBytes = captureWidth > 0 && captureHeight > 0 &&
				(uint64_t)captureWidth <= 0xffffffffull / ((uint64_t)captureHeight * 3ull) ?
				(uint64_t)captureWidth * (uint64_t)captureHeight * 3ull : 0;
			if (screenshot.Size() == 0 || bytesPerPixel == 0 || sourceBytes == 0 || outputBytes == 0 ||
				outputBytes > 0xffffffffull || sourceBytes > VIDEO_MAX_SINGLE_FRAME_BYTES || sourceBytes > screenshot.Size())
			{
				return CAPTURE_FRAME_INVALID;
			}

			FQueuedVideoFrame frame;
			frame.Width = captureWidth;
			frame.Height = captureHeight;
			frame.Pitch = pitch;
			frame.ColorType = colorType;
			frame.BottomUp = bottomUp;
			frame.Gamma = gamma > 0.0f ? gamma : 1.0f;
			frame.OutputBytes = outputBytes;
			frame.CaptureTimeNS = captureTimeNS;
			// Move a tightly-packed native readback straight into the bounded queue.
			// RGBA swizzling and vertical orientation are intentionally deferred to
			// the background PNG/AVI writer; the render thread only needs to retire
			// the GPU buffer promptly. Keep a compact copy path for padded third-
			// party renderers.
			if (pitch == captureWidth * bytesPerPixel && screenshot.Size() == sourceBytes)
			{
				frame.Pixels = std::move(screenshot);
				frame.StorageBytes = sourceBytes;
			}
			else if (!CopyFinalFrame(frame, screenshot, pitch))
			{
				return CAPTURE_FRAME_INVALID;
			}
			return QueueFrame(std::move(frame)) ? CAPTURE_FRAME_QUEUED : CAPTURE_FRAME_QUEUE_FULL;
		}

		void CompleteInteractiveStop()
		{
			if (!JoinWriterIfFinished())
			{
				if (!mStopNoticeShown)
				{
					Printf("Video recording is stopping in the background; captured frames are being finalized.\n");
					mStopNoticeShown = true;
				}
				return;
			}
			CompleteStopAndRestartIfRequested();
		}

		void CompleteStopAndRestartIfRequested()
		{
			const bool restart = mRestartPending;
			FString restartName = mPendingStartName;
			mRestartPending = false;
			mPendingStartName = "";
			CompleteStop();
			if (restart)
			{
				Start(restartName.IsEmpty() ? nullptr : restartName.GetChars());
			}
		}

		void RequestWriterStopAfterCaptureDrain()
		{
			const uint64_t stopTimeNS = mStopCaptureDrainTimeNS != 0 ?
				mStopCaptureDrainTimeNS : CaptureWallClockNS();
			mStopCaptureDrainPending = false;
			if (screen != nullptr)
			{
					// All tail frames have been consumed, or finalization is ending
					// because the renderer is being released. This releases completed
					// backing storage and retires only genuinely unfinished GPU work
					// without a blocking wait.
				screen->ResetVideoCapture();
			}
			RequestWriterStop(true, stopTimeNS);
			mStopCaptureDrainTimeNS = 0;
			mStopCaptureDrainStartedNS = 0;
		}

		void DrainStoppingReadback()
		{
			if (!mStopCaptureDrainPending)
			{
				return;
			}
			if (screen == nullptr)
			{
				Printf("Video capture renderer was released before its final GPU readback completed.\n");
				RequestWriterStopAfterCaptureDrain();
				CompleteInteractiveStop();
				return;
			}
			if (WriterNeedsStop())
			{
				Printf("Video capture writer stopped before the final GPU readback completed.\n");
				RequestWriterStopAfterCaptureDrain();
				CompleteInteractiveStop();
				return;
			}

			const uint64_t now = CaptureWallClockNS();

			if (!screen->HasPendingVideoCapture())
			{
				RequestWriterStopAfterCaptureDrain();
				CompleteInteractiveStop();
				return;
			}

			if (mStopCaptureDrainStartedNS != 0 && now >= mStopCaptureDrainStartedNS &&
				now - mStopCaptureDrainStartedNS >= VIDEO_STOP_READBACK_DRAIN_MAX_NS)
			{
				// All previously queued CPU frames are still finalized. The only
				// discarded data is the last GPU sample that could not safely reach
				// the bounded writer within this grace period; waiting longer would
				// recreate the recording hang this subsystem exists to avoid.
				mStopReadbackAbandoned = true;
				Printf("Video capture finalizing: final GPU sample could not be finalized; preserving all completed frames.\n");
				PublishStoppingStatus(mStopCaptureDrainTimeNS, "Final GPU sample timed out; preserving completed frames");
				RequestWriterStopAfterCaptureDrain();
				CompleteInteractiveStop();
				return;
			}

			// The pending backend slot can predate a resize. Reserve the hard
			// single-frame maximum instead of the *current* viewport, so we never
			// consume a valid old-size tail and then reject it for lack of queue
			// capacity. This only runs while stopping and consequently waits for a
			// fully free bounded queue before touching the PBO/staging image.
			const uint64_t maximumReadbackBytes = VIDEO_MAX_SINGLE_FRAME_BYTES;
			// Do not consume a ready PBO/staging image until the writer has a slot
			// to own it. That lets the worker free a bounded queue naturally rather
			// than turning finalization into a tail-frame discard.
			if (!CanQueueFrame(maximumReadbackBytes))
			{
				if (!mStopDrainQueueWaitNoticeShown)
				{
					Printf("Video capture finalizing: waiting for the writer to receive the final GPU frame.\n");
					PublishStoppingStatus(mStopCaptureDrainTimeNS, "Waiting for writer before final GPU frame");
					mStopDrainQueueWaitNoticeShown = true;
				}
				return;
			}

			int captureWidth = 0;
			int captureHeight = 0;
			int pitch = 0;
			ESSType colorType = SS_RGB;
			float gamma = 1.0f;
			uint64_t captureTimeNS = now;
			bool readbackPending = false;
			bool bottomUp = false;
			auto screenshot = screen->GetVideoCaptureBuffer(captureWidth, captureHeight, pitch, colorType, gamma,
				now, captureTimeNS, readbackPending, bottomUp, false);
			if (screenshot.Size() != 0)
			{
				const EQueuedCaptureResult queued = QueueCapturedFrame(std::move(screenshot), captureWidth, captureHeight,
					pitch, colorType, gamma, captureTimeNS, bottomUp);
				if (queued == CAPTURE_FRAME_INVALID)
				{
					mProducerStopReason = "the final GPU readback was incomplete";
					RequestWriterStopAfterCaptureDrain();
					CompleteInteractiveStop();
					return;
				}
				if (queued == CAPTURE_FRAME_QUEUE_FULL)
				{
					// The preflight above makes this possible only if the writer
					// stopped between checks. Surface the fatal hand-off failure rather
					// than pretending the final frame was safely retained.
					mProducerStopReason = "the video writer stopped before it could accept the final GPU readback";
					RequestWriterStopAfterCaptureDrain();
					CompleteInteractiveStop();
					return;
				}
			}

			if (!screen->HasPendingVideoCapture())
			{
				RequestWriterStopAfterCaptureDrain();
				CompleteInteractiveStop();
			}
		}

			void PublishRecordingStatus(const FString &detail)
		{
			std::lock_guard<std::mutex> lock(mStatusMutex);
			mRecordingStatus.State = VRS_Recording;
			mRecordingStatus.ElapsedMilliseconds = 0;
			mRecordingStatus.Detail = detail;
			mStatusTakeStartNS = mTakeStartTimeNS;
			mStatusVisibleUntilNS = 0;
		}

		void PublishStoppingStatus(uint64_t stopTimeNS, const FString &detail)
		{
			std::lock_guard<std::mutex> lock(mStatusMutex);
			mRecordingStatus.State = VRS_Stopping;
			mRecordingStatus.ElapsedMilliseconds =
				mTakeStartTimeNS != 0 && stopTimeNS >= mTakeStartTimeNS ?
				(stopTimeNS - mTakeStartTimeNS) / 1000000ull : 0;
			mRecordingStatus.Detail = detail;
			mStatusVisibleUntilNS = 0;
		}

		void PublishTerminalStatus(EVideoRecordingState state, uint64_t elapsedMilliseconds,
			const FString &detail)
		{
			std::lock_guard<std::mutex> lock(mStatusMutex);
			mRecordingStatus.State = state;
			mRecordingStatus.ElapsedMilliseconds = elapsedMilliseconds;
			mRecordingStatus.Detail = detail;
			mStatusVisibleUntilNS = CaptureWallClockNS() + VIDEO_STATUS_NOTICE_NS;
		}

		void PublishStartFailure(const char *reason)
		{
			PublishTerminalStatus(VRS_Failed, 0, FString(reason));
		}

		void CompleteStop()
		{
			EVideoWriterResult result = VIDEO_WRITER_COMPLETE;
			FString message;
			uint64_t stopTimeNS = 0;
			bool pngPoolFallback = false;
			{
				std::lock_guard<std::mutex> lock(mQueueMutex);
				result = mWriterResult;
				message = mWriterResultMessage;
				stopTimeNS = mStopTimeNS;
				pngPoolFallback = mWriterPngPoolFallback;
			}
			if (pngPoolFallback)
			{
				// The writer thread may not touch the console. Report its safe
				// serial fallback only after JoinWriter* established the hand-off.
				Printf("PNG encoder pool was unavailable; used bounded serial encoding.\n");
			}
			if (mProducerStopReason.IsNotEmpty())
			{
				result = VIDEO_WRITER_FAILURE;
				message = mProducerStopReason;
			}
			if (result != VIDEO_WRITER_COMPLETE)
			{
				Printf("Video recording stopped: %s.\n", message.IsEmpty() ? "the writer could not complete the take" : message.GetChars());
			}
			const uint64_t completedFrames = mFormat == VIDEO_RECORDING_RGB_AVI ? mFinalizedAviFrames : mTotalFrames;
			const int completedParts = mFormat == VIDEO_RECORDING_RGB_AVI ? mFinalizedAviParts : mPart;
			if (completedFrames > 0)
			{
				if (mFormat == VIDEO_RECORDING_PNG_SEQUENCE)
				{
					Printf("Video recording stopped: %llu lossless PNG frames at %d FPS (%s_frame%%06d.png).\n",
						(unsigned long long)completedFrames, mFrameRate, mSequenceStem.GetChars());
				}
				else
				{
					Printf("Video recording stopped: %llu lossless RGB AVI frames in %d part%s.\n",
						(unsigned long long)completedFrames, completedParts, completedParts == 1 ? "" : "s");
					if (mAviDuplicatedFrames != 0)
					{
						Printf("AVI maintained its selected cadence with %llu held frame%s.\n",
							(unsigned long long)mAviDuplicatedFrames, mAviDuplicatedFrames == 1 ? "" : "s");
					}
				}
			}
			else if (result == VIDEO_WRITER_COMPLETE)
			{
				Printf("Video recording stopped before a composited frame was available.\n");
			}
			if (mDroppedFrames != 0)
			{
				Printf("Video capture skipped %llu frame%s to keep the game responsive.\n",
					(unsigned long long)mDroppedFrames, mDroppedFrames == 1 ? "" : "s");
			}
			if (mReadbackDeferredFrames != 0)
			{
				Printf("Video capture deferred %llu GPU readback sample%s; output timing retained elapsed time.\n",
					(unsigned long long)mReadbackDeferredFrames, mReadbackDeferredFrames == 1 ? "" : "s");
			}
			if (mStopReadbackAbandoned)
			{
				Printf("Video capture finalized without its unavailable last GPU sample; completed frames and audio were preserved.\n");
			}
			if (result == VIDEO_WRITER_COMPLETE && completedFrames != 0)
			{
				if (mFormat == VIDEO_RECORDING_PNG_SEQUENCE && mAudioOutputPath.IsNotEmpty())
				{
					Printf("Video recording audio: %s\n", mAudioOutputPath.GetChars());
				}
				else if (mFormat == VIDEO_RECORDING_RGB_AVI)
				{
					Printf("Video recording audio: PCM %u Hz stereo muxed into AVI.\n",
						I_GetVideoRecordingAudioSampleRate());
				}
			}
			const uint64_t completedAtNS = stopTimeNS != 0 ? stopTimeNS : CaptureWallClockNS();
			const uint64_t elapsedMilliseconds = mTakeStartTimeNS != 0 && completedAtNS >= mTakeStartTimeNS ?
				(completedAtNS - mTakeStartTimeNS) / 1000000ull : 0;
			FString statusDetail;
			if (result != VIDEO_WRITER_COMPLETE)
			{
				statusDetail = message.IsEmpty() ? "The video writer could not complete the take" : message;
				PublishTerminalStatus(VRS_Failed, elapsedMilliseconds, statusDetail);
			}
			else if (completedFrames == 0)
			{
				PublishTerminalStatus(VRS_Finalized, elapsedMilliseconds, "Stopped before a composited frame was available");
			}
			else
			{
				statusDetail = mFormat == VIDEO_RECORDING_RGB_AVI ? mBaseFile : mSequenceStem;
				if (mStopReadbackAbandoned)
				{
					statusDetail += " (final GPU sample unavailable)";
				}
				PublishTerminalStatus(VRS_Finalized, elapsedMilliseconds, statusDetail);
			}
			ResetAfterStop();
		}
		static FString StripExtension(const FString &filename)
		{
			const ptrdiff_t slash = filename.LastIndexOfAny(":/\\");
			const ptrdiff_t dot = filename.LastIndexOf('.');
			return dot > slash ? filename.Left(dot) : filename;
		}

		bool SelectUnusedSequenceStem()
		{
			const FString original = mSequenceStem;
			for (unsigned int index = 0; index <= 9999; ++index)
			{
				FString candidate = IndexedCaptureStem(original, index);
				if (SequenceStemAvailable(candidate))
				{
					mSequenceStem = candidate;
					return true;
				}
			}
			return false;
		}

		bool SelectUnusedAviFamily()
		{
			const FString original = StripExtension(mBaseFile);
			for (unsigned int index = 0; index <= 9999; ++index)
			{
				const FString candidate = IndexedCaptureStem(original, index);
				if (AviStemAvailable(candidate))
				{
					mBaseFile = candidate + ".avi";
					return true;
				}
			}
			return false;
		}

		FString SequenceStemForPart() const
		{
			if (mPart == 1) return mSequenceStem;
			FString result;
			result.Format("%s_part%03d", mSequenceStem.GetChars(), mPart);
			return result;
		}

		FString AviFileForPart() const
		{
			if (mPart == 1) return mBaseFile;
			FString result;
			result.Format("%s_part%03d.avi", StripExtension(mBaseFile).GetChars(), mPart);
			return result;
		}

		bool InitializeOutputBudget()
		{
			const FString destination = mFormat == VIDEO_RECORDING_RGB_AVI ? mBaseFile : mSequenceStem;
			uint64_t available = 0;
			if (!QueryCaptureFreeSpace(destination, available))
			{
				Printf("Could not verify free space for the video capture destination.\n");
				return false;
			}
			if (available <= VIDEO_DISK_RESERVE_BYTES)
			{
				Printf("Video recording needs at least 4 GiB of free-space reserve at its destination.\n");
				return false;
			}
			mOutputByteBudget = available - VIDEO_DISK_RESERVE_BYTES;
			const uint64_t halfAvailable = available / VIDEO_MAX_FREE_SPACE_FRACTION;
			if (mOutputByteBudget > halfAvailable)
			{
				mOutputByteBudget = halfAvailable;
			}
			if (mOutputByteBudget > VIDEO_SESSION_MAX_BYTES)
			{
				mOutputByteBudget = VIDEO_SESSION_MAX_BYTES;
			}
			// The startup query already proved that the full take budget can retain
			// its reserve. Defer the next probe until output has meaningfully
			// advanced; this avoids a potentially blocking filesystem query on every
			// PNG/AVI frame while preserving a bounded external-disk-use window.
			mNextOutputSpaceProbeAtBytes = VIDEO_FREE_SPACE_CHECK_INTERVAL;
			return true;
		}

			void ScheduleNextCapture(uint64_t captureStarted)
			{
				const uint64_t completed = CaptureWallClockNS();
				const uint64_t maximum = std::numeric_limits<uint64_t>::max();
				// Keep capture slots on an absolute wall-clock grid.  Rebasing the
				// next slot on `completed + period` made every non-blocking readback,
				// queue hand-off, or slow frame add its own cost to the following
				// interval. That turns a nominal 60 FPS take into a lower and drifting
				// rate even when the renderer can otherwise sustain it.  We deliberately
				// skip overdue slots rather than replaying them: no catch-up work is
				// queued on the render thread and the timestamped output still retains
				// the real elapsed interval.
				uint64_t next = mNextCaptureTime;
				if (next == 0)
				{
					next = captureStarted > maximum - mCapturePeriodNS ? maximum :
						captureStarted + mCapturePeriodNS;
				}
				if (next <= completed)
				{
					const uint64_t overdue = completed - next;
					const uint64_t slots = overdue / mCapturePeriodNS + 1;
					if (slots > (maximum - next) / mCapturePeriodNS)
					{
						next = maximum;
					}
					else
					{
						next += slots * mCapturePeriodNS;
					}
				}
				mNextCaptureTime = next;
			}

		bool RequireOutputSpace(uint64_t expectedBytes)
		{
			if (mEstimatedOutputBytes > mOutputByteBudget || expectedBytes > mOutputByteBudget - mEstimatedOutputBytes)
			{
				mWriterSpaceLimited = true;
				mWriterFailureReason = "the recording reached its safe output limit";
				return false;
			}
			const uint64_t projectedBytes = mEstimatedOutputBytes + expectedBytes;
			if (mNextOutputSpaceProbeAtBytes != 0 && projectedBytes < mNextOutputSpaceProbeAtBytes)
			{
				return true;
			}
			uint64_t available = 0;
			const FString destination = mFormat == VIDEO_RECORDING_RGB_AVI ? mBaseFile : mSequenceStem;
			if (!QueryCaptureFreeSpace(destination, available))
			{
				mWriterSpaceLimited = true;
				mWriterFailureReason = "could not verify free space for the capture destination";
				return false;
			}
			if (available <= VIDEO_DISK_RESERVE_BYTES || expectedBytes > available - VIDEO_DISK_RESERVE_BYTES)
			{
				mWriterSpaceLimited = true;
				mWriterFailureReason = "the capture kept its 4 GiB free-space reserve";
				return false;
			}
			const uint64_t maximum = std::numeric_limits<uint64_t>::max();
			mNextOutputSpaceProbeAtBytes = projectedBytes > maximum - VIDEO_FREE_SPACE_CHECK_INTERVAL ?
				maximum : projectedBytes + VIDEO_FREE_SPACE_CHECK_INTERVAL;
			return true;
		}

		// PNG tasks reserve their conservative maximum before an encoder starts.
		// This lets several workers write in parallel without weakening the take
		// budget or the 4 GiB free-space reserve. Completed output remains in
		// mEstimatedOutputBytes; in-flight output lives here until the ordered
		// coordinator commits its timing entry.
		bool ReservePngOutputSpace(uint64_t expectedBytes)
		{
			if (mEstimatedOutputBytes > mOutputByteBudget ||
				mPngReservedOutputBytes > mOutputByteBudget - mEstimatedOutputBytes ||
				expectedBytes > mOutputByteBudget - mEstimatedOutputBytes - mPngReservedOutputBytes)
			{
				mWriterSpaceLimited = true;
				mWriterFailureReason = "the recording reached its safe output limit";
				return false;
			}
			const uint64_t projectedBytes = mEstimatedOutputBytes + mPngReservedOutputBytes + expectedBytes;
			if (mNextOutputSpaceProbeAtBytes == 0 || projectedBytes >= mNextOutputSpaceProbeAtBytes)
			{
				uint64_t available = 0;
				if (!QueryCaptureFreeSpace(mSequenceStem, available))
				{
					mWriterSpaceLimited = true;
					mWriterFailureReason = "could not verify free space for the capture destination";
					return false;
				}
				if (available <= VIDEO_DISK_RESERVE_BYTES ||
					mPngReservedOutputBytes > available - VIDEO_DISK_RESERVE_BYTES ||
					expectedBytes > available - VIDEO_DISK_RESERVE_BYTES - mPngReservedOutputBytes)
				{
					mWriterSpaceLimited = true;
					mWriterFailureReason = "the capture kept its 4 GiB free-space reserve";
					return false;
				}
				const uint64_t maximum = std::numeric_limits<uint64_t>::max();
				mNextOutputSpaceProbeAtBytes = projectedBytes > maximum - VIDEO_FREE_SPACE_CHECK_INTERVAL ?
					maximum : projectedBytes + VIDEO_FREE_SPACE_CHECK_INTERVAL;
			}
			mPngReservedOutputBytes += expectedBytes;
			return true;
		}

		void ReleasePngOutputReservation(uint64_t reservedBytes)
		{
			mPngReservedOutputBytes -= reservedBytes;
		}

		bool EnsureOutput(int width, int height, uint64_t changeTimeNS)
		{
			if (!mOutputOpen)
			{
				mWidth = width;
				mHeight = height;
				mOutputOpen = true;
				if (mFormat == VIDEO_RECORDING_RGB_AVI && !OpenAviPart())
				{
					return false;
				}
				return true;
			}
			if (mWidth == width && mHeight == height)
			{
				return true;
			}

				// The first frame at the new resolution owns its selected-rate slot.
				// Complete only the preceding slots with the old packed image, then
				// start a new AVI part without moving the global cadence boundary.
				if (mFormat == VIDEO_RECORDING_RGB_AVI && mAviHavePackedFrame &&
					!FillAviSlotsUntil(AviSlotForTime(changeTimeNS)))
				{
					return false;
				}
				if (mFormat == VIDEO_RECORDING_RGB_AVI && !FinishAviPart())
			{
				DiscardAviPart();
				if (mWriterFailureReason.IsEmpty()) mWriterFailureReason = "could not finalize the AVI part after a resolution change";
				return false;
			}
			if (mFormat == VIDEO_RECORDING_PNG_SEQUENCE && !FinishPngTimeline(changeTimeNS))
			{
				if (mWriterFailureReason.IsEmpty()) mWriterFailureReason = "could not finalize the PNG timing sidecar after a resolution change";
				return false;
			}
			++mPart;
				mWidth = width;
				mHeight = height;
				mBgrFrame.Reset();
				mAviHavePackedFrame = false;
				if (mFormat == VIDEO_RECORDING_RGB_AVI && !OpenAviPart())
			{
				return false;
			}
			return true;
		}

		bool CopyFinalFrame(FQueuedVideoFrame &frame, const TArray<uint8_t> &screenshot, int pitch)
		{
			const int bytesPerPixel = VideoBytesPerPixel(frame.ColorType);
			if (bytesPerPixel == 0 || pitch < frame.Width * bytesPerPixel)
			{
				return false;
			}
			const uint64_t sourceSize = (uint64_t)pitch * (uint64_t)frame.Height;
			const uint64_t packedRowBytes = (uint64_t)frame.Width * (uint64_t)bytesPerPixel;
			const uint64_t packedSize = packedRowBytes * (uint64_t)frame.Height;
			if (packedSize > 0xffffffffull || sourceSize > screenshot.Size())
			{
				return false;
			}
			frame.Pixels.Resize((unsigned int)packedSize);
			for (int y = 0; y < frame.Height; ++y)
			{
				memcpy(frame.Pixels.Data() + (size_t)y * packedRowBytes,
					screenshot.Data() + (size_t)y * pitch, (size_t)packedRowBytes);
			}
			frame.Pitch = (int)packedRowBytes;
			frame.StorageBytes = packedSize;
			return true;
		}

		bool WriteFrame(const FQueuedVideoFrame &frame)
		{
			if (!EnsureOutput(frame.Width, frame.Height, frame.CaptureTimeNS))
			{
				return false;
			}
			const bool written = mFormat == VIDEO_RECORDING_PNG_SEQUENCE ? WritePngFrame(frame) : WriteAviFrame(frame);
			if (written)
			{
				++mTotalFrames;
			}
			return written;
		}

		FString PngTimelinePath() const
		{
			FString result = SequenceStemForPart();
			result += ".ffconcat";
			return result;
		}

		static FString EscapeFfconcatPath(const FString &path)
		{
			FString escaped;
			for (unsigned int index = 0; index < path.Len(); ++index)
			{
				const char character = path[index];
				// ffconcat uses a backslash to escape the delimiters in its
				// single-quoted file form. Keep timeline output robust for a
				// perfectly valid user-selected capture folder/name.
				if (character == '\\' || character == '\'')
				{
					escaped += '\\';
				}
				escaped += character;
			}
			return escaped;
		}

		bool WritePngTimelineText(const FString &text)
		{
			if (mPngTimelineFailed)
			{
				return false;
			}
			if (mPngTimeline == nullptr || text.IsEmpty())
			{
				return text.IsEmpty();
			}
			const uint64_t length = text.Len();
			// Payload writes periodically refresh the conservative free-space
			// check. The adjacent timing metadata is tiny and already covered by
			// that reservation, so it does not need its own filesystem-space
			// syscall for every pair of ffconcat lines. Keep the session budget
			// strict even for this metadata.
			if (mEstimatedOutputBytes > mOutputByteBudget ||
				length > mOutputByteBudget - mEstimatedOutputBytes ||
				mPngTimeline->Write(text.GetChars(), text.Len()) != text.Len())
			{
				mWriterFailureReason = "could not write the PNG timing sidecar";
				AbortPngTimeline();
				return false;
			}
			mEstimatedOutputBytes += length;
			return true;
		}

		bool EnsurePngTimeline()
		{
			if (mPngTimelineFailed)
			{
				return false;
			}
			if (mPngTimeline != nullptr)
			{
				return true;
			}
			mPngTimelinePath = PngTimelinePath();
			if (FileExists(mPngTimelinePath.GetChars()))
			{
				mWriterFailureReason = "would overwrite an existing PNG timing sidecar";
				return false;
			}
			mPngTimeline = FileWriter::Open(mPngTimelinePath.GetChars());
			if (mPngTimeline == nullptr)
			{
				mWriterFailureReason = "could not create the PNG timing sidecar";
				return false;
			}
			if (!WritePngTimelineText("ffconcat version 1.0\n"))
			{
				// WritePngTimelineText invalidates and removes a partial sidecar.
				return false;
			}
			return true;
		}

		bool WritePngTimelineEntry(const FString &filename, uint64_t durationNS)
		{
			const uint64_t safeDuration = durationNS != 0 ? durationNS : mCapturePeriodNS;
			FString line;
			line.Format("file '%s'\nduration %.9f\n", EscapeFfconcatPath(filename).GetChars(),
				(double)safeDuration / 1000000000.0);
			return WritePngTimelineText(line);
		}

		bool AppendPngTimelineFrame(const FString &filename, uint64_t captureTimeNS)
		{
			if (!EnsurePngTimeline())
			{
				return false;
			}
			if (mPngTimelineLastFile.IsNotEmpty())
			{
				const uint64_t duration = captureTimeNS > mPngTimelineLastCaptureTimeNS ?
					captureTimeNS - mPngTimelineLastCaptureTimeNS : mCapturePeriodNS;
				if (!WritePngTimelineEntry(mPngTimelineLastFile, duration))
				{
					return false;
				}
			}
			// The sidecar lives beside the images, so a basename keeps it portable
			// if the completed capture family is moved as a directory.
			mPngTimelineLastFile = ExtractFileBase(filename.GetChars(), true);
			mPngTimelineLastCaptureTimeNS = captureTimeNS;
			return true;
		}

		bool FinishPngTimeline(uint64_t requestedEndTimeNS = 0)
		{
			if (mPngTimelineFailed)
			{
				AbortPngTimeline();
				return false;
			}
			if (mPngTimeline == nullptr)
			{
				return true;
			}
			bool written = true;
			if (mPngTimelineLastFile.IsNotEmpty())
			{
				uint64_t endTimeNS = requestedEndTimeNS != 0 ? requestedEndTimeNS : GetStopTimeNS();
				if (endTimeNS <= mPngTimelineLastCaptureTimeNS)
				{
					endTimeNS = mPngTimelineLastCaptureTimeNS + mCapturePeriodNS;
				}
				written = WritePngTimelineEntry(mPngTimelineLastFile,
					endTimeNS - mPngTimelineLastCaptureTimeNS);
				// ffconcat applies the final duration only when a subsequent entry
				// exists; duplicate the last still image without adding media data.
				if (written)
				{
					FString finalEntry;
					finalEntry.Format("file '%s'\n", EscapeFfconcatPath(mPngTimelineLastFile).GetChars());
					written = WritePngTimelineText(finalEntry);
				}
			}
			const bool closed = mPngTimeline->CloseChecked();
			delete mPngTimeline;
			mPngTimeline = nullptr;
			if (!written || !closed)
			{
				mWriterFailureReason = "could not finalize the PNG timing sidecar";
				AbortPngTimeline();
				return false;
			}
			mPngTimelinePath = "";
			mPngTimelineLastFile = "";
			mPngTimelineLastCaptureTimeNS = 0;
			return true;
		}

		bool FinishPngAudio()
		{
			const uint64_t stopTimeNS = GetStopTimeNS();
			if (mTotalFrames == 0 || stopTimeNS <= mTakeStartTimeNS)
			{
				return true;
			}
			const uint64_t frames = AudioFrameForTime(stopTimeNS);
			const uint64_t blockAlign = (uint64_t)I_GetVideoRecordingAudioChannels() *
				(I_GetVideoRecordingAudioBitsPerSample() / 8u);
			if (frames == 0 || frames > (0xffffffffull - 36ull) / blockAlign)
			{
				mWriterFailureReason = "the PNG audio sidecar exceeds the WAV size limit";
				return false;
			}
			const uint64_t bytes = 44ull + frames * blockAlign;
			if (!RequireOutputSpace(bytes))
			{
				return false;
			}
			FString writtenPath;
			FString error;
			if (!I_WriteVideoRecordingAudioWav(writtenPath, error))
			{
				mWriterFailureReason = error.IsEmpty() ? "could not write the PNG audio sidecar" : error;
				return false;
			}
			mEstimatedOutputBytes += bytes;
			mAudioOutputPath = writtenPath;
			return true;
		}

		void AbortPngTimeline()
		{
			mPngTimelineFailed = true;
			if (mPngTimeline != nullptr)
			{
				// A close error cannot make a partial ffconcat file trustworthy.
				// Close for the filesystem's benefit, then remove it regardless.
				mPngTimeline->CloseChecked();
				delete mPngTimeline;
				mPngTimeline = nullptr;
			}
			if (mPngTimelinePath.IsNotEmpty())
			{
				RemoveFile(mPngTimelinePath.GetChars());
			}
			mPngTimelinePath = "";
			mPngTimelineLastFile = "";
			mPngTimelineLastCaptureTimeNS = 0;
		}

		void DiscardPngTimeline()
		{
			if (mPngTimeline != nullptr || mPngTimelineFailed)
			{
				AbortPngTimeline();
			}
			else
			{
				mPngTimelinePath = "";
			}
			mPngTimelineLastFile = "";
			mPngTimelineLastCaptureTimeNS = 0;
			mPngTimelineFailed = false;
		}

		bool WritePngFrame(const FQueuedVideoFrame &frame)
		{
			const uint64_t estimate = frame.OutputBytes + VIDEO_PNG_ESTIMATE_OVERHEAD;
			if (estimate > std::numeric_limits<uint64_t>::max() - VIDEO_PNG_TIMELINE_RESERVE ||
				!RequireOutputSpace(estimate + VIDEO_PNG_TIMELINE_RESERVE))
			{
				return false;
			}
			FString filename;
			filename.Format("%s_frame%06llu.png", SequenceStemForPart().GetChars(), (unsigned long long)(mTotalFrames + 1));
			if (FileExists(filename.GetChars()))
			{
				mWriterFailureReason = "would overwrite an existing PNG frame";
				return false;
			}
			std::unique_ptr<FileWriter> file(FileWriter::Open(filename.GetChars()));
			if (file == nullptr)
			{
				mWriterFailureReason = "could not create a PNG frame";
				return false;
			}
			const int bytesPerPixel = VideoBytesPerPixel(frame.ColorType);
			const uint8_t *pixels = frame.Pixels.Data();
			int pitch = frame.Pitch;
			if (frame.BottomUp && pixels != nullptr && pitch > 0)
			{
				pixels += (size_t)(mHeight - 1) * (size_t)pitch;
				pitch = -pitch;
			}
			bool encoded = false;
			bool closed = false;
			try
			{
				encoded = bytesPerPixel != 0 && pitch != 0 &&
					M_CreatePNGWithEncoder(file.get(), pixels, nullptr, frame.ColorType, mWidth, mHeight, pitch,
						frame.Gamma, mPngCompressionLevel, mPngGammaOverride, true, &mPngEncoder) && M_FinishPNG(file.get());
				closed = file->CloseChecked();
				file.reset();
			}
			catch (...)
			{
				// A PNG allocator or encoder error must close the descriptor before
				// the partial pathname is removed. The serial writer turns it into
				// an ordinary failed frame rather than letting its thread terminate.
				file.reset();
				RemoveFile(filename.GetChars());
				mWriterFailureReason = "could not write a PNG frame";
				return false;
			}
			bool written = encoded && closed;
			if (!written)
			{
				RemoveFile(filename.GetChars());
				mWriterFailureReason = "could not write a PNG frame";
			}
			else
			{
				mEstimatedOutputBytes += estimate;
				if (!AppendPngTimelineFrame(filename, frame.CaptureTimeNS))
				{
					// The timing sidecar is part of a PNG recording's playback
					// contract. Do not leave an unreferenced final frame when it
					// cannot be safely described.
					RemoveFile(filename.GetChars());
					written = false;
				}
			}
			return written;
		}

		static FPngEncodeResult EncodePngTask(FPngEncodeTask &&task, int compressionLevel,
			float configuredGamma, FPNGEncoder &encoder)
		{
			FPngEncodeResult result;
			result.Filename = task.Filename;
			result.FrameNumber = task.FrameNumber;
			result.CaptureTimeNS = task.Frame.CaptureTimeNS;
			result.StorageBytes = task.Frame.StorageBytes;
			result.EstimateBytes = task.EstimateBytes;
			result.ReservationBytes = task.ReservationBytes;

			// The coordinator checked this before dispatching, but re-check on the
			// worker so an external file creation can never be overwritten while a
			// take is in progress.
			if (FileExists(result.Filename.GetChars()))
			{
				result.StaticError = "would overwrite an existing PNG frame";
				return result;
			}
			std::unique_ptr<FileWriter> file(FileWriter::Open(result.Filename.GetChars()));
			if (file == nullptr)
			{
				result.StaticError = "could not create a PNG frame";
				return result;
			}
			task.OutputOpened = true;
			const int bytesPerPixel = VideoBytesPerPixel(task.Frame.ColorType);
			const uint8_t *pixels = task.Frame.Pixels.Data();
			int pitch = task.Frame.Pitch;
			if (task.Frame.BottomUp && pixels != nullptr && pitch > 0)
			{
				pixels += (size_t)(task.Frame.Height - 1) * (size_t)pitch;
				pitch = -pitch;
			}
			bool encoded = false;
			bool closed = false;
			try
			{
				encoded = bytesPerPixel != 0 && pixels != nullptr && pitch != 0 &&
					M_CreatePNGWithEncoder(file.get(), pixels, nullptr, task.Frame.ColorType,
						task.Frame.Width, task.Frame.Height, pitch, task.Frame.Gamma,
						compressionLevel, configuredGamma, true, &encoder) && M_FinishPNG(file.get());
				closed = file->CloseChecked();
				file.reset();
			}
			catch (...)
			{
				file.reset();
				task.OutputOpened = false;
				RemoveFile(result.Filename.GetChars());
				result.StaticError = "could not write a PNG frame";
				return result;
			}
			if (!encoded || !closed)
			{
				task.OutputOpened = false;
				RemoveFile(result.Filename.GetChars());
				result.StaticError = "could not write a PNG frame";
				return result;
			}
			result.Succeeded = true;
			return result;
		}

		bool WriteBytes(const void *data, size_t size)
		{
			if (mFile == nullptr || mFile->Write(data, size) != size)
			{
				return false;
			}
			mEstimatedOutputBytes += size;
			return true;
		}

		bool WriteU16(uint16_t value)
		{
			const uint8_t bytes[2] = { uint8_t(value), uint8_t(value >> 8) };
			return WriteBytes(bytes, sizeof(bytes));
		}

		bool WriteU32(uint32_t value)
		{
			const uint8_t bytes[4] = { uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24) };
			return WriteBytes(bytes, sizeof(bytes));
		}

		bool WriteFourCC(const char *fourCC)
		{
			return WriteBytes(fourCC, 4);
		}

		bool PatchU32(ptrdiff_t position, uint32_t value)
		{
			const ptrdiff_t restore = mFile->Tell();
			const uint8_t bytes[4] = { uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24) };
			if (restore < 0 || mFile->Seek(position, SEEK_SET) != 0 || mFile->Write(bytes, sizeof(bytes)) != sizeof(bytes) || mFile->Seek(restore, SEEK_SET) != 0)
			{
				return false;
			}
			return true;
		}

		bool OpenAviPart()
		{
			const uint64_t rowBytes = (uint64_t)mWidth * 3ull;
			const uint64_t stride = (rowBytes + 3ull) & ~3ull;
			const uint64_t frameBytes = stride * (uint64_t)mHeight;
			if (frameBytes == 0 || frameBytes > 0xffffffffull || mWidth > 0x7fffffff || mHeight > 0x7fffffff)
			{
				mWriterFailureReason = "the AVI dimensions are invalid";
				return false;
			}
			if (!RequireOutputSpace(VIDEO_AVI_HEADER_RESERVE))
			{
				return false;
			}
			mAviStride = (uint32_t)stride;
			mAviFrameBytes = (uint32_t)frameBytes;
			mBgrFrame.Resize(mAviFrameBytes);
			mCurrentFile = AviFileForPart();
			if (FileExists(mCurrentFile.GetChars()))
			{
				mWriterFailureReason = "would overwrite an existing AVI part";
				return false;
			}
			mFile = FileWriter::Open(mCurrentFile.GetChars());
			if (mFile == nullptr)
			{
				mWriterFailureReason = "could not create the AVI output file";
				return false;
			}
				mAviIndex.Reset();
				mAviPayloadBytes = 0;
				mAviPartFrames = 0;
				mAviPartAudioFrames = 0;
				mAviPartFirstSlot = mAviNextSlot;

			const uint64_t audioBytesPerSecond = (uint64_t)I_GetVideoRecordingAudioSampleRate() *
				I_GetVideoRecordingAudioChannels() * (I_GetVideoRecordingAudioBitsPerSample() / 8u);
			const uint64_t maximumBytesPerSecond = (uint64_t)mAviFrameBytes * (uint64_t)mFrameRate + audioBytesPerSecond;
			const uint32_t bytesPerSecond = maximumBytesPerSecond > 0xffffffffull ? 0xffffffffu : (uint32_t)maximumBytesPerSecond;
			if (!WriteFourCC("RIFF")) { mWriterFailureReason = "could not write the AVI header"; return false; }
			mRiffSizePosition = mFile->Tell();
			if (!WriteU32(0) || !WriteFourCC("AVI ") || !WriteFourCC("LIST")) { mWriterFailureReason = "could not write the AVI header"; return false; }
			const ptrdiff_t hdrlSizePosition = mFile->Tell();
			if (!WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			const ptrdiff_t hdrlStart = mFile->Tell();
			if (!WriteFourCC("hdrl") || !WriteFourCC("avih") || !WriteU32(56)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			mAvihMicrosecondsPerFramePosition = mFile->Tell();
				if (!WriteU32(AviPartMicrosecondsPerFrame())) { mWriterFailureReason = "could not write the AVI header"; return false; }
			mAvihMaxBytesPerSecondPosition = mFile->Tell();
			if (!WriteU32(bytesPerSecond) || !WriteU32(0) || !WriteU32(0x10)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			mAvihFrameCountPosition = mFile->Tell();
			if (!WriteU32(0) || !WriteU32(0) || !WriteU32(2) || !WriteU32(mAviFrameBytes) || !WriteU32((uint32_t)mWidth) || !WriteU32((uint32_t)mHeight)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			for (int index = 0; index < 4; ++index) if (!WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }

			if (!WriteFourCC("LIST")) { mWriterFailureReason = "could not write the AVI header"; return false; }
			const ptrdiff_t strlSizePosition = mFile->Tell();
			if (!WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			const ptrdiff_t strlStart = mFile->Tell();
			if (!WriteFourCC("strl") || !WriteFourCC("strh") || !WriteU32(56) || !WriteFourCC("vids") || !WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			if (!WriteU32(0) || !WriteU16(0) || !WriteU16(0) || !WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			mStrhScalePosition = mFile->Tell();
			if (!WriteU32(1)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			mStrhRatePosition = mFile->Tell();
			if (!WriteU32((uint32_t)mFrameRate) || !WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			mStrhFrameCountPosition = mFile->Tell();
			if (!WriteU32(0) || !WriteU32(mAviFrameBytes) || !WriteU32(0xffffffffu) || !WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			for (int index = 0; index < 4; ++index) if (!WriteU16(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }

			if (!WriteFourCC("strf") || !WriteU32(40) || !WriteU32(40) || !WriteU32((uint32_t)mWidth) || !WriteU32((uint32_t)mHeight)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			if (!WriteU16(1) || !WriteU16(24) || !WriteU32(0) || !WriteU32(mAviFrameBytes) || !WriteU32(0) || !WriteU32(0) || !WriteU32(0) || !WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }

			const ptrdiff_t afterStrl = mFile->Tell();
			if (afterStrl < strlSizePosition + 4 || !PatchU32(strlSizePosition, (uint32_t)(afterStrl - strlSizePosition - 4))) { mWriterFailureReason = "could not write the AVI header"; return false; }

			// PCM audio is a second native AVI stream. Its scale/sample-size are
			// one interleaved 16-bit stereo frame so dwLength is directly patchable
			// from the mixed audio-frame count at part finalization.
			if (!WriteFourCC("LIST")) { mWriterFailureReason = "could not write the AVI header"; return false; }
			const ptrdiff_t audioStrlSizePosition = mFile->Tell();
			if (!WriteU32(0) || !WriteFourCC("strl") || !WriteFourCC("strh") || !WriteU32(56) || !WriteFourCC("auds") || !WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			if (!WriteU32(0) || !WriteU16(0) || !WriteU16(0) || !WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			if (!WriteU32(I_GetVideoRecordingAudioChannels() * (I_GetVideoRecordingAudioBitsPerSample() / 8u))) { mWriterFailureReason = "could not write the AVI header"; return false; }
			if (!WriteU32((uint32_t)audioBytesPerSecond) || !WriteU32(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			mStrhAudioFrameCountPosition = mFile->Tell();
			if (!WriteU32(0) || !WriteU32(16384) || !WriteU32(0xffffffffu) ||
				!WriteU32(I_GetVideoRecordingAudioChannels() * (I_GetVideoRecordingAudioBitsPerSample() / 8u))) { mWriterFailureReason = "could not write the AVI header"; return false; }
			for (int index = 0; index < 4; ++index) if (!WriteU16(0)) { mWriterFailureReason = "could not write the AVI header"; return false; }
			if (!WriteFourCC("strf") || !WriteU32(16) || !WriteU16(1) ||
				!WriteU16(I_GetVideoRecordingAudioChannels()) || !WriteU32(I_GetVideoRecordingAudioSampleRate()) ||
				!WriteU32((uint32_t)audioBytesPerSecond) ||
				!WriteU16(I_GetVideoRecordingAudioChannels() * (I_GetVideoRecordingAudioBitsPerSample() / 8u)) ||
				!WriteU16(I_GetVideoRecordingAudioBitsPerSample())) { mWriterFailureReason = "could not write the AVI header"; return false; }
			const ptrdiff_t afterAudioStrl = mFile->Tell();
			if (afterAudioStrl < audioStrlSizePosition + 4 || !PatchU32(audioStrlSizePosition, (uint32_t)(afterAudioStrl - audioStrlSizePosition - 4))) { mWriterFailureReason = "could not write the AVI header"; return false; }
			const ptrdiff_t afterHdrl = mFile->Tell();
			if (afterHdrl < hdrlSizePosition + 4 || !PatchU32(hdrlSizePosition, (uint32_t)(afterHdrl - hdrlSizePosition - 4))) { mWriterFailureReason = "could not write the AVI header"; return false; }

			if (!WriteFourCC("LIST")) { mWriterFailureReason = "could not write the AVI header"; return false; }
			mMoviSizePosition = mFile->Tell();
			if (!WriteU32(0) || !WriteFourCC("movi")) { mWriterFailureReason = "could not write the AVI header"; return false; }
			mMoviDataStart = mFile->Tell();
			return mMoviDataStart >= 0;
		}

			uint64_t AviSlotForTime(uint64_t timeNS) const
			{
				if (mAviTimelineOriginNS == 0 || timeNS <= mAviTimelineOriginNS)
				{
					return 0;
				}
				const uint64_t elapsedNS = timeNS - mAviTimelineOriginNS;
				const uint64_t seconds = elapsedNS / 1000000000ull;
				const uint64_t remainderNS = elapsedNS % 1000000000ull;
				const uint64_t maximum = std::numeric_limits<uint64_t>::max();
				if (seconds > maximum / (uint64_t)mFrameRate)
				{
					return maximum;
				}
				return seconds * (uint64_t)mFrameRate +
					(remainderNS * (uint64_t)mFrameRate) / 1000000000ull;
			}

			uint64_t AviSlotCountAt(uint64_t timeNS) const
			{
				if (mAviTimelineOriginNS == 0 || timeNS <= mAviTimelineOriginNS)
				{
					return 0;
				}
				const uint64_t elapsedNS = timeNS - mAviTimelineOriginNS;
				const uint64_t seconds = elapsedNS / 1000000000ull;
				const uint64_t remainderNS = elapsedNS % 1000000000ull;
				const uint64_t maximum = std::numeric_limits<uint64_t>::max();
				if (seconds > maximum / (uint64_t)mFrameRate)
				{
					return maximum;
				}
				uint64_t slots = seconds * (uint64_t)mFrameRate;
				const uint64_t partialSlots = remainderNS * (uint64_t)mFrameRate;
				slots += partialSlots / 1000000000ull;
				if (partialSlots % 1000000000ull != 0 && slots != maximum)
				{
					++slots;
				}
				return slots;
			}

			uint64_t AviTimeForSlot(uint64_t slot) const
			{
				if (mAviTimelineOriginNS == 0)
				{
					return 0;
				}
				const uint64_t seconds = slot / (uint64_t)mFrameRate;
				const uint64_t remainder = slot % (uint64_t)mFrameRate;
				if (seconds > (std::numeric_limits<uint64_t>::max() - mAviTimelineOriginNS) / 1000000000ull)
				{
					return std::numeric_limits<uint64_t>::max();
				}
				const uint64_t base = mAviTimelineOriginNS + seconds * 1000000000ull;
				const uint64_t partial = remainder * 1000000000ull / (uint64_t)mFrameRate;
				return base > std::numeric_limits<uint64_t>::max() - partial ?
					std::numeric_limits<uint64_t>::max() : base + partial;
			}

			uint64_t AudioFrameForTime(uint64_t timeNS) const
			{
				if (mTakeStartTimeNS == 0 || timeNS <= mTakeStartTimeNS)
				{
					return 0;
				}
				const uint64_t elapsed = timeNS - mTakeStartTimeNS;
				const uint64_t rate = I_GetVideoRecordingAudioSampleRate();
				return elapsed > std::numeric_limits<uint64_t>::max() / rate ?
					std::numeric_limits<uint64_t>::max() : elapsed * rate / 1000000000ull;
			}

			uint64_t AviAudioFramesForPart(uint64_t videoFrames = std::numeric_limits<uint64_t>::max()) const
			{
				if (videoFrames == std::numeric_limits<uint64_t>::max())
				{
					videoFrames = mAviPartFrames;
				}
				const uint64_t endSlot = mAviPartFirstSlot > std::numeric_limits<uint64_t>::max() - videoFrames ?
					std::numeric_limits<uint64_t>::max() : mAviPartFirstSlot + videoFrames;
				const uint64_t start = AudioFrameForTime(AviTimeForSlot(mAviPartFirstSlot));
				const uint64_t end = AudioFrameForTime(AviTimeForSlot(endSlot));
				return end > start ? end - start : 0;
			}

			uint64_t AviAudioReservationBytes(uint64_t videoFrames) const
			{
				const uint64_t frames = AviAudioFramesForPart(videoFrames);
				const uint64_t blockAlign = (uint64_t)I_GetVideoRecordingAudioChannels() *
					(I_GetVideoRecordingAudioBitsPerSample() / 8u);
				if (frames > std::numeric_limits<uint64_t>::max() / blockAlign)
				{
					return std::numeric_limits<uint64_t>::max();
				}
				const uint64_t blocks = (frames + 4095ull) / 4096ull;
				const uint64_t payload = frames * blockAlign;
				return payload > std::numeric_limits<uint64_t>::max() - blocks * 8ull ?
					std::numeric_limits<uint64_t>::max() : payload + blocks * 8ull;
			}

			void AddAviIndex(const char *chunkID, uint32_t flags, uint32_t offset, uint32_t size)
			{
				FAviIndexEntry entry{};
				memcpy(entry.ChunkID, chunkID, sizeof(entry.ChunkID));
				entry.Flags = flags;
				entry.Offset = offset;
				entry.Size = size;
				mAviIndex.Push(entry);
			}

			bool WriteAviAudioChunk(const int16_t *samples, size_t frames)
			{
				const uint64_t blockAlign = (uint64_t)I_GetVideoRecordingAudioChannels() *
					(I_GetVideoRecordingAudioBitsPerSample() / 8u);
				if (samples == nullptr || frames == 0 || frames > 0xffffffffull / blockAlign)
				{
					mWriterFailureReason = "the AVI audio block is invalid";
					return false;
				}
				const uint32_t payloadBytes = (uint32_t)(frames * blockAlign);
				const uint64_t chunkBytes = 8ull + payloadBytes + (payloadBytes & 1u);
				const uint64_t indexBytes = ((uint64_t)mAviIndex.Size() + 1ull) * 16ull;
				if (indexBytes > AVI_INDEX_MAX_BYTES || indexBytes > AVI_PART_LIMIT - VIDEO_AVI_FINALIZATION_RESERVE ||
					chunkBytes > AVI_PART_LIMIT - VIDEO_AVI_FINALIZATION_RESERVE - indexBytes ||
					mAviPayloadBytes > AVI_PART_LIMIT - VIDEO_AVI_FINALIZATION_RESERVE - indexBytes - chunkBytes ||
					!RequireOutputSpace(chunkBytes + indexBytes + VIDEO_AVI_FINALIZATION_RESERVE))
				{
					if (mWriterFailureReason.IsEmpty()) mWriterFailureReason = "could not reserve space for AVI audio";
					return false;
				}
				const ptrdiff_t chunkStart = mFile != nullptr ? mFile->Tell() : -1;
				const ptrdiff_t moviTypeStart = mMoviDataStart - 4;
				if (chunkStart < mMoviDataStart || moviTypeStart < 0 || (uint64_t)(chunkStart - moviTypeStart) > 0xffffffffull ||
					!WriteFourCC("01wb") || !WriteU32(payloadBytes) || !WriteBytes(samples, payloadBytes))
				{
					mWriterFailureReason = "could not write AVI audio";
					return false;
				}
				if ((payloadBytes & 1u) != 0)
				{
					const uint8_t padding = 0;
					if (!WriteBytes(&padding, 1))
					{
						mWriterFailureReason = "could not pad AVI audio";
						return false;
					}
				}
				AddAviIndex("01wb", 0, (uint32_t)(chunkStart - moviTypeStart), payloadBytes);
				mAviPayloadBytes += chunkBytes;
				mAviPartAudioFrames += frames;
				return true;
			}

			struct FAviAudioWriteContext
			{
				FVideoRecorder *Recorder = nullptr;
				uint64_t Frames = 0;
			};

			static bool AviAudioSink(const int16_t *samples, size_t frames, void *userdata)
			{
				auto *context = static_cast<FAviAudioWriteContext *>(userdata);
				if (context == nullptr || context->Recorder == nullptr || !context->Recorder->WriteAviAudioChunk(samples, frames))
				{
					return false;
				}
				context->Frames += frames;
				return true;
			}

			bool WriteAviSilence(uint64_t frames)
			{
				int16_t silence[4096 * 2]{};
				while (frames != 0)
				{
					const size_t count = (size_t)std::min<uint64_t>(frames, 4096);
					if (!WriteAviAudioChunk(silence, count))
					{
						return false;
					}
					frames -= count;
				}
				return true;
			}

			bool WriteAviAudioForPart()
			{
				const uint64_t endSlot = mAviPartFirstSlot > std::numeric_limits<uint64_t>::max() - mAviPartFrames ?
					std::numeric_limits<uint64_t>::max() : mAviPartFirstSlot + mAviPartFrames;
				const uint64_t partStartNS = AviTimeForSlot(mAviPartFirstSlot);
				const uint64_t partEndNS = AviTimeForSlot(endSlot);
				const uint64_t targetFrames = AviAudioFramesForPart();
				const uint64_t stopTimeNS = GetStopTimeNS();
				const uint64_t contentEndNS = stopTimeNS != 0 ? std::min(partEndNS, stopTimeNS) : partEndNS;
				FAviAudioWriteContext context{ this };
				if (contentEndNS > partStartNS)
				{
					FString error;
					if (!I_RenderVideoRecordingAudio(partStartNS, contentEndNS, AviAudioSink, &context, error))
					{
						mWriterFailureReason = error.IsEmpty() ? "could not mix AVI audio" : error;
						return false;
					}
				}
				if (context.Frames > targetFrames)
				{
					mWriterFailureReason = "the AVI audio timeline exceeds its video part";
					return false;
				}
				return WriteAviSilence(targetFrames - context.Frames);
			}

			bool WriteAviPackedFrame()
			{
				const uint64_t chunkBytes = 8ull + mAviFrameBytes + (mAviFrameBytes & 1u);
				const uint64_t candidateFrames = mAviPartFrames == std::numeric_limits<uint64_t>::max() ?
					mAviPartFrames : (uint64_t)mAviPartFrames + 1ull;
				const uint64_t audioFrames = AviAudioFramesForPart(candidateFrames);
				const uint64_t audioReservation = AviAudioReservationBytes(candidateFrames);
				const uint64_t audioChunks = (audioFrames + 4095ull) / 4096ull;
				const uint64_t indexBytes = ((uint64_t)mAviIndex.Size() + 1ull + audioChunks) * 16ull;
				if (mAviPartFrames != 0 &&
					(audioReservation == std::numeric_limits<uint64_t>::max() || indexBytes > AVI_INDEX_MAX_BYTES ||
						audioReservation > AVI_PART_LIMIT - VIDEO_AVI_FINALIZATION_RESERVE - indexBytes ||
						chunkBytes > AVI_PART_LIMIT - VIDEO_AVI_FINALIZATION_RESERVE - indexBytes - audioReservation ||
						mAviPayloadBytes > AVI_PART_LIMIT - VIDEO_AVI_FINALIZATION_RESERVE - indexBytes - audioReservation - chunkBytes))
				{
					// A file-size rollover does not change the global timeline or the
					// packed hold image. The successor immediately writes the same slot
					// or the next one at the selected fixed cadence.
					if (!FinishAviPart())
					{
						DiscardAviPart();
						if (mWriterFailureReason.IsEmpty()) mWriterFailureReason = "could not finalize an AVI part";
						return false;
					}
					++mPart;
					if (!OpenAviPart())
					{
						return false;
					}
				}
				const uint64_t nextAudioFrames = AviAudioFramesForPart((uint64_t)mAviPartFrames + 1ull);
				const uint64_t nextAudioChunks = (nextAudioFrames + 4095ull) / 4096ull;
				const uint64_t nextAudioReservation = AviAudioReservationBytes((uint64_t)mAviPartFrames + 1ull);
				const uint64_t finalizationBytes = ((uint64_t)mAviIndex.Size() + 1ull + nextAudioChunks) * 16ull + VIDEO_AVI_FINALIZATION_RESERVE;
				if (nextAudioReservation == std::numeric_limits<uint64_t>::max() ||
					finalizationBytes > std::numeric_limits<uint64_t>::max() - chunkBytes - nextAudioReservation ||
					!RequireOutputSpace(chunkBytes + finalizationBytes + nextAudioReservation))
				{
					return false;
				}

				const ptrdiff_t chunkStart = mFile->Tell();
				// AVI idx1 offsets are relative to the `movi` list type (not its
				// first payload byte), so a first frame begins at offset four. This
				// convention is required by strict AVI readers such as ffmpeg.
				const ptrdiff_t moviTypeStart = mMoviDataStart - 4;
				if (moviTypeStart < 0 || chunkStart < mMoviDataStart || (uint64_t)(chunkStart - moviTypeStart) > 0xffffffffull ||
					!WriteFourCC("00db") || !WriteU32(mAviFrameBytes) || !WriteBytes(mBgrFrame.Data(), mAviFrameBytes))
				{
					mWriterFailureReason = "could not write an AVI frame";
					return false;
				}
				if ((mAviFrameBytes & 1u) != 0)
				{
					const uint8_t padding = 0;
					if (!WriteBytes(&padding, 1))
					{
						mWriterFailureReason = "could not pad an AVI frame";
						return false;
					}
				}
				AddAviIndex("00db", 0x10, (uint32_t)(chunkStart - moviTypeStart), mAviFrameBytes);
				mAviPayloadBytes += chunkBytes;
				++mAviPartFrames;
				return true;
			}

			bool WriteAviPackedSlot(bool duplicated)
			{
				if (mAviNextSlot == std::numeric_limits<uint64_t>::max())
				{
					mWriterFailureReason = "the AVI capture timeline is too long";
					return false;
				}
				if (!WriteAviPackedFrame())
				{
					return false;
				}
				if (duplicated)
				{
					++mAviDuplicatedFrames;
				}
				++mAviNextSlot;
				return true;
			}

			bool FillAviSlotsUntil(uint64_t endExclusive)
			{
				// AVI represents its selected-rate timeline as complete RGB slots.
				// When a native sample is late, hold the last finished image on the
				// writer thread instead of shortening the recording. This keeps the
				// video and PCM duration faithful to the take while the bounded CPU
				// queue continues to drop live samples under pressure. Each duplicate
				// is independently guarded by the normal session/free-space budget;
				// no duplicate image is retained in the producer or GPU queues.
				while (mAviNextSlot < endExclusive)
				{
					if (!WriteAviPackedSlot(true))
					{
						return false;
					}
				}
				return true;
			}

			bool PackAviFrame(const FQueuedVideoFrame &frame)
			{
				const int bytesPerPixel = VideoBytesPerPixel(frame.ColorType);
				const uint64_t minimumBytes = bytesPerPixel > 0 && frame.Pitch >= mWidth * bytesPerPixel ?
					(uint64_t)frame.Pitch * (uint64_t)mHeight : 0;
				if (mFile == nullptr || bytesPerPixel == 0 || minimumBytes == 0 ||
					minimumBytes > frame.Pixels.Size() || mBgrFrame.Size() != mAviFrameBytes)
				{
					mWriterFailureReason = "the AVI frame data was incomplete";
					return false;
				}
				for (int y = 0; y < mHeight; ++y)
				{
					const int sourceY = frame.BottomUp ? y : mHeight - y - 1;
					const uint8_t *source = frame.Pixels.Data() + (size_t)sourceY * (size_t)frame.Pitch;
					uint8_t *destination = mBgrFrame.Data() + (size_t)y * mAviStride;
					for (int x = 0; x < mWidth; ++x)
					{
						if (frame.ColorType == SS_RGB)
						{
							destination[x * 3 + 0] = source[x * 3 + 2];
							destination[x * 3 + 1] = source[x * 3 + 1];
							destination[x * 3 + 2] = source[x * 3 + 0];
						}
						else if (frame.ColorType == SS_RGBA)
						{
							destination[x * 3 + 0] = source[x * 4 + 2];
							destination[x * 3 + 1] = source[x * 4 + 1];
							destination[x * 3 + 2] = source[x * 4];
						}
						else // SS_BGRA
						{
							destination[x * 3 + 0] = source[x * 4];
							destination[x * 3 + 1] = source[x * 4 + 1];
							destination[x * 3 + 2] = source[x * 4 + 2];
						}
					}
					if (mAviStride > (uint32_t)mWidth * 3)
					{
						memset(destination + (size_t)mWidth * 3, 0, mAviStride - (uint32_t)mWidth * 3);
					}
				}
				return true;
			}

			bool WriteAviFrame(const FQueuedVideoFrame &frame)
			{
				// The first actual composited image, not the command time, defines slot
				// zero. An asynchronous PBO can legitimately deliver its first image
				// tens of milliseconds after Start(); charging that startup latency to
				// the selected-rate timeline would either write misleading pre-image
				// duplicates or shorten a healthy cold start before it emits one frame.
				if (mAviTimelineOriginNS == 0)
				{
					mAviTimelineOriginNS = frame.CaptureTimeNS != 0 ? frame.CaptureTimeNS : mTakeStartTimeNS;
				}
				const uint64_t slot = AviSlotForTime(frame.CaptureTimeNS);
				if (mAviHavePackedFrame && slot > mAviNextSlot && !FillAviSlotsUntil(slot))
				{
					return false;
				}
				if (!PackAviFrame(frame))
				{
					return false;
				}
				if (!mAviHavePackedFrame)
				{
					// The origin above makes the first real image slot zero. Do not invent
					// leading duplicates: audio and video both begin at this composited
					// frame, while later gaps remain governed by output-space limits.
					mAviHavePackedFrame = true;
				}
				if (slot >= mAviNextSlot)
				{
					return WriteAviPackedSlot(false);
				}
				// Multiple native samples can land in one selected-rate slot. The
				// latest packed image becomes the hold source for the following slot;
				// no extra AVI frame is emitted above the selected cadence.
				return true;
			}

			uint32_t AviPartMicrosecondsPerFrame(uint64_t requestedEndTimeNS = 0) const
			{
				(void)requestedEndTimeNS;
				return (uint32_t)((1000000ull + (uint64_t)mFrameRate / 2ull) / (uint64_t)mFrameRate);
			}

			bool FinishAviPart(uint64_t requestedEndTimeNS = 0)
			{
				if (mFile == nullptr)
				{
					return true;
				}
				// AVI is a selected-rate stream. Before an explicit resolution or
				// take-end boundary, extend the last packed image through every
				// missing wall-clock slot. This happens exclusively on the writer
				// thread, so it cannot create render-thread catch-up work or queue
				// raw GPU images in memory.
				if (requestedEndTimeNS != 0 && mAviHavePackedFrame &&
					!FillAviSlotsUntil(AviSlotCountAt(requestedEndTimeNS)))
				{
					return false;
				}
				if (mAviPartFrames == 0)
			{
				const bool closed = CloseAviFile();
				RemoveFile(mCurrentFile.GetChars());
				if (!closed)
				{
					mWriterFailureReason = "could not close the empty AVI output file";
				}
				return closed;
			}
			if (!WriteAviAudioForPart())
			{
				return false;
			}
			const uint64_t indexBytes = (uint64_t)mAviIndex.Size() * 16ull;
			if (indexBytes > 0xffffffffull || !RequireOutputSpace(indexBytes + VIDEO_AVI_FINALIZATION_RESERVE))
			{
				if (mWriterFailureReason.IsEmpty()) mWriterFailureReason = "could not reserve space to finalize the AVI output file";
				return false;
			}

			const ptrdiff_t moviEnd = mFile->Tell();
			if (moviEnd < mMoviSizePosition + 4 || !PatchU32(mMoviSizePosition, (uint32_t)(moviEnd - mMoviSizePosition - 4)) ||
				!WriteFourCC("idx1") || !WriteU32((uint32_t)indexBytes))
			{
				mWriterFailureReason = "could not finalize the AVI output file";
				return false;
			}
			for (const auto &entry : mAviIndex)
			{
				if (!WriteBytes(entry.ChunkID, sizeof(entry.ChunkID)) || !WriteU32(entry.Flags) ||
					!WriteU32(entry.Offset) || !WriteU32(entry.Size))
				{
					mWriterFailureReason = "could not finalize the AVI output file";
					return false;
				}
			}
			const ptrdiff_t fileEnd = mFile->Tell();
				const uint32_t microsecondsPerFrame = AviPartMicrosecondsPerFrame();
				const uint64_t audioBytesPerSecond = (uint64_t)I_GetVideoRecordingAudioSampleRate() *
					I_GetVideoRecordingAudioChannels() * (I_GetVideoRecordingAudioBitsPerSample() / 8u);
					const uint64_t maximumBytesPerSecond = (uint64_t)mAviFrameBytes * (uint64_t)mFrameRate + audioBytesPerSecond;
			const uint32_t bytesPerSecond = maximumBytesPerSecond > 0xffffffffull ?
				0xffffffffu : (uint32_t)maximumBytesPerSecond;
			if (fileEnd < 8 || !PatchU32(mAvihMicrosecondsPerFramePosition, microsecondsPerFrame) ||
				!PatchU32(mAvihMaxBytesPerSecondPosition, bytesPerSecond) ||
					!PatchU32(mAvihFrameCountPosition, mAviPartFrames) ||
					!PatchU32(mStrhScalePosition, 1u) ||
					!PatchU32(mStrhRatePosition, (uint32_t)mFrameRate) ||
					!PatchU32(mStrhFrameCountPosition, mAviPartFrames) ||
					mAviPartAudioFrames > 0xffffffffull || !PatchU32(mStrhAudioFrameCountPosition, (uint32_t)mAviPartAudioFrames) ||
				!PatchU32(mRiffSizePosition, (uint32_t)(fileEnd - 8)))
			{
				mWriterFailureReason = "could not finalize the AVI output file";
				return false;
			}
			if (!CloseAviFile())
			{
				RemoveFile(mCurrentFile.GetChars());
				mWriterFailureReason = "could not close the AVI output file";
				return false;
			}
			mFinalizedAviFrames += mAviPartFrames;
			++mFinalizedAviParts;
			return true;
		}

		void DiscardAviPart()
		{
			if (mFile != nullptr)
			{
				delete mFile;
				mFile = nullptr;
				if (mCurrentFile.IsNotEmpty()) RemoveFile(mCurrentFile.GetChars());
			}
		}

		bool CloseAviFile()
		{
			if (mFile == nullptr)
			{
				return true;
			}
			const bool closed = mFile->CloseChecked();
			delete mFile;
			mFile = nullptr;
			return closed;
		}

		bool CanQueueFrame(uint64_t bytes) const
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			if (mStopRequested || mWriterAborting || mWriterFinished || mOutstandingFrames >= VIDEO_QUEUE_MAX_FRAMES)
			{
				return false;
			}
			if (bytes > VIDEO_QUEUE_MAX_BYTES && mOutstandingFrames != 0)
			{
				return false;
			}
			const uint64_t limit = bytes > VIDEO_QUEUE_MAX_BYTES ? VIDEO_MAX_SINGLE_FRAME_BYTES : VIDEO_QUEUE_MAX_BYTES;
			return bytes <= limit && mQueuedBytes <= limit - bytes;
		}

		bool QueueFrame(FQueuedVideoFrame &&frame)
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			const uint64_t bytes = frame.StorageBytes;
			if (mStopRequested || mWriterAborting || mWriterFinished || mOutstandingFrames >= VIDEO_QUEUE_MAX_FRAMES ||
				(bytes > VIDEO_QUEUE_MAX_BYTES && mOutstandingFrames != 0))
			{
				return false;
			}
			const uint64_t limit = bytes > VIDEO_QUEUE_MAX_BYTES ? VIDEO_MAX_SINGLE_FRAME_BYTES : VIDEO_QUEUE_MAX_BYTES;
			if (bytes > limit || mQueuedBytes > limit - bytes)
			{
				return false;
			}
			// deque growth is the one allocation on the render-thread admission
			// path. Do not account for the slot until it exists: an allocation
			// failure must degrade to a dropped frame, never escape the render loop
			// or leave the bounded queue counters permanently inflated.
			try
			{
				mQueue.emplace_back(std::move(frame));
			}
			catch (...)
			{
				return false;
			}
			mQueuedBytes += bytes;
			++mOutstandingFrames;
			mQueueWake.notify_one();
			return true;
		}

			void NoteDroppedFrame()
			{
				++mDroppedFrames;
				if (!mDropNoticeShown)
				{
					Printf("Video capture queue is full; skipping frames to keep the game responsive.\n");
					mDropNoticeShown = true;
				}
			}

			void NoteReadbackDeferredFrame()
			{
				++mReadbackDeferredFrames;
				if (!mReadbackNoticeShown)
				{
					Printf("Video capture GPU readback is pending; preserving game responsiveness.\n");
					mReadbackNoticeShown = true;
				}
			}

		bool WriterNeedsStop() const
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			return mWriterFinished && mWriterResult != VIDEO_WRITER_COMPLETE;
		}

		bool StopRequested() const
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			return mStopRequested;
		}

		uint64_t GetStopTimeNS() const
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			return mStopTimeNS;
		}

			uint64_t RequestWriterStop(bool drain, uint64_t requestedAtNS = 0)
			{
				if (requestedAtNS == 0)
				{
					requestedAtNS = CaptureWallClockNS();
				}
			if (!mWriterThread.joinable())
			{
				return requestedAtNS;
			}
			uint64_t stopTimeNS = requestedAtNS;
			{
				std::lock_guard<std::mutex> lock(mQueueMutex);
				if (!mStopRequested)
				{
					mStopTimeNS = requestedAtNS;
				}
				else if (mStopTimeNS != 0)
				{
					stopTimeNS = mStopTimeNS;
				}
				mStopRequested = true;
				if (!drain)
				{
					ClearQueuedFramesLocked();
				}
			}
				mQueueWake.notify_one();
			return stopTimeNS;
		}

		bool JoinWriterIfFinished()
		{
			if (!mWriterThread.joinable())
			{
				return true;
			}
			{
				std::lock_guard<std::mutex> lock(mQueueMutex);
				if (!mWriterFinished)
				{
					return false;
				}
			}
			mWriterThread.join();
			return true;
		}

		void JoinWriter()
		{
			if (mWriterThread.joinable())
			{
				mWriterThread.join();
			}
		}

		void StopWriter(bool drain)
		{
			RequestWriterStop(drain);
			JoinWriter();
		}

		void ClearQueuedFrames()
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			ClearQueuedFramesLocked();
		}

		void ClearQueuedFramesLocked()
		{
			for (const auto &frame : mQueue)
			{
				mQueuedBytes -= frame.StorageBytes;
			}
			mOutstandingFrames -= (unsigned int)mQueue.size();
			mQueue.clear();
		}

		void ReleaseFrameSlot(uint64_t storageBytes)
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			mQueuedBytes -= storageBytes;
			--mOutstandingFrames;
		}

		void AbortFrameAdmissionForWriterFailure()
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			mWriterAborting = true;
			ClearQueuedFramesLocked();
		}

		void PublishWriterResult(EVideoWriterResult result, const FString &message)
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			mWriterResult = result;
			mWriterResultMessage = message;
			mWriterFinished = true;
		}

		EVideoWriterResult WriteQueuedFramesSerial()
		{
			EVideoWriterResult result = VIDEO_WRITER_COMPLETE;
			while (true)
			{
				FQueuedVideoFrame frame;
				{
					std::unique_lock<std::mutex> lock(mQueueMutex);
					mQueueWake.wait(lock, [this]() { return mStopRequested || !mQueue.empty(); });
					if (mQueue.empty())
					{
						if (mStopRequested) break;
						continue;
					}
					frame = std::move(mQueue.front());
					mQueue.pop_front();
				}
				bool written = false;
				try
				{
					written = WriteFrame(frame);
				}
				catch (...)
				{
					// Keep a bad allocator/encoder path contained on the writer
					// thread. The current slot must still be released exactly once
					// before the remaining bounded queue is discarded.
					mWriterFailureReason = "could not write a video frame";
					written = false;
				}
				ReleaseFrameSlot(frame.StorageBytes);
				if (!written)
				{
					result = mWriterSpaceLimited ? VIDEO_WRITER_SPACE_LIMIT : VIDEO_WRITER_FAILURE;
					AbortFrameAdmissionForWriterFailure();
					break;
				}
			}
			return result;
		}

		bool EnsurePngOutputForFrame(int width, int height, uint64_t changeTimeNS)
		{
			if (!mOutputOpen)
			{
				mWidth = width;
				mHeight = height;
				mOutputOpen = true;
				return true;
			}
			if (mWidth == width && mHeight == height)
			{
				return true;
			}
			// The parallel coordinator calls this only after every task from the
			// old size has been committed. That makes the part boundary and its
			// timing sidecar unambiguous even when encoders completed out of order.
			if (!FinishPngTimeline(changeTimeNS))
			{
				return false;
			}
			++mPart;
			mWidth = width;
			mHeight = height;
			return true;
		}

		EVideoWriterResult WritePngFramesParallel()
		{
			const unsigned int workerCount = VideoPngEncoderWorkerCount();
			if (workerCount <= 1)
			{
				// Keep the established one-worker path on small or unknown machines.
				return WriteQueuedFramesSerial();
			}

			FPngWorkerState state;
			std::condition_variable workerWake;
			std::vector<std::thread> workers;
			auto workerMain = [this, &state, &workerWake]() noexcept
			{
				// A completion slot is preallocated for every possible outstanding
				// frame. Publishing therefore needs no heap allocation after a worker
				// accepts a task, including while reporting an allocation failure.
				auto publishCompletion = [this, &state, &workerWake](FPngEncodeResult &&completed)
				{
					std::unique_lock<std::mutex> lock(mQueueMutex);
					// The global outstanding-frame cap means this cannot normally
					// wait: a worker still counted in EncodingFrames leaves at
					// least one of the three completion slots free. The wait keeps
					// that invariant defensive without allocating or dropping a
					// finished task if future changes weaken the arithmetic.
					workerWake.wait(lock, [&state]()
					{
						return state.CompletedCount < state.Completed.size();
					});
					state.Completed[state.CompletedCount++] = std::move(completed);
					--state.EncodingFrames;
					mQueueWake.notify_one();
				};

				auto publishFailedTask = [this, &state](FPngEncodeResult &&failed) noexcept
				{
					// A worker that still owns a task also still contributes one to
					// EncodingFrames. With the global three-frame limit that proves a
					// completion slot is free, so this emergency path never waits or
					// allocates while recovering from another exception.
					{
						std::lock_guard<std::mutex> lock(mQueueMutex);
						assert(state.EncodingFrames != 0);
						assert(state.CompletedCount < state.Completed.size());
						state.Completed[state.CompletedCount++] = std::move(failed);
						--state.EncodingFrames;
					}
					mQueueWake.notify_one();
				};

				auto markWorkerFailure = [this, &state, &workerWake]() noexcept
				{
					{
						std::lock_guard<std::mutex> lock(mQueueMutex);
						state.WorkerFailed = true;
						state.StopWorkers = true;
					}
					workerWake.notify_all();
					mQueueWake.notify_all();
				};

				FPngEncodeTask task;
				bool taskInFlight = false;
				try
				{
					FPNGEncoder encoder;
					while (true)
					{
						bool cancelled = false;
						{
							std::unique_lock<std::mutex> lock(mQueueMutex);
							workerWake.wait(lock, [&state]() { return state.StopWorkers || state.PendingCount != 0; });
							if (state.PendingCount == 0)
							{
								if (state.StopWorkers) break;
								continue;
							}
							task = std::move(state.Pending[--state.PendingCount]);
							taskInFlight = true;
							cancelled = state.StopWorkers;
						}

						FPngEncodeResult completed;
						if (cancelled)
						{
							// The coordinator failed after dispatching this bounded task.
							// Publish it as a cancelled result so the post-join cleanup owns
							// its one reservation and raw-frame release without doing more I/O.
							completed.Filename = std::move(task.Filename);
							completed.FrameNumber = task.FrameNumber;
							completed.CaptureTimeNS = task.Frame.CaptureTimeNS;
							completed.StorageBytes = task.Frame.StorageBytes;
							completed.EstimateBytes = task.EstimateBytes;
							completed.ReservationBytes = task.ReservationBytes;
							completed.StaticError = "PNG recording stopped before encoding a queued frame";
						}
						else
						{
							try
							{
								completed = EncodePngTask(std::move(task), mPngCompressionLevel, mPngGammaOverride, encoder);
							}
							catch (...)
							{
								// Do not allocate while recovering from an encoder failure.
								// The fixed completion slot lets the coordinator release this
								// task's exact reservation and raw-frame accounting.
								const bool removeOutput = task.OutputOpened;
								task.OutputOpened = false;
								completed.Filename = std::move(task.Filename);
								completed.FrameNumber = task.FrameNumber;
								completed.CaptureTimeNS = task.Frame.CaptureTimeNS;
								completed.StorageBytes = task.Frame.StorageBytes;
								completed.EstimateBytes = task.EstimateBytes;
								completed.ReservationBytes = task.ReservationBytes;
								completed.StaticError = "could not write a PNG frame";
								if (removeOutput && completed.Filename.IsNotEmpty()) RemoveFile(completed.Filename.GetChars());
							}
						}
						publishCompletion(std::move(completed));
						taskInFlight = false;
						task = FPngEncodeTask();
					}
					encoder.Reset();
				}
				catch (...)
				{
					if (taskInFlight)
					{
						// Keep the final guard outside the per-encode handler: it also
						// contains unexpected encoder construction/teardown failures and
						// any future worker-side operation. This path uses only moves,
						// scalars, and a fixed completion slot.
						FPngEncodeResult failed;
						const bool removeOutput = task.OutputOpened;
						task.OutputOpened = false;
						failed.Filename = std::move(task.Filename);
						failed.FrameNumber = task.FrameNumber;
						failed.CaptureTimeNS = task.Frame.CaptureTimeNS;
						failed.StorageBytes = task.Frame.StorageBytes;
						failed.EstimateBytes = task.EstimateBytes;
						failed.ReservationBytes = task.ReservationBytes;
						failed.StaticError = "PNG encoder worker stopped unexpectedly";
						if (removeOutput && failed.Filename.IsNotEmpty()) RemoveFile(failed.Filename.GetChars());
						publishFailedTask(std::move(failed));
					}
					// If no task was active, a fixed failure flag still wakes the
					// coordinator so it can stop admission and clean up its bounded
					// pending list rather than waiting forever for this worker.
					markWorkerFailure();
				}
			};

			try
			{
				for (unsigned int index = 0; index < workerCount; ++index)
				{
					workers.emplace_back(workerMain);
				}
			}
			catch (...)
			{
				{
					std::lock_guard<std::mutex> lock(mQueueMutex);
					state.StopWorkers = true;
				}
				workerWake.notify_all();
				for (auto &worker : workers)
				{
					if (worker.joinable()) worker.join();
				}
				{
					std::lock_guard<std::mutex> lock(mQueueMutex);
					mWriterPngPoolFallback = true;
				}
				return WriteQueuedFramesSerial();
			}

			uint64_t nextFrameNumber = 1;
			uint64_t nextCommitNumber = 1;
			uint64_t firstFailedFrame = 0;
			bool terminalFailure = false;
			EVideoWriterResult result = VIDEO_WRITER_COMPLETE;

			auto stopForFailure = [&](EVideoWriterResult failure, uint64_t failedFrame, const FString &reason)
			{
				// A coordinator-side failure happens before the current queue head
				// is dispatched. Its cutoff is therefore the next globally issued
				// ordinal: all lower tasks still form a valid, ordered prefix and
				// must drain; the head and every later frame are abandoned.
				const uint64_t failureCutoff = failedFrame != 0 ? failedFrame : nextFrameNumber;
				if (!terminalFailure || failureCutoff < firstFailedFrame)
				{
					terminalFailure = true;
					firstFailedFrame = failureCutoff;
					result = failure;
					if (!reason.IsEmpty()) mWriterFailureReason = reason;
				}
				AbortFrameAdmissionForWriterFailure();
			};

			auto takeCompleted = [&](uint64_t frameNumber, FPngEncodeResult &completed)
			{
				bool taken = false;
				{
					std::lock_guard<std::mutex> lock(mQueueMutex);
					for (unsigned int index = 0; index < state.CompletedCount; ++index)
					{
						if (state.Completed[index].FrameNumber != frameNumber) continue;
						completed = std::move(state.Completed[index]);
						--state.CompletedCount;
						if (index != state.CompletedCount)
						{
							state.Completed[index] = std::move(state.Completed[state.CompletedCount]);
						}
						taken = true;
						break;
					}
				}
				if (taken)
				{
					workerWake.notify_one();
				}
				return taken;
			};

			auto takeCompletedAtOrAfter = [&](uint64_t frameNumber, FPngEncodeResult &completed)
			{
				bool taken = false;
				{
					std::lock_guard<std::mutex> lock(mQueueMutex);
					for (unsigned int index = 0; index < state.CompletedCount; ++index)
					{
						if (state.Completed[index].FrameNumber < frameNumber) continue;
						completed = std::move(state.Completed[index]);
						--state.CompletedCount;
						if (index != state.CompletedCount)
						{
							state.Completed[index] = std::move(state.Completed[state.CompletedCount]);
						}
						taken = true;
						break;
					}
				}
				if (taken)
				{
					workerWake.notify_one();
				}
				return taken;
			};

			auto copyWorkerFailureReason = [](const FPngEncodeResult &completed, FString &reason)
			{
				reason = completed.Error;
				if (reason.IsEmpty() && completed.StaticError != nullptr)
				{
					// String ownership belongs to the coordinator. Workers carry a
					// static fallback so handling their own allocation failure never
					// performs another allocation on a background thread.
					reason = completed.StaticError;
				}
			};

			auto findWorkerFailure = [&](uint64_t &frameNumber, FString &reason)
			{
				std::lock_guard<std::mutex> lock(mQueueMutex);
				bool found = false;
				for (unsigned int index = 0; index < state.CompletedCount; ++index)
				{
					const auto &completed = state.Completed[index];
					if (!completed.Succeeded && (!found || completed.FrameNumber < frameNumber))
					{
						frameNumber = completed.FrameNumber;
						copyWorkerFailureReason(completed, reason);
						found = true;
					}
				}
				return found;
			};

			auto abandonPendingTasks = [&]()
			{
				std::array<FPngEncodeTask, VIDEO_PNG_MAX_ENCODER_WORKERS> abandoned;
				unsigned int abandonedCount = 0;
				{
					std::lock_guard<std::mutex> lock(mQueueMutex);
					while (state.PendingCount != 0)
					{
						abandoned[abandonedCount++] = std::move(state.Pending[--state.PendingCount]);
						--state.EncodingFrames;
					}
				}
				uint64_t firstAbandonedFrame = 0;
				for (unsigned int index = 0; index < abandonedCount; ++index)
				{
					if (firstAbandonedFrame == 0 || abandoned[index].FrameNumber < firstAbandonedFrame)
					{
						firstAbandonedFrame = abandoned[index].FrameNumber;
					}
					ReleasePngOutputReservation(abandoned[index].ReservationBytes);
					ReleaseFrameSlot(abandoned[index].Frame.StorageBytes);
				}
				return firstAbandonedFrame;
			};

			try
			{
				while (true)
				{
					uint64_t failedFrame = 0;
					FString failureReason;
					if (findWorkerFailure(failedFrame, failureReason))
					{
						stopForFailure(VIDEO_WRITER_FAILURE, failedFrame, failureReason);
					}
					bool workerFailed = false;
					{
						std::lock_guard<std::mutex> lock(mQueueMutex);
						workerFailed = state.WorkerFailed;
					}
					if (workerFailed)
					{
						// A worker may fail before it has claimed a pending task. Drain
						// those fixed slots now; otherwise their EncodingFrames count
						// would make the coordinator wait forever for a dead worker.
						const uint64_t firstAbandonedFrame = abandonPendingTasks();
						mWriterFailureReason = "a PNG encoder worker stopped unexpectedly";
						stopForFailure(VIDEO_WRITER_FAILURE, firstAbandonedFrame, mWriterFailureReason);
					}

				// Only the coordinator writes the timeline. A completed later image
				// stays accounted as an outstanding frame until every earlier ordinal
				// has committed, so ordering never needs an unbounded reorder buffer.
				while (!terminalFailure || nextCommitNumber < firstFailedFrame)
				{
					FPngEncodeResult completed;
					if (!takeCompleted(nextCommitNumber, completed)) break;
					bool reservationReleased = false;
					bool frameReleased = false;
					bool committed = false;
					auto releaseTakenCompletion = [&](bool removeOutput)
					{
						if (!reservationReleased)
						{
							ReleasePngOutputReservation(completed.ReservationBytes);
							reservationReleased = true;
						}
						if (removeOutput && completed.Succeeded && !committed)
						{
							RemoveFile(completed.Filename.GetChars());
						}
						if (!frameReleased)
						{
							ReleaseFrameSlot(completed.StorageBytes);
							frameReleased = true;
						}
					};
					try
					{
						if (!completed.Succeeded)
						{
							releaseTakenCompletion(false);
							FString completionReason;
							copyWorkerFailureReason(completed, completionReason);
							stopForFailure(VIDEO_WRITER_FAILURE, completed.FrameNumber, completionReason);
							++nextCommitNumber;
							break;
						}
						releaseTakenCompletion(false);
						mEstimatedOutputBytes += completed.EstimateBytes;
						if (!AppendPngTimelineFrame(completed.Filename, completed.CaptureTimeNS))
						{
							// The sidecar is the playback contract. Do not retain a PNG that
							// cannot be described by its ordered timeline.
							RemoveFile(completed.Filename.GetChars());
							stopForFailure(VIDEO_WRITER_FAILURE, completed.FrameNumber, mWriterFailureReason);
							++nextCommitNumber;
							break;
						}
						committed = true;
						++mTotalFrames;
						++nextCommitNumber;
					}
					catch (...)
					{
						// takeCompleted() moved this result out of the fixed completion
						// array. Keep it locally accounted if a timing-sidecar allocation
						// or another coordinator operation throws before the normal path
						// can release its reservation and raw-frame slot.
						releaseTakenCompletion(true);
						throw;
					}
				}

				// Once a task has failed, later completions cannot be referenced by
				// the ordered ffconcat prefix. Discard them as they arrive so they
				// free their raw-frame slots and cannot make terminal cleanup spin.
				// Earlier ordinals remain untouched and continue to commit above.
				if (terminalFailure)
				{
					FPngEncodeResult discarded;
					while (takeCompletedAtOrAfter(firstFailedFrame, discarded))
					{
						ReleasePngOutputReservation(discarded.ReservationBytes);
						if (discarded.Succeeded) RemoveFile(discarded.Filename.GetChars());
						ReleaseFrameSlot(discarded.StorageBytes);
					}
				}

				if (!terminalFailure)
				{
					while (true)
					{
						int width = 0;
						int height = 0;
						uint64_t captureTimeNS = 0;
						uint64_t outputBytes = 0;
						bool haveFrame = false;
						bool pipelineEmpty = false;
						{
							std::lock_guard<std::mutex> lock(mQueueMutex);
							if (!state.WorkerFailed && !mQueue.empty() && state.EncodingFrames < workerCount &&
								state.PendingCount < state.Pending.size())
							{
								const auto &frame = mQueue.front();
								width = frame.Width;
								height = frame.Height;
								captureTimeNS = frame.CaptureTimeNS;
								outputBytes = frame.OutputBytes;
								haveFrame = true;
								pipelineEmpty = state.EncodingFrames == 0 && state.CompletedCount == 0;
							}
						}
						if (!haveFrame) break;
						if (mOutputOpen && (mWidth != width || mHeight != height) && !pipelineEmpty)
						{
							// A part boundary cannot overtake a prior frame. Wait for the
							// bounded pipeline to commit, then retry this same queue head.
							break;
						}
						if (!EnsurePngOutputForFrame(width, height, captureTimeNS))
						{
							stopForFailure(VIDEO_WRITER_FAILURE, 0, mWriterFailureReason);
							break;
						}
						FString filename;
						filename.Format("%s_frame%06llu.png", SequenceStemForPart().GetChars(),
							(unsigned long long)nextFrameNumber);
						if (FileExists(filename.GetChars()))
						{
							mWriterFailureReason = "would overwrite an existing PNG frame";
							stopForFailure(VIDEO_WRITER_FAILURE, 0, mWriterFailureReason);
							break;
						}
						if (outputBytes > std::numeric_limits<uint64_t>::max() - VIDEO_PNG_ESTIMATE_OVERHEAD - VIDEO_PNG_TIMELINE_RESERVE)
						{
							mWriterFailureReason = "the PNG frame exceeds the safe output limit";
							stopForFailure(VIDEO_WRITER_FAILURE, 0, mWriterFailureReason);
							break;
						}
						const uint64_t estimateBytes = outputBytes + VIDEO_PNG_ESTIMATE_OVERHEAD;
						const uint64_t reservationBytes = estimateBytes + VIDEO_PNG_TIMELINE_RESERVE;
						// Finish every potentially allocating task setup before reserving
						// output space. The only operations after the reservation moves
						// are noexcept hand-offs into fixed slots, so a popped queue head
						// always has exactly one matching reservation and frame release.
						FPngEncodeTask task;
						task.Filename = filename;
						task.FrameNumber = nextFrameNumber;
						task.EstimateBytes = estimateBytes;
						task.ReservationBytes = reservationBytes;
						if (!ReservePngOutputSpace(reservationBytes))
						{
							stopForFailure(VIDEO_WRITER_SPACE_LIMIT, 0, mWriterFailureReason);
							break;
						}
						bool dispatched = false;
						{
							std::lock_guard<std::mutex> lock(mQueueMutex);
							if (!state.WorkerFailed && !mQueue.empty() && state.EncodingFrames < workerCount &&
								state.PendingCount < state.Pending.size())
							{
								task.Frame = std::move(mQueue.front());
								mQueue.pop_front();
								++state.EncodingFrames;
								state.Pending[state.PendingCount++] = std::move(task);
								dispatched = true;
							}
						}
						if (!dispatched)
						{
							ReleasePngOutputReservation(reservationBytes);
							break;
						}
						++nextFrameNumber;
						workerWake.notify_one();
					}
				}

				bool pipelineDrained = false;
				bool validPrefixCompletion = false;
				bool stopRequested = false;
				bool queuedFrames = false;
				{
					std::lock_guard<std::mutex> lock(mQueueMutex);
					pipelineDrained = state.EncodingFrames == 0;
					for (unsigned int index = 0; index < state.CompletedCount; ++index)
					{
						if (!terminalFailure || state.Completed[index].FrameNumber < firstFailedFrame)
						{
							validPrefixCompletion = true;
							break;
						}
					}
					stopRequested = mStopRequested;
					queuedFrames = !mQueue.empty();
				}
				if (terminalFailure && pipelineDrained && !validPrefixCompletion) break;
				if (!terminalFailure && stopRequested && !queuedFrames && pipelineDrained && !validPrefixCompletion)
				{
					// Completion publication and EncodingFrames are synchronized under
					// one mutex. Requiring both to be empty prevents a final worker
					// result from being discarded in the tiny publish/stop race.
					break;
				}

					std::unique_lock<std::mutex> lock(mQueueMutex);
					mQueueWake.wait(lock, [&]()
					{
					if (state.WorkerFailed) return true;
					if (terminalFailure)
					{
						for (unsigned int index = 0; index < state.CompletedCount; ++index)
						{
							const auto &completed = state.Completed[index];
							if (completed.FrameNumber < firstFailedFrame && completed.FrameNumber == nextCommitNumber)
							{
								return true;
							}
						}
						return state.EncodingFrames == 0;
					}
					for (unsigned int index = 0; index < state.CompletedCount; ++index)
					{
						const auto &completed = state.Completed[index];
						if (!completed.Succeeded || completed.FrameNumber == nextCommitNumber) return true;
					}
					if (!mQueue.empty() && state.EncodingFrames < workerCount)
					{
						const auto &head = mQueue.front();
						const bool sizeChangeBarrier = mOutputOpen &&
							(mWidth != head.Width || mHeight != head.Height);
						if (!sizeChangeBarrier || (state.EncodingFrames == 0 && state.CompletedCount == 0)) return true;
					}
					return mStopRequested && mQueue.empty() && state.EncodingFrames == 0 && state.CompletedCount == 0;
					});
				}
			}
			catch (...)
			{
				// The workers are still joined below before this function can unwind.
				// That prevents an unexpected coordinator allocation/filesystem error
				// from destroying a joinable std::thread and terminating the process.
				result = mWriterSpaceLimited ? VIDEO_WRITER_SPACE_LIMIT : VIDEO_WRITER_FAILURE;
				if (mWriterFailureReason.IsEmpty())
				{
					mWriterFailureReason = "could not coordinate PNG video frames";
				}
				AbortFrameAdmissionForWriterFailure();
			}

			{
				std::lock_guard<std::mutex> lock(mQueueMutex);
				state.StopWorkers = true;
			}
			workerWake.notify_all();
			for (auto &worker : workers)
			{
				if (worker.joinable()) worker.join();
			}

			// A stopped worker normally converts every dispatched task into a fixed
			// completion record above. Keep a defensive post-join sweep as well: no
			// pending task may outlive a coordinator exception without releasing its
			// exact reservation and raw-frame slot.
			abandonPendingTasks();

			// A failed task may have completed after a later worker. Those images
			// were never referenced by ffconcat, so remove them and release their
			// reservations only after all fixed worker threads have stopped.
			while (true)
			{
				FPngEncodeResult completed;
				{
					std::lock_guard<std::mutex> lock(mQueueMutex);
					if (state.CompletedCount == 0) break;
					completed = std::move(state.Completed[--state.CompletedCount]);
				}
				ReleasePngOutputReservation(completed.ReservationBytes);
				if (completed.Succeeded) RemoveFile(completed.Filename.GetChars());
				ReleaseFrameSlot(completed.StorageBytes);
			}
			return result;
		}

		void WriterMain()
		{
			EVideoWriterResult result = VIDEO_WRITER_FAILURE;
			try
			{
				result = mFormat == VIDEO_RECORDING_PNG_SEQUENCE ?
					WritePngFramesParallel() : WriteQueuedFramesSerial();

				// A space limit is detected before the next chunk is begun, so its
				// current AVI can be indexed normally. A real write failure may have
				// left a truncated chunk behind; remove that active part instead of
				// trying to make a misleadingly indexed file from it.
				if (mFormat == VIDEO_RECORDING_RGB_AVI && mFile != nullptr && result != VIDEO_WRITER_FAILURE)
				{
					const bool finalizeExistingPrefix = result == VIDEO_WRITER_SPACE_LIMIT;
					if (!FinishAviPart(finalizeExistingPrefix ? 0 : GetStopTimeNS()))
					{
						if (!finalizeExistingPrefix && mWriterSpaceLimited)
						{
							// A normal stop exhausted its output budget while extending the
							// final held frame. Keep the already-emitted contiguous prefix,
							// rather than retrying the same tail or discarding valid slots.
							result = VIDEO_WRITER_SPACE_LIMIT;
							if (!FinishAviPart(0))
							{
								result = mWriterSpaceLimited ? VIDEO_WRITER_SPACE_LIMIT : VIDEO_WRITER_FAILURE;
								DiscardAviPart();
							}
						}
						else
						{
							result = mWriterSpaceLimited ? VIDEO_WRITER_SPACE_LIMIT : VIDEO_WRITER_FAILURE;
							DiscardAviPart();
						}
					}
				}
				else if (result == VIDEO_WRITER_FAILURE)
				{
					DiscardAviPart();
				}
				if (mFormat == VIDEO_RECORDING_PNG_SEQUENCE && !FinishPngTimeline())
				{
					result = VIDEO_WRITER_FAILURE;
				}
				if (mFormat == VIDEO_RECORDING_PNG_SEQUENCE && result == VIDEO_WRITER_COMPLETE && !FinishPngAudio())
				{
					result = mWriterSpaceLimited ? VIDEO_WRITER_SPACE_LIMIT : VIDEO_WRITER_FAILURE;
				}
				if (result == VIDEO_WRITER_COMPLETE && mWriterSpaceLimited)
				{
					result = VIDEO_WRITER_SPACE_LIMIT;
				}
			}
			catch (...)
			{
				// This is the final containment boundary for the writer thread. Frame
				// encoders handle ordinary I/O/PNG errors locally, but an unexpected
				// allocation or finalization exception must never terminate the process.
				result = mWriterSpaceLimited ? VIDEO_WRITER_SPACE_LIMIT : VIDEO_WRITER_FAILURE;
				if (mWriterFailureReason.IsEmpty())
				{
					mWriterFailureReason = "the video writer encountered an unexpected error";
				}
				AbortFrameAdmissionForWriterFailure();
				DiscardAviPart();
				DiscardPngTimeline();
			}
			if (result == VIDEO_WRITER_FAILURE && mWriterFailureReason.IsEmpty())
			{
				mWriterFailureReason = "the video writer could not complete the take";
			}
			// The main thread normally joins and resets us immediately. Releasing
			// these writer-owned high-water buffers here also covers a failed take
			// while rendering is paused before its next capture callback.
			mBgrFrame.Reset();
			mAviIndex.Reset();
			mPngEncoder.Reset();
			PublishWriterResult(result, mWriterFailureReason);
		}

		void ResetAfterStop()
		{
			// StopWriter always joins before this reset. Reset, rather than Clear,
			// deliberately releases the high-water RGB/AVI allocations from a
			// completed 4K+ take.
			DiscardAviPart();
			DiscardPngTimeline();
			// Writer finalization has finished (or failed) before reset is reached;
			// release the bounded stream history and effect-source references now,
			// never leave a completed session resident until the next recording.
			I_DiscardVideoRecordingAudio();
			mBgrFrame.Reset();
			mAviIndex.Reset();
			mPngEncoder.Reset();
			mActive = false;
			mOutputOpen = false;
			mCurrentFile = "";
			mBaseFile = "";
			mSequenceStem = "";
			mAudioOutputPath = "";
			mPngTimelinePath = "";
			mTotalFrames = 0;
				mFinalizedAviFrames = 0;
				mFinalizedAviParts = 0;
				mAviNextSlot = 0;
				mAviDuplicatedFrames = 0;
				mAviHavePackedFrame = false;
				mAviTimelineOriginNS = 0;
				mAviPartFirstSlot = 0;
				mAviPartAudioFrames = 0;
			mEstimatedOutputBytes = 0;
			mPngReservedOutputBytes = 0;
			mOutputByteBudget = 0;
				mNextOutputSpaceProbeAtBytes = 0;
				mNextCaptureTime = 0;
				mTakeStartTimeNS = 0;
				mProducerStopReason = "";
				mStopNoticeShown = false;
				mStopDrainQueueWaitNoticeShown = false;
				mStopCaptureDrainPending = false;
				mStopCaptureDrainTimeNS = 0;
				mStopCaptureDrainStartedNS = 0;
				mStopReadbackAbandoned = false;
				mReadbackAbandonedForBackpressure = false;
				mRestartPending = false;
			mPendingStartName = "";
			{
				std::lock_guard<std::mutex> lock(mQueueMutex);
				mQueue.clear();
				mQueuedBytes = 0;
				mOutstandingFrames = 0;
				mStopRequested = false;
				mWriterAborting = false;
				mStopTimeNS = 0;
				mWriterFinished = false;
				mWriterResult = VIDEO_WRITER_COMPLETE;
				mWriterResultMessage = "";
				mWriterPngPoolFallback = false;
			}
		}

			bool mActive = false;
			bool mOutputOpen = false;
			bool mDropNoticeShown = false;
			bool mReadbackNoticeShown = false;
			bool mStopNoticeShown = false;
			bool mStopDrainQueueWaitNoticeShown = false;
			bool mStopCaptureDrainPending = false;
			bool mStopReadbackAbandoned = false;
			bool mReadbackAbandonedForBackpressure = false;
			bool mRestartPending = false;
		int mFormat = VIDEO_RECORDING_PNG_SEQUENCE;
		int mFrameRate = 60;
		int mPngCompressionLevel = 5;
		int mPart = 1;
		int mWidth = 0;
		int mHeight = 0;
		float mPngGammaOverride = 0.0f;
		uint64_t mCapturePeriodNS = 0;
			uint64_t mNextCaptureTime = 0;
			uint64_t mTakeStartTimeNS = 0;
			uint64_t mStopCaptureDrainTimeNS = 0;
			uint64_t mStopCaptureDrainStartedNS = 0;
		uint64_t mTotalFrames = 0;
			uint64_t mFinalizedAviFrames = 0;
			int mFinalizedAviParts = 0;
			uint64_t mAviNextSlot = 0;
			uint64_t mAviDuplicatedFrames = 0;
			bool mAviHavePackedFrame = false;
			uint64_t mAviTimelineOriginNS = 0;
			uint64_t mAviPartFirstSlot = 0;
			uint64_t mDroppedFrames = 0;
			uint64_t mReadbackDeferredFrames = 0;
			uint64_t mEstimatedOutputBytes = 0;
			// Bytes reserved for PNG tasks that an encoder has accepted but the
			// serial coordinator has not yet committed to the timing sidecar.
			// Keeping this separate from completed output prevents concurrent
			// encoders from collectively exceeding the existing disk budget.
			uint64_t mPngReservedOutputBytes = 0;
			uint64_t mOutputByteBudget = 0;
			uint64_t mNextOutputSpaceProbeAtBytes = 0;
		FString mBaseFile;
		FString mSequenceStem;
		FString mAudioOutputPath;
		FString mCurrentFile;
		FString mPngTimelinePath;
		FString mPngTimelineLastFile;
		FString mPendingStartName;
		FString mProducerStopReason;
		FString mWriterFailureReason;
			bool mWriterSpaceLimited = false;
			TArray<uint8_t> mBgrFrame;
			FPNGEncoder mPngEncoder;
		FileWriter *mFile = nullptr;
		FileWriter *mPngTimeline = nullptr;
		bool mPngTimelineFailed = false;
		TArray<FAviIndexEntry> mAviIndex;
		uint32_t mAviStride = 0;
			uint32_t mAviFrameBytes = 0;
			uint32_t mAviPartFrames = 0;
			uint64_t mAviPartAudioFrames = 0;
			uint64_t mAviPayloadBytes = 0;
		uint64_t mPngTimelineLastCaptureTimeNS = 0;
		ptrdiff_t mRiffSizePosition = 0;
		ptrdiff_t mAvihMicrosecondsPerFramePosition = 0;
		ptrdiff_t mAvihMaxBytesPerSecondPosition = 0;
		ptrdiff_t mAvihFrameCountPosition = 0;
		ptrdiff_t mStrhScalePosition = 0;
		ptrdiff_t mStrhRatePosition = 0;
		ptrdiff_t mStrhFrameCountPosition = 0;
		ptrdiff_t mStrhAudioFrameCountPosition = 0;
		ptrdiff_t mMoviSizePosition = 0;
		ptrdiff_t mMoviDataStart = 0;

		std::thread mWriterThread;
		mutable std::mutex mQueueMutex;
		std::condition_variable mQueueWake;
		std::deque<FQueuedVideoFrame> mQueue;
		// Includes the frame currently being written. This makes the bounded
		// memory contract real even while the worker has popped a frame.
		uint64_t mQueuedBytes = 0;
		unsigned int mOutstandingFrames = 0;
		bool mStopRequested = false;
		bool mWriterAborting = false;
		uint64_t mStopTimeNS = 0;
		bool mWriterFinished = false;
		EVideoWriterResult mWriterResult = VIDEO_WRITER_COMPLETE;
		FString mWriterResultMessage;
		bool mWriterPngPoolFallback = false;

		mutable std::mutex mStatusMutex;
		FVideoRecordingStatus mRecordingStatus;
		uint64_t mStatusTakeStartNS = 0;
		uint64_t mStatusVisibleUntilNS = 0;
	};

	FVideoRecorder VideoRecorder;
}

bool M_StartVideoRecording(const char *requestedName)
{
	return VideoRecorder.Start(requestedName);
}

void M_StopVideoRecording()
{
	VideoRecorder.Stop();
}

void M_FailVideoRecording(const char *reason)
{
	VideoRecorder.Fail(reason);
}

void M_FinishVideoRecording()
{
	VideoRecorder.Finish();
}

bool M_IsVideoRecording()
{
	return VideoRecorder.IsActive();
}

bool M_GetVideoRecordingStatus(FVideoRecordingStatus &status)
{
	return VideoRecorder.GetStatus(status);
}

void M_RequestScreenShot(const char *filename)
{
	PendingScreenShotName = filename ? filename : "";
	PendingScreenShot = true;
}

void M_PollStoppingVideoRecording()
{
	VideoRecorder.PollStopping();
}

void M_ProcessPendingScreenShot()
{
	VideoRecorder.CaptureFrame();
	if (PendingScreenShot)
	{
		PendingScreenShot = false;
		FString filename = PendingScreenShotName;
		PendingScreenShotName = "";
		M_ScreenShot(filename.GetChars());
	}
}

CCMD(startvideorecording)
{
	if (argv.argc() > 2)
	{
		Printf("Usage: startvideorecording [base name]\n");
		return;
	}
	M_StartVideoRecording(argv.argc() > 1 ? argv[1] : nullptr);
}

CCMD(stopvideorecording)
{
	M_StopVideoRecording();
}

CCMD(togglevideorecording)
{
	if (argv.argc() > 2)
	{
		Printf("Usage: togglevideorecording [base name]\n");
		return;
	}
	VideoRecorder.Toggle(argv.argc() > 1 ? argv[1] : nullptr);
}

CCMD(pastecapturepath)
{
	FString clipboard = I_GetFromClipboard(false);
	clipboard.StripLeftRight();
	if (clipboard.IsEmpty())
	{
		Printf("The clipboard does not contain an export folder.\n");
		return;
	}
	capture_export_dir = clipboard.GetChars();
	Printf("Capture export folder set to %s\n", M_GetCaptureExportPath().GetChars());
}

CCMD(copycapturepath)
{
	const FString path = M_GetCaptureExportPath();
	I_PutInClipboard(path.GetChars());
	Printf("Capture export folder copied to the clipboard.\n");
}

CCMD(resetcapturepath)
{
	capture_export_dir = "";
	Printf("Capture export folder reset to %s\n", M_GetCaptureExportPath().GetChars());
}

CCMD(opencaptures)
{
	const FString path = M_GetCaptureExportPath();
	I_OpenShellFolder(path.GetChars());
}

UNSAFE_CCMD (screenshot)
{
	if (I_IsHeadless())
	{
		// There is no rendered frame to capture; warn once instead of
		// queuing a pending screenshot that can never be processed.
		static bool headlessScreenshotWarned = false;
		if (!headlessScreenshotWarned)
		{
			headlessScreenshotWarned = true;
			Printf ("Screenshot unavailable in headless mode.\n");
		}
		return;
	}
	if (argv.argc() == 1)
		G_ScreenShot (NULL);
	else
		G_ScreenShot (argv[1]);
}

CCMD(openscreenshots)
{
	size_t dirlen;
	FString autoname;
	autoname = Args->CheckValue("-shotdir");
	if (autoname.IsEmpty())
	{
		autoname = screenshot_dir;
	}
	dirlen = autoname.Len();
	if (dirlen == 0)
	{
		autoname = M_GetScreenshotsPath();
		dirlen = autoname.Len();
	}
	if (dirlen > 0)
	{
		if (autoname[dirlen-1] != '/' && autoname[dirlen-1] != '\\')
		{
			autoname += '/';
		}
	}
	autoname = NicePath(autoname.GetChars());

	CreatePath(autoname.GetChars());

	I_OpenShellFolder(autoname.GetChars());
}

static int SaveConfig()
{
	return M_SaveDefaults(nullptr);
}

DEFINE_ACTION_FUNCTION_NATIVE(_CVar, SaveConfig, SaveConfig)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(M_SaveDefaults(nullptr));
}
