//---------------------------------------------------------------------------
//
// BiasedDoom embedded Python runtime
//
// Python scripts are trusted native-equivalent mod code. The runtime is
// therefore opt-in and deliberately makes no claim of sandboxing CPython.
// ACS and ZScript continue to use their existing, independent runtimes.
//
//---------------------------------------------------------------------------

#include "python_runtime.h"
#include "python_game_api.h"
#include "python_displaylist.h"

#include "actor.h"
#include "c_bind.h"
#include "c_buttons.h"
#include "c_console.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "d_buttons.h"
#include "d_player.h"
#include "doomstat.h"
#include "engineerrors.h"
#include "filesystem.h"
#include "g_level.h"
#include "g_levellocals.h"
#include "i_interface.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_random.h"
#include "p_local.h"
#include "p_spec.h"
#include "r_defs.h"
#include "serializer.h"
#include "version.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef BIASEDDOOM_PYTHON
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#if PY_VERSION_HEX < 0x030D0000
// CPython 3.13 removed frameobject.h and moved PyFrame_Check into Python.h;
// older supported versions (3.10-3.12) only declare it here.
#include <frameobject.h>
#endif
#ifdef _PyCFunction_CAST
#define BD_PY_KEYWORD_FUNCTION(function) _PyCFunction_CAST(function)
#else
// CPython 3.10 predates _PyCFunction_CAST. This is the same two-step
// function-pointer cast used by CPython 3.11+ to avoid incompatible-signature
// warnings for METH_VARARGS | METH_KEYWORDS methods.
#define BD_PY_KEYWORD_FUNCTION(function) \
	reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(function))
#endif
#endif

