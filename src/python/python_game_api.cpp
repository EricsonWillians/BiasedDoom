//---------------------------------------------------------------------------
//
// BiasedDoom embedded Python gameplay API
//
// This file contains the high-frequency, mutable gameplay surface. Keeping it
// separate from interpreter/bootstrap code makes the cost model explicit:
// Python receives small native handles and crosses the C API once for bulk
// operations instead of rebuilding full dictionaries every tic.
//
//---------------------------------------------------------------------------

#include "python_game_api.h"
#include "python_runtime.h"
#include "python_displaylist.h"
#include "python_imgui.h"

#ifdef BIASEDDOOM_PYTHON

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "a_sharedglobal.h"
#include "actor.h"
#include "c_cvars.h"
#include "cmdlib.h"
#include "d_event.h"
#include "d_player.h"
#include "dobjgc.h"
#include "doomstat.h"
#include "g_game.h"
#include "g_levellocals.h"
#include "g_level.h"
#include "g_statusbar/sbar.h"
#include "gamestate.h"
#include "m_random.h"
#include "p_conversation.h"
#include "p_lnspec.h"
#include "p_local.h"
#include "r_defs.h"
#include "r_data/r_translate.h"
#include "s_doomsound.h"
#include "s_music.h"
#include "s_soundinternal.h"
#include "savegamemanager.h"
#include "scriptutil.h"
#include "thingdef.h"
#include "types.h"
#include "v_font.h"
#include "vm.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _PyCFunction_CAST
#define BD_GAME_KEYWORD_FUNCTION(function) _PyCFunction_CAST(function)
#else
#define BD_GAME_KEYWORD_FUNCTION(function) \
	reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(function))
#endif

EXTERN_CVAR(Float, i_timescale)

namespace PythonRuntime::GameApi
{
namespace
{
struct ActorSlot
{
	TObjPtr<AActor*> Actor = MakeObjPtr<AActor*>(nullptr);
	uint32_t Generation = 1;
	uint32_t PythonReferences = 0;
};

struct PyActorRef
{
	PyObject_HEAD
	uint32_t Slot;
	uint32_t Generation;
};

struct PyPlayerRef
{
	PyObject_HEAD
	int Index;
};

enum class WorldKind : uint8_t
{
	Sector,
	Line,
};

struct PyWorldRef
{
	PyObject_HEAD
	uint32_t Generation;
	int Index;
};

struct PyRngStream
{
	PyObject_HEAD
	FRandom* Stream;
};

std::vector<ActorSlot> actorSlots;
std::unordered_map<AActor*, uint32_t> actorLookup;
PyTypeObject* actorRefType = nullptr;
PyTypeObject* playerRefType = nullptr;
PyTypeObject* sectorRefType = nullptr;
PyTypeObject* lineRefType = nullptr;
PyTypeObject* rngStreamType = nullptr;
uint32_t worldGeneration = 1;
bool markerRegistered = false;

// Persistent per-actor script data backing bd.actor_data(). Keyed by
// (slot << 32) | generation so a recycled slot can never resurrect a previous
// actor's dict (AcquireActorSlot bumps the generation on every reuse). The map
// holds a strong reference to each dict; entries are purged from
// InvalidateActorSlot (destruction/GC) and InvalidateWorld (map change), so
// the data deliberately does not survive savegames or level transitions.
std::map<uint64_t, PyObject*> actorDataStore;

// Actor slots found unusable inside an engine GC marker cannot be invalidated
// there: InvalidateActorSlot purges the actor_data dict with Py_DECREF, and
// no Python C-API work may run from inside GC::Mark. Such slots are queued
// here and drained from non-marker contexts (start of OnWorldPreTick,
// InvalidateWorld, Shutdown). Queued slots stay occupied until drained, so
// AcquireActorSlot can never recycle one behind the queue's back.
std::vector<uint32_t> deferredSlotInvalidations;

void QueueDeferredSlotInvalidation(uint32_t index)
{
	if (std::find(deferredSlotInvalidations.begin(), deferredSlotInvalidations.end(), index) == deferredSlotInvalidations.end())
	{
		deferredSlotInvalidations.push_back(index);
	}
}

// Runs outside marker contexts only; safe to Py_DECREF.
void InvalidateActorSlot(uint32_t index);

void DrainDeferredInvalidationsInternal()
{
	if (deferredSlotInvalidations.empty()) return;
	// Swap first: the purge decrefs below can run Python finalizers that
	// resolve actor handles and re-queue entries; those are drained on the
	// next pass instead of invalidating this vector.
	std::vector<uint32_t> pending;
	pending.swap(deferredSlotInvalidations);
	for (uint32_t index : pending)
	{
		if (index < actorSlots.size()) InvalidateActorSlot(index);
	}
}

uint64_t ActorDataKey(uint32_t slot, uint32_t generation)
{
	return (static_cast<uint64_t>(slot) << 32) | generation;
}

void PurgeActorData(uint32_t slot, uint32_t generation)
{
	auto entry = actorDataStore.find(ActorDataKey(slot, generation));
	if (entry == actorDataStore.end()) return;
	Py_DECREF(entry->second);
	actorDataStore.erase(entry);
}

bool IsUsableActor(AActor* actor)
{
	return actor != nullptr && !(actor->ObjectFlags & OF_EuthanizeMe) &&
		actor->Level == primaryLevel;
}

void InvalidateActorSlot(uint32_t index)
{
	if (index >= actorSlots.size()) return;
	ActorSlot& slot = actorSlots[index];
	AActor* actor = slot.Actor.ForceGet();
	if (actor != nullptr) actorLookup.erase(actor);
	slot.Actor = nullptr;
	// Purge with the generation at invalidation time: only matching-generation
	// handles could ever resolve to this actor, so this key can no longer be
	// reached once the slot is gone or recycled.
	PurgeActorData(index, slot.Generation);
}

uint32_t AcquireActorSlot(AActor* actor)
{
	auto existing = actorLookup.find(actor);
	if (existing != actorLookup.end())
	{
		ActorSlot& slot = actorSlots[existing->second];
		if (slot.Actor.ForceGet() == actor)
		{
			++slot.PythonReferences;
			return existing->second;
		}
		actorLookup.erase(existing);
	}

	uint32_t index = 0;
	for (; index < actorSlots.size(); ++index)
	{
		if (actorSlots[index].PythonReferences == 0 && actorSlots[index].Actor.ForceGet() == nullptr) break;
	}
	if (index == actorSlots.size()) actorSlots.emplace_back();

	ActorSlot& slot = actorSlots[index];
	if (++slot.Generation == 0) ++slot.Generation;
	slot.Actor = actor;
	slot.PythonReferences = 1;
	actorLookup[actor] = index;
	return index;
}

AActor* ResolveActor(PyActorRef* reference, bool mutation, bool raise)
{
	if (!CheckApiThread()) return nullptr;
	if (mutation && !CheckGameplayMutation()) return nullptr;
	if (reference->Slot >= actorSlots.size())
	{
		if (raise) PyErr_SetString(PyExc_ReferenceError, "actor handle is no longer valid");
		return nullptr;
	}
	ActorSlot& slot = actorSlots[reference->Slot];
	if (slot.Generation != reference->Generation || !IsUsableActor(slot.Actor.ForceGet()))
	{
		// The non-raising valid-check path can be reached from contexts that
		// must not Py_DECREF (actor_data purge) right now, so defer the slot
		// bookkeeping; the raising path runs from ordinary Python calls and
		// invalidates immediately.
		if (slot.Generation == reference->Generation)
		{
			if (raise) InvalidateActorSlot(reference->Slot);
			else QueueDeferredSlotInvalidation(reference->Slot);
		}
		if (raise) PyErr_SetString(PyExc_ReferenceError, "actor was destroyed or belongs to an unloaded map");
		return nullptr;
	}
	return slot.Actor.Get();
}

AActor* ResolveActorArgument(PyObject* object, const char* argument, bool allowNone, bool mutation)
{
	if (object == Py_None && allowNone) return nullptr;
	if (actorRefType != nullptr && PyObject_TypeCheck(object, actorRefType))
	{
		return ResolveActor(reinterpret_cast<PyActorRef*>(object), mutation, true);
	}
	if (PyLong_Check(object))
	{
		const long tid = PyLong_AsLong(object);
		if (PyErr_Occurred()) return nullptr;
		if (tid == 0)
		{
			if (allowNone) return nullptr;
			PyErr_Format(PyExc_LookupError, "%s TID must not be zero", argument);
			return nullptr;
		}
		if (mutation && !CheckGameplayMutation()) return nullptr;
		if (!mutation && !CheckApiThread()) return nullptr;
		if (primaryLevel != nullptr)
		{
			AActor* actor = primaryLevel->GetActorIterator(static_cast<int>(tid)).Next();
			if (IsUsableActor(actor)) return actor;
		}
		PyErr_Format(PyExc_LookupError, "no live actor for %s TID %ld", argument, tid);
		return nullptr;
	}
	PyErr_Format(PyExc_TypeError, "%s must be an Actor, a nonzero TID, or None", argument);
	return nullptr;
}

PyObject* Vector3(double x, double y, double z)
{
	return Py_BuildValue("(ddd)", x, y, z);
}

PyObject* Vector2(double x, double y)
{
	return Py_BuildValue("(dd)", x, y);
}

bool ParseVector3(PyObject* object, double& x, double& y, double& z, const char* description)
{
	PyObject* sequence = PySequence_Fast(object, description);
	if (sequence == nullptr) return false;
	if (PySequence_Fast_GET_SIZE(sequence) != 3)
	{
		Py_DECREF(sequence);
		PyErr_SetString(PyExc_ValueError, description);
		return false;
	}
	x = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sequence, 0));
	y = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sequence, 1));
	z = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sequence, 2));
	const bool valid = !PyErr_Occurred();
	Py_DECREF(sequence);
	return valid;
}

PyObject* ActorSnapshot(AActor* actor)
{
	PyObject* result = PyDict_New();
	if (result == nullptr) return nullptr;
	auto set = [result](const char* key, PyObject* value)
	{
		if (value == nullptr) return false;
		const bool ok = PyDict_SetItemString(result, key, value) == 0;
		Py_DECREF(value);
		return ok;
	};
	set("ref", MakeActorRef(actor));
	set("class_name", PyUnicode_FromString(actor->GetClass()->TypeName.GetChars()));
	set("tid", PyLong_FromLong(actor->tid));
	set("health", PyLong_FromLong(actor->health));
	set("position", Vector3(actor->X(), actor->Y(), actor->Z()));
	set("velocity", Vector3(actor->Vel.X, actor->Vel.Y, actor->Vel.Z));
	set("angles", Vector3(actor->Angles.Yaw.Degrees(), actor->Angles.Pitch.Degrees(), actor->Angles.Roll.Degrees()));
	set("radius", PyFloat_FromDouble(actor->radius));
	set("height", PyFloat_FromDouble(actor->Height));
	set("alive", PyBool_FromLong(actor->health > 0));
	return result;
}

void ActorRefDealloc(PyObject* object)
{
	PyActorRef* reference = reinterpret_cast<PyActorRef*>(object);
	if (reference->Slot < actorSlots.size())
	{
		ActorSlot& slot = actorSlots[reference->Slot];
		if (slot.Generation == reference->Generation && slot.PythonReferences > 0)
		{
			if (--slot.PythonReferences == 0)
			{
				InvalidateActorSlot(reference->Slot);
				if (++slot.Generation == 0) ++slot.Generation;
			}
		}
	}
	Py_TYPE(object)->tp_free(object);
}

PyObject* ActorRefRepr(PyObject* object)
{
	PyActorRef* reference = reinterpret_cast<PyActorRef*>(object);
	AActor* actor = ResolveActor(reference, false, false);
	if (actor == nullptr)
	{
		PyErr_Clear();
		return PyUnicode_FromString("<biaseddoom.Actor invalid>");
	}
	return PyUnicode_FromFormat("<biaseddoom.Actor %s tid=%d at %p>",
		actor->GetClass()->TypeName.GetChars(), actor->tid, actor);
}

int ActorRefBool(PyObject* object)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, false);
	if (actor != nullptr) return 1;
	PyErr_Clear();
	return 0;
}

Py_hash_t ActorRefHash(PyObject* object)
{
	PyActorRef* reference = reinterpret_cast<PyActorRef*>(object);
	const uint64_t value = (static_cast<uint64_t>(reference->Generation) << 32) | reference->Slot;
	return static_cast<Py_hash_t>(value == static_cast<uint64_t>(-1) ? -2 : value);
}

PyObject* ActorRefRichCompare(PyObject* left, PyObject* right, int operation)
{
	if (actorRefType == nullptr || !PyObject_TypeCheck(right, actorRefType)) Py_RETURN_NOTIMPLEMENTED;
	PyActorRef* a = reinterpret_cast<PyActorRef*>(left);
	PyActorRef* b = reinterpret_cast<PyActorRef*>(right);
	const bool equal = a->Slot == b->Slot && a->Generation == b->Generation;
	if (operation == Py_EQ) return PyBool_FromLong(equal);
	if (operation == Py_NE) return PyBool_FromLong(!equal);
	Py_RETURN_NOTIMPLEMENTED;
}

PyObject* ActorValid(PyObject* object, void*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, false);
	if (actor != nullptr) Py_RETURN_TRUE;
	PyErr_Clear();
	Py_RETURN_FALSE;
}

enum class ActorScalar : intptr_t
{
	Tid,
	Health,
	X,
	Y,
	Z,
	VelocityX,
	VelocityY,
	VelocityZ,
	Angle,
	Pitch,
	Roll,
	Radius,
	Height,
	Speed,
	Gravity,
	Mass,
	Alpha,
	ScaleX,
	ScaleY,
	Tics,
	Score,
	Special,
	DamageFactor,
	DamageMultiply,
	WaterLevel,
	FloorZ,
	CeilingZ,
};

ActorScalar ScalarFromClosure(void* closure)
{
	return static_cast<ActorScalar>(reinterpret_cast<intptr_t>(closure));
}

PyObject* ActorScalarGet(PyObject* object, void* closure)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	switch (ScalarFromClosure(closure))
	{
	case ActorScalar::Tid: return PyLong_FromLong(actor->tid);
	case ActorScalar::Health: return PyLong_FromLong(actor->health);
	case ActorScalar::X: return PyFloat_FromDouble(actor->X());
	case ActorScalar::Y: return PyFloat_FromDouble(actor->Y());
	case ActorScalar::Z: return PyFloat_FromDouble(actor->Z());
	case ActorScalar::VelocityX: return PyFloat_FromDouble(actor->Vel.X);
	case ActorScalar::VelocityY: return PyFloat_FromDouble(actor->Vel.Y);
	case ActorScalar::VelocityZ: return PyFloat_FromDouble(actor->Vel.Z);
	case ActorScalar::Angle: return PyFloat_FromDouble(actor->Angles.Yaw.Degrees());
	case ActorScalar::Pitch: return PyFloat_FromDouble(actor->Angles.Pitch.Degrees());
	case ActorScalar::Roll: return PyFloat_FromDouble(actor->Angles.Roll.Degrees());
	case ActorScalar::Radius: return PyFloat_FromDouble(actor->radius);
	case ActorScalar::Height: return PyFloat_FromDouble(actor->Height);
	case ActorScalar::Speed: return PyFloat_FromDouble(actor->Speed);
	case ActorScalar::Gravity: return PyFloat_FromDouble(actor->Gravity);
	case ActorScalar::Mass: return PyLong_FromLong(actor->Mass);
	case ActorScalar::Alpha: return PyFloat_FromDouble(actor->Alpha);
	case ActorScalar::ScaleX: return PyFloat_FromDouble(actor->Scale.X);
	case ActorScalar::ScaleY: return PyFloat_FromDouble(actor->Scale.Y);
	case ActorScalar::Tics: return PyLong_FromLong(actor->tics);
	case ActorScalar::Score: return PyLong_FromLong(actor->Score);
	case ActorScalar::Special: return PyLong_FromLong(actor->special);
	case ActorScalar::DamageFactor: return PyFloat_FromDouble(actor->DamageFactor);
	case ActorScalar::DamageMultiply: return PyFloat_FromDouble(actor->DamageMultiply);
	case ActorScalar::WaterLevel: return PyLong_FromLong(actor->waterlevel);
	case ActorScalar::FloorZ: return PyFloat_FromDouble(actor->floorz);
	case ActorScalar::CeilingZ: return PyFloat_FromDouble(actor->ceilingz);
	}
	Py_RETURN_NONE;
}

int ActorScalarSet(PyObject* object, PyObject* value, void* closure)
{
	if (value == nullptr)
	{
		PyErr_SetString(PyExc_TypeError, "actor properties cannot be deleted");
		return -1;
	}
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return -1;
	const ActorScalar field = ScalarFromClosure(closure);
	if (field == ActorScalar::FloorZ || field == ActorScalar::CeilingZ || field == ActorScalar::WaterLevel)
	{
		PyErr_SetString(PyExc_AttributeError, "this actor property is read-only");
		return -1;
	}

	if (field == ActorScalar::Tid || field == ActorScalar::Health || field == ActorScalar::Mass ||
		field == ActorScalar::Tics || field == ActorScalar::Score || field == ActorScalar::Special)
	{
		const long number = PyLong_AsLong(value);
		if (PyErr_Occurred()) return -1;
		switch (field)
		{
		case ActorScalar::Tid: actor->SetTID(static_cast<int>(number)); break;
		case ActorScalar::Health: actor->health = static_cast<int>(number); break;
		case ActorScalar::Mass: actor->Mass = static_cast<int32_t>(number); break;
		case ActorScalar::Tics: actor->tics = static_cast<int32_t>(number); break;
		case ActorScalar::Score: actor->Score = static_cast<int>(number); break;
		case ActorScalar::Special: actor->special = static_cast<int>(number); break;
		default: break;
		}
		return 0;
	}

	const double number = PyFloat_AsDouble(value);
	if (PyErr_Occurred()) return -1;
	switch (field)
	{
	case ActorScalar::X: actor->SetOrigin(number, actor->Y(), actor->Z(), true); break;
	case ActorScalar::Y: actor->SetOrigin(actor->X(), number, actor->Z(), true); break;
	case ActorScalar::Z: actor->SetOrigin(actor->X(), actor->Y(), number, true); break;
	case ActorScalar::VelocityX: actor->Vel.X = number; break;
	case ActorScalar::VelocityY: actor->Vel.Y = number; break;
	case ActorScalar::VelocityZ: actor->Vel.Z = number; break;
	case ActorScalar::Angle: actor->Angles.Yaw = DAngle::fromDeg(number); break;
	case ActorScalar::Pitch: actor->Angles.Pitch = DAngle::fromDeg(number); break;
	case ActorScalar::Roll: actor->Angles.Roll = DAngle::fromDeg(number); break;
	case ActorScalar::Radius: actor->radius = std::max(0.0, number); break;
	case ActorScalar::Height: actor->Height = std::max(0.0, number); break;
	case ActorScalar::Speed: actor->Speed = number; break;
	case ActorScalar::Gravity: actor->Gravity = number; break;
	case ActorScalar::Alpha: actor->Alpha = std::clamp(number, 0.0, 1.0); break;
	case ActorScalar::ScaleX: actor->Scale.X = number; break;
	case ActorScalar::ScaleY: actor->Scale.Y = number; break;
	case ActorScalar::DamageFactor: actor->DamageFactor = std::max(0.0, number); break;
	case ActorScalar::DamageMultiply: actor->DamageMultiply = std::max(0.0, number); break;
	default: break;
	}
	return 0;
}

