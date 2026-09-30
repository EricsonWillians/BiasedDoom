//---------------------------------------------------------------------------
//
// BiasedDoom Dear ImGui Python bindings (bd.imgui)
//
// Thin wrappers over the vendored Dear ImGui 1.92.8, submitted through the
// engine overlay layer (src/common/imgui/bd_imgui.cpp). Every call crosses
// the C API once per widget and is only legal while an imgui_frame handler
// runs (between ImGui::NewFrame and ImGui::Render); the guard enforces that
// with a RuntimeError instead of letting a stray call corrupt the context.
// When no script registers imgui_frame, the entire feature costs one
// HasCallbacks check per frame (see PythonRuntime::OnImguiFrame).
//
//---------------------------------------------------------------------------

#include "python_imgui.h"
#include "python_runtime.h"

#ifdef BIASEDDOOM_IMGUI

// BIASEDDOOM_IMGUI implies BIASEDDOOM_PYTHON (the overlay is only enabled
// when the embedded runtime is available; see the root CMakeLists.txt).
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "imgui.h"

#include "common/imgui/bd_imgui.h"
#include "python_game_api.h"
#include "texturemanager.h"
#include "gametexture.h"
#include "actor.h"
#include "r_state.h"
#include "r_data/sprites.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifdef _PyCFunction_CAST
#define BD_IMGUI_KEYWORD_FUNCTION(function) _PyCFunction_CAST(function)
#else
#define BD_IMGUI_KEYWORD_FUNCTION(function) \
	reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(function))
#endif

namespace PythonImGui
{
namespace
{

//---------------------------------------------------------------------------
// Call guard. Every widget function runs this first: it pins the engine's
// ImGui context as current and rejects calls made outside an imgui_frame
// handler, where ImGui state is not between NewFrame and Render.
//---------------------------------------------------------------------------

bool BeginImGuiCall()
{
	if (!PythonRuntime::CheckApiThread()) return false;
	if (!BdImGui::FrameActive())
	{
		PyErr_SetString(PyExc_RuntimeError,
			"bd.imgui calls are only valid inside an imgui_frame handler");
		return false;
	}
	ImGui::SetCurrentContext(BdImGui::Context());
	return true;
}

PyObject* ReturnBool(bool value)
{
	return PyBool_FromLong(value ? 1 : 0);
}

PyObject* ReturnChangedValue(bool changed, PyObject* value)
{
	if (value == nullptr) return nullptr;
	PyObject* changedValue = PyBool_FromLong(changed ? 1 : 0);
	if (changedValue == nullptr)
	{
		Py_DECREF(value);
		return nullptr;
	}
	PyObject* result = PyTuple_Pack(2, changedValue, value);
	Py_DECREF(changedValue);
	Py_DECREF(value);
	return result;
}

//---------------------------------------------------------------------------
// Windows and layout
//---------------------------------------------------------------------------

PyObject* ImBegin(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "name", "open", "flags", nullptr };
	const char* name = nullptr;
	PyObject* openObject = Py_None;
	int flags = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|Oi:begin", const_cast<char**>(keywords),
		&name, &openObject, &flags)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	if (openObject == Py_None)
	{
		return ReturnBool(ImGui::Begin(name, nullptr, flags));
	}
	bool open = PyObject_IsTrue(openObject) == 1;
	const bool expanded = ImGui::Begin(name, &open, flags);
	return Py_BuildValue("(NN)", PyBool_FromLong(expanded ? 1 : 0), PyBool_FromLong(open ? 1 : 0));
}

PyObject* ImEnd(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::End();
	Py_RETURN_NONE;
}

PyObject* ImBeginChild(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "id", "size", "border", "flags", nullptr };
	const char* id = nullptr;
	double width = 0.0, height = 0.0;
	int border = 0;
	int flags = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|(dd)pi:begin_child", const_cast<char**>(keywords),
		&id, &width, &height, &border, &flags)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	const ImGuiChildFlags childFlags = border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None;
	return ReturnBool(ImGui::BeginChild(id, ImVec2((float)width, (float)height), childFlags, flags));
}

PyObject* ImEndChild(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::EndChild();
	Py_RETURN_NONE;
}

PyObject* ImSetNextWindowPos(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "x", "y", "cond", nullptr };
	float x = 0.0f, y = 0.0f;
	int cond = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ff|i:set_next_window_pos", const_cast<char**>(keywords),
		&x, &y, &cond)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::SetNextWindowPos(ImVec2(x, y), cond);
	Py_RETURN_NONE;
}

PyObject* ImSetNextWindowSize(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "w", "h", "cond", nullptr };
	float w = 0.0f, h = 0.0f;
	int cond = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ff|i:set_next_window_size", const_cast<char**>(keywords),
		&w, &h, &cond)) return nullptr;
	// Non-finite sizes would poison ImGui's window layout and clip rects.
	if (!std::isfinite(w) || !std::isfinite(h))
	{
		PyErr_SetString(PyExc_ValueError, "set_next_window_size values must be finite");
		return nullptr;
	}
	if (!BeginImGuiCall()) return nullptr;
	ImGui::SetNextWindowSize(ImVec2(w, h), cond);
	Py_RETURN_NONE;
}

PyObject* ImSetNextWindowCollapsed(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "collapsed", "cond", nullptr };
	int collapsed = 0;
	int cond = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "p|i:set_next_window_collapsed", const_cast<char**>(keywords),
		&collapsed, &cond)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::SetNextWindowCollapsed(collapsed != 0, cond);
	Py_RETURN_NONE;
}

PyObject* ImSetNextWindowBgAlpha(PyObject*, PyObject* args)
{
	float alpha = 1.0f;
	if (!PyArg_ParseTuple(args, "f:set_next_window_bg_alpha", &alpha)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::SetNextWindowBgAlpha(alpha);
	Py_RETURN_NONE;
}

PyObject* ImIsWindowFocused(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::IsWindowFocused());
}

PyObject* ImIsWindowHovered(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::IsWindowHovered());
}

PyObject* ImGetWindowPos(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	const ImVec2 pos = ImGui::GetWindowPos();
	return Py_BuildValue("(ff)", pos.x, pos.y);
}

PyObject* ImGetWindowSize(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	const ImVec2 size = ImGui::GetWindowSize();
	return Py_BuildValue("(ff)", size.x, size.y);
}

//---------------------------------------------------------------------------
// Text widgets
//---------------------------------------------------------------------------

PyObject* ImText(PyObject*, PyObject* args)
{
	const char* text = nullptr;
	if (!PyArg_ParseTuple(args, "s:text", &text)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::TextUnformatted(text);
	Py_RETURN_NONE;
}

PyObject* ImTextColored(PyObject*, PyObject* args)
{
	float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
	const char* text = nullptr;
	if (!PyArg_ParseTuple(args, "ffffs:text_colored", &r, &g, &b, &a, &text)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::TextColored(ImVec4(r, g, b, a), "%s", text);
	Py_RETURN_NONE;
}

PyObject* ImTextDisabled(PyObject*, PyObject* args)
{
	const char* text = nullptr;
	if (!PyArg_ParseTuple(args, "s:text_disabled", &text)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::TextDisabled("%s", text);
	Py_RETURN_NONE;
}

PyObject* ImTextWrapped(PyObject*, PyObject* args)
{
	const char* text = nullptr;
	if (!PyArg_ParseTuple(args, "s:text_wrapped", &text)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::TextWrapped("%s", text);
	Py_RETURN_NONE;
}

PyObject* ImLabelText(PyObject*, PyObject* args)
{
	const char* label = nullptr;
	const char* text = nullptr;
	if (!PyArg_ParseTuple(args, "ss:label_text", &label, &text)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::LabelText(label, "%s", text);
	Py_RETURN_NONE;
}

PyObject* ImBulletText(PyObject*, PyObject* args)
{
	const char* text = nullptr;
	if (!PyArg_ParseTuple(args, "s:bullet_text", &text)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::BulletText("%s", text);
	Py_RETURN_NONE;
}

//---------------------------------------------------------------------------
// Buttons and value widgets
//---------------------------------------------------------------------------

PyObject* ImButton(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "w", "h", nullptr };
	const char* label = nullptr;
	float w = 0.0f, h = 0.0f;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|ff:button", const_cast<char**>(keywords),
		&label, &w, &h)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::Button(label, ImVec2(w, h)));
}

PyObject* ImSmallButton(PyObject*, PyObject* args)
{
	const char* label = nullptr;
	if (!PyArg_ParseTuple(args, "s:small_button", &label)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::SmallButton(label));
}

PyObject* ImCheckbox(PyObject*, PyObject* args)
{
	const char* label = nullptr;
	int checked = 0;
	if (!PyArg_ParseTuple(args, "sp:checkbox", &label, &checked)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	bool value = checked != 0;
	const bool changed = ImGui::Checkbox(label, &value);
	return ReturnChangedValue(changed, PyBool_FromLong(value ? 1 : 0));
}

PyObject* ImRadioButton(PyObject*, PyObject* args)
{
	const char* label = nullptr;
	int active = 0;
	if (!PyArg_ParseTuple(args, "sp:radio_button", &label, &active)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::RadioButton(label, active != 0));
}

PyObject* ImSliderInt(PyObject*, PyObject* args)
{
	const char* label = nullptr;
	int value = 0, minimum = 0, maximum = 0;
	if (!PyArg_ParseTuple(args, "siii:slider_int", &label, &value, &minimum, &maximum)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	const bool changed = ImGui::SliderInt(label, &value, minimum, maximum);
	return ReturnChangedValue(changed, PyLong_FromLong(value));
}

PyObject* ImSliderFloat(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "value", "min", "max", "format", nullptr };
	const char* label = nullptr;
	float value = 0.0f, minimum = 0.0f, maximum = 0.0f;
	const char* format = "%.3f";
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sfff|s:slider_float", const_cast<char**>(keywords),
		&label, &value, &minimum, &maximum, &format)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	const bool changed = ImGui::SliderFloat(label, &value, minimum, maximum, format);
	return ReturnChangedValue(changed, PyFloat_FromDouble(value));
}

PyObject* ImDragInt(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "value", "speed", "min", "max", nullptr };
	const char* label = nullptr;
	int value = 0;
	float speed = 1.0f;
	int minimum = 0, maximum = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "si|fii:drag_int", const_cast<char**>(keywords),
		&label, &value, &speed, &minimum, &maximum)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	const bool changed = ImGui::DragInt(label, &value, speed, minimum, maximum);
	return ReturnChangedValue(changed, PyLong_FromLong(value));
}

PyObject* ImDragFloat(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "value", "speed", "min", "max", "format", nullptr };
	const char* label = nullptr;
	float value = 0.0f;
	float speed = 1.0f;
	float minimum = 0.0f, maximum = 0.0f;
	const char* format = "%.3f";
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sf|fffs:drag_float", const_cast<char**>(keywords),
		&label, &value, &speed, &minimum, &maximum, &format)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	const bool changed = ImGui::DragFloat(label, &value, speed, minimum, maximum, format);
	return ReturnChangedValue(changed, PyFloat_FromDouble(value));
}

PyObject* ImInputText(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "text", "max_length", "flags", nullptr };
	const char* label = nullptr;
	const char* text = nullptr;
	int maxLength = 256;
	int flags = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss|ii:input_text", const_cast<char**>(keywords),
		&label, &text, &maxLength, &flags)) return nullptr;
	if (maxLength < 1 || maxLength > 65536)
	{
		PyErr_SetString(PyExc_ValueError, "max_length must be between 1 and 65536");
		return nullptr;
	}
	if (!BeginImGuiCall()) return nullptr;

	// Small inputs stay on the stack; larger ones spill to a heap buffer.
	char stackBuffer[512];
	std::vector<char> heapBuffer;
	char* buffer = stackBuffer;
	size_t bufferSize = (size_t)maxLength + 1;
	if (bufferSize > sizeof(stackBuffer))
	{
		heapBuffer.resize(bufferSize);
		buffer = heapBuffer.data();
	}
	std::strncpy(buffer, text, bufferSize - 1);
	buffer[bufferSize - 1] = '\0';

	const bool changed = ImGui::InputText(label, buffer, bufferSize, (ImGuiInputTextFlags)flags);
	// A byte-length limit can cut a valid Python string in the middle of a
	// UTF-8 codepoint. Decode with replacement so that an invalid temporary
	// buffer reports a normal value instead of failing the imgui_frame handler.
	return ReturnChangedValue(changed, PyUnicode_DecodeUTF8(buffer, strlen(buffer), "replace"));
}

PyObject* ImInputInt(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "value", "step", "step_fast", nullptr };
	const char* label = nullptr;
	int value = 0;
	int step = 1, stepFast = 100;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "si|ii:input_int", const_cast<char**>(keywords),
		&label, &value, &step, &stepFast)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	const bool changed = ImGui::InputInt(label, &value, step, stepFast);
	return ReturnChangedValue(changed, PyLong_FromLong(value));
}

PyObject* ImInputFloat(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "value", "step", "step_fast", "format", nullptr };
	const char* label = nullptr;
	float value = 0.0f;
	float step = 0.0f, stepFast = 0.0f;
	const char* format = "%.3f";
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sf|ffs:input_float", const_cast<char**>(keywords),
		&label, &value, &step, &stepFast, &format)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	const bool changed = ImGui::InputFloat(label, &value, step, stepFast, format);
	return ReturnChangedValue(changed, PyFloat_FromDouble(value));
}

