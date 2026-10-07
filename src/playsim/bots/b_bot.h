/*******************************
* B_Bot.h                      *
* Description:                 *
* Used with all b_*            *
*******************************/

#ifndef __B_BOT_H__
#define __B_BOT_H__

#include "c_cvars.h"
#include "info.h"
#include "doomdef.h"
#include "d_protocol.h"
#include "r_defs.h"
#include "a_pickups.h"
#include "a_weapons.h"
#include "stats.h"

#define FORWARDWALK		0x1900
#define FORWARDRUN		0x3200
#define SIDEWALK		0x1800
#define SIDERUN			0x2800

#define BOT_VERSION 0.97
//Switches-
#define BOT_RELEASE_COMPILE //Define this when compiling a version that should be released.

#define NOCOLOR     11
#define MAXTHINGNODES 100 //Path stuff (paths created between items).
#define SPAWN_DELAY 80  //Used to determine how many tics there are between each bot spawn when bot's are being spawned in a row (like when entering a new map).

#define BOTFILENAME "bots.cfg"

#define MAX_TRAVERSE_DIST (100000000/65536.) //10 meters, used within b_func.c
#define AVOID_DIST   (45000000/65536.) //Try avoid incoming missiles once they reached this close
#define SAFE_SELF_MISDIST (140.)    //Distance from self to target where it's safe to pull a rocket.
#define FRIEND_DIST  (15000000/65536.) //To friend.
#define DARK_DIST  (5000000/65536.) //Distance that bot can see enemies in the dark from.
#define WHATS_DARK  50 //light value thats classed as dark.
#define MAX_MONSTER_TARGET_DIST  (50000000/65536.) //Too high can slow down the performance, see P_mobj.c
#define ENEMY_SCAN_FOV (120.)
#define THINGTRYTICK 1000
#define MAXMOVEHEIGHT (32) //MAXSTEPMOVE but with jumping counted in.
#define GETINCOMBAT (35000000/65536.) //Max distance to item. if it's due to be icked up in a combat situation.
#define SHOOTFOV	(60.)
#define AFTERTICS   (2*TICRATE) //Seconds that bot will be alert on an recent enemy. Ie not looking the other way
#define MAXROAM		(4*TICRATE) //When this time is elapsed the bot will roam after something else.
//monster mod
#define MSPAWN_DELAY 20//Tics between each spawn.
#define MMAXSELECT   100 //Maximum number of monsters that can be selected at a time.

// Player pawns may use ordinary ledges, but bot navigation must only relax
// the monster-style drop limit for a verified, goal-directed route.
constexpr double BOT_MAX_PLANNED_DESCENT = 64.0;

inline bool BotCanTraverseDrop(double upperFloor, double lowerFloor,
	double normalDropLimit, bool allowPlannedDescent)
{
	const double drop = upperFloor - lowerFloor;
	return drop <= normalDropLimit ||
		(allowPlannedDescent && drop <= BOT_MAX_PLANNED_DESCENT);
}

struct FCheckPosition;

// A failed movement probe is not always a physical wall. Bots may use a
// verified manual mover for a blocking door/lift, but must never press an
// adjacent special after the same probe has rejected a hazard, cliff, or bad
// clearance. Keep the reason explicit instead of treating every false result
// from CleanAhead as an invitation to use a line.
enum class EBotMoveBlockReason
{
	None,
	Geometry,
	VerticalStep,
	Unsafe,
};

struct botskill_t
{
	int aiming;
	int perfection;
	int reaction;   //How fast the bot will fire after seeing the player.
	int isp;        //Instincts of Self Preservation. Personality
};

enum
{
	BOTINUSE_No,
	BOTINUSE_Waiting,
	BOTINUSE_Yes,
};

//Info about all bots in the bots.cfg
//Updated during each level start.
//Info given to bots when they're spawned.
struct botinfo_t
{
	botinfo_t *next = nullptr;
	FString Name;
	FString Info;
	botskill_t skill = {};
	int inuse = 0;
	int lastteam = 0;
	// Local host-side reservation data for a queued DEM_ADDBOT. The live
	// DBot carries the same values after the command is applied, so profile
	// membership is not inferred from roster order or player slots.
	int companionProfile = -1;
	uint32_t companionId = 0;
};