PyObject* ActorClassName(PyObject* object, void*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	return actor == nullptr ? nullptr : PyUnicode_FromString(actor->GetClass()->TypeName.GetChars());
}

PyObject* ActorAlive(PyObject* object, void*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	return PyBool_FromLong(actor->health > 0);
}

PyObject* ActorIsPlayer(PyObject* object, void*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	return PyBool_FromLong(actor->player != nullptr);
}

PyObject* ActorIsMonster(PyObject* object, void*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	return PyBool_FromLong((actor->flags3 & MF3_ISMONSTER) != 0);
}

PyObject* ActorPosition(PyObject* object, void*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	return actor == nullptr ? nullptr : Vector3(actor->X(), actor->Y(), actor->Z());
}

int ActorPositionSet(PyObject* object, PyObject* value, void*)
{
	double x, y, z;
	if (!ParseVector3(value, x, y, z, "position must contain exactly three numbers")) return -1;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return -1;
	actor->SetOrigin(x, y, z, true);
	return 0;
}

PyObject* ActorVelocity(PyObject* object, void*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	return actor == nullptr ? nullptr : Vector3(actor->Vel.X, actor->Vel.Y, actor->Vel.Z);
}

int ActorVelocitySet(PyObject* object, PyObject* value, void*)
{
	double x, y, z;
	if (!ParseVector3(value, x, y, z, "velocity must contain exactly three numbers")) return -1;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return -1;
	actor->Vel = DVector3(x, y, z);
	return 0;
}

PyObject* ActorAngles(PyObject* object, void*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	return actor == nullptr ? nullptr : Vector3(actor->Angles.Yaw.Degrees(), actor->Angles.Pitch.Degrees(), actor->Angles.Roll.Degrees());
}

int ActorAnglesSet(PyObject* object, PyObject* value, void*)
{
	double yaw, pitch, roll;
	if (!ParseVector3(value, yaw, pitch, roll, "angles must contain exactly three degree values")) return -1;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return -1;
	actor->Angles.Yaw = DAngle::fromDeg(yaw);
	actor->Angles.Pitch = DAngle::fromDeg(pitch);
	actor->Angles.Roll = DAngle::fromDeg(roll);
	return 0;
}

enum class ActorRelation : intptr_t
{
	Target,
	Master,
	Tracer,
};

PyObject* ActorRelationGet(PyObject* object, void* closure)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	AActor* related = nullptr;
	switch (static_cast<ActorRelation>(reinterpret_cast<intptr_t>(closure)))
	{
	case ActorRelation::Target: related = actor->target; break;
	case ActorRelation::Master: related = actor->master; break;
	case ActorRelation::Tracer: related = actor->tracer; break;
	}
	return MakeActorRef(IsUsableActor(related) ? related : nullptr);
}

int ActorRelationSet(PyObject* object, PyObject* value, void* closure)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return -1;
	AActor* related = ResolveActorArgument(value, "relationship", true, true);
	if (related == nullptr && value != Py_None) return -1;
	switch (static_cast<ActorRelation>(reinterpret_cast<intptr_t>(closure)))
	{
	case ActorRelation::Target: actor->target = related; GC::WriteBarrier(actor, actor->target); break;
	case ActorRelation::Master: actor->master = related; GC::WriteBarrier(actor, actor->master); break;
	case ActorRelation::Tracer: actor->tracer = related; GC::WriteBarrier(actor, actor->tracer); break;
	}
	return 0;
}

PyObject* ActorArgsGet(PyObject* object, void*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	return Py_BuildValue("(iiiii)", actor->args[0], actor->args[1], actor->args[2], actor->args[3], actor->args[4]);
}

int ActorArgsSet(PyObject* object, PyObject* value, void*)
{
	PyObject* sequence = PySequence_Fast(value, "args must be a sequence of exactly five integers");
	if (sequence == nullptr) return -1;
	if (PySequence_Fast_GET_SIZE(sequence) != 5)
	{
		Py_DECREF(sequence);
		PyErr_SetString(PyExc_ValueError, "args must contain exactly five integers");
		return -1;
	}
	int parsed[5];
	for (int index = 0; index < 5; ++index)
	{
		parsed[index] = static_cast<int>(PyLong_AsLong(PySequence_Fast_GET_ITEM(sequence, index)));
		if (PyErr_Occurred())
		{
			Py_DECREF(sequence);
			return -1;
		}
	}
	Py_DECREF(sequence);
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return -1;
	std::copy(std::begin(parsed), std::end(parsed), std::begin(actor->args));
	return 0;
}

// tint: (r, g, b) tuple remaps the sprite's brightness ramp to that color
// (both renderers honor actor->Translation); None resets to the class
// default translation. Built on CreateActorTintTranslation's lazily
// created, deduplicated palette tables; scripts re-apply tints after a
// savegame load, so nothing here needs serialization.
PyObject* ActorTintGet(PyObject* object, void*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	PalEntry color;
	if (!ActorTintTranslationColor(actor->Translation, color)) Py_RETURN_NONE;
	return Py_BuildValue("(iii)", color.r, color.g, color.b);
}

int ActorTintSet(PyObject* object, PyObject* value, void*)
{
	if (value == nullptr)
	{
		PyErr_SetString(PyExc_TypeError, "actor properties cannot be deleted");
		return -1;
	}
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return -1;
	if (value == Py_None)
	{
		actor->Translation = actor->GetDefault()->Translation;
		return 0;
	}
	PyObject* sequence = PySequence_Fast(value, "tint must be an (r, g, b) tuple or None");
	if (sequence == nullptr) return -1;
	if (PySequence_Fast_GET_SIZE(sequence) != 3)
	{
		Py_DECREF(sequence);
		PyErr_SetString(PyExc_ValueError, "tint must contain exactly three components (r, g, b)");
		return -1;
	}
	int components[3];
	for (int index = 0; index < 3; ++index)
	{
		components[index] = static_cast<int>(PyLong_AsLong(PySequence_Fast_GET_ITEM(sequence, index)));
		if (PyErr_Occurred())
		{
			Py_DECREF(sequence);
			return -1;
		}
	}
	Py_DECREF(sequence);
	const PalEntry color(255,
		static_cast<uint8_t>(std::clamp(components[0], 0, 255)),
		static_cast<uint8_t>(std::clamp(components[1], 0, 255)),
		static_cast<uint8_t>(std::clamp(components[2], 0, 255)));
	actor->Translation = CreateActorTintTranslation(color);
	return 0;
}

PyObject* ActorSnapshotMethod(PyObject* object, PyObject*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	return actor == nullptr ? nullptr : ActorSnapshot(actor);
}

PyObject* ActorSetPosition(PyObject* object, PyObject* args, PyObject* kwargs)
{
	double x, y, z;
	int check = 1;
	int fog = 0;
	static const char* keywords[] = { "x", "y", "z", "check", "fog", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ddd|pp:set_position", const_cast<char**>(keywords),
		&x, &y, &z, &check, &fog)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	const DVector3 oldPosition = actor->Pos();
	bool moved = false;
	if (fog)
	{
		moved = P_MoveThing(actor, DVector3(x, y, z), true);
	}
	else
	{
		actor->SetOrigin(x, y, z, true);
		moved = !check || P_TestMobjLocation(actor);
		if (!moved) actor->SetOrigin(oldPosition, true);
	}
	return PyBool_FromLong(moved);
}

PyObject* ActorSetVelocity(PyObject* object, PyObject* args, PyObject* kwargs)
{
	double x, y, z;
	int add = 0;
	static const char* keywords[] = { "x", "y", "z", "add", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ddd|p:set_velocity", const_cast<char**>(keywords),
		&x, &y, &z, &add)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	if (add) actor->Vel += DVector3(x, y, z);
	else actor->Vel = DVector3(x, y, z);
	Py_RETURN_NONE;
}

PyObject* ActorThrust(PyObject* object, PyObject* args, PyObject* kwargs)
{
	double angle, force;
	double vertical = 0;
	int replace = 0;
	static const char* keywords[] = { "angle", "force", "vertical", "replace", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "dd|dp:thrust", const_cast<char**>(keywords),
		&angle, &force, &vertical, &replace)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	const double radians = angle * (M_PI / 180.0);
	const DVector3 velocity(std::cos(radians) * force, std::sin(radians) * force, vertical);
	if (replace) actor->Vel = velocity;
	else actor->Vel += velocity;
	Py_RETURN_NONE;
}

PyObject* ActorDamage(PyObject* object, PyObject* args, PyObject* kwargs)
{
	int amount;
	const char* damageType = "None";
	PyObject* inflictorObject = Py_None;
	PyObject* sourceObject = Py_None;
	int flags = 0;
	static const char* keywords[] = { "amount", "damage_type", "inflictor", "source", "flags", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "i|sOOi:damage", const_cast<char**>(keywords),
		&amount, &damageType, &inflictorObject, &sourceObject, &flags)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	AActor* inflictor = ResolveActorArgument(inflictorObject, "inflictor", true, true);
	if (inflictor == nullptr && inflictorObject != Py_None) return nullptr;
	AActor* source = ResolveActorArgument(sourceObject, "source", true, true);
	if (source == nullptr && sourceObject != Py_None) return nullptr;
	return PyLong_FromLong(P_DamageMobj(actor, inflictor, source, amount, FName(damageType), flags));
}

PyObject* ActorHeal(PyObject* object, PyObject* args, PyObject* kwargs)
{
	int amount;
	int maximum = 0;
	static const char* keywords[] = { "amount", "maximum", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "i|i:heal", const_cast<char**>(keywords), &amount, &maximum)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	return PyBool_FromLong(P_GiveBody(actor, amount, maximum));
}

PyObject* ActorDestroy(PyObject* object, PyObject*)
{
	PyActorRef* reference = reinterpret_cast<PyActorRef*>(object);
	AActor* actor = ResolveActor(reference, true, true);
	if (actor == nullptr) return nullptr;
	actor->ClearCounters();
	actor->Destroy();
	InvalidateActorSlot(reference->Slot);
	Py_RETURN_NONE;
}