//---------------------------------------------------------------------------
// Item sequences (combo, list box, plot values)
//---------------------------------------------------------------------------

// Converts a Python sequence of str into owned strings; raises on failure.
bool ParseStringSequence(PyObject* object, const char* argument, std::vector<std::string>& out)
{
	PyObject* fast = PySequence_Fast(object, "items must be a sequence of str");
	if (fast == nullptr) return false;
	const Py_ssize_t count = PySequence_Fast_GET_SIZE(fast);
	out.clear();
	out.reserve((size_t)count);
	PyObject** items = PySequence_Fast_ITEMS(fast);
	for (Py_ssize_t i = 0; i < count; ++i)
	{
		const char* text = PyUnicode_AsUTF8(items[i]);
		if (text == nullptr)
		{
			Py_DECREF(fast);
			if (!PyErr_Occurred())
				PyErr_Format(PyExc_TypeError, "%s must be a sequence of str", argument);
			return false;
		}
		out.emplace_back(text);
	}
	Py_DECREF(fast);
	return true;
}

PyObject* ImCombo(PyObject*, PyObject* args)
{
	const char* label = nullptr;
	int current = 0;
	PyObject* itemsObject = nullptr;
	if (!PyArg_ParseTuple(args, "siO:combo", &label, &current, &itemsObject)) return nullptr;
	std::vector<std::string> items;
	if (!ParseStringSequence(itemsObject, "items", items)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;

	bool changed = false;
	const char* preview = (current >= 0 && current < (int)items.size()) ? items[(size_t)current].c_str() : "";
	if (ImGui::BeginCombo(label, preview))
	{
		for (int i = 0; i < (int)items.size(); ++i)
		{
			const bool selected = (i == current);
			if (ImGui::Selectable(items[(size_t)i].c_str(), selected))
			{
				current = i;
				changed = true;
			}
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	return ReturnChangedValue(changed, PyLong_FromLong(current));
}

PyObject* ImListBox(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "current_index", "items", "height_items", nullptr };
	const char* label = nullptr;
	int current = 0;
	PyObject* itemsObject = nullptr;
	int heightItems = -1;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "siO|i:list_box", const_cast<char**>(keywords),
		&label, &current, &itemsObject, &heightItems)) return nullptr;
	std::vector<std::string> items;
	if (!ParseStringSequence(itemsObject, "items", items)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;

	std::vector<const char*> pointers;
	pointers.reserve(items.size());
	for (const std::string& item : items)
		pointers.push_back(item.c_str());
	const bool changed = ImGui::ListBox(label, &current, pointers.data(), (int)pointers.size(), heightItems);
	return ReturnChangedValue(changed, PyLong_FromLong(current));
}

PyObject* ImSelectable(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "selected", "flags", nullptr };
	const char* label = nullptr;
	int selected = 0;
	int flags = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sp|i:selectable", const_cast<char**>(keywords),
		&label, &selected, &flags)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::Selectable(label, selected != 0, (ImGuiSelectableFlags)flags));
}

//---------------------------------------------------------------------------
// Trees and headers
//---------------------------------------------------------------------------

PyObject* ImTreeNode(PyObject*, PyObject* args)
{
	const char* label = nullptr;
	if (!PyArg_ParseTuple(args, "s:tree_node", &label)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::TreeNode(label));
}

PyObject* ImTreePop(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::TreePop();
	Py_RETURN_NONE;
}

PyObject* ImCollapsingHeader(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "flags", nullptr };
	const char* label = nullptr;
	int flags = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|i:collapsing_header", const_cast<char**>(keywords),
		&label, &flags)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::CollapsingHeader(label, (ImGuiTreeNodeFlags)flags));
}

//---------------------------------------------------------------------------
// Layout primitives
//---------------------------------------------------------------------------

PyObject* ImSeparator(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::Separator();
	Py_RETURN_NONE;
}

PyObject* ImSameLine(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "offset", "spacing", nullptr };
	float offset = 0.0f, spacing = -1.0f;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|ff:same_line", const_cast<char**>(keywords),
		&offset, &spacing)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::SameLine(offset, spacing);
	Py_RETURN_NONE;
}

PyObject* ImSpacing(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::Spacing();
	Py_RETURN_NONE;
}

PyObject* ImNewline(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::NewLine();
	Py_RETURN_NONE;
}

PyObject* ImIndent(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "width", nullptr };
	float width = 0.0f;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|f:indent", const_cast<char**>(keywords), &width)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::Indent(width);
	Py_RETURN_NONE;
}

PyObject* ImUnindent(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "width", nullptr };
	float width = 0.0f;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|f:unindent", const_cast<char**>(keywords), &width)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::Unindent(width);
	Py_RETURN_NONE;
}

PyObject* ImAlignTextToFramePadding(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::AlignTextToFramePadding();
	Py_RETURN_NONE;
}

//---------------------------------------------------------------------------
// Tables
//---------------------------------------------------------------------------

PyObject* ImBeginTable(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "id", "columns", "flags", nullptr };
	const char* id = nullptr;
	int columns = 0;
	int flags = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "si|i:begin_table", const_cast<char**>(keywords),
		&id, &columns, &flags)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::BeginTable(id, columns, (ImGuiTableFlags)flags));
}

PyObject* ImEndTable(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::EndTable();
	Py_RETURN_NONE;
}

PyObject* ImTableNextRow(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "flags", "min_height", nullptr };
	int flags = 0;
	float minHeight = 0.0f;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|if:table_next_row", const_cast<char**>(keywords),
		&flags, &minHeight)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::TableNextRow((ImGuiTableRowFlags)flags, minHeight);
	Py_RETURN_NONE;
}

PyObject* ImTableNextColumn(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::TableNextColumn());
}

PyObject* ImTableSetupColumn(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "flags", "init_width", nullptr };
	const char* label = nullptr;
	int flags = 0;
	float initWidth = 0.0f;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|if:table_setup_column", const_cast<char**>(keywords),
		&label, &flags, &initWidth)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::TableSetupColumn(label, (ImGuiTableColumnFlags)flags, initWidth);
	Py_RETURN_NONE;
}

PyObject* ImTableHeadersRow(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::TableHeadersRow();
	Py_RETURN_NONE;
}

//---------------------------------------------------------------------------
// Progress, colors, plots, images
//---------------------------------------------------------------------------

PyObject* ImProgressBar(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "fraction", "w", "h", "overlay", nullptr };
	float fraction = 0.0f;
	float w = -1.0f, h = 0.0f;
	const char* overlay = nullptr;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "f|ffz:progress_bar", const_cast<char**>(keywords),
		&fraction, &w, &h, &overlay)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	// ImGui's full-width sentinel is a negative width; -FLT_MIN is the stock
	// default and plain -1 fills the row minus one pixel.
	ImGui::ProgressBar(fraction, ImVec2(w < 0.0f ? -FLT_MIN : w, h), overlay);
	Py_RETURN_NONE;
}

PyObject* ImColorEdit3(PyObject*, PyObject* args)
{
	const char* label = nullptr;
	float col[3] = { 0.0f, 0.0f, 0.0f };
	if (!PyArg_ParseTuple(args, "sfff:color_edit3", &label, &col[0], &col[1], &col[2])) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	const bool changed = ImGui::ColorEdit3(label, col);
	return Py_BuildValue("(Nfff)", PyBool_FromLong(changed ? 1 : 0), col[0], col[1], col[2]);
}

PyObject* ImColorEdit4(PyObject*, PyObject* args)
{
	const char* label = nullptr;
	float col[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	if (!PyArg_ParseTuple(args, "sffff:color_edit4", &label, &col[0], &col[1], &col[2], &col[3])) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	const bool changed = ImGui::ColorEdit4(label, col);
	return Py_BuildValue("(Nffff)", PyBool_FromLong(changed ? 1 : 0), col[0], col[1], col[2], col[3]);
}

PyObject* ImPlotLines(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "values", "overlay", "scale_min", "scale_max", "w", "h", nullptr };
	const char* label = nullptr;
	PyObject* valuesObject = nullptr;
	const char* overlay = nullptr;
	float scaleMin = FLT_MAX, scaleMax = FLT_MAX;
	float w = 0.0f, h = 0.0f;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO|zffff:plot_lines", const_cast<char**>(keywords),
		&label, &valuesObject, &overlay, &scaleMin, &scaleMax, &w, &h)) return nullptr;

	PyObject* fast = PySequence_Fast(valuesObject, "values must be a sequence of numbers");
	if (fast == nullptr) return nullptr;
	const Py_ssize_t count = PySequence_Fast_GET_SIZE(fast);
	std::vector<float> values;
	values.reserve((size_t)count);
	PyObject** items = PySequence_Fast_ITEMS(fast);
	for (Py_ssize_t i = 0; i < count; ++i)
	{
		const double v = PyFloat_AsDouble(items[i]);
		if (v == -1.0 && PyErr_Occurred())
		{
			Py_DECREF(fast);
			return nullptr;
		}
		values.push_back((float)v);
	}
	Py_DECREF(fast);
	if (!BeginImGuiCall()) return nullptr;
	if (values.empty())
	{
		ImGui::PlotLines(label, nullptr, 0, 0, overlay, scaleMin, scaleMax, ImVec2(w, h));
	}
	else
	{
		ImGui::PlotLines(label, values.data(), (int)values.size(), 0, overlay, scaleMin, scaleMax, ImVec2(w, h));
	}
	Py_RETURN_NONE;
}

//---------------------------------------------------------------------------
// Image texture resolution, shared by image() and image_size(). Accepts:
//  - a texture name string: MiscPatch + TryAny first (the same lookup the
//    display list's draw_texture uses, so any lump a mod can draw on the
//    canvas works here), then a sprite-namespace fallback (PLAYA1 style
//    sprite frame names are not reachable through the MiscPatch lookup).
//  - an Actor handle: the texture of the actor's current sprite frame,
//    rotation 0 (the front view), resolved through the global sprite tables
//    exactly like FState::GetSpriteTexture does for scripts.
//---------------------------------------------------------------------------

FGameTexture* ResolveImageTexture(PyObject* object)
{
	if (PyUnicode_Check(object))
	{
		const char* name = PyUnicode_AsUTF8(object);
		if (name == nullptr) return nullptr;
		FGameTexture* texture = TexMan.FindGameTexture(name, ETextureType::MiscPatch, FTextureManager::TEXMAN_TryAny);
		if (texture == nullptr)
			texture = TexMan.FindGameTexture(name, ETextureType::Sprite, FTextureManager::TEXMAN_TryAny);
		if (texture == nullptr || !texture->isValid())
		{
			PyErr_Format(PyExc_ValueError, "unknown texture '%s'", name);
			return nullptr;
		}
		return texture;
	}

	AActor* actor = PythonRuntime::GameApi::ActorFromHandle(object);
	if (actor == nullptr)
	{
		if (!PyErr_Occurred())
			PyErr_SetString(PyExc_TypeError, "texture must be a texture name string or an Actor handle");
		return nullptr;
	}
	const int sprnum = actor->sprite;
	if (sprnum < 0 || sprnum >= (int)sprites.Size() || actor->frame >= sprites[sprnum].numframes)
	{
		PyErr_SetString(PyExc_ValueError, "actor has no current sprite frame");
		return nullptr;
	}
	const spriteframe_t& sprframe = SpriteFrames[sprites[sprnum].spriteframes + actor->frame];
	FGameTexture* texture = TexMan.GetGameTexture(sprframe.Texture[0], true);
	if (texture == nullptr || !texture->isValid())
	{
		PyErr_SetString(PyExc_ValueError, "actor's current sprite frame has no drawable texture");
		return nullptr;
	}
	return texture;
}

PyObject* ImImage(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "texture", "w", "h", "uv0", "uv1", "tint", "border", nullptr };
	PyObject* textureObject = nullptr;
	float w = 0.0f, h = 0.0f;
	float uv0x = 0.0f, uv0y = 0.0f, uv1x = 1.0f, uv1y = 1.0f;
	float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f, tintA = 1.0f;
	float borderR = 0.0f, borderG = 0.0f, borderB = 0.0f, borderA = 0.0f;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|ff(ff)(ff)(ffff)(ffff):image", const_cast<char**>(keywords),
		&textureObject, &w, &h, &uv0x, &uv0y, &uv1x, &uv1y,
		&tintR, &tintG, &tintB, &tintA, &borderR, &borderG, &borderB, &borderA)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;

	FGameTexture* texture = ResolveImageTexture(textureObject);
	if (texture == nullptr) return nullptr;
	// w/h 0 means the texture's natural display size (scale factors applied).
	if (w <= 0.0f) w = texture->GetDisplayWidth();
	if (h <= 0.0f) h = texture->GetDisplayHeight();

	// ImGui 1.92: Image() lost its tint/border parameters; tint lives on
	// ImageWithBg() and a per-image border rides on the ImageBorderSize style
	// var plus the Border color. The overlay's draw-data translator resolves
	// ImTextureID back to the FGameTexture pointer, which is exactly what the
	// resolver returns.
	const ImTextureRef ref((ImTextureID)(intptr_t)texture);
	if (borderA > 0.0f)
	{
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(borderR, borderG, borderB, borderA));
		ImGui::PushStyleVar(ImGuiStyleVar_ImageBorderSize, 1.0f);
	}
	ImGui::ImageWithBg(ref, ImVec2(w, h), ImVec2(uv0x, uv0y), ImVec2(uv1x, uv1y),
		ImVec4(0, 0, 0, 0), ImVec4(tintR, tintG, tintB, tintA));
	if (borderA > 0.0f)
	{
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}
	Py_RETURN_NONE;
}