struct BotInfoData
{
	int MoveCombatDist = 0;
	int flags = 0;
	PClassActor *projectileType = nullptr;
};


enum
{
	BIF_BOT_REACTION_SKILL_THING = 1,
	BIF_BOT_EXPLOSIVE = 2,
	BIF_BOT_BFG = 4,
	// This weapon has no BOTSUPP entry and is only being handled through the
	// generic weapon flags. Keep the bot at a conservative distance and never
	// fire an unknown explosive or BFG-class weapon.
	BIF_BOT_FALLBACK = 8,
	BIF_BOT_NO_FIRE = 16,
};


using BotInfoMap = TMap<FName, BotInfoData>;

extern BotInfoMap BotInfo;

inline BotInfoData GetBotInfo(AActor *weap)
{
	if (weap == nullptr)
		return BotInfoData();
	auto k = BotInfo.CheckKey(weap->GetClass()->TypeName);
	if (k)
		return *k;

	// BOTSUPP remains authoritative whenever an entry exists. A missing entry
	// used to look exactly like a melee weapon, which made unfamiliar ranged
	// weapons charge into danger. Use the engine's generic weapon metadata as
	// a safe, deliberately limited fallback instead.
	const int weaponflags = weap->IntVar(NAME_WeaponFlags);
	if (weaponflags & WIF_MELEEWEAPON)
		return BotInfoData();

	BotInfoData fallback;
	fallback.MoveCombatDist = 24000000;
	fallback.flags = BIF_BOT_FALLBACK;
	if (weaponflags & WIF_EXPLOSIVE)
		fallback.flags |= BIF_BOT_EXPLOSIVE | BIF_BOT_NO_FIRE;
	if (weaponflags & WIF_BFG)
		fallback.flags |= BIF_BOT_BFG | BIF_BOT_NO_FIRE;
	return fallback;
}

//Used to keep all the globally needed variables in nice order.
class FCajunMaster
{
public:
	// Keep cooperative games readable and leave room for human players even
	// though the engine itself supports a larger player table.
	static constexpr int MaxCompanions = 7;
	static constexpr int MaxCoopParticipants = 8;
	static constexpr int NoCompanionProfile = -1;
	static constexpr uint32_t NoCompanionId = 0;

	~FCajunMaster();

	void ClearPlayer (int playernum, bool keepTeam);

	//(b_game.cpp)
	void Main (FLevelLocals *Level);
	void Init ();
	void End();
	bool SpawnBot (const char *name, int color = NOCOLOR, int companionProfile = -1);
	// Profile-aware companion spawning keeps the menu's ordered identity and
	// appearance choices attached to the first through seventh co-op slots.
	// A direct add still uses the next unoccupied profile, while legacy bots
	// keep the old SpawnBot entry point unchanged.
	bool SpawnConfiguredCompanion(int profile, const char *legacyName = nullptr);
	void TryAddBot (FLevelLocals *Level, TArrayView<uint8_t>& stream, int player);
	void RemoveAllBots (FLevelLocals *Level, bool fromlist);
	bool LoadBots ();
	bool EnsureRosterLoaded();
	void ForgetBots ();

