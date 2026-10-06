// tt_config.cpp - configs/tfc_teams.ini, and who counts as an admin.
//
// The file is "key value" per line; blank lines and lines starting with
// //, # or ; are ignored, as is anything after a // on a line. Unknown keys are
// reported in the trace log rather than silently dropped, so a typo shows up.
// Reloaded at every map start and by "tt_reload".
//
// ADMINS come from two places: "admin STEAM_0:1:2345" lines in this file, and
// (amxx_users 1) AMX Mod X's own users.ini - any SteamID entry whose access
// flags include amxx_flag ("j", AMXX's vote flag, by default). Only SteamID
// entries are taken from users.ini: name and IP entries there rely on AMXX's
// password handling, which this plugin does not have.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tt_common.h"
#include "tt_stats.h"
#include "tt_net.h"

TTConfig g_tt;

void TT_ConfigDefaults(void)
{
	memset(&g_tt, 0, sizeof(g_tt));
	g_tt.enabled             = 1;
	g_tt.debug               = 1;

	g_tt.balanceEnabled      = 1;
	g_tt.balanceThreshold    = 2;
	g_tt.balanceDelay        = 5.0f;
	g_tt.balancePatience     = 30.0f;
	g_tt.balanceNewWindow    = 180.0f;
	g_tt.balanceCandidates   = 3;
	g_tt.balanceImmunity     = 120.0f;
	g_tt.balanceIncludeBots  = 1;
	g_tt.balancePreferBots   = 1;
	g_tt.balanceForceAfter   = 60.0f;
	g_tt.balanceForceWarn    = 10.0f;
	g_tt.blockUnevenJoin     = 1;
	g_tt.adminImmunity       = 0;

	g_tt.voteEnabled         = 1;
	g_tt.votePercent         = 40.0f;
	g_tt.voteMinVotes        = 1;
	g_tt.voteMapStartDelay   = 180.0f;
	g_tt.voteCooldown        = 300.0f;
	g_tt.voteAfkTime         = 60.0f;
	g_tt.voteCountSpectators = 0;
	g_tt.scrambleMaxWait     = 90.0f;
	g_tt.scrambleIncludeBots = 1;
	g_tt.scrambleMode        = 0; // TT_SCR_RESPAWN
	g_tt.scrambleResetDelay  = 3.0f;
	g_tt.scrambleResetFrags  = 1;
	g_tt.advertInterval      = 300;
	g_tt.advertFirst         = 270;  // half way between the RTV plugin's tips
	g_tt.advertJoin          = 40;

	g_tt.balanceBySkill      = 1;
	g_tt.balancePreferAuto   = 1;
	g_tt.joinAutoSkill       = 1;
	g_tt.pickMode            = TT_PICK_RANK;
	g_tt.pickMixedChance     = 50;
	g_tt.namesTrack          = 1;

	g_tt.amxxUsers           = 1;
	TT_StatsConfigDefaults();
	TT_NetConfigDefaults();
	g_tt.amxxFlag            = 'j';
}

static int   ClampI(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }
static float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void AddAdmin(const char *auth, const char *from)
{
	if (!auth || !auth[0])
		return;
	for (int i = 0; i < g_tt.adminCount; i++)
		if (!strcasecmp(g_tt.admins[i], auth))
			return;
	if (g_tt.adminCount >= TT_MAX_ADMINS)
	{
		TT_Trace("Config: admin list full (%d), \"%s\" from %s ignored", TT_MAX_ADMINS, auth, from);
		return;
	}
	strncpy(g_tt.admins[g_tt.adminCount], auth, sizeof(g_tt.admins[0]) - 1);
	g_tt.admins[g_tt.adminCount][sizeof(g_tt.admins[0]) - 1] = 0;
	g_tt.adminCount++;
}