PyObject* ImImageSize(PyObject*, PyObject* args)
{
	PyObject* textureObject = nullptr;
	if (!PyArg_ParseTuple(args, "O:image_size", &textureObject)) return nullptr;
	// No ImGui state is touched here (only TexMan and the sprite tables), so
	// this one is legal outside imgui_frame like master_visible().
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	FGameTexture* texture = ResolveImageTexture(textureObject);
	if (texture == nullptr) return nullptr;
	return Py_BuildValue("(ff)", texture->GetDisplayWidth(), texture->GetDisplayHeight());
}

//---------------------------------------------------------------------------
// Docking (requires the docking branch of Dear ImGui, which is what
// libraries/imgui vendors; multi-viewport stays disabled, see BdImGui::Init)
//---------------------------------------------------------------------------

PyObject* ImDockSpaceOverViewport(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "flags", nullptr };
	int flags = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i:dock_space_over_viewport", const_cast<char**>(keywords),
		&flags)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
#ifdef IMGUI_HAS_DOCK
	return PyLong_FromUnsignedLong((unsigned long)ImGui::DockSpaceOverViewport(0, nullptr, (ImGuiDockNodeFlags)flags));
#else
	PyErr_SetString(PyExc_RuntimeError, "docking requires the Dear ImGui docking branch");
	return nullptr;
#endif
}

PyObject* ImDockSpace(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "id", "w", "h", "flags", nullptr };
	unsigned int id = 0;
	float w = 0.0f, h = 0.0f;
	int flags = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "I|ffi:dock_space", const_cast<char**>(keywords),
		&id, &w, &h, &flags)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
#ifdef IMGUI_HAS_DOCK
	return PyLong_FromUnsignedLong((unsigned long)ImGui::DockSpace((ImGuiID)id, ImVec2(w, h), (ImGuiDockNodeFlags)flags));
#else
	PyErr_SetString(PyExc_RuntimeError, "docking requires the Dear ImGui docking branch");
	return nullptr;
#endif
}

PyObject* ImSetNextWindowDockID(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "id", "cond", nullptr };
	unsigned int id = 0;
	int cond = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "I|i:set_next_window_dock_id", const_cast<char**>(keywords),
		&id, &cond)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
#ifdef IMGUI_HAS_DOCK
	ImGui::SetNextWindowDockID((ImGuiID)id, (ImGuiCond)cond);
	Py_RETURN_NONE;
#else
	PyErr_SetString(PyExc_RuntimeError, "docking requires the Dear ImGui docking branch");
	return nullptr;
#endif
}

//---------------------------------------------------------------------------
// Menus and tooltips
//---------------------------------------------------------------------------

PyObject* ImBeginMainMenuBar(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::BeginMainMenuBar());
}

PyObject* ImEndMainMenuBar(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::EndMainMenuBar();
	Py_RETURN_NONE;
}

PyObject* ImBeginMenu(PyObject*, PyObject* args)
{
	const char* label = nullptr;
	if (!PyArg_ParseTuple(args, "s:begin_menu", &label)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::BeginMenu(label));
}

PyObject* ImEndMenu(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::EndMenu();
	Py_RETURN_NONE;
}

PyObject* ImMenuItem(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "label", "shortcut", "selected", "enabled", nullptr };
	const char* label = nullptr;
	const char* shortcut = nullptr;
	int selected = 0;
	int enabled = 1;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|zpp:menu_item", const_cast<char**>(keywords),
		&label, &shortcut, &selected, &enabled)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::MenuItem(label, shortcut, selected != 0, enabled != 0));
}

PyObject* ImBeginTooltip(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::BeginTooltip());
}

PyObject* ImEndTooltip(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::EndTooltip();
	Py_RETURN_NONE;
}

PyObject* ImSetTooltip(PyObject*, PyObject* args)
{
	const char* text = nullptr;
	if (!PyArg_ParseTuple(args, "s:set_tooltip", &text)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::SetTooltip("%s", text);
	Py_RETURN_NONE;
}

//---------------------------------------------------------------------------
// Item and input state
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Any-event style/config calls. Unlike BeginImGuiCall() these do not
// require an active frame, but they do require the overlay context to
// exist (i.e. at least one Frame() has run); before that there is no
// ImGuiStyle instance to mutate.
//---------------------------------------------------------------------------

bool BeginStyleCall()
{
	if (!PythonRuntime::CheckApiThread()) return false;
	if (BdImGui::Context() == nullptr)
	{
		PyErr_SetString(PyExc_RuntimeError,
			"bd.imgui style calls require an initialized overlay (no frame has run yet)");
		return false;
	}
	ImGui::SetCurrentContext(BdImGui::Context());
	return true;
}

//---------------------------------------------------------------------------
// Fonts (any-event calls; the layer rebuilds the atlas outside the frame)
//---------------------------------------------------------------------------

PyObject* ImAddFontTTF(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "name", "data", "size", nullptr };
	const char* name = nullptr;
	Py_buffer data;
	float size = 0.0f;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sy*f:add_font_ttf", const_cast<char**>(keywords),
		&name, &data, &size)) return nullptr;
	if (size <= 0.0f)
	{
		PyBuffer_Release(&data);
		PyErr_SetString(PyExc_ValueError, "font size must be positive");
		return nullptr;
	}
	if (!PythonRuntime::CheckApiThread())
	{
		PyBuffer_Release(&data);
		return nullptr;
	}
	const bool added = BdImGui::AddFontTTF(name, data.buf, (size_t)data.len, size);
	PyBuffer_Release(&data);
	return ReturnBool(added);
}

PyObject* ImAddFontDefault(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "name", "size", "bitmap", nullptr };
	const char* name = nullptr;
	float size = 13.0f;
	int bitmap = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|fp:add_font_default", const_cast<char**>(keywords),
		&name, &size, &bitmap)) return nullptr;
	if (size <= 0.0f)
	{
		PyErr_SetString(PyExc_ValueError, "font size must be positive");
		return nullptr;
	}
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	return ReturnBool(BdImGui::AddFontDefault(name, size, bitmap != 0));
}

PyObject* ImRemoveFont(PyObject*, PyObject* args)
{
	const char* name = nullptr;
	if (!PyArg_ParseTuple(args, "s:remove_font", &name)) return nullptr;
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	return ReturnBool(BdImGui::RemoveFont(name));
}

PyObject* ImClearFonts(PyObject*, PyObject*)
{
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	BdImGui::ClearFonts();
	Py_RETURN_NONE;
}

PyObject* ImListFonts(PyObject*, PyObject*)
{
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	TArray<BdImGui::FontInfo> fonts;
	BdImGui::ListFonts(fonts);
	PyObject* list = PyList_New((Py_ssize_t)fonts.Size());
	if (list == nullptr) return nullptr;
	for (unsigned int i = 0; i < fonts.Size(); i++)
	{
		PyObject* item = Py_BuildValue("(sfNN)", fonts[i].Name.GetChars(), (double)fonts[i].SizePixels,
			PyBool_FromLong(fonts[i].Bitmap ? 1 : 0), PyBool_FromLong(fonts[i].BuiltIn ? 1 : 0));
		if (item == nullptr)
		{
			Py_DECREF(list);
			return nullptr;
		}
		PyList_SET_ITEM(list, (Py_ssize_t)i, item);
	}
	return list;
}

PyObject* ImSetDefaultFont(PyObject*, PyObject* args)
{
	const char* name = nullptr;
	if (!PyArg_ParseTuple(args, "s:set_default_font", &name)) return nullptr;
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	return ReturnBool(BdImGui::SetDefaultFont(name));
}

PyObject* ImGetDefaultFont(PyObject*, PyObject*)
{
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	const FString name = BdImGui::GetDefaultFontName();
	if (name.IsEmpty()) Py_RETURN_NONE;
	return PyUnicode_FromString(name.GetChars());
}

