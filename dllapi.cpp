// dllapi.cpp - the game-DLL hooks. All pre-hooks (metamod runs them before
// TFC's own function). Every one ends in RETURN_META - a hook that returns
// without setting a result leaves the previous call's in place, which can make
// metamod skip the game's own function (see Weapon Changer Plus's dllapi.cpp
// for the 84MB log that taught us this).

#include <extdll.h>
#include <dllapi.h>
#include <meta_api.h>

#include <stdlib.h>
#include <string.h>

#include "tt_common.h"
#include "tt_stats.h"

// Every entity key the map sets, before the entities spawn. The only place
// info_tfdetect's team limits and civilian teams can be seen - TFC keeps them
// in private globals afterwards.
static void TT_KeyValue(edict_t *pentKeyvalue, KeyValueData *pkvd)
{
	TT_MapKeyValue(pentKeyvalue, pkvd);
	RETURN_META(MRES_IGNORED);
}

void TT_WarnForeignCopies(void); // meta_api.cpp

void TT_OnMapStart(void)
{
	g_mapStartTime = gpGlobals->time;
	TT_WarnForeignCopies();
	TT_ConfigLoad();
	TT_MapFinish();
	TT_PlayersReset();
	TT_BalanceReset();
	TT_ScrambleReset();
	TT_StatsMapStart();
}

static void TT_ServerActivate(edict_t *pEdictList, int edictCount, int clientMax)
{
	TT_Trace("==== ServerActivate: %s ====", STRING(gpGlobals->mapname));
	TT_OnMapStart();
	RETURN_META(MRES_IGNORED);
}

static void TT_ClientPutInServer(edict_t *pEntity)
{
	TT_PlayerConnect(pEntity);
	TT_StatsPlayerConnect(pEntity);
	RETURN_META(MRES_IGNORED);
}

static void TT_ClientDisconnect(edict_t *pEntity)
{
	TT_StatsPlayerDisconnect(pEntity);
	TT_PlayerDisconnect(pEntity);
	RETURN_META(MRES_IGNORED);
}

static void TT_ClientCommand(edict_t *pEntity)
{
	if (!g_tt.enabled || FNullEnt(pEntity))
		RETURN_META(MRES_IGNORED);

	const char *cmd = CMD_ARGV(0);
	if (!cmd)
		RETURN_META(MRES_IGNORED);

	// The VGUI team menu sends "jointeam <n>" (TFC's ClientCommand ->
	// TeamFortress_TeamSet; 5 = auto-assign). Only real clients' commands
	// arrive here - ours and bots' go straight to the game DLL.
	if (!strcasecmp(cmd, "jointeam") && CMD_ARGC() > 1)
	{
		int team = atoi(CMD_ARGV(1));
		if (TT_JoinTeamBlocked(pEntity, team))
			RETURN_META(MRES_SUPERCEDE);
		RETURN_META(MRES_IGNORED);
	}

	// A key picked in our stats / rankings menu (TT_ShowMenu).
	if (!strcasecmp(cmd, "menuselect") && CMD_ARGC() > 1)
	{
		if (TT_StatsMenuSelect(pEntity, atoi(CMD_ARGV(1))))
			RETURN_META(MRES_SUPERCEDE);
		RETURN_META(MRES_IGNORED);
	}

	if (!strcasecmp(cmd, "say") || !strcasecmp(cmd, "say_team"))
	{
		if (TT_ChatCommand(pEntity, CMD_ARGS()))
			RETURN_META(MRES_SUPERCEDE);
		RETURN_META(MRES_IGNORED);
	}

	RETURN_META(MRES_IGNORED);
}

static void TT_StartFrame(void)
{
	if (g_tt.enabled)
	{
		TT_PlayersFrame();
		TT_StatsFrame();
		TT_ScrambleFrame();
		TT_BalanceFrame();
	}
	RETURN_META(MRES_IGNORED);
}

// Before TFC's own PreThink, which zeroes the damage the player took last
// frame - see tt_stats.cpp.
static void TT_PlayerPreThink(edict_t *pEntity)
{
	if (g_tt.enabled)
		TT_StatsPreThink(pEntity);
	RETURN_META(MRES_IGNORED);
}

// Map over: rate this map and save (the summary itself goes out at intermission).
static void TT_ServerDeactivate(void)
{
	TT_StatsMapEnd();
	RETURN_META(MRES_IGNORED);
}

static DLL_FUNCTIONS gFunctionTable;

C_DLLEXPORT FORCE_STACK_ALIGN int GetEntityAPI2(DLL_FUNCTIONS *pFunctionTable,
		int *interfaceVersion)
{
	if (!pFunctionTable)
	{
		UTIL_LogPrintf("GetEntityAPI2 called with null pFunctionTable");
		return FALSE;
	}
	else if (*interfaceVersion != INTERFACE_VERSION)
	{
		UTIL_LogPrintf("GetEntityAPI2 version mismatch; requested=%d ours=%d", *interfaceVersion, INTERFACE_VERSION);
		*interfaceVersion = INTERFACE_VERSION;
		return FALSE;
	}
	// By name - a positional initialiser that miscounts hooks the wrong slot.
	memset(&gFunctionTable, 0, sizeof(gFunctionTable));
	gFunctionTable.pfnKeyValue          = TT_KeyValue;
	gFunctionTable.pfnServerActivate    = TT_ServerActivate;
	gFunctionTable.pfnClientPutInServer = TT_ClientPutInServer;
	gFunctionTable.pfnClientDisconnect  = TT_ClientDisconnect;
	gFunctionTable.pfnClientCommand     = TT_ClientCommand;
	gFunctionTable.pfnStartFrame        = TT_StartFrame;
	gFunctionTable.pfnPlayerPreThink    = TT_PlayerPreThink;
	gFunctionTable.pfnServerDeactivate  = TT_ServerDeactivate;
	memcpy(pFunctionTable, &gFunctionTable, sizeof(DLL_FUNCTIONS));
	return TRUE;
}