PyObject* ActorDistanceTo(PyObject* object, PyObject* args)
{
	PyObject* otherObject = nullptr;
	if (!PyArg_ParseTuple(args, "O:distance_to", &otherObject)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	AActor* other = ResolveActorArgument(otherObject, "other", false, false);
	if (other == nullptr) return nullptr;
	return PyFloat_FromDouble((actor->Pos() - other->Pos()).Length());
}

PyObject* ActorCheckSight(PyObject* object, PyObject* args, PyObject* kwargs)
{
	PyObject* otherObject = nullptr;
	int flags = 0;
	static const char* keywords[] = { "other", "flags", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|i:check_sight", const_cast<char**>(keywords), &otherObject, &flags)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	AActor* other = ResolveActorArgument(otherObject, "other", false, false);
	if (other == nullptr) return nullptr;
	return PyBool_FromLong(P_CheckSight(actor, other, flags));
}

PyObject* ActorGetFlag(PyObject* object, PyObject* args)
{
	const char* name = nullptr;
	if (!PyArg_ParseTuple(args, "s:get_flag", &name)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	return PyBool_FromLong(CheckActorFlag(actor, name, false));
}

PyObject* ActorSetFlag(PyObject* object, PyObject* args)
{
	const char* name = nullptr;
	int enabled = 0;
	if (!PyArg_ParseTuple(args, "sp:set_flag", &name, &enabled)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	FFlagDef* flag = FindFlag(actor->GetClass(), name, nullptr, true);
	if (flag == nullptr)
	{
		PyErr_Format(PyExc_ValueError, "unknown or inaccessible actor flag '%s'", name);
		return nullptr;
	}
	ModActorFlag(actor, flag, enabled != 0);
	Py_RETURN_NONE;
}

FState* FindActorState(AActor* actor, const char* label)
{
	std::vector<FName> names;
	std::string remaining(label == nullptr ? "" : label);
	size_t start = 0;
	while (start <= remaining.size())
	{
		const size_t end = remaining.find('.', start);
		const std::string part = remaining.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (!part.empty()) names.emplace_back(part.c_str());
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return names.empty() ? nullptr : actor->FindState(static_cast<int>(names.size()), names.data(), false);
}

PyObject* ActorSetState(PyObject* object, PyObject* args, PyObject* kwargs)
{
	const char* label = nullptr;
	int callActions = 1;
	static const char* keywords[] = { "label", "call_actions", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|p:set_state", const_cast<char**>(keywords), &label, &callActions)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	FState* state = FindActorState(actor, label);
	if (state == nullptr)
	{
		PyErr_Format(PyExc_LookupError, "actor class %s has no state '%s'", actor->GetClass()->TypeName.GetChars(), label);
		return nullptr;
	}
	return PyBool_FromLong(actor->SetState(state, !callActions));
}

PClassActor* InventoryClass(const char* className)
{
	PClassActor* itemClass = PClass::FindActor(FName(className));
	if (itemClass == nullptr)
	{
		PyErr_Format(PyExc_ValueError, "unknown inventory class '%s'", className);
	}
	return itemClass;
}

PyObject* ActorInventoryCount(PyObject* object, PyObject* args, PyObject* kwargs)
{
	const char* className = nullptr;
	int subclasses = 0;
	static const char* keywords[] = { "class_name", "subclasses", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|p:inventory_count", const_cast<char**>(keywords), &className, &subclasses)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), false, true);
	if (actor == nullptr) return nullptr;
	PClassActor* itemClass = InventoryClass(className);
	if (itemClass == nullptr) return nullptr;
	AActor* item = actor->FindInventory(itemClass, subclasses != 0);
	return PyLong_FromLong(item == nullptr ? 0 : item->IntVar(NAME_Amount));
}

PyObject* ActorGiveInventory(PyObject* object, PyObject* args, PyObject* kwargs)
{
	const char* className = nullptr;
	int amount = 1;
	static const char* keywords[] = { "class_name", "amount", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|i:give_inventory", const_cast<char**>(keywords), &className, &amount)) return nullptr;
	if (amount < 0)
	{
		PyErr_SetString(PyExc_ValueError, "amount must not be negative");
		return nullptr;
	}
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	PClassActor* itemClass = InventoryClass(className);
	if (itemClass == nullptr) return nullptr;
	ScriptUtil::Exec(NAME_GiveInventory, ScriptUtil::Pointer, actor,
		ScriptUtil::Int, itemClass->TypeName.GetIndex(), ScriptUtil::Int, amount, ScriptUtil::End);
	AActor* item = actor->FindInventory(itemClass, false);
	return PyLong_FromLong(item == nullptr ? 0 : item->IntVar(NAME_Amount));
}

PyObject* ActorTakeInventory(PyObject* object, PyObject* args, PyObject* kwargs)
{
	const char* className = nullptr;
	int amount = 1;
	static const char* keywords[] = { "class_name", "amount", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|i:take_inventory", const_cast<char**>(keywords), &className, &amount)) return nullptr;
	if (amount < 0)
	{
		PyErr_SetString(PyExc_ValueError, "amount must not be negative");
		return nullptr;
	}
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	PClassActor* itemClass = InventoryClass(className);
	if (itemClass == nullptr) return nullptr;
	ScriptUtil::Exec(NAME_TakeInventory, ScriptUtil::Pointer, actor,
		ScriptUtil::Int, itemClass->TypeName.GetIndex(), ScriptUtil::Int, amount, ScriptUtil::End);
	AActor* item = actor->FindInventory(itemClass, false);
	return PyLong_FromLong(item == nullptr ? 0 : item->IntVar(NAME_Amount));
}

PyObject* ActorUseInventory(PyObject* object, PyObject* args)
{
	const char* className = nullptr;
	if (!PyArg_ParseTuple(args, "s:use_inventory", &className)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	PClassActor* itemClass = InventoryClass(className);
	if (itemClass == nullptr) return nullptr;
	AActor* item = actor->FindInventory(itemClass, false);
	return PyBool_FromLong(item != nullptr && actor->UseInventory(item));
}

PyObject* ActorClearInventory(PyObject* object, PyObject*)
{
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	ScriptUtil::Exec(NAME_ClearInventory, ScriptUtil::Pointer, actor, ScriptUtil::End);
	Py_RETURN_NONE;
}

PyObject* ActorPlaySound(PyObject* object, PyObject* args, PyObject* kwargs)
{
	const char* soundName = nullptr;
	int channel = CHAN_BODY;
	double volume = 1.0;
	int looping = 0;
	double attenuation = ATTN_NORM;
	int local = 0;
	double pitch = 1.0;
	static const char* keywords[] = { "sound", "channel", "volume", "looping", "attenuation", "local", "pitch", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|idpdpd:play_sound", const_cast<char**>(keywords),
		&soundName, &channel, &volume, &looping, &attenuation, &local, &pitch)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	const FSoundID sound = S_FindSound(soundName);
	if (sound == NO_SOUND)
	{
		PyErr_Format(PyExc_LookupError, "unknown sound '%s'", soundName);
		return nullptr;
	}
	int flagBits = 0;
	if (looping) flagBits |= CHANF_LOOP | CHANF_NOSTOP;
	if (local) flagBits |= CHANF_LOCAL;
	S_SoundPitchActor(actor, channel & 7, EChanFlags::FromInt(flagBits | (channel & ~7)), sound,
		static_cast<float>(volume), static_cast<float>(attenuation), static_cast<float>(pitch));
	Py_RETURN_NONE;
}

PyObject* ActorStopSound(PyObject* object, PyObject* args)
{
	int channel = CHAN_BODY;
	if (!PyArg_ParseTuple(args, "|i:stop_sound", &channel)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	S_StopSound(actor, channel);
	Py_RETURN_NONE;
}

PyObject* ActorActivate(PyObject* object, PyObject* args, PyObject* kwargs)
{
	PyObject* activatorObject = Py_None;
	int deactivate = 0;
	static const char* keywords[] = { "activator", "deactivate", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|Op:activate", const_cast<char**>(keywords),
		&activatorObject, &deactivate)) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;
	AActor* activator = ResolveActorArgument(activatorObject, "activator", true, true);
	if (activator == nullptr && activatorObject != Py_None) return nullptr;
	if (deactivate) actor->CallDeactivate(activator);
	else actor->CallActivate(activator);
	Py_RETURN_NONE;
}

bool IsSupportedZScriptValue(PType* type)
{
	if (type == nullptr) return false;
	const int registerType = type->GetRegType();
	const int registerCount = type->GetRegCount();
	if (registerType == REGT_INT || registerType == REGT_STRING)
	{
		return registerCount == 1;
	}
	if (registerType == REGT_FLOAT)
	{
		return registerCount >= 1 && registerCount <= 4;
	}
	if (registerType == REGT_POINTER && registerCount == 1 && type->isObjectPointer())
	{
		PClass* pointedClass = static_cast<PObjectPointer*>(type)->PointedClass();
		return pointedClass != nullptr && pointedClass->IsDescendantOf(RUNTIME_CLASS(AActor));
	}
	return false;
}

bool AppendZScriptArgument(TArray<VMValue>& parameters, std::vector<FString>& strings,
	PType* type, PyObject* value, const char* methodName, unsigned argumentIndex)
{
	const int registerType = type->GetRegType();
	const int registerCount = type->GetRegCount();
	if (registerType == REGT_INT && registerCount == 1)
	{
		const long long parsed = PyLong_AsLongLong(value);
		if (PyErr_Occurred()) return false;
		if (parsed < INT_MIN || parsed > INT_MAX)
		{
			PyErr_Format(PyExc_OverflowError, "%s argument %u is outside the ZScript int range",
				methodName, argumentIndex);
			return false;
		}
		parameters.Push(VMValue(static_cast<int>(parsed)));
		return true;
	}
	if (registerType == REGT_FLOAT)
	{
		if (registerCount == 1)
		{
			const double parsed = PyFloat_AsDouble(value);
			if (PyErr_Occurred()) return false;
			parameters.Push(VMValue(parsed));
			return true;
		}
		PyObject* sequence = PySequence_Fast(value, "ZScript vector arguments must be sequences");
		if (sequence == nullptr) return false;
		if (PySequence_Fast_GET_SIZE(sequence) != registerCount)
		{
			PyErr_Format(PyExc_ValueError, "%s argument %u requires exactly %d vector components",
				methodName, argumentIndex, registerCount);
			Py_DECREF(sequence);
			return false;
		}
		for (int component = 0; component < registerCount; ++component)
		{
			const double parsed = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sequence, component));
			if (PyErr_Occurred())
			{
				Py_DECREF(sequence);
				return false;
			}
			parameters.Push(VMValue(parsed));
		}
		Py_DECREF(sequence);
		return true;
	}
	if (registerType == REGT_STRING && registerCount == 1)
	{
		const char* parsed = PyUnicode_AsUTF8(value);
		if (parsed == nullptr) return false;
		strings.emplace_back(parsed);
		parameters.Push(VMValue(&strings.back()));
		return true;
	}
	if (registerType == REGT_POINTER && registerCount == 1 && type->isObjectPointer())
	{
		AActor* actor = nullptr;
		if (value != Py_None)
		{
			actor = ResolveActorArgument(value, "ZScript actor argument", false, true);
			if (actor == nullptr) return false;
			PClass* expectedClass = static_cast<PObjectPointer*>(type)->PointedClass();
			if (expectedClass == nullptr || !actor->GetClass()->IsDescendantOf(expectedClass))
			{
				PyErr_Format(PyExc_TypeError, "%s argument %u requires %s, got %s",
					methodName, argumentIndex,
					expectedClass == nullptr ? "an actor" : expectedClass->TypeName.GetChars(),
					actor->GetClass()->TypeName.GetChars());
				return false;
			}
		}
		parameters.Push(VMValue(actor));
		return true;
	}
	PyErr_Format(PyExc_TypeError, "%s argument %u has unsupported ZScript type %s",
		methodName, argumentIndex, type->DescriptiveName());
	return false;
}

PyObject* ActorCallZScript(PyObject* object, PyObject* args)
{
	const Py_ssize_t pythonArgumentCount = PyTuple_GET_SIZE(args);
	if (pythonArgumentCount < 1)
	{
		PyErr_SetString(PyExc_TypeError, "call_zscript() requires a method name");
		return nullptr;
	}
	const char* methodName = PyUnicode_AsUTF8(PyTuple_GET_ITEM(args, 0));
	if (methodName == nullptr) return nullptr;
	AActor* actor = ResolveActor(reinterpret_cast<PyActorRef*>(object), true, true);
	if (actor == nullptr) return nullptr;

	PFunction* function = dyn_cast<PFunction>(actor->GetClass()->FindSymbol(FName(methodName), true));
	if (function == nullptr || function->Variants.Size() == 0)
	{
		PyErr_Format(PyExc_AttributeError, "actor class %s has no ZScript method '%s'",
			actor->GetClass()->TypeName.GetChars(), methodName);
		return nullptr;
	}
	PFunction::Variant& variant = function->Variants[0];
	constexpr uint32_t blockedFlags = VARF_Action | VARF_Private | VARF_Protected |
		VARF_Static | VARF_InternalAccess | VARF_VarArg | VARF_UI | VARF_Abstract;
	if ((variant.Flags & VARF_Method) == 0 || (variant.Flags & blockedFlags) != 0 ||
		variant.Implementation == nullptr || variant.Implementation->Unsafe)
	{
		PyErr_Format(PyExc_PermissionError, "ZScript method %s.%s is not callable from Python",
			actor->GetClass()->TypeName.GetChars(), methodName);
		return nullptr;
	}
	if (variant.Proto == nullptr || variant.Proto->ArgumentTypes.Size() == 0)
	{
		PyErr_Format(PyExc_RuntimeError, "ZScript method %s.%s has an invalid prototype",
			actor->GetClass()->TypeName.GetChars(), methodName);
		return nullptr;
	}
	if (variant.ArgFlags.Size() != variant.Proto->ArgumentTypes.Size())
	{
		PyErr_Format(PyExc_RuntimeError, "ZScript method %s.%s has inconsistent argument metadata",
			actor->GetClass()->TypeName.GetChars(), methodName);
		return nullptr;
	}

	const unsigned explicitCount = variant.Proto->ArgumentTypes.Size() - 1;
	unsigned requiredCount = explicitCount;
	while (requiredCount > 0 &&
		(variant.ArgFlags[requiredCount] & VARF_Optional) != 0) --requiredCount;
	const unsigned suppliedCount = static_cast<unsigned>(pythonArgumentCount - 1);
	if (suppliedCount < requiredCount || suppliedCount > explicitCount)
	{
		PyErr_Format(PyExc_TypeError, "%s() takes %u..%u ZScript arguments (%u given)",
			methodName, requiredCount, explicitCount, suppliedCount);
		return nullptr;
	}
	for (unsigned index = 1; index < variant.Proto->ArgumentTypes.Size(); ++index)
	{
		if ((variant.ArgFlags[index] & (VARF_Out | VARF_Ref)) != 0 ||
			!IsSupportedZScriptValue(variant.Proto->ArgumentTypes[index]))
		{
			PyErr_Format(PyExc_TypeError, "ZScript method %s.%s uses unsupported argument type %s",
				actor->GetClass()->TypeName.GetChars(), methodName,
				variant.Proto->ArgumentTypes[index]->DescriptiveName());
			return nullptr;
		}
	}
	if (variant.Proto->ReturnTypes.Size() > 1 ||
		(variant.Proto->ReturnTypes.Size() == 1 && !IsSupportedZScriptValue(variant.Proto->ReturnTypes[0])))
	{
		PyErr_Format(PyExc_TypeError, "ZScript method %s.%s has an unsupported return signature",
			actor->GetClass()->TypeName.GetChars(), methodName);
		return nullptr;
	}

	TArray<VMValue> parameters;
	parameters.Push(VMValue(actor));
	std::vector<FString> strings;
	strings.reserve(suppliedCount);
	for (unsigned index = 0; index < suppliedCount; ++index)
	{
		if (!AppendZScriptArgument(parameters, strings,
			variant.Proto->ArgumentTypes[index + 1], PyTuple_GET_ITEM(args, index + 1),
			methodName, index + 1)) return nullptr;
	}

	VMFunction* implementation = variant.Implementation;
	if ((variant.Flags & VARF_Virtual) != 0 && implementation->VirtualIndex != ~0u &&
		actor->GetClass()->Virtuals.Size() > implementation->VirtualIndex)
	{
		implementation = actor->GetClass()->Virtuals[implementation->VirtualIndex];
	}
	if (implementation == nullptr)
	{
		PyErr_Format(PyExc_RuntimeError, "ZScript method %s.%s has no runtime implementation",
			actor->GetClass()->TypeName.GetChars(), methodName);
		return nullptr;
	}

	PType* returnType = variant.Proto->ReturnTypes.Size() == 0 ? nullptr : variant.Proto->ReturnTypes[0];
	int integerResult = 0;
	double floatResult[4] = {};
	FString stringResult;
	void* pointerResult = nullptr;
	VMReturn result;
	VMReturn* resultPointer = nullptr;
	if (returnType != nullptr)
	{
		result.Location = returnType->GetRegType() == REGT_INT ? static_cast<void*>(&integerResult) :
			returnType->GetRegType() == REGT_STRING ? static_cast<void*>(&stringResult) :
			returnType->GetRegType() == REGT_POINTER ? static_cast<void*>(&pointerResult) :
			static_cast<void*>(floatResult);
		result.RegType = static_cast<VM_UBYTE>(returnType->GetRegType());
		if (returnType->GetRegType() == REGT_FLOAT)
		{
			switch (returnType->GetRegCount())
			{
			case 2: result.RegType |= REGT_MULTIREG2; break;
			case 3: result.RegType |= REGT_MULTIREG3; break;
			case 4: result.RegType |= REGT_MULTIREG4; break;
			default: break;
			}
		}
		resultPointer = &result;
	}

	try
	{
		unsigned fullParameterCount = 0;
		for (PType* argumentType : variant.Proto->ArgumentTypes)
		{
			fullParameterCount += argumentType->GetRegCount();
		}
		if (parameters.Size() < fullParameterCount && implementation->DefaultArgs.Size() < fullParameterCount)
		{
			PyErr_Format(PyExc_TypeError, "%s() cannot omit arguments for this virtual implementation",
				methodName);
			return nullptr;
		}
		VMCallWithDefaults(implementation, parameters, resultPointer, resultPointer == nullptr ? 0 : 1);
	}
	catch (CVMAbortException& error)
	{
		const char* detail = error.GetMessage();
		PyErr_Format(PyExc_RuntimeError, "ZScript method %s.%s aborted%s%s",
			actor->GetClass()->TypeName.GetChars(), methodName,
			detail == nullptr || *detail == 0 ? "" : ": ", detail == nullptr ? "" : detail);
		return nullptr;
	}

	if (returnType == nullptr) Py_RETURN_NONE;
	switch (returnType->GetRegType())
	{
	case REGT_INT: return PyLong_FromLong(integerResult);
	case REGT_STRING: return PyUnicode_FromStringAndSize(stringResult.GetChars(), stringResult.Len());
	case REGT_POINTER:
		return pointerResult == nullptr ? Py_NewRef(Py_None) : MakeActorRef(static_cast<AActor*>(pointerResult));
	case REGT_FLOAT:
		if (returnType->GetRegCount() == 1) return PyFloat_FromDouble(floatResult[0]);
		{
			PyObject* tuple = PyTuple_New(returnType->GetRegCount());
			if (tuple == nullptr) return nullptr;
			for (int index = 0; index < returnType->GetRegCount(); ++index)
			{
				PyObject* component = PyFloat_FromDouble(floatResult[index]);
				if (component == nullptr)
				{
					Py_DECREF(tuple);
					return nullptr;
				}
				PyTuple_SET_ITEM(tuple, index, component);
			}
			return tuple;
		}
	default: break;
	}
	PyErr_SetString(PyExc_RuntimeError, "unreachable ZScript return conversion");
	return nullptr;
}

PyMethodDef ActorMethods[] = {
	{ "snapshot", ActorSnapshotMethod, METH_NOARGS, "Return a serialization-friendly snapshot." },
	{ "set_position", BD_GAME_KEYWORD_FUNCTION(ActorSetPosition), METH_VARARGS | METH_KEYWORDS, "Move immediately, optionally checking collision. Returns True when the actor moved." },
	{ "set_velocity", BD_GAME_KEYWORD_FUNCTION(ActorSetVelocity), METH_VARARGS | METH_KEYWORDS, "Replace or add to velocity." },
	{ "thrust", BD_GAME_KEYWORD_FUNCTION(ActorThrust), METH_VARARGS | METH_KEYWORDS, "Apply horizontal/vertical thrust." },
	{ "damage", BD_GAME_KEYWORD_FUNCTION(ActorDamage), METH_VARARGS | METH_KEYWORDS, "Apply native gameplay damage." },
	{ "heal", BD_GAME_KEYWORD_FUNCTION(ActorHeal), METH_VARARGS | METH_KEYWORDS, "Restore health through the native healing path." },
	{ "destroy", ActorDestroy, METH_NOARGS, "Destroy the actor." },
	{ "distance_to", ActorDistanceTo, METH_VARARGS, "Return 3D distance to another actor." },
	{ "check_sight", BD_GAME_KEYWORD_FUNCTION(ActorCheckSight), METH_VARARGS | METH_KEYWORDS, "Run the native sight check." },
	{ "get_flag", ActorGetFlag, METH_VARARGS, "Read an actor flag by engine name." },
	{ "set_flag", ActorSetFlag, METH_VARARGS, "Change an actor flag by engine name." },
	{ "set_state", BD_GAME_KEYWORD_FUNCTION(ActorSetState), METH_VARARGS | METH_KEYWORDS, "Enter a named actor state." },
	{ "inventory_count", BD_GAME_KEYWORD_FUNCTION(ActorInventoryCount), METH_VARARGS | METH_KEYWORDS, "Return an inventory amount." },
	{ "give_inventory", BD_GAME_KEYWORD_FUNCTION(ActorGiveInventory), METH_VARARGS | METH_KEYWORDS, "Give inventory through the native pickup path." },
	{ "take_inventory", BD_GAME_KEYWORD_FUNCTION(ActorTakeInventory), METH_VARARGS | METH_KEYWORDS, "Take inventory." },
	{ "use_inventory", ActorUseInventory, METH_VARARGS, "Use a named inventory item." },
	{ "clear_inventory", ActorClearInventory, METH_NOARGS, "Remove all inventory." },
	{ "play_sound", BD_GAME_KEYWORD_FUNCTION(ActorPlaySound), METH_VARARGS | METH_KEYWORDS, "Start an actor sound." },
	{ "stop_sound", ActorStopSound, METH_VARARGS, "Stop an actor sound channel." },
	{ "activate", BD_GAME_KEYWORD_FUNCTION(ActorActivate), METH_VARARGS | METH_KEYWORDS, "Activate or deactivate the actor." },
	{ "call_zscript", ActorCallZScript, METH_VARARGS, "Call a public, supported ZScript method on this actor." },
	{ nullptr, nullptr, 0, nullptr },
};

#define ACTOR_SCALAR(name, field, doc) \
	{ name, ActorScalarGet, ActorScalarSet, doc, reinterpret_cast<void*>(static_cast<intptr_t>(ActorScalar::field)) }
#define ACTOR_READONLY_SCALAR(name, field, doc) \
	{ name, ActorScalarGet, nullptr, doc, reinterpret_cast<void*>(static_cast<intptr_t>(ActorScalar::field)) }

PyGetSetDef ActorGetSets[] = {
	{ "valid", ActorValid, nullptr, "Whether this handle still names a live actor.", nullptr },
	{ "class_name", ActorClassName, nullptr, "Runtime actor class name.", nullptr },
	{ "alive", ActorAlive, nullptr, "Whether health is above zero.", nullptr },
	{ "is_player", ActorIsPlayer, nullptr, "Whether this actor owns a player.", nullptr },
	{ "is_monster", ActorIsMonster, nullptr, "Whether this actor is a monster.", nullptr },
	{ "position", ActorPosition, ActorPositionSet, "(x, y, z) position.", nullptr },
	{ "velocity", ActorVelocity, ActorVelocitySet, "(x, y, z) velocity.", nullptr },
	{ "angles", ActorAngles, ActorAnglesSet, "(yaw, pitch, roll) in degrees.", nullptr },
	{ "target", ActorRelationGet, ActorRelationSet, "AI target.", reinterpret_cast<void*>(static_cast<intptr_t>(ActorRelation::Target)) },
	{ "master", ActorRelationGet, ActorRelationSet, "Master/owner actor.", reinterpret_cast<void*>(static_cast<intptr_t>(ActorRelation::Master)) },
	{ "tracer", ActorRelationGet, ActorRelationSet, "Tracer actor.", reinterpret_cast<void*>(static_cast<intptr_t>(ActorRelation::Tracer)) },
	{ "args", ActorArgsGet, ActorArgsSet, "Five actor special arguments.", nullptr },
	ACTOR_SCALAR("tid", Tid, "Thing identifier."),
	ACTOR_SCALAR("health", Health, "Current health."),
	ACTOR_SCALAR("x", X, "World X position."),
	ACTOR_SCALAR("y", Y, "World Y position."),
	ACTOR_SCALAR("z", Z, "World Z position."),
	ACTOR_SCALAR("velocity_x", VelocityX, "X velocity."),
	ACTOR_SCALAR("velocity_y", VelocityY, "Y velocity."),
	ACTOR_SCALAR("velocity_z", VelocityZ, "Z velocity."),
	ACTOR_SCALAR("angle", Angle, "Yaw in degrees."),
	ACTOR_SCALAR("pitch", Pitch, "Pitch in degrees."),
	ACTOR_SCALAR("roll", Roll, "Roll in degrees."),
	ACTOR_SCALAR("radius", Radius, "Collision radius."),
	ACTOR_SCALAR("height", Height, "Collision height."),
	ACTOR_SCALAR("speed", Speed, "Actor speed property."),
	ACTOR_SCALAR("gravity", Gravity, "Actor gravity multiplier."),
	ACTOR_SCALAR("mass", Mass, "Actor mass."),
	ACTOR_SCALAR("alpha", Alpha, "Render alpha."),
	ACTOR_SCALAR("scale_x", ScaleX, "Horizontal render scale."),
	ACTOR_SCALAR("scale_y", ScaleY, "Vertical render scale."),
	ACTOR_SCALAR("tics", Tics, "Remaining state tics."),
	ACTOR_SCALAR("score", Score, "Actor score field."),
	ACTOR_SCALAR("special", Special, "Actor action special."),
	ACTOR_SCALAR("damage_factor", DamageFactor, "Multiplier for damage TAKEN by this actor."),
	ACTOR_SCALAR("damage_multiply", DamageMultiply, "Multiplier for damage DEALT by this actor."),
	{ "tint", ActorTintGet, ActorTintSet, "Sprite tint as an (r, g, b) tuple (0-255), or None when untinted; assign None to reset to the class default.", nullptr },
	ACTOR_READONLY_SCALAR("water_level", WaterLevel, "Water immersion level."),
	ACTOR_READONLY_SCALAR("floor_z", FloorZ, "Current floor clipping height."),
	ACTOR_READONLY_SCALAR("ceiling_z", CeilingZ, "Current ceiling clipping height."),
	{ nullptr, nullptr, nullptr, nullptr, nullptr },
};

#undef ACTOR_SCALAR
#undef ACTOR_READONLY_SCALAR

PyObject* PlayerRepr(PyObject* object)
{
	const int index = reinterpret_cast<PyPlayerRef*>(object)->Index;
	return PyUnicode_FromFormat("<biaseddoom.Player index=%d valid=%s>", index,
		index >= 0 && index < static_cast<int>(MAXPLAYERS) && playeringame[index] ? "True" : "False");
}

player_t* ResolvePlayer(PyPlayerRef* reference, bool mutation, bool requirePawn = false)
{
	if (!CheckApiThread()) return nullptr;
	if (mutation && !CheckGameplayMutation()) return nullptr;
	if (reference->Index < 0 || reference->Index >= static_cast<int>(MAXPLAYERS) || !playeringame[reference->Index])
	{
		PyErr_SetString(PyExc_ReferenceError, "player is no longer in the game");
		return nullptr;
	}
	player_t* player = &players[reference->Index];
	if (requirePawn && !IsUsableActor(player->mo))
	{
		PyErr_SetString(PyExc_ReferenceError, "player currently has no live pawn");
		return nullptr;
	}
	return player;
}

PyObject* PlayerValid(PyObject* object, void*)
{
	const int index = reinterpret_cast<PyPlayerRef*>(object)->Index;
	if (!CheckApiThread()) return nullptr;
	return PyBool_FromLong(index >= 0 && index < static_cast<int>(MAXPLAYERS) && playeringame[index]);
}

PyObject* PlayerIndex(PyObject* object, void*)
{
	return PyLong_FromLong(reinterpret_cast<PyPlayerRef*>(object)->Index);
}

PyObject* PlayerName(PyObject* object, void*)
{
	player_t* player = ResolvePlayer(reinterpret_cast<PyPlayerRef*>(object), false);
	return player == nullptr ? nullptr : PyUnicode_FromString(player->userinfo.GetName());
}

PyObject* PlayerActor(PyObject* object, void*)
{
	player_t* player = ResolvePlayer(reinterpret_cast<PyPlayerRef*>(object), false);
	return player == nullptr ? nullptr : MakeActorRef(IsUsableActor(player->mo) ? player->mo : nullptr);
}

enum class PlayerScalar : intptr_t
{
	Buttons,
	Pitch,
	Yaw,
	Roll,
	Forward,
	Side,
	Up,
	Fov,
	FragCount,
	KillCount,
	ItemCount,
	SecretCount,
};

PyObject* PlayerScalarGet(PyObject* object, void* closure)
{
	player_t* player = ResolvePlayer(reinterpret_cast<PyPlayerRef*>(object), false);
	if (player == nullptr) return nullptr;
	switch (static_cast<PlayerScalar>(reinterpret_cast<intptr_t>(closure)))
	{
	case PlayerScalar::Buttons: return PyLong_FromUnsignedLong(player->cmd.buttons);
	case PlayerScalar::Pitch: return PyLong_FromLong(player->cmd.pitch);
	case PlayerScalar::Yaw: return PyLong_FromLong(player->cmd.yaw);
	case PlayerScalar::Roll: return PyLong_FromLong(player->cmd.roll);
	case PlayerScalar::Forward: return PyLong_FromLong(player->cmd.forwardmove);
	case PlayerScalar::Side: return PyLong_FromLong(player->cmd.sidemove);
	case PlayerScalar::Up: return PyLong_FromLong(player->cmd.upmove);
	case PlayerScalar::Fov: return PyFloat_FromDouble(player->DesiredFOV);
	case PlayerScalar::FragCount: return PyLong_FromLong(player->fragcount);
	case PlayerScalar::KillCount: return PyLong_FromLong(player->killcount);
	case PlayerScalar::ItemCount: return PyLong_FromLong(player->itemcount);
	case PlayerScalar::SecretCount: return PyLong_FromLong(player->secretcount);
	}
	Py_RETURN_NONE;
}

int PlayerScalarSet(PyObject* object, PyObject* value, void* closure)
{
	if (value == nullptr)
	{
		PyErr_SetString(PyExc_TypeError, "player properties cannot be deleted");
		return -1;
	}
	player_t* player = ResolvePlayer(reinterpret_cast<PyPlayerRef*>(object), true);
	if (player == nullptr) return -1;
	const PlayerScalar field = static_cast<PlayerScalar>(reinterpret_cast<intptr_t>(closure));
	if (field == PlayerScalar::Fov)
	{
		player->DesiredFOV = static_cast<float>(PyFloat_AsDouble(value));
		return PyErr_Occurred() ? -1 : 0;
	}
	const long number = PyLong_AsLong(value);
	if (PyErr_Occurred()) return -1;
	switch (field)
	{
	case PlayerScalar::Buttons: player->cmd.buttons = static_cast<uint32_t>(number); break;
	case PlayerScalar::Pitch: player->cmd.pitch = static_cast<short>(number); break;
	case PlayerScalar::Yaw: player->cmd.yaw = static_cast<short>(number); break;
	case PlayerScalar::Roll: player->cmd.roll = static_cast<short>(number); break;
	case PlayerScalar::Forward: player->cmd.forwardmove = static_cast<short>(number); break;
	case PlayerScalar::Side: player->cmd.sidemove = static_cast<short>(number); break;
	case PlayerScalar::Up: player->cmd.upmove = static_cast<short>(number); break;
	case PlayerScalar::FragCount: player->fragcount = static_cast<int>(number); break;
	case PlayerScalar::KillCount: player->killcount = static_cast<int>(number); break;
	case PlayerScalar::ItemCount: player->itemcount = static_cast<int>(number); break;
	case PlayerScalar::SecretCount: player->secretcount = static_cast<int>(number); break;
	case PlayerScalar::Fov: break;
	}
	return 0;
}

PyObject* PlayerSetInput(PyObject* object, PyObject* args, PyObject* kwargs)
{
	PyObject* buttons = Py_None;
	PyObject* forward = Py_None;
	PyObject* side = Py_None;
	PyObject* up = Py_None;
	PyObject* yaw = Py_None;
	PyObject* pitch = Py_None;
	static const char* keywords[] = { "buttons", "forward", "side", "up", "yaw", "pitch", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OOOOOO:set_input", const_cast<char**>(keywords),
		&buttons, &forward, &side, &up, &yaw, &pitch)) return nullptr;
	player_t* player = ResolvePlayer(reinterpret_cast<PyPlayerRef*>(object), true);
	if (player == nullptr) return nullptr;
	auto setShort = [](PyObject* value, short& destination)
	{
		if (value == Py_None) return true;
		const long parsed = PyLong_AsLong(value);
		if (PyErr_Occurred()) return false;
		destination = static_cast<short>(std::clamp<long>(parsed, -32768, 32767));
		return true;
	};
	if (buttons != Py_None)
	{
		const unsigned long parsed = PyLong_AsUnsignedLong(buttons);
		if (PyErr_Occurred()) return nullptr;
		player->cmd.buttons = static_cast<uint32_t>(parsed);
	}
	if (!setShort(forward, player->cmd.forwardmove) || !setShort(side, player->cmd.sidemove) ||
		!setShort(up, player->cmd.upmove) || !setShort(yaw, player->cmd.yaw) || !setShort(pitch, player->cmd.pitch)) return nullptr;
	Py_RETURN_NONE;
}

PyObject* PlayerSetWeapon(PyObject* object, PyObject* args)
{
	const char* className = nullptr;
	if (!PyArg_ParseTuple(args, "s:set_weapon", &className)) return nullptr;
	player_t* player = ResolvePlayer(reinterpret_cast<PyPlayerRef*>(object), true, true);
	if (player == nullptr) return nullptr;
	PClassActor* weapon = PClass::FindActor(FName(className));
	if (weapon == nullptr)
	{
		PyErr_Format(PyExc_ValueError, "unknown weapon class '%s'", className);
		return nullptr;
	}
	const int result = ScriptUtil::Exec(NAME_SetWeapon, ScriptUtil::Pointer, player->mo,
		ScriptUtil::Class, weapon, ScriptUtil::End);
	return PyBool_FromLong(result);
}

PyMethodDef PlayerMethods[] = {
	{ "set_input", BD_GAME_KEYWORD_FUNCTION(PlayerSetInput), METH_VARARGS | METH_KEYWORDS, "Override the current native user command." },
	{ "set_weapon", PlayerSetWeapon, METH_VARARGS, "Switch to an owned weapon class." },
	{ nullptr, nullptr, 0, nullptr },
};

#define PLAYER_SCALAR(name, field, doc) \
	{ name, PlayerScalarGet, PlayerScalarSet, doc, reinterpret_cast<void*>(static_cast<intptr_t>(PlayerScalar::field)) }
PyGetSetDef PlayerGetSets[] = {
	{ "valid", PlayerValid, nullptr, "Whether the player remains in game.", nullptr },
	{ "index", PlayerIndex, nullptr, "Player slot index.", nullptr },
	{ "name", PlayerName, nullptr, "User-visible player name.", nullptr },
	{ "actor", PlayerActor, nullptr, "Current pawn Actor or None.", nullptr },
	PLAYER_SCALAR("buttons", Buttons, "Current input button mask."),
	PLAYER_SCALAR("input_pitch", Pitch, "Current pitch command."),
	PLAYER_SCALAR("input_yaw", Yaw, "Current yaw command."),
	PLAYER_SCALAR("input_roll", Roll, "Current roll command."),
	PLAYER_SCALAR("forward_move", Forward, "Current forward movement command."),
	PLAYER_SCALAR("side_move", Side, "Current side movement command."),
	PLAYER_SCALAR("up_move", Up, "Current vertical movement command."),
	PLAYER_SCALAR("fov", Fov, "Desired field of view."),
	PLAYER_SCALAR("frag_count", FragCount, "Current frag count."),
	PLAYER_SCALAR("kill_count", KillCount, "Current kill count."),
	PLAYER_SCALAR("item_count", ItemCount, "Current item count."),
	PLAYER_SCALAR("secret_count", SecretCount, "Current secret count."),
	{ nullptr, nullptr, nullptr, nullptr, nullptr },
};
#undef PLAYER_SCALAR

sector_t* ResolveSector(PyWorldRef* reference, bool mutation)
{
	if (!CheckApiThread()) return nullptr;
	if (mutation && !CheckGameplayMutation()) return nullptr;
	if (primaryLevel == nullptr || reference->Generation != worldGeneration || reference->Index < 0 ||
		static_cast<unsigned>(reference->Index) >= primaryLevel->sectors.Size())
	{
		PyErr_SetString(PyExc_ReferenceError, "sector belongs to an unloaded map");
		return nullptr;
	}
	return &primaryLevel->sectors[reference->Index];
}

line_t* ResolveLine(PyWorldRef* reference, bool mutation)
{
	if (!CheckApiThread()) return nullptr;
	if (mutation && !CheckGameplayMutation()) return nullptr;
	if (primaryLevel == nullptr || reference->Generation != worldGeneration || reference->Index < 0 ||
		static_cast<unsigned>(reference->Index) >= primaryLevel->lines.Size())
	{
		PyErr_SetString(PyExc_ReferenceError, "line belongs to an unloaded map");
		return nullptr;
	}
	return &primaryLevel->lines[reference->Index];
}

PyObject* MakeWorldRef(PyTypeObject* type, int index)
{
	PyWorldRef* reference = PyObject_New(PyWorldRef, type);
	if (reference == nullptr) return nullptr;
	reference->Generation = worldGeneration;
	reference->Index = index;
	return reinterpret_cast<PyObject*>(reference);
}

PyObject* SectorRepr(PyObject* object)
{
	PyWorldRef* reference = reinterpret_cast<PyWorldRef*>(object);
	return PyUnicode_FromFormat("<biaseddoom.Sector index=%d generation=%u>", reference->Index, reference->Generation);
}

PyObject* LineRepr(PyObject* object)
{
	PyWorldRef* reference = reinterpret_cast<PyWorldRef*>(object);
	return PyUnicode_FromFormat("<biaseddoom.Line index=%d generation=%u>", reference->Index, reference->Generation);
}

PyObject* WorldIndex(PyObject* object, void*)
{
	return PyLong_FromLong(reinterpret_cast<PyWorldRef*>(object)->Index);
}

enum class SectorScalar : intptr_t
{
	Light,
	Gravity,
	Special,
	Damage,
	DamageInterval,
	Leakiness,
	FloorHeight,
	CeilingHeight,
};

PyObject* SectorScalarGet(PyObject* object, void* closure)
{
	sector_t* sector = ResolveSector(reinterpret_cast<PyWorldRef*>(object), false);
	if (sector == nullptr) return nullptr;
	switch (static_cast<SectorScalar>(reinterpret_cast<intptr_t>(closure)))
	{
	case SectorScalar::Light: return PyLong_FromLong(sector->GetLightLevel());
	case SectorScalar::Gravity: return PyFloat_FromDouble(sector->gravity);
	case SectorScalar::Special: return PyLong_FromLong(sector->special);
	case SectorScalar::Damage: return PyLong_FromLong(sector->damageamount);
	case SectorScalar::DamageInterval: return PyLong_FromLong(sector->damageinterval);
	case SectorScalar::Leakiness: return PyLong_FromLong(sector->leakydamage);
	case SectorScalar::FloorHeight: return PyFloat_FromDouble(sector->CenterFloor());
	case SectorScalar::CeilingHeight: return PyFloat_FromDouble(sector->CenterCeiling());
	}
	Py_RETURN_NONE;
}

int SectorScalarSet(PyObject* object, PyObject* value, void* closure)
{
	if (value == nullptr)
	{
		PyErr_SetString(PyExc_TypeError, "sector properties cannot be deleted");
		return -1;
	}
	sector_t* sector = ResolveSector(reinterpret_cast<PyWorldRef*>(object), true);
	if (sector == nullptr) return -1;
	const SectorScalar field = static_cast<SectorScalar>(reinterpret_cast<intptr_t>(closure));
	if (field == SectorScalar::FloorHeight || field == SectorScalar::CeilingHeight)
	{
		PyErr_SetString(PyExc_AttributeError, "use move_floor() or move_ceiling() for plane heights");
		return -1;
	}
	if (field == SectorScalar::Gravity)
	{
		sector->gravity = PyFloat_AsDouble(value);
		return PyErr_Occurred() ? -1 : 0;
	}
	const long number = PyLong_AsLong(value);
	if (PyErr_Occurred()) return -1;
	switch (field)
	{
	case SectorScalar::Light: sector->SetLightLevel(static_cast<int>(number)); break;
	case SectorScalar::Special: sector->special = static_cast<int>(number); break;
	case SectorScalar::Damage: sector->damageamount = static_cast<int>(number); break;
	case SectorScalar::DamageInterval: sector->damageinterval = static_cast<short>(number); break;
	case SectorScalar::Leakiness: sector->leakydamage = static_cast<short>(number); break;
	default: break;
	}
	return 0;
}

PyObject* SectorTags(PyObject* object, void*)
{
	sector_t* sector = ResolveSector(reinterpret_cast<PyWorldRef*>(object), false);
	if (sector == nullptr) return nullptr;
	PyObject* result = PyList_New(0);
	const int count = primaryLevel->tagManager.CountSectorTags(sector);
	for (int index = 0; index < count; ++index)
	{
		PyObject* tag = PyLong_FromLong(primaryLevel->tagManager.GetSectorTag(sector, index));
		PyList_Append(result, tag);
		Py_DECREF(tag);
	}
	return result;
}

PyObject* SectorMovePlane(PyObject* object, PyObject* args, PyObject* kwargs, bool ceiling)
{
	double height;
	double speed = 0;
	int crush = -1;
	static const char* keywords[] = { "height", "speed", "crush", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "d|di:move_plane", const_cast<char**>(keywords), &height, &speed, &crush)) return nullptr;
	sector_t* sector = ResolveSector(reinterpret_cast<PyWorldRef*>(object), true);
	if (sector == nullptr) return nullptr;
	const double current = ceiling ? sector->CenterCeiling() : sector->CenterFloor();
	if (height == current) return PyLong_FromLong(0);
	const int direction = height > current ? 1 : -1;
	const double effectiveSpeed = speed <= 0 ? std::abs(height - current) : speed;
	const bool instant = speed <= 0;
	const double destination = (ceiling ? sector->ceilingplane : sector->floorplane).PointToDist(sector->centerspot, height);
	const EMoveResult result = ceiling
		? sector->MoveCeiling(effectiveSpeed, destination, crush, direction, false)
		: sector->MoveFloor(effectiveSpeed, destination, crush, direction, false, instant);
	return PyLong_FromLong(static_cast<int>(result));
}

PyObject* SectorMoveFloor(PyObject* object, PyObject* args, PyObject* kwargs)
{
	return SectorMovePlane(object, args, kwargs, false);
}

PyObject* SectorMoveCeiling(PyObject* object, PyObject* args, PyObject* kwargs)
{
	return SectorMovePlane(object, args, kwargs, true);
}

PyMethodDef SectorMethods[] = {
	{ "move_floor", BD_GAME_KEYWORD_FUNCTION(SectorMoveFloor), METH_VARARGS | METH_KEYWORDS, "Move the floor toward an absolute height." },
	{ "move_ceiling", BD_GAME_KEYWORD_FUNCTION(SectorMoveCeiling), METH_VARARGS | METH_KEYWORDS, "Move the ceiling toward an absolute height." },
	{ nullptr, nullptr, 0, nullptr },
};

#define SECTOR_SCALAR(name, field, doc) \
	{ name, SectorScalarGet, SectorScalarSet, doc, reinterpret_cast<void*>(static_cast<intptr_t>(SectorScalar::field)) }
#define SECTOR_READONLY(name, field, doc) \
	{ name, SectorScalarGet, nullptr, doc, reinterpret_cast<void*>(static_cast<intptr_t>(SectorScalar::field)) }
PyGetSetDef SectorGetSets[] = {
	{ "index", WorldIndex, nullptr, "Sector array index.", nullptr },
	{ "tags", SectorTags, nullptr, "All sector tags.", nullptr },
	SECTOR_SCALAR("light", Light, "Sector light level."),
	SECTOR_SCALAR("gravity", Gravity, "Sector gravity multiplier."),
	SECTOR_SCALAR("special", Special, "Sector special."),
	SECTOR_SCALAR("damage", Damage, "Periodic damage amount."),
	SECTOR_SCALAR("damage_interval", DamageInterval, "Periodic damage interval."),
	SECTOR_SCALAR("leakiness", Leakiness, "Suit leak probability."),
	SECTOR_READONLY("floor_height", FloorHeight, "Floor height at sector center."),
	SECTOR_READONLY("ceiling_height", CeilingHeight, "Ceiling height at sector center."),
	{ nullptr, nullptr, nullptr, nullptr, nullptr },
};
#undef SECTOR_SCALAR
#undef SECTOR_READONLY

enum class LineScalar : intptr_t
{
	Special,
	Flags,
	Activation,
	Alpha,
	Health,
};

PyObject* LineScalarGet(PyObject* object, void* closure)
{
	line_t* line = ResolveLine(reinterpret_cast<PyWorldRef*>(object), false);
	if (line == nullptr) return nullptr;
	switch (static_cast<LineScalar>(reinterpret_cast<intptr_t>(closure)))
	{
	case LineScalar::Special: return PyLong_FromLong(line->special);
	case LineScalar::Flags: return PyLong_FromUnsignedLong(line->flags);
	case LineScalar::Activation: return PyLong_FromUnsignedLong(line->activation);
	case LineScalar::Alpha: return PyFloat_FromDouble(line->alpha);
	case LineScalar::Health: return PyLong_FromLong(line->health);
	}
	Py_RETURN_NONE;
}

int LineScalarSet(PyObject* object, PyObject* value, void* closure)
{
	if (value == nullptr)
	{
		PyErr_SetString(PyExc_TypeError, "line properties cannot be deleted");
		return -1;
	}
	line_t* line = ResolveLine(reinterpret_cast<PyWorldRef*>(object), true);
	if (line == nullptr) return -1;
	const LineScalar field = static_cast<LineScalar>(reinterpret_cast<intptr_t>(closure));
	if (field == LineScalar::Alpha)
	{
		line->setAlpha(std::clamp(PyFloat_AsDouble(value), 0.0, 1.0));
		return PyErr_Occurred() ? -1 : 0;
	}
	const unsigned long number = PyLong_AsUnsignedLong(value);
	if (PyErr_Occurred()) return -1;
	switch (field)
	{
	case LineScalar::Special: line->special = static_cast<int>(number); break;
	case LineScalar::Flags: line->flags = static_cast<uint32_t>(number); break;
	case LineScalar::Activation: line->activation = static_cast<uint32_t>(number); break;
	case LineScalar::Health: line->health = static_cast<int>(number); break;
	case LineScalar::Alpha: break;
	}
	return 0;
}

PyObject* LineArgsGet(PyObject* object, void*)
{
	line_t* line = ResolveLine(reinterpret_cast<PyWorldRef*>(object), false);
	if (line == nullptr) return nullptr;
	return Py_BuildValue("(iiiii)", line->args[0], line->args[1], line->args[2], line->args[3], line->args[4]);
}

int LineArgsSet(PyObject* object, PyObject* value, void*)
{
	PyObject* sequence = PySequence_Fast(value, "args must contain exactly five integers");
	if (sequence == nullptr) return -1;
	if (PySequence_Fast_GET_SIZE(sequence) != 5)
	{
		Py_DECREF(sequence);
		PyErr_SetString(PyExc_ValueError, "args must contain exactly five integers");
		return -1;
	}
	int parsed[5];
	for (int index = 0; index < 5; ++index)
	{
		parsed[index] = static_cast<int>(PyLong_AsLong(PySequence_Fast_GET_ITEM(sequence, index)));
		if (PyErr_Occurred())
		{
			Py_DECREF(sequence);
			return -1;
		}
	}
	Py_DECREF(sequence);
	line_t* line = ResolveLine(reinterpret_cast<PyWorldRef*>(object), true);
	if (line == nullptr) return -1;
	std::copy(std::begin(parsed), std::end(parsed), std::begin(line->args));
	return 0;
}

PyObject* LineSectorGet(PyObject* object, void* closure)
{
	line_t* line = ResolveLine(reinterpret_cast<PyWorldRef*>(object), false);
	if (line == nullptr) return nullptr;
	sector_t* sector = reinterpret_cast<intptr_t>(closure) == 0 ? line->frontsector : line->backsector;
	if (sector == nullptr) Py_RETURN_NONE;
	return MakeWorldRef(sectorRefType, sector->Index());
}

PyObject* LineActivate(PyObject* object, PyObject* args, PyObject* kwargs)
{
	PyObject* activatorObject = Py_None;
	int backSide = 0;
	int clear = 0;
	static const char* keywords[] = { "activator", "back_side", "clear", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|Opp:activate", const_cast<char**>(keywords),
		&activatorObject, &backSide, &clear)) return nullptr;
	line_t* line = ResolveLine(reinterpret_cast<PyWorldRef*>(object), true);
	if (line == nullptr) return nullptr;
	AActor* activator = ResolveActorArgument(activatorObject, "activator", true, true);
	if (activator == nullptr && activatorObject != Py_None) return nullptr;
	const int result = P_ExecuteSpecial(primaryLevel, line->special, line, activator, backSide != 0,
		line->args[0], line->args[1], line->args[2], line->args[3], line->args[4]);
	if (clear && result) line->special = 0;
	return PyLong_FromLong(result);
}

PyMethodDef LineMethods[] = {
	{ "activate", BD_GAME_KEYWORD_FUNCTION(LineActivate), METH_VARARGS | METH_KEYWORDS, "Execute this line's action special." },
	{ nullptr, nullptr, 0, nullptr },
};

#define LINE_SCALAR(name, field, doc) \
	{ name, LineScalarGet, LineScalarSet, doc, reinterpret_cast<void*>(static_cast<intptr_t>(LineScalar::field)) }
PyGetSetDef LineGetSets[] = {
	{ "index", WorldIndex, nullptr, "Line array index.", nullptr },
	{ "front_sector", LineSectorGet, nullptr, "Front Sector.", reinterpret_cast<void*>(0) },
	{ "back_sector", LineSectorGet, nullptr, "Back Sector or None.", reinterpret_cast<void*>(1) },
	{ "args", LineArgsGet, LineArgsSet, "Five action-special arguments.", nullptr },
	LINE_SCALAR("special", Special, "Line action special."),
	LINE_SCALAR("flags", Flags, "Line flags bitmask."),
	LINE_SCALAR("activation", Activation, "Activation type bitmask."),
	LINE_SCALAR("alpha", Alpha, "Line translucency."),
	LINE_SCALAR("health", Health, "Destructible line health."),
	{ nullptr, nullptr, nullptr, nullptr, nullptr },
};
#undef LINE_SCALAR

PyObject* PyActorRefByTid(PyObject*, PyObject* args)
{
	int tid;
	if (!PyArg_ParseTuple(args, "i:actor_ref", &tid)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	if (primaryLevel == nullptr || tid == 0) Py_RETURN_NONE;
	return MakeActorRef(primaryLevel->GetActorIterator(tid).Next());
}

PyObject* PyActorRefs(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* className = nullptr;
	int tid = 0;
	int limit = 4096;
	int subclasses = 1;
	PyObject* sphereObject = Py_None;
	PyObject* zObject = Py_None;
	static const char* keywords[] = { "class_name", "tid", "limit", "subclasses", "sphere", "z", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|ziipOO:actor_refs", const_cast<char**>(keywords),
		&className, &tid, &limit, &subclasses, &sphereObject, &zObject)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	if (limit < 0 || limit > 1000000)
	{
		PyErr_SetString(PyExc_ValueError, "limit must be between 0 and 1000000");
		return nullptr;
	}
	PClassActor* filter = nullptr;
	if (className != nullptr)
	{
		filter = PClass::FindActor(FName(className));
		if (filter == nullptr)
		{
			PyErr_Format(PyExc_ValueError, "unknown actor class '%s'", className);
			return nullptr;
		}
	}
	// Push-down spatial filters: evaluated in C++ during the single thinker
	// scan, so no per-actor Python crossings happen while filtering.
	bool useSphere = sphereObject != Py_None;
	double sphereX = 0, sphereY = 0, sphereRadius = 0;
	if (useSphere)
	{
		PyObject* sphere = PySequence_Fast(sphereObject, "sphere must be an (x, y, radius) sequence");
		if (sphere == nullptr) return nullptr;
		if (PySequence_Fast_GET_SIZE(sphere) != 3)
		{
			Py_DECREF(sphere);
			PyErr_SetString(PyExc_ValueError, "sphere must contain exactly three numbers: x, y, radius");
			return nullptr;
		}
		sphereX = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sphere, 0));
		sphereY = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sphere, 1));
		sphereRadius = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sphere, 2));
		const bool valid = !PyErr_Occurred();
		Py_DECREF(sphere);
		if (!valid) return nullptr;
		if (sphereRadius < 0)
		{
			PyErr_SetString(PyExc_ValueError, "sphere radius must not be negative");
			return nullptr;
		}
	}
	bool useZ = zObject != Py_None;
	double sphereZ = 0;
	if (useZ)
	{
		sphereZ = PyFloat_AsDouble(zObject);
		if (PyErr_Occurred()) return nullptr;
		if (!useSphere)
		{
			PyErr_SetString(PyExc_ValueError, "z requires the sphere filter");
			return nullptr;
		}
	}
	const double radiusSquared = sphereRadius * sphereRadius;
	auto matches = [&](AActor* actor)
	{
		if (filter != nullptr)
		{
			if (subclasses != 0)
			{
				if (!actor->IsKindOf(filter)) return false;
			}
			else if (actor->GetClass() != filter) return false;
		}
		if (useSphere)
		{
			const double dx = actor->X() - sphereX;
			const double dy = actor->Y() - sphereY;
			if (dx * dx + dy * dy > radiusSquared) return false;
			if (useZ && std::abs(actor->Z() - sphereZ) > sphereRadius) return false;
		}
		return true;
	};
	PyObject* result = PyList_New(0);
	if (result == nullptr || primaryLevel == nullptr) return result;
	if (tid != 0)
	{
		auto iterator = primaryLevel->GetActorIterator(tid);
		AActor* actor = nullptr;
		while (PyList_GET_SIZE(result) < limit && (actor = iterator.Next()) != nullptr)
		{
			if (!matches(actor)) continue;
			PyObject* reference = MakeActorRef(actor);
			if (reference == nullptr || PyList_Append(result, reference) < 0)
			{
				Py_XDECREF(reference);
				Py_DECREF(result);
				return nullptr;
			}
			Py_DECREF(reference);
		}
	}
	else
	{
		auto iterator = primaryLevel->GetThinkerIterator<AActor>();
		AActor* actor = nullptr;
		while (PyList_GET_SIZE(result) < limit && (actor = iterator.Next()) != nullptr)
		{
			if (!matches(actor)) continue;
			PyObject* reference = MakeActorRef(actor);
			if (reference == nullptr || PyList_Append(result, reference) < 0)
			{
				Py_XDECREF(reference);
				Py_DECREF(result);
				return nullptr;
			}
			Py_DECREF(reference);
		}
	}
	return result;
}

