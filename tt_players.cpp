// tt_players.cpp - who is on which team, when they got there, whether they
// are AFK, and the one function that moves somebody (see tt_common.h for why
// it goes through the game's own commands).

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "tt_common.h"
#include "tt_stats.h"

TTPlayer g_pl[TT_MAX_PLAYERS + 1];

// TFC's class numbers (PC_SCOUT = 1 ... PC_ENGINEER = 9) and the ClientCommand
// names that select them - every one of these strings is in ClientCommand()'s
// compare chain in tfc.so. 10 is "randompc" and 11 "civilian"; neither is
// restored (a random pick has already been resolved to a real class by the
// time we look, and civilians only exist on teams we never balance).
static const char *g_classCmd[10] =
{
	NULL, "scout", "sniper", "soldier", "demoman", "medic", "hwguy", "pyro", "spy", "engineer"
};
static const char *g_classTitle[10] =
{
	NULL, "Scout", "Sniper", "Soldier", "Demoman", "Medic", "HWGuy", "Pyro", "Spy", "Engineer"
};

// CLASS LIMITS. TFC decides in CBasePlayer::CantChange (tfc.so 0xc5d90), which
// ChangeClass calls for every class pick:
//  - the map's illegal classes (info_tfdetect maxammo_shells/nails/rockets/
//    cells for teams 1-4), as bits: scout 1, sniper 2, soldier 4, demoman 8,
//    medic 16, hwguy 32, pyro 64, random 128, spy 256, engineer 512 -
//    refused with "#Game_cantplayclass";
//  - then ClassIsRestricted(team, class) (0x104850), the server's cr_<class>
//    cvars: 0 = no limit, -1 = nobody may play it, N = at most N per team
//    (counting everyone on that team who is, or is about to become, that
//    class) - refused with "#Game_enoughofclass".
// A refused pick leaves playerclass untouched, which is how a move sees it.
// TT_ClassAllowed below is the same test from outside, used to rank balance
// candidates and to find bots a class that is open. It cannot see the
// class someone has queued for their next respawn, so the game's own answer
// (did playerclass change?) is still what a move goes by.
static const int g_classBit[10] = { 0, 1, 2, 4, 8, 16, 32, 64, 256, 512 };
static const char *g_classCvar[10] =
{
	NULL, "cr_scout", "cr_sniper", "cr_soldier", "cr_demoman", "cr_medic", "cr_hwguy", "cr_pyro", "cr_spy", "cr_engineer"
};

const char *TT_ClassTitle(int cls)
{
	return (cls >= 1 && cls <= 9) ? g_classTitle[cls] : "no class";
}

bool TT_ClassAllowed(int team, int cls, edict_t *ignore)
{
	if (cls < 1 || cls > 9 || team < 1 || team > TT_MAX_TEAMS)
		return false;
	int ill = g_map.illegal[team];
	if (ill == -1)
		return false; // civilian-only team
	if (ill > 0 && (ill & g_classBit[cls]))
		return false;
	int limit = (int)CVAR_GET_FLOAT(g_classCvar[cls]);
	if (limit == -1)
		return false;
	if (limit <= 0)
		return true;
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (e && e != ignore && TT_PlayerTeam(e) == team && (int)e->v.playerclass == cls)
			n++;
	}
	return n < limit;
}

// Give a player who has just been moved their class back. If the new team
// will not have it (class limit or map rules): a bot is given the first open
// class instead (a bot cannot use the menu), and a person gets the class
// menu and a line saying why. Returns the class they ended up with, 0 = none yet.
static int TT_GiveClass(edict_t *p, int team, int cls)
{
	TT_FakeClientCommand(p, g_classCmd[cls], NULL);
	if ((int)p->v.playerclass == cls)
		return cls;
	if (TT_IsBot(p))
	{
		int start = RANDOM_LONG(1, 9);
		for (int k = 0; k < 9; k++)
		{
			int c = 1 + (start - 1 + k) % 9;
			if (c == cls || !TT_ClassAllowed(team, c, p))
				continue;
			TT_FakeClientCommand(p, g_classCmd[c], NULL);
			if ((int)p->v.playerclass == c)
			{
				TT_Trace("Class: %s cannot be %s on %s (class limit) - given %s instead",
					STRING(p->v.netname), g_classTitle[cls], TT_TeamName(team), g_classTitle[c]);
				return c;
			}
		}
	}
	return 0;
}

