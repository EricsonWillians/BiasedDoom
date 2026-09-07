#pragma once

class AActor;
class FName;
class FSerializer;

namespace PythonRuntime
{
	// The build may contain stubs when CPython development files are absent.
	bool IsCompiled();
	bool IsActive();
	bool Initialize();
	void Shutdown();
	bool Reload();

	void OnWorldLoaded();
	void OnWorldUnloaded(const char* nextMap);
	void OnWorldPreTick();
	void OnWorldTick();
	void OnWorldPostTick();
	void OnActorSpawned(AActor* actor);
	// source is the killer passed to AActor::Die (can be nullptr): the shooter
	// for missile kills, the attacker for hitscan/melee, the bomb owner for
	// explosions; nullptr for environmental deaths (crushers, falling, ...).
	void OnActorDied(AActor* actor, AActor* inflictor, AActor* source);
	void OnActorDamaged(AActor* actor, AActor* inflictor, AActor* source,
		int damage, const char* damageType, int flags, double angle);
	// Mutable pre-damage filter fired from the top of DoDamageMobj, before
	// armor, damage factors, and the actor_damaged event. Handlers rewrite the
	// event dict's damage/damage_type in place or set cancel=true to swallow
	// the hit entirely (the return value reports cancellation). Offline-only:
	// dispatch is skipped in multiplayer and demo sessions for determinism.
	// When no handler is registered this early-outs on one HasCallbacks read.
	bool OnBeforeDamage(AActor* target, AActor* inflictor, AActor* source,
		int& damage, FName& mod, int flags, double angle);
	void OnActorDestroyed(AActor* actor);
	void OnActorRevived(AActor* actor);
	void OnLineActivated(int lineIndex, AActor* actor, int activationType);
	// reason is an ESpecialFailReason code (see p_spec.h); the event payload
	// carries it as reason_code (int) and reason (string).
	void OnLineActivationFailed(int lineIndex, int special, const int* args, AActor* actor, int activationType, int reason);
	void OnPlayerEvent(const char* eventName, int playerIndex, bool fromHub = false);
	void OnItemPicked(AActor* item, AActor* toucher, int amount);
	void OnItemDropped(AActor* item, AActor* dropper, int amount);
	// Fires when a player's ready weapon actually changes during bring-up
	// (PlayerPawn.BringUpWeapon assigning ReadyWeapon). weapon may be nullptr
	// when the player ends up empty-handed.
	void OnWeaponChanged(AActor* pawn, AActor* weapon);
	void OnSecretFound(int playernum);
	// Fires once per rendered frame from the Dear ImGui overlay layer while it
	// is visible; Python widgets submit ImGui draw calls from this callback.
	void OnImguiFrame();
	// Fires when a Strife conversation is successfully entered
	// (P_StartConversation, after all early-out checks). playerIndex is the
	// replying player's index or -1 when pc is not a player.
	void OnConversationStarted(AActor* npc, AActor* pc, int playerIndex);
	// Fires exactly once per committed conversation reply, from the netcode
	// handler (HandleReply, reachable only via P_ConversationCommand), on every
	// machine; player_index identifies the replying player. logNumber/-1,
	// logString/nullptr and nextNode/-1 use -1/nullptr for "none".
	// itemChanged reports whether the reply gave or took inventory items.
	void OnConversationReply(int playerIndex, AActor* npc, int nodeNumber, int replyIndex,
		int logNumber, const char* logString, int nextNode, bool itemChanged);
	// Fires when the `pyui <name>` console command runs. Bridges console
	// aliases and key bindings to Python UI handlers; the payload carries
	// command=name. Pure notification: never mutates world state.
	void OnUiCommand(const char* name);

	// Total Python errors reported this session, including dedup-suppressed
	// repeats. Backs the -scripttest exit status.
	unsigned int GetErrorCount();

	// -pyerrorlog <file>: append Python errors as JSON lines for external
	// tooling (editors, CI). Empty path disables the feed.
	void SetErrorLogPath(const char* path);

	// Rate-limited yellow-console warning channel for script-facing issues
	// that must not count as -scripttest failures. Also feeds -pyerrorlog
	// with severity "warning".
	void ReportScriptWarning(const char* fmt, ...);

	// Python's shared state dictionary is stored as JSON in savegames. Reading
	// is split from callback dispatch because actors are restored later.
	void SerializeState(FSerializer& arc);
	void FinishLoadState();

	void PrintStatus();

	// Internal contract used by the separately compiled native gameplay API.
	// Every Python entry point checks these before touching engine state.
	bool CheckApiThread();
	bool CheckGameplayMutation();
	bool CheckSessionMutation();
	// Local presentation effects (HUD, screen blends, UI sounds, display list)
	// stay available in multiplayer/demo observer mode; this only requires an
	// active level, unlike CheckGameplayMutation.
	bool CheckLocalPresentation();
}