static void ApplyKey(const char *key, const char *val, int line)
{
	int   iv = atoi(val);
	float fv = (float)atof(val);

	if      (!strcasecmp(key, "enabled"))                g_tt.enabled = iv ? 1 : 0;
	else if (!strcasecmp(key, "debug"))                  g_tt.debug = iv ? 1 : 0;
	else if (!strcasecmp(key, "balance_enabled"))        g_tt.balanceEnabled = iv ? 1 : 0;
	else if (!strcasecmp(key, "balance_threshold"))      g_tt.balanceThreshold = ClampI(iv, 2, 16);
	else if (!strcasecmp(key, "balance_delay"))          g_tt.balanceDelay = ClampF(fv, 0, 600);
	else if (!strcasecmp(key, "balance_patience"))       g_tt.balancePatience = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "balance_new_window"))     g_tt.balanceNewWindow = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "balance_candidates"))     g_tt.balanceCandidates = ClampI(iv, 1, 32);
	else if (!strcasecmp(key, "balance_immunity"))       g_tt.balanceImmunity = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "balance_include_bots"))   g_tt.balanceIncludeBots = iv ? 1 : 0;
	else if (!strcasecmp(key, "balance_prefer_bots"))    g_tt.balancePreferBots = iv ? 1 : 0;
	else if (!strcasecmp(key, "balance_force_after"))    g_tt.balanceForceAfter = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "balance_force_warn"))     g_tt.balanceForceWarn = ClampF(fv, 0, 60);
	else if (!strcasecmp(key, "block_uneven_join"))      g_tt.blockUnevenJoin = iv ? 1 : 0;
	else if (!strcasecmp(key, "admin_immunity"))         g_tt.adminImmunity = iv ? 1 : 0;
	else if (!strcasecmp(key, "vote_enabled"))           g_tt.voteEnabled = iv ? 1 : 0;
	else if (!strcasecmp(key, "vote_percent"))           g_tt.votePercent = ClampF(fv, 1, 100);
	else if (!strcasecmp(key, "vote_min_votes"))         g_tt.voteMinVotes = ClampI(iv, 1, 32);
	else if (!strcasecmp(key, "vote_map_start_delay"))   g_tt.voteMapStartDelay = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "vote_cooldown"))          g_tt.voteCooldown = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "vote_afk_time"))          g_tt.voteAfkTime = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "vote_count_spectators"))  g_tt.voteCountSpectators = iv ? 1 : 0;
	else if (!strcasecmp(key, "scramble_max_wait"))      g_tt.scrambleMaxWait = ClampF(fv, 5, 3600);
	else if (!strcasecmp(key, "scramble_include_bots"))  g_tt.scrambleIncludeBots = iv ? 1 : 0;
	else if (!strcasecmp(key, "scramble_mode"))
	{
		if (!strcasecmp(val, "reset") || !strcasecmp(val, "spectator") || iv == 2)
			g_tt.scrambleMode = TT_SCR_RESET;
		else if (!strcasecmp(val, "respawn") || (val[0] == '0' && !val[1]))
			g_tt.scrambleMode = TT_SCR_RESPAWN;
		else
			TT_Trace("Config: line %d: scramble_mode \"%s\" - use respawn or reset", line, val);
	}
	else if (!strcasecmp(key, "scramble_reset_delay"))   g_tt.scrambleResetDelay = ClampF(fv, 1.5f, 30);
	else if (!strcasecmp(key, "scramble_reset_restore_frags")) g_tt.scrambleResetFrags = iv ? 1 : 0;
	else if (!strcasecmp(key, "advert_interval"))        g_tt.advertInterval = (fv <= 0) ? 0 : ClampF(fv, 30, 3600);
	else if (!strcasecmp(key, "advert_first"))           g_tt.advertFirst = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "advert_join"))            g_tt.advertJoin = (fv <= 0) ? 0 : ClampF(fv, 1, 600);
	else if (!strcasecmp(key, "advert"))
	{
		if (g_tt.advertCount < 8 && val[0])
		{
			strncpy(g_tt.adverts[g_tt.advertCount], val, sizeof(g_tt.adverts[0]) - 1);
			g_tt.adverts[g_tt.advertCount][sizeof(g_tt.adverts[0]) - 1] = 0;
			g_tt.advertCount++;
		}
		else if (val[0])
			TT_Trace("Config: line %d: more than 8 advert lines - left out", line);
	}
	else if (!strcasecmp(key, "stats_enabled"))          g_st.enabled = iv ? 1 : 0;
	else if (!strcasecmp(key, "stats_min_map_minutes"))  g_st.minMapMinutes = ClampF(fv, 0, 120);
	else if (!strcasecmp(key, "stats_min_rank_minutes")) g_st.minRankMinutes = ClampF(fv, 0, 6000);
	else if (!strcasecmp(key, "stats_save_interval"))    g_st.saveInterval = ClampF(fv, 30, 3600);
	else if (!strcasecmp(key, "stats_summary"))          g_st.summary = iv < 0 ? 0 : (iv > 2 ? 2 : iv);
	else if (!strcasecmp(key, "stats_points_kill"))      g_st.wKill = fv;
	else if (!strcasecmp(key, "stats_points_carrier_kill")) g_st.wCarrierKill = fv;
	else if (!strcasecmp(key, "stats_points_cap"))       g_st.wCap = fv;
	else if (!strcasecmp(key, "stats_points_pickup"))    g_st.wPickup = fv;
	else if (!strcasecmp(key, "stats_points_teamkill"))  g_st.wTeamkill = fv;
	else if (!strcasecmp(key, "stats_points_heal"))      g_st.wHeal = fv;
	else if (!strcasecmp(key, "stats_window"))           g_st.window = iv < 0 ? 0 : (iv > 2 ? 2 : iv);
	else if (!strcasecmp(key, "stats_menu_time"))        g_st.menuTime = ClampF(fv, 0, 600);
	else if (!strcasecmp(key, "stats_debug_caps"))       g_st.debugCaps = iv ? 1 : 0;
	else if (!strcasecmp(key, "stats_cap_word"))         { strncpy(g_st.capWord, val, sizeof(g_st.capWord) - 1); g_st.capWord[sizeof(g_st.capWord) - 1] = 0; }
	else if (!strcasecmp(key, "stats_return_word"))      { strncpy(g_st.returnWord, val, sizeof(g_st.returnWord) - 1); g_st.returnWord[sizeof(g_st.returnWord) - 1] = 0; }
	else if (!strcasecmp(key, "stats_new_rating"))       g_st.newRating = ClampF(fv, 0, 1000);
	else if (!strcasecmp(key, "amxx_users"))             g_tt.amxxUsers = iv ? 1 : 0;
	else if (!strcasecmp(key, "amxx_flag"))              g_tt.amxxFlag = val[0] ? val[0] : 'j';
	else if (!strcasecmp(key, "admin"))                  AddAdmin(val, "tfc_teams.ini");
	else if (!strcasecmp(key, "balance_by_skill"))       g_tt.balanceBySkill = iv ? 1 : 0;
	else if (!strcasecmp(key, "balance_prefer_auto"))    g_tt.balancePreferAuto = iv ? 1 : 0;
	else if (!strcasecmp(key, "join_auto_skill"))        g_tt.joinAutoSkill = iv ? 1 : 0;
	else if (!strcasecmp(key, "pick_mode"))
	{
		if (!strcasecmp(val, "rank") || !strcasecmp(val, "skill") || (val[0] == '0' && !val[1]))
			g_tt.pickMode = TT_PICK_RANK;
		else if (!strcasecmp(val, "random") || iv == 1)
			g_tt.pickMode = TT_PICK_RANDOM;
		else if (!strcasecmp(val, "mixed") || iv == 2)
			g_tt.pickMode = TT_PICK_MIXED;
		else
			TT_Trace("Config: line %d: pick_mode \"%s\" - use rank, random or mixed", line, val);
	}
	else if (!strcasecmp(key, "pick_mixed_chance"))      g_tt.pickMixedChance = ClampI(iv, 0, 100);
	else if (!strcasecmp(key, "names_track"))            g_tt.namesTrack = iv ? 1 : 0;
	else if (!strcasecmp(key, "stats_rivals"))           g_st.rivals = iv ? 1 : 0;
	else if (TT_NetConfigKey(key, val))                  ;
	else
		TT_Trace("Config: line %d: unknown key \"%s\" ignored", line, key);
}