CVAR(Bool, py_enabled, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL | CVAR_SYSTEM_ONLY)
CVAR(Int, py_tick_budget_ms, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
CVAR(Bool, py_tick_hard_budget, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
CVAR(Int, py_tick_overrun_limit, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
CVAR(Int, py_max_tasks, 4096, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)

// Defined in python_game_api.cpp: flushes actor-slot invalidations that the
// engine GC markers queued instead of running Py_DECREF inside the marker
// (python_game_api.h is frozen for this change, hence the local declaration).
namespace PythonRuntime::GameApi
{
	void DrainDeferredInvalidations();
}

namespace PythonRuntime
{
#ifdef BIASEDDOOM_PYTHON

namespace
{
constexpr int PythonApiVersion = 2;

struct ScriptEntry
{
	int Container = -1;
	std::string Path;
	std::string Resource;
};

struct ScriptModule
{
	PyObject* Module = nullptr;
	int Container = -1;
	std::string Path;
};

struct Callback
{
	std::string Event;
	int EventIndex = -1;
	PyObject* Callable = nullptr;
	int Container = -1;
	std::string Source;
	PClassActor* ClassFilter = nullptr;
	int TidFilter = 0;
	int PlayerFilter = -1;
	unsigned Every = 1;
	int Priority = 0;
	bool Failed = false;
	bool BudgetDisabled = false;
	unsigned BudgetWarnings = 0;
	unsigned ConsecutiveOverruns = 0;
	uint64_t Seen = 0;
	uint64_t Calls = 0;
	uint64_t TotalMicroseconds = 0;
	uint64_t MaximumMicroseconds = 0;
	uint64_t BudgetSkips = 0;
	uint64_t BudgetOverruns = 0;
};

struct ScheduledTask
{
	uint64_t Id = 0;
	uint64_t DueTick = 0;
	uint64_t Interval = 0;
	PyObject* Callable = nullptr;
	int Container = -1;
	std::string Source;
	uint64_t MapSerial = 0;
	bool MapLocal = true;
	bool Cancelled = false;
	uint64_t Calls = 0;
	uint64_t TotalMicroseconds = 0;
	uint64_t MaximumMicroseconds = 0;
	uint64_t BudgetSkips = 0;
	uint64_t BudgetOverruns = 0;
	unsigned ConsecutiveOverruns = 0;
};

bool active = false;
bool inittabRegistered = false;
bool loadCallbackPending = false;
bool gameplayMutationBlocked = false;
bool callbacksNeedSort = false;
unsigned callbackDispatchDepth = 0;
uint64_t tickBudgetMicroseconds = 0;
uint64_t tickBudgetOverruns = 0;
uint64_t tickBudgetSkips = 0;
uint64_t taskClock = 0;
uint64_t nextTaskId = 1;
uint64_t mapSerial = 0;
unsigned taskDispatchDepth = 0;
int currentContainer = -1;
std::string currentSource;
std::string stdoutBuffer;
std::string stderrBuffer;
// dedup state for identical consecutive Python errors (see ReportPythonError)
FString s_lastPythonError;
unsigned s_repeatPythonErrorCount = 0;
// dedup state for identical consecutive script warnings (bd.warn); kept
// separate from the error dedup so warnings and errors never suppress
// each other
FString s_lastPythonWarning;
unsigned s_repeatPythonWarningCount = 0;
// -scripttest: total reported errors, including suppressed repeats
unsigned s_pythonErrorCount = 0;
// -pyerrorlog <file>: JSON-lines feed of Python errors for external tools
std::string s_pythonErrorLogPath;
// Dedicated deterministic stream backing bd.random()/randrange()/randint()/
// choice(). Named, so StaticClearRandom seeds it with the run's rngseed on a
// new game like every other engine RNG; per-map reseeding happens in
// OnWorldLoaded and the full state round-trips through the "pythonstate"
// save blob under the reserved RngStateKey ("__biaseddoom_rng_state_v1__";
// pre-4.15.15 saves may still carry the legacy "__rng_state__" key, which
// LoadStateJson also accepts).
FRandom s_pyRandom("PythonRandom");
// Reserved state-dictionary key holding the script RNG snapshot while a
// savegame/hub snapshot is being written or read. Collision-resistant on
// purpose: a user script owning this exact key is a serialization error, not
// an overwrite. The legacy key is rejected on write just the same (it is
// only accepted when reading pre-4.15.15 saves).
const char* const RngStateKey = "__biaseddoom_rng_state_v1__";
const char* const LegacyRngStateKey = "__rng_state__";
// Set by LoadStateJson when it handled the stream for the pending map entry,
// so OnWorldLoaded does not clobber a restored savegame/hub-snapshot state.
bool s_rngStateLoaded = false;
// Per-player last-known sector index backing the sector_entered/sector_exited
// events. -1 means "unknown/not in a sector"; reset on map load and unload.
int s_lastPlayerSector[MAXPLAYERS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
// Number of generic custom action buttons (Button_PyAction1..32 in
// d_buttons.h, console names +pyaction1..+pyaction32 registered through
// DoomButtons in d_main.cpp). They are local-only input state: they never
// enter usercmd, demos, or network traffic.
constexpr int PyActionCount = 32;
// Last down-state reported by the custom action per-tic scan, bit n-1 for
// action n. Drives transition detection so each press/release fires exactly
// one custom_action event; the raw bWentDown/bWentUp edge flags cannot be
// used for this (they are only reset per tic on Windows; SDL builds never
// call ResetButtonTriggers).
uint32_t s_pyActionDownMask = 0;
std::vector<ScriptEntry> discoveredScripts;
std::vector<ScriptModule> modules;
std::vector<Callback> callbacks;
std::vector<ScheduledTask> scheduledTasks;
std::thread::id engineThread;
PyObject* engineModule = nullptr;
PyObject* stateDictionary = nullptr; // Strong runtime-owned reference.

const char* const EventNames[] = {
	"engine_start",
	"map_load",
	"map_unload",
	"pre_tick",
	"tick",
	"post_tick",
	"actor_spawned",
	"actor_died",
	"actor_damaged",
	"actor_destroyed",
	"actor_revived",
	"line_activated",
	"line_activation_failed",
	"player_entered",
	"player_spawned",
	"player_respawned",
	"player_died",
	"player_disconnected",
	"save",
	"load",
	"engine_shutdown",
	"item_picked",
	"secret_found",
	"item_dropped",
	"weapon_changed",
	"sector_entered",
	"sector_exited",
	"imgui_frame",
	"conversation_started",
	"conversation_reply",
	// APPEND new event names at the END only: eventHasCallbacks is indexed by
	// position in this table.
	"ui_command",
	"actor_before_damage",
	"custom_action",
};
constexpr size_t EventCount = sizeof(EventNames) / sizeof(EventNames[0]);
std::array<bool, EventCount> eventHasCallbacks{};

int FindEventIndex(const char* event)
{
	for (size_t index = 0; index < EventCount; ++index)
	{
		if (strcmp(event, EventNames[index]) == 0) return static_cast<int>(index);
	}
	return -1;
}

void RebuildEventPresence()
{
	eventHasCallbacks.fill(false);
	for (const Callback& callback : callbacks)
	{
		if (!callback.Failed && !callback.BudgetDisabled && callback.EventIndex >= 0)
		{
			eventHasCallbacks[static_cast<size_t>(callback.EventIndex)] = true;
		}
	}
}

bool IsKnownEvent(const char* event)
{
	return FindEventIndex(event) >= 0;
}

bool HasCallbacks(const char* event)
{
	if (!active) return false;
	const int index = FindEventIndex(event);
	return index >= 0 && eventHasCallbacks[static_cast<size_t>(index)];
}

bool RuntimeRequested()
{
	if (Args != nullptr && Args->CheckParm("-nopython")) return false;
	return py_enabled || (Args != nullptr && Args->CheckParm("-python"));
}

bool CheckEngineThread()
{
	if (!active)
	{
		PyErr_SetString(PyExc_RuntimeError,
			"the biaseddoom API is unavailable while the interpreter is starting or shutting down");
		return false;
	}
	if (std::this_thread::get_id() == engineThread) return true;
	PyErr_SetString(PyExc_RuntimeError,
		"the biaseddoom API may only be called from the engine scripting thread");
	return false;
}

void EmitBufferedOutput(std::string& buffer, const char* text, bool flush, bool error)
{
	if (text != nullptr) buffer += text;

	size_t newline = 0;
	while ((newline = buffer.find('\n')) != std::string::npos)
	{
		std::string line = buffer.substr(0, newline);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty())
		{
			Printf(error ? TEXTCOLOR_RED "[Python stderr] %s\n" : "[Python] %s\n", line.c_str());
		}
		buffer.erase(0, newline + 1);
	}

	if (flush && !buffer.empty())
	{
		Printf(error ? TEXTCOLOR_RED "[Python stderr] %s\n" : "[Python] %s\n", buffer.c_str());
		buffer.clear();
	}
}

std::string PyString(PyObject* object)
{
	if (object == nullptr) return {};
	PyObject* stringObject = PyObject_Str(object);
	if (stringObject == nullptr) return {};
	const char* text = PyUnicode_AsUTF8(stringObject);
	std::string result = text == nullptr ? std::string() : std::string(text);
	Py_DECREF(stringObject);
	return result;
}

std::string JsonEscape(const char* text)
{
	std::string out;
	if (text == nullptr) return out;
	for (const char* p = text; *p != 0; ++p)
	{
		switch (*p)
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if ((unsigned char)*p >= 0x20) out += *p;
			break;
		}
	}
	return out;
}

// Structured fields appended to each -pyerrorlog JSON line. ExcFile/
// ExcFunc/ExcLine describe the last traceback frame belonging to a user
// script (bootstrap frames use synthetic "<...>" filenames and are skipped).
struct PythonLogRecord
{
	const char* Severity = "error"; // "error" | "warning" | "assert"
	const char* ExcType = "";        // exception class name, when raised by Python
	std::string ExcFile;
	int ExcLine = 0;
	std::string ExcFunc;
};

// append one JSON line to the -pyerrorlog feed (no-op when not configured)
void WritePythonErrorLog(const char* context, const std::string& source,
	const FString& traceback, unsigned repeats, bool heartbeat,
	const PythonLogRecord& record)
{
	if (s_pythonErrorLogPath.empty()) return;

	FILE* file = fopen(s_pythonErrorLogPath.c_str(), "a");
	if (file == nullptr) return;

	const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	const char* map = primaryLevel != nullptr ? primaryLevel->MapName.GetChars() : "";

	fprintf(file,
		"{\"time_ms\":%lld,\"map\":\"%s\",\"context\":\"%s\",\"source\":\"%s\","
		"\"severity\":\"%s\",\"repeats_suppressed\":%u,\"heartbeat\":%s,"
		"\"exc_type\":\"%s\",\"exc_file\":\"%s\",\"exc_line\":%d,\"exc_func\":\"%s\","
		"\"traceback\":\"%s\"}\n",
		(long long)nowMs, JsonEscape(map).c_str(), JsonEscape(context).c_str(),
		JsonEscape(source.c_str()).c_str(), JsonEscape(record.Severity).c_str(),
		repeats, heartbeat ? "true" : "false",
		JsonEscape(record.ExcType).c_str(), JsonEscape(record.ExcFile.c_str()).c_str(),
		record.ExcLine, JsonEscape(record.ExcFunc.c_str()).c_str(),
		JsonEscape(traceback.GetChars()).c_str());
	fclose(file);
}

// Pull the exception class name and the innermost user-script frame out of a
// normalized exception/traceback pair. Introspection failures never replace
// the original error report.
void ExtractTracebackInfo(PyObject* type, PyObject* tracebackObject, PythonLogRecord& record)
{
	if (type != nullptr && PyType_Check(type))
	{
		record.ExcType = reinterpret_cast<PyTypeObject*>(type)->tp_name;
	}
	if (tracebackObject == nullptr || tracebackObject == Py_None) return;
	PyObject* tb = tracebackObject;
	Py_INCREF(tb);
	while (tb != nullptr && tb != Py_None)
	{
		PyObject* frame = PyObject_GetAttrString(tb, "tb_frame");
		PyObject* linenoObject = PyObject_GetAttrString(tb, "tb_lineno");
		PyObject* next = PyObject_GetAttrString(tb, "tb_next");
		if (frame != nullptr && PyFrame_Check(frame))
		{
			PyCodeObject* code = PyFrame_GetCode(reinterpret_cast<PyFrameObject*>(frame));
			if (code != nullptr)
			{
				const char* filename = PyUnicode_AsUTF8(code->co_filename);
				if (filename != nullptr && strchr(filename, '<') == nullptr)
				{
					record.ExcFile = filename;
					const char* funcName = PyUnicode_AsUTF8(code->co_name);
					record.ExcFunc = funcName != nullptr ? funcName : "";
					record.ExcLine = linenoObject != nullptr
						? static_cast<int>(PyLong_AsLong(linenoObject)) : 0;
				}
				Py_DECREF(code);
			}
		}
		Py_XDECREF(frame);
		Py_XDECREF(linenoObject);
		Py_DECREF(tb);
		tb = next;
	}
	PyErr_Clear();
}

void ReportPythonError(const char* context, const std::string& source)
{
	if (!PyErr_Occurred()) return;

	s_pythonErrorCount++;

	PyObject* type = nullptr;
	PyObject* value = nullptr;
	PyObject* tracebackObject = nullptr;
	PyErr_Fetch(&type, &value, &tracebackObject);
	PyErr_NormalizeException(&type, &value, &tracebackObject);

	std::string formatted;
	PyObject* tracebackModule = PyImport_ImportModule("traceback");
	if (tracebackModule != nullptr)
	{
		PyObject* formatter = PyObject_GetAttrString(tracebackModule, "format_exception");
		if (formatter != nullptr && PyCallable_Check(formatter))
		{
			PyObject* lines = PyObject_CallFunctionObjArgs(formatter,
				type == nullptr ? Py_None : type,
				value == nullptr ? Py_None : value,
				tracebackObject == nullptr ? Py_None : tracebackObject,
				nullptr);
			if (lines != nullptr)
			{
				PyObject* separator = PyUnicode_FromString("");
				PyObject* joined = separator == nullptr ? nullptr : PyUnicode_Join(separator, lines);
				if (joined != nullptr)
				{
					const char* text = PyUnicode_AsUTF8(joined);
					if (text != nullptr) formatted = text;
					Py_DECREF(joined);
				}
				Py_XDECREF(separator);
				Py_DECREF(lines);
			}
		}
		Py_XDECREF(formatter);
		Py_DECREF(tracebackModule);
	}
	else
	{
		PyErr_Clear();
	}

	if (formatted.empty())
	{
		formatted = PyString(value != nullptr ? value : type);
	}

	PythonLogRecord record;
	ExtractTracebackInfo(type, tracebackObject, record);

	// flush pending print() output first, so it appears before the error
	EmitBufferedOutput(stdoutBuffer, nullptr, true, false);
	EmitBufferedOutput(stderrBuffer, nullptr, true, true);

	FString full;
	full.Format("Python %s failed%s%s:\n%s%s",
		context,
		source.empty() ? "" : " in ",
		source.empty() ? "" : source.c_str(),
		formatted.c_str(),
		formatted.empty() || formatted.back() == '\n' ? "" : "\n");

	// suppress identical errors (a failing tick handler would otherwise
	// print a full traceback 35 times per second and flood the console)
	if (s_lastPythonError.IsNotEmpty() && full == s_lastPythonError)
	{
		s_repeatPythonErrorCount++;

		// periodic heartbeat, roughly every 10 seconds at full tic rate
		if (s_repeatPythonErrorCount % 350 == 0)
		{
			Printf(TEXTCOLOR_RED "(the previous Python error has now "
				"repeated %u times; duplicates suppressed)\n",
				s_repeatPythonErrorCount);
			WritePythonErrorLog(context, source, s_lastPythonError,
				s_repeatPythonErrorCount, true, record);
		}

		Py_XDECREF(type);
		Py_XDECREF(value);
		Py_XDECREF(tracebackObject);
		PyErr_Clear();
		return;
	}

	if (s_repeatPythonErrorCount > 0)
	{
		Printf(TEXTCOLOR_RED "(the previous Python error repeated %u more "
			"time%s before this one; duplicates suppressed)\n",
			s_repeatPythonErrorCount,
			s_repeatPythonErrorCount == 1 ? "" : "s");
		WritePythonErrorLog(context, source, s_lastPythonError,
			s_repeatPythonErrorCount, true, record);
		s_repeatPythonErrorCount = 0;
	}

	s_lastPythonError = full;

	Printf(TEXTCOLOR_RED "%s", full.GetChars());
	WritePythonErrorLog(context, source, full, 0, false, record);

	Py_XDECREF(type);
	Py_XDECREF(value);
	Py_XDECREF(tracebackObject);
	PyErr_Clear();
}

// Yellow-console warning channel for script-facing issues that are not test
// failures (bd.warn, internal C++ diagnostics). Mirrors ReportPythonError's
// identical-message dedup and heartbeat, keyed separately so warnings and
// errors never suppress each other. Never increments s_pythonErrorCount.
void EmitScriptWarning(const char* message)
{
	FString full;
	full.Format("SCRIPT WARNING: %s", message != nullptr ? message : "");
	if (full.IsEmpty() || full.Back() != '\n') full += '\n';

	PythonLogRecord record;
	record.Severity = "warning";

	if (s_lastPythonWarning.IsNotEmpty() && full == s_lastPythonWarning)
	{
		s_repeatPythonWarningCount++;

		// periodic heartbeat, same cadence as the error dedup
		if (s_repeatPythonWarningCount % 350 == 0)
		{
			Printf(TEXTCOLOR_YELLOW "(the previous Python warning has now "
				"repeated %u times; duplicates suppressed)\n",
				s_repeatPythonWarningCount);
			WritePythonErrorLog("warning", currentSource, s_lastPythonWarning,
				s_repeatPythonWarningCount, true, record);
		}
		return;
	}

	if (s_repeatPythonWarningCount > 0)
	{
		Printf(TEXTCOLOR_YELLOW "(the previous Python warning repeated %u more "
			"time%s before this one; duplicates suppressed)\n",
			s_repeatPythonWarningCount,
			s_repeatPythonWarningCount == 1 ? "" : "s");
		WritePythonErrorLog("warning", currentSource, s_lastPythonWarning,
			s_repeatPythonWarningCount, true, record);
		s_repeatPythonWarningCount = 0;
	}

	s_lastPythonWarning = full;

	// flush pending print() output first, so it appears before the warning
	EmitBufferedOutput(stdoutBuffer, nullptr, true, false);
	EmitBufferedOutput(stderrBuffer, nullptr, true, true);

	Printf(TEXTCOLOR_YELLOW "%s", full.GetChars());
	WritePythonErrorLog("warning", currentSource, full, 0, false, record);
}

bool ValidateStateDictionary()
{
	if (engineModule == nullptr || stateDictionary == nullptr)
	{
		PyErr_SetString(PyExc_RuntimeError, "biaseddoom.state is unavailable");
		return false;
	}
	PyObject* attached = PyObject_GetAttrString(engineModule, "state");
	if (attached == nullptr)
	{
		PyErr_Clear();
		PyErr_SetString(PyExc_TypeError,
			"biaseddoom.state must not be deleted; mutate the existing dictionary in place");
		return false;
	}
	const bool valid = attached == stateDictionary;
	Py_DECREF(attached);
	if (!valid)
	{
		PyErr_SetString(PyExc_TypeError,
			"biaseddoom.state must not be rebound; mutate the existing dictionary in place");
	}
	return valid;
}

bool ValidResourcePath(const std::string& path, bool requirePythonExtension)
{
	if (path.empty() || path.front() == '/' || path.front() == '\\') return false;
	if (path.find('\0') != std::string::npos || path.find("..") != std::string::npos || path.find('\\') != std::string::npos) return false;
	return !requirePythonExtension || (path.size() >= 3 && path.substr(path.size() - 3) == ".py");
}

bool ReadResourceText(int container, const std::string& path, std::string& output, bool requirePythonExtension)
{
	if (!ValidResourcePath(path, requirePythonExtension)) return false;
	const int lump = fileSystem.CheckNumForFullName(path.c_str(), container);
	if (lump < 0) return false;
	auto data = fileSystem.ReadFile(lump);
	output.assign(data.string(), data.size());
	return true;
}

std::string Trim(std::string text)
{
	auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
	text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
	text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
	return text;
}

void DiscoverScripts()
{
	discoveredScripts.clear();
	int lastLump = 0;
	int manifest = -1;
	while ((manifest = fileSystem.FindLump("PYTHON", &lastLump, true)) >= 0)
	{
		const char* manifestPath = fileSystem.GetFileFullName(manifest);
		if (manifestPath == nullptr || stricmp(manifestPath, "PYTHON") != 0) continue;
		const int container = fileSystem.GetFileContainer(manifest);
		const char* resourceName = fileSystem.GetResourceFileFullName(container);
		const char* resourceLabel = resourceName == nullptr ? "<resource>" : resourceName;
		auto data = fileSystem.ReadFile(manifest);
		std::string contents(data.string(), data.size());
		size_t offset = 0;
		unsigned lineNumber = 0;
		while (offset <= contents.size())
		{
			const size_t end = contents.find('\n', offset);
			std::string line = contents.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
			offset = end == std::string::npos ? contents.size() + 1 : end + 1;
			++lineNumber;
			if (lineNumber == 1 && line.size() >= 3 &&
				static_cast<unsigned char>(line[0]) == 0xef &&
				static_cast<unsigned char>(line[1]) == 0xbb &&
				static_cast<unsigned char>(line[2]) == 0xbf)
			{
				line.erase(0, 3);
			}

			const size_t comment = line.find('#');
			if (comment != std::string::npos) line.erase(comment);
			line = Trim(std::move(line));
			if (line.size() >= 2 && ((line.front() == '"' && line.back() == '"') ||
				(line.front() == '\'' && line.back() == '\'')))
			{
				line = line.substr(1, line.size() - 2);
			}
			if (line.empty()) continue;

			if (!ValidResourcePath(line, true))
			{
				Printf(TEXTCOLOR_RED "Invalid Python script path at %s:PYTHON:%u: %s\n",
					resourceLabel, lineNumber, line.c_str());
				continue;
			}
			if (fileSystem.CheckNumForFullName(line.c_str(), container) < 0)
			{
				Printf(TEXTCOLOR_RED "Python script %s was not found in %s (manifest line %u)\n",
					line.c_str(), resourceLabel, lineNumber);
				continue;
			}

			discoveredScripts.push_back({ container, line, resourceLabel });
		}
	}
}

void DictSet(PyObject* dictionary, const char* key, PyObject* value)
{
	if (dictionary == nullptr || value == nullptr)
	{
		Py_XDECREF(value);
		return;
	}
	PyDict_SetItemString(dictionary, key, value);
	Py_DECREF(value);
}

void DictSetString(PyObject* dictionary, const char* key, const char* value)
{
	DictSet(dictionary, key, PyUnicode_FromString(value == nullptr ? "" : value));
}

void DictSetInt(PyObject* dictionary, const char* key, long long value)
{
	DictSet(dictionary, key, PyLong_FromLongLong(value));
}

void DictSetFloat(PyObject* dictionary, const char* key, double value)
{
	DictSet(dictionary, key, PyFloat_FromDouble(value));
}

void DictSetBool(PyObject* dictionary, const char* key, bool value)
{
	DictSet(dictionary, key, PyBool_FromLong(value ? 1 : 0));
}

int ActorPlayerNumber(AActor* actor)
{
	if (actor == nullptr || actor->player == nullptr) return -1;
	for (unsigned i = 0; i < MAXPLAYERS; ++i)
	{
		if (&players[i] == actor->player) return static_cast<int>(i);
	}
	return -1;
}

PyObject* ActorSnapshot(AActor* actor)
{
	if (actor == nullptr || (actor->ObjectFlags & OF_EuthanizeMe))
	{
		Py_RETURN_NONE;
	}

	PyObject* result = PyDict_New();
	DictSetString(result, "class_name", actor->GetClass()->TypeName.GetChars());
	DictSetInt(result, "tid", actor->tid);
	DictSetInt(result, "health", actor->health);
	DictSetFloat(result, "x", actor->X());
	DictSetFloat(result, "y", actor->Y());
	DictSetFloat(result, "z", actor->Z());
	DictSetFloat(result, "angle", actor->Angles.Yaw.Degrees());
	DictSetFloat(result, "pitch", actor->Angles.Pitch.Degrees());
	DictSetFloat(result, "velocity_x", actor->Vel.X);
	DictSetFloat(result, "velocity_y", actor->Vel.Y);
	DictSetFloat(result, "velocity_z", actor->Vel.Z);
	DictSetBool(result, "alive", actor->health > 0);
	DictSetBool(result, "is_monster", (actor->flags3 & MF3_ISMONSTER) != 0);
	DictSetBool(result, "is_player", actor->player != nullptr);
	DictSetInt(result, "player_index", ActorPlayerNumber(actor));
	DictSet(result, "ref", GameApi::MakeActorRef(actor));
	return result;
}

AActor* FindActor(int tid)
{
	if (primaryLevel == nullptr || tid == 0) return nullptr;
	return primaryLevel->GetActorIterator(tid).Next();
}

bool CheckSessionMutationAllowed()
{
	if (netgame || multiplayer || demoplayback || demorecording)
	{
		// The warning channel dedups identical consecutive messages, so
		// repeated blocked calls do not flood the console. The RuntimeError
		// contract for the caller is unchanged.
		ReportScriptWarning("mutation blocked during multiplayer/demo session");
		PyErr_SetString(PyExc_RuntimeError,
			"Python gameplay mutations are disabled in multiplayer and demo sessions to protect synchronization");
		return false;
	}
	return true;
}

// Presentation-only effects (HUD text, screen blends, UI sounds, music, the
// display list) are safe in multiplayer and demo sessions because they only
// touch the local console player's view; they just need an active level.
bool CheckLocalPresentationAllowed()
{
	if (primaryLevel == nullptr || primaryLevel->MapName.IsEmpty())
	{
		PyErr_SetString(PyExc_RuntimeError, "no level is currently active");
		return false;
	}
	return true;
}

bool CheckMutationAllowed()
{
	if (!CheckSessionMutationAllowed()) return false;
	if (gameplayMutationBlocked)
	{
		PyErr_SetString(PyExc_RuntimeError,
			"Python gameplay mutations are unavailable during save and world-unload callbacks");
		return false;
	}
	if (primaryLevel == nullptr || primaryLevel->MapName.IsEmpty())
	{
		PyErr_SetString(PyExc_RuntimeError, "no level is currently active");
		return false;
	}
	return true;
}

bool RegisterCallback(const char* event, PyObject* callable, int container, const std::string& source,
	unsigned every = 1, int priority = 0, const char* className = nullptr,
	int tidFilter = 0, int playerFilter = -1)
{
	const int eventIndex = FindEventIndex(event);
	if (eventIndex < 0)
	{
		PyErr_Format(PyExc_ValueError, "unknown BiasedDoom event '%s'", event);
		return false;
	}
	if (!PyCallable_Check(callable))
	{
		PyErr_SetString(PyExc_TypeError, "callback must be callable");
		return false;
	}
	if (every == 0 || every > 1000000)
	{
		PyErr_SetString(PyExc_ValueError, "callback every must be between 1 and 1000000");
		return false;
	}
	if (playerFilter < -1 || playerFilter >= static_cast<int>(MAXPLAYERS))
	{
		PyErr_Format(PyExc_ValueError, "callback player filter must be -1..%zu", MAXPLAYERS - 1);
		return false;
	}
	PClassActor* classFilter = nullptr;
	if (className != nullptr)
	{
		classFilter = PClass::FindActor(FName(className));
		if (classFilter == nullptr)
		{
			PyErr_Format(PyExc_ValueError, "unknown actor class filter '%s'", className);
			return false;
		}
	}

	for (const Callback& existing : callbacks)
	{
		if (existing.Event == event && existing.Callable == callable) return true;
	}

	Py_INCREF(callable);
	Callback callback;
	callback.Event = event;
	callback.EventIndex = eventIndex;
	callback.Callable = callable;
	callback.Container = container;
	callback.Source = source;
	callback.ClassFilter = classFilter;
	callback.TidFilter = tidFilter;
	callback.PlayerFilter = playerFilter;
	callback.Every = every;
	callback.Priority = priority;
	callbacks.push_back(std::move(callback));
	eventHasCallbacks[static_cast<size_t>(eventIndex)] = true;
	callbacksNeedSort = true;
	return true;
}

PyObject* BuildEvent(const char* name)
{
	PyObject* event = PyDict_New();
	DictSetString(event, "name", name);
	DictSetString(event, "map", primaryLevel == nullptr ? "" : primaryLevel->MapName.GetChars());
	DictSetInt(event, "level_time", primaryLevel == nullptr ? 0 : primaryLevel->time);
	return event;
}

void SortCallbacks()
{
	if (!callbacksNeedSort || callbackDispatchDepth != 0) return;
	std::stable_sort(callbacks.begin(), callbacks.end(), [](const Callback& left, const Callback& right)
	{
		return left.Priority > right.Priority;
	});
	callbacksNeedSort = false;
}

bool CallbackMatches(Callback& callback, AActor* subject, int playerIndex)
{
	if (callback.ClassFilter != nullptr && (subject == nullptr || !subject->IsKindOf(callback.ClassFilter))) return false;
	if (callback.TidFilter != 0 && (subject == nullptr || subject->tid != callback.TidFilter)) return false;
	if (callback.PlayerFilter >= 0)
	{
		const int actualPlayer = playerIndex >= 0 ? playerIndex : ActorPlayerNumber(subject);
		if (actualPlayer != callback.PlayerFilter) return false;
	}
	++callback.Seen;
	return callback.Every == 1 || ((callback.Seen - 1) % callback.Every) == 0;
}

bool IsTickEvent(const char* eventName)
{
	return strcmp(eventName, "pre_tick") == 0 || strcmp(eventName, "tick") == 0 ||
		strcmp(eventName, "post_tick") == 0 || strcmp(eventName, "custom_action") == 0;
}

void InvokeEvent(const char* eventName, PyObject* event, AActor* subject = nullptr, int playerIndex = -1)
{
	if (!active)
	{
		Py_XDECREF(event);
		return;
	}
	if (event == nullptr) event = BuildEvent(eventName);

	SortCallbacks();
	PyObject* arguments = PyTuple_Pack(1, event);
	if (arguments == nullptr)
	{
		Py_DECREF(event);
		ReportPythonError(eventName, "event argument construction");
		return;
	}
	const int previousContainer = currentContainer;
	const std::string previousSource = currentSource;
	// A running callback may import another script whose decorators append to
	// this vector. Iterate only the callbacks present at dispatch start and do
	// not retain references across PyObject_CallObject(), which can reallocate
	// the vector. Newly registered callbacks begin with the next event.
	const size_t callbackCount = callbacks.size();
	++callbackDispatchDepth;
	const bool tickEvent = IsTickEvent(eventName);
	const int eventIndex = FindEventIndex(eventName);
	bool callbackAvailabilityChanged = false;
	for (size_t index = 0; index < callbackCount; ++index)
	{
		if (callbacks[index].Failed || callbacks[index].BudgetDisabled || callbacks[index].EventIndex != eventIndex) continue;
		if (!CallbackMatches(callbacks[index], subject, playerIndex)) continue;
		const uint64_t hardLimit = py_tick_budget_ms > 0
			? static_cast<uint64_t>(static_cast<int>(py_tick_budget_ms)) * 1000u : 0;
		if (tickEvent && py_tick_hard_budget && hardLimit > 0 && tickBudgetMicroseconds >= hardLimit)
		{
			++callbacks[index].BudgetSkips;
			++tickBudgetSkips;
			continue;
		}

		const int container = callbacks[index].Container;
		const std::string source = callbacks[index].Source;
		PyObject* const callable = callbacks[index].Callable;
		currentContainer = container;
		currentSource = source;
		const auto start = std::chrono::steady_clock::now();
		PyObject* result = PyObject_CallObject(callable, arguments);
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start).count();
		++callbacks[index].Calls;
		callbacks[index].TotalMicroseconds += static_cast<uint64_t>(std::max<int64_t>(0, elapsed));
		callbacks[index].MaximumMicroseconds = std::max(callbacks[index].MaximumMicroseconds,
			static_cast<uint64_t>(std::max<int64_t>(0, elapsed)));
		const uint64_t elapsedMicroseconds = static_cast<uint64_t>(std::max<int64_t>(0, elapsed));
		if (tickEvent) tickBudgetMicroseconds += elapsedMicroseconds;
		if (result == nullptr)
		{
			ReportPythonError(eventName, source);
			callbacks[index].Failed = true;
			callbackAvailabilityChanged = true;
			Printf(TEXTCOLOR_YELLOW "Python callback %s in %s has been disabled until py_reload.\n",
				eventName, source.c_str());
		}
		else
		{
			Py_DECREF(result);
		}

		if (tickEvent && py_tick_budget_ms > 0 && elapsed > static_cast<int64_t>(static_cast<int>(py_tick_budget_ms)) * 1000 && callbacks[index].BudgetWarnings < 3)
		{
			++callbacks[index].BudgetWarnings;
			Printf(TEXTCOLOR_YELLOW "Python %s callback in %s took %.3f ms (whole-tic budget: %d ms).\n",
				 eventName, source.c_str(), elapsed / 1000.0, static_cast<int>(py_tick_budget_ms));
		}
		if (tickEvent && py_tick_hard_budget && hardLimit > 0 && elapsedMicroseconds >= hardLimit)
		{
			++callbacks[index].BudgetOverruns;
			++callbacks[index].ConsecutiveOverruns;
			++tickBudgetOverruns;
			const int overrunLimit = static_cast<int>(py_tick_overrun_limit);
			if (overrunLimit > 0 && callbacks[index].ConsecutiveOverruns >= static_cast<unsigned>(overrunLimit))
			{
				callbacks[index].BudgetDisabled = true;
				callbackAvailabilityChanged = true;
				Printf(TEXTCOLOR_YELLOW "Python %s callback in %s exceeded the %d ms budget %u consecutive times and was disabled until py_reload.\n",
					eventName, source.c_str(), static_cast<int>(py_tick_budget_ms),
					callbacks[index].ConsecutiveOverruns);
			}
		}
		else if (tickEvent)
		{
			callbacks[index].ConsecutiveOverruns = 0;
		}
	}
	--callbackDispatchDepth;
	if (callbackAvailabilityChanged) RebuildEventPresence();
	SortCallbacks();
	// Actor APIs can synchronously dispatch nested spawn/death events. Restore
	// the caller's resource context so it can keep using same-mod VFS helpers.
	currentContainer = previousContainer;
	currentSource = previousSource;
	Py_DECREF(arguments);
	Py_DECREF(event);
}

void CleanupScheduledTasks()
{
	if (taskDispatchDepth != 0) return;
	for (ScheduledTask& task : scheduledTasks)
	{
		if (task.Cancelled) Py_CLEAR(task.Callable);
	}
	scheduledTasks.erase(std::remove_if(scheduledTasks.begin(), scheduledTasks.end(),
		[](const ScheduledTask& task) { return task.Cancelled; }), scheduledTasks.end());
}

void CancelMapLocalTasks()
{
	for (ScheduledTask& task : scheduledTasks)
	{
		if (task.MapLocal) task.Cancelled = true;
	}
	CleanupScheduledTasks();
}

void ProcessScheduledTasks()
{
	if (!active || scheduledTasks.empty()) return;
	const size_t taskCount = scheduledTasks.size();
	++taskDispatchDepth;
	for (size_t index = 0; index < taskCount; ++index)
	{
		if (scheduledTasks[index].Cancelled || scheduledTasks[index].DueTick > taskClock) continue;
		if (scheduledTasks[index].MapLocal && scheduledTasks[index].MapSerial != mapSerial)
		{
			scheduledTasks[index].Cancelled = true;
			continue;
		}
		const uint64_t hardLimit = py_tick_budget_ms > 0
			? static_cast<uint64_t>(static_cast<int>(py_tick_budget_ms)) * 1000u : 0;
		if (py_tick_hard_budget && hardLimit > 0 && tickBudgetMicroseconds >= hardLimit)
		{
			++scheduledTasks[index].BudgetSkips;
			++tickBudgetSkips;
			continue;
		}

		const int previousContainer = currentContainer;
		const std::string previousSource = currentSource;
		currentContainer = scheduledTasks[index].Container;
		currentSource = scheduledTasks[index].Source;
		PyObject* callable = scheduledTasks[index].Callable;
		const uint64_t id = scheduledTasks[index].Id;
		const auto start = std::chrono::steady_clock::now();
		PyObject* result = PyObject_CallNoArgs(callable);
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start).count();
		currentContainer = previousContainer;
		currentSource = previousSource;

		// New tasks can reallocate the vector, so reacquire by stable ID.
		auto taskIterator = std::find_if(scheduledTasks.begin(), scheduledTasks.end(),
			[id](const ScheduledTask& task) { return task.Id == id; });
		if (taskIterator == scheduledTasks.end())
		{
			Py_XDECREF(result);
			continue;
		}
		ScheduledTask& task = *taskIterator;
		++task.Calls;
		const uint64_t elapsedUs = static_cast<uint64_t>(std::max<int64_t>(0, elapsed));
		task.TotalMicroseconds += elapsedUs;
		task.MaximumMicroseconds = std::max(task.MaximumMicroseconds, elapsedUs);
		tickBudgetMicroseconds += elapsedUs;
		if (py_tick_hard_budget && hardLimit > 0 && elapsedUs >= hardLimit)
		{
			++task.BudgetOverruns;
			++task.ConsecutiveOverruns;
			++tickBudgetOverruns;
			const int overrunLimit = static_cast<int>(py_tick_overrun_limit);
			if (overrunLimit > 0 && task.ConsecutiveOverruns >= static_cast<unsigned>(overrunLimit))
			{
				task.Cancelled = true;
				Printf(TEXTCOLOR_YELLOW "Python scheduled task in %s exceeded the %d ms budget %u consecutive times and was cancelled.\n",
					task.Source.c_str(), static_cast<int>(py_tick_budget_ms), task.ConsecutiveOverruns);
			}
		}
		else
		{
			task.ConsecutiveOverruns = 0;
		}
		if (result == nullptr)
		{
			ReportPythonError("scheduled task", task.Source);
			task.Cancelled = true;
		}
		else
		{
			if (result == Py_False) task.Cancelled = true;
			Py_DECREF(result);
		}
		if (!task.Cancelled)
		{
			if (task.Interval == 0) task.Cancelled = true;
			else task.DueTick = taskClock + task.Interval;
		}
	}
	--taskDispatchDepth;
	CleanupScheduledTasks();
}

PyObject* PyBdLog(PyObject*, PyObject* args, PyObject* kwargs)
{
	if (!CheckEngineThread()) return nullptr;
	PyObject* message = nullptr;
	const char* level = "info";
	static const char* keywords[] = { "message", "level", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|s:log", const_cast<char**>(keywords), &message, &level)) return nullptr;
	const std::string text = PyString(message);
	if (PyErr_Occurred()) return nullptr;
	const bool isError = stricmp(level, "error") == 0;
	const bool isWarning = stricmp(level, "warning") == 0 || stricmp(level, "warn") == 0;
	if (isError) Printf(TEXTCOLOR_RED "[Python:error] %s\n", text.c_str());
	else if (isWarning) Printf(TEXTCOLOR_YELLOW "[Python:warning] %s\n", text.c_str());
	else Printf("[Python:%s] %s\n", level, text.c_str());
	Py_RETURN_NONE;
}

PyObject* PyBdWriteOutput(PyObject*, PyObject* args)
{
	if (!CheckEngineThread()) return nullptr;
	const char* text = nullptr;
	int stream = 0;
	int flush = 0;
	if (!PyArg_ParseTuple(args, "sii:_write_output", &text, &stream, &flush)) return nullptr;
	EmitBufferedOutput(stream == 0 ? stdoutBuffer : stderrBuffer, text, flush != 0, stream != 0);
	Py_RETURN_NONE;
}

PyObject* PyBdRegisterCallback(PyObject*, PyObject* args, PyObject* kwargs)
{
	if (!CheckEngineThread()) return nullptr;
	const char* event = nullptr;
	PyObject* callable = nullptr;
	unsigned every = 1;
	int priority = 0;
	const char* className = nullptr;
	int tid = 0;
	int player = -1;
	static const char* keywords[] = { "event", "callback", "every", "priority", "class_name", "tid", "player", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO|Iizii:_register_callback", const_cast<char**>(keywords),
		&event, &callable, &every, &priority, &className, &tid, &player)) return nullptr;
	if (!RegisterCallback(event, callable, currentContainer, currentSource, every, priority, className, tid, player)) return nullptr;
	Py_RETURN_NONE;
}

PyObject* PyBdCurrentMap(PyObject*, PyObject*)
{
	if (!CheckEngineThread()) return nullptr;
	if (primaryLevel == nullptr || primaryLevel->MapName.IsEmpty()) Py_RETURN_NONE;
	return PyUnicode_FromString(primaryLevel->MapName.GetChars());
}

PyObject* PyBdLevelTime(PyObject*, PyObject*)
{
	if (!CheckEngineThread()) return nullptr;
	return PyLong_FromLong(primaryLevel == nullptr ? 0 : primaryLevel->time);
}

PyObject* PyBdHeadless(PyObject*, PyObject*)
{
	if (!CheckEngineThread()) return nullptr;
	return PyBool_FromLong(I_IsHeadless() ? 1 : 0);
}

PyObject* PyBdPlayers(PyObject*, PyObject*)
{
	if (!CheckEngineThread()) return nullptr;
	PyObject* result = PyList_New(0);
	for (unsigned i = 0; i < MAXPLAYERS; ++i)
	{
		if (!playeringame[i]) continue;
		PyObject* player = PyDict_New();
		DictSetInt(player, "index", i);
		DictSetString(player, "name", players[i].userinfo.GetName());
		DictSetBool(player, "in_game", true);
		DictSet(player, "actor", ActorSnapshot(players[i].mo));
		PyList_Append(result, player);
		Py_DECREF(player);
	}
	return result;
}

PyObject* PyBdActors(PyObject*, PyObject* args, PyObject* kwargs)
{
	if (!CheckEngineThread()) return nullptr;
	const char* className = nullptr;
	int tid = 0;
	int limit = 1024;
	static const char* keywords[] = { "class_name", "tid", "limit", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|zii:actors", const_cast<char**>(keywords), &className, &tid, &limit)) return nullptr;
	if (limit < 1 || limit > 100000)
	{
		PyErr_SetString(PyExc_ValueError, "limit must be between 1 and 100000");
		return nullptr;
	}

	PClassActor* classFilter = nullptr;
	if (className != nullptr)
	{
		classFilter = PClass::FindActor(FName(className));
		if (classFilter == nullptr)
		{
			PyErr_Format(PyExc_ValueError, "unknown actor class '%s'", className);
			return nullptr;
		}
	}

	PyObject* result = PyList_New(0);
	if (primaryLevel == nullptr) return result;
	if (tid != 0)
	{
		auto iterator = primaryLevel->GetActorIterator(tid);
		AActor* actor = nullptr;
		while (PyList_Size(result) < limit && (actor = iterator.Next()) != nullptr)
		{
			if (classFilter != nullptr && !actor->IsKindOf(classFilter)) continue;
			PyObject* snapshot = ActorSnapshot(actor);
			PyList_Append(result, snapshot);
			Py_DECREF(snapshot);
		}
	}
	else
	{
		auto iterator = primaryLevel->GetThinkerIterator<AActor>();
		AActor* actor = nullptr;
		while (PyList_Size(result) < limit && (actor = iterator.Next()) != nullptr)
		{
			if (classFilter != nullptr && !actor->IsKindOf(classFilter)) continue;
			PyObject* snapshot = ActorSnapshot(actor);
			PyList_Append(result, snapshot);
			Py_DECREF(snapshot);
		}
	}
	return result;
}

PyObject* PyBdActor(PyObject*, PyObject* args)
{
	if (!CheckEngineThread()) return nullptr;
	int tid = 0;
	if (!PyArg_ParseTuple(args, "i:actor", &tid)) return nullptr;
	return ActorSnapshot(FindActor(tid));
}

PyObject* PyBdSpawnActor(PyObject*, PyObject* args, PyObject* kwargs)
{
	if (!CheckEngineThread()) return nullptr;
	const char* className = nullptr;
	double x = 0, y = 0, z = 0, angle = 0;
	int tid = 0;
	int force = 0;
	static const char* keywords[] = { "class_name", "x", "y", "z", "angle", "tid", "force", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sddd|dii:spawn_actor", const_cast<char**>(keywords),
		&className, &x, &y, &z, &angle, &tid, &force)) return nullptr;
	if (!CheckMutationAllowed()) return nullptr;

	PClassActor* actorClass = PClass::FindActor(FName(className));
	if (actorClass == nullptr)
	{
		PyErr_Format(PyExc_ValueError, "unknown actor class '%s'", className);
		return nullptr;
	}
	AActor* actor = Spawn(primaryLevel, actorClass, DVector3(x, y, z), ALLOW_REPLACE);
	if (actor == nullptr)
	{
		PyErr_SetString(PyExc_RuntimeError, "actor creation failed");
		return nullptr;
	}
	if (!force && !P_TestMobjLocation(actor))
	{
		actor->ClearCounters();
		actor->Destroy();
		PyErr_SetString(PyExc_RuntimeError, "actor does not fit at the requested position; pass force=True to override");
		return nullptr;
	}
	actor->Angles.Yaw = DAngle::fromDeg(angle);
	if (tid == 0) tid = primaryLevel->FindUniqueTID(10000, 100000, false);
	if (tid == 0)
	{
		actor->Destroy();
		PyErr_SetString(PyExc_RuntimeError, "could not allocate a unique TID");
		return nullptr;
	}
	actor->SetTID(tid);
	return ActorSnapshot(actor);
}

// actor class registry for the bootstrap's bd.actors helper: returns a
// list of (class_name, parent_name, kind_mask) tuples for every
// non-abstract Actor descendant known to the engine (including mod- and
// script-defined classes). kind_mask bits: 1=monster, 2=projectile,
// 4=weapon, 8=inventory item, 16=player pawn.
PyObject* PyBdActorClassInfo(PyObject*, PyObject*)
{
	if (!CheckEngineThread()) return nullptr;

	static PClassActor* actorBase = PClass::FindActor("Actor");
	static PClassActor* weaponBase = PClass::FindActor("Weapon");
	static PClassActor* inventoryBase = PClass::FindActor("Inventory");
	static PClassActor* playerBase = PClass::FindActor("PlayerPawn");

	PyObject* result = PyList_New(0);
	if (result == nullptr) return nullptr;

	for (PClass* cls : PClass::AllClasses)
	{
		if (cls == nullptr || cls->bAbstract) continue;
		if (actorBase != nullptr && !cls->IsDescendantOf(actorBase)) continue;

		int kind = 0;
		const AActor* def = (const AActor*)cls->Defaults;
		if (def != nullptr)
		{
			if ((def->flags & MF_SHOOTABLE) && (def->flags & MF_COUNTKILL)) kind |= 1;
			if (def->flags & MF_MISSILE) kind |= 2;
		}
		if (weaponBase != nullptr && cls->IsDescendantOf(weaponBase)) kind |= 4;
		if (inventoryBase != nullptr && cls->IsDescendantOf(inventoryBase)) kind |= 8;
		if (playerBase != nullptr && cls->IsDescendantOf(playerBase)) kind |= 16;

		const char* parent = cls->ParentClass != nullptr ? cls->ParentClass->TypeName.GetChars() : "";
		PyObject* entry = Py_BuildValue("(ssi)", cls->TypeName.GetChars(), parent, kind);
		if (entry == nullptr || PyList_Append(result, entry) < 0)
		{
			Py_XDECREF(entry);
			Py_DECREF(result);
			return nullptr;
		}
		Py_DECREF(entry);
	}

	return result;
}

PyObject* PyBdDamageActor(PyObject*, PyObject* args, PyObject* kwargs)
{
	if (!CheckEngineThread()) return nullptr;
	int tid = 0;
	int damage = 0;
	const char* damageType = "None";
	static const char* keywords[] = { "tid", "damage", "damage_type", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ii|s:damage_actor", const_cast<char**>(keywords),
		&tid, &damage, &damageType)) return nullptr;
	if (!CheckMutationAllowed()) return nullptr;
	AActor* actor = FindActor(tid);
	if (actor == nullptr)
	{
		PyErr_Format(PyExc_LookupError, "no actor with TID %d", tid);
		return nullptr;
	}
	return PyLong_FromLong(P_DamageMobj(actor, nullptr, nullptr, damage, FName(damageType)));
}

PyObject* PyBdSetActorVelocity(PyObject*, PyObject* args)
{
	if (!CheckEngineThread()) return nullptr;
	int tid = 0;
	double x = 0, y = 0, z = 0;
	if (!PyArg_ParseTuple(args, "iddd:set_actor_velocity", &tid, &x, &y, &z)) return nullptr;
	if (!CheckMutationAllowed()) return nullptr;
	AActor* actor = FindActor(tid);
	if (actor == nullptr)
	{
		PyErr_Format(PyExc_LookupError, "no actor with TID %d", tid);
		return nullptr;
	}
	actor->Vel = DVector3(x, y, z);
	return ActorSnapshot(actor);
}

PyObject* PyBdDestroyActor(PyObject*, PyObject* args)
{
	if (!CheckEngineThread()) return nullptr;
	int tid = 0;
	if (!PyArg_ParseTuple(args, "i:destroy_actor", &tid)) return nullptr;
	if (!CheckMutationAllowed()) return nullptr;
	AActor* actor = FindActor(tid);
	if (actor == nullptr) Py_RETURN_FALSE;
	actor->ClearCounters();
	actor->Destroy();
	Py_RETURN_TRUE;
}

PyObject* CVarToPython(FBaseCVar* cvar)
{
	switch (cvar->GetRealType())
	{
	case CVAR_Bool: return PyBool_FromLong(cvar->GetGenericRep(CVAR_Bool).Bool ? 1 : 0);
	case CVAR_Int:
	case CVAR_Color: return PyLong_FromLong(cvar->GetGenericRep(CVAR_Int).Int);
	case CVAR_Float: return PyFloat_FromDouble(cvar->GetGenericRep(CVAR_Float).Float);
	default: return PyUnicode_FromString(cvar->GetGenericRep(CVAR_String).String);
	}
}

PyObject* PyBdGetCVar(PyObject*, PyObject* args)
{
	if (!CheckEngineThread()) return nullptr;
	const char* name = nullptr;
	if (!PyArg_ParseTuple(args, "s:get_cvar", &name)) return nullptr;
	FBaseCVar* cvar = FindCVar(name, nullptr);
	if (cvar == nullptr)
	{
		PyErr_Format(PyExc_KeyError, "unknown CVar '%s'", name);
		return nullptr;
	}
	return CVarToPython(cvar);
}

PyObject* PyBdSetCVar(PyObject*, PyObject* args)
{
	if (!CheckEngineThread()) return nullptr;
	const char* name = nullptr;
	PyObject* value = nullptr;
	if (!PyArg_ParseTuple(args, "sO:set_cvar", &name, &value)) return nullptr;
	if (!CheckSessionMutationAllowed()) return nullptr;
	FBaseCVar* cvar = FindCVar(name, nullptr);
	if (cvar == nullptr)
	{
		PyErr_Format(PyExc_KeyError, "unknown CVar '%s'", name);
		return nullptr;
	}
	const int flags = cvar->GetFlags();
	if (flags & (CVAR_NOSET | CVAR_SYSTEM_ONLY | CVAR_IGNORE))
	{
		PyErr_Format(PyExc_PermissionError, "CVar '%s' is not writable by mod scripts", name);
		return nullptr;
	}
	if ((flags & CVAR_CHEAT) && sysCallbacks.CheckCheatmode != nullptr &&
		sysCallbacks.CheckCheatmode(false, false))
	{
		PyErr_Format(PyExc_PermissionError, "CVar '%s' is currently cheat-protected", name);
		return nullptr;
	}

	switch (cvar->GetRealType())
	{
	case CVAR_Bool:
	{
		const int truth = PyObject_IsTrue(value);
		if (truth < 0) return nullptr;
		cvar->SetGenericRep(UCVarValue(truth != 0), CVAR_Bool);
		break;
	}
	case CVAR_Int:
	case CVAR_Color:
	{
		const long number = PyLong_AsLong(value);
		if (PyErr_Occurred()) return nullptr;
		if (number < INT_MIN || number > INT_MAX)
		{
			PyErr_SetString(PyExc_OverflowError, "CVar integer is outside the engine's 32-bit range");
			return nullptr;
		}
		cvar->SetGenericRep(UCVarValue(static_cast<int>(number)), CVAR_Int);
		break;
	}
	case CVAR_Float:
	{
		const double number = PyFloat_AsDouble(value);
		if (PyErr_Occurred()) return nullptr;
		cvar->SetGenericRep(UCVarValue(number), CVAR_Float);
		break;
	}
	default:
	{
		const std::string text = PyString(value);
		if (PyErr_Occurred()) return nullptr;
		cvar->SetGenericRep(UCVarValue(text.c_str()), CVAR_String);
		break;
	}
	}
	return CVarToPython(cvar);
}

PyObject* PyBdExecute(PyObject*, PyObject* args)
{
	if (!CheckEngineThread()) return nullptr;
	const char* command = nullptr;
	if (!PyArg_ParseTuple(args, "s:execute", &command)) return nullptr;
	if (!CheckSessionMutationAllowed()) return nullptr;
	AddCommandString(command);
	Py_RETURN_NONE;
}

PyObject* PyBdExecuteACS(PyObject*, PyObject* args, PyObject* kwargs)
{
	if (!CheckEngineThread()) return nullptr;
	PyObject* scriptObject = nullptr;
	PyObject* argumentsObject = Py_None;
	int always = 0;
	int wantResult = 0;
	static const char* keywords[] = { "script", "arguments", "always", "want_result", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|Opp:execute_acs", const_cast<char**>(keywords),
		&scriptObject, &argumentsObject, &always, &wantResult)) return nullptr;
	if (!CheckMutationAllowed()) return nullptr;

	int script = 0;
	if (PyLong_Check(scriptObject))
	{
		const long number = PyLong_AsLong(scriptObject);
		if (PyErr_Occurred()) return nullptr;
		if (number < INT_MIN || number > INT_MAX)
		{
			PyErr_SetString(PyExc_OverflowError, "ACS script number is outside the engine's 32-bit range");
			return nullptr;
		}
		script = static_cast<int>(number);
	}
	else if (PyUnicode_Check(scriptObject))
	{
		const char* name = PyUnicode_AsUTF8(scriptObject);
		if (name == nullptr) return nullptr;
		script = -FName(name).GetIndex();
	}
	else
	{
		PyErr_SetString(PyExc_TypeError, "script must be an integer or string");
		return nullptr;
	}

	int acsArguments[4] = { 0, 0, 0, 0 };
	int argumentCount = 0;
	if (argumentsObject != Py_None)
	{
		PyObject* sequence = PySequence_Fast(argumentsObject, "arguments must be a sequence of at most four integers");
		if (sequence == nullptr) return nullptr;
		const Py_ssize_t sequenceSize = PySequence_Fast_GET_SIZE(sequence);
		if (sequenceSize > 4)
		{
			Py_DECREF(sequence);
			PyErr_SetString(PyExc_ValueError, "ACS accepts at most four arguments");
			return nullptr;
		}
		argumentCount = static_cast<int>(sequenceSize);
		for (int i = 0; i < argumentCount; ++i)
		{
			const long number = PyLong_AsLong(PySequence_Fast_GET_ITEM(sequence, i));
			if (PyErr_Occurred())
			{
				Py_DECREF(sequence);
				return nullptr;
			}
			if (number < INT_MIN || number > INT_MAX)
			{
				Py_DECREF(sequence);
				PyErr_SetString(PyExc_OverflowError, "ACS argument is outside the engine's 32-bit range");
				return nullptr;
			}
			acsArguments[i] = static_cast<int>(number);
		}
		Py_DECREF(sequence);
	}

	int flags = always ? ACS_ALWAYS : 0;
	if (wantResult) flags |= ACS_ALWAYS | ACS_WANTRESULT;
	AActor* activator = (consoleplayer >= 0 && static_cast<unsigned>(consoleplayer) < MAXPLAYERS)
		? players[consoleplayer].mo : nullptr;
	const int result = P_StartScript(primaryLevel, activator, nullptr, script,
		primaryLevel->MapName.GetChars(), acsArguments, argumentCount, flags);
	return wantResult ? PyLong_FromLong(result) : PyBool_FromLong(result != 0);
}