	// Reconcile is authoritative-host work: additions go through the existing
	// replicated SpawnBot path. A companion identity is a replicated stable ID,
	// never an ordinal or reusable player slot.
	int GetDesiredCompanionCount() const;
	void SetDesiredCompanionCount(int count);
	int GetCompanionCapacity() const;
	int CountCompanions() const;
	int CountPendingCompanions() const;
	int GetRosterCount() const;
	const char *GetRosterName(int index) const;
	int GetRosterState(int index) const;
	// Kept for old console callers only. New network removal records must use
	// the stable-ID methods below, since ordinals move when another companion
	// leaves first.
	int GetCompanionSlotByOrdinal(unsigned int ordinal) const;
	int GetCompanionSlotById(uint32_t companionId) const;
	int GetCompanionSlotByProfile(int profile) const;
	uint32_t GetCompanionIdByProfile(int profile) const;
	const char *GetCompanionNameByProfile(int profile) const;
	bool IsCompanionProfilePending(int profile) const;
	// A promoted network host owns a different local bots.cfg instance than
	// the previous host. Rebuild its local availability flags from the live
	// replicated bot players before it can choose another random identity.
	void RehydrateCompanionRosterUsage();
	// A map with no collision-clear companion position must never retain a
	// PlayerInGame slot without a pawn: many engine systems correctly assume
	// that invariant. Keep the desired roster, suppress retries for this map,
	// and try again after BeginCompanionMap() on the next load.
	void BeginCompanionMap();
	void RejectUnplaceableCompanion(FLevelLocals *Level, unsigned int player, bool notifyDisconnect);
	// The host queues an exact stable ID. This also accepts a locally waiting
	// reservation: event order guarantees its DEM_ADDBOT is applied before the
	// following remove record on every peer.
	int QueueCompanionRemovals(int target);
	bool QueueCompanionRemoval(uint32_t companionId);
	void NotifyCompanionRemovalProcessed(uint32_t companionId);
	bool ReconcileCompanions(FLevelLocals *Level);
	bool RemoveCompanionById(FLevelLocals *Level, uint32_t companionId, bool keepTeam = false);
	bool RemoveCompanion(FLevelLocals *Level, unsigned int ordinal, bool keepTeam = false);
	bool UsingBuiltInRoster() const { return using_builtin_roster; }
	// FLevelLocals persists this with save/hub state. It records that the local
	// session entered co-op solely because of its companion squad.
	bool HasCompanionForcedMultiplayer() const { return companion_forced_multiplayer; }
	void SetCompanionForcedMultiplayer(bool value) { companion_forced_multiplayer = value; }

	//(b_func.cpp)
	void StartTravel ();
	void FinishTravel ();
	bool IsLeader (player_t *player);
	void SetBodyAt (FLevelLocals *Level, const DVector3 &pos, int hostnum);
	double FakeFire (AActor *source, AActor *dest, usercmd_t *cmd);
	bool SafeCheckPosition (AActor *actor, double x, double y, FCheckPosition &tm);
	void BotTick(AActor *mo);

	//(b_move.cpp)
	bool CleanAhead (AActor *thing, double x, double y, usercmd_t *cmd,
		const DVector2 *plannedDescentGoal = nullptr, double plannedDescentFloor = 0.0,
		EBotMoveBlockReason *blockReason = nullptr);
	bool IsDangerous (sector_t *sec);
	bool IsDangerous(AActor *actor, sector_t *sec, const DVector2 &position, double floorz);

	TArray<FString> getspawned; //Array of bots (their names) which should be spawned when starting a game.
	
	//uint8_t freeze;			//Game in freeze mode.
	//uint8_t changefreeze;	//Game wants to change freeze mode.
	int botnum;
	botinfo_t *botinfo;
	int spawn_tries;
	int wanted_botnum;
	TObjPtr<AActor*> firstthing;
	TObjPtr<AActor*>	body1;
	TObjPtr<AActor*> body2;

	bool	 m_Thinking;

private:
	//(b_game.cpp)
	bool DoAddBot (FLevelLocals *Level, TArrayView<uint8_t> info, botskill_t skill,
		int companionProfile, uint32_t companionId);
	int LoadBuiltInBots();
	bool RemoveCompanionAt(FLevelLocals *Level, unsigned int player, bool keepTeam);
	int CountRosterEntries() const;
	uint32_t AllocateCompanionJoinOrder();
	bool HasProfileManagedCompanions() const;
	bool IsCompanionRemovalPending(uint32_t companionId) const;
	// A bot can promote a local single-player map to co-op after it has loaded.
	// Remember that exceptional promotion so removing the final companion can
	// restore the original local mode without touching an intentional co-op or
	// network session.
	void RestoreSinglePlayerModeIfIdle();

protected:
	bool	 ctf;
	int		 t_join;
	uint32_t next_companion_join_order = 1;
	TArray<uint32_t> pending_companion_removals;
	bool companion_spawns_blocked = false;
	bool companion_forced_multiplayer = false;
	// Set only when a complete DEM_ADDBOT could not fit in this tic's atomic
	// event budget. Callers retain the requested companion and retry later;
	// ordinary roster/configuration failures still follow their normal fallback.
	bool spawn_event_rejected = false;
	bool using_builtin_roster = false;
	bool roster_load_attempted = false;
};