static void TT_AskForClass(edict_t *p, int team, int cls)
{
	TT_ShowClassMenu(p);
	TT_Say(p, "%s %s is full on %s - pick another class.", TT_TAG, TT_ClassTitle(cls), TT_TeamName(team));
	TT_Trace("Class: %s cannot be %s on %s (class limit) - class menu shown",
		STRING(p->v.netname), TT_ClassTitle(cls), TT_TeamName(team));
}

edict_t *TT_Player(int idx)
{
	if (idx < 1 || idx > gpGlobals->maxClients || idx > TT_MAX_PLAYERS)
		return NULL;
	edict_t *e = INDEXENT(idx);
	if (FNullEnt(e) || e->free)
		return NULL;
	// Only once the game has put them in the server (ClientPutInServer). A
	// client slot is reused without being cleared: someone connecting into the
	// slot a kicked bot had has that bot's flags (FL_CLIENT, FL_FAKECLIENT) and
	// team on the edict until they are put in the server, which made them look
	// like a bot already on a team while still loading.
	if (!g_pl[idx].inGame)
		return NULL;
	if (!(e->v.flags & FL_CLIENT))
		return NULL;
	if (!e->v.netname || !STRING(e->v.netname)[0])
		return NULL;
	return e;
}

// BOTS. FL_FAKECLIENT is not enough on its own: TFC's CBasePlayer::Spawn
// (tfc.so 0xc65d0) does "pev->flags &= FL_PROXY; pev->flags |= FL_CLIENT" on
// every respawn, which wipes FL_FAKECLIENT until the bot plugin sets it again.
// The engine's own answer - a fake client's auth ID is "BOT" - does not change.
bool TT_IsBot(edict_t *p)
{
	if (FNullEnt(p))
		return false;
	if (p->v.flags & FL_FAKECLIENT)
		return true;
	const char *auth = GETPLAYERAUTHID(p);
	return auth && !strcmp(auth, "BOT");
}
bool TT_IsHLTV(edict_t *p) { return !FNullEnt(p) && (p->v.flags & FL_PROXY) != 0; }

int TT_PlayerTeam(edict_t *p)
{
	if (FNullEnt(p) || TT_IsHLTV(p))
		return 0;
	int t = (int)p->v.team;
	return TT_TeamPlayable(t) ? t : 0;
}

float TT_Score(edict_t *p)
{
	return FNullEnt(p) ? 0.0f : p->v.frags;
}

// "Only on death": the death animation has finished (TeamSet refuses during
// DEAD_DYING) and they have not respawned yet. PlayerDeathThink (tfc.so
// 0xc3110) goes DYING -> DEAD, and to RESPAWNABLE in the same think if no
// button is held; the respawn itself needs a later think, and StartFrame -
// where we act - always runs between two thinks. Someone on a team who has not
// picked a class yet is not in play either, so moving them costs nothing.
bool TT_MovableNow(edict_t *p)
{
	if (FNullEnt(p))
		return false;
	if (p->v.deadflag == DEAD_DYING)
		return false;
	if (p->v.playerclass == 0)
		return true;
	return p->v.deadflag >= DEAD_DEAD;
}

bool TT_IsAdmin(edict_t *p)
{
	if (FNullEnt(p) || TT_IsBot(p))
		return false;
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return false;
	if (g_pl[idx].adminKnown)
		return g_pl[idx].admin;

	// The host of a listen server is always an admin.
	if (!IS_DEDICATED_SERVER() && idx == 1)
	{
		g_pl[idx].adminKnown = true;
		g_pl[idx].admin = true;
		return true;
	}
	const char *auth = GETPLAYERAUTHID(p);
	if (!auth || !auth[0] || !strcasecmp(auth, "STEAM_ID_PENDING")
		|| !strcasecmp(auth, "BOT") || !strcasecmp(auth, "HLTV"))
		return false; // not known yet - ask again later, do not cache
	g_pl[idx].adminKnown = true;
	g_pl[idx].admin = TT_IsAdminAuth(auth);
	return g_pl[idx].admin;
}

static void TT_ResetSlot(int idx)
{
	memset(&g_pl[idx], 0, sizeof(g_pl[idx]));
}

void TT_PlayersReset(void)
{
	memset(g_pl, 0, sizeof(g_pl));
}