PyObject* PyBdReadText(PyObject*, PyObject* args)
{
	if (!CheckEngineThread()) return nullptr;
	const char* path = nullptr;
	if (!PyArg_ParseTuple(args, "s:read_text", &path)) return nullptr;
	if (currentContainer < 0)
	{
		PyErr_SetString(PyExc_RuntimeError, "read_text must be called while a mod script or callback is executing");
		return nullptr;
	}
	std::string source;
	if (!ReadResourceText(currentContainer, path, source, false))
	{
		PyErr_Format(PyExc_FileNotFoundError, "resource '%s' was not found in the current mod", path);
		return nullptr;
	}
	return PyUnicode_DecodeUTF8(source.data(), static_cast<Py_ssize_t>(source.size()), "strict");
}

// Binary companion of read_text: same in-mod-only VFS scoping (no absolute
// paths, no '..', no backslashes) and the same FileNotFoundError contract,
// but returns bytes and caps the payload at 32 MiB since binary resources
// (e.g. font files) are far larger than text scripts.
PyObject* PyBdReadBytes(PyObject*, PyObject* args)
{
	if (!CheckEngineThread()) return nullptr;
	const char* path = nullptr;
	if (!PyArg_ParseTuple(args, "s:read_bytes", &path)) return nullptr;
	if (currentContainer < 0)
	{
		PyErr_SetString(PyExc_RuntimeError, "read_bytes must be called while a mod script or callback is executing");
		return nullptr;
	}
	constexpr size_t kMaxReadBytesSize = 32u * 1024u * 1024u;
	if (!ValidResourcePath(path, false))
	{
		PyErr_Format(PyExc_FileNotFoundError, "resource '%s' was not found in the current mod", path);
		return nullptr;
	}
	const int lump = fileSystem.CheckNumForFullName(path, currentContainer);
	if (lump < 0)
	{
		PyErr_Format(PyExc_FileNotFoundError, "resource '%s' was not found in the current mod", path);
		return nullptr;
	}
	auto data = fileSystem.ReadFile(lump);
	if (data.size() > kMaxReadBytesSize)
	{
		PyErr_Format(PyExc_ValueError, "resource '%s' exceeds the 32 MiB read_bytes limit", path);
		return nullptr;
	}
	return PyBytes_FromStringAndSize(data.string(), static_cast<Py_ssize_t>(data.size()));
}

