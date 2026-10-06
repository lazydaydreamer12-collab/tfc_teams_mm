// tt_common.h - shared state for TFC Teams (auto-balance + scramble vote).
//
// HOW A PLAYER IS MOVED, AND WHY IT IS DONE THIS WAY
//
// TFC does not keep a player's team in pev->team. It keeps it in its own
// CBasePlayer member (team_no, this+0x138 in the Linux tfc.so) and only copies
// it OUT to pev->team. Writing pev->team moves the scoreboard colour and
// nothing else: spawn points, friendly fire, goals, the "#Game_changedteam"
// message and every team count the game makes all read team_no. That is why
// the earlier scramble plugins on this server that set pev->team directly
// looked like they ran and moved nobody.
//
// So we never touch the team ourselves. We hand the player the game's own
// "jointeam N" command (ClientCommand -> CBasePlayer::TeamFortress_TeamSet(N),
// tfc.so 0x69a23 -> 0xc73c0), which does everything a real team change does,
// then the game's own class command ("soldier" etc. -> ChangeClass) to give
// them back the class TeamSet just took away. Both go straight into the game
// DLL's ClientCommand with CMD_ARGV/CMD_ARGC/CMD_ARGS served by our engine
// hooks (tt_engine.cpp) - the same route AMX Mod X uses for amx_client_cmd,
// and the only one that reaches bots.
//
// WHAT TeamSet DOES, read off tfc.so, which decides WHEN we may call it:
//   - refuses outright while pev->deadflag == DEAD_DYING (the death animation)
//   - refuses a change within 1s of the last one (this+0x738)
//   - refuses a team at its info_tfdetect player limit ("#Game_teamfull")
//   - otherwise: removes buildings, detpacks, live grenades and rockets,
//     sets playerclass = 0, strips items, health = 0, calls Killed() (a
//     suicide in the kill feed and one death on the scoreboard - the frag it
//     costs is handed straight back), sets the new team, colours and skin,
//     and opens the class menu.
// Doing it while the player is already dead (deadflag DEAD_DEAD/RESPAWNABLE)
// is what TFC itself does when a dead player picks "change team", and it is
// what "only on death" means here.
#ifndef TFC_TEAMS_COMMON_H
#define TFC_TEAMS_COMMON_H

// The C/C++ runtime headers go in before extdll.h, which defines min() and
// max() as macros that break libstdc++'s <cmath> if it is included after them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <extdll.h>
#include <meta_api.h>

#include "sdk_compat.h"

#define TT_MAX_PLAYERS 32
#define TT_MAX_TEAMS   4
#define TT_MAX_ADMINS  64
#define TT_TAG         "[Teams]"

// ---------------------------------------------------------------------------
// Settings - tt_config.cpp. Every field has a default, so a missing or partial
// config file still gives a working plugin.
struct TTConfig
{
	int   enabled;
	int   debug;

	// Auto-balance
	int   balanceEnabled;
	int   balanceThreshold;     // act when biggest - smallest >= this
	float balanceDelay;         // the gap must last this long first (s)
	float balancePatience;      // then this long waiting for a preferred pick
	float balanceNewWindow;     // "newest joiner" = joined within this (s)
	int   balanceCandidates;    // how many of the ranked list are preferred
	float balanceImmunity;      // not moved again for this long (s)
	int   balanceIncludeBots;
	int   balancePreferBots;
	float balanceForceAfter;    // nobody on the big team died for this long: move someone alive (0 = never)
	float balanceForceWarn;     // ...after warning them this many seconds first
	int   blockUnevenJoin;
	int   adminImmunity;        // admins are never auto-moved or join-blocked

	// Matching players
	int   balanceBySkill;       // balance and scramble by skill rating (0 = this map's frags)
	int   balancePreferAuto;    // move players who used auto-assign before those who picked their team
	int   joinAutoSkill;        // auto-assign puts a player on the team that evens out skill
	int   pickMode;             // TT_PICK_*: how scrambles and balancing choose who moves
	int   pickMixedChance;      // mixed: % chance each rank-picked move actually happens
	int   namesTrack;           // keep tt_names.txt (every name each SteamID has used)

