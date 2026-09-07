#pragma once

class AActor;
struct _object;

namespace PythonRuntime::GameApi
{
	// Adds the high-performance gameplay types and functions to the built-in
	// biaseddoom module. The caller retains ownership of module.
	bool Initialize(_object* module);

	// Actor handles are rooted only while Python objects refer to them. These
	// hooks integrate that registry with the engine GC and map lifecycle.
	void MarkRoots();
	void InvalidateWorld();
	void Shutdown();

	// Return a new Python reference representing actor, or None for nullptr.
	_object* MakeActorRef(AActor* actor);

	// Return a new Python Player handle for the given player index.
	_object* MakePlayerRef(int index);

	// Unwraps an Actor handle (or a nonzero TID) into a live actor for other
	// native modules. Raises a Python error and returns nullptr when invalid.
	AActor* ActorFromHandle(_object* object);
}