PyObject* ExecuteResourceModule(int container, const std::string& path, const std::string& moduleName, bool registerNamed);

// Reserved or outright dangerous import_script module names. "biaseddoom" is
// the engine module itself and the bd_* namespace is reserved for the
// engine-shipped framework packages; the remaining entries are stdlib modules
// whose replacement with a VFS module would break the interpreter or mod
// scripts in non-obvious ways. Occupied-name conflicts with any other module
// are caught separately by the sys.modules check in PyBdImportScript.
bool IsDeniedVfsModuleName(const std::string& name)
{
	if (name == "biaseddoom" || name.compare(0, 3, "bd_") == 0) return true;
	static const char* const denied[] = {
		"sys", "sysconfig", "builtins", "io", "os", "os.path", "posix", "nt",
		"errno", "signal", "select", "socket", "ssl", "subprocess", "importlib",
		"json", "math", "random", "re", "time", "datetime", "threading",
		"_thread", "ctypes", "marshal", "gc", "site", "encodings", "codecs",
		"warnings", "types", "typing", "collections", "collections.abc",
		"itertools", "functools", "operator", "weakref", "struct", "pickle",
		"abc", "traceback", "linecache", "heapq", "copyreg", "sre_compile",
		"stat", "genericpath", "posixpath", "ntpath", "contextlib", "enum",
		"pathlib", "tempfile", "shutil", "keyword", "reprlib",
	};
	for (const char* entry : denied)
	{
		if (name == entry) return true;
	}
	return false;
}

// True when module is an earlier import_script module loaded from the same VFS
// path (its __file__ is vfs://<path>): an explicit re-import/reload of the
// same resource, which is allowed to reuse the name. Anything else occupying
// the name (stdlib, engine module, another VFS path) is a refusal.
bool IsSameVfsModulePath(PyObject* module, const char* path)
{
	if (!PyModule_Check(module)) return false;
	PyObject* file = PyObject_GetAttrString(module, "__file__");
	if (file == nullptr)
	{
		PyErr_Clear();
		return false;
	}
	const char* text = PyUnicode_Check(file) ? PyUnicode_AsUTF8(file) : nullptr;
	const bool same = text != nullptr && std::string("vfs://") + path == text;
	Py_DECREF(file);
	return same;
}

// import_script without an explicit name gets a fresh sys.modules entry on
// every call (biaseddoom_vfs_<container>_<counter>): the old fixed
// "biaseddoom_vfs_helper" name silently replaced the previous module,
// breaking siblings that still held the earlier object.
std::string UniqueVfsModuleName(int container)
{
	static unsigned long long counter = 0;
	PyObject* sysModules = PyImport_GetModuleDict();
	for (;;)
	{
		std::string candidate = "biaseddoom_vfs_" + std::to_string(container) + "_" + std::to_string(++counter);
		if (sysModules == nullptr || PyDict_GetItemString(sysModules, candidate.c_str()) == nullptr)
		{
			return candidate;
		}
	}
}

PyObject* PyBdImportScript(PyObject*, PyObject* args, PyObject* kwargs)
{
	if (!CheckEngineThread()) return nullptr;
	const char* path = nullptr;
	const char* requestedName = nullptr;
	static const char* keywords[] = { "path", "module_name", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|z:import_script", const_cast<char**>(keywords),
		&path, &requestedName)) return nullptr;
	if (currentContainer < 0)
	{
		PyErr_SetString(PyExc_RuntimeError, "import_script must be called while a mod script or callback is executing");
		return nullptr;
	}
	std::string moduleName;
	if (requestedName != nullptr)
	{
		if (IsDeniedVfsModuleName(requestedName))
		{
			PyErr_Format(PyExc_ValueError, "module_name '%s' is reserved and cannot be used for import_script", requestedName);
			return nullptr;
		}
		PyObject* sysModules = PyImport_GetModuleDict();
		PyObject* existing = sysModules == nullptr ? nullptr : PyDict_GetItemString(sysModules, requestedName);
		if (existing != nullptr && !IsSameVfsModulePath(existing, path))
		{
			PyErr_Format(PyExc_RuntimeError, "module_name '%s' is already used by a module from a different path", requestedName);
			return nullptr;
		}
		moduleName = requestedName;
	}
	else
	{
		moduleName = UniqueVfsModuleName(currentContainer);
	}
	return ExecuteResourceModule(currentContainer, path, moduleName, false);
}

PyObject* PyBdSchedule(PyObject*, PyObject* args, PyObject* kwargs)
{
	if (!CheckEngineThread()) return nullptr;
	PyObject* callable = nullptr;
	unsigned long long delay = 1;
	unsigned long long repeat = 0;
	int mapLocal = 1;
	static const char* keywords[] = { "callback", "delay", "repeat", "map_local", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|KKp:schedule", const_cast<char**>(keywords),
		&callable, &delay, &repeat, &mapLocal)) return nullptr;
	if (!PyCallable_Check(callable))
	{
		PyErr_SetString(PyExc_TypeError, "scheduled callback must be callable");
		return nullptr;
	}
	const int taskLimit = std::clamp(static_cast<int>(py_max_tasks), 1, 100000);
	if (static_cast<int>(scheduledTasks.size()) >= taskLimit)
	{
		PyErr_Format(PyExc_RuntimeError, "Python scheduled-task limit (%d) reached", taskLimit);
		return nullptr;
	}
	if (delay > 0x7fffffffffffffffULL || repeat > 0x7fffffffffffffffULL)
	{
		PyErr_SetString(PyExc_OverflowError, "task delays must fit in a signed 63-bit tic count");
		return nullptr;
	}
	if (nextTaskId == 0) ++nextTaskId;
	ScheduledTask task;
	task.Id = nextTaskId++;
	task.DueTick = taskClock + delay;
	task.Interval = repeat;
	task.Callable = Py_NewRef(callable);
	task.Container = currentContainer;
	task.Source = currentSource;
	task.MapLocal = mapLocal != 0;
	const bool levelActive = primaryLevel != nullptr && primaryLevel->MapName.IsNotEmpty();
	task.MapSerial = mapSerial + (task.MapLocal && !levelActive ? 1 : 0);
	const uint64_t id = task.Id;
	scheduledTasks.push_back(std::move(task));
	return PyLong_FromUnsignedLongLong(id);
}

PyObject* PyBdCancelTask(PyObject*, PyObject* args)
{
	if (!CheckEngineThread()) return nullptr;
	unsigned long long id = 0;
	if (!PyArg_ParseTuple(args, "K:cancel_task", &id)) return nullptr;
	for (ScheduledTask& task : scheduledTasks)
	{
		if (task.Id != id || task.Cancelled) continue;
		task.Cancelled = true;
		CleanupScheduledTasks();
		Py_RETURN_TRUE;
	}
	Py_RETURN_FALSE;
}

PyObject* PyBdTaskCount(PyObject*, PyObject*)
{
	if (!CheckEngineThread()) return nullptr;
	Py_ssize_t count = 0;
	for (const ScheduledTask& task : scheduledTasks)
	{
		if (!task.Cancelled) ++count;
	}
	return PyLong_FromSsize_t(count);
}

PyObject* PyBdProfile(PyObject*, PyObject*)
{
	if (!CheckEngineThread()) return nullptr;
	PyObject* result = PyDict_New();
	PyObject* entries = PyList_New(0);
	if (result == nullptr || entries == nullptr)
	{
		Py_XDECREF(result);
		Py_XDECREF(entries);
		return nullptr;
	}
	for (const Callback& callback : callbacks)
	{
		PyObject* entry = Py_BuildValue("{s:s,s:s,s:K,s:K,s:K,s:K,s:K,s:I,s:i,s:i,s:i}",
			"event", callback.Event.c_str(),
			"source", callback.Source.c_str(),
			"calls", callback.Calls,
			"total_us", callback.TotalMicroseconds,
			"max_us", callback.MaximumMicroseconds,
			"budget_skips", callback.BudgetSkips,
			"budget_overruns", callback.BudgetOverruns,
			"every", callback.Every,
			"priority", callback.Priority,
			"failed", callback.Failed ? 1 : 0,
			"budget_disabled", callback.BudgetDisabled ? 1 : 0);
		if (entry == nullptr || PyList_Append(entries, entry) < 0)
		{
			Py_XDECREF(entry);
			Py_DECREF(entries);
			Py_DECREF(result);
			return nullptr;
		}
		Py_DECREF(entry);
	}
	PyDict_SetItemString(result, "callbacks", entries);
	Py_DECREF(entries);
	PyObject* tasks = PyList_New(0);
	if (tasks == nullptr) { Py_DECREF(result); return nullptr; }
	for (const ScheduledTask& task : scheduledTasks)
	{
		if (task.Cancelled) continue;
		PyObject* entry = Py_BuildValue("{s:K,s:s,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:O}",
			"id", task.Id,
			"source", task.Source.c_str(),
			"due_tick", task.DueTick,
			"repeat", task.Interval,
			"calls", task.Calls,
			"total_us", task.TotalMicroseconds,
			"max_us", task.MaximumMicroseconds,
			"budget_skips", task.BudgetSkips,
			"budget_overruns", task.BudgetOverruns,
			"map_local", task.MapLocal ? Py_True : Py_False);
		if (entry == nullptr || PyList_Append(tasks, entry) < 0)
		{
			Py_XDECREF(entry); Py_DECREF(tasks); Py_DECREF(result); return nullptr;
		}
		Py_DECREF(entry);
	}
	PyDict_SetItemString(result, "tasks", tasks);
	Py_DECREF(tasks);
	PyObject* budget = PyLong_FromLong(static_cast<int>(py_tick_budget_ms));
	PyObject* hard = PyBool_FromLong(py_tick_hard_budget ? 1 : 0);
	PyObject* overrunLimit = PyLong_FromLong(static_cast<int>(py_tick_overrun_limit));
	PyObject* overruns = PyLong_FromUnsignedLongLong(tickBudgetOverruns);
	PyObject* skips = PyLong_FromUnsignedLongLong(tickBudgetSkips);
	if (budget != nullptr) { PyDict_SetItemString(result, "tick_budget_ms", budget); Py_DECREF(budget); }
	if (hard != nullptr) { PyDict_SetItemString(result, "hard_budget", hard); Py_DECREF(hard); }
	if (overrunLimit != nullptr) { PyDict_SetItemString(result, "overrun_limit", overrunLimit); Py_DECREF(overrunLimit); }
	if (overruns != nullptr) { PyDict_SetItemString(result, "budget_overruns", overruns); Py_DECREF(overruns); }
	if (skips != nullptr) { PyDict_SetItemString(result, "budget_skips", skips); Py_DECREF(skips); }
	return result;
}

PyObject* PyBdResetProfile(PyObject*, PyObject*)
{
	if (!CheckEngineThread()) return nullptr;
	for (Callback& callback : callbacks)
	{
		callback.Calls = 0;
		callback.TotalMicroseconds = 0;
		callback.MaximumMicroseconds = 0;
		callback.BudgetSkips = 0;
		callback.BudgetOverruns = 0;
		callback.BudgetWarnings = 0;
		callback.ConsecutiveOverruns = 0;
	}
	for (ScheduledTask& task : scheduledTasks)
	{
		task.Calls = 0;
		task.TotalMicroseconds = 0;
		task.MaximumMicroseconds = 0;
		task.BudgetSkips = 0;
		task.BudgetOverruns = 0;
		task.ConsecutiveOverruns = 0;
	}
	tickBudgetOverruns = 0;
	tickBudgetSkips = 0;
	Py_RETURN_NONE;
}

//---------------------------------------------------------------------------
// bd.assert_true(cond, msg="") - scripted test assertions. A failure prints
// in red, feeds -pyerrorlog with severity "assert", and increments the
// -scripttest error count, but returns None instead of raising so a test
// script can report several failures in one run.
//---------------------------------------------------------------------------

PyObject* PyBdAssertTrue(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* condition = nullptr;
	const char* message = "";
	static const char* keywords[] = { "cond", "msg", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|s:assert_true",
		const_cast<char**>(keywords), &condition, &message)) return nullptr;
	if (!CheckEngineThread()) return nullptr;
	const int truthy = PyObject_IsTrue(condition);
	if (truthy < 0) return nullptr;
	if (truthy) Py_RETURN_NONE;

	s_pythonErrorCount++;

	// caller location: PyEval_GetFrame returns the Python frame that invoked
	// this C function
	std::string file;
	std::string func;
	int line = 0;
	PyFrameObject* frame = PyEval_GetFrame();
	if (frame != nullptr)
	{
		PyCodeObject* code = PyFrame_GetCode(frame);
		if (code != nullptr)
		{
			const char* filename = PyUnicode_AsUTF8(code->co_filename);
			if (filename != nullptr) file = filename;
			const char* funcName = PyUnicode_AsUTF8(code->co_name);
			if (funcName != nullptr) func = funcName;
			Py_DECREF(code);
		}
		line = PyFrame_GetLineNumber(frame);
	}
	// Location introspection must never leak a secondary error into the
	// Py_None return below.
	PyErr_Clear();

	// flush pending print() output first, so it appears before the failure
	EmitBufferedOutput(stdoutBuffer, nullptr, true, false);
	EmitBufferedOutput(stderrBuffer, nullptr, true, true);

	FString full;
	if (message[0] != 0) full.Format("SCRIPT ASSERT FAILED: %s", message);
	else full = "SCRIPT ASSERT FAILED";
	if (!file.empty()) full.AppendFormat(" (%s:%d)", file.c_str(), line);
	full += '\n';

	Printf(TEXTCOLOR_RED "%s", full.GetChars());

	PythonLogRecord record;
	record.Severity = "assert";
	record.ExcFile = file;
	record.ExcLine = line;
	record.ExcFunc = func;
	WritePythonErrorLog("assert", file.empty() ? currentSource : file,
		full, 0, false, record);
	Py_RETURN_NONE;
}

PyObject* PyBdWarn(PyObject*, PyObject* args)
{
	const char* message = nullptr;
	if (!PyArg_ParseTuple(args, "s:warn", &message)) return nullptr;
	if (!CheckEngineThread()) return nullptr;
	EmitScriptWarning(message);
	Py_RETURN_NONE;
}

//---------------------------------------------------------------------------
// bd.random()/randrange()/randint()/choice() - deterministic script RNG
// backed by s_pyRandom. These live in EngineMethods (not the gameplay API)
// so the stream stays usable before any level is loaded; the stream is
// reseeded per map in OnWorldLoaded and serialized with bd.state.
//---------------------------------------------------------------------------

PyObject* PyBdRandom(PyObject*, PyObject*)
{
	if (!CheckEngineThread()) return nullptr;
	return PyFloat_FromDouble(s_pyRandom.GenRand_Real2());
}

PyObject* PyBdRandRange(PyObject*, PyObject* args)
{
	const Py_ssize_t count = PyTuple_GET_SIZE(args);
	long long lo = 0;
	long long hi = 0;
	if (count == 1)
	{
		hi = PyLong_AsLongLong(PyTuple_GET_ITEM(args, 0));
	}
	else if (count == 2)
	{
		lo = PyLong_AsLongLong(PyTuple_GET_ITEM(args, 0));
		hi = PyLong_AsLongLong(PyTuple_GET_ITEM(args, 1));
	}
	else
	{
		PyErr_SetString(PyExc_TypeError, "randrange expects randrange(hi) or randrange(lo, hi)");
		return nullptr;
	}
	if (PyErr_Occurred()) return nullptr;
	if (!CheckEngineThread()) return nullptr;
	if (hi <= lo)
	{
		PyErr_SetString(PyExc_ValueError, "randrange requires hi > lo");
		return nullptr;
	}
	const uint64_t range = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo);
	const uint64_t value = range <= 0x7fffffffu
		? static_cast<uint32_t>(s_pyRandom(static_cast<int>(range)))
		: s_pyRandom.GenRand64() % range;
	return PyLong_FromLongLong(static_cast<long long>(static_cast<uint64_t>(lo) + value));
}

PyObject* PyBdRandInt(PyObject*, PyObject* args, PyObject* kwargs)
{
	long long lo, hi;
	static const char* keywords[] = { "lo", "hi", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "LL:randint",
		const_cast<char**>(keywords), &lo, &hi)) return nullptr;
	if (!CheckEngineThread()) return nullptr;
	if (lo > hi)
	{
		PyErr_SetString(PyExc_ValueError, "lo must not exceed hi");
		return nullptr;
	}
	const uint64_t range = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo) + 1u;
	uint64_t value;
	if (range == 0) value = s_pyRandom.GenRand64(); // range spans the full int64 domain
	else if (range <= 0x7fffffffu) value = static_cast<uint32_t>(s_pyRandom(static_cast<int>(range)));
	else value = s_pyRandom.GenRand64() % range;
	return PyLong_FromLongLong(static_cast<long long>(static_cast<uint64_t>(lo) + value));
}

PyObject* PyBdChoice(PyObject*, PyObject* args)
{
	PyObject* sequenceObject = nullptr;
	if (!PyArg_ParseTuple(args, "O:choice", &sequenceObject)) return nullptr;
	if (!CheckEngineThread()) return nullptr;
	PyObject* sequence = PySequence_Fast(sequenceObject, "choice requires a non-empty sequence");
	if (sequence == nullptr) return nullptr;
	const Py_ssize_t size = PySequence_Fast_GET_SIZE(sequence);
	if (size == 0)
	{
		Py_DECREF(sequence);
		PyErr_SetString(PyExc_ValueError, "cannot choose from an empty sequence");
		return nullptr;
	}
	PyObject* item = PySequence_Fast_GET_ITEM(sequence, s_pyRandom(static_cast<int>(size)));
	Py_INCREF(item);
	Py_DECREF(sequence);
	return item;
}

//---------------------------------------------------------------------------
// bd.custom_action_down(n) / bd.custom_action_mask() / bd.set_custom_action(n,
// down) / bd.input_binding(command) - the 32 generic custom action buttons
// (Button_PyAction1..32, console names +pyaction1..+pyaction32). These are
// local-only input state: they are never added to usercmd, so they work
// headless, are not recorded in demos, and are not transmitted in
// multiplayer. Python mods get conflict-free, user-bindable inputs that show
// up in Options -> Customize Controls -> Custom Actions.
//---------------------------------------------------------------------------

PyObject* PyBdCustomActionDown(PyObject*, PyObject* args)
{
	int n = 0;
	if (!PyArg_ParseTuple(args, "i:custom_action_down", &n)) return nullptr;
	if (!CheckEngineThread()) return nullptr;
	if (n < 1 || n > PyActionCount)
	{
		PyErr_Format(PyExc_ValueError, "custom action index must be 1..%d", PyActionCount);
		return nullptr;
	}
	return PyBool_FromLong(buttonMap.ButtonDown(Button_PyAction1 + n - 1) ? 1 : 0);
}

PyObject* PyBdCustomActionMask(PyObject*, PyObject*)
{
	if (!CheckEngineThread()) return nullptr;
	uint32_t mask = 0;
	for (int i = 0; i < PyActionCount; ++i)
	{
		if (buttonMap.ButtonDown(Button_PyAction1 + i)) mask |= 1u << i;
	}
	return PyLong_FromUnsignedLong(mask);
}

PyObject* PyBdSetCustomAction(PyObject*, PyObject* args)
{
	int n = 0;
	int down = 0;
	if (!PyArg_ParseTuple(args, "ip:set_custom_action", &n, &down)) return nullptr;
	if (!CheckEngineThread()) return nullptr;
	if (n < 1 || n > PyActionCount)
	{
		PyErr_Format(PyExc_ValueError, "custom action index must be 1..%d", PyActionCount);
		return nullptr;
	}
	FButtonStatus* button = buttonMap.GetButton(Button_PyAction1 + n - 1);
	// Idempotent: a request matching the current state is a no-op, so a mod
	// may "ensure held" every tick without re-firing events. PressKey/
	// ReleaseKey with keynum 0 are the same code path as typing +pyactionN /
	// -pyactionN at the console, so synthetic and bound input share one
	// button state and the per-tic scan cannot miss or double-fire the edge.
	if (down)
	{
		if (!button->bDown) button->PressKey(0);
	}
	else if (button->bDown)
	{
		button->ReleaseKey(0);
	}
	Py_RETURN_NONE;
}