	// Scramble vote
	int   voteEnabled;
	float votePercent;
	int   voteMinVotes;
	float voteMapStartDelay;    // no vote in the first N seconds of a map
	float voteCooldown;         // no vote for N seconds after a scramble
	float voteAfkTime;          // idle this long = AFK, vote dropped
	int   voteCountSpectators;
	float scrambleMaxWait;      // give up on moves still pending after this
	int   scrambleIncludeBots;
	int   scrambleMode;         // TT_SCR_RESPAWN or TT_SCR_RESET, for votes and plain !forcescramble
	float scrambleResetDelay;   // reset mode: seconds everyone spends in spectator
	int   scrambleResetFrags;   // reset mode: give players their frags back afterwards

	// Chat tips (!scramble, !teams, !stats)
	float advertInterval;       // seconds between tips (0 = off)
	float advertFirst;          // first tip this many seconds into a map
	float advertJoin;           // a tip to each player this long after they first join (0 = off)
	char  adverts[8][192];      // advert "text" lines: replace the built-in tips
	int   advertCount;

	// Admins
	int   amxxUsers;            // also read amxmodx/configs/users.ini
	char  amxxFlag;             // access flag that counts as admin there
	int   adminCount;
	char  admins[TT_MAX_ADMINS][40];
};
extern TTConfig g_tt;

void TT_ConfigDefaults(void);
void TT_ConfigLoad(void);             // defaults, then the file on top
bool TT_IsAdminAuth(const char *authid);

// ---------------------------------------------------------------------------
// Map team layout - tt_map.cpp. Read from the map's own info_tfdetect and
// spawn points as the engine hands them to the game (pfnKeyValue), with the
// .bsp entity lump as a fallback when the plugin was loaded mid-map.
struct TTMapTeams
{
	int      numberOfTeams;               // info_tfdetect number_of_teams, 0 = not set
	int      limit[TT_MAX_TEAMS + 1];     // max players, 0 = none
	int      illegal[TT_MAX_TEAMS + 1];   // illegal-class bits, -1 = civilian-only team
	unsigned spawnMask;                   // bit N = team N has a teamspawn
	char     name[TT_MAX_TEAMS + 1][32];
	bool     seenAny;                     // got anything at all this map

	// Derived by TT_MapFinish().
	int      playableCount;
	int      playable[TT_MAX_TEAMS];      // team numbers, ascending
	unsigned playableMask;
};
extern TTMapTeams g_map;

void TT_MapReset(void);
void TT_MapKeyValue(edict_t *pent, KeyValueData *pkvd);
void TT_MapFinish(void);                  // ServerActivate
bool TT_TeamPlayable(int team);
const char *TT_TeamName(int team);
int  TT_TeamLimit(int team);              // 0 = unlimited

// ---------------------------------------------------------------------------
// Players - tt_players.cpp
struct TTPlayer
{
	bool  inGame;
	float connectedAt;
	int   lastTeam;          // pev->team seen last tick
	float teamJoinedAt;      // when lastTeam became what it is
	float lastActive;        // for AFK
	int   lastButtons;
	float lastYaw, lastPitch;
	bool  afk;
	bool  voted;
	int   pendingTeam;       // scramble target, 0 = none
	int   pendingFrom;       // the team they were on when it was planned
	float movedAt;           // last time WE moved them
	float nextMoveTry;       // retry gate after a refused move
	float nextJoinMsg;       // rate limit on the join-block message
	bool  inReset;           // reset scramble: waiting in spectator to be placed
	int   resetClass;        // the class to give back (0 = let them pick)
	float resetFrags;        // their frags before spectator wiped them
	bool  adminKnown;
	bool  admin;
	bool  joinTipDone;       // the join tip was sent (or isn't needed)
	bool  joinedAuto;        // got onto their team with auto-assign (jointeam 5) this map
	bool  joinChoiceKnown;   // we saw how they joined
};
extern TTPlayer g_pl[TT_MAX_PLAYERS + 1];

edict_t *TT_Player(int idx);              // NULL unless a live, in-game client
bool TT_IsBot(edict_t *p);
bool TT_IsHLTV(edict_t *p);
bool TT_IsAdmin(edict_t *p);
int  TT_PlayerTeam(edict_t *p);           // playable team or 0
bool TT_MovableNow(edict_t *p);           // dead and settled, or no class yet
float TT_Score(edict_t *p);

void TT_PlayersReset(void);
void TT_PlayerConnect(edict_t *p);
void TT_PlayerDisconnect(edict_t *p);
void TT_PlayersFrame(void);               // team-join times, AFK