PyObject* ImPushFont(PyObject*, PyObject* args)
{
	const char* name = nullptr;
	if (!PyArg_ParseTuple(args, "s:push_font", &name)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImFont* font = BdImGui::FindFont(name);
	if (font == nullptr)
	{
		PyErr_Format(PyExc_ValueError, "unknown font '%s' (or the font atlas has not been rebuilt yet)", name);
		return nullptr;
	}
	ImGui::PushFont(font, 0.0f);
	Py_RETURN_NONE;
}

PyObject* ImPopFont(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::PopFont();
	Py_RETURN_NONE;
}

//---------------------------------------------------------------------------
// Scaling and style
//---------------------------------------------------------------------------

PyObject* ImSetUiScale(PyObject*, PyObject* args)
{
	float factor = 1.0f;
	if (!PyArg_ParseTuple(args, "f:set_ui_scale", &factor)) return nullptr;
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	return ReturnBool(BdImGui::SetUiScale(factor));
}

PyObject* ImGetUiScale(PyObject*, PyObject*)
{
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	return PyFloat_FromDouble(BdImGui::GetUiScale());
}

PyObject* ImSetWindowFontScale(PyObject*, PyObject* args)
{
	float scale = 1.0f;
	if (!PyArg_ParseTuple(args, "f:set_window_font_scale", &scale)) return nullptr;
	// A NaN/Inf scale would corrupt every subsequent text draw in the window.
	if (!std::isfinite(scale))
	{
		PyErr_SetString(PyExc_ValueError, "set_window_font_scale value must be finite");
		return nullptr;
	}
	if (!BeginImGuiCall()) return nullptr;
	ImGui::SetWindowFontScale(scale);
	Py_RETURN_NONE;
}

PyObject* ImGetStyleColor(PyObject*, PyObject* args)
{
	int idx = 0;
	if (!PyArg_ParseTuple(args, "i:get_style_color", &idx)) return nullptr;
	if (idx < 0 || idx >= ImGuiCol_COUNT)
	{
		PyErr_Format(PyExc_ValueError, "style color index %d out of range (0..%d)", idx, (int)ImGuiCol_COUNT - 1);
		return nullptr;
	}
	if (!BeginStyleCall()) return nullptr;
	const ImVec4 col = ImGui::GetStyle().Colors[idx];
	return Py_BuildValue("(ffff)", col.x, col.y, col.z, col.w);
}

PyObject* ImSetStyleColor(PyObject*, PyObject* args)
{
	int idx = 0;
	float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
	if (!PyArg_ParseTuple(args, "iffff:set_style_color", &idx, &r, &g, &b, &a)) return nullptr;
	if (idx < 0 || idx >= ImGuiCol_COUNT)
	{
		PyErr_Format(PyExc_ValueError, "style color index %d out of range (0..%d)", idx, (int)ImGuiCol_COUNT - 1);
		return nullptr;
	}
	if (!BeginStyleCall()) return nullptr;
	ImGui::GetStyle().Colors[idx] = ImVec4(r, g, b, a);
	Py_RETURN_NONE;
}

// True for ImGuiStyleVar entries backed by an ImVec2; mirrors the member
// layout of ImGuiStyle (see imgui.h ImGuiStyleVar_).
bool IsVec2StyleVar(int idx)
{
	switch (idx)
	{
	case ImGuiStyleVar_WindowPadding:
	case ImGuiStyleVar_WindowMinSize:
	case ImGuiStyleVar_WindowTitleAlign:
	case ImGuiStyleVar_FramePadding:
	case ImGuiStyleVar_ItemSpacing:
	case ImGuiStyleVar_ItemInnerSpacing:
	case ImGuiStyleVar_CellPadding:
	case ImGuiStyleVar_TableAngledHeadersTextAlign:
	case ImGuiStyleVar_ButtonTextAlign:
	case ImGuiStyleVar_SelectableTextAlign:
	case ImGuiStyleVar_SeparatorTextAlign:
	case ImGuiStyleVar_SeparatorTextPadding:
		return true;
	default:
		return false;
	}
}

PyObject* ImGetStyleVar(PyObject*, PyObject* args)
{
	int idx = 0;
	if (!PyArg_ParseTuple(args, "i:get_style_var", &idx)) return nullptr;
	if (idx < 0 || idx >= ImGuiStyleVar_COUNT)
	{
		PyErr_Format(PyExc_ValueError, "style var index %d out of range (0..%d)", idx, (int)ImGuiStyleVar_COUNT - 1);
		return nullptr;
	}
	if (!BeginStyleCall()) return nullptr;
	const ImGuiStyle& style = ImGui::GetStyle();
	switch (idx)
	{
	case ImGuiStyleVar_WindowPadding:		return Py_BuildValue("(ff)", style.WindowPadding.x, style.WindowPadding.y);
	case ImGuiStyleVar_WindowMinSize:		return Py_BuildValue("(ff)", style.WindowMinSize.x, style.WindowMinSize.y);
	case ImGuiStyleVar_WindowTitleAlign:	return Py_BuildValue("(ff)", style.WindowTitleAlign.x, style.WindowTitleAlign.y);
	case ImGuiStyleVar_FramePadding:		return Py_BuildValue("(ff)", style.FramePadding.x, style.FramePadding.y);
	case ImGuiStyleVar_ItemSpacing:			return Py_BuildValue("(ff)", style.ItemSpacing.x, style.ItemSpacing.y);
	case ImGuiStyleVar_ItemInnerSpacing:	return Py_BuildValue("(ff)", style.ItemInnerSpacing.x, style.ItemInnerSpacing.y);
	case ImGuiStyleVar_CellPadding:			return Py_BuildValue("(ff)", style.CellPadding.x, style.CellPadding.y);
	case ImGuiStyleVar_TableAngledHeadersTextAlign: return Py_BuildValue("(ff)", style.TableAngledHeadersTextAlign.x, style.TableAngledHeadersTextAlign.y);
	case ImGuiStyleVar_ButtonTextAlign:		return Py_BuildValue("(ff)", style.ButtonTextAlign.x, style.ButtonTextAlign.y);
	case ImGuiStyleVar_SelectableTextAlign:	return Py_BuildValue("(ff)", style.SelectableTextAlign.x, style.SelectableTextAlign.y);
	case ImGuiStyleVar_SeparatorTextAlign:	return Py_BuildValue("(ff)", style.SeparatorTextAlign.x, style.SeparatorTextAlign.y);
	case ImGuiStyleVar_SeparatorTextPadding: return Py_BuildValue("(ff)", style.SeparatorTextPadding.x, style.SeparatorTextPadding.y);
	case ImGuiStyleVar_Alpha:				return PyFloat_FromDouble(style.Alpha);
	case ImGuiStyleVar_DisabledAlpha:		return PyFloat_FromDouble(style.DisabledAlpha);
	case ImGuiStyleVar_WindowRounding:		return PyFloat_FromDouble(style.WindowRounding);
	case ImGuiStyleVar_WindowBorderSize:	return PyFloat_FromDouble(style.WindowBorderSize);
	case ImGuiStyleVar_ChildRounding:		return PyFloat_FromDouble(style.ChildRounding);
	case ImGuiStyleVar_ChildBorderSize:		return PyFloat_FromDouble(style.ChildBorderSize);
	case ImGuiStyleVar_PopupRounding:		return PyFloat_FromDouble(style.PopupRounding);
	case ImGuiStyleVar_PopupBorderSize:		return PyFloat_FromDouble(style.PopupBorderSize);
	case ImGuiStyleVar_FrameRounding:		return PyFloat_FromDouble(style.FrameRounding);
	case ImGuiStyleVar_FrameBorderSize:		return PyFloat_FromDouble(style.FrameBorderSize);
	case ImGuiStyleVar_IndentSpacing:		return PyFloat_FromDouble(style.IndentSpacing);
	case ImGuiStyleVar_ScrollbarSize:		return PyFloat_FromDouble(style.ScrollbarSize);
	case ImGuiStyleVar_ScrollbarRounding:	return PyFloat_FromDouble(style.ScrollbarRounding);
	case ImGuiStyleVar_ScrollbarPadding:	return PyFloat_FromDouble(style.ScrollbarPadding);
	case ImGuiStyleVar_GrabMinSize:			return PyFloat_FromDouble(style.GrabMinSize);
	case ImGuiStyleVar_GrabRounding:		return PyFloat_FromDouble(style.GrabRounding);
	case ImGuiStyleVar_ImageRounding:		return PyFloat_FromDouble(style.ImageRounding);
	case ImGuiStyleVar_ImageBorderSize:		return PyFloat_FromDouble(style.ImageBorderSize);
	case ImGuiStyleVar_TabRounding:			return PyFloat_FromDouble(style.TabRounding);
	case ImGuiStyleVar_TabBorderSize:		return PyFloat_FromDouble(style.TabBorderSize);
	case ImGuiStyleVar_TabMinWidthBase:		return PyFloat_FromDouble(style.TabMinWidthBase);
	case ImGuiStyleVar_TabMinWidthShrink:	return PyFloat_FromDouble(style.TabMinWidthShrink);
	case ImGuiStyleVar_TabBarBorderSize:	return PyFloat_FromDouble(style.TabBarBorderSize);
	case ImGuiStyleVar_TabBarOverlineSize:	return PyFloat_FromDouble(style.TabBarOverlineSize);
	case ImGuiStyleVar_TableAngledHeadersAngle: return PyFloat_FromDouble(style.TableAngledHeadersAngle);
	case ImGuiStyleVar_TreeLinesSize:		return PyFloat_FromDouble(style.TreeLinesSize);
	case ImGuiStyleVar_TreeLinesRounding:	return PyFloat_FromDouble(style.TreeLinesRounding);
	case ImGuiStyleVar_DragDropTargetRounding: return PyFloat_FromDouble(style.DragDropTargetRounding);
	case ImGuiStyleVar_SeparatorSize:		return PyFloat_FromDouble(style.SeparatorSize);
	case ImGuiStyleVar_SeparatorTextBorderSize: return PyFloat_FromDouble(style.SeparatorTextBorderSize);
	case ImGuiStyleVar_DockingSeparatorSize: return PyFloat_FromDouble(style.DockingSeparatorSize);
	default:
		PyErr_Format(PyExc_ValueError, "style var index %d has no accessor", idx);
		return nullptr;
	}
}

PyObject* ImSetStyleVar(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "idx", "x", "y", nullptr };
	int idx = 0;
	float x = 0.0f;
	PyObject* yObject = Py_None;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "if|O:set_style_var", const_cast<char**>(keywords),
		&idx, &x, &yObject)) return nullptr;
	if (idx < 0 || idx >= ImGuiStyleVar_COUNT)
	{
		PyErr_Format(PyExc_ValueError, "style var index %d out of range (0..%d)", idx, (int)ImGuiStyleVar_COUNT - 1);
		return nullptr;
	}
	if (!BeginStyleCall()) return nullptr;
	ImGuiStyle& style = ImGui::GetStyle();
	if (IsVec2StyleVar(idx))
	{
		if (yObject == Py_None)
		{
			PyErr_SetString(PyExc_TypeError, "this style var is an (x, y) pair; pass y");
			return nullptr;
		}
		const double y = PyFloat_AsDouble(yObject);
		if (y == -1.0 && PyErr_Occurred()) return nullptr;
		const ImVec2 value(x, (float)y);
		switch (idx)
		{
		case ImGuiStyleVar_WindowPadding:		style.WindowPadding = value; break;
		case ImGuiStyleVar_WindowMinSize:		style.WindowMinSize = value; break;
		case ImGuiStyleVar_WindowTitleAlign:	style.WindowTitleAlign = value; break;
		case ImGuiStyleVar_FramePadding:		style.FramePadding = value; break;
		case ImGuiStyleVar_ItemSpacing:			style.ItemSpacing = value; break;
		case ImGuiStyleVar_ItemInnerSpacing:	style.ItemInnerSpacing = value; break;
		case ImGuiStyleVar_CellPadding:			style.CellPadding = value; break;
		case ImGuiStyleVar_TableAngledHeadersTextAlign: style.TableAngledHeadersTextAlign = value; break;
		case ImGuiStyleVar_ButtonTextAlign:		style.ButtonTextAlign = value; break;
		case ImGuiStyleVar_SelectableTextAlign:	style.SelectableTextAlign = value; break;
		case ImGuiStyleVar_SeparatorTextAlign:	style.SeparatorTextAlign = value; break;
		case ImGuiStyleVar_SeparatorTextPadding: style.SeparatorTextPadding = value; break;
		default:
			// Mirror the getter: an in-range but unenumerated index must not be
			// silently ignored.
			PyErr_Format(PyExc_ValueError, "style var index %d has no accessor", idx);
			return nullptr;
		}
		Py_RETURN_NONE;
	}
	if (yObject != Py_None)
	{
		PyErr_SetString(PyExc_TypeError, "this style var is a scalar; omit y");
		return nullptr;
	}
	switch (idx)
	{
	case ImGuiStyleVar_Alpha:					style.Alpha = x; break;
	case ImGuiStyleVar_DisabledAlpha:			style.DisabledAlpha = x; break;
	case ImGuiStyleVar_WindowRounding:			style.WindowRounding = x; break;
	case ImGuiStyleVar_WindowBorderSize:		style.WindowBorderSize = x; break;
	case ImGuiStyleVar_ChildRounding:			style.ChildRounding = x; break;
	case ImGuiStyleVar_ChildBorderSize:			style.ChildBorderSize = x; break;
	case ImGuiStyleVar_PopupRounding:			style.PopupRounding = x; break;
	case ImGuiStyleVar_PopupBorderSize:			style.PopupBorderSize = x; break;
	case ImGuiStyleVar_FrameRounding:			style.FrameRounding = x; break;
	case ImGuiStyleVar_FrameBorderSize:			style.FrameBorderSize = x; break;
	case ImGuiStyleVar_IndentSpacing:			style.IndentSpacing = x; break;
	case ImGuiStyleVar_ScrollbarSize:			style.ScrollbarSize = x; break;
	case ImGuiStyleVar_ScrollbarRounding:		style.ScrollbarRounding = x; break;
	case ImGuiStyleVar_ScrollbarPadding:		style.ScrollbarPadding = x; break;
	case ImGuiStyleVar_GrabMinSize:				style.GrabMinSize = x; break;
	case ImGuiStyleVar_GrabRounding:			style.GrabRounding = x; break;
	case ImGuiStyleVar_ImageRounding:			style.ImageRounding = x; break;
	case ImGuiStyleVar_ImageBorderSize:			style.ImageBorderSize = x; break;
	case ImGuiStyleVar_TabRounding:				style.TabRounding = x; break;
	case ImGuiStyleVar_TabBorderSize:			style.TabBorderSize = x; break;
	case ImGuiStyleVar_TabMinWidthBase:			style.TabMinWidthBase = x; break;
	case ImGuiStyleVar_TabMinWidthShrink:		style.TabMinWidthShrink = x; break;
	case ImGuiStyleVar_TabBarBorderSize:		style.TabBarBorderSize = x; break;
	case ImGuiStyleVar_TabBarOverlineSize:		style.TabBarOverlineSize = x; break;
	case ImGuiStyleVar_TableAngledHeadersAngle:	style.TableAngledHeadersAngle = x; break;
	case ImGuiStyleVar_TreeLinesSize:			style.TreeLinesSize = x; break;
	case ImGuiStyleVar_TreeLinesRounding:		style.TreeLinesRounding = x; break;
	case ImGuiStyleVar_DragDropTargetRounding:	style.DragDropTargetRounding = x; break;
	case ImGuiStyleVar_SeparatorSize:			style.SeparatorSize = x; break;
	case ImGuiStyleVar_SeparatorTextBorderSize:	style.SeparatorTextBorderSize = x; break;
	case ImGuiStyleVar_DockingSeparatorSize:	style.DockingSeparatorSize = x; break;
	default:
		// Mirror the getter: an in-range but unenumerated index must not be
		// silently ignored.
		PyErr_Format(PyExc_ValueError, "style var index %d has no accessor", idx);
		return nullptr;
	}
	Py_RETURN_NONE;
}

PyObject* ImStyleTheme(PyObject*, PyObject* args)
{
	const char* name = nullptr;
	if (!PyArg_ParseTuple(args, "s:style_theme", &name)) return nullptr;
	if (!BeginStyleCall()) return nullptr;
	ImGuiStyle& style = ImGui::GetStyle();
	// StyleColors*() reinitializes every color and the alpha but not the
	// font scale factors; preserve them across the call.
	const float fontScaleMain = style.FontScaleMain;
	const float fontScaleDpi = style.FontScaleDpi;
	if (stricmp(name, "dark") == 0) ImGui::StyleColorsDark(&style);
	else if (stricmp(name, "classic") == 0) ImGui::StyleColorsClassic(&style);
	else if (stricmp(name, "light") == 0) ImGui::StyleColorsLight(&style);
	else
	{
		PyErr_Format(PyExc_ValueError, "unknown style theme '%s' (use 'dark', 'classic' or 'light')", name);
		return nullptr;
	}
	style.FontScaleMain = fontScaleMain;
	style.FontScaleDpi = fontScaleDpi;
	Py_RETURN_NONE;
}

//---------------------------------------------------------------------------
// Keyboard, focus, popups and cursor layout
//---------------------------------------------------------------------------

bool ParseKeyArgument(PyObject* object, ImGuiKey& out)
{
	const long key = PyLong_AsLong(object);
	if (key == -1 && PyErr_Occurred()) return false;
	if (key < ImGuiKey_NamedKey_BEGIN || key >= ImGuiKey_NamedKey_END)
	{
		PyErr_Format(PyExc_ValueError, "invalid ImGui key value %ld (use imgui.Key.* or imgui.Mod.* for the mods argument)", key);
		return false;
	}
	out = (ImGuiKey)key;
	return true;
}

PyObject* ImIsKeyDown(PyObject*, PyObject* args)
{
	PyObject* keyObject = nullptr;
	if (!PyArg_ParseTuple(args, "O:is_key_down", &keyObject)) return nullptr;
	ImGuiKey key;
	if (!ParseKeyArgument(keyObject, key)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::IsKeyDown(key));
}

PyObject* ImIsKeyPressed(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "key", "repeat", nullptr };
	PyObject* keyObject = nullptr;
	int repeat = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|p:is_key_pressed", const_cast<char**>(keywords),
		&keyObject, &repeat)) return nullptr;
	ImGuiKey key;
	if (!ParseKeyArgument(keyObject, key)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::IsKeyPressed(key, repeat != 0));
}