PyObject* PyBdInputBinding(PyObject*, PyObject* args)
{
	const char* command = nullptr;
	if (!PyArg_ParseTuple(args, "s:input_binding", &command)) return nullptr;
	if (!CheckEngineThread()) return nullptr;
	for (int i = 0; i < NUM_KEYS; ++i)
	{
		const char* bind = Bindings.GetBind(i);
		if (bind == nullptr || stricmp(bind, command) != 0) continue;
		const char* name = KeyNames[i];
		return PyUnicode_FromString(name != nullptr ? name : "Unknown");
	}
	Py_RETURN_NONE;
}

PyMethodDef EngineMethods[] = {
	{ "log", BD_PY_KEYWORD_FUNCTION(PyBdLog), METH_VARARGS | METH_KEYWORDS, "log(message, level='info') -> None" },
	{ "_write_output", PyBdWriteOutput, METH_VARARGS, nullptr },
	{ "_register_callback", BD_PY_KEYWORD_FUNCTION(PyBdRegisterCallback), METH_VARARGS | METH_KEYWORDS, nullptr },
	{ "current_map", PyBdCurrentMap, METH_NOARGS, "Return the current map lump name or None." },
	{ "level_time", PyBdLevelTime, METH_NOARGS, "Return elapsed level time in 35 Hz tics." },
	{ "headless", PyBdHeadless, METH_NOARGS, "Return True when the engine runs on the null video driver (nothing renders; imgui_frame never fires)." },
	{ "players", PyBdPlayers, METH_NOARGS, "Return snapshots of all active players." },
	{ "actors", BD_PY_KEYWORD_FUNCTION(PyBdActors), METH_VARARGS | METH_KEYWORDS, "Return actor snapshots, optionally filtered by class or TID." },
	{ "_actor_class_info", PyBdActorClassInfo, METH_NOARGS, nullptr },
	{ "actor", PyBdActor, METH_VARARGS, "Return the first actor snapshot for a TID, or None." },
	{ "spawn_actor", BD_PY_KEYWORD_FUNCTION(PyBdSpawnActor), METH_VARARGS | METH_KEYWORDS, "Spawn an actor and return its snapshot." },
	{ "damage_actor", BD_PY_KEYWORD_FUNCTION(PyBdDamageActor), METH_VARARGS | METH_KEYWORDS, "Damage the first actor with a TID." },
	{ "set_actor_velocity", PyBdSetActorVelocity, METH_VARARGS, "Set actor velocity by TID." },
	{ "destroy_actor", PyBdDestroyActor, METH_VARARGS, "Destroy the first actor with a TID." },
	{ "get_cvar", PyBdGetCVar, METH_VARARGS, "Read a console variable using its native Python type." },
	{ "set_cvar", PyBdSetCVar, METH_VARARGS, "Set a console variable and return the applied value." },
	{ "execute", PyBdExecute, METH_VARARGS, "Queue an engine console command." },
	{ "execute_acs", BD_PY_KEYWORD_FUNCTION(PyBdExecuteACS), METH_VARARGS | METH_KEYWORDS, "Start a numeric or named ACS script." },
	{ "read_text", PyBdReadText, METH_VARARGS, "Read a UTF-8 resource from the current mod." },
	{ "read_bytes", PyBdReadBytes, METH_VARARGS, "Read a binary resource from the current mod, capped at 32 MiB." },
	{ "import_script", BD_PY_KEYWORD_FUNCTION(PyBdImportScript), METH_VARARGS | METH_KEYWORDS, "Execute and return another Python module from the current mod." },
	{ "profile", PyBdProfile, METH_NOARGS, "Return per-callback timing and budget statistics." },
	{ "reset_profile", PyBdResetProfile, METH_NOARGS, "Reset callback timing and budget statistics." },
	{ "schedule", BD_PY_KEYWORD_FUNCTION(PyBdSchedule), METH_VARARGS | METH_KEYWORDS, "Schedule a one-shot or repeating callable in engine tics." },
	{ "cancel_task", PyBdCancelTask, METH_VARARGS, "Cancel a scheduled task by ID." },
	{ "task_count", PyBdTaskCount, METH_NOARGS, "Return the number of active scheduled tasks." },
	{ "assert_true", BD_PY_KEYWORD_FUNCTION(PyBdAssertTrue), METH_VARARGS | METH_KEYWORDS, "Fail the script test with a message when the condition is false." },
	{ "warn", PyBdWarn, METH_VARARGS, "Print a rate-limited script warning without failing the script test." },
	{ "random", PyBdRandom, METH_NOARGS, "Return the next deterministic float in [0, 1)." },
	{ "randrange", PyBdRandRange, METH_VARARGS, "Return the next deterministic integer in [lo, hi); randrange(hi) uses [0, hi)." },
	{ "randint", BD_PY_KEYWORD_FUNCTION(PyBdRandInt), METH_VARARGS | METH_KEYWORDS, "Return the next deterministic integer in [lo, hi] inclusive." },
	{ "choice", PyBdChoice, METH_VARARGS, "Return a deterministic item from a non-empty sequence." },
	{ "custom_action_down", PyBdCustomActionDown, METH_VARARGS, "custom_action_down(n) -> bool; True while custom action n (1..32) is held. Local-only input state; not recorded in demos." },
	{ "custom_action_mask", PyBdCustomActionMask, METH_NOARGS, "custom_action_mask() -> int; bitmask of held custom actions; bit n-1 set while action n is held." },
	{ "set_custom_action", PyBdSetCustomAction, METH_VARARGS, "set_custom_action(n, down) -> None; synthetic press/release of custom action n, surfaced as a custom_action event on the next tick." },
	{ "input_binding", PyBdInputBinding, METH_VARARGS, "input_binding(command) -> str | None; display name (engine-canonical, e.g. 'Q', 'Mouse1') of the key bound to a command like '+pyaction1', or None when unbound." },
	{ nullptr, nullptr, 0, nullptr },
};

PyModuleDef EngineModuleDefinition = {
	PyModuleDef_HEAD_INIT,
	"biaseddoom",
	"Trusted embedded scripting API for BiasedDoom mods.",
	-1,
	EngineMethods,
};

PyMODINIT_FUNC PyInit_biaseddoom()
{
	PyObject* module = PyModule_Create(&EngineModuleDefinition);
	if (module == nullptr) return nullptr;
	if (!GameApi::Initialize(module))
	{
		Py_DECREF(module);
		return nullptr;
	}
	PyModule_AddIntConstant(module, "API_VERSION", PythonApiVersion);
	PyModule_AddIntConstant(module, "TICRATE", TICRATE);
	PyModule_AddIntConstant(module, "PYACTION_COUNT", PyActionCount);
	PyModule_AddStringConstant(module, "RUNTIME", "CPython");
	return module;
}

const char* BootstrapSource = R"PY(
import sys as _sys

def on(event_name, *, every=1, priority=0, class_name=None, tid=0, player=-1):
    """Decorator registering a callback for a BiasedDoom lifecycle event."""
    def decorate(callback):
        _register_callback(
            event_name,
            callback,
            every=every,
            priority=priority,
            class_name=class_name,
            tid=tid,
            player=player,
        )
        return callback
    return decorate

class _EngineWriter:
    def __init__(self, stream):
        self.stream = stream

    def write(self, text):
        _write_output(str(text), self.stream, 0)
        return len(text)

    def flush(self):
        _write_output("", self.stream, 1)

    def isatty(self):
        return False

_sys.stdout = _EngineWriter(0)
_sys.stderr = _EngineWriter(1)

def _actor_const_name(class_name):
    # "DoomImp" -> "DOOM_IMP", "MBFHelperDog" -> "MBF_HELPER_DOG"
    out = []
    for i, ch in enumerate(class_name):
        if not ch.isalnum():
            out.append('_')
            continue
        if ch.isupper() and i > 0 and (class_name[i - 1].islower() or class_name[i - 1].isdigit()
                                       or (i + 1 < len(class_name) and class_name[i + 1].islower())):
            out.append('_')
        out.append(ch.upper())
    name = ''.join(out)
    if name and name[0].isdigit():
        name = '_' + name
    return name

class _ActorsRegistry:
    """Actor class registry: named constants, discovery, random spawns.

    Attribute access maps UPPER_SNAKE constants to engine class names:
        actors.DOOM_IMP -> "DoomImp"
    The registry stays callable, so actors(...) still queries live actors.
    """

    def __init__(self, query):
        self._query = query
        self._loaded = False
        self._info = {}      # class name -> (parent name, kind mask)
        self._by_const = {}  # CONST name -> class name
        self._by_class = {}  # class name -> CONST name

    def __call__(self, *args, **kwargs):
        return self._query(*args, **kwargs)

    def _load(self):
        if self._loaded:
            return
        self._loaded = True
        for class_name, parent, kind in _actor_class_info():
            self._info[class_name] = (parent, kind)
            const = _actor_const_name(class_name)
            if const not in self._by_const:
                self._by_const[const] = class_name
                self._by_class[class_name] = const

    def __getattr__(self, name):
        if name.startswith('_'):
            raise AttributeError(name)
        self._load()
        try:
            return self._by_const[name]
        except KeyError:
            raise AttributeError(
                f"biaseddoom.actors has no constant {name!r}; "
                "use actors.names() or dir(actors) to list available actor classes") from None

    def __dir__(self):
        self._load()
        return sorted(self._by_const)

    def names(self):
        """All registered actor class names, sorted (e.g. \"DoomImp\")."""
        self._load()
        return sorted(self._info)

    def constants(self):
        """All constant names, sorted (e.g. \"DOOM_IMP\")."""
        self._load()
        return sorted(self._by_const)

    def resolve(self, name):
        """Accept a CONST name or an engine class name; return the class name or None."""
        self._load()
        if name in self._info:
            return name
        return self._by_const.get(name)

    def children_of(self, parent):
        """Sorted class names descending from parent (inclusive);
        parent may be a CONST name or an engine class name."""
        self._load()
        root = self.resolve(parent)
        if root is None:
            raise ValueError(f"unknown actor class {parent!r}")
        result = []
        for class_name in self._info:
            node = class_name
            while node:
                if node == root:
                    result.append(class_name)
                    break
                node = self._info.get(node, (None, 0))[0]
        return sorted(result)

    def _kind_names(self, mask):
        self._load()
        return sorted(n for n, (_, kind) in self._info.items() if kind & mask)

    def monsters(self):
        """Class names of shootable, kill-counted actors."""
        return self._kind_names(1)

    def projectiles(self):
        """Class names of missile actors."""
        return self._kind_names(2)

    def weapons(self):
        """Class names descending from Weapon."""
        return self._kind_names(4)

    def items(self):
        """Class names descending from Inventory."""
        return self._kind_names(8)

    def players(self):
        """Class names descending from PlayerPawn."""
        return self._kind_names(16)

    def random(self, kind=None):
        """Return a random actor class name. kind may be None (any actor),
        a category (\"monsters\", \"projectiles\", \"weapons\", \"items\",
        \"players\"), or a class/CONST name to pick among its descendants."""
        if kind is None:
            pool = self.names()
        else:
            mask = _ACTOR_KINDS.get(str(kind).lower())
            pool = self._kind_names(mask) if mask is not None else self.children_of(kind)
        if not pool:
            raise ValueError(f"no actor classes match {kind!r}")
        return choice(pool)

    def spawn_random(self, x, y, z, kind="monsters", **kwargs):
        """Spawn a random actor of the given category at (x, y, z)."""
        return spawn_actor(self.random(kind), x, y, z, **kwargs)

_ACTOR_KINDS = {
    "monster": 1, "monsters": 1,
    "projectile": 2, "projectiles": 2,
    "weapon": 4, "weapons": 4,
    "item": 8, "items": 8, "inventory": 8,
    "player": 16, "players": 16,
}

_actors_query = actors
actors = _ActorsRegistry(_actors_query)

_STUB_BEGIN = "    # @@GENERATED ACTOR CONSTANTS BEGIN@@"
_STUB_END = "    # @@GENERATED ACTOR CONSTANTS END@@"

def _stub_constants_block():
    lines = [_STUB_BEGIN]
    for const in actors.constants():
        lines.append(f"    {const}: str  # {actors.resolve(const)}")
    lines.append(_STUB_END)
    return "\n".join(lines)

def _public_api_names(mod):
    names = []
    for name in sorted(dir(mod)):
        if name.startswith('_'):
            continue
        value = getattr(mod, name)
        if isinstance(value, type(_sys)):
            continue
        names.append(name)
    return names

def _stub_skeleton():
    mod = _sys.modules["biaseddoom"]
    out = ['"""Type stubs for the embedded biaseddoom module (generated by dumppystub)."""',
           "from typing import Any, Callable, Optional, Union", ""]
    for name in _public_api_names(mod):
        if name == "actors":
            continue
        value = getattr(mod, name)
        if isinstance(value, bool):
            continue
        if isinstance(value, int):
            out.append(f"{name}: int")
        elif isinstance(value, str):
            out.append(f"{name}: str")
        elif isinstance(value, dict):
            out.append(f"{name}: dict")
    out.append("")
    for tname in ("Actor", "Line", "Sector", "Player"):
        t = getattr(mod, tname, None)
        if not isinstance(t, type):
            continue
        out.append(f"class {tname}:")
        for mname in sorted(dir(t)):
            if mname.startswith("__"):
                continue
            member = getattr(t, mname, None)
            doc = (getattr(member, "__doc__", None) or "").strip().splitlines()
            comment = f"  # {doc[0]}" if doc else ""
            if callable(member):
                out.append(f"    def {mname}(self, *args, **kwargs): ...{comment}")
            else:
                out.append(f"    {mname}: Any{comment}")
        out.append("")
    for name in _public_api_names(mod):
        value = getattr(mod, name)
        if not callable(value) or isinstance(value, type) or name == "actors":
            continue
        doc = (getattr(value, "__doc__", None) or "").strip().splitlines()
        comment = f"  # {doc[0]}" if doc else ""
        out.append(f"def {name}(*args, **kwargs): ...{comment}")
    out.append("")
    out.append("class _ActorsRegistry:")
    out.append("    def __call__(self, *args, **kwargs): ...")
    for mname in ("names", "constants", "resolve", "children_of", "monsters",
                  "projectiles", "weapons", "items", "players", "random", "spawn_random"):
        out.append(f"    def {mname}(self, *args, **kwargs): ...")
    out.append("    def __getattr__(self, name: str) -> str: ...")
    out.append(_stub_constants_block())
    out.append("")
    out.append("actors: _ActorsRegistry")
    out.append("")
    return "\n".join(out)

def _dump_stub(path):
    """Regenerate the .pyi stub's actor constants block (or a full skeleton)."""
    if not path:
        path = "biaseddoom.pyi"
    block = _stub_constants_block()
    try:
        with open(path, "r", encoding="utf-8") as handle:
            text = handle.read()
    except OSError:
        text = None
    if text is None:
        text = _stub_skeleton()
        action = "created"
    elif _STUB_BEGIN in text and _STUB_END in text:
        start = text.index(_STUB_BEGIN)
        end = text.index(_STUB_END) + len(_STUB_END)
        text = text[:start] + block + text[end:]
        action = "updated"
    else:
        raise RuntimeError(f"{path}: no generated-constants markers found; refusing to overwrite")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    missing = []
    mod = _sys.modules["biaseddoom"]
    for name in _public_api_names(mod):
        if name == "actors":
            continue
        if f"def {name}(" in text or f"{name}:" in text or f"class {name}" in text:
            continue
        missing.append(name)
    message = f"stub {action}: {path} ({len(actors.constants())} actor constants)"
    if missing:
        message += "\nWARNING: public API missing from stub: " + ", ".join(missing)
    return message
)PY";

const char* UiToolkitSource = R"PY(
# ---------------------------------------------------------------------------
# bd.ui: embedded pure-Python UI toolkit, built on the bd.draw_* display list.
# Runs inside the biaseddoom module namespace; no imports beyond builtins.
# ---------------------------------------------------------------------------

# All toolkit text uses draw_text's height= (a normalized screen-height
# fraction) instead of scale= (raw pixels), so panels stay the same size
# and text stays readable at any resolution.
_UI_ROW_H = 0.022        # normalized row height
_UI_PAD = 0.008          # normalized padding
_UI_TEXT_H = 0.015       # row text height (fraction of screen height)
_UI_TITLE_H = 0.019      # panel title text height
_UI_TOAST_H = 0.016      # toast text height
_UI_ANNOUNCE_H = 0.050   # announcement title height
_UI_SUBTITLE_H = 0.016   # announcement subtitle height
_UI_ID_BASE = 900000     # toolkit-owned display-list id range
_UI_ID_STRIDE = 100      # display-list ids reserved per panel (<= 22 rows)
_UI_TOAST_ID = 999000    # transient toast/announce ids live above the panels
_UI_ANNOUNCE_ID = 999100

_ui_panels = []


def _safe(fn, *args, **kwargs):
    """Call fn, swallowing RuntimeError (raised when no level/HUD is active).

    Failed draws are retried on the next row update and on the toolkit's
    map_load re-render hook, so nothing is lost across level transitions.
    """
    try:
        return fn(*args, **kwargs)
    except RuntimeError:
        return None


def _rgb(color):
    return (color[0], color[1], color[2])


def _alpha(color, fallback: float = 1.0) -> float:
    return color[3] / 255.0 if len(color) > 3 else fallback


def _shade(color, factor: float):
    return (int(color[0] * factor), int(color[1] * factor), int(color[2] * factor))


class _UiTheme:
    """Mutable color theme for bd.ui. Colors are (r, g, b) or (r, g, b, a)."""

    def __init__(self):
        self.bg = (10, 10, 26, 200)
        self.bg2 = (24, 14, 40, 200)
        self.border = (255, 140, 40)
        self.text = (235, 230, 220)
        self.dim = (150, 145, 135)
        self.accent = (255, 180, 60)
        self.good = (90, 220, 110)
        self.warn = (240, 210, 80)
        self.bad = (235, 70, 60)
        self.gold = (255, 200, 80)


_theme = _UiTheme()


def _bar_color(frac: float):
    if frac > 0.6:
        return _theme.good
    if frac > 0.3:
        return _theme.warn
    return _theme.bad