PyObject* PySpawnActorRef(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* className = nullptr;
	double x, y, z;
	double angle = 0;
	int tid = 0;
	int force = 0;
	static const char* keywords[] = { "class_name", "x", "y", "z", "angle", "tid", "force", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sddd|dii:spawn", const_cast<char**>(keywords),
		&className, &x, &y, &z, &angle, &tid, &force)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
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
	if (tid != 0) actor->SetTID(tid);
	return MakeActorRef(actor);
}

PyObject* PyPlayer(PyObject*, PyObject* args)
{
	int index = consoleplayer;
	if (!PyArg_ParseTuple(args, "|i:player", &index)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	if (index < 0 || index >= static_cast<int>(MAXPLAYERS) || !playeringame[index]) Py_RETURN_NONE;
	return MakePlayerRef(index);
}

PyObject* PyPlayerRefs(PyObject*, PyObject*)
{
	if (!CheckApiThread()) return nullptr;
	PyObject* result = PyList_New(0);
	if (result == nullptr) return nullptr;
	for (int index = 0; index < static_cast<int>(MAXPLAYERS); ++index)
	{
		if (!playeringame[index]) continue;
		PyObject* reference = MakePlayerRef(index);
		if (reference == nullptr || PyList_Append(result, reference) < 0)
		{
			Py_XDECREF(reference);
			Py_DECREF(result);
			return nullptr;
		}
		Py_DECREF(reference);
	}
	return result;
}

PyObject* PySector(PyObject*, PyObject* args)
{
	int index;
	if (!PyArg_ParseTuple(args, "i:sector", &index)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	if (primaryLevel == nullptr || index < 0 || static_cast<unsigned>(index) >= primaryLevel->sectors.Size()) Py_RETURN_NONE;
	return MakeWorldRef(sectorRefType, index);
}

PyObject* PySectors(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* tagObject = Py_None;
	static const char* keywords[] = { "tag", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O:sectors", const_cast<char**>(keywords), &tagObject)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	PyObject* result = PyList_New(0);
	if (result == nullptr || primaryLevel == nullptr) return result;
	if (tagObject == Py_None)
	{
		for (unsigned index = 0; index < primaryLevel->sectors.Size(); ++index)
		{
			PyObject* reference = MakeWorldRef(sectorRefType, static_cast<int>(index));
			PyList_Append(result, reference);
			Py_DECREF(reference);
		}
	}
	else
	{
		const long tag = PyLong_AsLong(tagObject);
		if (PyErr_Occurred()) { Py_DECREF(result); return nullptr; }
		auto iterator = primaryLevel->GetSectorTagIterator(static_cast<int>(tag));
		int index;
		while ((index = iterator.Next()) >= 0)
		{
			PyObject* reference = MakeWorldRef(sectorRefType, index);
			PyList_Append(result, reference);
			Py_DECREF(reference);
		}
	}
	return result;
}

PyObject* PySectorAt(PyObject*, PyObject* args)
{
	double x, y;
	if (!PyArg_ParseTuple(args, "dd:sector_at", &x, &y)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	if (primaryLevel == nullptr || primaryLevel->sectors.Size() == 0) Py_RETURN_NONE;
	sector_t* sector = primaryLevel->PointInSector(x, y);
	if (sector == nullptr) Py_RETURN_NONE;
	return MakeWorldRef(sectorRefType, sector->Index());
}

PyObject* PyActorsInSector(PyObject*, PyObject* args)
{
	PyObject* sectorObject = nullptr;
	if (!PyArg_ParseTuple(args, "O:actors_in_sector", &sectorObject)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	int sectorIndex = -1;
	if (sectorRefType != nullptr && PyObject_TypeCheck(sectorObject, sectorRefType))
	{
		sector_t* sector = ResolveSector(reinterpret_cast<PyWorldRef*>(sectorObject), false);
		if (sector == nullptr) return nullptr;
		sectorIndex = sector->Index();
	}
	else if (PyLong_Check(sectorObject))
	{
		const long index = PyLong_AsLong(sectorObject);
		if (PyErr_Occurred()) return nullptr;
		if (primaryLevel == nullptr || index < 0 || static_cast<unsigned>(index) >= primaryLevel->sectors.Size())
		{
			PyErr_Format(PyExc_ValueError, "sector index %ld is out of range", index);
			return nullptr;
		}
		sectorIndex = static_cast<int>(index);
	}
	else
	{
		PyErr_SetString(PyExc_TypeError, "sector must be a Sector handle or a sector index");
		return nullptr;
	}
	sector_t* target = &primaryLevel->sectors[sectorIndex];
	PyObject* result = PyList_New(0);
	if (result == nullptr) return nullptr;
	auto iterator = primaryLevel->GetThinkerIterator<AActor>();
	AActor* actor = nullptr;
	while ((actor = iterator.Next()) != nullptr)
	{
		if (actor->Sector != target) continue;
		PyObject* reference = MakeActorRef(actor);
		if (reference == nullptr || PyList_Append(result, reference) < 0)
		{
			Py_XDECREF(reference);
			Py_DECREF(result);
			return nullptr;
		}
		Py_DECREF(reference);
	}
	return result;
}

PyObject* PyLine(PyObject*, PyObject* args)
{
	int index;
	if (!PyArg_ParseTuple(args, "i:line", &index)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	if (primaryLevel == nullptr || index < 0 || static_cast<unsigned>(index) >= primaryLevel->lines.Size()) Py_RETURN_NONE;
	return MakeWorldRef(lineRefType, index);
}

PyObject* PyLines(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* idObject = Py_None;
	static const char* keywords[] = { "line_id", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O:lines", const_cast<char**>(keywords), &idObject)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	PyObject* result = PyList_New(0);
	if (result == nullptr || primaryLevel == nullptr) return result;
	if (idObject == Py_None)
	{
		for (unsigned index = 0; index < primaryLevel->lines.Size(); ++index)
		{
			PyObject* reference = MakeWorldRef(lineRefType, static_cast<int>(index));
			PyList_Append(result, reference);
			Py_DECREF(reference);
		}
	}
	else
	{
		const long lineId = PyLong_AsLong(idObject);
		if (PyErr_Occurred()) { Py_DECREF(result); return nullptr; }
		auto iterator = primaryLevel->GetLineIdIterator(static_cast<int>(lineId));
		int index;
		while ((index = iterator.Next()) >= 0)
		{
			PyObject* reference = MakeWorldRef(lineRefType, index);
			PyList_Append(result, reference);
			Py_DECREF(reference);
		}
	}
	return result;
}

PyObject* PyExecuteSpecial(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* specialObject = nullptr;
	PyObject* argumentsObject = Py_None;
	PyObject* activatorObject = Py_None;
	PyObject* lineObject = Py_None;
	int backSide = 0;
	static const char* keywords[] = { "special", "arguments", "activator", "line", "back_side", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|OOOp:execute_special", const_cast<char**>(keywords),
		&specialObject, &argumentsObject, &activatorObject, &lineObject, &backSide)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	int special = 0;
	int minimum = 0;
	int maximum = 5;
	if (PyLong_Check(specialObject))
	{
		special = static_cast<int>(PyLong_AsLong(specialObject));
		if (PyErr_Occurred()) return nullptr;
	}
	else if (PyUnicode_Check(specialObject))
	{
		const char* name = PyUnicode_AsUTF8(specialObject);
		if (name == nullptr) return nullptr;
		special = P_FindLineSpecial(name, &minimum, &maximum);
		if (special == 0)
		{
			PyErr_Format(PyExc_ValueError, "unknown action special '%s'", name);
			return nullptr;
		}
	}
	else
	{
		PyErr_SetString(PyExc_TypeError, "special must be an integer or action-special name");
		return nullptr;
	}
	int values[5] = { 0, 0, 0, 0, 0 };
	int count = 0;
	if (argumentsObject != Py_None)
	{
		PyObject* sequence = PySequence_Fast(argumentsObject, "arguments must be a sequence of at most five integers");
		if (sequence == nullptr) return nullptr;
		count = static_cast<int>(PySequence_Fast_GET_SIZE(sequence));
		if (count > 5)
		{
			Py_DECREF(sequence);
			PyErr_SetString(PyExc_ValueError, "action specials accept at most five arguments");
			return nullptr;
		}
		for (int index = 0; index < count; ++index)
		{
			values[index] = static_cast<int>(PyLong_AsLong(PySequence_Fast_GET_ITEM(sequence, index)));
			if (PyErr_Occurred()) { Py_DECREF(sequence); return nullptr; }
		}
		Py_DECREF(sequence);
	}
	if (PyUnicode_Check(specialObject) && (count < minimum || count > maximum))
	{
		PyErr_Format(PyExc_ValueError, "action special requires %d..%d arguments, received %d", minimum, maximum, count);
		return nullptr;
	}
	AActor* activator = ResolveActorArgument(activatorObject, "activator", true, true);
	if (activator == nullptr && activatorObject != Py_None) return nullptr;
	line_t* line = nullptr;
	if (lineObject != Py_None)
	{
		if (lineRefType == nullptr || !PyObject_TypeCheck(lineObject, lineRefType))
		{
			PyErr_SetString(PyExc_TypeError, "line must be a Line or None");
			return nullptr;
		}
		line = ResolveLine(reinterpret_cast<PyWorldRef*>(lineObject), true);
		if (line == nullptr) return nullptr;
	}
	return PyLong_FromLong(P_ExecuteSpecial(primaryLevel, special, line, activator, backSide != 0,
		values[0], values[1], values[2], values[3], values[4]));
}

PyObject* PyRadiusDamage(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* spotObject = nullptr;
	int damage;
	double distance;
	PyObject* sourceObject = Py_None;
	const char* damageType = "Explosion";
	int hurtSource = 1;
	static const char* keywords[] = { "spot", "damage", "distance", "source", "damage_type", "hurt_source", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oid|Osp:radius_damage", const_cast<char**>(keywords),
		&spotObject, &damage, &distance, &sourceObject, &damageType, &hurtSource)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	AActor* spot = ResolveActorArgument(spotObject, "spot", false, true);
	if (spot == nullptr) return nullptr;
	AActor* source = ResolveActorArgument(sourceObject, "source", true, true);
	if (source == nullptr && sourceObject != Py_None) return nullptr;
	P_RadiusAttack(spot, source, damage, distance, FName(damageType),
		hurtSource ? RADF_HURTSOURCE : 0);
	Py_RETURN_NONE;
}

PyObject* PyApplyActorBatch(PyObject*, PyObject* args)
{
	PyObject* operationsObject = nullptr;
	if (!PyArg_ParseTuple(args, "O:apply_actor_batch", &operationsObject)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	PyObject* operations = PySequence_Fast(operationsObject, "operations must be a sequence");
	if (operations == nullptr) return nullptr;
	const Py_ssize_t operationCount = PySequence_Fast_GET_SIZE(operations);
	Py_ssize_t applied = 0;
	Py_ssize_t skipped = 0;
	for (Py_ssize_t index = 0; index < operationCount; ++index)
	{
		PyObject* operation = PySequence_Fast(PySequence_Fast_GET_ITEM(operations, index),
			"each batch operation must be a sequence");
		if (operation == nullptr) { Py_DECREF(operations); return nullptr; }
		const Py_ssize_t size = PySequence_Fast_GET_SIZE(operation);
		if (size < 2 || !PyUnicode_Check(PySequence_Fast_GET_ITEM(operation, 0)))
		{
			Py_DECREF(operation); Py_DECREF(operations);
			PyErr_Format(PyExc_ValueError, "batch operation %zd needs an operation name and Actor", index);
			return nullptr;
		}
		const char* name = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(operation, 0));
		PyObject* actorObject = PySequence_Fast_GET_ITEM(operation, 1);
		AActor* actor = nullptr;
		if (name != nullptr && actorRefType != nullptr && PyObject_TypeCheck(actorObject, actorRefType))
		{
			// Batches are best-effort: ops whose Actor handle went stale since
			// the batch was built are skipped (counted, reported once per call
			// via the warning channel) instead of aborting the remaining ops.
			actor = ResolveActor(reinterpret_cast<PyActorRef*>(actorObject), true, false);
			if (actor == nullptr)
			{
				if (PyErr_Occurred()) { Py_DECREF(operation); Py_DECREF(operations); return nullptr; }
				++skipped;
				Py_DECREF(operation);
				continue;
			}
		}
		else if (name != nullptr)
		{
			actor = ResolveActorArgument(actorObject, "batch actor", false, true);
		}
		if (name == nullptr || actor == nullptr) { Py_DECREF(operation); Py_DECREF(operations); return nullptr; }
		if (strcmp(name, "velocity") == 0 || strcmp(name, "add_velocity") == 0 || strcmp(name, "position") == 0)
		{
			if (size != 5)
			{
				Py_DECREF(operation); Py_DECREF(operations);
				PyErr_Format(PyExc_ValueError, "batch %s operation requires Actor, x, y, z", name);
				return nullptr;
			}
			const double x = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(operation, 2));
			const double y = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(operation, 3));
			const double z = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(operation, 4));
			if (PyErr_Occurred()) { Py_DECREF(operation); Py_DECREF(operations); return nullptr; }
			if (strcmp(name, "position") == 0) actor->SetOrigin(x, y, z, true);
			else if (strcmp(name, "add_velocity") == 0) actor->Vel += DVector3(x, y, z);
			else actor->Vel = DVector3(x, y, z);
		}
		else if (strcmp(name, "health") == 0 || strcmp(name, "damage") == 0)
		{
			if (size != 3)
			{
				Py_DECREF(operation); Py_DECREF(operations);
				PyErr_Format(PyExc_ValueError, "batch %s operation requires Actor and amount", name);
				return nullptr;
			}
			const long amount = PyLong_AsLong(PySequence_Fast_GET_ITEM(operation, 2));
			if (PyErr_Occurred()) { Py_DECREF(operation); Py_DECREF(operations); return nullptr; }
			if (strcmp(name, "damage") == 0) P_DamageMobj(actor, nullptr, nullptr, static_cast<int>(amount), NAME_None);
			else actor->health = static_cast<int>(amount);
		}
		else if (strcmp(name, "destroy") == 0)
		{
			if (size != 2)
			{
				Py_DECREF(operation); Py_DECREF(operations);
				PyErr_SetString(PyExc_ValueError, "batch destroy operation only accepts an Actor");
				return nullptr;
			}
			actor->ClearCounters();
			actor->Destroy();
		}
		else if (strcmp(name, "speed") == 0 || strcmp(name, "alpha") == 0 ||
			strcmp(name, "scale") == 0 || strcmp(name, "damage_factor") == 0 ||
			strcmp(name, "damage_multiply") == 0)
		{
			if (size != 3)
			{
				Py_DECREF(operation); Py_DECREF(operations);
				PyErr_Format(PyExc_ValueError, "batch %s operation requires Actor and a number", name);
				return nullptr;
			}
			const double value = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(operation, 2));
			if (PyErr_Occurred()) { Py_DECREF(operation); Py_DECREF(operations); return nullptr; }
			if (strcmp(name, "speed") == 0) actor->Speed = value;
			else if (strcmp(name, "alpha") == 0) actor->Alpha = std::clamp(value, 0.0, 1.0);
			else if (strcmp(name, "scale") == 0) actor->Scale.X = actor->Scale.Y = std::max(0.0, value);
			else if (strcmp(name, "damage_factor") == 0) actor->DamageFactor = std::max(0.0, value);
			else actor->DamageMultiply = std::max(0.0, value);
		}
		else if (strcmp(name, "tint") == 0)
		{
			if (size != 5)
			{
				Py_DECREF(operation); Py_DECREF(operations);
				PyErr_SetString(PyExc_ValueError, "batch tint operation requires Actor, r, g, b");
				return nullptr;
			}
			int rgb[3];
			for (int component = 0; component < 3; ++component)
			{
				rgb[component] = static_cast<int>(PyLong_AsLong(PySequence_Fast_GET_ITEM(operation, 2 + component)));
				if (PyErr_Occurred()) { Py_DECREF(operation); Py_DECREF(operations); return nullptr; }
			}
			const PalEntry color(255,
				static_cast<uint8_t>(std::clamp(rgb[0], 0, 255)),
				static_cast<uint8_t>(std::clamp(rgb[1], 0, 255)),
				static_cast<uint8_t>(std::clamp(rgb[2], 0, 255)));
			actor->Translation = CreateActorTintTranslation(color);
		}
		else
		{
			Py_DECREF(operation); Py_DECREF(operations);
			PyErr_Format(PyExc_ValueError, "unknown batch operation '%s'", name);
			return nullptr;
		}
		++applied;
		Py_DECREF(operation);
	}
	Py_DECREF(operations);
	if (skipped > 0)
	{
		ReportScriptWarning("apply_actor_batch skipped %u operation%s with stale actor handles",
			static_cast<unsigned>(skipped), skipped == 1 ? "" : "s");
	}
	return PyLong_FromSsize_t(applied);
}

enum class ActorBatchField : uint8_t
{
	Health,
	X,
	Y,
	Z,
	Angle,
	Pitch,
	Roll,
	Speed,
	Alpha,
	Tid,
	ClassName,
	Alive,
	IsPlayer,
	IsMonster,
	Special,
	DamageFactor,
};

struct ActorBatchFieldName
{
	const char* Name;
	ActorBatchField Field;
};

const ActorBatchFieldName ActorBatchFields[] = {
	{ "health", ActorBatchField::Health },
	{ "x", ActorBatchField::X },
	{ "y", ActorBatchField::Y },
	{ "z", ActorBatchField::Z },
	{ "angle", ActorBatchField::Angle },
	{ "pitch", ActorBatchField::Pitch },
	{ "roll", ActorBatchField::Roll },
	{ "speed", ActorBatchField::Speed },
	{ "alpha", ActorBatchField::Alpha },
	{ "tid", ActorBatchField::Tid },
	{ "class_name", ActorBatchField::ClassName },
	{ "alive", ActorBatchField::Alive },
	{ "is_player", ActorBatchField::IsPlayer },
	{ "is_monster", ActorBatchField::IsMonster },
	{ "special", ActorBatchField::Special },
	{ "damage_factor", ActorBatchField::DamageFactor },
};

const char* ActorBatchFieldNames =
	"health, x, y, z, angle, pitch, roll, speed, alpha, tid, class_name, "
	"alive, is_player, is_monster, special, damage_factor";

PyObject* ActorBatchFieldValue(AActor* actor, ActorBatchField field)
{
	switch (field)
	{
	case ActorBatchField::Health: return PyLong_FromLong(actor->health);
	case ActorBatchField::X: return PyFloat_FromDouble(actor->X());
	case ActorBatchField::Y: return PyFloat_FromDouble(actor->Y());
	case ActorBatchField::Z: return PyFloat_FromDouble(actor->Z());
	case ActorBatchField::Angle: return PyFloat_FromDouble(actor->Angles.Yaw.Degrees());
	case ActorBatchField::Pitch: return PyFloat_FromDouble(actor->Angles.Pitch.Degrees());
	case ActorBatchField::Roll: return PyFloat_FromDouble(actor->Angles.Roll.Degrees());
	case ActorBatchField::Speed: return PyFloat_FromDouble(actor->Speed);
	case ActorBatchField::Alpha: return PyFloat_FromDouble(actor->Alpha);
	case ActorBatchField::Tid: return PyLong_FromLong(actor->tid);
	case ActorBatchField::ClassName: return PyUnicode_FromString(actor->GetClass()->TypeName.GetChars());
	case ActorBatchField::Alive: return PyBool_FromLong(actor->health > 0);
	case ActorBatchField::IsPlayer: return PyBool_FromLong(actor->player != nullptr);
	case ActorBatchField::IsMonster: return PyBool_FromLong((actor->flags3 & MF3_ISMONSTER) != 0);
	case ActorBatchField::Special: return PyLong_FromLong(actor->special);
	case ActorBatchField::DamageFactor: return PyFloat_FromDouble(actor->DamageFactor);
	}
	Py_RETURN_NONE;
}

PyObject* PyActorFieldBatch(PyObject*, PyObject* args)
{
	PyObject* refsObject = nullptr;
	PyObject* fieldsObject = nullptr;
	if (!PyArg_ParseTuple(args, "OO:actor_field_batch", &refsObject, &fieldsObject)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	PyObject* fields = PySequence_Fast(fieldsObject, "fields must be a sequence of field names");
	if (fields == nullptr) return nullptr;
	const Py_ssize_t fieldCount = PySequence_Fast_GET_SIZE(fields);
	std::vector<ActorBatchField> parsed;
	parsed.reserve(fieldCount);
	for (Py_ssize_t index = 0; index < fieldCount; ++index)
	{
		PyObject* item = PySequence_Fast_GET_ITEM(fields, index);
		if (!PyUnicode_Check(item))
		{
			Py_DECREF(fields);
			PyErr_Format(PyExc_TypeError, "field %zd must be a string", index);
			return nullptr;
		}
		const char* name = PyUnicode_AsUTF8(item);
		if (name == nullptr) { Py_DECREF(fields); return nullptr; }
		bool found = false;
		for (const ActorBatchFieldName& entry : ActorBatchFields)
		{
			if (strcmp(name, entry.Name) == 0)
			{
				parsed.push_back(entry.Field);
				found = true;
				break;
			}
		}
		if (!found)
		{
			Py_DECREF(fields);
			PyErr_Format(PyExc_ValueError, "unknown actor field '%s'; valid fields: %s", name, ActorBatchFieldNames);
			return nullptr;
		}
	}
	Py_DECREF(fields);
	PyObject* refs = PySequence_Fast(refsObject, "refs must be a sequence of Actor handles");
	if (refs == nullptr) return nullptr;
	const Py_ssize_t refCount = PySequence_Fast_GET_SIZE(refs);
	PyObject* result = PyList_New(refCount);
	if (result == nullptr) { Py_DECREF(refs); return nullptr; }
	for (Py_ssize_t row = 0; row < refCount; ++row)
	{
		PyObject* item = PySequence_Fast_GET_ITEM(refs, row);
		// Reads are best-effort: stale or invalid handles yield a row of None
		// values instead of aborting the whole batch.
		AActor* actor = nullptr;
		if (actorRefType != nullptr && PyObject_TypeCheck(item, actorRefType))
		{
			actor = ResolveActor(reinterpret_cast<PyActorRef*>(item), false, false);
			if (actor == nullptr && PyErr_Occurred()) PyErr_Clear();
		}
		PyObject* tuple = PyTuple_New(fieldCount);
		if (tuple == nullptr) { Py_DECREF(refs); Py_DECREF(result); return nullptr; }
		for (Py_ssize_t column = 0; column < fieldCount; ++column)
		{
			PyObject* value = actor != nullptr ? ActorBatchFieldValue(actor, parsed[column]) : nullptr;
			if (value == nullptr)
			{
				PyErr_Clear();
				value = Py_NewRef(Py_None);
			}
			PyTuple_SET_ITEM(tuple, column, value);
		}
		PyList_SET_ITEM(result, row, tuple);
	}
	Py_DECREF(refs);
	return result;
}

PyObject* PySpawnMissile(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* sourceObject = nullptr;
	PyObject* targetObject = nullptr;
	const char* className = nullptr;
	PyObject* positionObject = Py_None;
	PyObject* ownerObject = Py_None;
	int check = 1;
	static const char* keywords[] = { "source", "target", "class_name", "position", "owner", "check", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOs|OOp:spawn_missile", const_cast<char**>(keywords),
		&sourceObject, &targetObject, &className, &positionObject, &ownerObject, &check)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	AActor* source = ResolveActorArgument(sourceObject, "source", false, true);
	if (source == nullptr) return nullptr;
	AActor* target = ResolveActorArgument(targetObject, "target", false, true);
	if (target == nullptr) return nullptr;
	AActor* owner = ResolveActorArgument(ownerObject, "owner", true, true);
	if (owner == nullptr && ownerObject != Py_None) return nullptr;
	PClassActor* missileClass = PClass::FindActor(FName(className));
	if (missileClass == nullptr)
	{
		PyErr_Format(PyExc_ValueError, "unknown missile class '%s'", className);
		return nullptr;
	}
	DVector3 position = source->PosPlusZ(source->Height * 0.5);
	if (positionObject != Py_None)
	{
		double x, y, z;
		if (!ParseVector3(positionObject, x, y, z, "position must contain exactly three numbers")) return nullptr;
		position = DVector3(x, y, z);
	}
	AActor* missile = P_SpawnMissileXYZ(position, source, target, missileClass, check != 0, owner);
	if (missile == nullptr) Py_RETURN_NONE;
	return MakeActorRef(missile);
}

PyObject* PyLineAttack(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* sourceObject = nullptr;
	PyObject* angleObject = Py_None;
	double distance = 8192.0;
	PyObject* pitchObject = Py_None;
	int damage = 5;
	const char* damageType = "None";
	const char* puffClassName = "BulletPuff";
	int flags = 0;
	static const char* keywords[] = { "source", "angle", "distance", "pitch", "damage", "damage_type", "puff_class", "flags", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|OdOissi:line_attack", const_cast<char**>(keywords),
		&sourceObject, &angleObject, &distance, &pitchObject, &damage, &damageType, &puffClassName, &flags)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	AActor* source = ResolveActorArgument(sourceObject, "source", false, true);
	if (source == nullptr) return nullptr;
	double angle = source->Angles.Yaw.Degrees();
	double pitch = source->Angles.Pitch.Degrees();
	if (angleObject != Py_None)
	{
		angle = PyFloat_AsDouble(angleObject);
		if (PyErr_Occurred()) return nullptr;
	}
	if (pitchObject != Py_None)
	{
		pitch = PyFloat_AsDouble(pitchObject);
		if (PyErr_Occurred()) return nullptr;
	}
	PClassActor* puffClass = PClass::FindActor(FName(puffClassName));
	if (puffClass == nullptr)
	{
		PyErr_Format(PyExc_ValueError, "unknown puff class '%s'", puffClassName);
		return nullptr;
	}
	FTranslatedLineTarget victim{};
	int actualDamage = 0;
	AActor* puff = P_LineAttack(source, DAngle::fromDeg(angle), distance, DAngle::fromDeg(pitch),
		damage, FName(damageType), puffClass, flags, &victim, &actualDamage);
	PyObject* result = PyDict_New();
	if (result == nullptr) return nullptr;
	PyObject* target = MakeActorRef(IsUsableActor(victim.linetarget) ? victim.linetarget : nullptr);
	PyObject* puffReference = MakeActorRef(IsUsableActor(puff) ? puff : nullptr);
	PyObject* applied = PyLong_FromLong(actualDamage);
	if (target == nullptr || puffReference == nullptr || applied == nullptr ||
		PyDict_SetItemString(result, "target", target) < 0 ||
		PyDict_SetItemString(result, "puff", puffReference) < 0 ||
		PyDict_SetItemString(result, "damage", applied) < 0)
	{
		Py_XDECREF(target); Py_XDECREF(puffReference); Py_XDECREF(applied); Py_DECREF(result);
		return nullptr;
	}
	Py_DECREF(target); Py_DECREF(puffReference); Py_DECREF(applied);
	return result;
}

PyObject* PyExitLevel(PyObject*, PyObject* args, PyObject* kwargs)
{
	int position = 0;
	int secret = 0;
	int keepFacing = 0;
	static const char* keywords[] = { "position", "secret", "keep_facing", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|ipp:exit_level", const_cast<char**>(keywords),
		&position, &secret, &keepFacing)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	if (secret) primaryLevel->SecretExitLevel(position);
	else primaryLevel->ExitLevel(position, keepFacing != 0);
	Py_RETURN_NONE;
}

PyObject* PyChangeLevel(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* mapName = nullptr;
	int position = 0;
	int flags = 0;
	int nextSkill = -1;
	static const char* keywords[] = { "map_name", "position", "flags", "next_skill", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|iii:change_level", const_cast<char**>(keywords),
		&mapName, &position, &flags, &nextSkill)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	primaryLevel->ChangeLevel(mapName, position, flags, nextSkill);
	Py_RETURN_NONE;
}

PyObject* PyCenterMessage(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* message = nullptr;
	int bold = 0;
	static const char* keywords[] = { "message", "bold", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|p:center_message", const_cast<char**>(keywords),
		&message, &bold)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	// Center messages only attach to the local status bar; they stay available
	// in multiplayer/demo observer mode.
	if (!CheckLocalPresentation()) return nullptr;
	C_MidPrint(nullptr, message, bold != 0);
	Py_RETURN_NONE;
}

PyObject* PySetMusic(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* name = nullptr;
	int order = 0;
	int looping = 1;
	int force = 0;
	static const char* keywords[] = { "name", "order", "looping", "force", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|ipp:set_music", const_cast<char**>(keywords),
		&name, &order, &looping, &force)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	// S_ChangeMusic only drives the local ZMusic stream (mus_playing); music
	// is per-client presentation, not serialized world state, so it stays
	// available in multiplayer/demo observer mode.
	if (!CheckLocalPresentation()) return nullptr;
	return PyBool_FromLong(S_ChangeMusic(name, order, looping != 0, force != 0));
}

PyObject* PyPlayerLog(PyObject*, PyObject* args, PyObject* kwargs)
{
	int playerIndex = 0;
	static const char* keywords[] = { "player_index", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i:player_log", const_cast<char**>(keywords),
		&playerIndex)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	// Read-only: no mutation guard, so observer mode may query the log too.
	if (playerIndex < 0 || playerIndex >= static_cast<int>(MAXPLAYERS) || !playeringame[playerIndex])
	{
		Py_RETURN_NONE;
	}
	const FString& log = players[playerIndex].LogText;
	if (log.IsEmpty()) Py_RETURN_NONE;
	return PyUnicode_FromString(log.GetChars());
}

PyObject* PySetPlayerLog(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* text = nullptr;
	int playerIndex = 0;
	static const char* keywords[] = { "text", "player_index", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|i:set_player_log", const_cast<char**>(keywords),
		&text, &playerIndex)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	if (playerIndex < 0 || playerIndex >= static_cast<int>(MAXPLAYERS) || !playeringame[playerIndex] ||
		players[playerIndex].mo == nullptr)
	{
		PyErr_Format(PyExc_ValueError, "no player in slot %d", playerIndex);
		return nullptr;
	}
	players[playerIndex].SetLogText(text);
	Py_RETURN_NONE;
}

PyObject* PyStartConversation(PyObject*, PyObject* args, PyObject* kwargs)
{
	PyObject* npcObject = nullptr;
	static const char* keywords[] = { "npc", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:start_conversation", const_cast<char**>(keywords),
		&npcObject)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	AActor* npc = ResolveActorArgument(npcObject, "npc", false, false);
	if (npc == nullptr) return nullptr;
	player_t* console = &players[consoleplayer];
	if (console->mo == nullptr || console->mo->Level == nullptr) Py_RETURN_FALSE;
	// Mirror CanTalk (p_map.cpp) so a failed start leaves no dangling
	// conversation state on the player.
	if (npc->Conversation == nullptr || npc->health <= 0 || (npc->flags4 & MF4_INCOMBAT))
	{
		Py_RETURN_FALSE;
	}
	// Mirror the USE-path arguments from P_TalkFacing: faceTalker + saveAngle.
	npc->ConversationAnimation(0);
	P_StartConversation(npc, console->mo, true, true);
	Py_RETURN_TRUE;
}

//---------------------------------------------------------------------------
// bd.rng(seed) - deterministic per-object random streams. Each stream owns
// an unnamed FRandom, so streams never affect gameplay RNG or savegames.
//---------------------------------------------------------------------------

PyObject* RngStreamNew(PyTypeObject*, PyObject*, PyObject*)
{
	PyErr_SetString(PyExc_TypeError, "RngStream instances are created with biaseddoom.rng(seed)");
	return nullptr;
}

void RngStreamDealloc(PyObject* object)
{
	PyRngStream* self = reinterpret_cast<PyRngStream*>(object);
	delete self->Stream;
	self->Stream = nullptr;
	Py_TYPE(object)->tp_free(object);
}

FRandom* ResolveRngStream(PyObject* object)
{
	if (!CheckApiThread()) return nullptr;
	FRandom* stream = reinterpret_cast<PyRngStream*>(object)->Stream;
	if (stream == nullptr)
	{
		PyErr_SetString(PyExc_ReferenceError, "random stream is no longer valid");
	}
	return stream;
}

PyObject* RngStreamInt(PyObject* object, PyObject* args, PyObject* kwargs)
{
	long long lo, hi;
	static const char* keywords[] = { "lo", "hi", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "LL:int", const_cast<char**>(keywords), &lo, &hi)) return nullptr;
	FRandom* stream = ResolveRngStream(object);
	if (stream == nullptr) return nullptr;
	if (lo > hi)
	{
		PyErr_SetString(PyExc_ValueError, "lo must not exceed hi");
		return nullptr;
	}
	const uint64_t range = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo) + 1u;
	uint64_t value;
	if (range == 0) value = stream->GenRand64(); // range spans the full int64 domain
	else if (range <= 0x7fffffffu) value = static_cast<uint32_t>((*stream)(static_cast<int>(range)));
	else value = stream->GenRand64() % range;
	return PyLong_FromLongLong(static_cast<long long>(static_cast<uint64_t>(lo) + value));
}

PyObject* RngStreamFloat(PyObject* object, PyObject*)
{
	FRandom* stream = ResolveRngStream(object);
	if (stream == nullptr) return nullptr;
	return PyFloat_FromDouble(stream->GenRand_Real2());
}

PyObject* RngStreamChoice(PyObject* object, PyObject* args)
{
	PyObject* sequenceObject = nullptr;
	if (!PyArg_ParseTuple(args, "O:choice", &sequenceObject)) return nullptr;
	FRandom* stream = ResolveRngStream(object);
	if (stream == nullptr) return nullptr;
	PyObject* sequence = PySequence_Fast(sequenceObject, "choice requires a non-empty sequence");
	if (sequence == nullptr) return nullptr;
	const Py_ssize_t size = PySequence_Fast_GET_SIZE(sequence);
	if (size == 0)
	{
		Py_DECREF(sequence);
		PyErr_SetString(PyExc_ValueError, "cannot choose from an empty sequence");
		return nullptr;
	}
	PyObject* item = PySequence_Fast_GET_ITEM(sequence, (*stream)(static_cast<int>(size)));
	Py_INCREF(item);
	Py_DECREF(sequence);
	return item;
}

PyMethodDef RngStreamMethods[] = {
	{ "int", BD_GAME_KEYWORD_FUNCTION(RngStreamInt), METH_VARARGS | METH_KEYWORDS, "Return the next deterministic integer in [lo, hi]." },
	{ "float", RngStreamFloat, METH_NOARGS, "Return the next deterministic float in [0, 1)." },
	{ "choice", RngStreamChoice, METH_VARARGS, "Return a deterministic item from a non-empty sequence." },
	{ nullptr, nullptr, 0, nullptr },
};

PyObject* PyBdRngStream(PyObject*, PyObject* args, PyObject* kwargs)
{
	long long seed = 0;
	static const char* keywords[] = { "seed", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|L:rng", const_cast<char**>(keywords), &seed)) return nullptr;
	if (!CheckApiThread()) return nullptr;
	if (rngStreamType == nullptr)
	{
		PyErr_SetString(PyExc_RuntimeError, "biaseddoom.RngStream type is not initialized");
		return nullptr;
	}
	PyRngStream* stream = PyObject_New(PyRngStream, rngStreamType);
	if (stream == nullptr) return nullptr;
	stream->Stream = new FRandom();
	stream->Stream->Init(static_cast<uint32_t>(seed));
	return reinterpret_cast<PyObject*>(stream);
}

PyObject* PySetTimescale(PyObject*, PyObject* args)
{
	double scale;
	if (!PyArg_ParseTuple(args, "d:set_timescale", &scale)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	// Assignment runs the engine's own clamp/freeze logic (min 0.05,
	// forced to 1.0 in netgames); return the value actually applied.
	i_timescale = static_cast<float>(scale);
	return PyFloat_FromDouble(*i_timescale);
}

PyObject* PyGetTimescale(PyObject*, PyObject*)
{
	if (!CheckApiThread()) return nullptr;
	return PyFloat_FromDouble(*i_timescale);
}

PyObject* PyHudText(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* text = nullptr;
	int id = 0;
	double x = 0.5;
	double y = 0.1;
	const char* colorName = "gold";
	double hold = 2.0;
	double fade = 0.5;
	static const char* keywords[] = { "text", "id", "x", "y", "color", "hold", "fade", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|iddsdd:hud_text", const_cast<char**>(keywords),
		&text, &id, &x, &y, &colorName, &hold, &fade)) return nullptr;
	if (!CheckLocalPresentation()) return nullptr;
	if (StatusBar == nullptr)
	{
		PyErr_SetString(PyExc_RuntimeError, "hud_text requires an active status bar");
		return nullptr;
	}
	EColorRange color = V_FindFontColor(FName(colorName));
	if (color == CR_UNTRANSLATED) color = CR_GOLD;
	// x/y use the ACS HUDMessage convention: fractions of the screen with
	// hudwidth/hudheight zero; hold/fade are seconds.
	DHUDMessage* message = Create<DHUDMessageFadeOut>(V_GetFont("SmallFont"), text,
		static_cast<float>(x), static_cast<float>(y), 0, 0, color,
		static_cast<float>(hold), static_cast<float>(fade));
	StatusBar->AttachMessage(message, id != 0 ? 0xff000000u | static_cast<uint32_t>(id) : 0u);
	Py_RETURN_NONE;
}

PyObject* PyHudClear(PyObject*, PyObject* args, PyObject* kwargs)
{
	int id = 0;
	static const char* keywords[] = { "id", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i:hud_clear", const_cast<char**>(keywords), &id)) return nullptr;
	if (!CheckLocalPresentation()) return nullptr;
	// Messages attached with the default id 0 cannot be addressed; clearing is
	// a graceful no-op without a status bar, matching AttachMessage's detach.
	if (StatusBar == nullptr || id == 0) Py_RETURN_NONE;
	DHUDMessageBase* old = StatusBar->DetachMessage(0xff000000u | static_cast<uint32_t>(id));
	if (old != nullptr) old->Destroy();
	Py_RETURN_NONE;
}

player_t* RequireConsolePlayer(const char* function)
{
	player_t* viewer = &players[consoleplayer];
	if (!playeringame[consoleplayer] || viewer->mo == nullptr)
	{
		PyErr_Format(PyExc_RuntimeError, "%s requires an active console player", function);
		return nullptr;
	}
	return viewer;
}

PyObject* PyScreenFlash(PyObject*, PyObject* args, PyObject* kwargs)
{
	int red, green, blue;
	double alpha;
	static const char* keywords[] = { "r", "g", "b", "alpha", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "iiid:screen_flash", const_cast<char**>(keywords),
		&red, &green, &blue, &alpha)) return nullptr;
	if (!CheckLocalPresentation()) return nullptr;
	player_t* viewer = RequireConsolePlayer("screen_flash");
	if (viewer == nullptr) return nullptr;
	viewer->BlendR = std::clamp(red, 0, 255) / 255.f;
	viewer->BlendG = std::clamp(green, 0, 255) / 255.f;
	viewer->BlendB = std::clamp(blue, 0, 255) / 255.f;
	viewer->BlendA = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
	Py_RETURN_NONE;
}

PyObject* PyScreenFade(PyObject*, PyObject* args, PyObject* kwargs)
{
	int red, green, blue;
	double alpha;
	double seconds = 1.0;
	static const char* keywords[] = { "r", "g", "b", "alpha", "seconds", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "iiid|d:screen_fade", const_cast<char**>(keywords),
		&red, &green, &blue, &alpha, &seconds)) return nullptr;
	if (!CheckLocalPresentation()) return nullptr;
	player_t* viewer = RequireConsolePlayer("screen_fade");
	if (viewer == nullptr) return nullptr;
	const float fromR = std::clamp(red, 0, 255) / 255.f;
	const float fromG = std::clamp(green, 0, 255) / 255.f;
	const float fromB = std::clamp(blue, 0, 255) / 255.f;
	const float fromA = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
	if (seconds <= 0.0)
	{
		viewer->BlendR = 0.f;
		viewer->BlendG = 0.f;
		viewer->BlendB = 0.f;
		viewer->BlendA = 0.f;
	}
	else
	{
		primaryLevel->CreateThinker<DFlashFader>(fromR, fromG, fromB, fromA,
			0.f, 0.f, 0.f, 0.f, static_cast<float>(seconds), viewer->mo);
	}
	Py_RETURN_NONE;
}

PyObject* PyPlayUiSound(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* name = nullptr;
	double volume = 1.0;
	static const char* keywords[] = { "name", "volume", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|d:play_ui_sound", const_cast<char**>(keywords),
		&name, &volume)) return nullptr;
	if (!CheckLocalPresentation()) return nullptr;
	S_Sound(CHAN_AUTO, CHANF_UI | CHANF_NORUMBLE, name, static_cast<float>(volume), ATTN_NORM);
	Py_RETURN_NONE;
}

bool SanitizeCheckpointName(const char* name, FString& result)
{
	result = "";
	for (const char* cursor = name; *cursor != 0; ++cursor)
	{
		const char c = *cursor;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
		{
			result += c;
		}
	}
	if (result.IsEmpty())
	{
		PyErr_SetString(PyExc_ValueError, "checkpoint name must contain at least one letter, digit, '-' or '_'");
		return false;
	}
	return true;
}

PyObject* PySaveCheckpoint(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* name = "checkpoint";
	const char* description = "";
	static const char* keywords[] = { "name", "description", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|ss:save_checkpoint", const_cast<char**>(keywords),
		&name, &description)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	FString clean;
	if (!SanitizeCheckpointName(name, clean)) return nullptr;
	if (!usergame || gamestate != GS_LEVEL)
	{
		PyErr_SetString(PyExc_RuntimeError, "checkpoints can only be saved while a level is being played");
		return nullptr;
	}
	// G_SaveGame only schedules the save (sendsave); it runs at the next tic
	// boundary. Checkpoints land in the save dir as <name>.zds.
	const FString file = G_BuildSaveName(clean.GetChars());
	G_SaveGame(file.GetChars(), description, false);
	Py_RETURN_NONE;
}

PyObject* PyLoadCheckpoint(PyObject*, PyObject* args, PyObject* kwargs)
{
	const char* name = "checkpoint";
	static const char* keywords[] = { "name", nullptr };
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|s:load_checkpoint", const_cast<char**>(keywords),
		&name)) return nullptr;
	if (!CheckGameplayMutation()) return nullptr;
	FString clean;
	if (!SanitizeCheckpointName(name, clean)) return nullptr;
	const FString file = G_BuildSaveName(clean.GetChars());
	if (!FileExists(file))
	{
		PyErr_Format(PyExc_FileNotFoundError, "checkpoint '%s' does not exist", clean.GetChars());
		return nullptr;
	}
	// G_LoadGame defers via gameaction=ga_loadgame; loading aborts the
	// current level at the next tic boundary.
	G_LoadGame(file.GetChars(), false);
	Py_RETURN_NONE;
}