PyObject* ImIsKeyChordPressed(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "key", "mods", nullptr };
	PyObject* keyObject = nullptr;
	unsigned int mods = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|I:is_key_chord_pressed", const_cast<char**>(keywords),
		&keyObject, &mods)) return nullptr;
	ImGuiKey key;
	if (!ParseKeyArgument(keyObject, key)) return nullptr;
	if ((mods & ~(unsigned int)ImGuiMod_Mask_) != 0)
	{
		PyErr_SetString(PyExc_ValueError, "mods must be a combination of imgui.Mod.* values");
		return nullptr;
	}
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::IsKeyChordPressed((ImGuiKeyChord)(mods | (unsigned int)key)));
}

PyObject* ImShortcut(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "key", "mods", nullptr };
	PyObject* keyObject = nullptr;
	unsigned int mods = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|I:shortcut", const_cast<char**>(keywords),
		&keyObject, &mods)) return nullptr;
	ImGuiKey key;
	if (!ParseKeyArgument(keyObject, key)) return nullptr;
	if ((mods & ~(unsigned int)ImGuiMod_Mask_) != 0)
	{
		PyErr_SetString(PyExc_ValueError, "mods must be a combination of imgui.Mod.* values");
		return nullptr;
	}
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::Shortcut((ImGuiKeyChord)(mods | (unsigned int)key)));
}

PyObject* ImSetItemDefaultFocus(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::SetItemDefaultFocus();
	Py_RETURN_NONE;
}

PyObject* ImSetKeyboardFocusHere(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "offset", nullptr };
	float offset = 0.0f;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|f:set_keyboard_focus_here", const_cast<char**>(keywords),
		&offset)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::SetKeyboardFocusHere((int)offset);
	Py_RETURN_NONE;
}

PyObject* ImOpenPopup(PyObject*, PyObject* args)
{
	const char* strId = nullptr;
	if (!PyArg_ParseTuple(args, "s:open_popup", &strId)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::OpenPopup(strId);
	Py_RETURN_NONE;
}

PyObject* ImBeginPopup(PyObject*, PyObject* args)
{
	const char* strId = nullptr;
	if (!PyArg_ParseTuple(args, "s:begin_popup", &strId)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::BeginPopup(strId));
}

PyObject* ImEndPopup(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::EndPopup();
	Py_RETURN_NONE;
}

PyObject* ImCloseCurrentPopup(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	ImGui::CloseCurrentPopup();
	Py_RETURN_NONE;
}

PyObject* ImIsPopupOpen(PyObject*, PyObject* args)
{
	const char* strId = nullptr;
	if (!PyArg_ParseTuple(args, "s:is_popup_open", &strId)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::IsPopupOpen(strId));
}

PyObject* ImCalcTextSize(PyObject*, PyObject* args)
{
	const char* text = nullptr;
	if (!PyArg_ParseTuple(args, "s:calc_text_size", &text)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	const ImVec2 size = ImGui::CalcTextSize(text);
	return Py_BuildValue("(ff)", size.x, size.y);
}

PyObject* ImSetCursorPos(PyObject*, PyObject* args)
{
	float x = 0.0f, y = 0.0f;
	if (!PyArg_ParseTuple(args, "ff:set_cursor_pos", &x, &y)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::SetCursorPos(ImVec2(x, y));
	Py_RETURN_NONE;
}

PyObject* ImGetCursorPos(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	const ImVec2 pos = ImGui::GetCursorPos();
	return Py_BuildValue("(ff)", pos.x, pos.y);
}

PyObject* ImGetCursorScreenPos(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	return Py_BuildValue("(ff)", pos.x, pos.y);
}


PyObject* ImIsItemHovered(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::IsItemHovered());
}

PyObject* ImIsItemClicked(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "button", nullptr };
	int button = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i:is_item_clicked", const_cast<char**>(keywords),
		&button)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::IsItemClicked((ImGuiMouseButton)button));
}

PyObject* ImIsItemActive(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::IsItemActive());
}

PyObject* ImIsAnyItemActive(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::IsAnyItemActive());
}

//---------------------------------------------------------------------------
// Style
//---------------------------------------------------------------------------

PyObject* ImPushStyleColor(PyObject*, PyObject* args)
{
	int idx = 0;
	float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
	if (!PyArg_ParseTuple(args, "iffff:push_style_color", &idx, &r, &g, &b, &a)) return nullptr;
	if (idx < 0 || idx >= ImGuiCol_COUNT)
	{
		PyErr_Format(PyExc_ValueError, "style color index %d out of range (0..%d)", idx, (int)ImGuiCol_COUNT - 1);
		return nullptr;
	}
	if (!BeginImGuiCall()) return nullptr;
	ImGui::PushStyleColor((ImGuiCol)idx, ImVec4(r, g, b, a));
	Py_RETURN_NONE;
}

PyObject* ImPopStyleColor(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "count", nullptr };
	int count = 1;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i:pop_style_color", const_cast<char**>(keywords), &count)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::PopStyleColor(count);
	Py_RETURN_NONE;
}

PyObject* ImPushStyleVar(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "idx", "x", "y", nullptr };
	int idx = 0;
	float x = 0.0f;
	PyObject* yObject = Py_None;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "if|O:push_style_var", const_cast<char**>(keywords),
		&idx, &x, &yObject)) return nullptr;
	if (idx < 0 || idx >= ImGuiStyleVar_COUNT)
	{
		PyErr_Format(PyExc_ValueError, "style var index %d out of range (0..%d)", idx, (int)ImGuiStyleVar_COUNT - 1);
		return nullptr;
	}
	if (!BeginImGuiCall()) return nullptr;
	if (yObject == Py_None)
	{
		ImGui::PushStyleVar((ImGuiStyleVar)idx, x);
	}
	else
	{
		const double y = PyFloat_AsDouble(yObject);
		if (y == -1.0 && PyErr_Occurred()) return nullptr;
		ImGui::PushStyleVar((ImGuiStyleVar)idx, ImVec2(x, (float)y));
	}
	Py_RETURN_NONE;
}

PyObject* ImPopStyleVar(PyObject*, PyObject* args, PyObject* kwargs)
{
	static const char* keywords[] = { "count", nullptr };
	int count = 1;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i:pop_style_var", const_cast<char**>(keywords), &count)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	ImGui::PopStyleVar(count);
	Py_RETURN_NONE;
}

PyObject* ImGetFontSize(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return PyFloat_FromDouble(ImGui::GetFontSize());
}

//---------------------------------------------------------------------------
// Debug and capture state
//---------------------------------------------------------------------------

PyObject* ImShowDemoWindow(PyObject*, PyObject* args)
{
	int open = 1;
	if (!PyArg_ParseTuple(args, "p:show_demo_window", &open)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	bool stillOpen = open != 0;
	ImGui::ShowDemoWindow(&stillOpen);
	return ReturnBool(stillOpen);
}

PyObject* ImShowMetricsWindow(PyObject*, PyObject* args)
{
	int open = 1;
	if (!PyArg_ParseTuple(args, "p:show_metrics_window", &open)) return nullptr;
	if (!BeginImGuiCall()) return nullptr;
	bool stillOpen = open != 0;
	ImGui::ShowMetricsWindow(&stillOpen);
	return ReturnBool(stillOpen);
}

PyObject* ImWantCaptureMouse(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::GetIO().WantCaptureMouse);
}

PyObject* ImWantCaptureKeyboard(PyObject*, PyObject*)
{
	if (!BeginImGuiCall()) return nullptr;
	return ReturnBool(ImGui::GetIO().WantCaptureKeyboard);
}

// The master switch is a plain cvar toggle, not an ImGui call, so these two
// intentionally do NOT require an active imgui_frame; scripts use them from
// key handlers to turn the whole overlay on and off.
PyObject* ImSetMasterVisible(PyObject*, PyObject* args)
{
	int visible = 1;
	if (!PyArg_ParseTuple(args, "p:set_master_visible", &visible)) return nullptr;
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	BdImGui::SetMasterVisible(visible != 0);
	Py_RETURN_NONE;
}

PyObject* ImMasterVisible(PyObject*, PyObject*)
{
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	return ReturnBool(BdImGui::MasterVisible());
}

// Keyboard navigation is an ImGui config flag, not frame state, so like
// set_master_visible() these two intentionally do NOT require an active
// imgui_frame; scripts can toggle them from any event.
PyObject* ImSetNavEnabled(PyObject*, PyObject* args)
{
	int enabled = 1;
	if (!PyArg_ParseTuple(args, "p:set_nav_enabled", &enabled)) return nullptr;
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	BdImGui::SetNavEnabled(enabled != 0);
	Py_RETURN_NONE;
}

PyObject* ImNavEnabled(PyObject*, PyObject*)
{
	if (!PythonRuntime::CheckApiThread()) return nullptr;
	return ReturnBool(BdImGui::NavEnabled());
}

//---------------------------------------------------------------------------
// Constant namespaces (imgui.Col / imgui.WindowFlags / imgui.Cond)
//---------------------------------------------------------------------------

struct ConstantEntry
{
	const char* Name;
	long Value;
};

const ConstantEntry ColEntries[] = {
	{ "Text", ImGuiCol_Text },
	{ "TextDisabled", ImGuiCol_TextDisabled },
	{ "WindowBg", ImGuiCol_WindowBg },
	{ "ChildBg", ImGuiCol_ChildBg },
	{ "PopupBg", ImGuiCol_PopupBg },
	{ "Border", ImGuiCol_Border },
	{ "BorderShadow", ImGuiCol_BorderShadow },
	{ "FrameBg", ImGuiCol_FrameBg },
	{ "FrameBgHovered", ImGuiCol_FrameBgHovered },
	{ "FrameBgActive", ImGuiCol_FrameBgActive },
	{ "TitleBg", ImGuiCol_TitleBg },
	{ "TitleBgActive", ImGuiCol_TitleBgActive },
	{ "TitleBgCollapsed", ImGuiCol_TitleBgCollapsed },
	{ "MenuBarBg", ImGuiCol_MenuBarBg },
	{ "ScrollbarBg", ImGuiCol_ScrollbarBg },
	{ "ScrollbarGrab", ImGuiCol_ScrollbarGrab },
	{ "ScrollbarGrabHovered", ImGuiCol_ScrollbarGrabHovered },
	{ "ScrollbarGrabActive", ImGuiCol_ScrollbarGrabActive },
	{ "CheckMark", ImGuiCol_CheckMark },
	{ "CheckboxSelectedBg", ImGuiCol_CheckboxSelectedBg },
	{ "SliderGrab", ImGuiCol_SliderGrab },
	{ "SliderGrabActive", ImGuiCol_SliderGrabActive },
	{ "Button", ImGuiCol_Button },
	{ "ButtonHovered", ImGuiCol_ButtonHovered },
	{ "ButtonActive", ImGuiCol_ButtonActive },
	{ "Header", ImGuiCol_Header },
	{ "HeaderHovered", ImGuiCol_HeaderHovered },
	{ "HeaderActive", ImGuiCol_HeaderActive },
	{ "Separator", ImGuiCol_Separator },
	{ "SeparatorHovered", ImGuiCol_SeparatorHovered },
	{ "SeparatorActive", ImGuiCol_SeparatorActive },
	{ "ResizeGrip", ImGuiCol_ResizeGrip },
	{ "ResizeGripHovered", ImGuiCol_ResizeGripHovered },
	{ "ResizeGripActive", ImGuiCol_ResizeGripActive },
	{ "InputTextCursor", ImGuiCol_InputTextCursor },
	{ "TabHovered", ImGuiCol_TabHovered },
	{ "Tab", ImGuiCol_Tab },
	{ "TabSelected", ImGuiCol_TabSelected },
	{ "TabSelectedOverline", ImGuiCol_TabSelectedOverline },
	{ "TabDimmed", ImGuiCol_TabDimmed },
	{ "TabDimmedSelected", ImGuiCol_TabDimmedSelected },
	{ "TabDimmedSelectedOverline", ImGuiCol_TabDimmedSelectedOverline },
	{ "DockingPreview", ImGuiCol_DockingPreview },
	{ "DockingEmptyBg", ImGuiCol_DockingEmptyBg },
	{ "PlotLines", ImGuiCol_PlotLines },
	{ "PlotLinesHovered", ImGuiCol_PlotLinesHovered },
	{ "PlotHistogram", ImGuiCol_PlotHistogram },
	{ "PlotHistogramHovered", ImGuiCol_PlotHistogramHovered },
	{ "TableHeaderBg", ImGuiCol_TableHeaderBg },
	{ "TableBorderStrong", ImGuiCol_TableBorderStrong },
	{ "TableBorderLight", ImGuiCol_TableBorderLight },
	{ "TableRowBg", ImGuiCol_TableRowBg },
	{ "TableRowBgAlt", ImGuiCol_TableRowBgAlt },
	{ "TextLink", ImGuiCol_TextLink },
	{ "TextSelectedBg", ImGuiCol_TextSelectedBg },
	{ "TreeLines", ImGuiCol_TreeLines },
	{ "DragDropTarget", ImGuiCol_DragDropTarget },
	{ "DragDropTargetBg", ImGuiCol_DragDropTargetBg },
	{ "UnsavedMarker", ImGuiCol_UnsavedMarker },
	{ "NavCursor", ImGuiCol_NavCursor },
	{ "NavWindowingHighlight", ImGuiCol_NavWindowingHighlight },
	{ "NavWindowingDimBg", ImGuiCol_NavWindowingDimBg },
	{ "ModalWindowDimBg", ImGuiCol_ModalWindowDimBg },
};