// Cut a // comment off a line - but not inside "quotes", so an advert can
// hold a web address.
static void StripComment(char *line)
{
	bool q = false;
	for (char *c = line; *c; c++)
	{
		if (*c == '"')
			q = !q;
		else if (!q && c[0] == '/' && c[1] == '/')
		{
			*c = 0;
			return;
		}
	}
}

static FILE *OpenFirst(const char *rel, char *usedPath, size_t usedLen)
{
	char dir[400];
	FILE *f = NULL;
	if (TT_ModuleDir(dir, sizeof(dir)))
	{
		_snprintf_wc(usedPath, usedLen - 1, "%s/%s", dir, rel);
		usedPath[usedLen - 1] = 0;
		f = fopen(usedPath, "r");
		if (f)
			return f;
	}
	char gamedir[256];
	GET_GAME_DIR(gamedir);
	_snprintf_wc(usedPath, usedLen - 1, "%s/addons/tfc_teams_mm/%s", gamedir, rel);
	usedPath[usedLen - 1] = 0;
	return fopen(usedPath, "r");
}

// Pull the next "quoted" or bare token out of a users.ini line.
static const char *NextField(const char *s, char *out, size_t outLen)
{
	out[0] = 0;
	while (*s == ' ' || *s == '\t')
		s++;
	if (!*s)
		return s;
	size_t n = 0;
	if (*s == '"')
	{
		s++;
		while (*s && *s != '"')
		{
			if (n + 1 < outLen)
				out[n++] = *s;
			s++;
		}
		if (*s == '"')
			s++;
	}
	else
	{
		while (*s && *s != ' ' && *s != '\t')
		{
			if (n + 1 < outLen)
				out[n++] = *s;
			s++;
		}
	}
	out[n] = 0;
	return s;
}