PyObject* PySessionReadOnly(PyObject*, PyObject*)
{
	if (!CheckApiThread()) return nullptr;
	return PyBool_FromLong(netgame || multiplayer || demoplayback || demorecording);
}

PyObject* PyActorData(PyObject*, PyObject* args)
{
	PyObject* object = nullptr;
	if (!PyArg_ParseTuple(args, "O:actor_data", &object)) return nullptr;
	if (actorRefType == nullptr || !PyObject_TypeCheck(object, actorRefType))
	{
		PyErr_SetString(PyExc_TypeError, "actor_data requires an Actor handle");
		return nullptr;
	}
	PyActorRef* reference = reinterpret_cast<PyActorRef*>(object);
	// Read-only resolve (raises ReferenceError on stale/invalid handles like
	// every other handle use): the data dict is script-side storage and
	// carries no gameplay mutation by itself.
	if (ResolveActor(reference, false, true) == nullptr) return nullptr;
	const uint64_t key = ActorDataKey(reference->Slot, reference->Generation);
	auto existing = actorDataStore.find(key);
	if (existing != actorDataStore.end()) return Py_NewRef(existing->second);
	PyObject* dict = PyDict_New();
	if (dict == nullptr) return nullptr;
	// The store keeps its own strong reference; the caller owns the new one.
	actorDataStore.emplace(key, Py_NewRef(dict));
	return dict;
}

