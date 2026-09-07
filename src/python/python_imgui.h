#pragma once

struct _object;

namespace PythonImGui
{
	// Creates the biaseddoom.imgui submodule and attaches it to the module
	// object passed to PythonRuntime::GameApi::Initialize. The caller retains
	// ownership of module. No-op (returns true) in builds without ImGui
	// support, so the game API never has to care.
	bool Initialize(_object* module);
}