class _UiPanel:
    """Themed HUD panel: gradient backdrop, framed, auto-height from rows.

    Layers: backdrop 0, content/bars 1, frame 2. All text is smallfont
    sized with height= (normalized screen-height fraction, resolution-
    independent), outlined. Ids: panel base .. base+99 (slot 10 + index*4
    per row: +0 label, +1 value/bar bg, +2 flash/bar fill, +3 bar frame).
    """

    def __init__(self, x: float, y: float, w: float, title, anchor: str, base_id: int):
        if anchor not in ('tl', 'tr', 'bl', 'br'):
            raise ValueError("anchor must be 'tl', 'tr', 'bl' or 'br'")
        self.id = base_id
        self.x = float(x)
        self.y = float(y)
        self.w = float(w)
        self.title = title
        self.anchor = anchor
        self._rows = {}    # label -> {'kind': 'row'|'bar', ...}
        self._order = []   # labels in insertion order
        self._ids = set()  # display-list ids currently owned
        self._visible = True
        _ui_panels.append(self)
        self._render()

    def _draw(self, offset: int, fn, *args, **kwargs):
        self._ids.add(self.id + offset)
        return _safe(fn, *args, id=self.id + offset, **kwargs)

    def _geometry(self):
        title_h = _UI_ROW_H * 1.5 if self.title else 0.0
        h = 2 * _UI_PAD + title_h + _UI_ROW_H * len(self._order)
        left = self.x if self.anchor[1] == 'l' else self.x - self.w
        top = self.y if self.anchor[0] == 't' else self.y - h
        left = min(max(left, 0.0), max(0.0, 1.0 - self.w))  # never off-screen
        top = min(max(top, 0.0), max(0.0, 1.0 - h))
        return left, top, h, title_h

    def _render(self, alpha: float = 1.0) -> None:
        left, top, h, title_h = self._geometry()
        w = self.w
        self._draw(0, draw_rect, x=left, y=top, w=w, h=h,
                   color=_rgb(_theme.bg), color2=_rgb(_theme.bg2),
                   alpha=_alpha(_theme.bg, 0.8) * alpha, layer=0)
        self._draw(1, draw_frame, x=left, y=top, w=w, h=h,
                   color=_rgb(_theme.border), thickness=2, alpha=alpha, layer=2)
        cursor = top + _UI_PAD
        if self.title:
            self._draw(2, draw_text, str(self.title), x=left + w * 0.5, y=cursor,
                       color=_rgb(_theme.gold), height=_UI_TITLE_H, outline=True,
                       align='center', alpha=alpha, layer=1)
            sep_y = cursor + title_h - 0.004
            self._draw(3, draw_line, x1=left + _UI_PAD, y1=sep_y,
                       x2=left + w - _UI_PAD, y2=sep_y,
                       color=_rgb(_theme.gold), alpha=0.8 * alpha, layer=1)
            cursor += title_h
        for index, label in enumerate(self._order):
            self._render_row(index, label, left, cursor, w, alpha)
            cursor += _UI_ROW_H

    def _render_row(self, index: int, label: str, left: float, y: float,
                    w: float, alpha: float) -> None:
        row = self._rows[label]
        slot = 10 + index * 4
        text_y = y + (_UI_ROW_H - _UI_TEXT_H) * 0.5 - 0.001
        right = left + w - _UI_PAD
        self._draw(slot, draw_text, str(label), x=left + _UI_PAD, y=text_y,
                   color=_rgb(_theme.text), height=_UI_TEXT_H, outline=True,
                   alpha=alpha, layer=1)
        if row['kind'] == 'bar':
            frac = min(max(float(row['frac']), 0.0), 1.0)
            fg = row['fg'] if row['fg'] is not None else _bar_color(frac)
            bar_x = left + w * 0.45
            bar_w = right - bar_x
            bar_y = y + _UI_ROW_H * 0.22
            bar_h = _UI_ROW_H * 0.56
            self._draw(slot + 1, draw_rect, x=bar_x, y=bar_y, w=bar_w, h=bar_h,
                       color=(5, 5, 10), alpha=0.7 * alpha, layer=1)
            if frac > 0.0:
                self._draw(slot + 2, draw_rect, x=bar_x, y=bar_y,
                           w=bar_w * frac, h=bar_h, color=_rgb(fg),
                           color2=_shade(_rgb(fg), 0.55),
                           alpha=_alpha(fg) * alpha, layer=1)
            else:
                _safe(draw_clear, self.id + slot + 2)
            self._draw(slot + 3, draw_frame, x=bar_x, y=bar_y, w=bar_w, h=bar_h,
                       color=_rgb(_theme.dim), thickness=1, alpha=alpha, layer=1)
        else:
            color = row['value_color']
            color = _theme.accent if color is None else color
            self._draw(slot + 1, draw_text, str(row['value']), x=right, y=text_y,
                       color=_rgb(color), height=_UI_TEXT_H, outline=True,
                       align='right', alpha=alpha, layer=1)
            if row['flash']:
                # Brief gold highlight over the value; auto-expires.
                self._draw(slot + 2, draw_text, str(row['value']), x=right, y=text_y,
                           color=_rgb(_theme.gold), height=_UI_TEXT_H, outline=True,
                           align='right', layer=2, duration=0.4)
                row['flash'] = False
            else:
                _safe(draw_clear, self.id + slot + 2)

    def row(self, label: str, value: str = '', *, value_color=None,
            flash: bool = False) -> '_UiPanel':
        """Add a label/value row, or update it in place if label exists."""
        if label not in self._rows:
            self._order.append(label)
        self._rows[label] = {'kind': 'row', 'value': value,
                             'value_color': value_color, 'flash': flash}
        self._render(1.0 if self._visible else 0.0)
        return self

    def bar(self, label: str, frac: float, *, fg=None) -> '_UiPanel':
        """Add/update a bordered gradient bar row; fg overrides the
        good->warn->bad color picked from frac."""
        if label not in self._rows:
            self._order.append(label)
        self._rows[label] = {'kind': 'bar', 'frac': frac, 'fg': fg}
        self._render(1.0 if self._visible else 0.0)
        return self

    def hide(self) -> None:
        """Keep the panel registered but re-register every item at alpha 0."""
        self._visible = False
        self._render(0.0)

    def show(self) -> None:
        """Restore a hidden panel at full opacity."""
        self._visible = True
        self._render(1.0)

    def close(self) -> None:
        """Remove every display-list item owned by this panel."""
        for item_id in self._ids:
            _safe(draw_clear, item_id)
        self._ids.clear()
        if self in _ui_panels:
            _ui_panels.remove(self)


class _UiNamespace:
    """bd.ui - embedded UI toolkit: themed panels, toasts and announcements."""

    def __init__(self):
        self.theme = _theme
        self._next_id = _UI_ID_BASE

    def panel(self, *, x: float, y: float, w: float, title=None,
              anchor: str = 'tl', id=None) -> _UiPanel:
        """Create a panel anchored to a screen corner ('tl', 'tr', 'bl', 'br').
        x, y refer to that corner; w is the normalized width; height grows
        with the rows. id optionally overrides the allocated id base."""
        if id is None:
            base = self._next_id
            self._next_id += _UI_ID_STRIDE
        else:
            base = int(id)
        _ui_ensure_hook()
        return _UiPanel(x, y, w, title, anchor, base)

    def toast(self, text: str, *, color=None, duration: float = 1.5,
              y: float = 0.72) -> None:
        """Small centered outlined toast; auto-expires after duration seconds."""
        color = _theme.text if color is None else color
        _ui_ensure_hook()
        _safe(draw_text, str(text), id=_UI_TOAST_ID, x=0.5, y=y,
              font='smallfont', color=_rgb(color), height=_UI_TOAST_H,
              outline=True, align='center', layer=3, duration=duration)

    def announce(self, title: str, *, subtitle=None, color=None,
                 duration: float = 2.5) -> None:
        """Big centered announcement: outlined bigfont title, smallfont
        subtitle, a screen-fade accent and a UI sound. Text is sized with
        height= (normalized screen height), so it reads the same at any
        resolution."""
        color = _theme.gold if color is None else color
        _ui_ensure_hook()
        _safe(draw_text, str(title), id=_UI_ANNOUNCE_ID, x=0.5, y=0.38,
              font='bigfont', color=_rgb(color), height=_UI_ANNOUNCE_H,
              outline=True, align='center', layer=3, duration=duration)
        if subtitle is not None:
            _safe(draw_text, str(subtitle), id=_UI_ANNOUNCE_ID + 1, x=0.5, y=0.48,
                  font='smallfont', color=_rgb(_theme.text),
                  height=_UI_SUBTITLE_H, outline=True, align='center',
                  layer=3, duration=duration)
        accent = _rgb(color)
        _safe(screen_fade, accent[0], accent[1], accent[2], 0.15, 0.4)
        _safe(play_ui_sound, 'switches/normbutn')


ui = _UiNamespace()
try:
    _sys.modules['biaseddoom.ui'] = ui
except Exception:
    pass


def _ui_map_load(event):
    # Re-render all live panels after a level transition (also retries any
    # draw that failed while no level/HUD was active).
    for panel in list(_ui_panels):
        panel._render(1.0 if panel._visible else 0.0)


_ui_hook_registered = [False]


def _ui_ensure_hook():
    # Registering a callback is not allowed while the interpreter starts, so
    # the map_load hook is deferred to the first toolkit use (which always
    # happens inside a user callback, where registration is legal).
    if _ui_hook_registered[0]:
        return
    try:
        on('map_load')(_ui_map_load)
        _ui_hook_registered[0] = True
    except RuntimeError:
        pass
)PY";

void RegisterNamedCallbacks(PyObject* module, int container, const std::string& source)
{
	static const std::pair<const char*, const char*> callbackNames[] = {
		{ "on_engine_start", "engine_start" },
		{ "on_map_load", "map_load" },
		{ "on_map_unload", "map_unload" },
		{ "on_pre_tick", "pre_tick" },
		{ "on_tick", "tick" },
		{ "on_post_tick", "post_tick" },
		{ "on_actor_spawned", "actor_spawned" },
		{ "on_actor_died", "actor_died" },
		{ "on_actor_damaged", "actor_damaged" },
		{ "on_actor_destroyed", "actor_destroyed" },
		{ "on_actor_revived", "actor_revived" },
		{ "on_line_activated", "line_activated" },
		{ "on_line_activation_failed", "line_activation_failed" },
		{ "on_player_entered", "player_entered" },
		{ "on_player_spawned", "player_spawned" },
		{ "on_player_respawned", "player_respawned" },
		{ "on_player_died", "player_died" },
		{ "on_player_disconnected", "player_disconnected" },
		{ "on_save", "save" },
		{ "on_load", "load" },
		{ "on_engine_shutdown", "engine_shutdown" },
		{ "on_item_picked", "item_picked" },
		{ "on_secret_found", "secret_found" },
		{ "on_item_dropped", "item_dropped" },
		{ "on_weapon_changed", "weapon_changed" },
		{ "on_sector_entered", "sector_entered" },
		{ "on_sector_exited", "sector_exited" },
		{ "on_imgui_frame", "imgui_frame" },
		{ "on_conversation_started", "conversation_started" },
		{ "on_conversation_reply", "conversation_reply" },
		{ "on_ui_command", "ui_command" },
		{ "on_actor_before_damage", "actor_before_damage" },
		{ "on_custom_action", "custom_action" },
	};

	for (const auto& names : callbackNames)
	{
		PyObject* callable = PyObject_GetAttrString(module, names.first);
		if (callable == nullptr)
		{
			PyErr_Clear();
			continue;
		}
		if (PyCallable_Check(callable)) RegisterCallback(names.second, callable, container, source);
		else Printf(TEXTCOLOR_YELLOW "Python value %s in %s is not callable and was ignored.\n", names.first, source.c_str());
		Py_DECREF(callable);
	}
}

PyObject* ExecuteResourceModule(int container, const std::string& path, const std::string& moduleName, bool registerNamed)
{
	std::string source;
	if (!ReadResourceText(container, path, source, true))
	{
		PyErr_Format(PyExc_FileNotFoundError, "resource '%s' was not found in the current mod", path.c_str());
		return nullptr;
	}
	if (source.find('\0') != std::string::npos)
	{
		PyErr_Format(PyExc_SyntaxError, "Python resource '%s' contains an embedded NUL byte", path.c_str());
		return nullptr;
	}

	PyObject* module = PyModule_New(moduleName.c_str());
	if (module == nullptr) return nullptr;
	PyObject* dictionary = PyModule_GetDict(module);
	PyObject* fileName = PyUnicode_FromFormat("vfs://%s", path.c_str());
	if (fileName != nullptr)
	{
		PyDict_SetItemString(dictionary, "__file__", fileName);
		Py_DECREF(fileName);
	}
	PyDict_SetItemString(dictionary, "__builtins__", PyEval_GetBuiltins());

	const int previousContainer = currentContainer;
	const std::string previousSource = currentSource;
	const size_t callbackStart = callbacks.size();
	currentContainer = container;
	currentSource = path;
	PyObject* code = Py_CompileString(source.c_str(), path.c_str(), Py_file_input);
	PyObject* result = code == nullptr ? nullptr : PyEval_EvalCode(code, dictionary, dictionary);
	Py_XDECREF(code);
	currentContainer = previousContainer;
	currentSource = previousSource;
	if (result == nullptr)
	{
		// Decorators execute immediately. A later top-level exception must not
		// leave callbacks from a module that the loader reports as skipped.
		for (size_t index = callbackStart; index < callbacks.size(); ++index)
		{
			Py_XDECREF(callbacks[index].Callable);
		}
		callbacks.resize(callbackStart);
		RebuildEventPresence();
		Py_DECREF(module);
		return nullptr;
	}
	Py_DECREF(result);
	// Register the module in sys.modules so sibling mod scripts can reach it
	// with a plain `import <module_name>` instead of threading the object
	// returned by import_script through every call site.
	PyObject* moduleDict = PyImport_GetModuleDict();
	if (moduleDict != nullptr)
	{
		PyDict_SetItemString(moduleDict, moduleName.c_str(), module);
	}
	if (registerNamed) RegisterNamedCallbacks(module, container, path);
	return module;
}

bool InitializeInterpreter()
{
	if (!inittabRegistered)
	{
		if (PyImport_AppendInittab("biaseddoom", &PyInit_biaseddoom) == -1)
		{
			Printf(TEXTCOLOR_RED "Could not register the BiasedDoom Python module.\n");
			return false;
		}
		inittabRegistered = true;
	}

	PyConfig config;
	PyConfig_InitIsolatedConfig(&config);
	config.install_signal_handlers = 0;
	config.parse_argv = 0;
	config.site_import = 0;
	config.user_site_directory = 0;
	config.write_bytecode = 0;
	PyStatus status = PyConfig_SetBytesString(&config, &config.program_name, GAMENAMELOWERCASE);

	FString pythonHome = progdir;
	if (pythonHome.IsNotEmpty() && pythonHome.Back() != '/' && pythonHome.Back() != '\\')
	{
		pythonHome << '/';
	}
	pythonHome << "python";
	FString encodings = pythonHome;
#ifdef _WIN32
	encodings << "/Lib/encodings/__init__.py";
#else
	encodings.AppendFormat("/lib/python%d.%d/encodings/__init__.py", PY_MAJOR_VERSION, PY_MINOR_VERSION);
#endif
	if (!PyStatus_Exception(status) && !FileExists(encodings))
	{
		Printf(TEXTCOLOR_RED "Could not initialize CPython: private standard library is missing (%s).\n",
			encodings.GetChars());
		PyConfig_Clear(&config);
		return false;
	}
	if (!PyStatus_Exception(status))
	{
		status = PyConfig_SetBytesString(&config, &config.home, pythonHome.GetChars());
	}

	if (!PyStatus_Exception(status)) status = Py_InitializeFromConfig(&config);
	if (PyStatus_Exception(status))
	{
		Printf(TEXTCOLOR_RED "Could not initialize CPython: %s\n",
			status.err_msg == nullptr ? "unknown initialization error" : status.err_msg);
		PyConfig_Clear(&config);
		return false;
	}
	PyConfig_Clear(&config);

	engineModule = PyImport_ImportModule("biaseddoom");
	if (engineModule == nullptr)
	{
		ReportPythonError("module initialization", "biaseddoom");
		Py_FinalizeEx();
		return false;
	}

	stateDictionary = PyDict_New();
	PyObject* moduleStateReference = stateDictionary == nullptr ? nullptr : Py_NewRef(stateDictionary);
	if (stateDictionary == nullptr || PyModule_AddObject(engineModule, "state", moduleStateReference) < 0)
	{
		Py_XDECREF(moduleStateReference);
		Py_XDECREF(stateDictionary);
		stateDictionary = nullptr;
		ReportPythonError("state initialization", "biaseddoom");
		Py_DECREF(engineModule);
		engineModule = nullptr;
		Py_FinalizeEx();
		return false;
	}
	PyObject* bootstrapResult = PyRun_String(BootstrapSource, Py_file_input,
		PyModule_GetDict(engineModule), PyModule_GetDict(engineModule));
	if (bootstrapResult == nullptr)
	{
		ReportPythonError("bootstrap", "biaseddoom");
		Py_DECREF(stateDictionary);
		stateDictionary = nullptr;
		Py_DECREF(engineModule);
		engineModule = nullptr;
		Py_FinalizeEx();
		return false;
	}
	Py_DECREF(bootstrapResult);
	// The embedded bd.ui toolkit execs into the same module dict, right after
	// the bootstrap, so it builds on the bootstrap's helpers (on, _sys, ...).
	PyObject* uiToolkitResult = PyRun_String(UiToolkitSource, Py_file_input,
		PyModule_GetDict(engineModule), PyModule_GetDict(engineModule));
	if (uiToolkitResult == nullptr)
	{
		ReportPythonError("ui toolkit", "biaseddoom");
		Py_DECREF(stateDictionary);
		stateDictionary = nullptr;
		Py_DECREF(engineModule);
		engineModule = nullptr;
		Py_FinalizeEx();
		return false;
	}
	Py_DECREF(uiToolkitResult);
	return true;
}

// Deterministic per-map seed for the script RNG: the run's global rngseed
// mixed with the level number; FRandom::Init additionally mixes the stream's
// name CRC, exactly like every other engine RNG seeded by StaticClearRandom.
void SeedPythonRandom()
{
	const uint32_t levelPart = primaryLevel != nullptr
		? static_cast<uint32_t>(primaryLevel->levelnum) : 0u;
	s_pyRandom.Init(rngseed ^ (levelPart * 0x9E3779B9u));
}

// JSON-ready snapshot of the script RNG stream (reserved bd.state key).
PyObject* CaptureRngState()
{
	std::vector<uint32_t> words(static_cast<size_t>(FRandom::StateWordCount));
	int index = 0;
	s_pyRandom.GetState(words.data(), index);
	PyObject* list = PyList_New(FRandom::StateWordCount);
	if (list == nullptr) return nullptr;
	for (int i = 0; i < FRandom::StateWordCount; ++i)
	{
		PyObject* word = PyLong_FromUnsignedLong(words[static_cast<size_t>(i)]);
		if (word == nullptr)
		{
			Py_DECREF(list);
			return nullptr;
		}
		PyList_SET_ITEM(list, i, word);
	}
	PyObject* dict = PyDict_New();
	if (dict == nullptr)
	{
		Py_DECREF(list);
		return nullptr;
	}
	PyObject* indexObject = PyLong_FromLong(index);
	int ok = indexObject != nullptr ? PyDict_SetItemString(dict, "index", indexObject) : -1;
	Py_XDECREF(indexObject);
	if (ok == 0) ok = PyDict_SetItemString(dict, "state", list);
	Py_DECREF(list);
	if (ok != 0)
	{
		Py_DECREF(dict);
		return nullptr;
	}
	return dict;
}

// Restore the script RNG stream from a previously captured snapshot. Any
// format problem (or a legacy save without the key, handled by the caller)
// falls back to the deterministic map seed; no SAVEVER bump.
void RestoreRngState(PyObject* stateObject)
{
	bool restored = false;
	if (stateObject != nullptr && PyDict_Check(stateObject))
	{
		PyObject* indexObject = PyDict_GetItemString(stateObject, "index");
		PyObject* wordsObject = PyDict_GetItemString(stateObject, "state");
		const long index = indexObject != nullptr ? PyLong_AsLong(indexObject) : -1;
		if (!PyErr_Occurred() && indexObject != nullptr && wordsObject != nullptr &&
			PyList_Check(wordsObject) &&
			PyList_GET_SIZE(wordsObject) == FRandom::StateWordCount &&
			index >= 0 && index <= FRandom::StateWordCount)
		{
			std::vector<uint32_t> words(static_cast<size_t>(FRandom::StateWordCount));
			bool valid = true;
			for (int i = 0; i < FRandom::StateWordCount; ++i)
			{
				PyObject* item = PyList_GET_ITEM(wordsObject, i);
				if (!PyLong_Check(item))
				{
					valid = false;
					break;
				}
				words[static_cast<size_t>(i)] =
					static_cast<uint32_t>(PyLong_AsUnsignedLongMask(item));
			}
			if (valid)
			{
				s_pyRandom.SetState(words.data(), static_cast<int>(index));
				restored = true;
			}
		}
	}
	PyErr_Clear();
	if (!restored)
	{
		Printf(TEXTCOLOR_YELLOW "Python: saved script RNG state is missing or corrupt; reseeding from the level seed.\n");
		SeedPythonRandom();
	}
}