PyObject* PyActorDataDrop(PyObject*, PyObject* args)
{
	PyObject* object = nullptr;
	if (!PyArg_ParseTuple(args, "O:actor_data_drop", &object)) return nullptr;
	if (actorRefType == nullptr || !PyObject_TypeCheck(object, actorRefType))
	{
		PyErr_SetString(PyExc_TypeError, "actor_data_drop requires an Actor handle");
		return nullptr;
	}
	if (!CheckApiThread()) return nullptr;
	PyActorRef* reference = reinterpret_cast<PyActorRef*>(object);
	// Drop is idempotent: stale handles (destroyed actor, unloaded map) return
	// False instead of raising. Resolving without raise also lets the stale
	// path invalidate the slot, which purges any orphaned entry as a side
	// effect.
	if (ResolveActor(reference, false, false) == nullptr) Py_RETURN_FALSE;
	auto entry = actorDataStore.find(ActorDataKey(reference->Slot, reference->Generation));
	if (entry == actorDataStore.end()) Py_RETURN_FALSE;
	Py_DECREF(entry->second);
	actorDataStore.erase(entry);
	Py_RETURN_TRUE;
}

PyMethodDef GameMethods[] = {
	{ "actor_ref", PyActorRefByTid, METH_VARARGS, "Return a live Actor handle for a TID, or None." },
	{ "actor_refs", BD_GAME_KEYWORD_FUNCTION(PyActorRefs), METH_VARARGS | METH_KEYWORDS, "Return lightweight live Actor handles." },
	{ "spawn", BD_GAME_KEYWORD_FUNCTION(PySpawnActorRef), METH_VARARGS | METH_KEYWORDS, "Spawn and return a live Actor handle." },
	{ "actor_data", PyActorData, METH_VARARGS, "Return the actor's persistent per-actor data dict, creating it on demand. Purged automatically when the actor is destroyed or the map changes; not saved in savegames." },
	{ "actor_data_drop", PyActorDataDrop, METH_VARARGS, "Drop the actor's per-actor data dict if present. Returns True when one existed." },
	{ "player", PyPlayer, METH_VARARGS, "Return a live Player handle by slot." },
	{ "player_refs", PyPlayerRefs, METH_NOARGS, "Return all in-game Player handles." },
	{ "sector", PySector, METH_VARARGS, "Return a Sector handle by index." },
	{ "sectors", BD_GAME_KEYWORD_FUNCTION(PySectors), METH_VARARGS | METH_KEYWORDS, "Return Sector handles, optionally by tag." },
	{ "sector_at", PySectorAt, METH_VARARGS, "Return the Sector containing the given point, or None." },
	{ "actors_in_sector", PyActorsInSector, METH_VARARGS, "Return live Actor handles currently inside the given sector." },
	{ "line", PyLine, METH_VARARGS, "Return a Line handle by index." },
	{ "lines", BD_GAME_KEYWORD_FUNCTION(PyLines), METH_VARARGS | METH_KEYWORDS, "Return Line handles, optionally by line ID." },
	{ "execute_special", BD_GAME_KEYWORD_FUNCTION(PyExecuteSpecial), METH_VARARGS | METH_KEYWORDS, "Execute any numeric or named action special." },
	{ "radius_damage", BD_GAME_KEYWORD_FUNCTION(PyRadiusDamage), METH_VARARGS | METH_KEYWORDS, "Perform a native radius attack." },
	{ "apply_actor_batch", PyApplyActorBatch, METH_VARARGS, "Apply many actor mutations in one C API crossing." },
	{ "actor_field_batch", PyActorFieldBatch, METH_VARARGS, "Read many actor fields in one C API crossing, returning a tuple per actor. Stale or invalid handles yield a tuple of None values." },
	{ "spawn_missile", BD_GAME_KEYWORD_FUNCTION(PySpawnMissile), METH_VARARGS | METH_KEYWORDS, "Spawn a native aimed missile." },
	{ "line_attack", BD_GAME_KEYWORD_FUNCTION(PyLineAttack), METH_VARARGS | METH_KEYWORDS, "Fire a native hitscan and return its result." },
	{ "exit_level", BD_GAME_KEYWORD_FUNCTION(PyExitLevel), METH_VARARGS | METH_KEYWORDS, "Exit through the normal or secret route." },
	{ "change_level", BD_GAME_KEYWORD_FUNCTION(PyChangeLevel), METH_VARARGS | METH_KEYWORDS, "Request an explicit map transition." },
	{ "center_message", BD_GAME_KEYWORD_FUNCTION(PyCenterMessage), METH_VARARGS | METH_KEYWORDS, "Display an immediate center-screen message." },
	{ "player_log", BD_GAME_KEYWORD_FUNCTION(PyPlayerLog), METH_VARARGS | METH_KEYWORDS, "Return the player's conversation log text (the Strife journal line), or None." },
	{ "set_player_log", BD_GAME_KEYWORD_FUNCTION(PySetPlayerLog), METH_VARARGS | METH_KEYWORDS, "Set the player's conversation log text (the Strife journal line)." },
	{ "start_conversation", BD_GAME_KEYWORD_FUNCTION(PyStartConversation), METH_VARARGS | METH_KEYWORDS, "Start a Strife conversation between the local player and the given actor." },
	{ "set_music", BD_GAME_KEYWORD_FUNCTION(PySetMusic), METH_VARARGS | METH_KEYWORDS, "Change level music immediately." },
	{ "rng", BD_GAME_KEYWORD_FUNCTION(PyBdRngStream), METH_VARARGS | METH_KEYWORDS, "Create a deterministic random stream from a seed." },
	{ "set_timescale", PySetTimescale, METH_VARARGS, "Set the game time scale and return the applied value." },
	{ "get_timescale", PyGetTimescale, METH_NOARGS, "Return the current game time scale." },
	{ "hud_text", BD_GAME_KEYWORD_FUNCTION(PyHudText), METH_VARARGS | METH_KEYWORDS, "Display a fading HUD message." },
	{ "hud_clear", BD_GAME_KEYWORD_FUNCTION(PyHudClear), METH_VARARGS | METH_KEYWORDS, "Remove the HUD message with the given id." },
	{ "screen_flash", BD_GAME_KEYWORD_FUNCTION(PyScreenFlash), METH_VARARGS | METH_KEYWORDS, "Instantly set the console player's screen blend (r, g, b are 0-255, alpha is 0-1)." },
	{ "screen_fade", BD_GAME_KEYWORD_FUNCTION(PyScreenFade), METH_VARARGS | METH_KEYWORDS, "Fade the console player's screen blend from a color to transparent." },
	{ "play_ui_sound", BD_GAME_KEYWORD_FUNCTION(PyPlayUiSound), METH_VARARGS | METH_KEYWORDS, "Play a UI sound for the local player." },
	{ "save_checkpoint", BD_GAME_KEYWORD_FUNCTION(PySaveCheckpoint), METH_VARARGS | METH_KEYWORDS, "Save the game into a named checkpoint slot at the next tic boundary." },
	{ "load_checkpoint", BD_GAME_KEYWORD_FUNCTION(PyLoadCheckpoint), METH_VARARGS | METH_KEYWORDS, "Load a named checkpoint slot at the next tic boundary, aborting the current level." },
	{ "session_read_only", PySessionReadOnly, METH_NOARGS, "Return True when world mutations are blocked (multiplayer or demo session)." },
	{ nullptr, nullptr, 0, nullptr },
};