const ConstantEntry StyleVarEntries[] = {
	{ "Alpha", ImGuiStyleVar_Alpha },
	{ "DisabledAlpha", ImGuiStyleVar_DisabledAlpha },
	{ "WindowPadding", ImGuiStyleVar_WindowPadding },
	{ "WindowRounding", ImGuiStyleVar_WindowRounding },
	{ "WindowBorderSize", ImGuiStyleVar_WindowBorderSize },
	{ "WindowMinSize", ImGuiStyleVar_WindowMinSize },
	{ "WindowTitleAlign", ImGuiStyleVar_WindowTitleAlign },
	{ "ChildRounding", ImGuiStyleVar_ChildRounding },
	{ "ChildBorderSize", ImGuiStyleVar_ChildBorderSize },
	{ "PopupRounding", ImGuiStyleVar_PopupRounding },
	{ "PopupBorderSize", ImGuiStyleVar_PopupBorderSize },
	{ "FramePadding", ImGuiStyleVar_FramePadding },
	{ "FrameRounding", ImGuiStyleVar_FrameRounding },
	{ "FrameBorderSize", ImGuiStyleVar_FrameBorderSize },
	{ "ItemSpacing", ImGuiStyleVar_ItemSpacing },
	{ "ItemInnerSpacing", ImGuiStyleVar_ItemInnerSpacing },
	{ "IndentSpacing", ImGuiStyleVar_IndentSpacing },
	{ "CellPadding", ImGuiStyleVar_CellPadding },
	{ "ScrollbarSize", ImGuiStyleVar_ScrollbarSize },
	{ "ScrollbarRounding", ImGuiStyleVar_ScrollbarRounding },
	{ "ScrollbarPadding", ImGuiStyleVar_ScrollbarPadding },
	{ "GrabMinSize", ImGuiStyleVar_GrabMinSize },
	{ "GrabRounding", ImGuiStyleVar_GrabRounding },
	{ "ImageRounding", ImGuiStyleVar_ImageRounding },
	{ "ImageBorderSize", ImGuiStyleVar_ImageBorderSize },
	{ "TabRounding", ImGuiStyleVar_TabRounding },
	{ "TabBorderSize", ImGuiStyleVar_TabBorderSize },
	{ "TabMinWidthBase", ImGuiStyleVar_TabMinWidthBase },
	{ "TabMinWidthShrink", ImGuiStyleVar_TabMinWidthShrink },
	{ "TabBarBorderSize", ImGuiStyleVar_TabBarBorderSize },
	{ "TabBarOverlineSize", ImGuiStyleVar_TabBarOverlineSize },
	{ "TableAngledHeadersAngle", ImGuiStyleVar_TableAngledHeadersAngle },
	{ "TableAngledHeadersTextAlign", ImGuiStyleVar_TableAngledHeadersTextAlign },
	{ "TreeLinesSize", ImGuiStyleVar_TreeLinesSize },
	{ "TreeLinesRounding", ImGuiStyleVar_TreeLinesRounding },
	{ "DragDropTargetRounding", ImGuiStyleVar_DragDropTargetRounding },
	{ "ButtonTextAlign", ImGuiStyleVar_ButtonTextAlign },
	{ "SelectableTextAlign", ImGuiStyleVar_SelectableTextAlign },
	{ "SeparatorSize", ImGuiStyleVar_SeparatorSize },
	{ "SeparatorTextBorderSize", ImGuiStyleVar_SeparatorTextBorderSize },
	{ "SeparatorTextAlign", ImGuiStyleVar_SeparatorTextAlign },
	{ "SeparatorTextPadding", ImGuiStyleVar_SeparatorTextPadding },
	{ "DockingSeparatorSize", ImGuiStyleVar_DockingSeparatorSize },
};

const ConstantEntry KeyEntries[] = {
	{ "Tab", ImGuiKey_Tab },
	{ "Left", ImGuiKey_LeftArrow },
	{ "Right", ImGuiKey_RightArrow },
	{ "Up", ImGuiKey_UpArrow },
	{ "Down", ImGuiKey_DownArrow },
	{ "PageUp", ImGuiKey_PageUp },
	{ "PageDown", ImGuiKey_PageDown },
	{ "Home", ImGuiKey_Home },
	{ "End", ImGuiKey_End },
	{ "Insert", ImGuiKey_Insert },
	{ "Delete", ImGuiKey_Delete },
	{ "Backspace", ImGuiKey_Backspace },
	{ "Space", ImGuiKey_Space },
	{ "Enter", ImGuiKey_Enter },
	{ "KeyPadEnter", ImGuiKey_KeypadEnter },
	{ "Escape", ImGuiKey_Escape },
	{ "0", ImGuiKey_0 }, { "1", ImGuiKey_1 }, { "2", ImGuiKey_2 }, { "3", ImGuiKey_3 },
	{ "4", ImGuiKey_4 }, { "5", ImGuiKey_5 }, { "6", ImGuiKey_6 }, { "7", ImGuiKey_7 },
	{ "8", ImGuiKey_8 }, { "9", ImGuiKey_9 },
	{ "A", ImGuiKey_A }, { "B", ImGuiKey_B }, { "C", ImGuiKey_C }, { "D", ImGuiKey_D },
	{ "E", ImGuiKey_E }, { "F", ImGuiKey_F }, { "G", ImGuiKey_G }, { "H", ImGuiKey_H },
	{ "I", ImGuiKey_I }, { "J", ImGuiKey_J }, { "K", ImGuiKey_K }, { "L", ImGuiKey_L },
	{ "M", ImGuiKey_M }, { "N", ImGuiKey_N }, { "O", ImGuiKey_O }, { "P", ImGuiKey_P },
	{ "Q", ImGuiKey_Q }, { "R", ImGuiKey_R }, { "S", ImGuiKey_S }, { "T", ImGuiKey_T },
	{ "U", ImGuiKey_U }, { "V", ImGuiKey_V }, { "W", ImGuiKey_W }, { "X", ImGuiKey_X },
	{ "Y", ImGuiKey_Y }, { "Z", ImGuiKey_Z },
	{ "F1", ImGuiKey_F1 }, { "F2", ImGuiKey_F2 }, { "F3", ImGuiKey_F3 }, { "F4", ImGuiKey_F4 },
	{ "F5", ImGuiKey_F5 }, { "F6", ImGuiKey_F6 }, { "F7", ImGuiKey_F7 }, { "F8", ImGuiKey_F8 },
	{ "F9", ImGuiKey_F9 }, { "F10", ImGuiKey_F10 }, { "F11", ImGuiKey_F11 }, { "F12", ImGuiKey_F12 },
};

const ConstantEntry ModEntries[] = {
	{ "Ctrl", ImGuiMod_Ctrl },
	{ "Shift", ImGuiMod_Shift },
	{ "Alt", ImGuiMod_Alt },
	{ "Super", ImGuiMod_Super },
};

const ConstantEntry InputTextFlagsEntries[] = {
	{ "CharsDecimal", ImGuiInputTextFlags_CharsDecimal },
	{ "CharsHexadecimal", ImGuiInputTextFlags_CharsHexadecimal },
	{ "CharsUppercase", ImGuiInputTextFlags_CharsUppercase },
	{ "CharsNoBlank", ImGuiInputTextFlags_CharsNoBlank },
	{ "EnterReturnsTrue", ImGuiInputTextFlags_EnterReturnsTrue },
	{ "ReadOnly", ImGuiInputTextFlags_ReadOnly },
	{ "Password", ImGuiInputTextFlags_Password },
	{ "AutoSelectAll", ImGuiInputTextFlags_AutoSelectAll },
};

const ConstantEntry WindowFlagsEntries[] = {
	{ "NoTitleBar", ImGuiWindowFlags_NoTitleBar },
	{ "NoResize", ImGuiWindowFlags_NoResize },
	{ "NoMove", ImGuiWindowFlags_NoMove },
	{ "NoCollapse", ImGuiWindowFlags_NoCollapse },
	{ "NoBackground", ImGuiWindowFlags_NoBackground },
	{ "NoScrollbar", ImGuiWindowFlags_NoScrollbar },
	{ "MenuBar", ImGuiWindowFlags_MenuBar },
	{ "AlwaysAutoResize", ImGuiWindowFlags_AlwaysAutoResize },
};

const ConstantEntry CondEntries[] = {
	{ "Always", ImGuiCond_Always },
	{ "Once", ImGuiCond_Once },
	{ "FirstUseEver", ImGuiCond_FirstUseEver },
	{ "Appearing", ImGuiCond_Appearing },
};

// A heap type used as a plain attribute namespace: imgui.Col.Text etc.
PyObject* MakeConstantNamespace(const char* name, const char* doc, const ConstantEntry* entries, size_t count)
{
	PyType_Slot slots[] = {
		{ 0, nullptr },
	};
	PyType_Spec spec = { name, 0, 0, Py_TPFLAGS_DEFAULT, slots };
	PyObject* type = reinterpret_cast<PyObject*>(PyType_FromSpec(&spec));
	if (type == nullptr) return nullptr;
	if (PyObject_SetAttrString(type, "__doc__", PyUnicode_FromString(doc)) < 0)
	{
		Py_DECREF(type);
		return nullptr;
	}
	for (size_t i = 0; i < count; ++i)
	{
		if (PyObject_SetAttrString(type, entries[i].Name, PyLong_FromLong(entries[i].Value)) < 0)
		{
			Py_DECREF(type);
			return nullptr;
		}
	}
	return type;
}

//---------------------------------------------------------------------------
// Module definition
//---------------------------------------------------------------------------

