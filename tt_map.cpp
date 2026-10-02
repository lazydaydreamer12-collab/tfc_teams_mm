// tt_map.cpp - which teams this map actually has.
//
// Everything comes from the map's own entities, read as the engine hands
// them to the game (pfnKeyValue, before any of them spawn):
//
//   info_tfdetect   number_of_teams        how many teams
//                   team1_name..team4_name
//                   ammo_medikit           team 1 player limit   } TFC reuses
//                   ammo_detpack           team 2 player limit   } Quake TF's
//                   maxammo_medikit        team 3 player limit   } ammo keys for
//                   maxammo_detpack        team 4 player limit   } these.
//                   maxammo_shells/nails/rockets/cells
//                                          teams 1-4 illegal classes; -1 means
//                                          civilians only (a VIP team, as on
//                                          hunted) - never balanced into.
//   info_player_teamspawn / i_p_t   team_no   which teams have spawn points
//
// The key-to-team mapping is not folklore: it is ParseTFDetect() in tfc.so
// (0xfec60), which copies entity fields 0xd4/0xdc/0xd0/0xe0 into the team
// limit array and 0xd8/0xe8/0xf8/0xf0 into illegalclasses, and
// CBaseEntity::KeyValuePartTwo, which stores exactly those keys into exactly
// those fields. A limit of 0 means "none" (ParseTFDetect turns it into 100).
//
// Plugin loaded after the map was already up (meta load mid-game)? The
// KeyValue calls have been and gone, so the same entities are read from the
// .bsp's entity lump instead.

#include <stdlib.h>
#include <string.h>

#include "tt_common.h"

TTMapTeams g_map;

// The KeyValue stream is one entity at a time: classname first as a rule, but
// the engine does not promise that, so we collect an entity's keys and only
// act on them once we know what it is. The engine passes pkvd->szClassName on
// every call anyway, which is what we use.
static void TT_MapKey(const char *cls, const char *key, const char *val)
{
	if (!cls || !key || !val)
		return;

	if (!strcasecmp(cls, "worldspawn"))
	{
		// Proof the KeyValue stream for this map reached us, even on a map
		// with no TFC entities at all.
		g_map.seenAny = true;
		return;
	}

	if (!strcasecmp(cls, "info_tfdetect"))
	{
		g_map.seenAny = true;
		if (!strcasecmp(key, "number_of_teams"))
			g_map.numberOfTeams = atoi(val);
		else if (!strcasecmp(key, "ammo_medikit"))    g_map.limit[1] = atoi(val);
		else if (!strcasecmp(key, "ammo_detpack"))    g_map.limit[2] = atoi(val);
		else if (!strcasecmp(key, "maxammo_medikit")) g_map.limit[3] = atoi(val);
		else if (!strcasecmp(key, "maxammo_detpack")) g_map.limit[4] = atoi(val);
		else if (!strcasecmp(key, "maxammo_shells"))  g_map.illegal[1] = atoi(val);
		else if (!strcasecmp(key, "maxammo_nails"))   g_map.illegal[2] = atoi(val);
		else if (!strcasecmp(key, "maxammo_rockets")) g_map.illegal[3] = atoi(val);
		else if (!strcasecmp(key, "maxammo_cells"))   g_map.illegal[4] = atoi(val);
		else if (!strncasecmp(key, "team", 4) && key[4] >= '1' && key[4] <= '4'
			&& !strcasecmp(key + 5, "_name"))
		{
			int t = key[4] - '0';
			strncpy(g_map.name[t], val, sizeof(g_map.name[t]) - 1);
			g_map.name[t][sizeof(g_map.name[t]) - 1] = 0;
		}
		return;
	}

	if (!strcasecmp(cls, "info_player_teamspawn") || !strcasecmp(cls, "i_p_t"))
	{
		g_map.seenAny = true;
		if (!strcasecmp(key, "team_no"))
		{
			int t = atoi(val);
			if (t >= 1 && t <= TT_MAX_TEAMS)
				g_map.spawnMask |= 1u << t;
		}
	}
}

void TT_MapReset(void)
{
	memset(&g_map, 0, sizeof(g_map));
}

void TT_MapKeyValue(edict_t *pent, KeyValueData *pkvd)
{
	if (!pkvd)
		return;
	// A new map's worldspawn arrives before any other entity: that is the
	// moment to forget the last map. (ServerActivate is too late - it runs
	// after every KeyValue.) Every worldspawn key resets, not just one named
	// key, because nothing else has been read yet at that point and it does
	// not depend on which keys the engine chooses to pass.
	if (pkvd->szClassName && !strcasecmp(pkvd->szClassName, "worldspawn"))
		TT_MapReset();
	TT_MapKey(pkvd->szClassName, pkvd->szKeyName, pkvd->szValue);
}