// Loaded mid-map ("meta load"): everyone already playing was put in the
// server before we were here to see it. Called once, right after the map
// state is set up; the engine has filled these slots, so their data is real.
void TT_PlayersAdoptExisting(void)
{
	float now = gpGlobals->time;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = INDEXENT(i);
		if (FNullEnt(e) || e->free || !e->pvPrivateData || !(e->v.flags & FL_CLIENT)
			|| !e->v.netname || !STRING(e->v.netname)[0])
			continue;
		TT_ResetSlot(i);
		g_pl[i].inGame = true;
		g_pl[i].connectedAt = now;
		g_pl[i].lastActive = now;
		g_pl[i].lastTeam = -1;
		TT_StatsPlayerConnect(e);
	}
}

// The engine runs a client's PlayerPreThink only once that client is really in
// the game - never for one still loading. Bots arrive by a route no plugin sees:
// FoxBot hands its bots to the game DLL directly (ClientConnect and
// ClientPutInServer straight into tfc.so), so the put-in-server hook never
// fires for them. Their first PreThink is the first sign of them, and a safe
// one: a slot someone is still loading into does not think.
void TT_PlayerThinking(edict_t *p)
{
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS || g_pl[idx].inGame)
		return;
	if (FNullEnt(p) || p->free || !p->pvPrivateData || !p->v.netname || !STRING(p->v.netname)[0])
		return;
	TT_PlayerConnect(p);
	TT_StatsPlayerConnect(p);
}

void TT_PlayerConnect(edict_t *p)
{
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return;
	TT_ResetSlot(idx);
	g_pl[idx].inGame = true;
	g_pl[idx].connectedAt = gpGlobals->time;
	g_pl[idx].lastActive = gpGlobals->time;
	g_pl[idx].lastTeam = -1; // first frame records their team as a fresh join
}

void TT_PlayerDisconnect(edict_t *p)
{
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return;
	TT_ResetSlot(idx);
}

void TT_PlayersFrame(void)
{
	float now = gpGlobals->time;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		TTPlayer &s = g_pl[i];
		if (!e)
		{
			if (s.inGame)
				TT_ResetSlot(i);
			continue;
		}

		int team = (int)e->v.team;
		if (team != s.lastTeam)
		{
			if (s.lastTeam != -1 || TT_TeamPlayable(team))
				TT_Trace("Team: %s is now on %d (%s)%s", STRING(e->v.netname), team,
					TT_TeamName(team), s.lastTeam == -1 ? " [first seen]" : "");
			s.lastTeam = team;
			s.teamJoinedAt = now;
		}

		// AFK: any change of buttons or view angle counts as being here.
		int buttons = e->v.button;
		float yaw = e->v.v_angle.y, pitch = e->v.v_angle.x;
		if (buttons != s.lastButtons || fabs(yaw - s.lastYaw) > 0.5f || fabs(pitch - s.lastPitch) > 0.5f)
		{
			s.lastActive = now;
			if (s.afk)
			{
				s.afk = false;
				TT_Trace("AFK: %s is back", STRING(e->v.netname));
			}
		}
		s.lastButtons = buttons;
		s.lastYaw = yaw;
		s.lastPitch = pitch;
		if (!s.afk && !TT_IsBot(e) && g_tt.voteAfkTime > 0 && now - s.lastActive >= g_tt.voteAfkTime)
		{
			s.afk = true;
			TT_Trace("AFK: %s idle for %.0fs", STRING(e->v.netname), now - s.lastActive);
		}
	}
}

void TT_TeamCounts(int counts[TT_MAX_TEAMS + 1], float scores[TT_MAX_TEAMS + 1],
	int excludeIdx, bool projected)
{
	for (int t = 0; t <= TT_MAX_TEAMS; t++)
	{
		counts[t] = 0;
		if (scores)
			scores[t] = 0;
	}
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		if (i == excludeIdx)
			continue;
		edict_t *e = TT_Player(i);
		if (!e || TT_IsHLTV(e))
			continue;
		int t = TT_PlayerTeam(e);
		// Projected: where pending scramble moves will put people - including
		// a reset scramble's players, who are in spectator (team 0) meanwhile.
		if (projected && g_pl[i].pendingTeam && TT_TeamPlayable(g_pl[i].pendingTeam))
			t = g_pl[i].pendingTeam;
		if (!t)
			continue;
		counts[t]++;
		if (scores)
			scores[t] += TT_Score(e);
	}
}

bool TT_MovePlayer(edict_t *p, int team, const char *why)
{
	if (FNullEnt(p))
		return false;
	return TT_MovePlayerEx(p, team, (int)p->v.playerclass, why);
}