std::string DumpStateJson()
{
	if (!active || stateDictionary == nullptr) return {};
	if (!ValidateStateDictionary())
	{
		ReportPythonError("state serialization", "biaseddoom.state");
		return {};
	}
	PyObject* json = PyImport_ImportModule("json");
	if (json == nullptr)
	{
		ReportPythonError("state serialization", "import json");
		return {};
	}

	// Merge the script RNG stream state under a reserved key so savegames
	// restore deterministic bd.random() sequences; popped back out right
	// after the dump so user scripts never observe it. A user script owning
	// either reserved key is a hard serialization error on write: never
	// overwrite it (the legacy key stays loadable only for old saves).
	for (const char* reservedKey : { RngStateKey, LegacyRngStateKey })
	{
		if (PyDict_GetItemString(stateDictionary, reservedKey) != nullptr)
		{
			PyErr_Format(PyExc_RuntimeError, "biaseddoom.state key '%s' is reserved for engine RNG state", reservedKey);
			Py_DECREF(json);
			ReportPythonError("state serialization", "biaseddoom.state");
			return {};
		}
	}
	bool rngMerged = false;
	PyObject* rngState = CaptureRngState();
	if (rngState != nullptr)
	{
		rngMerged = PyDict_SetItemString(stateDictionary, RngStateKey, rngState) == 0;
		Py_DECREF(rngState);
	}
	else
	{
		ReportPythonError("state serialization", "biaseddoom rng state");
	}

	// allow_nan stays true on purpose: bd.state holding NaN/Infinity must
	// round-trip (Python's json extension accepts these tokens on loads)
	// instead of failing the whole save over one non-finite float.
	PyObject* dumps = PyObject_GetAttrString(json, "dumps");
	PyObject* kwargs = Py_BuildValue("{s:O,s:O,s:O}",
		"sort_keys", Py_True, "ensure_ascii", Py_False, "allow_nan", Py_True);
	PyObject* args = PyTuple_Pack(1, stateDictionary);
	PyObject* encoded = dumps == nullptr ? nullptr : PyObject_Call(dumps, args, kwargs);
	std::string result;
	if (encoded != nullptr)
	{
		const char* text = PyUnicode_AsUTF8(encoded);
		if (text != nullptr) result = text;
	}
	else ReportPythonError("state serialization", "biaseddoom.state");
	if (rngMerged && PyDict_DelItemString(stateDictionary, RngStateKey) != 0)
	{
		PyErr_Clear();
	}
	Py_XDECREF(encoded);
	Py_DECREF(args);
	Py_DECREF(kwargs);
	Py_XDECREF(dumps);
	Py_DECREF(json);
	return result;
}

bool LoadStateJson(const std::string& encoded)
{
	if (!active || stateDictionary == nullptr || encoded.empty()) return false;
	if (!ValidateStateDictionary())
	{
		ReportPythonError("state restoration", "biaseddoom.state");
		return false;
	}
	PyObject* json = PyImport_ImportModule("json");
	PyObject* loads = json == nullptr ? nullptr : PyObject_GetAttrString(json, "loads");
	PyObject* text = PyUnicode_FromStringAndSize(encoded.data(), static_cast<Py_ssize_t>(encoded.size()));
	PyObject* decoded = loads == nullptr || text == nullptr ? nullptr : PyObject_CallFunctionObjArgs(loads, text, nullptr);
	bool success = false;
	if (decoded != nullptr && PyDict_Check(decoded))
	{
		PyDict_Clear(stateDictionary);
		success = PyDict_Update(stateDictionary, decoded) == 0;
		if (success)
		{
			// Consume the reserved RNG snapshot before the "load" event fires
			// and before user scripts can observe it. New saves carry
			// RngStateKey; pre-4.15.15 saves may still use the legacy
			// "__rng_state__" key, which is consumed (and deleted) the same way.
			PyObject* rngState = PyDict_GetItemString(stateDictionary, RngStateKey);
			const char* consumedKey = RngStateKey;
			if (rngState == nullptr)
			{
				rngState = PyDict_GetItemString(stateDictionary, LegacyRngStateKey);
				consumedKey = LegacyRngStateKey;
			}
			if (rngState != nullptr)
			{
				RestoreRngState(rngState);
				PyDict_DelItemString(stateDictionary, consumedKey);
			}
			else
			{
				// Saves written before the deterministic stream existed carry
				// no state; reseed from the level seed instead.
				SeedPythonRandom();
			}
			s_rngStateLoaded = true;
		}
	}
	else if (decoded != nullptr)
	{
		PyErr_SetString(PyExc_TypeError, "saved biaseddoom.state must decode to a dictionary");
	}
	if (!success) ReportPythonError("state restoration", "biaseddoom.state");
	Py_XDECREF(decoded);
	Py_XDECREF(text);
	Py_XDECREF(loads);
	Py_XDECREF(json);
	return success;
}

} // namespace

bool CheckApiThread()
{
	return CheckEngineThread();
}

bool CheckGameplayMutation()
{
	return CheckMutationAllowed();
}

unsigned int GetErrorCount()
{
	return s_pythonErrorCount;
}

void SetErrorLogPath(const char* path)
{
	s_pythonErrorLogPath = path != nullptr ? path : "";
}

void ReportScriptWarning(const char* fmt, ...)
{
	if (!active) return;
	va_list args;
	va_start(args, fmt);
	FString message;
	message.VFormat(fmt, args);
	va_end(args);
	EmitScriptWarning(message.GetChars());
}

void DumpStub(const char* path)
{
	if (!IsActive())
	{
		Printf("Python is not active (start the game with -python and a script).\n");
		return;
	}
	PyObject* func = PyObject_GetAttrString(engineModule, "_dump_stub");
	if (func == nullptr)
	{
		PyErr_Clear();
		Printf("stub generator unavailable\n");
		return;
	}
	PyObject* result = path != nullptr
		? PyObject_CallFunction(func, "s", path)
		: PyObject_CallFunction(func, "O", Py_None);
	Py_DECREF(func);
	if (result == nullptr)
	{
		ReportPythonError("dumppystub", "");
		return;
	}
	Printf("%s\n", PyString(result).c_str());
	Py_DECREF(result);
}

bool CheckSessionMutation()
{
	return CheckSessionMutationAllowed();
}

bool CheckLocalPresentation()
{
	return CheckLocalPresentationAllowed();
}

bool IsCompiled()
{
	return true;
}

bool IsActive()
{
	return active;
}

void* BeginIdleWait()
{
	// PyEval_SaveThread releases the GIL; the caller must not touch any engine
	// Python API until the returned state is passed to EndIdleWait.
	if (!active) return nullptr;
	return PyEval_SaveThread();
}

void EndIdleWait(void* state)
{
	if (state == nullptr) return;
	PyEval_RestoreThread(static_cast<PyThreadState*>(state));
}

bool Initialize()
{
	if (active) return true;
	engineThread = std::this_thread::get_id();
	DiscoverScripts();
	if (discoveredScripts.empty()) return false;
	if (!RuntimeRequested())
	{
		Printf(TEXTCOLOR_YELLOW "%zu Python script%s found but not executed. Python mods are trusted code; use -python or set py_enabled true to opt in.\n",
			discoveredScripts.size(), discoveredScripts.size() == 1 ? " was" : "s were");
		return false;
	}

	if (!InitializeInterpreter()) return false;
	active = true;
	unsigned loaded = 0;
	for (size_t index = 0; index < discoveredScripts.size(); ++index)
	{
		const ScriptEntry& entry = discoveredScripts[index];
		const std::string moduleName = "biaseddoom_mod_" + std::to_string(entry.Container) + "_" + std::to_string(index);
		PyObject* module = ExecuteResourceModule(entry.Container, entry.Path, moduleName, true);
		if (module == nullptr)
		{
			ReportPythonError("script load", entry.Resource + ":" + entry.Path);
			continue;
		}
		modules.push_back({ module, entry.Container, entry.Path });
		++loaded;
	}

	Printf("Python: CPython %s initialized; loaded %u/%zu script%s.\n",
		Py_GetVersion(), loaded, discoveredScripts.size(), discoveredScripts.size() == 1 ? "" : "s");
	InvokeEvent("engine_start", BuildEvent("engine_start"));
	return true;
}

void Shutdown()
{
	if (!active) return;
	InvokeEvent("engine_shutdown", BuildEvent("engine_shutdown"));
	EmitBufferedOutput(stdoutBuffer, nullptr, true, false);
	EmitBufferedOutput(stderrBuffer, nullptr, true, true);
	// Prevent atexit hooks and object finalizers from calling back into engine
	// state or appending new callbacks while interpreter-owned references are
	// being released. Detach the writers so CPython's final flush is inert.
	if (PySys_SetObject("stdout", Py_None) < 0) PyErr_Clear();
	if (PySys_SetObject("stderr", Py_None) < 0) PyErr_Clear();
	active = false;
	for (Callback& callback : callbacks) Py_XDECREF(callback.Callable);
	callbacks.clear();
	eventHasCallbacks.fill(false);
	for (ScheduledTask& task : scheduledTasks) Py_XDECREF(task.Callable);
	scheduledTasks.clear();
	callbacksNeedSort = false;
	callbackDispatchDepth = 0;
	tickBudgetMicroseconds = 0;
	tickBudgetOverruns = 0;
	tickBudgetSkips = 0;
	taskClock = 0;
	nextTaskId = 1;
	mapSerial = 0;
	taskDispatchDepth = 0;
	for (ScriptModule& module : modules) Py_XDECREF(module.Module);
	modules.clear();
	Py_CLEAR(stateDictionary);
	Py_CLEAR(engineModule);
	GameApi::Shutdown();
	loadCallbackPending = false;
	s_rngStateLoaded = false;
	s_lastPythonWarning = "";
	s_repeatPythonWarningCount = 0;
	currentContainer = -1;
	currentSource.clear();
	Py_FinalizeEx();
}

bool Reload()
{
	Printf(TEXTCOLOR_YELLOW "Python: reloading scripts. Scheduled tasks and callbacks do not survive a reload and will be cleared; re-register them from your engine_start/map_load handlers.\n");
	// Snapshot bd.state before tearing the interpreter down. When the state
	// dictionary holds user data it must serialize: bailing out here leaves
	// the running runtime (and its state) intact instead of losing it.
	std::string state;
	if (stateDictionary != nullptr && PyDict_Size(stateDictionary) > 0)
	{
		state = DumpStateJson();
		if (state.empty())
		{
			Printf(TEXTCOLOR_RED "Python reload ABORTED: biaseddoom.state could not be serialized; the current scripts and state were left running. Fix the serialization error and try again.\n");
			return false;
		}
	}
	const bool hadLevel = primaryLevel != nullptr && primaryLevel->MapName.IsNotEmpty();
	Shutdown();
	if (!Initialize()) return false;
	if (!state.empty()) LoadStateJson(state);
	if (hadLevel) OnWorldLoaded();
	return true;
}

void OnWorldLoaded()
{
	++mapSerial;
	// Savegame and hub-snapshot restores already restored (or reseeded) the
	// script RNG inside LoadStateJson; only fresh map entries seed here, so a
	// loaded stream position is never clobbered.
	if (s_rngStateLoaded) s_rngStateLoaded = false;
	else SeedPythonRandom();
	std::fill(std::begin(s_lastPlayerSector), std::end(s_lastPlayerSector), -1);
	if (!HasCallbacks("map_load")) return;
	PyObject* event = BuildEvent("map_load");
	DictSetBool(event, "from_savegame", savegamerestore);
	// Hub re-entry restores the world from a snapshot without counting as a
	// savegame restore; framework packs use from_hub to skip fresh-map setup.
	DictSetBool(event, "from_hub", primaryLevel != nullptr && primaryLevel->FromSnapshot && !savegamerestore);
	InvokeEvent("map_load", event);
}

void OnWorldUnloaded(const char* nextMap)
{
	std::fill(std::begin(s_lastPlayerSector), std::end(s_lastPlayerSector), -1);
	if (!HasCallbacks("map_unload"))
	{
		CancelMapLocalTasks();
		GameApi::InvalidateWorld();
		PythonDisplayList::PurgeWorldItems();
		return;
	}
	PyObject* event = BuildEvent("map_unload");
	if (nextMap == nullptr || *nextMap == 0) DictSet(event, "next_map", Py_NewRef(Py_None));
	else DictSetString(event, "next_map", nextMap);
	const bool wasBlocked = gameplayMutationBlocked;
	gameplayMutationBlocked = true;
	InvokeEvent("map_unload", event);
	gameplayMutationBlocked = wasBlocked;
	CancelMapLocalTasks();
	GameApi::InvalidateWorld();
	PythonDisplayList::PurgeWorldItems();
}

namespace
{
// Scans the 32 generic custom action buttons once per gametic and dispatches
// a custom_action event for every down/up transition. Runs at the top of
// OnWorldPreTick: bDown was latched by the input events of this tic (and by
// any bd.set_custom_action call since the previous scan), and dispatching
// before scheduled tasks and pre_tick means handlers observe a stable,
// already-reported input state for the rest of the tic. Ordering per
// gametic: input latch -> custom_action -> pre_tick -> P_PlayerThink ->
// tick -> post_tick. A transition caused by a handler (e.g. from pre_tick)
// therefore surfaces on the next gametic's scan.
// Transitions are derived from bDown alone, compared against the remembered
// mask: the bWentDown/bWentUp edge flags are NOT per-tic on SDL builds
// (only the Windows I_StartTic calls ResetButtonTriggers), so honoring them
// would re-fire stale edges every tic. bDown is the state every platform
// maintains correctly. When no script subscribed to custom_action the scan
// is skipped entirely except for resyncing the remembered mask, so late
// subscriptions never see a spurious edge for input that changed while
// nobody listened.
void ScanCustomActions()
{
	if (!HasCallbacks("custom_action"))
	{
		uint32_t sync = 0;
		for (int i = 0; i < PyActionCount; ++i)
		{
			if (buttonMap.ButtonDown(Button_PyAction1 + i)) sync |= 1u << i;
		}
		s_pyActionDownMask = sync;
		return;
	}
	for (int i = 0; i < PyActionCount; ++i)
	{
		const uint32_t bit = 1u << i;
		const bool down = buttonMap.ButtonDown(Button_PyAction1 + i);
		const bool remembered = (s_pyActionDownMask & bit) != 0;
		if (down == remembered) continue;
		if (down) s_pyActionDownMask |= bit;
		else s_pyActionDownMask &= ~bit;
		PyObject* event = BuildEvent("custom_action");
		DictSetInt(event, "action", i + 1);
		DictSetBool(event, "pressed", down);
		InvokeEvent("custom_action", event);
	}
}
} // namespace

void OnWorldPreTick()
{
	if (!active) return;
	// Slot invalidations queued by the engine GC markers may run Python
	// decrefs now that we are outside any marker context.
	GameApi::DrainDeferredInvalidations();
	// Reset the per-tic budget BEFORE the custom-action scan so input edges
	// are never dropped just because the previous tic saturated the budget.
	tickBudgetMicroseconds = 0;
	ScanCustomActions();
	++taskClock;
	ProcessScheduledTasks();
	if (!HasCallbacks("pre_tick")) return;
	PyObject* event = BuildEvent("pre_tick");
	DictSetBool(event, "paused", paused != 0);
	InvokeEvent("pre_tick", event);
}

void OnWorldTick()
{
	if (!HasCallbacks("tick")) return;
	PyObject* event = BuildEvent("tick");
	DictSetBool(event, "paused", paused != 0);
	InvokeEvent("tick", event);
}

namespace
{
// Fires one sector_entered/sector_exited event for a player. sectorIndex must
// be a valid sector index in the primary level.
void FireSectorEvent(const char* eventName, int playerIndex, int sectorIndex)
{
	if (!HasCallbacks(eventName)) return;
	PyObject* event = BuildEvent(eventName);
	DictSetInt(event, "sector", sectorIndex);
	// Same tag source as the Sector handle's "tags" getset in
	// python_game_api.cpp: the level tag manager.
	PyObject* tagList = PyList_New(0);
	if (tagList != nullptr)
	{
		if (primaryLevel != nullptr && sectorIndex >= 0 && sectorIndex < static_cast<int>(primaryLevel->sectors.Size()))
		{
			sector_t* sector = &primaryLevel->sectors[sectorIndex];
			const int count = primaryLevel->tagManager.CountSectorTags(sector);
			for (int index = 0; index < count; ++index)
			{
				PyObject* tag = PyLong_FromLong(primaryLevel->tagManager.GetSectorTag(sector, index));
				if (tag == nullptr || PyList_Append(tagList, tag) < 0)
				{
					Py_XDECREF(tag);
					break;
				}
				Py_DECREF(tag);
			}
		}
		DictSet(event, "tags", tagList);
	}
	DictSetInt(event, "player_index", playerIndex);
	AActor* actor = playerIndex >= 0 && playerIndex < static_cast<int>(MAXPLAYERS)
		? players[playerIndex].mo : nullptr;
	DictSet(event, "actor_ref", GameApi::MakeActorRef(actor));
	InvokeEvent(eventName, event, actor, playerIndex);
}

// Compares each in-game player's current sector against the last known one and
// fires sector_exited/sector_entered on changes. Runs from OnWorldPostTick;
// fully skipped unless a script subscribed to either event.
void UpdatePlayerSectorTracking()
{
	if (!active || primaryLevel == nullptr) return;
	if (!HasCallbacks("sector_entered") && !HasCallbacks("sector_exited")) return;
	for (int i = 0; i < static_cast<int>(MAXPLAYERS); ++i)
	{
		if (!playeringame[i])
		{
			s_lastPlayerSector[i] = -1;
			continue;
		}
		AActor* mo = players[i].mo;
		const int current = mo != nullptr && mo->Sector != nullptr ? mo->Sector->Index() : -1;
		const int previous = s_lastPlayerSector[i];
		if (current == previous) continue;
		if (previous != -1) FireSectorEvent("sector_exited", i, previous);
		if (current != -1) FireSectorEvent("sector_entered", i, current);
		s_lastPlayerSector[i] = current;
	}
}
} // namespace

void OnWorldPostTick()
{
	UpdatePlayerSectorTracking();
	if (!HasCallbacks("post_tick")) return;
	PyObject* event = BuildEvent("post_tick");
	DictSetBool(event, "paused", paused != 0);
	DictSetInt(event, "python_time_us", tickBudgetMicroseconds);
	InvokeEvent("post_tick", event);
}

void OnActorSpawned(AActor* actor)
{
	if (!HasCallbacks("actor_spawned")) return;
	PyObject* event = BuildEvent("actor_spawned");
	DictSet(event, "actor", ActorSnapshot(actor));
	DictSet(event, "actor_ref", GameApi::MakeActorRef(actor));
	InvokeEvent("actor_spawned", event, actor, ActorPlayerNumber(actor));
}

void OnActorDied(AActor* actor, AActor* inflictor, AActor* source)
{
	if (!HasCallbacks("actor_died")) return;
	PyObject* event = BuildEvent("actor_died");
	DictSet(event, "actor", ActorSnapshot(actor));
	DictSet(event, "inflictor", ActorSnapshot(inflictor));
	DictSet(event, "actor_ref", GameApi::MakeActorRef(actor));
	DictSet(event, "inflictor_ref", GameApi::MakeActorRef(inflictor));
	// Killer attribution: source is AActor::Die's first argument. For missile
	// kills it is the shooter (P_DamageMobj receives missile->target as the
	// source, see p_map.cpp; the missile itself stays in inflictor), for
	// hitscan and melee the attacker itself, for explosions the bomb owner.
	// It is null for environmental deaths (crushers, falling damage, damaging
	// terrain) and for bd.damage_actor / Actor.damage calls without a source.
	DictSet(event, "attacker_ref", GameApi::MakeActorRef(source));
	if (source != nullptr) DictSetString(event, "attacker_class", source->GetClass()->TypeName.GetChars());
	else DictSet(event, "attacker_class", Py_NewRef(Py_None));
	const int attackerPlayer = ActorPlayerNumber(source);
	if (attackerPlayer >= 0) DictSetInt(event, "attacker_player_index", attackerPlayer);
	else DictSet(event, "attacker_player_index", Py_NewRef(Py_None));
	InvokeEvent("actor_died", event, actor, ActorPlayerNumber(actor));
}

void OnActorDamaged(AActor* actor, AActor* inflictor, AActor* source,
	int damage, const char* damageType, int flags, double angle)
{
	if (!HasCallbacks("actor_damaged")) return;
	PyObject* event = BuildEvent("actor_damaged");
	DictSet(event, "actor_ref", GameApi::MakeActorRef(actor));
	DictSet(event, "inflictor_ref", GameApi::MakeActorRef(inflictor));
	DictSet(event, "source_ref", GameApi::MakeActorRef(source));
	DictSetInt(event, "damage", damage);
	DictSetString(event, "damage_type", damageType);
	DictSetInt(event, "flags", flags);
	DictSetFloat(event, "angle", angle);
	InvokeEvent("actor_damaged", event, actor, ActorPlayerNumber(actor));
}