PyTypeObject* CreateType(const char* name, size_t basicSize, PyType_Slot* slots)
{
	PyType_Spec spec = { name, static_cast<int>(basicSize), 0, Py_TPFLAGS_DEFAULT, slots };
	return reinterpret_cast<PyTypeObject*>(PyType_FromSpec(&spec));
}

bool AddType(PyObject* module, const char* name, PyTypeObject* type)
{
	if (type == nullptr) return false;
	return PyModule_AddObject(module, name, Py_NewRef(reinterpret_cast<PyObject*>(type))) == 0;
}
} // namespace

bool Initialize(PyObject* module)
{
	if (!markerRegistered)
	{
		GC::AddMarkerFunc(MarkRoots);
		markerRegistered = true;
	}

	PyType_Slot actorSlotsDefinition[] = {
		{ Py_tp_dealloc, reinterpret_cast<void*>(ActorRefDealloc) },
		{ Py_tp_repr, reinterpret_cast<void*>(ActorRefRepr) },
		{ Py_tp_hash, reinterpret_cast<void*>(ActorRefHash) },
		{ Py_tp_richcompare, reinterpret_cast<void*>(ActorRefRichCompare) },
		{ Py_tp_methods, ActorMethods },
		{ Py_tp_getset, ActorGetSets },
		{ Py_nb_bool, reinterpret_cast<void*>(ActorRefBool) },
		{ 0, nullptr },
	};
	PyType_Slot playerSlotsDefinition[] = {
		{ Py_tp_repr, reinterpret_cast<void*>(PlayerRepr) },
		{ Py_tp_methods, PlayerMethods },
		{ Py_tp_getset, PlayerGetSets },
		{ 0, nullptr },
	};
	PyType_Slot sectorSlotsDefinition[] = {
		{ Py_tp_repr, reinterpret_cast<void*>(SectorRepr) },
		{ Py_tp_methods, SectorMethods },
		{ Py_tp_getset, SectorGetSets },
		{ 0, nullptr },
	};
	PyType_Slot lineSlotsDefinition[] = {
		{ Py_tp_repr, reinterpret_cast<void*>(LineRepr) },
		{ Py_tp_methods, LineMethods },
		{ Py_tp_getset, LineGetSets },
		{ 0, nullptr },
	};
	PyType_Slot rngStreamSlotsDefinition[] = {
		{ Py_tp_dealloc, reinterpret_cast<void*>(RngStreamDealloc) },
		{ Py_tp_new, reinterpret_cast<void*>(RngStreamNew) },
		{ Py_tp_methods, RngStreamMethods },
		{ 0, nullptr },
	};

	actorRefType = CreateType("biaseddoom.Actor", sizeof(PyActorRef), actorSlotsDefinition);
	playerRefType = CreateType("biaseddoom.Player", sizeof(PyPlayerRef), playerSlotsDefinition);
	sectorRefType = CreateType("biaseddoom.Sector", sizeof(PyWorldRef), sectorSlotsDefinition);
	lineRefType = CreateType("biaseddoom.Line", sizeof(PyWorldRef), lineSlotsDefinition);
	rngStreamType = CreateType("biaseddoom.RngStream", sizeof(PyRngStream), rngStreamSlotsDefinition);
	if (actorRefType == nullptr || playerRefType == nullptr || sectorRefType == nullptr || lineRefType == nullptr || rngStreamType == nullptr) return false;
	if (PyModule_AddFunctions(module, GameMethods) < 0) return false;
	if (!PythonDisplayList::AddFunctions(module)) return false;
	if (!PythonImGui::Initialize(module)) return false;
	if (!AddType(module, "Actor", actorRefType) || !AddType(module, "Player", playerRefType) ||
		!AddType(module, "Sector", sectorRefType) || !AddType(module, "Line", lineRefType) ||
		!AddType(module, "RngStream", rngStreamType)) return false;

	PyModule_AddIntConstant(module, "BT_ATTACK", BT_ATTACK);
	PyModule_AddIntConstant(module, "BT_USE", BT_USE);
	PyModule_AddIntConstant(module, "BT_JUMP", BT_JUMP);
	PyModule_AddIntConstant(module, "BT_CROUCH", BT_CROUCH);
	PyModule_AddIntConstant(module, "BT_ALTATTACK", BT_ALTATTACK);
	PyModule_AddIntConstant(module, "BT_RELOAD", BT_RELOAD);
	PyModule_AddIntConstant(module, "BT_ZOOM", BT_ZOOM);
	PyModule_AddIntConstant(module, "BT_USER1", BT_USER1);
	PyModule_AddIntConstant(module, "BT_USER2", BT_USER2);
	PyModule_AddIntConstant(module, "BT_USER3", BT_USER3);
	PyModule_AddIntConstant(module, "BT_USER4", BT_USER4);
	PyModule_AddIntConstant(module, "CHANGELEVEL_KEEPFACING", CHANGELEVEL_KEEPFACING);
	PyModule_AddIntConstant(module, "CHANGELEVEL_RESETINVENTORY", CHANGELEVEL_RESETINVENTORY);
	PyModule_AddIntConstant(module, "CHANGELEVEL_NOMONSTERS", CHANGELEVEL_NOMONSTERS);
	PyModule_AddIntConstant(module, "CHANGELEVEL_NOINTERMISSION", CHANGELEVEL_NOINTERMISSION);
	PyModule_AddIntConstant(module, "CHANGELEVEL_RESETHEALTH", CHANGELEVEL_RESETHEALTH);
	return true;
}