class DBot : public DThinker
{
	DECLARE_CLASS(DBot,DThinker)
	HAS_OBJECT_POINTERS
public:
	static const int DEFAULT_STAT = STAT_BOT;
	void Construct ();

	void Clear ();
	// Route sectors, waypoints, and movement waits describe one loaded level.
	// Keep the player/skill/profile identity across a transition, but discard
	// those transient decisions after the pawn is relinked into the next map.
	void ResetNavigationAfterTravel();
	void Serialize(FSerializer &arc);
	void Tick ();

	//(b_think.cpp)
	void WhatToGet (AActor *item);

	//(b_func.cpp)
	bool Check_LOS (AActor *to, DAngle vangle);

	player_t	*player;
	DAngle		Angle;		// The wanted angle that the bot try to get every tic.
							//  (used to get a smooth view movement)
	TObjPtr<AActor*>		dest;		// Move Destination.
	TObjPtr<AActor*>		prev;		// Previous move destination.
	TObjPtr<AActor*>		enemy;		// The dead meat.
	TObjPtr<AActor*>		missile;	// A threatening missile that needs to be avoided.
	TObjPtr<AActor*>		mate;		// Friend (used for grouping in teamplay or coop).
	TObjPtr<AActor*>		last_mate;	// If bots mate disappeared (not if died) that mate is
							// pointed to by this. Allows bot to roam to it if
							// necessary.

	// A route is deliberately only one sector transition long. The normal
	// collision-aware rover still performs the movement, but this guide stops
	// it treating a straight line through a whole map as its only option.
	TObjPtr<AActor*> route_target;
	DVector2 route_waypoint;
	double route_waypoint_floor;
	int route_waypoint_sector;
	int route_target_sector;
	bool route_waypoint_valid;
	bool route_waypoint_allows_planned_descent;
	bool route_waypoint_uses_planned_descent;
	// A closed, verified movement link is discovered while planning movement,
	// but the real use trace runs on the following player tic after the queued
	// turn. Keep the line and the side from which it was selected only until
	// that final yaw has been validated. A line pointer alone is insufficient:
	// a use trace that reaches its reverse side may activate a different action
	// (or be rejected by P_UseLines entirely).
	line_t *pending_navigation_use_line = nullptr;
	int pending_navigation_use_side = -1;

	// Per-think descent proof for the final movement-command guard. It is not
	// serialized: restored bots must plan against the current map geometry.
	DVector2 planned_descent_goal;
	double planned_descent_floor;
	bool planned_descent_active;

	//Skills
	struct botskill_t	skill;

	//Tickers
	int			t_active;	// Open door, lower lift stuff, door must open and
							// lift must go down before bot does anything
							// radical like try a stuckmove
	int			t_respawn;
	int			t_strafe;
	int			t_react;
	int			t_fight;
	int			t_roam;
	int			t_rocket;
	int			t_route;
	int			t_stuck;
	int			t_blocked_direction;
	int			blocked_move_direction;
	// Consecutive unreachable, long-distance follow ticks. This lets a
	// companion recover from a lost route without turning every tight corner
	// into an immediate teleport.
	int			t_follow;
	// A successful NewChaseDir probe chooses a world-space lane, but player
	// commands are consumed on the following tic after their yaw command has
	// been applied. Keep the lane until Think has that final yaw, then project
	// it into forward/side input exactly once. This is intentionally transient:
	// Think clears it before planning each command, so there is no unfinished
	// recovery action to serialize across a save or map transition.
	int			pendingRecoveryMoveDirection;

	//Misc booleans
	bool		first_shot;	// Used for reaction skill.
	bool		sleft;		// If false, strafe is right.
	bool		allround;
	bool		increase;
	// Captured from the completed command at the end of a tic. The next pass can
	// tell a real collision from a deliberate door/lift wait and re-plan early.
	bool		movement_requested;