PyMethodDef ImGuiMethods[] = {
	{ "begin", BD_IMGUI_KEYWORD_FUNCTION(ImBegin), METH_VARARGS | METH_KEYWORDS,
		"begin(name, open=None, flags=0) -> bool | (bool, bool)\n"
		"Push a window onto the stack; every call must be paired with end(). "
		"With open=None (the default) returns a single bool: False when the window "
		"is collapsed/clipped (still call end()). With open set to a bool the window "
		"gets a close button and the return is a (expanded, open) tuple; assign the "
		"second element back to your visibility state." },
	{ "end", ImEnd, METH_NOARGS,
		"end() -> None\nPop the current window. Always call it, even when begin() returned False." },
	{ "begin_child", BD_IMGUI_KEYWORD_FUNCTION(ImBeginChild), METH_VARARGS | METH_KEYWORDS,
		"begin_child(id, size=(0, 0), border=False, flags=0) -> bool\n"
		"Begin a scrolling child region; returns False when clipped (still call end_child())." },
	{ "end_child", ImEndChild, METH_NOARGS,
		"end_child() -> None\nEnd the current child region." },
	{ "set_next_window_pos", BD_IMGUI_KEYWORD_FUNCTION(ImSetNextWindowPos), METH_VARARGS | METH_KEYWORDS,
		"set_next_window_pos(x, y, cond=0) -> None\nSet the position of the next begin() window. cond is an imgui.Cond value (0 = always)." },
	{ "set_next_window_size", BD_IMGUI_KEYWORD_FUNCTION(ImSetNextWindowSize), METH_VARARGS | METH_KEYWORDS,
		"set_next_window_size(w, h, cond=0) -> None\nSet the size of the next begin() window; use 0 on an axis for auto-fit." },
	{ "set_next_window_collapsed", BD_IMGUI_KEYWORD_FUNCTION(ImSetNextWindowCollapsed), METH_VARARGS | METH_KEYWORDS,
		"set_next_window_collapsed(collapsed, cond=0) -> None\nForce the collapsed state of the next begin() window." },
	{ "set_next_window_bg_alpha", ImSetNextWindowBgAlpha, METH_VARARGS,
		"set_next_window_bg_alpha(a) -> None\nOverride the background alpha of the next begin() window (0.0 - 1.0)." },
	{ "is_window_focused", ImIsWindowFocused, METH_NOARGS,
		"is_window_focused() -> bool\nTrue when the current window is focused." },
	{ "is_window_hovered", ImIsWindowHovered, METH_NOARGS,
		"is_window_hovered() -> bool\nTrue when the current window is hovered. For input dispatch decisions use want_capture_mouse() instead." },
	{ "get_window_pos", ImGetWindowPos, METH_NOARGS,
		"get_window_pos() -> (float, float)\nCurrent window position in screen pixels." },
	{ "get_window_size", ImGetWindowSize, METH_NOARGS,
		"get_window_size() -> (float, float)\nCurrent window size in screen pixels." },

	{ "text", ImText, METH_VARARGS,
		"text(s) -> None\nUnformatted text (no printf interpretation)." },
	{ "text_colored", ImTextColored, METH_VARARGS,
		"text_colored(r, g, b, a, s) -> None\nText with an explicit RGBA color (components 0.0 - 1.0)." },
	{ "text_disabled", ImTextDisabled, METH_VARARGS,
		"text_disabled(s) -> None\nText drawn in the disabled color." },
	{ "text_wrapped", ImTextWrapped, METH_VARARGS,
		"text_wrapped(s) -> None\nText wrapped at the window width." },
	{ "label_text", ImLabelText, METH_VARARGS,
		"label_text(label, s) -> None\nText with a right-aligned label, like the value+label widgets." },
	{ "bullet_text", ImBulletText, METH_VARARGS,
		"bullet_text(s) -> None\nText prefixed with a bullet." },

	{ "button", BD_IMGUI_KEYWORD_FUNCTION(ImButton), METH_VARARGS | METH_KEYWORDS,
		"button(label, w=0, h=0) -> bool\nStandard button; True on the frame it is clicked." },
	{ "small_button", ImSmallButton, METH_VARARGS,
		"small_button(label) -> bool\nCompact button for embedding in text lines." },
	{ "checkbox", ImCheckbox, METH_VARARGS,
		"checkbox(label, checked) -> (bool, bool)\nReturns (changed, new_value)." },
	{ "radio_button", ImRadioButton, METH_VARARGS,
		"radio_button(label, active) -> bool\nTrue when pressed; pass value == button_value as active and assign on True." },
	{ "slider_int", ImSliderInt, METH_VARARGS,
		"slider_int(label, value, min, max) -> (bool, int)\nReturns (changed, value)." },
	{ "slider_float", BD_IMGUI_KEYWORD_FUNCTION(ImSliderFloat), METH_VARARGS | METH_KEYWORDS,
		"slider_float(label, value, min, max, format='%.3f') -> (bool, float)\nReturns (changed, value)." },
	{ "drag_int", BD_IMGUI_KEYWORD_FUNCTION(ImDragInt), METH_VARARGS | METH_KEYWORDS,
		"drag_int(label, value, speed=1.0, min=0, max=0) -> (bool, int)\nDrag widget; min >= max means unbounded. Returns (changed, value)." },
	{ "drag_float", BD_IMGUI_KEYWORD_FUNCTION(ImDragFloat), METH_VARARGS | METH_KEYWORDS,
		"drag_float(label, value, speed=1.0, min=0.0, max=0.0, format='%.3f') -> (bool, float)\nmin >= max means unbounded. Returns (changed, value)." },
	{ "input_text", BD_IMGUI_KEYWORD_FUNCTION(ImInputText), METH_VARARGS | METH_KEYWORDS,
		"input_text(label, text, max_length=256, flags=0) -> (bool, str)\nSingle-line edit box backed by a fixed buffer of max_length + 1 bytes. "
		"The returned text is the current buffer contents every frame; what 'changed' "
		"means depends on flags (with no flags it is True on every edit; pass "
		"ImGuiInputTextFlags_EnterReturnsTrue (value 32) to report only on Enter)." },
	{ "input_int", BD_IMGUI_KEYWORD_FUNCTION(ImInputInt), METH_VARARGS | METH_KEYWORDS,
		"input_int(label, value, step=1, step_fast=100) -> (bool, int)\nInteger input with +/- steppers. Returns (changed, value)." },
	{ "input_float", BD_IMGUI_KEYWORD_FUNCTION(ImInputFloat), METH_VARARGS | METH_KEYWORDS,
		"input_float(label, value, step=0.0, step_fast=0.0, format='%.3f') -> (bool, float)\nFloat input; step 0 hides the steppers. Returns (changed, value)." },
	{ "combo", ImCombo, METH_VARARGS,
		"combo(label, current_index, items) -> (bool, int)\nDrop-down over a sequence of str. Returns (changed, new_index)." },
	{ "list_box", BD_IMGUI_KEYWORD_FUNCTION(ImListBox), METH_VARARGS | METH_KEYWORDS,
		"list_box(label, current_index, items, height_items=-1) -> (bool, int)\nFramed scrolling list over a sequence of str. Returns (changed, new_index)." },
	{ "selectable", BD_IMGUI_KEYWORD_FUNCTION(ImSelectable), METH_VARARGS | METH_KEYWORDS,
		"selectable(label, selected, flags=0) -> bool\nTrue when pressed; toggle your own selection state on True." },
	{ "tree_node", ImTreeNode, METH_VARARGS,
		"tree_node(label) -> bool\nTrue when open; emit children and then call tree_pop() only in that case." },
	{ "tree_pop", ImTreePop, METH_NOARGS,
		"tree_pop() -> None\nClose a tree_node() that returned True." },
	{ "collapsing_header", BD_IMGUI_KEYWORD_FUNCTION(ImCollapsingHeader), METH_VARARGS | METH_KEYWORDS,
		"collapsing_header(label, flags=0) -> bool\nTrue when the header is open; no tree_pop() needed." },

	{ "separator", ImSeparator, METH_NOARGS,
		"separator() -> None\nHorizontal separator line (vertical inside menu bars)." },
	{ "same_line", BD_IMGUI_KEYWORD_FUNCTION(ImSameLine), METH_VARARGS | METH_KEYWORDS,
		"same_line(offset=0.0, spacing=-1.0) -> None\nKeep the next widget on the current line." },
	{ "spacing", ImSpacing, METH_NOARGS,
		"spacing() -> None\nVertical spacing." },
	{ "newline", ImNewline, METH_NOARGS,
		"newline() -> None\nUndo a same_line() / force a line break." },
	{ "indent", BD_IMGUI_KEYWORD_FUNCTION(ImIndent), METH_VARARGS | METH_KEYWORDS,
		"indent(width=0.0) -> None\nMove content right (0 = style default spacing)." },
	{ "unindent", BD_IMGUI_KEYWORD_FUNCTION(ImUnindent), METH_VARARGS | METH_KEYWORDS,
		"unindent(width=0.0) -> None\nMove content back left." },
	{ "align_text_to_frame_padding", ImAlignTextToFramePadding, METH_NOARGS,
		"align_text_to_frame_padding() -> None\nAlign text baseline to framed widgets on the same line." },

	{ "begin_table", BD_IMGUI_KEYWORD_FUNCTION(ImBeginTable), METH_VARARGS | METH_KEYWORDS,
		"begin_table(id, columns, flags=0) -> bool\nBegin a table; call end_table() only when it returns True." },
	{ "end_table", ImEndTable, METH_NOARGS,
		"end_table() -> None\nEnd a begin_table() that returned True." },
	{ "table_next_row", BD_IMGUI_KEYWORD_FUNCTION(ImTableNextRow), METH_VARARGS | METH_KEYWORDS,
		"table_next_row(flags=0, min_height=0.0) -> None\nAdvance into the first cell of a new row." },
	{ "table_next_column", ImTableNextColumn, METH_NOARGS,
		"table_next_column() -> bool\nAdvance into the next column; False when the column is clipped." },
	{ "table_setup_column", BD_IMGUI_KEYWORD_FUNCTION(ImTableSetupColumn), METH_VARARGS | METH_KEYWORDS,
		"table_setup_column(label, flags=0, init_width=0.0) -> None\nDeclare one column before the first row." },
	{ "table_headers_row", ImTableHeadersRow, METH_NOARGS,
		"table_headers_row() -> None\nEmit a header row from the declared columns." },

	{ "progress_bar", BD_IMGUI_KEYWORD_FUNCTION(ImProgressBar), METH_VARARGS | METH_KEYWORDS,
		"progress_bar(fraction, w=-1, h=0, overlay=None) -> None\nHorizontal progress bar; fraction is 0.0 - 1.0, w < 0 fills the row." },
	{ "color_edit3", ImColorEdit3, METH_VARARGS,
		"color_edit3(label, r, g, b) -> (bool, float, float, float)\nRGB color editor. Returns (changed, r, g, b)." },
	{ "color_edit4", ImColorEdit4, METH_VARARGS,
		"color_edit4(label, r, g, b, a) -> (bool, float, float, float, float)\nRGBA color editor. Returns (changed, r, g, b, a)." },
	{ "plot_lines", BD_IMGUI_KEYWORD_FUNCTION(ImPlotLines), METH_VARARGS | METH_KEYWORDS,
		"plot_lines(label, values, overlay=None, scale_min=FLT_MAX, scale_max=FLT_MAX, w=0, h=0) -> None\n"
		"Line plot over a sequence of floats. Leave scale_min/scale_max at their defaults "
		"for auto-scaling (the FLT_MAX sentinel is ImGui's 'compute from data' semantic)." },
	{ "image", BD_IMGUI_KEYWORD_FUNCTION(ImImage), METH_VARARGS | METH_KEYWORDS,
		"image(texture, w=0, h=0, uv0=(0, 0), uv1=(1, 1), tint=(1, 1, 1, 1), border=(0, 0, 0, 0)) -> None\n"
		"Draw a game texture inside the current window. texture is either a lump name "
		"(MiscPatch lookup first, sprite-namespace fallback for PLAYA1 style names) or an "
		"Actor handle (its current sprite frame texture, rotation 0). w/h 0 means the "
		"texture's natural display size; uv0/uv1 select a sub-rectangle; tint is an RGBA "
		"multiplier; border with alpha > 0 draws a 1px border of that color. Unknown "
		"texture raises ValueError." },
	{ "image_size", ImImageSize, METH_VARARGS,
		"image_size(texture) -> (float, float)\n"
		"Natural display size (w, h) of a texture accepted by image(). Callable from any event." },
	{ "dock_space_over_viewport", BD_IMGUI_KEYWORD_FUNCTION(ImDockSpaceOverViewport), METH_VARARGS | METH_KEYWORDS,
		"dock_space_over_viewport(flags=0) -> int\n"
		"Create a dockspace covering the whole viewport and return its dockspace id. "
		"flags are ImGuiDockNodeFlags_* (2 = PassthruCentralNode). Requires the docking "
		"branch (always vendored); multi-viewport is not supported." },
	{ "dock_space", BD_IMGUI_KEYWORD_FUNCTION(ImDockSpace), METH_VARARGS | METH_KEYWORDS,
		"dock_space(id, w=0, h=0, flags=0) -> int\n"
		"Submit a dockspace node with the given id inside the current window; returns the "
		"node id. Windows become dockable while any dockspace exists." },
	{ "set_next_window_dock_id", BD_IMGUI_KEYWORD_FUNCTION(ImSetNextWindowDockID), METH_VARARGS | METH_KEYWORDS,
		"set_next_window_dock_id(id, cond=0) -> None\n"
		"Dock the next begin() window into the dockspace node with the given id (a value "
		"returned by dock_space_over_viewport()/dock_space()). cond is an imgui.Cond value." },

	{ "begin_main_menu_bar", ImBeginMainMenuBar, METH_NOARGS,
		"begin_main_menu_bar() -> bool\nBegin the screen-top menu bar; call end_main_menu_bar() only when True." },
	{ "end_main_menu_bar", ImEndMainMenuBar, METH_NOARGS,
		"end_main_menu_bar() -> None\nEnd the main menu bar." },
	{ "begin_menu", ImBeginMenu, METH_VARARGS,
		"begin_menu(label) -> bool\nBegin a sub-menu; call end_menu() only when True." },
	{ "end_menu", ImEndMenu, METH_NOARGS,
		"end_menu() -> None\nEnd a begin_menu() that returned True." },
	{ "menu_item", BD_IMGUI_KEYWORD_FUNCTION(ImMenuItem), METH_VARARGS | METH_KEYWORDS,
		"menu_item(label, shortcut=None, selected=False, enabled=True) -> bool\nTrue when the item is activated." },

	{ "begin_tooltip", ImBeginTooltip, METH_NOARGS,
		"begin_tooltip() -> bool\nBegin a tooltip window; call end_tooltip() only when True." },
	{ "end_tooltip", ImEndTooltip, METH_NOARGS,
		"end_tooltip() -> None\nEnd a begin_tooltip() that returned True." },
	{ "set_tooltip", ImSetTooltip, METH_VARARGS,
		"set_tooltip(s) -> None\nText-only tooltip, typically after is_item_hovered()." },

	{ "set_keyboard_focus_here", BD_IMGUI_KEYWORD_FUNCTION(ImSetKeyboardFocusHere), METH_VARARGS | METH_KEYWORDS,
		"set_keyboard_focus_here(offset=0.0) -> None\nFocus keyboard on the next widget (or on a sub component with a positive offset; -1 addresses the previous widget)." },
	{ "is_item_hovered", ImIsItemHovered, METH_NOARGS,
		"is_item_hovered() -> bool\nTrue when the last submitted item is hovered." },
	{ "is_item_clicked", BD_IMGUI_KEYWORD_FUNCTION(ImIsItemClicked), METH_VARARGS | METH_KEYWORDS,
		"is_item_clicked(button=0) -> bool\nTrue when the last item was clicked with the given mouse button." },
	{ "is_item_active", ImIsItemActive, METH_NOARGS,
		"is_item_active() -> bool\nTrue while the last item is being interacted with." },
	{ "is_any_item_active", ImIsAnyItemActive, METH_NOARGS,
		"is_any_item_active() -> bool\nTrue while any item is active." },

	{ "push_style_color", ImPushStyleColor, METH_VARARGS,
		"push_style_color(idx, r, g, b, a) -> None\nOverride a style color (idx is an imgui.Col value). Pair with pop_style_color()." },
	{ "pop_style_color", BD_IMGUI_KEYWORD_FUNCTION(ImPopStyleColor), METH_VARARGS | METH_KEYWORDS,
		"pop_style_color(count=1) -> None\nUndo style color pushes." },
	{ "push_style_var", BD_IMGUI_KEYWORD_FUNCTION(ImPushStyleVar), METH_VARARGS | METH_KEYWORDS,
		"push_style_var(idx, x, y=None) -> None\nOverride a style variable; y=None pushes a scalar var, a number pushes an ImVec2 var. Pair with pop_style_var()." },
	{ "pop_style_var", BD_IMGUI_KEYWORD_FUNCTION(ImPopStyleVar), METH_VARARGS | METH_KEYWORDS,
		"pop_style_var(count=1) -> None\nUndo style var pushes." },
	{ "get_font_size", ImGetFontSize, METH_NOARGS,
		"get_font_size() -> float\nCurrent font height in pixels after global scaling." },

	{ "add_font_ttf", BD_IMGUI_KEYWORD_FUNCTION(ImAddFontTTF), METH_VARARGS | METH_KEYWORDS,
		"add_font_ttf(name, data, size) -> bool\n"
		"Register a TTF/OTF font from a bytes object (e.g. bd.read_bytes()) under name, requested "
		"pixel size 4..96. The atlas rebuilds outside the frame; a mutation during imgui_frame is "
		"applied after the frame renders, so the font becomes usable on the next frame. Duplicate "
		"or empty names are rejected (returns False). Callable from any event." },
	{ "add_font_default", BD_IMGUI_KEYWORD_FUNCTION(ImAddFontDefault), METH_VARARGS | METH_KEYWORDS,
		"add_font_default(name, size, bitmap=False) -> bool\n"
		"Register a font from ImGui's embedded default data; bitmap=True selects the classic "
		"pixel font, False the scalable vector font. Same rebuild and naming rules as add_font_ttf(). "
		"Callable from any event." },
	{ "remove_font", ImRemoveFont, METH_VARARGS,
		"remove_font(name) -> bool\nRemove a script-registered font and rebuild the atlas. The built-in 'Default' font cannot be removed. Callable from any event." },
	{ "clear_fonts", ImClearFonts, METH_NOARGS,
		"clear_fonts() -> None\nRemove all script-registered fonts, keeping 'Default'. Callable from any event." },
	{ "list_fonts", ImListFonts, METH_NOARGS,
		"list_fonts() -> [(name, size, bitmap, builtin), ...]\nSnapshot of the font registry. Callable from any event." },
	{ "set_default_font", ImSetDefaultFont, METH_VARARGS,
		"set_default_font(name) -> bool\nSelect the font every imgui_frame starts on (the overlay pushes it around the event). Returns False for an unknown name. Callable from any event." },
	{ "get_default_font", ImGetDefaultFont, METH_NOARGS,
		"get_default_font() -> str | None\nName of the font every imgui_frame starts on. Callable from any event." },
	{ "push_font", ImPushFont, METH_VARARGS,
		"push_font(name) -> None\nSwitch to a registered font; pair with pop_font(). Raises ValueError for an unknown name." },
	{ "pop_font", ImPopFont, METH_NOARGS,
		"pop_font() -> None\nUndo push_font()." },

	{ "set_ui_scale", ImSetUiScale, METH_VARARGS,
		"set_ui_scale(factor) -> bool\n"
		"Global UI scale, clamped to [0.5, 4.0]. Sets style.FontScaleMain and rescales all "
		"spacing/padding via ScaleAllSizes() by the ratio between the new and the previous factor, "
		"so repeated calls compose (2.0 then 2.0 yields 4x spacing, not 8x). Callable from any event." },
	{ "get_ui_scale", ImGetUiScale, METH_NOARGS,
		"get_ui_scale() -> float\nCurrent global UI scale factor. Callable from any event." },
	{ "set_window_font_scale", ImSetWindowFontScale, METH_VARARGS,
		"set_window_font_scale(scale) -> None\nPer-window font scale for the current window; prefer set_ui_scale() for global scaling." },
	{ "get_style_color", ImGetStyleColor, METH_VARARGS,
		"get_style_color(idx) -> (r, g, b, a)\nRead a style color (idx is an imgui.Col value). Callable from any event." },
	{ "set_style_color", ImSetStyleColor, METH_VARARGS,
		"set_style_color(idx, r, g, b, a) -> None\nWrite a style color (idx is an imgui.Col value). Callable from any event." },
	{ "get_style_var", ImGetStyleVar, METH_VARARGS,
		"get_style_var(idx) -> float | (x, y)\nRead a style variable (idx is an imgui.StyleVar value); ImVec2-backed vars return a tuple. Callable from any event." },
	{ "set_style_var", BD_IMGUI_KEYWORD_FUNCTION(ImSetStyleVar), METH_VARARGS | METH_KEYWORDS,
		"set_style_var(idx, x, y=None) -> None\nWrite a style variable; ImVec2-backed vars require y. Callable from any event." },
	{ "style_theme", ImStyleTheme, METH_VARARGS,
		"style_theme(name) -> None\nReset all colors to a stock theme: 'dark', 'classic' or 'light'. The UI scale factors are preserved. Callable from any event." },

	{ "is_key_down", ImIsKeyDown, METH_VARARGS,
		"is_key_down(key) -> bool\nTrue while the key is held (key is an imgui.Key value)." },
	{ "is_key_pressed", BD_IMGUI_KEYWORD_FUNCTION(ImIsKeyPressed), METH_VARARGS | METH_KEYWORDS,
		"is_key_pressed(key, repeat=False) -> bool\nTrue on the frame the key went down; repeat=True also reports held-key repeats." },
	{ "is_key_chord_pressed", BD_IMGUI_KEYWORD_FUNCTION(ImIsKeyChordPressed), METH_VARARGS | METH_KEYWORDS,
		"is_key_chord_pressed(key, mods=0) -> bool\nTrue on the frame the mods+key chord went down (mods is an OR of imgui.Mod values). Does no focus routing; prefer shortcut()." },
	{ "shortcut", BD_IMGUI_KEYWORD_FUNCTION(ImShortcut), METH_VARARGS | METH_KEYWORDS,
		"shortcut(key, mods=0) -> bool\nLike is_key_chord_pressed() but with ImGui focus routing, so the deepest focused window wins." },
	{ "set_item_default_focus", ImSetItemDefaultFocus, METH_NOARGS,
		"set_item_default_focus() -> None\nMake the last submitted item the default focused item of a newly appearing window." },
	{ "open_popup", ImOpenPopup, METH_VARARGS,
		"open_popup(str_id) -> None\nMark a popup as open; call on an event (e.g. a button press), not every frame." },
	{ "begin_popup", ImBeginPopup, METH_VARARGS,
		"begin_popup(str_id) -> bool\nTrue when the popup is open; call end_popup() only in that case." },
	{ "end_popup", ImEndPopup, METH_NOARGS,
		"end_popup() -> None\nEnd a begin_popup() that returned True." },
	{ "close_current_popup", ImCloseCurrentPopup, METH_NOARGS,
		"close_current_popup() -> None\nClose the popup currently open in this scope." },
	{ "is_popup_open", ImIsPopupOpen, METH_VARARGS,
		"is_popup_open(str_id) -> bool\nTrue when the popup with this id is open." },
	{ "calc_text_size", ImCalcTextSize, METH_VARARGS,
		"calc_text_size(s) -> (w, h)\nSize of a text string in the current font, in pixels." },
	{ "set_cursor_pos", ImSetCursorPos, METH_VARARGS,
		"set_cursor_pos(x, y) -> None\nSet the cursor position inside the current window (window-local coordinates)." },
	{ "get_cursor_pos", ImGetCursorPos, METH_NOARGS,
		"get_cursor_pos() -> (x, y)\nCursor position in window-local coordinates." },
	{ "get_cursor_screen_pos", ImGetCursorScreenPos, METH_NOARGS,
		"get_cursor_screen_pos() -> (x, y)\nCursor position in absolute screen coordinates." },

	{ "show_demo_window", ImShowDemoWindow, METH_VARARGS,
		"show_demo_window(open) -> bool\nShow the ImGui demo window while open is True; returns the still-open state (the window's close button flips it)." },
	{ "show_metrics_window", ImShowMetricsWindow, METH_VARARGS,
		"show_metrics_window(open) -> bool\nShow the ImGui metrics/debugger window; returns the still-open state." },
	{ "want_capture_mouse", ImWantCaptureMouse, METH_NOARGS,
		"want_capture_mouse() -> bool\nTrue when ImGui is consuming the mouse this frame." },
	{ "want_capture_keyboard", ImWantCaptureKeyboard, METH_NOARGS,
		"want_capture_keyboard() -> bool\nTrue when ImGui is consuming the keyboard this frame." },
	{ "set_master_visible", ImSetMasterVisible, METH_VARARGS,
		"set_master_visible(visible) -> None\nToggle the whole overlay (the py_imgui CVar). Callable from any event, not just imgui_frame." },
	{ "master_visible", ImMasterVisible, METH_NOARGS,
		"master_visible() -> bool\nRead the py_imgui master switch. Callable from any event." },
	{ "set_nav_enabled", ImSetNavEnabled, METH_VARARGS,
		"set_nav_enabled(enabled) -> None\nEnable or disable ImGui keyboard navigation (ImGuiConfigFlags_NavEnableKeyboard, on by default). Callable from any event, not just imgui_frame." },
	{ "nav_enabled", ImNavEnabled, METH_NOARGS,
		"nav_enabled() -> bool\nTrue when ImGui keyboard navigation is enabled. Callable from any event." },
	{ nullptr, nullptr, 0, nullptr },
};