bool TT_MovePlayerEx(edict_t *p, int team, int cls, const char *why)
{
	if (FNullEnt(p) || !TT_TeamPlayable(team))
		return false;
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return false;
	float now = gpGlobals->time;
	int from = (int)p->v.team;
	float fragsBefore = p->v.frags;
	int deadBefore = p->v.deadflag;
	if (from == team)
		return true;
	if (p->v.deadflag == DEAD_DYING)
		return false; // TeamSet would refuse; try again next frame

	char num[8];
	_snprintf_wc(num, sizeof(num) - 1, "%d", team);
	num[sizeof(num) - 1] = 0;

	bool restore = (cls >= 1 && cls <= 9);
	TT_SuppressVGUIMenus(restore ? p : NULL);
	TT_StatsIgnoreDeathsOf(p);
	TT_FakeClientCommand(p, "jointeam", num);
	TT_StatsIgnoreDeathsOf(NULL);
	bool ok = ((int)p->v.team == team);
	bool classBack = false;
	if (ok && restore)
		classBack = TT_GiveClass(p, team, cls) != 0;
	TT_SuppressVGUIMenus(NULL);
	// The class could not be given back (the new team is at its limit for
	// it): the menu we hid is the right thing after all.
	if (ok && restore && !classBack)
		TT_AskForClass(p, team, cls);

	TT_Trace("Move %s: %s team %d->%d (%s) class %d->%d%s deadflag %d->%d health %.0f frags %.0f->%.0f [%s]",
		ok ? "OK" : "REFUSED", STRING(p->v.netname), from, (int)p->v.team, TT_TeamName(team),
		cls, (int)p->v.playerclass, (restore && !classBack && ok) ? " (class menu shown)" : "",
		deadBefore, p->v.deadflag, p->v.health, fragsBefore, p->v.frags, why ? why : "");

	if (ok)
	{
		g_pl[idx].movedAt = now;
		// Record the new team as already seen, so our own move is not taken
		// for the player choosing to join it - "newest joiner" means people
		// who picked a team themselves.
		g_pl[idx].lastTeam = team;
	}
	else
		g_pl[idx].nextMoveTry = now + 1.5f;
	return ok;
}

// "spectate" (tfc.so ClientCommand 0x6863e) goes to StartObserver() only when
// allow_spectators is non-zero and the player has not changed team in the last
// second. StartObserver sets team 0, playerclass 0, deadflag RESPAWNABLE,
// removes their buildings and items - and sets their frags to 0, taking them
// off the team total too, which is why a reset scramble saves and restores
// pev->frags. allow_spectators is lifted for this one call by writing the
// game's own cvar value directly: the game reads that float (0x183bf8 is the
// cvar_t registered as "allow_spectators"), and going through CVAR_SET_FLOAT
// would announce "Server cvar changed" to everybody twice.
bool TT_SendToSpectator(edict_t *p)
{
	if (FNullEnt(p))
		return false;
	if ((int)p->v.team == 0)
		return true;
	cvar_t *allow = CVAR_GET_POINTER("allow_spectators");
	float old = allow ? allow->value : 1.0f;
	if (allow && allow->value == 0.0f)
		allow->value = 1.0f;
	TT_FakeClientCommand(p, "spectate", NULL);
	if (allow)
		allow->value = old;
	bool ok = ((int)p->v.team == 0);
	int idx = ENTINDEX(p);
	if (idx >= 1 && idx <= TT_MAX_PLAYERS)
		g_pl[idx].lastTeam = (int)p->v.team; // ours, not a choice they made
	TT_Trace("Spectate %s: %s (allow_spectators was %.0f) team now %d class %d deadflag %d frags %.0f",
		ok ? "OK" : "REFUSED", STRING(p->v.netname), old, (int)p->v.team,
		(int)p->v.playerclass, p->v.deadflag, p->v.frags);
	return ok;
}

bool TT_RestoreClass(edict_t *p, int cls)
{
	if (FNullEnt(p) || cls < 1 || cls > 9)
		return false;
	int team = (int)p->v.team;
	TT_SuppressVGUIMenus(p);
	bool ok = TT_GiveClass(p, team, cls) != 0;
	TT_SuppressVGUIMenus(NULL);
	if (!ok)
		TT_AskForClass(p, team, cls);
	TT_Trace("Class %s: %s class %d (wanted %d)", ok ? "OK" : "REFUSED", STRING(p->v.netname),
		(int)p->v.playerclass, cls);
	return ok;
}