	// Assigned in the replicated DEM_ADDBOT record. It survives player rebirths
	// and savegames, so removals name the same bot on every peer.
	int CompanionProfile = FCajunMaster::NoCompanionProfile;
	uint32_t CompanionJoinOrder = FCajunMaster::NoCompanionId;

	DVector2	old;

private:
	//(b_think.cpp)
	void Think ();
	void ThinkForMove (usercmd_t *cmd);
	void Set_enemy ();

	//(b_func.cpp)
	bool Reachable(AActor *target, bool allowPlannedDescent = false,
		double *plannedLandingFloor = nullptr, bool *usesPlannedDescent = nullptr);
	void Dofire (usercmd_t *cmd);
	// Test the ray which the queued player command will actually fire, rather
	// than treating the enemy's centre as the firing direction. This keeps the
	// friendly-fire guard correct while a bot is still turning or leading a
	// projectile target.
	bool CanFireAt(AActor *target, const BotInfoData &weaponInfo,
		DAngle firingYaw, DAngle firingPitch) const;
	void ValidateAttackCommand(usercmd_t *cmd, DAngle nextTickYaw,
		DAngle nextTickPitch);
	AActor *Choose_Mate ();
	AActor *Find_enemy ();
	DAngle FireRox (AActor *enemy, usercmd_t *cmd);
	bool TryCatchUpToMate();

	//(b_move.cpp)
	void Roam (usercmd_t *cmd);
	bool Move (usercmd_t *cmd);
	bool TryWalk (usercmd_t *cmd);
	void NewChaseDir (usercmd_t *cmd, const DVector2 *goal = nullptr);
	bool FindRouteWaypoint(AActor *target, DVector2 &waypoint,
		double &waypointFloor, bool &usesPlannedDescent, bool allowPlannedDescent);
	void ApplyPendingRecoveryMoveCommand(usercmd_t *cmd, DAngle nextTickYaw);
	void ValidateMovementCommand(usercmd_t *cmd, DAngle nextTickYaw);
	void ValidateNavigationUseCommand(usercmd_t *cmd, DAngle nextTickYaw);
	void TurnToAng ();
	void Pitch (AActor *target);
};


//Externs
extern cycle_t BotThinkCycles, BotSupportCycles;

// Persisted companion profiles are intentionally independent of a map recipe:
// the same squad can join an ordinary IWAD map or a generated map. A profile
// index is zero based and corresponds to the numbered menus shown to players.
const char *BotCompanionProfileName(int profile);
// Used by the lifecycle only when an old/stale configured identity must be
// replaced by a verified available roster entry. It commits the name before a
// replacement bot event is queued, so a busy network tic cannot create a bot
// whose identity is lost on the next map. Menu edits remain guarded by their
// host-only native API.
bool BotCompanionSetProfileName(int profile, const char *name);
const char *BotCompanionProfileSkin(int profile);
int BotCompanionProfileStyle(int profile);
// True when another configured squad position reserves this identity. The
// caller supplies the number of active/planned positions so unused later
// profiles do not starve a random earlier companion.
bool BotCompanionProfileNameReserved(const char *name, int exceptProfile, int profileLimit);

// The profile-membership mask is the canonical desired companion roster.
// It deliberately supports sparse profile selections (for example profiles
// 1 and 4 only), which a scalar target count cannot represent.
bool BotCompanionProfileEnabled(int profile);
int BotCompanionEnabledProfileCount();
int BotCompanionEnabledProfileMask();
void BotCompanionNormalizeProfileMask();
bool BotCompanionSetEnabled(int profile, bool enabled);
void BotCompanionClearProfile(int profile);
bool BotCompanionClearAllProfiles();
void BotApplyCompanionSkillPreset(int profile, botskill_t &skill);
int BotCompanionProfileSkill(int profile);

EXTERN_CVAR (Float, bot_flag_return_time)
EXTERN_CVAR (Int, bot_next_color)
EXTERN_CVAR (Bool, bot_companion_respawn)

#endif	// __B_BOT_H__