bool OnBeforeDamage(AActor* target, AActor* inflictor, AActor* source,
	int& damage, FName& mod, int flags, double angle)
{
	if (!HasCallbacks("actor_before_damage")) return false;
	// Pre-damage filters mutate gameplay outcomes, so they are offline-only
	// like every other gameplay mutation: skip dispatch in multiplayer and
	// demo sessions to protect determinism. Same condition as
	// CheckSessionMutationAllowed, but silent: this runs per damage event on
	// the hottest gameplay path, not per explicit script call. (Note that the
	// read-only actor_damaged event, dispatched from EventManager::
	// WorldThingDamaged, is NOT gated this way; only the filter is.)
	if (netgame || multiplayer || demoplayback || demorecording) return false;
	PyObject* event = BuildEvent("actor_before_damage");
	DictSet(event, "actor_ref", GameApi::MakeActorRef(target));
	DictSet(event, "inflictor_ref", GameApi::MakeActorRef(inflictor));
	DictSet(event, "attacker_ref", GameApi::MakeActorRef(source));
	if (source != nullptr) DictSetString(event, "attacker_class", source->GetClass()->TypeName.GetChars());
	else DictSet(event, "attacker_class", Py_NewRef(Py_None));
	const int attackerPlayer = ActorPlayerNumber(source);
	if (attackerPlayer >= 0) DictSetInt(event, "attacker_player_index", attackerPlayer);
	else DictSet(event, "attacker_player_index", Py_NewRef(Py_None));
	DictSetInt(event, "damage", damage);
	DictSetString(event, "damage_type", mod.GetChars());
	DictSetInt(event, "flags", flags);
	DictSetFloat(event, "angle", angle);
	DictSetBool(event, "cancel", false);
	// InvokeEvent passes this same dict object to every handler and handlers
	// mutate it in place; keep our own reference so the write-backs can be
	// read after dispatch (InvokeEvent releases the reference it is given).
	Py_INCREF(event);
	InvokeEvent("actor_before_damage", event, target, ActorPlayerNumber(target));

	// Mutable contract read-back. All reads are defensive: a missing or
	// wrongly typed key keeps the original value. A handler that raised was
	// already disabled by InvokeEvent; whatever it wrote before raising
	// simply survives here.
	PyObject* cancelValue = PyDict_GetItemString(event, "cancel"); // borrowed
	if (cancelValue != nullptr)
	{
		const int truth = PyObject_IsTrue(cancelValue);
		if (truth < 0) PyErr_Clear(); // a hostile __bool__ cancels nothing
		else if (truth == 1)
		{
			Py_DECREF(event);
			return true;
		}
	}
	PyObject* damageValue = PyDict_GetItemString(event, "damage"); // borrowed
	if (damageValue != nullptr)
	{
		if (PyLong_Check(damageValue))
		{
			const long long value = PyLong_AsLongLong(damageValue);
			if (PyErr_Occurred()) PyErr_Clear();
			else damage = static_cast<int>(std::clamp<long long>(value, 0, 0x7fffffff));
		}
		else if (PyFloat_Check(damageValue))
		{
			const double value = PyFloat_AsDouble(damageValue);
			if (PyErr_Occurred()) PyErr_Clear();
			else if (value <= 0.0) damage = 0; // NaN included: no damage
			else damage = value >= 2147483647.0 ? 0x7fffffff : static_cast<int>(value);
		}
		else
		{
			ReportScriptWarning("actor_before_damage: ignoring non-numeric damage write-back");
		}
	}
	PyObject* typeValue = PyDict_GetItemString(event, "damage_type"); // borrowed
	if (typeValue != nullptr)
	{
		if (PyUnicode_Check(typeValue))
		{
			const char* text = PyUnicode_AsUTF8(typeValue);
			if (text == nullptr) PyErr_Clear();
			else if (text[0] != '\0') mod = FName(text);
			else ReportScriptWarning("actor_before_damage: ignoring empty damage_type write-back");
		}
		else
		{
			ReportScriptWarning("actor_before_damage: ignoring non-string damage_type write-back");
		}
	}
	Py_DECREF(event);
	return false;
}

void OnActorDestroyed(AActor* actor)
{
	if (!HasCallbacks("actor_destroyed")) return;
	PyObject* event = BuildEvent("actor_destroyed");
	DictSet(event, "actor_ref", GameApi::MakeActorRef(actor));
	DictSet(event, "actor", ActorSnapshot(actor));
	InvokeEvent("actor_destroyed", event, actor, ActorPlayerNumber(actor));
}

void OnActorRevived(AActor* actor)
{
	if (!HasCallbacks("actor_revived")) return;
	PyObject* event = BuildEvent("actor_revived");
	DictSet(event, "actor_ref", GameApi::MakeActorRef(actor));
	InvokeEvent("actor_revived", event, actor, ActorPlayerNumber(actor));
}

void OnLineActivated(int lineIndex, AActor* actor, int activationType)
{
	if (!HasCallbacks("line_activated")) return;
	PyObject* event = BuildEvent("line_activated");
	DictSetInt(event, "line_index", lineIndex);
	DictSet(event, "actor_ref", GameApi::MakeActorRef(actor));
	DictSetInt(event, "activation_type", activationType);
	InvokeEvent("line_activated", event, actor, ActorPlayerNumber(actor));
}

void OnLineActivationFailed(int lineIndex, int special, const int* args, AActor* actor, int activationType, int reason)
{
	if (!HasCallbacks("line_activation_failed")) return;
	PyObject* event = BuildEvent("line_activation_failed");
	DictSetInt(event, "line_index", lineIndex);
	DictSetInt(event, "special", special);
	PyObject* argList = PyList_New(5);
	if (argList != nullptr)
	{
		for (int i = 0; i < 5; ++i)
			PyList_SET_ITEM(argList, i, PyLong_FromLong(args == nullptr ? 0 : args[i]));
		DictSet(event, "args", argList);
	}
	DictSet(event, "actor_ref", GameApi::MakeActorRef(actor));
	DictSetInt(event, "activation_type", activationType);
	// Why the activation failed: stable string + numeric ESpecialFailReason code.
	DictSetString(event, "reason", P_SpecialFailReasonName(reason));
	DictSetInt(event, "reason_code", reason);
	InvokeEvent("line_activation_failed", event, actor, ActorPlayerNumber(actor));
}

void OnPlayerEvent(const char* eventName, int playerIndex, bool fromHub)
{
	if (!IsKnownEvent(eventName) || !HasCallbacks(eventName)) return;
	PyObject* event = BuildEvent(eventName);
	DictSetInt(event, "player_index", playerIndex);
	DictSetBool(event, "from_hub", fromHub);
	AActor* actor = playerIndex >= 0 && playerIndex < static_cast<int>(MAXPLAYERS)
		? players[playerIndex].mo : nullptr;
	DictSet(event, "actor_ref", GameApi::MakeActorRef(actor));
	InvokeEvent(eventName, event, actor, playerIndex);
}

void OnItemPicked(AActor* item, AActor* toucher, int amount)
{
	if (item == nullptr || !HasCallbacks("item_picked")) return;
	PyObject* event = BuildEvent("item_picked");
	DictSetString(event, "class_name", item->GetClass()->TypeName.GetChars());
	// GetTag falls back to the class name when the item has no pretty name.
	DictSetString(event, "name", item->GetTag());
	DictSetInt(event, "amount", amount);
	const int playerNumber = ActorPlayerNumber(toucher);
	DictSetInt(event, "player", playerNumber);
	InvokeEvent("item_picked", event, item, playerNumber);
}

void OnItemDropped(AActor* item, AActor* dropper, int amount)
{
	if (item == nullptr || !HasCallbacks("item_dropped")) return;
	PyObject* event = BuildEvent("item_dropped");
	DictSet(event, "actor_ref", GameApi::MakeActorRef(item));
	DictSet(event, "dropper_ref", GameApi::MakeActorRef(dropper));
	const int playerNumber = ActorPlayerNumber(dropper);
	if (playerNumber >= 0) DictSetInt(event, "player_index", playerNumber);
	else DictSet(event, "player_index", Py_NewRef(Py_None));
	DictSetString(event, "class_name", item->GetClass()->TypeName.GetChars());
	DictSetInt(event, "amount", amount);
	InvokeEvent("item_dropped", event, item, playerNumber);
}

void OnWeaponChanged(AActor* pawn, AActor* weapon)
{
	if (!HasCallbacks("weapon_changed")) return;
	const int playerIndex = ActorPlayerNumber(pawn);
	PyObject* event = BuildEvent("weapon_changed");
	DictSetInt(event, "player_index", playerIndex);
	if (weapon != nullptr)
	{
		DictSetString(event, "weapon", weapon->GetClass()->TypeName.GetChars());
		DictSet(event, "actor_ref", GameApi::MakeActorRef(weapon));
	}
	else
	{
		DictSet(event, "weapon", Py_NewRef(Py_None));
		DictSet(event, "actor_ref", Py_NewRef(Py_None));
	}
	if (playerIndex >= 0 && playerIndex < static_cast<int>(MAXPLAYERS) && playeringame[playerIndex])
	{
		DictSet(event, "player_ref", GameApi::MakePlayerRef(playerIndex));
	}
	else
	{
		DictSet(event, "player_ref", Py_NewRef(Py_None));
	}
	AActor* actor = playerIndex >= 0 && playerIndex < static_cast<int>(MAXPLAYERS)
		? players[playerIndex].mo : nullptr;
	InvokeEvent("weapon_changed", event, actor, playerIndex);
}

void OnSecretFound(int playernum)
{
	if (!HasCallbacks("secret_found")) return;
	PyObject* event = BuildEvent("secret_found");
	DictSetInt(event, "player", playernum);
	DictSetInt(event, "found_secrets", primaryLevel == nullptr ? 0 : primaryLevel->found_secrets);
	DictSetInt(event, "total_secrets", primaryLevel == nullptr ? 0 : primaryLevel->total_secrets);
	AActor* actor = playernum >= 0 && playernum < static_cast<int>(MAXPLAYERS)
		? players[playernum].mo : nullptr;
	InvokeEvent("secret_found", event, actor, playernum);
}

// Dispatched once per rendered frame from the Dear ImGui overlay layer
// (BdImGui::Frame), between ImGui::NewFrame() and ImGui::Render(), so that
// Python widgets can emit ImGui draw calls. BuildEvent already enriches the
// dict with name/map/level_time.
void OnImguiFrame()
{
	if (!HasCallbacks("imgui_frame")) return;
	InvokeEvent("imgui_frame", BuildEvent("imgui_frame"));
}

// Dispatched from P_StartConversation once the conversation is successfully
// entered (all early-out checks passed). pc is guaranteed non-null by the
// call site, npc as well; MakeActorRef still maps nullptr to None.
void OnConversationStarted(AActor* npc, AActor* pc, int playerIndex)
{
	if (!HasCallbacks("conversation_started")) return;
	PyObject* event = BuildEvent("conversation_started");
	DictSet(event, "npc_ref", GameApi::MakeActorRef(npc));
	DictSet(event, "pc_ref", GameApi::MakeActorRef(pc));
	if (playerIndex >= 0) DictSetInt(event, "player_index", playerIndex);
	else DictSet(event, "player_index", Py_NewRef(Py_None));
	if (npc != nullptr) DictSetString(event, "npc_class", npc->GetClass()->TypeName.GetChars());
	else DictSet(event, "npc_class", Py_NewRef(Py_None));
	InvokeEvent("conversation_started", event, npc, playerIndex);
}

// Dispatched from HandleReply (p_conversation.cpp), the single commit point
// for conversation replies: it is only reachable from P_ConversationCommand,
// which the netcode/demo stream runs exactly once per reply on each machine.
void OnConversationReply(int playerIndex, AActor* npc, int nodeNumber, int replyIndex,
	int logNumber, const char* logString, int nextNode, bool itemChanged)
{
	if (!HasCallbacks("conversation_reply")) return;
	PyObject* event = BuildEvent("conversation_reply");
	if (playerIndex >= 0) DictSetInt(event, "player_index", playerIndex);
	else DictSet(event, "player_index", Py_NewRef(Py_None));
	DictSet(event, "npc_ref", GameApi::MakeActorRef(npc));
	DictSetInt(event, "node", nodeNumber);
	DictSetInt(event, "reply_index", replyIndex);
	DictSetInt(event, "log_number", logNumber);
	if (logString != nullptr && logString[0] != '\0') DictSetString(event, "log_string", logString);
	else DictSet(event, "log_string", Py_NewRef(Py_None));
	DictSetInt(event, "next_node", nextNode);
	DictSetBool(event, "item_changed", itemChanged);
	AActor* subject = playerIndex >= 0 && playerIndex < static_cast<int>(MAXPLAYERS)
		? players[playerIndex].mo : npc;
	InvokeEvent("conversation_reply", event, subject, playerIndex);
}

// Dispatched from the pyui console command. This is the bridge that lets
// console aliases and key bindings drive script UI: an alias like
// `alias toggle_journal "pyui journal"` reaches Python as a ui_command event
// whose command field is "journal". Pure notification; the handler decides
// what to toggle.
void OnUiCommand(const char* name)
{
	if (name == nullptr || name[0] == '\0' || !HasCallbacks("ui_command")) return;
	PyObject* event = BuildEvent("ui_command");
	DictSetString(event, "command", name);
	InvokeEvent("ui_command", event);
}

void SerializeState(FSerializer& arc)
{
	if (!active) return;
	FString encoded;
	if (arc.isWriting())
	{
		const bool wasBlocked = gameplayMutationBlocked;
		gameplayMutationBlocked = true;
		InvokeEvent("save", BuildEvent("save"));
		gameplayMutationBlocked = wasBlocked;
		encoded = DumpStateJson().c_str();
		if (encoded.IsEmpty() && stateDictionary != nullptr && PyDict_Size(stateDictionary) > 0)
		{
			// Abort the save rather than archive an empty blob over a
			// non-empty bd.state: the savegame would load back with all
			// Python state silently lost.
			I_Error("Save aborted: biaseddoom.state could not be serialized; writing the savegame would lose non-empty Python state.");
		}
	}
	arc("pythonstate", encoded);
	if (arc.isReading() && encoded.IsNotEmpty())
	{
		loadCallbackPending = LoadStateJson(encoded.GetChars());
	}
}

void FinishLoadState()
{
	if (!active || !loadCallbackPending) return;
	loadCallbackPending = false;
	InvokeEvent("load", BuildEvent("load"));
}

void PrintStatus()
{
	Printf("Python scripting: compiled (CPython %s), runtime %s, trust opt-in %s\n",
		PY_VERSION, active ? "active" : "inactive", RuntimeRequested() ? "enabled" : "disabled");
	Printf("Python manifests: %zu valid script entr%s; modules: %zu; callbacks: %zu\n",
		discoveredScripts.size(), discoveredScripts.size() == 1 ? "y" : "ies", modules.size(), callbacks.size());
	for (const ScriptModule& module : modules)
	{
		Printf("  %s (resource container %d)\n", module.Path.c_str(), module.Container);
	}
	if (active)
	{
		Printf("Python tick budget: %d ms, hard between-callback enforcement %s, overrun disable limit %d, skips: %llu, overruns: %llu\n",
			static_cast<int>(py_tick_budget_ms), py_tick_hard_budget ? "on" : "off",
			static_cast<int>(py_tick_overrun_limit),
			static_cast<unsigned long long>(tickBudgetSkips),
			static_cast<unsigned long long>(tickBudgetOverruns));
		for (const Callback& callback : callbacks)
		{
			if (callback.Calls == 0 && callback.BudgetSkips == 0) continue;
			Printf("  %s %s: %llu calls, avg %.3f ms, max %.3f ms, %llu skips, %llu overruns%s\n",
				callback.Event.c_str(), callback.Source.c_str(),
				static_cast<unsigned long long>(callback.Calls),
				callback.Calls == 0 ? 0.0 : callback.TotalMicroseconds / (1000.0 * callback.Calls),
				callback.MaximumMicroseconds / 1000.0,
				static_cast<unsigned long long>(callback.BudgetSkips),
				static_cast<unsigned long long>(callback.BudgetOverruns),
				callback.BudgetDisabled ? ", disabled" : "");
		}
	}
}

#else // BIASEDDOOM_PYTHON

bool IsCompiled() { return false; }
bool IsActive() { return false; }
void* BeginIdleWait() { return nullptr; }
void EndIdleWait(void*) {}
bool Initialize()
{
	const bool disabled = Args != nullptr && Args->CheckParm("-nopython");
	if (!disabled && (py_enabled || (Args != nullptr && Args->CheckParm("-python"))))
	{
		Printf(TEXTCOLOR_YELLOW "Python scripting was requested, but this executable was built without CPython support. ACS and ZScript are still available.\n");
	}
	return false;
}
void Shutdown() {}
bool Reload() { return false; }
void OnWorldLoaded() {}
void OnWorldUnloaded(const char*) {}
void OnWorldPreTick() {}
void OnWorldTick() {}
void OnWorldPostTick() {}
void OnActorSpawned(AActor*) {}
void OnActorDied(AActor*, AActor*, AActor*) {}
void OnActorDamaged(AActor*, AActor*, AActor*, int, const char*, int, double) {}
bool OnBeforeDamage(AActor*, AActor*, AActor*, int&, FName&, int, double) { return false; }
void OnActorDestroyed(AActor*) {}
void OnActorRevived(AActor*) {}
void OnLineActivated(int, AActor*, int) {}
void OnLineActivationFailed(int, int, const int*, AActor*, int, int) {}
void OnPlayerEvent(const char*, int, bool) {}
void OnItemPicked(AActor*, AActor*, int) {}
void OnItemDropped(AActor*, AActor*, int) {}
void OnWeaponChanged(AActor*, AActor*) {}
void OnSecretFound(int) {}
void OnImguiFrame() {}
void OnConversationStarted(AActor*, AActor*, int) {}
void OnConversationReply(int, AActor*, int, int, int, const char*, int, bool) {}
void OnUiCommand(const char*) {}
unsigned int GetErrorCount() { return 0; }
void SetErrorLogPath(const char*) {}
void ReportScriptWarning(const char*, ...) {}
void DumpStub(const char*)
{
	Printf("Python scripting is not compiled into this executable.\n");
}
void SerializeState(FSerializer&) {}
void FinishLoadState() {}
bool CheckApiThread() { return false; }
bool CheckGameplayMutation() { return false; }
bool CheckSessionMutation() { return false; }
bool CheckLocalPresentation() { return false; }
void PrintStatus()
{
	Printf("Python scripting: not compiled into this executable. Configure with -DBIASEDDOOM_ENABLE_PYTHON=ON and CPython 3.10+ development files.\n");
}

#endif // BIASEDDOOM_PYTHON
} // namespace PythonRuntime

CCMD(py_status)
{
	PythonRuntime::PrintStatus();
}

CCMD(dumppystub)
{
	PythonRuntime::DumpStub(argv.argc() > 1 ? argv[1] : nullptr);
}

// pyui is a plain CCMD like py_status: it only dispatches a ui_command
// notification event and never mutates world state, so it does not need the
// UNSAFE_CCMD netplay/demo treatment py_reload gets. Being blocked during
// demo playback is acceptable for a UI toggle. It is registered even in
// stub builds (same as the other py_ commands); PythonRuntime::OnUiCommand
// is then a no-op.
CCMD(pyui)
{
	if (argv.argc() != 2)
	{
		Printf("usage: pyui <name> - dispatch a ui_command event to Python scripts (for console aliases and key binds)\n");
		return;
	}
	PythonRuntime::OnUiCommand(argv[1]);
}

UNSAFE_CCMD(py_reload)
{
	if (!PythonRuntime::IsCompiled())
	{
		PythonRuntime::PrintStatus();
		return;
	}
	if (!PythonRuntime::Reload())
	{
		Printf(TEXTCOLOR_RED "Python scripts could not be reloaded. Check the preceding log messages.\n");
	}
}