PyModuleDef ImGuiModuleDef = {
	PyModuleDef_HEAD_INIT,
	"biaseddoom.imgui",
	"Dear ImGui immediate-mode UI overlay.\n\n"
	"All widget calls in this module submit ImGui draw commands and are only "
	"valid inside an imgui_frame event handler (they raise RuntimeError "
	"anywhere else). Not frame-gated: the font registry (add_font_ttf(), "
	"add_font_default(), remove_font(), clear_fonts(), list_fonts(), "
	"set_default_font(), get_default_font()), the global scale "
	"(set_ui_scale()/get_ui_scale()), the persistent style accessors "
	"(get_style_color()/set_style_color(), get_style_var()/set_style_var(), "
	"style_theme()), set_master_visible()/master_visible() and "
	"set_nav_enabled()/nav_enabled(); the style accessors require the "
	"overlay to have rendered at least one frame. Cost model: one CPython "
	"crossing per widget call, and when no script registers imgui_frame the "
	"whole feature costs a single HasCallbacks check per frame. The overlay "
	"is gated by the py_imgui CVar (default on) and the py_imgui_demo "
	"console command opens the stock demo window.",
	-1,
	ImGuiMethods,
	nullptr, nullptr, nullptr, nullptr,
};

} // namespace

bool Initialize(PyObject* module)
{
	PyObject* imguiModule = PyModule_Create(&ImGuiModuleDef);
	if (imguiModule == nullptr) return false;

	PyObject* col = MakeConstantNamespace("biaseddoom.imgui.Col",
		"ImGuiCol_* indices for push_style_color()/get_style_color()/set_style_color().",
		ColEntries, std::size(ColEntries));
	PyObject* styleVar = MakeConstantNamespace("biaseddoom.imgui.StyleVar",
		"ImGuiStyleVar_* indices for push_style_var()/get_style_var()/set_style_var().",
		StyleVarEntries, std::size(StyleVarEntries));
	PyObject* windowFlags = MakeConstantNamespace("biaseddoom.imgui.WindowFlags",
		"ImGuiWindowFlags_* bits for begin()/begin_child().", WindowFlagsEntries, std::size(WindowFlagsEntries));
	PyObject* cond = MakeConstantNamespace("biaseddoom.imgui.Cond",
		"ImGuiCond_* values for set_next_window_*() calls.", CondEntries, std::size(CondEntries));
	PyObject* key = MakeConstantNamespace("biaseddoom.imgui.Key",
		"ImGuiKey_* values for is_key_down()/is_key_pressed()/is_key_chord_pressed()/shortcut().",
		KeyEntries, std::size(KeyEntries));
	PyObject* mod = MakeConstantNamespace("biaseddoom.imgui.Mod",
		"ImGuiMod_* modifier bits for the mods argument of is_key_chord_pressed()/shortcut().",
		ModEntries, std::size(ModEntries));
	PyObject* inputTextFlags = MakeConstantNamespace("biaseddoom.imgui.InputTextFlags",
		"ImGuiInputTextFlags_* bits for input_text().", InputTextFlagsEntries, std::size(InputTextFlagsEntries));
	if (col == nullptr || styleVar == nullptr || windowFlags == nullptr || cond == nullptr ||
		key == nullptr || mod == nullptr || inputTextFlags == nullptr ||
		PyModule_AddObject(imguiModule, "Col", col) < 0 ||
		PyModule_AddObject(imguiModule, "StyleVar", styleVar) < 0 ||
		PyModule_AddObject(imguiModule, "WindowFlags", windowFlags) < 0 ||
		PyModule_AddObject(imguiModule, "Cond", cond) < 0 ||
		PyModule_AddObject(imguiModule, "Key", key) < 0 ||
		PyModule_AddObject(imguiModule, "Mod", mod) < 0 ||
		PyModule_AddObject(imguiModule, "InputTextFlags", inputTextFlags) < 0)
	{
		Py_XDECREF(col);
		Py_XDECREF(styleVar);
		Py_XDECREF(windowFlags);
		Py_XDECREF(cond);
		Py_XDECREF(key);
		Py_XDECREF(mod);
		Py_XDECREF(inputTextFlags);
		Py_DECREF(imguiModule);
		return false;
	}

	if (PyModule_AddObject(module, "imgui", imguiModule) < 0)
	{
		Py_DECREF(imguiModule);
		return false;
	}
	return true;
}
} // namespace PythonImGui

#else // !BIASEDDOOM_IMGUI

namespace PythonImGui
{
	bool Initialize(_object*) { return true; }
}

#endif // BIASEDDOOM_IMGUI