static void LoadAmxxUsers(void)
{
	char path[512], gamedir[256], dir[400];
	FILE *f = NULL;
	// Our DLL sits in addons/tfc_teams_mm, so AMXX's configs are one folder over.
	if (TT_ModuleDir(dir, sizeof(dir)))
	{
		_snprintf_wc(path, sizeof(path) - 1, "%s/../amxmodx/configs/users.ini", dir);
		path[sizeof(path) - 1] = 0;
		f = fopen(path, "r");
	}
	if (!f)
	{
		GET_GAME_DIR(gamedir);
		_snprintf_wc(path, sizeof(path) - 1, "%s/addons/amxmodx/configs/users.ini", gamedir);
		path[sizeof(path) - 1] = 0;
		f = fopen(path, "r");
	}
	if (!f)
	{
		TT_Trace("Config: amxx_users is on but users.ini was not found (last tried %s)", path);
		return;
	}
	int before = g_tt.adminCount;
	char line[512];
	while (fgets(line, sizeof(line), f))
	{
		char *s = TrimInPlace(line);
		if (!s[0] || s[0] == ';' || (s[0] == '/' && s[1] == '/'))
			continue;
		char auth[64], pass[64], access[64], flags[64];
		const char *p = NextField(s, auth, sizeof(auth));
		p = NextField(p, pass, sizeof(pass));
		p = NextField(p, access, sizeof(access));
		NextField(p, flags, sizeof(flags));
		if (!auth[0] || !strchr(access, g_tt.amxxFlag) || !strchr(flags, 'c'))
			continue;
		AddAdmin(auth, "users.ini");
	}
	fclose(f);
	TT_Trace("Config: %d admin(s) with flag '%c' taken from %s", g_tt.adminCount - before, g_tt.amxxFlag, path);
}

