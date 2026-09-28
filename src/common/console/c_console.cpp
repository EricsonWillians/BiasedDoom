/*
** c_console.cpp
** Implements the console itself
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

#include "c_bind.h"
#include "c_commandbuffer.h"
#include "c_console.h"
#include "c_consolebuffer.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "c_notifybufferbase.h"
#include "cmdlib.h"
#include "common/scripting/dap/GameEventEmit.h"
#include "d_eventbase.h"
#include "d_gui.h"
#include "g_input.h"
#include "gamestate.h"
#include "i_interface.h"
#include "i_system.h"
#include "i_time.h"
#include "menu.h"
#include "menustate.h"
#include "printf.h"
#include "texturemanager.h"
#include "utf8.h"
#include "v_2ddrawer.h"
#include "v_draw.h"
#include "v_font.h"
#include "v_text.h"
#include "v_video.h"
#include "version.h"
#include "vm.h"

#define LEFTMARGIN 8
#define RIGHTMARGIN 8
#define BOTTOMARGIN 12

extern bool AppActive;

CUSTOM_CVAR(Int, con_buffersize, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	// ensure a minimum size
	if (self >= 0 && self < 128) self = 128;
}

double NotifyFontScale = 1;

DEFINE_GLOBAL(NotifyFontScale)

void C_SetNotifyFontScale(double scale)
{
	NotifyFontScale = scale;
}


FConsoleBuffer *conbuffer;

static FTextureID conback;
static FTextureID conflat;
static uint32_t conshade;
static bool conline;

extern FConsoleCommand *Commands[FConsoleCommand::HASH_SIZE];

int			ConWidth;
bool		vidactive = false;
bool		cursoron = false;
int			ConBottom, ConScroll, RowAdjust;
uint64_t	CursorTicker;
uint8_t		ConsoleState = c_up;

DEFINE_GLOBAL(ConsoleState)

static int TopLine, InsertLine;

static void ClearConsole ();

struct GameAtExit
{
	GameAtExit(FString str) : Command(str) {}

	GameAtExit *Next;
	FString Command;
};

static GameAtExit *ExitCmdList;

#define SCROLLUP 1
#define SCROLLDN 2
#define SCROLLNO 0

// Buffer for AddToConsole()
static char *work = NULL;
static int worklen = 0;

CUSTOM_CVAR(Int, con_scale, 0, CVAR_ARCHIVE)
{
	if (self < 0) self = 0;
}

CUSTOM_CVAR(Float, con_alpha, 0.75f, CVAR_ARCHIVE)
{
	if (self < 0.f) self = 0.f;
	if (self > 1.f) self = 1.f;
}

// Selects the console scrollback font: 0 = new console font (default),
// 1 = classic CONFONT, 2 = small font.
CUSTOM_CVAR(Int, con_font, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0) self = 0;
	if (self > 2) self = 2;
}

FFont *ActiveConsoleFont()
{
	switch ((int)con_font)
	{
	case 1:
		if (ConFont != nullptr) return ConFont;
		break;
	case 2:
		if (SmallFont != nullptr) return SmallFont;
		break;
	default:
		break;
	}
	return NewConsoleFont != nullptr ? NewConsoleFont : ConFont;
}

CVARD(Bool, con_timestamps, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Prefix console scrollback lines with a [HH:MM:SS] timestamp (console display only, logfile is unaffected)")

CUSTOM_CVARD(Bool, con_quick_home_end, true, CVAR_ARCHIVE, "Use HOME/END keys to scroll when cursor is at start/end of line already")
{}

// Show developer messages if true.
CUSTOM_CVAR(Int, developer, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	FScriptPosition::Developer = self;
}


// Command to run when Ctrl-D is pressed at start of line
CVAR(String, con_ctrl_d, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)


struct History
{
	struct History *Older;
	struct History *Newer;
	FString String;
};

#define MAXHISTSIZE 50
static struct History *HistHead = NULL, *HistTail = NULL, *HistPos = NULL;
static int HistSize;

static FNotifyBufferBase *NotifyStrings;

void C_SetNotifyBuffer(FNotifyBufferBase* nbb)
{
	NotifyStrings = nbb;
}



int PrintColors[PRINTLEVELS+2] = { CR_UNTRANSLATED, CR_GOLD, CR_GRAY, CR_GREEN, CR_GREEN, CR_UNTRANSLATED };

static void setmsgcolor (int index, int color);

FILE *Logfile = NULL;


CVARD_NAMED(Int, msglevel, msg, 0, CVAR_ARCHIVE, "Filters HUD message by importance");

CUSTOM_CVAR (Int, msg0color, CR_UNTRANSLATED, CVAR_ARCHIVE)
{
	setmsgcolor (0, self);
}

CUSTOM_CVAR (Int, msg1color, CR_GOLD, CVAR_ARCHIVE)
{
	setmsgcolor (1, self);
}

CUSTOM_CVAR (Int, msg2color, CR_GRAY, CVAR_ARCHIVE)
{
	setmsgcolor (2, self);
}

CUSTOM_CVAR (Int, msg3color, CR_GREEN, CVAR_ARCHIVE)
{
	setmsgcolor (3, self);
}

CUSTOM_CVAR (Int, msg4color, CR_GREEN, CVAR_ARCHIVE)
{
	setmsgcolor (4, self);
}

CUSTOM_CVAR (Int, msgmidcolor, CR_UNTRANSLATED, CVAR_ARCHIVE)
{
	setmsgcolor (PRINTLEVELS, self);
}

CUSTOM_CVAR (Int, msgmidcolor2, CR_BROWN, CVAR_ARCHIVE)
{
	setmsgcolor (PRINTLEVELS+1, self);
}

void C_InitConback(FTextureID fallback, bool tile, double brightness)
{
	conback = TexMan.CheckForTexture ("CONBACK", ETextureType::MiscPatch);
	conflat = fallback;
	if (!conback.isValid())
	{
		conback.SetInvalid();
		conshade = MAKEARGB(uint8_t(255 - 255*brightness),0,0,0);
		conline = true;
		if (!tile) conback = fallback;
	}
	else
	{
		conshade = 0;
		conline = false;
	}
}

void C_InitConsole (int width, int height, bool ingame)
{
	int cwidth, cheight;

	vidactive = ingame;
	if (ActiveConsoleFont() != NULL)
	{
		cwidth = ActiveConsoleFont()->GetCharWidth ('M');
		cheight = ActiveConsoleFont()->GetHeight();
	}
	else
	{
		cwidth = cheight = 8;
	}
	ConWidth = (width - LEFTMARGIN - RIGHTMARGIN);
	CmdLine.ConCols = ConWidth / cwidth;

	if (conbuffer == NULL) conbuffer = new FConsoleBuffer;
}

//==========================================================================
//
// CCMD atexit
//
//==========================================================================

UNSAFE_CCMD (atexit)
{
	if (argv.argc() == 1)
	{
		Printf ("Registered atexit commands:\n");
		GameAtExit *record = ExitCmdList;
		while (record != NULL)
		{
			Printf ("%s\n", record->Command.GetChars());
			record = record->Next;
		}
		return;
	}
	for (int i = 1; i < argv.argc(); ++i)
	{
		GameAtExit *record = new GameAtExit(argv[i]);
		record->Next = ExitCmdList;
		ExitCmdList = record;
	}
}

//==========================================================================
//
// C_DeinitConsole
//
// Executes the contents of the atexit cvar, if any, at quit time.
// Then releases all of the console's memory.
//
//==========================================================================

void C_DeinitConsole ()
{
	GameAtExit *cmd = ExitCmdList;

	while (cmd != NULL)
	{
		GameAtExit *next = cmd->Next;
		AddCommandString (cmd->Command.GetChars());
		delete cmd;
		cmd = next;
	}

	// Free command history
	History *hist = HistTail;

	while (hist != NULL)
	{
		History *next = hist->Newer;
		delete hist;
		hist = next;
	}
	HistTail = HistHead = HistPos = NULL;

	// Free alias commands. (i.e. The "commands" that can be allocated
	// at runtime.)
	for (size_t i = 0; i < countof(Commands); ++i)
	{
		FConsoleCommand *command = Commands[i];

		while (command != NULL)
		{
			FConsoleCommand *nextcmd = command->m_Next;
			if (command->IsAlias())
			{
				delete command;
			}
			command = nextcmd;
		}
	}

	// Make sure all tab commands are cleared before the memory for
	// their names is deallocated.
	C_ClearTabCommands ();
	C_ClearDynCCmds();

	// Free AddToConsole()'s work buffer
	if (work != NULL)
	{
		free (work);
		work = NULL;
		worklen = 0;
	}

	if (conbuffer != NULL)
	{
		delete conbuffer;
		conbuffer = NULL;
	}
}

static void ClearConsole ()
{
	if (conbuffer != NULL)
	{
		conbuffer->Clear();
	}
	TopLine = InsertLine = 0;
}

static void setmsgcolor (int index, int color)
{
	if ((unsigned)color >= (unsigned)NUM_TEXT_COLORS)
		color = 0;
	PrintColors[index] = color;
}


void AddToConsole (int printlevel, const char *text)
{
	conbuffer->AddText(printlevel, MakeUTF8(text));
}

//==========================================================================
//
//
//
//==========================================================================

void WriteLineToLog(FILE *LogFile, const char *outline)
{
	// Strip out any color escape sequences before writing to the log file
	TArray<char> copy(strlen(outline) + 1);
	const char * srcp = outline;
	char * dstp = copy.Data();

	while (*srcp != 0)
	{

		if (*srcp != TEXTCOLOR_ESCAPE)
		{
			*dstp++ = *srcp++;
		}
		else if (srcp[1] == '[')
		{
			srcp += 2;
			while (*srcp != ']' && *srcp != 0) srcp++;
			if (*srcp == ']') srcp++;
		}
		else
		{
			if (srcp[1] != 0) srcp += 2;
			else break;
		}
	}
	*dstp = 0;

	fputs(copy.Data(), LogFile);
	fflush(LogFile);
}

extern bool gameisdead;

static void C_CopyScrollbackToClipboard();

int PrintString (int iprintlevel, const char *outline)
{
	if (gameisdead)
		return 0;

	if (!conbuffer) return 0;	// when called too early
	int printlevel = iprintlevel & PRINT_TYPES;
	if (*outline == '\0')
	{
		return 0;
	}
	if (printlevel != PRINT_LOG || !(iprintlevel & PRINT_NODAPEVENT) || Logfile != nullptr)
	{
		// Convert everything coming through here to UTF-8 so that all console text is in a consistent format
		int count;
		outline = MakeUTF8(outline, &count);

		if (printlevel != PRINT_LOG)
		{
			I_PrintStr(outline);

			conbuffer->AddText(printlevel, outline);
			if (vidactive && screen && !(iprintlevel & PRINT_NONOTIFY) && NotifyStrings)
			{
				if (printlevel >= msglevel)
				{
					NotifyStrings->AddString(iprintlevel, outline);
				}
			}
		}
		if (Logfile != nullptr && !(iprintlevel & PRINT_NOLOG))
		{
			WriteLineToLog(Logfile, outline);
		}
		if (!(iprintlevel & PRINT_NODAPEVENT))
		{
			DebugServer::RuntimeEvents::EmitLogEvent(iprintlevel, outline);
		}
		return count;
	}
	return 0;	// Don't waste time on calculating this if nothing at all was printed...
}

int VPrintf (int printlevel, const char *format, va_list parms)
{
	FString outline;
	outline.VFormat (format, parms);
	return PrintString (printlevel, outline.GetChars());
}

int Printf (int printlevel, const char *format, ...)
{
	va_list argptr;
	int count;

	va_start (argptr, format);
	count = VPrintf (printlevel, format, argptr);
	va_end (argptr);

	return count;
}

int Printf (const char *format, ...)
{
	va_list argptr;
	int count;

	va_start (argptr, format);
	count = VPrintf (PRINT_HIGH, format, argptr);
	va_end (argptr);

	return count;
}

int DPrintf (int level, const char *format, ...)
{
	va_list argptr;
	int count;

	if (developer >= level)
	{
		va_start (argptr, format);
		count = VPrintf (PRINT_HIGH, format, argptr);
		va_end (argptr);
		return count;
	}
	else
	{
		return 0;
	}
}

void C_FlushDisplay ()
{
	if (NotifyStrings) NotifyStrings->Clear();
}

void C_AdjustBottom ()
{
	if (gamestate == GS_FULLCONSOLE || gamestate == GS_STARTUP)
		ConBottom = twod->GetHeight();
	else if (ConBottom > twod->GetHeight() / 2 || ConsoleState == c_down)
		ConBottom = twod->GetHeight() / 2;
}

void C_NewModeAdjust ()
{
	C_InitConsole (screen->GetWidth(), screen->GetHeight(), true);
	C_FlushDisplay ();
	C_AdjustBottom ();
}

int consoletic = 0;
void C_Ticker()
{
	static int lasttic = 0;
	consoletic++;

	if (lasttic == 0)
		lasttic = consoletic - 1;

	if (con_buffersize > 0)
	{
		conbuffer->ResizeBuffer(con_buffersize);
	}

	if (ConsoleState != c_up)
	{
		if (ConsoleState == c_falling)
		{
			ConBottom += (consoletic - lasttic) * (twod->GetHeight() * 2 / 25);
			if (ConBottom >= twod->GetHeight() / 2)
			{
				ConBottom = twod->GetHeight() / 2;
				ConsoleState = c_down;
			}
		}
		else if (ConsoleState == c_rising)
		{
			ConBottom -= (consoletic - lasttic) * (twod->GetHeight() * 2 / 25);
			if (ConBottom <= 0)
			{
				ConsoleState = c_up;
				ConBottom = 0;
			}
		}
	}

	lasttic = consoletic;
	if (NotifyStrings) NotifyStrings->Tick();
}

//==========================================================================
//
// Mouse selection of scrollback text
//
// Positions are stored as (formatted line index, byte column) where the
// column indexes the line's text with color escapes stripped. Formatted
// indices stay valid across appends; a full reformat (font/width change or
// buffer clear) bumps the buffer's format generation and invalidates the
// selection.
//
//==========================================================================

static bool ConSelActive = false;		// a highlighted selection exists
static bool ConSelDragging = false;		// left button is held down to select
static int ConSelAnchorLine = 0, ConSelAnchorCol = 0;
static int ConSelCaretLine = 0, ConSelCaretCol = 0;
static unsigned int ConSelGeneration = 0;

static void C_ClearConsoleSelection()
{
	ConSelActive = false;
	ConSelDragging = false;
}

static void C_OrderConsoleSelection(int &l1, int &c1, int &l2, int &c2)
{
	l1 = ConSelAnchorLine;	c1 = ConSelAnchorCol;
	l2 = ConSelCaretLine;	c2 = ConSelCaretCol;

	if (l2 < l1 || (l2 == l1 && c2 < c1))
	{
		int tl = l1, tc = c1;
		l1 = l2;	c1 = c2;
		l2 = tl;	c2 = tc;
	}
}

static void C_ValidateConsoleSelection()
{
	if (ConSelActive && ConSelGeneration != conbuffer->GetFormatGeneration())
		C_ClearConsoleSelection();
}

// strip \cX / \c[Name] color escapes (same state machine as the logfile writer)
static FString C_StripColorEscapes(const char *srcp)
{
	FString result;

	while (*srcp != 0)
	{
		if (*srcp != TEXTCOLOR_ESCAPE)
		{
			result += *srcp++;
		}
		else if (srcp[1] == '[')
		{
			srcp += 2;
			while (*srcp != ']' && *srcp != 0) srcp++;
			if (*srcp == ']') srcp++;
		}
		else
		{
			if (srcp[1] != 0) srcp += 2;
			else break;
		}
	}

	return result;
}

// visible pixel width of the first `column` bytes of an escape-free string
static int C_ColumnToPixels(FFont *font, const FString &text, int column)
{
	int width = 0;
	const uint8_t *start = (const uint8_t *)text.GetChars();
	const uint8_t *ptr = start;

	while (*ptr != 0 && ptr - start < column)
	{
		int chr = GetCharFromString(ptr);
		width += font->GetCharWidth(chr) + font->GetDefaultKerning();
	}

	return width;
}

// byte column whose character cell contains (or is nearest to) the given
// pixel offset in an escape-free string
static int C_PixelsToColumn(FFont *font, const FString &text, int pixels)
{
	int width = 0;
	const uint8_t *start = (const uint8_t *)text.GetChars();
	const uint8_t *ptr = start;

	while (*ptr != 0)
	{
		const uint8_t *charstart = ptr;
		int chr = GetCharFromString(ptr);
		int w = font->GetCharWidth(chr) + font->GetDefaultKerning();

		if (pixels < width + w / 2)
			return (int)(charstart - start);
		width += w;
	}

	return (int)(ptr - start);
}

// number of visible scrollback rows and their y-origin adjustment
// (must match the drawing code in C_DrawConsole)
static void C_ConsoleTextGeometry(int textScale, int &lines, int &offset)
{
	lines = (ConBottom/textScale-ActiveConsoleFont()->GetHeight()*2)/ActiveConsoleFont()->GetHeight();
	if (-ActiveConsoleFont()->GetHeight() + lines*ActiveConsoleFont()->GetHeight() > ConBottom/textScale - ActiveConsoleFont()->GetHeight()*7/2)
	{
		offset = -ActiveConsoleFont()->GetHeight()/2;
		lines--;
	}
	else
	{
		offset = -ActiveConsoleFont()->GetHeight();
	}
}

// Map screen pixel coordinates to a formatted line index + byte column.
// With clamp=false, positions on/below the input line are rejected so
// clicks there don't start a selection; with clamp=true (during drags)
// they snap to the nearest scrollback row.
static bool C_ConsoleHitTest(int scrx, int scry, int &outline, int &outcol, bool clamp)
{
	int textScale = active_con_scale(twod);
	int lines, offset;

	C_ConsoleTextGeometry(textScale, lines, offset);
	if (lines <= 0) return false;

	// make sure the formatted lines are current (normally done every frame
	// by C_DrawConsole, but events can arrive before the first draw)
	conbuffer->FormatText(ActiveConsoleFont(), ConWidth / textScale);

	unsigned int consolelines = conbuffer->GetFormattedLineCount();
	if (consolelines == 0) return false;

	int fontheight = ActiveConsoleFont()->GetHeight();
	int vx = scrx / textScale - LEFTMARGIN;
	int vy = scry / textScale;

	// row n is drawn at y = offset + n*fontheight, with n in [1, lines]
	int n = (vy - offset) / fontheight;

	if (n < 1)
	{
		if (!clamp) return false;
		n = 1;
	}
	if (n > lines) n = lines;

	int fi = (int)consolelines - 1 - RowAdjust - (lines - n);
	if (fi < 0) fi = 0;
	if (fi > (int)consolelines - 1) fi = (int)consolelines - 1;

	FBrokenLines *blines = conbuffer->GetLines();
	FString stripped = C_StripColorEscapes(blines[fi].Text.GetChars());

	outline = fi;
	outcol = C_PixelsToColumn(ActiveConsoleFont(), stripped, max(vx, 0));
	return true;
}

// the selected text, with color escapes stripped; wrapped fragments of the
// same raw line are joined without a newline
static FString C_GetConsoleSelectionText()
{
	FString result;
	int l1, c1, l2, c2;

	C_OrderConsoleSelection(l1, c1, l2, c2);

	unsigned int consolelines = conbuffer->GetFormattedLineCount();
	if (consolelines == 0) return result;

	if (l2 > (int)consolelines - 1) l2 = (int)consolelines - 1;
	if (l1 < 0) l1 = 0;

	FBrokenLines *blines = conbuffer->GetLines();

	for (int fi = l1; fi <= l2; fi++)
	{
		FString stripped = C_StripColorEscapes(blines[fi].Text.GetChars());
		int len = (int)stripped.Len();
		int c0 = (fi == l1) ? c1 : 0;
		int ce = (fi == l2) ? c2 : len;

		if (c0 < 0) c0 = 0;
		if (c0 > len) c0 = len;
		if (ce < 0) ce = 0;
		if (ce > len) ce = len;

		if (fi > l1 &&
			conbuffer->GetRawLineForFormatted(fi) != conbuffer->GetRawLineForFormatted(fi - 1))
		{
			result += '\n';
		}

		if (ce > c0)
			result.AppendCStrPart(stripped.GetChars() + c0, ce - c0);
	}

	return result;
}

void C_DrawConsole ()
{
	static int oldbottom = 0;
	int lines, left, offset;

	int textScale = active_con_scale(twod);

	left = LEFTMARGIN;
	C_ConsoleTextGeometry(textScale, lines, offset);

	oldbottom = ConBottom;

	if (ConsoleState == c_up && gamestate == GS_LEVEL)
	{
		if (NotifyStrings) NotifyStrings->Draw();
		return;
	}
	else if (ConBottom)
	{
		int visheight;

		visheight = ConBottom;

		if (conback.isValid() && gamestate != GS_FULLCONSOLE)
		{
			DrawTexture (twod, conback, false, 0, visheight - screen->GetHeight(),
				DTA_DestWidth, twod->GetWidth(),
				DTA_DestHeight, twod->GetHeight(),
				DTA_ColorOverlay, conshade,
				DTA_Alpha, (gamestate != GS_FULLCONSOLE) ? (double)con_alpha : 1.,
				DTA_Masked, false,
				TAG_DONE);
		}
		else
		{
			if (conflat.isValid() && gamestate != GS_FULLCONSOLE)
			{
				int conbright = 255 - APART(conshade);
				PalEntry pe((uint8_t(255 * con_alpha)), conbright, conbright, conbright);
				twod->AddFlatFill(0, visheight - screen->GetHeight(), screen->GetWidth(), visheight, TexMan.GetGameTexture(conflat), 1, CleanXfac, pe, STYLE_Shaded);
			}
			else
			{
				PalEntry pe((uint8_t)(con_alpha * 255), 0, 0, 0);
				twod->AddColorOnlyQuad(0, 0, screen->GetWidth(), visheight, pe);
			}
		}

		if (conline && visheight < screen->GetHeight())
		{
			twod->AddColorOnlyQuad(0, visheight, screen->GetWidth(), 1, 0xff000000);
		}

		if (ConBottom >= 12)
		{
			if (textScale == 1)
				DrawText(twod, ActiveConsoleFont(), CR_ORANGE, twod->GetWidth() - 8 -
					ActiveConsoleFont()->StringWidth (GetVersionString()),
					round((float)ConBottom / textScale) - ActiveConsoleFont()->GetHeight() - 4,
					GetVersionString(), TAG_DONE);
			else
				DrawText(twod, ActiveConsoleFont(), CR_ORANGE, (float)twod->GetWidth() / textScale - 8 -
					ActiveConsoleFont()->StringWidth(GetVersionString()),
					round((float)ConBottom / textScale) - ActiveConsoleFont()->GetHeight() - 4,
					GetVersionString(),
					DTA_VirtualWidth, twod->GetWidth() / textScale,
					DTA_VirtualHeight, twod->GetHeight() / textScale,
					DTA_KeepRatio, true, TAG_DONE);

		}

	}

	if (menuactive != MENU_Off)
	{
		return;
	}

	if (lines > 0)
	{
		// No more enqueuing because adding new text to the console won't touch the actual print data.
		conbuffer->FormatText(ActiveConsoleFont(), ConWidth / textScale);
		unsigned int consolelines = conbuffer->GetFormattedLineCount();
		FBrokenLines *blines = conbuffer->GetLines();
		if (blines != nullptr)
		{
			FBrokenLines* printline = blines + consolelines - 1 - RowAdjust;

			int bottomline = ConBottom / textScale - ActiveConsoleFont()->GetHeight() * 2 - 4;

			C_ValidateConsoleSelection();
			int sell1 = 0, selc1 = 0, sell2 = -1, selc2 = 0;
			if (ConSelActive)
				C_OrderConsoleSelection(sell1, selc1, sell2, selc2);

			for (FBrokenLines* p = printline; p >= blines && lines > 0; p--, lines--)
			{
				if (ConSelActive)
				{
					int fi = (int)(p - blines);

					if (fi >= sell1 && fi <= sell2)
					{
						FString stripped = C_StripColorEscapes(p->Text.GetChars());
						int len = (int)stripped.Len();
						int c0 = (fi == sell1) ? selc1 : 0;
						int ce = (fi == sell2) ? selc2 : len;

						if (c0 < 0) c0 = 0;
						if (c0 > len) c0 = len;
						if (ce < 0) ce = 0;
						if (ce > len) ce = len;

						if (ce > c0)
						{
							int x0 = LEFTMARGIN + C_ColumnToPixels(ActiveConsoleFont(), stripped, c0);
							int x1 = LEFTMARGIN + C_ColumnToPixels(ActiveConsoleFont(), stripped, ce);
							int y = offset + lines * ActiveConsoleFont()->GetHeight();

							twod->AddColorOnlyQuad(x0 * textScale, y * textScale,
								(x1 - x0) * textScale, ActiveConsoleFont()->GetHeight() * textScale,
								PalEntry(140, 70, 100, 180));
						}
					}
				}

				if (textScale == 1)
				{
					DrawText(twod, ActiveConsoleFont(), CR_TAN, LEFTMARGIN, offset + lines * ActiveConsoleFont()->GetHeight(), p->Text.GetChars(), TAG_DONE);
				}
				else
				{
					DrawText(twod, ActiveConsoleFont(), CR_TAN, LEFTMARGIN, offset + lines * ActiveConsoleFont()->GetHeight(), p->Text.GetChars(),
						DTA_VirtualWidth, twod->GetWidth() / textScale,
						DTA_VirtualHeight, twod->GetHeight() / textScale,
						DTA_KeepRatio, true, TAG_DONE);
				}
			}

			if (ConBottom >= 20)
			{
				if (gamestate != GS_STARTUP)
				{
					auto now = I_msTime();
					if (now > CursorTicker)
					{
						CursorTicker = now + 500;
						cursoron = !cursoron;
					}
					CmdLine.Draw(left, bottomline, textScale, cursoron);
				}
				if (RowAdjust && ConBottom >= ActiveConsoleFont()->GetHeight() * 7 / 2)
				{
					// Indicate that the view has been scrolled up (10)
					// and if we can scroll no further (12)
					if (textScale == 1)
						DrawChar(twod, ActiveConsoleFont(), CR_GREEN, 0, bottomline, RowAdjust == conbuffer->GetFormattedLineCount() ? 12 : 10, TAG_DONE);
					else
						DrawChar(twod, ActiveConsoleFont(), CR_GREEN, 0, bottomline, RowAdjust == conbuffer->GetFormattedLineCount() ? 12 : 10,
							DTA_VirtualWidth, twod->GetWidth() / textScale,
							DTA_VirtualHeight, twod->GetHeight() / textScale,
							DTA_KeepRatio, true, TAG_DONE);
				}
			}
		}
	}
}

void C_FullConsole ()
{
	ConsoleState = c_down;
	HistPos = NULL;
	TabbedLast = false;
	TabbedList = false;
	gamestate = GS_FULLCONSOLE;
	C_AdjustBottom ();
}

void C_ToggleConsole ()
{
	int togglestate;
	if (gamestate == GS_INTRO) // blocked
	{
		return;
	}
	if (gamestate == GS_MENUSCREEN)
	{
		if (sysCallbacks.ToggleFullConsole) sysCallbacks.ToggleFullConsole();
		togglestate = c_down;
	}
	else if (!chatmodeon && (ConsoleState == c_up || ConsoleState == c_rising) && menuactive == MENU_Off)
	{
		ConsoleState = c_falling;
		HistPos = NULL;
		TabbedLast = false;
		TabbedList = false;
		togglestate = c_falling;
	}
	else if (gamestate != GS_FULLCONSOLE && gamestate != GS_STARTUP)
	{
		ConsoleState = c_rising;
		C_FlushDisplay();
		togglestate = c_rising;
	}
	else return;
	// This must be done as an event callback because the client code does not control the console toggling.
	if (sysCallbacks.ConsoleToggled) sysCallbacks.ConsoleToggled(togglestate);
}

void C_HideConsole ()
{
	if (gamestate != GS_FULLCONSOLE)
	{
		ConsoleState = c_up;
		ConBottom = 0;
		HistPos = NULL;
	}
}

static bool C_HandleKey (event_t *ev, FCommandBuffer &buffer)
{
	int data1 = ev->data1;
	bool keepappending = false;

	int page_height = (twod->GetHeight()-4)/active_con_scale(twod) /
		((gamestate == GS_FULLCONSOLE || gamestate == GS_STARTUP) ? ActiveConsoleFont()->GetHeight() : ActiveConsoleFont()->GetHeight()*2) - 3;
	int total_lines = conbuffer->GetFormattedLineCount();
	int top_row = total_lines - page_height - 1;

	switch (ev->subtype)
	{
	default:
		return false;

	case EV_GUI_Char:
		if (ev->data2)
		{
			// Bash-style shortcuts
			if (data1 == 'b')
			{
				buffer.CursorWordLeft();
				break;
			}
			else if (data1 == 'f')
			{
				buffer.CursorWordRight();
				break;
			}
		}
		// Add keypress to command line
		C_ClearConsoleSelection();
		buffer.AddChar(data1);
		HistPos = NULL;
		TabbedLast = false;
		TabbedList = false;
		break;

	case EV_GUI_WheelUp:
	case EV_GUI_WheelDown:
		if (!(ev->data3 & GKM_SHIFT))
		{
			data1 = GK_PGDN + EV_GUI_WheelDown - ev->subtype;
		}
		else
		{
			data1 = GK_DOWN + EV_GUI_WheelDown - ev->subtype;
		}
		// Intentional fallthrough

	case EV_GUI_KeyDown:
	case EV_GUI_KeyRepeat:
		switch (data1)
		{
		case '\t':
			// Try to do tab-completion
			C_TabComplete ((ev->data3 & GKM_SHIFT) ? false : true);
			break;

		case GK_PGUP:
			if (ev->subtype == EV_GUI_WheelUp)
			{ // Scroll console buffer up
				if (ev->data3 & (GKM_SHIFT|GKM_CTRL)) RowAdjust += 1;
				else RowAdjust += 3;

				if (RowAdjust > total_lines) RowAdjust = total_lines;
			}
			else
			{ // Scroll console buffer up one page
				RowAdjust += page_height;

				if (RowAdjust > top_row) RowAdjust = top_row; // intentionally different than scroll behavior
			}
			break;

		case GK_PGDN:
			if (ev->subtype == EV_GUI_WheelDown)
			{ // Scroll console buffer down
				if (ev->data3 & (GKM_SHIFT|GKM_CTRL)) RowAdjust -= 1;
				else RowAdjust -= 3;
			}
			else
			{ // Scroll console buffer down one page
				RowAdjust -= page_height;
			}
			if (RowAdjust < 0) RowAdjust = 0;
			break;

		case GK_HOME:
			if ((ev->data3 & (GKM_CTRL|GKM_SHIFT))
				|| (!buffer.CursorStart() && con_quick_home_end)) // Try to move cursor to start of line
			{ // Move to top of console buffer
				RowAdjust = top_row;
			}
			break;

		case GK_END:
			if ((ev->data3 & (GKM_CTRL|GKM_SHIFT))
				|| (!buffer.CursorEnd() && con_quick_home_end)) // Try to move cursor to end of line
			{ // Move to bottom of console buffer
				RowAdjust = 0;
			}
			break;

		case GK_LEFT:
			// Move cursor left one character
			buffer.CursorLeft();
			break;

		case GK_RIGHT:
			// Move cursor right one character
			buffer.CursorRight();
			break;

		case '\b':
			// Erase character to left of cursor
			buffer.DeleteLeft();
			TabbedLast = false;
			TabbedList = false;
			break;

		case GK_DEL:
			// Erase character under cursor
			buffer.DeleteRight();
			TabbedLast = false;
			TabbedList = false;
			break;

		case GK_UP:
			if (ev->data3 & GKM_SHIFT)
			{
				RowAdjust = min (total_lines, RowAdjust + 1); // match mousewheel
			}
			else
			{
				// Move to previous entry in the command history
				if (HistPos == NULL)
				{
					HistPos = HistHead;
				}
				else if (HistPos->Older)
				{
					HistPos = HistPos->Older;
				}

				if (HistPos)
				{
					buffer.SetString(HistPos->String);
				}

				TabbedLast = false;
				TabbedList = false;
			}
			break;

		case GK_DOWN:
			if (ev->data3 & GKM_SHIFT)
			{
				RowAdjust = max (0, RowAdjust - 1);
			}
			else
			{
				// Move to next entry in the command history
				if (HistPos && HistPos->Newer)
				{
					HistPos = HistPos->Newer;
					buffer.SetString(HistPos->String);
				}
				else
				{
					HistPos = NULL;
					buffer.SetString("");
				}
				TabbedLast = false;
				TabbedList = false;
			}
			break;

		case 'X':
			if (ev->data3 & GKM_CTRL)
			{
				buffer.SetString("");
				TabbedLast = TabbedList = false;
			}
			break;

		case 'D':
			if (ev->data3 & GKM_CTRL && buffer.TextLength() == 0)
			{ // Control-D pressed on an empty line
				if (strlen(con_ctrl_d) == 0)
				{
					break;	// Replacement is empty, so do nothing
				}
				buffer.SetString(*con_ctrl_d);
			}
			else
			{
				break;
			}
			// Intentional fall-through for command(s) added with Ctrl-D
			[[fallthrough]];

		case '\r':
		{
			// Execute command line (ENTER)
			FString bufferText = buffer.GetText();

			C_ClearConsoleSelection();
			bufferText.StripLeftRight();
			Printf(127, TEXTCOLOR_WHITE "]%s\n", bufferText.GetChars());

			if (bufferText.Len() == 0)
			{
				// Command line is empty, so do nothing to the history
			}
			else if (HistHead && HistHead->String.CompareNoCase(bufferText) == 0)
			{
				// Command line was the same as the previous one,
				// so leave the history list alone
			}
			else
			{
				// Command line is different from last command line,
				// or there is nothing in the history list,
				// so add it to the history list.

				History *temp = new History;
				temp->String = bufferText;
				temp->Older = HistHead;
				if (HistHead)
				{
					HistHead->Newer = temp;
				}
				temp->Newer = NULL;
				HistHead = temp;

				if (!HistTail)
				{
					HistTail = temp;
				}

				if (HistSize == MAXHISTSIZE)
				{
					HistTail = HistTail->Newer;
					delete HistTail->Older;
					HistTail->Older = NULL;
				}
				else
				{
					HistSize++;
				}
			}
			HistPos = NULL;
			buffer.SetString("");
			AddCommandString(bufferText.GetChars());
			TabbedLast = false;
			TabbedList = false;
			break;
		}

		case '`':
			// Check to see if we have ` bound to the console before accepting
			// it as a way to close the console.
			if (Bindings.GetBinding(KEY_GRAVE).CompareNoCase("toggleconsole"))
			{
				break;
			}
			[[fallthrough]];
		case GK_ESCAPE:
			// Close console and clear command line. But if we're in the
			// fullscreen console mode, there's nothing to fall back on
			// if it's closed, so open the main menu instead.
			if (ConSelActive)
			{
				// the first Escape just clears the selection
				C_ClearConsoleSelection();
				break;
			}
			if (gamestate == GS_STARTUP || !AppActive)
			{
				return false;
			}
			else if (gamestate == GS_FULLCONSOLE)
			{
				C_DoCommand ("menu_main");
			}
			else
			{
				buffer.SetString("");
				HistPos = NULL;
				C_ToggleConsole ();
			}
			break;

		case 'C':
		case 'V':
			TabbedLast = false;
			TabbedList = false;
#ifdef __APPLE__
			if (ev->data3 & GKM_META)
#else // !__APPLE__
			if (ev->data3 & GKM_CTRL)
#endif // __APPLE__
			{
				if (data1 == 'C')
				{ // copy to clipboard
					C_ValidateConsoleSelection();
					if (ConSelActive)
					{
						// copy the highlighted scrollback selection
						FString sel = C_GetConsoleSelectionText();

						if (sel.IsNotEmpty())
							I_PutInClipboard(sel.GetChars());
					}
					else if (buffer.TextLength() > 0)
					{
						I_PutInClipboard(buffer.GetText().GetChars());
					}
				}
				else
				{ // paste from clipboard
					buffer.AddString(I_GetFromClipboard(false));
					HistPos = NULL;
				}
				break;
			}
			break;

		// Bash-style shortcuts
		case 'A':
			if (ev->data3 & GKM_CTRL)
			{
				int textScale = active_con_scale(twod);

				conbuffer->FormatText(ActiveConsoleFont(), ConWidth / textScale);

				unsigned int consolelines = conbuffer->GetFormattedLineCount();

				if (consolelines > 0)
				{
					// Select the entire scrollback (highlighted on screen)
					// and copy it to the clipboard.
					I_PutInClipboard(conbuffer->GetText().GetChars());

					FBrokenLines *blines = conbuffer->GetLines();
					FString last = C_StripColorEscapes(blines[consolelines - 1].Text.GetChars());

					ConSelAnchorLine = 0;
					ConSelAnchorCol = 0;
					ConSelCaretLine = (int)consolelines - 1;
					ConSelCaretCol = (int)last.Len();
					ConSelGeneration = conbuffer->GetFormatGeneration();
					ConSelActive = true;
					ConSelDragging = false;
				}
				else
				{
					buffer.CursorStart();
				}
			}
			break;
		case 'E':
			if (ev->data3 & GKM_CTRL)
			{
				buffer.CursorEnd();
			}
			break;
		case 'W':
			if (ev->data3 & GKM_CTRL)
			{
				buffer.DeleteWordLeft();
				keepappending = true;
				TabbedLast = false;
				TabbedList = false;
			}
			break;
		case 'U':
			if (ev->data3 & GKM_CTRL)
			{
				buffer.DeleteLineLeft();
				keepappending = true;
				TabbedLast = false;
				TabbedList = false;
			}
			break;
		case 'K':
			if (ev->data3 & GKM_CTRL)
			{
				buffer.DeleteLineRight();
				keepappending = true;
				TabbedLast = false;
				TabbedList = false;
			}
			break;
		case 'Y':
			if (ev->data3 & GKM_CTRL)
			{
				buffer.AddYankBuffer();
				TabbedLast = false;
				TabbedList = false;
				HistPos = NULL;
			}
			break;
		}
		break;

#ifdef __unix__
	case EV_GUI_MButtonDown:
		buffer.AddString(I_GetFromClipboard(true));
		HistPos = NULL;
		break;
#endif

	case EV_GUI_LButtonDown:
	{
		int fi, col;

		C_ValidateConsoleSelection();

		if (C_ConsoleHitTest(ev->data1, ev->data2, fi, col, false))
		{
			// begin a new selection; it becomes visible once the drag
				// actually covers some text (see EV_GUI_MouseMove)
			ConSelDragging = true;
			ConSelAnchorLine = ConSelCaretLine = fi;
			ConSelAnchorCol = ConSelCaretCol = col;
			ConSelGeneration = conbuffer->GetFormatGeneration();
			ConSelActive = false;
		}
		else
		{
			// clicked outside the scrollback text (e.g. the input line)
			C_ClearConsoleSelection();
		}
		break;
	}

	case EV_GUI_LButtonUp:
		ConSelDragging = false;
		break;

	case EV_GUI_MouseMove:
		if (!ConSelDragging)
			return false;
		{
			int fi, col;

			if (C_ConsoleHitTest(ev->data1, ev->data2, fi, col, true))
			{
				ConSelCaretLine = fi;
				ConSelCaretCol = col;
				ConSelActive = (fi != ConSelAnchorLine || col != ConSelAnchorCol);
			}
		}
		break;
	}

	buffer.AppendToYankBuffer = keepappending;

	// Ensure that the cursor is always visible while typing
	CursorTicker = I_msTime() + 500;
	cursoron = 1;
	return true;
}

bool C_Responder (event_t *ev)
{
	if (ev->type != EV_GUI_Event ||
		ConsoleState == c_up ||
		ConsoleState == c_rising ||
		menuactive != MENU_Off)
	{
		return false;
	}

	return C_HandleKey(ev, CmdLine);
}

CCMD (history)
{
	struct History *hist = HistTail;

	while (hist)
	{
		Printf ("   %s\n", hist->String.GetChars());
		hist = hist->Newer;
	}
}

// copy the console scrollback to the OS clipboard (cross-platform)
static void C_CopyScrollbackToClipboard()
{
	FString text = conbuffer->GetText();

	I_PutInClipboard(text.GetChars());

	Printf ("%u console line(s) copied to clipboard.\n",
			conbuffer->GetRawLineCount());
}

CCMD (copyconsole)
{
	int lines = 0;

	if (argv.argc() > 1)
		lines = atoi(argv[1]);

	FString text = conbuffer->GetText(lines);

	I_PutInClipboard(text.GetChars());

	if (lines > 0)
		Printf ("Last %d console line(s) copied to clipboard.\n", lines);
	else
		Printf ("%u console line(s) copied to clipboard.\n",
				conbuffer->GetRawLineCount());
}

CCMD (clear)
{
	C_FlushDisplay ();
	ClearConsole ();
}

CCMD (echo)
{
	int last = argv.argc()-1;
	for (int i = 1; i <= last; ++i)
	{
		FString formatted = strbin1 (argv[i]);
		Printf ("%s%s", formatted.GetChars(), i!=last ? " " : "\n");
	}
}

CCMD(toggleconsole)
{
	C_ToggleConsole();
}