// counts[team] of players on each playable team. excludeIdx is left out
// (0 = nobody). projected: count scramble moves still pending as done.
void TT_TeamCounts(int counts[TT_MAX_TEAMS + 1], float scores[TT_MAX_TEAMS + 1],
	int excludeIdx, bool projected);

// Move one player to `team` with the game's own commands and give them back
// their class. Returns true when the game accepted the team change.
bool TT_MovePlayer(edict_t *p, int team, const char *why);
// Same, with the class to restore given explicitly (a spectator has none).
bool TT_MovePlayerEx(edict_t *p, int team, int restoreClass, const char *why);
// TFC's own "spectate" command, allowed even when allow_spectators is 0.
// Returns true when they ended up on team 0.
bool TT_SendToSpectator(edict_t *p);
// Just the class command, menu hidden; shows the class menu if it is refused.
bool TT_RestoreClass(edict_t *p, int cls);
bool TT_ClassAllowed(int team, int cls, edict_t *ignore);   // map illegal classes + cr_* limits
const char *TT_ClassTitle(int cls);

// ---------------------------------------------------------------------------
// Engine glue - tt_engine.cpp
void TT_FakeClientCommand(edict_t *p, const char *cmd, const char *arg1);
void TT_SuppressVGUIMenus(edict_t *p);    // NULL = stop suppressing
void TT_ShowClassMenu(edict_t *p);
void TT_Say(edict_t *p, const char *fmt, ...);      // chat line to one
void TT_SayAll(const char *fmt, ...);               // chat line to everyone
void TT_Center(edict_t *p, const char *fmt, ...);
void TT_ShowWindow(edict_t *p, const char *text);   // in-game MOTD panel
void TT_ShowMenu(edict_t *p, int keys, int seconds, const char *text);  // numbered HUD menu
void TT_HudText(edict_t *p, int channel, float x, float y, int r, int g, int b, float hold, const char *text);
void TT_Trace(const char *fmt, ...);                // tt_trace.log + metamod log
void TT_LogModuleIdentity(void);
bool TT_ModuleDir(char *out, size_t outLen);        // folder our DLL is in

// ---------------------------------------------------------------------------
// Balance - tt_balance.cpp
void TT_BalanceReset(void);
void TT_BalanceFrame(void);
// ClientCommand "jointeam N" from a real client. true = we refused it.
bool TT_JoinTeamBlocked(edict_t *p, int team);
// "jointeam 5" (Auto Assign) from a real client: true = we placed them ourselves (by skill).
bool TT_AutoAssign(edict_t *p);
// How strong a player is, for matching teams: their skill rating (or this
// map's frags when balance_by_skill is 0).
float TT_Strength(edict_t *p);

// ---------------------------------------------------------------------------
// Names and joins - tt_names.cpp
void TT_NamesInit(void);
void TT_NamesShutdown(void);
void TT_NamesMapStart(void);
void TT_NamesMapEnd(void);
void TT_NamesFrame(void);
void TT_NamesDisconnect(edict_t *p);
void TT_NamesTeamChoice(edict_t *p, bool autoAssign);
bool TT_NamesChat(edict_t *p, const char *rest);
void TT_NamesRegisterCommands(void);
void TT_FeedRegisterCommands(void);

// ---------------------------------------------------------------------------
// Scramble - tt_scramble.cpp
void TT_ScrambleReset(void);
void TT_ScrambleFrame(void);
bool TT_ScrambleActive(void);
bool TT_ChatCommand(edict_t *p, const char *text);  // true = ours
#define TT_SCR_RESPAWN 0   // each mover switches at their next death
#define TT_SCR_NOW     1   // movers switch immediately (living ones die)
#define TT_SCR_RESET   2   // everyone to spectator, then placed on the new teams

// pick_mode: how scrambles and the balancer choose who goes where.
#define TT_PICK_RANK    0   // by rank (skill, or this map's frags) - the most even teams
#define TT_PICK_RANDOM  1   // rank ignored: random teams, random balance picks
#define TT_PICK_MIXED   2   // rank picks the moves, a dice roll decides each one
const char *TT_PickModeName(int mode);
void TT_ScrambleStart(int mode, const char *who);
bool TT_ScrambleResetting(void);
void TT_ScrambleCancel(const char *who);
void TT_PrintStatus(edict_t *to);                    // NULL = server console
void TT_RegisterServerCommands(void);

extern float g_mapStartTime;

#endif // TFC_TEAMS_COMMON_H