// ---------------------------------------------------------------------------
// Fallback: the entity lump of maps/<map>.bsp (v30: header is version + 15
// lumps of {offset, length}; lump 0 is the entity text).
static void TT_MapFromBsp(void)
{
	char path[128];
	_snprintf_wc(path, sizeof(path) - 1, "maps/%s.bsp", STRING(gpGlobals->mapname));
	path[sizeof(path) - 1] = 0;
	int len = 0;
	byte *data = LOAD_FILE_FOR_ME(path, &len);
	if (!data)
	{
		TT_Trace("Map: %s could not be read for the entity fallback", path);
		return;
	}
	if (len < 8 + 15 * 8)
	{
		FREE_FILE(data);
		return;
	}
	int ofs = *(int *)(data + 4);
	int size = *(int *)(data + 8);
	if (ofs < 0 || size <= 0 || ofs + size > len)
	{
		FREE_FILE(data);
		TT_Trace("Map: %s has a bad entity lump header", path);
		return;
	}

	// Walk { "key" "value" ... } blocks. Two passes per entity: find its
	// classname, then feed every pair.
	const char *p = (const char *)data + ofs;
	const char *end = p + size;
	while (p < end)
	{
		while (p < end && *p != '{')
			p++;
		if (p >= end)
			break;
		const char *blockStart = ++p;
		const char *blockEnd = blockStart;
		while (blockEnd < end && *blockEnd != '}')
			blockEnd++;

		char cls[64] = "";
		for (int pass = 0; pass < 2; pass++)
		{
			const char *q = blockStart;
			while (q < blockEnd)
			{
				char kv[2][128];
				int got = 0;
				for (; got < 2 && q < blockEnd; )
				{
					while (q < blockEnd && *q != '"')
						q++;
					if (q >= blockEnd)
						break;
					q++;
					int n = 0;
					while (q < blockEnd && *q != '"')
					{
						if (n < 127)
							kv[got][n++] = *q;
						q++;
					}
					kv[got][n] = 0;
					q++;
					got++;
				}
				if (got < 2)
					break;
				if (pass == 0)
				{
					if (!strcasecmp(kv[0], "classname"))
					{
						strncpy(cls, kv[1], sizeof(cls) - 1);
						cls[sizeof(cls) - 1] = 0;
					}
				}
				else if (cls[0])
					TT_MapKey(cls, kv[0], kv[1]);
			}
		}
		p = blockEnd + 1;
	}
	FREE_FILE(data);
	TT_Trace("Map: team layout read from %s (plugin loaded after the map started)", path);
}

void TT_MapFinish(void)
{
	if (!g_map.seenAny)
		TT_MapFromBsp();

	// How many teams: info_tfdetect says, else the highest team with spawns.
	int n = g_map.numberOfTeams;
	if (n <= 0)
	{
		for (int t = TT_MAX_TEAMS; t >= 1; t--)
			if (g_map.spawnMask & (1u << t)) { n = t; break; }
	}
	if (n > TT_MAX_TEAMS)
		n = TT_MAX_TEAMS;

	g_map.playableCount = 0;
	g_map.playableMask = 0;
	for (int t = 1; t <= n; t++)
	{
		// A civilian-only team (illegal classes -1) is a VIP team; moving
		// people into or out of it would break the map.
		if (g_map.illegal[t] == -1)
			continue;
		// A team with no spawn points cannot be played, when the map has
		// team spawns at all.
		if (g_map.spawnMask && !(g_map.spawnMask & (1u << t)))
			continue;
		g_map.playable[g_map.playableCount++] = t;
		g_map.playableMask |= 1u << t;
	}

	TT_Trace("Map %s: number_of_teams=%d spawnMask=0x%x playable=%d [%s%s%s%s] limits=%d/%d/%d/%d illegal=%d/%d/%d/%d",
		STRING(gpGlobals->mapname), g_map.numberOfTeams, g_map.spawnMask, g_map.playableCount,
		(g_map.playableMask & 2) ? "1" : "", (g_map.playableMask & 4) ? "2" : "",
		(g_map.playableMask & 8) ? "3" : "", (g_map.playableMask & 16) ? "4" : "",
		g_map.limit[1], g_map.limit[2], g_map.limit[3], g_map.limit[4],
		g_map.illegal[1], g_map.illegal[2], g_map.illegal[3], g_map.illegal[4]);
}

bool TT_TeamPlayable(int team)
{
	return team >= 1 && team <= TT_MAX_TEAMS && (g_map.playableMask & (1u << team)) != 0;
}

int TT_TeamLimit(int team)
{
	if (team < 1 || team > TT_MAX_TEAMS)
		return 0;
	return g_map.limit[team] > 0 ? g_map.limit[team] : 0;
}

const char *TT_TeamName(int team)
{
	static const char *defaults[] = { "Spectator", "Blue", "Red", "Yellow", "Green" };
	if (team < 0 || team > TT_MAX_TEAMS)
		return "?";
	if (team >= 1 && g_map.name[team][0] && g_map.name[team][0] != '#')
		return g_map.name[team];
	return defaults[team];
}