void MarkRoots()
{
	for (uint32_t index = 0; index < actorSlots.size(); ++index)
	{
		ActorSlot& slot = actorSlots[index];
		if (slot.PythonReferences == 0 || slot.Actor.ForceGet() == nullptr) continue;
		if (!IsUsableActor(slot.Actor.ForceGet()))
		{
			// Never InvalidateActorSlot (Py_DECREF) from inside a marker: queue
			// the slot and keep marking the actor below so its pointer state
			// stays safe for the rest of this collection.
			QueueDeferredSlotInvalidation(index);
		}
		GC::Mark(slot.Actor);
	}
}

void DrainDeferredInvalidations()
{
	DrainDeferredInvalidationsInternal();
}

void InvalidateWorld()
{
	// Flush anything the markers queued before the wholesale invalidation
	// below (which also leaves the deferred queue empty).
	DrainDeferredInvalidationsInternal();
	// Drop every per-actor data dict; the per-slot purge below would reach the
	// same keys, but only for actors that still have a slot.
	for (auto& entry : actorDataStore) Py_DECREF(entry.second);
	actorDataStore.clear();
	for (uint32_t index = 0; index < actorSlots.size(); ++index) InvalidateActorSlot(index);
	deferredSlotInvalidations.clear();
	actorLookup.clear();
	if (++worldGeneration == 0) ++worldGeneration;
}

void Shutdown()
{
	InvalidateWorld();
	// Release the owning references returned by PyType_FromSpec. The module
	// retains its own references until interpreter finalization.
	Py_XDECREF(reinterpret_cast<PyObject*>(actorRefType));
	Py_XDECREF(reinterpret_cast<PyObject*>(playerRefType));
	Py_XDECREF(reinterpret_cast<PyObject*>(sectorRefType));
	Py_XDECREF(reinterpret_cast<PyObject*>(lineRefType));
	Py_XDECREF(reinterpret_cast<PyObject*>(rngStreamType));
	actorRefType = nullptr;
	playerRefType = nullptr;
	sectorRefType = nullptr;
	lineRefType = nullptr;
	rngStreamType = nullptr;
	actorSlots.clear();
}

PyObject* MakeActorRef(AActor* actor)
{
	if (!IsUsableActor(actor)) Py_RETURN_NONE;
	if (actorRefType == nullptr)
	{
		PyErr_SetString(PyExc_RuntimeError, "biaseddoom.Actor type is not initialized");
		return nullptr;
	}
	const uint32_t slotIndex = AcquireActorSlot(actor);
	PyActorRef* reference = PyObject_New(PyActorRef, actorRefType);
	if (reference == nullptr)
	{
		ActorSlot& slot = actorSlots[slotIndex];
		if (slot.PythonReferences > 0 && --slot.PythonReferences == 0) InvalidateActorSlot(slotIndex);
		return nullptr;
	}
	reference->Slot = slotIndex;
	reference->Generation = actorSlots[slotIndex].Generation;
	return reinterpret_cast<PyObject*>(reference);
}

PyObject* MakePlayerRef(int index)
{
	if (playerRefType == nullptr)
	{
		PyErr_SetString(PyExc_RuntimeError, "biaseddoom.Player type is not initialized");
		return nullptr;
	}
	PyPlayerRef* reference = PyObject_New(PyPlayerRef, playerRefType);
	if (reference == nullptr) return nullptr;
	reference->Index = index;
	return reinterpret_cast<PyObject*>(reference);
}

AActor* ActorFromHandle(_object* object)
{
	if (object == nullptr)
	{
		PyErr_SetString(PyExc_TypeError, "actor must be an Actor or a nonzero TID");
		return nullptr;
	}
	return ResolveActorArgument(reinterpret_cast<PyObject*>(object), "actor", false, false);
}
} // namespace PythonRuntime::GameApi

#else

namespace PythonRuntime::GameApi
{
bool Initialize(_object*) { return false; }
void MarkRoots() {}
void DrainDeferredInvalidations() {}
void InvalidateWorld() {}
void Shutdown() {}
_object* MakeActorRef(AActor*) { return nullptr; }
_object* MakePlayerRef(int) { return nullptr; }
AActor* ActorFromHandle(_object*) { return nullptr; }
}

#endif
