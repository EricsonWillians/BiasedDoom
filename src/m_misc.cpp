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
#include <chrono>
#include <utility>

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
		return !FileExists(baseFile.GetChars()) &&
			!CaptureFamilyHasFiles(stem, "_frame", ".png") &&
			!CaptureFamilyHasFiles(stem, "_part", ".png");
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
	constexpr uint64_t VIDEO_CATCHUP_SECONDS = 10;

	struct FAviIndexEntry
	{
		uint32_t Offset;
		uint32_t Size;
	};

	class FVideoRecorder
	{
	public:
		bool Start(const char *requestedName)
		{
			if (mActive)
			{
				Printf("Video recording is already active.\n");
				return false;
			}
			if (I_IsHeadless())
			{
				Printf("Video recording is unavailable in headless mode.\n");
				return false;
			}

			mFormat = vid_record_format == VIDEO_RECORDING_RGB_AVI ? VIDEO_RECORDING_RGB_AVI : VIDEO_RECORDING_PNG_SEQUENCE;
			mFrameRate = vid_record_fps;
			if (mFrameRate < 1) mFrameRate = 1;
			if (mFrameRate > 240) mFrameRate = 240;
			FString configuredName;
			configuredName = vid_record_name;
			const char *name = requestedName != nullptr && requestedName[0] != '\0' ? requestedName : configuredName.GetChars();
			mBaseFile = M_MakeCaptureFileName(name, mFormat == VIDEO_RECORDING_RGB_AVI ? ".avi" : ".png", "Video");
			if (mBaseFile.IsEmpty())
			{
				Printf("Could not find an unused video capture filename.\n");
				return false;
			}

			mSequenceStem = StripExtension(mBaseFile);
			if (mFormat == VIDEO_RECORDING_PNG_SEQUENCE)
			{
				if (!SelectUnusedSequenceStem())
				{
					Printf("Could not find an unused PNG sequence name.\n");
					return false;
				}
			}
			else if (!SelectUnusedAviFamily())
			{
				Printf("Could not find an unused AVI recording name.\n");
				return false;
			}
			mPart = 1;
			mStartTime = CaptureWallClockNS();
			mTotalFrames = 0;
			mOutputOpen = false;
			mLagNoticeShown = false;
			mLastFrame.Clear();
			mActive = true;

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

			bool finished = true;
			if (mFormat == VIDEO_RECORDING_RGB_AVI && mFile != nullptr)
			{
				finished = FinishAviPart();
			}
			if (!finished)
			{
				// Do not leave an open, unindexed AVI behind when its final header
				// patch fails. Completed earlier parts remain usable; the failed
				// current part is removed by Abort().
				Abort("could not finalize the AVI output file");
				return;
			}
			if (finished && mTotalFrames > 0)
			{
				if (mFormat == VIDEO_RECORDING_PNG_SEQUENCE)
				{
					Printf("Video recording stopped: %llu lossless PNG frames at %d FPS (%s_frame%%06d.png).\n",
						(unsigned long long)mTotalFrames, mFrameRate, mSequenceStem.GetChars());
				}
				else
				{
					Printf("Video recording stopped: %llu lossless RGB AVI frames in %d part%s.\n",
						(unsigned long long)mTotalFrames, mPart, mPart == 1 ? "" : "s");
				}
			}
			else if (finished)
			{
				Printf("Video recording stopped before a composited frame was available.\n");
			}
			Reset();
		}

		bool IsActive() const
		{
			return mActive;
		}

		void CaptureFrame()
		{
			if (!mActive || screen == nullptr)
			{
				return;
			}

			const uint64_t now = CaptureWallClockNS();
			const uint64_t elapsed = now >= mStartTime ? now - mStartTime : 0;
			uint64_t wantedFrames = elapsed * (uint64_t)mFrameRate / 1000000000ull + 1;
			if (wantedFrames <= mTotalFrames)
			{
				return;
			}
			const uint64_t maximumCatchup = (uint64_t)mFrameRate * VIDEO_CATCHUP_SECONDS;
			if (wantedFrames - mTotalFrames > maximumCatchup)
			{
				wantedFrames = mTotalFrames + maximumCatchup;
				if (!mLagNoticeShown)
				{
					Printf("Video capture fell behind; limiting duplicate-frame catch-up to %llu seconds.\n",
						(unsigned long long)VIDEO_CATCHUP_SECONDS);
					mLagNoticeShown = true;
				}
			}

			int pitch = 0;
			ESSType colorType = SS_RGB;
			float gamma = 1.0f;
			auto screenshot = screen->GetScreenshotBuffer(pitch, colorType, gamma);
			const int width = screen->GetWidth();
			const int height = screen->GetHeight();
			if (screenshot.Size() == 0 || colorType != SS_RGB || pitch < width * 3)
			{
				Abort("the active renderer could not provide an RGB final frame");
				return;
			}
			if (!EnsureOutput(width, height))
			{
				return;
			}
			if (!CopyFinalFrame(screenshot, pitch, gamma))
			{
				Abort("the final frame buffer was incomplete");
				return;
			}

			while (mTotalFrames + 1 < wantedFrames && mLastFrame.Size() != 0)
			{
				if (!WriteFrame(mLastFrame, mLastFrameGamma))
				{
					return;
				}
			}
			if (!WriteFrame(mCurrentFrame, mCurrentFrameGamma))
			{
				return;
			}
			mLastFrame = std::move(mCurrentFrame);
			mLastFrameGamma = mCurrentFrameGamma;
		}

	private:
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

		bool EnsureOutput(int width, int height)
		{
			if (!mOutputOpen)
			{
				mWidth = width;
				mHeight = height;
				mOutputOpen = true;
				if (mFormat == VIDEO_RECORDING_RGB_AVI && !OpenAviPart())
				{
					Abort("could not create the AVI output file");
					return false;
				}
				return true;
			}
			if (mWidth == width && mHeight == height)
			{
				return true;
			}

			if (mFormat == VIDEO_RECORDING_RGB_AVI && !FinishAviPart())
			{
				Abort("could not finalize the AVI part after a resolution change");
				return false;
			}
			++mPart;
			mWidth = width;
			mHeight = height;
			mLastFrame.Clear();
			if (mFormat == VIDEO_RECORDING_RGB_AVI && !OpenAviPart())
			{
				Abort("could not create the next AVI part after a resolution change");
				return false;
			}
			Printf("Video capture resolution changed; continuing in part %d at %dx%d.\n", mPart, mWidth, mHeight);
			return true;
		}

		bool CopyFinalFrame(const TArray<uint8_t> &screenshot, int pitch, float gamma)
		{
			const uint64_t frameSize = (uint64_t)mWidth * (uint64_t)mHeight * 3ull;
			const uint64_t sourceSize = (uint64_t)pitch * (uint64_t)mHeight;
			if (frameSize > 0xffffffffull || sourceSize > screenshot.Size())
			{
				return false;
			}
			mCurrentFrame.Resize((unsigned int)frameSize);
			for (int y = 0; y < mHeight; ++y)
			{
				memcpy(mCurrentFrame.Data() + (size_t)y * mWidth * 3,
					screenshot.Data() + (size_t)y * pitch, (size_t)mWidth * 3);
			}
			// Screenshot backends report the image gamma for PNG metadata. Keep
			// it with the frame so HDR/fullscreen captures retain the same color
			// interpretation as an ordinary engine screenshot.
			mCurrentFrameGamma = gamma > 0.0f ? gamma : 1.0f;
			return true;
		}

		bool WriteFrame(const TArray<uint8_t> &rgb, float gamma)
		{
			const bool written = mFormat == VIDEO_RECORDING_PNG_SEQUENCE ? WritePngFrame(rgb, gamma) : WriteAviFrame(rgb);
			if (written)
			{
				++mTotalFrames;
			}
			return written;
		}

		bool WritePngFrame(const TArray<uint8_t> &rgb, float gamma)
		{
			FString filename;
			filename.Format("%s_frame%06llu.png", SequenceStemForPart().GetChars(), (unsigned long long)(mTotalFrames + 1));
			if (FileExists(filename.GetChars()))
			{
				Abort("would overwrite an existing PNG frame");
				return false;
			}
			auto file = FileWriter::Open(filename.GetChars());
			if (file == nullptr)
			{
				Abort("could not create a PNG frame");
				return false;
			}
			const bool written = M_CreatePNG(file, rgb.Data(), nullptr, SS_RGB, mWidth, mHeight, mWidth * 3, gamma) && M_FinishPNG(file);
			delete file;
			if (!written)
			{
				RemoveFile(filename.GetChars());
				Abort("could not write a PNG frame");
			}
			return written;
		}

		bool WriteBytes(const void *data, size_t size)
		{
			return mFile != nullptr && mFile->Write(data, size) == size;
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
			if (restore < 0 || mFile->Seek(position, SEEK_SET) != 0 || !WriteU32(value) || mFile->Seek(restore, SEEK_SET) != 0)
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
				return false;
			}
			mAviStride = (uint32_t)stride;
			mAviFrameBytes = (uint32_t)frameBytes;
			mBgrFrame.Resize(mAviFrameBytes);
			mCurrentFile = AviFileForPart();
			if (FileExists(mCurrentFile.GetChars()))
			{
				return false;
			}
			mFile = FileWriter::Open(mCurrentFile.GetChars());
			if (mFile == nullptr)
			{
				return false;
			}
			mAviIndex.Clear();
			mAviPayloadBytes = 0;
			mAviPartFrames = 0;

			const uint64_t maximumBytesPerSecond = (uint64_t)mAviFrameBytes * (uint64_t)mFrameRate;
			const uint32_t bytesPerSecond = maximumBytesPerSecond > 0xffffffffull ? 0xffffffffu : (uint32_t)maximumBytesPerSecond;
			if (!WriteFourCC("RIFF")) return false;
			mRiffSizePosition = mFile->Tell();
			if (!WriteU32(0) || !WriteFourCC("AVI ") || !WriteFourCC("LIST")) return false;
			const ptrdiff_t hdrlSizePosition = mFile->Tell();
			if (!WriteU32(0)) return false;
			const ptrdiff_t hdrlStart = mFile->Tell();
			if (!WriteFourCC("hdrl") || !WriteFourCC("avih") || !WriteU32(56)) return false;
			if (!WriteU32((uint32_t)(1000000 / mFrameRate)) || !WriteU32(bytesPerSecond) || !WriteU32(0) || !WriteU32(0x10)) return false;
			mAvihFrameCountPosition = mFile->Tell();
			if (!WriteU32(0) || !WriteU32(0) || !WriteU32(1) || !WriteU32(mAviFrameBytes) || !WriteU32((uint32_t)mWidth) || !WriteU32((uint32_t)mHeight)) return false;
			for (int index = 0; index < 4; ++index) if (!WriteU32(0)) return false;

			if (!WriteFourCC("LIST")) return false;
			const ptrdiff_t strlSizePosition = mFile->Tell();
			if (!WriteU32(0)) return false;
			const ptrdiff_t strlStart = mFile->Tell();
			if (!WriteFourCC("strl") || !WriteFourCC("strh") || !WriteU32(56) || !WriteFourCC("vids") || !WriteU32(0)) return false;
			if (!WriteU32(0) || !WriteU16(0) || !WriteU16(0) || !WriteU32(0) || !WriteU32(1) || !WriteU32((uint32_t)mFrameRate) || !WriteU32(0)) return false;
			mStrhFrameCountPosition = mFile->Tell();
			if (!WriteU32(0) || !WriteU32(mAviFrameBytes) || !WriteU32(0xffffffffu) || !WriteU32(0)) return false;
			for (int index = 0; index < 4; ++index) if (!WriteU16(0)) return false;

			if (!WriteFourCC("strf") || !WriteU32(40) || !WriteU32(40) || !WriteU32((uint32_t)mWidth) || !WriteU32((uint32_t)mHeight)) return false;
			if (!WriteU16(1) || !WriteU16(24) || !WriteU32(0) || !WriteU32(mAviFrameBytes) || !WriteU32(0) || !WriteU32(0) || !WriteU32(0) || !WriteU32(0)) return false;

			const ptrdiff_t afterStrl = mFile->Tell();
			if (afterStrl < strlSizePosition + 4 || !PatchU32(strlSizePosition, (uint32_t)(afterStrl - strlSizePosition - 4))) return false;
			const ptrdiff_t afterHdrl = mFile->Tell();
			if (afterHdrl < hdrlSizePosition + 4 || !PatchU32(hdrlSizePosition, (uint32_t)(afterHdrl - hdrlSizePosition - 4))) return false;

			if (!WriteFourCC("LIST")) return false;
			mMoviSizePosition = mFile->Tell();
			if (!WriteU32(0) || !WriteFourCC("movi")) return false;
			mMoviDataStart = mFile->Tell();
			return mMoviDataStart >= 0;
		}

		bool WriteAviFrame(const TArray<uint8_t> &rgb)
		{
			const uint64_t chunkBytes = 8ull + mAviFrameBytes + (mAviFrameBytes & 1u);
			const uint64_t indexBytes = ((uint64_t)mAviIndex.Size() + 1ull) * 16ull;
			if (mAviPartFrames != 0 && mAviPayloadBytes + chunkBytes + indexBytes + 4096ull > AVI_PART_LIMIT)
			{
				if (!FinishAviPart())
				{
					Abort("could not finalize an AVI part");
					return false;
				}
				++mPart;
				if (!OpenAviPart())
				{
					Abort("could not create the next AVI part");
					return false;
				}
				Printf("Lossless RGB AVI reached its safe RIFF limit; continuing in part %d.\n", mPart);
			}

			for (int y = 0; y < mHeight; ++y)
			{
				const uint8_t *source = rgb.Data() + (size_t)(mHeight - y - 1) * mWidth * 3;
				uint8_t *destination = mBgrFrame.Data() + (size_t)y * mAviStride;
				for (int x = 0; x < mWidth; ++x)
				{
					destination[x * 3 + 0] = source[x * 3 + 2];
					destination[x * 3 + 1] = source[x * 3 + 1];
					destination[x * 3 + 2] = source[x * 3 + 0];
				}
				if (mAviStride > (uint32_t)mWidth * 3)
				{
					memset(destination + (size_t)mWidth * 3, 0, mAviStride - (uint32_t)mWidth * 3);
				}
			}

			const ptrdiff_t chunkStart = mFile->Tell();
			// AVI idx1 offsets are relative to the `movi` list type (not its
			// first payload byte), so a first frame begins at offset four. This
			// convention is required by strict AVI readers such as ffmpeg.
			const ptrdiff_t moviTypeStart = mMoviDataStart - 4;
			if (moviTypeStart < 0 || chunkStart < mMoviDataStart || (uint64_t)(chunkStart - moviTypeStart) > 0xffffffffull ||
				!WriteFourCC("00db") || !WriteU32(mAviFrameBytes) || !WriteBytes(mBgrFrame.Data(), mAviFrameBytes))
			{
				Abort("could not write an AVI frame");
				return false;
			}
			if ((mAviFrameBytes & 1u) != 0)
			{
				const uint8_t padding = 0;
				if (!WriteBytes(&padding, 1))
				{
					Abort("could not pad an AVI frame");
					return false;
				}
			}
			mAviIndex.Push({ (uint32_t)(chunkStart - moviTypeStart), mAviFrameBytes });
			mAviPayloadBytes += chunkBytes;
			++mAviPartFrames;
			return true;
		}

		bool FinishAviPart()
		{
			if (mFile == nullptr)
			{
				return true;
			}
			if (mAviPartFrames == 0)
			{
				delete mFile;
				mFile = nullptr;
				RemoveFile(mCurrentFile.GetChars());
				return true;
			}

			const ptrdiff_t moviEnd = mFile->Tell();
			if (moviEnd < mMoviSizePosition + 4 || !PatchU32(mMoviSizePosition, (uint32_t)(moviEnd - mMoviSizePosition - 4)) ||
				!WriteFourCC("idx1") || (uint64_t)mAviIndex.Size() * 16ull > 0xffffffffull || !WriteU32((uint32_t)mAviIndex.Size() * 16u))
			{
				return false;
			}
			for (const auto &entry : mAviIndex)
			{
				if (!WriteFourCC("00db") || !WriteU32(0x10) || !WriteU32(entry.Offset) || !WriteU32(entry.Size))
				{
					return false;
				}
			}
			const ptrdiff_t fileEnd = mFile->Tell();
			if (fileEnd < 8 || !PatchU32(mAvihFrameCountPosition, mAviPartFrames) || !PatchU32(mStrhFrameCountPosition, mAviPartFrames) ||
				!PatchU32(mRiffSizePosition, (uint32_t)(fileEnd - 8)))
			{
				return false;
			}
			delete mFile;
			mFile = nullptr;
			return true;
		}

		void Abort(const char *reason)
		{
			Printf("Video recording stopped: %s.\n", reason);
			if (mFile != nullptr)
			{
				delete mFile;
				mFile = nullptr;
				if (mCurrentFile.IsNotEmpty()) RemoveFile(mCurrentFile.GetChars());
			}
			Reset();
		}

		void Reset()
		{
			// All normal AVI paths close their writer before resetting. Keep this
			// guard for failed header/index writes so an error never leaks a file
			// handle through shutdown.
			if (mFile != nullptr)
			{
				delete mFile;
				mFile = nullptr;
			}
			mActive = false;
			mOutputOpen = false;
			mFile = nullptr;
			mLastFrame.Clear();
			mCurrentFrame.Clear();
			mLastFrameGamma = 1.0f;
			mCurrentFrameGamma = 1.0f;
			mBgrFrame.Clear();
			mAviIndex.Clear();
			mCurrentFile = "";
		}

		bool mActive = false;
		bool mOutputOpen = false;
		bool mLagNoticeShown = false;
		int mFormat = VIDEO_RECORDING_PNG_SEQUENCE;
		int mFrameRate = 60;
		int mPart = 1;
		int mWidth = 0;
		int mHeight = 0;
		uint64_t mStartTime = 0;
		uint64_t mTotalFrames = 0;
		FString mBaseFile;
		FString mSequenceStem;
		FString mCurrentFile;
		TArray<uint8_t> mLastFrame;
		TArray<uint8_t> mCurrentFrame;
		float mLastFrameGamma = 1.0f;
		float mCurrentFrameGamma = 1.0f;
		TArray<uint8_t> mBgrFrame;
		FileWriter *mFile = nullptr;
		TArray<FAviIndexEntry> mAviIndex;
		uint32_t mAviStride = 0;
		uint32_t mAviFrameBytes = 0;
		uint32_t mAviPartFrames = 0;
		uint64_t mAviPayloadBytes = 0;
		ptrdiff_t mRiffSizePosition = 0;
		ptrdiff_t mAvihFrameCountPosition = 0;
		ptrdiff_t mStrhFrameCountPosition = 0;
		ptrdiff_t mMoviSizePosition = 0;
		ptrdiff_t mMoviDataStart = 0;
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

bool M_IsVideoRecording()
{
	return VideoRecorder.IsActive();
}

void M_RequestScreenShot(const char *filename)
{
	PendingScreenShotName = filename ? filename : "";
	PendingScreenShot = true;
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
	if (M_IsVideoRecording()) M_StopVideoRecording();
	else
	{
		if (argv.argc() > 2)
		{
			Printf("Usage: togglevideorecording [base name]\n");
			return;
		}
		M_StartVideoRecording(argv.argc() > 1 ? argv[1] : nullptr);
	}
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