// PER-MAP SETTINGS. Two ways, and both can be used:
//   - a section in tfc_teams.ini: the lines under "[dustbowl]" apply only on
//     dustbowl. A section can name several maps ("[dustbowl avanti cz2]", commas
//     are fine too) and use * as a wildcard ("[cz*]"). "[all]" (or "[*]") goes
//     back to lines for every map. Put sections at the end of the file, after
//     the settings for every map, since a later line wins.
//   - a file configs/maps/<map>.ini, read after tfc_teams.ini, in the same
//     "key value" format.
// Only the keys listed change; everything else keeps the tfc_teams.ini value.

// Simple glob: * matches any run of characters, case-insensitive.
static bool MapGlob(const char *pat, const char *name)
{
	if (!*pat)
		return !*name;
	if (*pat == '*')
	{
		for (const char *n = name; ; n++)
		{
			if (MapGlob(pat + 1, n))
				return true;
			if (!*n)
				return false;
		}
	}
	if (!*name)
		return false;
	char a = *pat, b = *name;
	if (a >= 'A' && a <= 'Z') a += 32;
	if (b >= 'A' && b <= 'Z') b += 32;
	return a == b && MapGlob(pat + 1, name + 1);
}

// "[dustbowl, avanti cz*]" -> does it cover this map? "[all]" / "[*]" -> yes.
static bool SectionMatches(const char *inside, const char *map, bool *isAll)
{
	*isAll = false;
	char buf[256];
	strncpy(buf, inside, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	bool any = false;
	for (char *tok = strtok(buf, " ,\t"); tok; tok = strtok(NULL, " ,\t"))
	{
		if (!strcasecmp(tok, "all") || !strcmp(tok, "*"))
		{
			*isAll = true;
			return true;
		}
		if (MapGlob(tok, map))
			any = true;
	}
	return any;
}

// Reads one config file. inSections: honour [map] sections (tfc_teams.ini).
// Returns the number of settings that came from a matching map section (or,
// for a map file, from the file).
static int ParseConfigFile(FILE *f, const char *map, bool inSections, char *sections, size_t sectionsLen)
{
	char line[256];
	int n = 0, fromMap = 0;
	bool active = true, mapSection = false;
	while (fgets(line, sizeof(line), f))
	{
		n++;
		StripComment(line);
		char *s = TrimInPlace(line);
		if (!s[0] || s[0] == '#' || s[0] == ';')
			continue;
		if (inSections && s[0] == '[')
		{
			char *end = strchr(s, ']');
			if (end)
				*end = 0;
			bool isAll;
			active = SectionMatches(s + 1, map, &isAll);
			mapSection = active && !isAll;
			if (mapSection && sections && strlen(sections) + strlen(s + 1) + 4 < sectionsLen)
			{
				if (sections[0])
					strcat(sections, " ");
				strcat(sections, "[");
				strcat(sections, s + 1);
				strcat(sections, "]");
			}
			continue;
		}
		if (!inSections && s[0] == '[')
		{
			TT_Trace("Config: line %d: [sections] only work in tfc_teams.ini - this whole file is for %s", n, map);
			continue;
		}
		if (!active)
			continue;
		char key[64];
		char *rest = SplitFirstToken(s, key, sizeof(key));
		rest = TrimInPlace(rest);
		// Allow key "value" as well as key value.
		size_t rl = strlen(rest);
		if (rl >= 2 && rest[0] == '"' && rest[rl - 1] == '"')
		{
			rest[rl - 1] = 0;
			rest++;
		}
		ApplyKey(key, rest, n);
		if (mapSection || !inSections)
		{
			fromMap++;
			TT_Trace("Config: %s -> %s %s", map, key, rest);
		}
	}
	return fromMap;
}

void TT_ConfigLoad(void)
{
	TT_ConfigDefaults();
	const char *map = (gpGlobals && gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";

	char path[512];
	FILE *f = OpenFirst("configs/tfc_teams.ini", path, sizeof(path));
	if (!f)
	{
		TT_Trace("Config: %s not found - running on built-in defaults", path);
	}
	else
	{
		char sections[200] = "";
		int fromMap = ParseConfigFile(f, map, true, sections, sizeof(sections));
		fclose(f);
		TT_Trace("Config: loaded %s", path);
		if (fromMap)
			TT_Trace("Config: %d setting(s) for %s from section %s", fromMap, map, sections);
	}

	// configs/maps/<map>.ini, on top.
	if (map[0])
	{
		char rel[128];
		_snprintf_wc(rel, sizeof(rel) - 1, "configs/maps/%s.ini", map);
		rel[sizeof(rel) - 1] = 0;
		FILE *mf = OpenFirst(rel, path, sizeof(path));
		if (mf)
		{
			int fromMap = ParseConfigFile(mf, map, false, NULL, 0);
			fclose(mf);
			TT_Trace("Config: loaded %s (%d setting(s) for this map)", path, fromMap);
		}
	}

	if (g_tt.amxxUsers)
		LoadAmxxUsers();

	TT_Trace("Config (%s): balance=%d threshold=%d delay=%.0f patience=%.0f new=%.0f cand=%d immunity=%.0f bots=%d/%d force=%.0f/%.0f block=%d | vote=%d %.0f%% min=%d start=%.0f cool=%.0f afk=%.0f spec=%d wait=%.0f bots=%d mode=%s reset=%.1fs frags=%d | admins=%d",
		map[0] ? map : "?", g_tt.balanceEnabled, g_tt.balanceThreshold, g_tt.balanceDelay, g_tt.balancePatience,
		g_tt.balanceNewWindow, g_tt.balanceCandidates, g_tt.balanceImmunity,
		g_tt.balanceIncludeBots, g_tt.balancePreferBots,
		g_tt.balanceForceAfter, g_tt.balanceForceWarn, g_tt.blockUnevenJoin,
		g_tt.voteEnabled, g_tt.votePercent, g_tt.voteMinVotes, g_tt.voteMapStartDelay,
		g_tt.voteCooldown, g_tt.voteAfkTime, g_tt.voteCountSpectators,
		g_tt.scrambleMaxWait, g_tt.scrambleIncludeBots,
		g_tt.scrambleMode == TT_SCR_RESET ? "reset" : "respawn", g_tt.scrambleResetDelay,
		g_tt.scrambleResetFrags, g_tt.adminCount);
	TT_Trace("Config (%s): pick_mode=%s mixed_chance=%d%%", map[0] ? map : "?",
		TT_PickModeName(g_tt.pickMode), g_tt.pickMixedChance);
}

// STEAM_0:1:2345 and STEAM_1:1:2345 are the same account (the first digit is
// the "universe", which differs between engine builds), so compare what
// follows it.
static const char *AuthTail(const char *a)
{
	if (!strncasecmp(a, "STEAM_", 6) || !strncasecmp(a, "VALVE_", 6))
	{
		const char *c = strchr(a, ':');
		if (c)
			return c + 1;
	}
	return a;
}

bool TT_IsAdminAuth(const char *authid)
{
	if (!authid || !authid[0])
		return false;
	const char *tail = AuthTail(authid);
	for (int i = 0; i < g_tt.adminCount; i++)
	{
		if (!strcasecmp(g_tt.admins[i], authid) || !strcasecmp(AuthTail(g_tt.admins[i]), tail))
			return true;
	}
	return false;
}

const char *TT_PickModeName(int mode)
{
	return mode == TT_PICK_RANDOM ? "random" : (mode == TT_PICK_MIXED ? "mixed" : "rank");
}
