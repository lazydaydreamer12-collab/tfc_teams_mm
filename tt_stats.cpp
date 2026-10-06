// tt_stats.cpp - player statistics by SteamID, and the skill rating the
// spectator-reset scramble deals players by.
//
// WHERE EACH NUMBER COMES FROM (all read out of tfc.so, nothing guessed):
//
//   Kills, deaths, suicides, teamkills - TFC's own server log lines, which
//     every UTIL_LogPrintf sends through the engine's AlertMessage(at_logged)
//     whether or not "log on" is set:
//       "Name<uid><STEAMID><Team>" killed "Name<uid><STEAMID><Team>" with "weapon"
//       "Name<uid><STEAMID><Team>" committed suicide with "weapon"
//     (CHalfLifeMultiplay::DeathNotice, tfc.so 0x8bae0.)
//
//   Damage and hits - CBaseMonster::TakeDamage (0x6f720, called by the player's
//     own TakeDamage) stores the inflictor in pev->dmg_inflictor and adds the
//     health damage to pev->dmg_take; armour damage goes to pev->dmg_save.
//     CBasePlayer::UpdateClientData (0xc2480) zeroes them, and it runs inside
//     the player's PreThink - so our PreThink pre-hook sees exactly one
//     frame's worth, every frame, before the game throws it away. The attacker
//     is the inflictor when that is a player (hitscan, melee), otherwise the
//     projectile's pev->owner.
//
//   Shots - every TFC weapon announces each shot with PLAYBACK_EVENT_FULL on an
//     event it precached as events/wpn/tf_<weapon>.sc. Hooking PrecacheEvent
//     (post) names every event index; hooking PlaybackEvent counts shots per
//     player per weapon. That also says which gun a hitscan hit came from
//     (the sniper rifle and autorifle share a model; their events differ).
//
//   Headshots - TFC's own: CBasePlayer::TraceAttack (0xc5320) tells the
//     sniper "#Sniper_headshot" when it doubles a charged shot into the head;
//     that private TextMsg is counted (see TT_StatsPrivateText).
//
//   Flag pickups, caps and carrier kills - tfgoalitem_GiveToPlayer (0x1021c0)
//     makes a carried goal item follow its carrier (aiment = owner = the
//     player, MOVETYPE_FOLLOW), so "who is carrying" is read off the items
//     every frame. A carrier who loses their item while alive, within a moment
//     of their team's score going up (the TeamScore message,
//     TeamFortress_TeamIncreaseScore 0x1046e0), captured it. 2fort-style
//     "Captured_Red_Flag" log lines count as caps directly. A kill of someone
//     who was carrying is a carrier kill.
//
// ACCURACY is hits / shots per weapon, where one shot that damages several
// enemies (a rocket's splash, a shotgun blast) is one hit - so it can never
// pass 100%. Damage to team-mates and yourself is not a hit.
//
// THE RATING is "points per 10 minutes on a team":
//   kill 1, carrier kill 2 (on top of the kill), capture 5, flag pickup 1,
//   teamkill -2 (weights in the ini). It is smoothed across maps
//   (new = 70% old + 30% this map, once someone has played stats_min_map_minutes
//   on a map), so one lucky map does not swing it. Players never seen before
//   start at the median of everyone known. Bots are tracked as "BOT:<name>" so
//   the scramble can rate them too, but never appear in !rank or !top10.

#include <vector>
#include <algorithm>
#include <time.h>
#include <stdarg.h>

#include "tt_common.h"
#include "tt_stats.h"
#include "tt_net.h"

// ---------------------------------------------------------------------------
// Weapons
enum
{
	W_OTHER = 0,
	W_SHOTGUN, W_SUPERSHOTGUN, W_NAILGUN, W_SUPERNAILGUN, W_RAILGUN,
	W_ROCKET, W_INCENDIARY, W_GL, W_PIPEBOMB, W_SNIPER, W_AUTORIFLE,
	W_AC, W_FLAMER, W_TRANQ, W_AXE, W_SPANNER, W_KNIFE, W_MEDIKIT,
	W_GREN_NORMAL, W_GREN_CONC, W_GREN_NAIL, W_GREN_MIRV, W_GREN_NAPALM,
	W_GREN_GAS, W_GREN_EMP, W_CALTROP, W_DETPACK, W_SENTRY, W_DISPENSER,
	W_INFECTION, W_FIRE, W_TELEFRAG, W_WORLD,
	W_COUNT
};

static const char *g_wName[W_COUNT] =
{
	"other",
	"shotgun", "supershotgun", "nailgun", "supernailgun", "railgun",
	"rocketlauncher", "incendiary", "grenadelauncher", "pipebomb", "sniperrifle", "autorifle",
	"assaultcannon", "flamethrower", "tranq", "axe", "spanner", "knife", "medikit",
	"normalgrenade", "concgrenade", "nailgrenade", "mirvgrenade", "napalmgrenade",
	"gasgrenade", "empgrenade", "caltrop", "detpack", "sentrygun", "dispenser",
	"infection", "fire", "telefrag", "world",
};

struct TTNameMap { const char *name; int w; };

// events/wpn/<file> -> weapon (the file name without the folder)
static const TTNameMap g_eventMap[] =
{
	{ "tf_sg.sc", W_SHOTGUN }, { "tf_ssg.sc", W_SUPERSHOTGUN }, { "tf_nail.sc", W_NAILGUN },
	{ "tf_snail.sc", W_SUPERNAILGUN }, { "tf_rail.sc", W_RAILGUN }, { "tf_rpg.sc", W_ROCKET },
	{ "tf_ic.sc", W_INCENDIARY }, { "tf_gl.sc", W_GL }, { "tf_pipel.sc", W_PIPEBOMB },
	{ "tf_sniper.sc", W_SNIPER }, { "tf_ar.sc", W_AUTORIFLE }, { "tf_acfire.sc", W_AC },
	{ "tf_flame.sc", W_FLAMER }, { "tf_tranq.sc", W_TRANQ }, { "tf_axe.sc", W_AXE },
	{ "tf_knife.sc", W_KNIFE }, { "tf_mednormal.sc", W_MEDIKIT }, { "tf_medsuper.sc", W_MEDIKIT },
	{ NULL, 0 }
};

// Inflictor classnames, and the names the kill log uses (which are the same
// classnames with "tf_weapon_"/"weapon_"/"monster_"/"func_" cut off, or the
// held weapon's name for hitscan and melee).
static const TTNameMap g_classMap[] =
{
	{ "tf_rpg_rocket", W_ROCKET }, { "rpg", W_ROCKET }, { "rocket", W_ROCKET },
	{ "tf_ic_rocket", W_INCENDIARY }, { "ic", W_INCENDIARY },
	{ "tf_gl_grenade", W_GL }, { "gl", W_GL }, { "gl_grenade", W_GL },
	{ "tf_gl_pipebomb", W_PIPEBOMB }, { "pl", W_PIPEBOMB }, { "pipebomb", W_PIPEBOMB },
	{ "tf_flamethrower_burst", W_FLAMER }, { "flamethrower", W_FLAMER },
	{ "tf_flame", W_FIRE }, { "tf_fire", W_FIRE }, { "fire", W_FIRE },
	{ "shotgun", W_SHOTGUN }, { "supershotgun", W_SUPERSHOTGUN },
	{ "ng", W_NAILGUN }, { "nailgun", W_NAILGUN }, { "superng", W_SUPERNAILGUN },
	{ "railgun", W_RAILGUN }, { "sniperrifle", W_SNIPER }, { "autorifle", W_AUTORIFLE },
	{ "ac", W_AC }, { "tranq", W_TRANQ }, { "axe", W_AXE }, { "spanner", W_SPANNER },
	{ "knife", W_KNIFE }, { "medikit", W_MEDIKIT },
	{ "tf_weapon_normalgrenade", W_GREN_NORMAL }, { "normalgrenade", W_GREN_NORMAL },
	{ "tf_weapon_concussiongrenade", W_GREN_CONC }, { "concussiongrenade", W_GREN_CONC },
	{ "tf_weapon_nailgrenade", W_GREN_NAIL }, { "nailgrenade", W_GREN_NAIL },
	{ "tf_weapon_nailgrenadenail", W_GREN_NAIL }, { "nailgrenadenail", W_GREN_NAIL },
	{ "tf_weapon_mirvgrenade", W_GREN_MIRV }, { "mirvgrenade", W_GREN_MIRV },
	{ "tf_weapon_mirvbomblet", W_GREN_MIRV }, { "mirvbomblet", W_GREN_MIRV },
	{ "tf_weapon_napalmgrenade", W_GREN_NAPALM }, { "napalmgrenade", W_GREN_NAPALM },
	{ "tf_weapon_gasgrenade", W_GREN_GAS }, { "gasgrenade", W_GREN_GAS },
	{ "tf_weapon_empgrenade", W_GREN_EMP }, { "empgrenade", W_GREN_EMP },
	{ "tf_weapon_caltrop", W_CALTROP }, { "caltrop", W_CALTROP },
	{ "tf_weapon_caltropgrenade", W_CALTROP }, { "caltropgrenade", W_CALTROP },
	{ "detpack", W_DETPACK }, { "building_sentrygun", W_SENTRY }, { "sentrygun", W_SENTRY },
	{ "building_dispenser", W_DISPENSER }, { "dispenser", W_DISPENSER },
	{ "headshot", W_SNIPER }, { "infection", W_INFECTION }, { "teledeath", W_TELEFRAG }, { "teledeath2", W_TELEFRAG },
	{ "world", W_WORLD }, { "worldspawn", W_WORLD }, { "trigger_hurt", W_WORLD },
	{ "timer", W_FIRE }, { "flames", W_FIRE }, { "tf_flamethrower", W_FLAMER }, { "rpg", W_ROCKET }, { "superng", W_SUPERNAILGUN },
	{ NULL, 0 }
};

static int TT_MapName(const TTNameMap *m, const char *name)
{
	if (!name || !name[0])
		return -1;
	for (int i = 0; m[i].name; i++)
		if (!strcasecmp(m[i].name, name))
			return m[i].w;
	return -1;
}

// Inflictor classname to weapon. The live log showed hitscan damage arriving
// with the WEAPON entity as inflictor ("tf_weapon_sniperrifle", owned by the
// player) rather than the player, so the held-weapon classnames are matched
// with their prefix taken off, exactly as the kill log names them.
static int TT_ClassWeapon(const char *cls)
{
	if (!cls || !cls[0])
		return -1;
	int w = TT_MapName(g_classMap, cls);
	if (w >= 0)
		return w;
	if (!strncasecmp(cls, "tf_weapon_", 10))
		return TT_MapName(g_classMap, cls + 10);
	if (!strncasecmp(cls, "weapon_", 7))
		return TT_MapName(g_classMap, cls + 7);
	return -1;
}

static bool TT_IsThrown(int w)
{
	return (w >= W_GREN_NORMAL && w <= W_CALTROP) || w == W_DETPACK;
}

static int TT_WeaponByKey(const char *key)
{
	for (int w = 0; w < W_COUNT; w++)
		if (!strcasecmp(g_wName[w], key))
			return w;
	return -1;
}

// ---------------------------------------------------------------------------
// Numbers
struct TTWeaponStat
{
	int kills, deaths, shots, hits, headshots;
	float damage;
};

// One set of counters. A player's block has one for everything together and
// one per class they were playing at the time (TFC's playerclass: 1 scout,
// 2 sniper, 3 soldier, 4 demoman, 5 medic, 6 hwguy, 7 pyro, 8 spy,
// 9 engineer; 0 = no class yet). Sentry gun and dispenser kills always go to
// the engineer.
struct TTCounts
{
	int kills, deaths, suicides, teamkills;
	int caps, pickups, carrierKills, carrierDeaths;
	int shots, hits, headshots;
	int builds, buildKills;      // sentries/dispensers built; enemy ones destroyed
	int heals, cures;            // medic: team-mates healed / cured (TFC's Medic_Heal, Medic_Cured_*, Medic_Doused_Fire)
	int bestStreak;              // most enemy kills without dying (a best, not a total)
	float damage, seconds;
	float healed;                // medic: health given to team-mates
	TTWeaponStat w[W_COUNT];
};

#define TT_CLASSES 10
struct TTBlock : TTCounts
{
	TTCounts cls[TT_CLASSES];
};

static const char *g_clsKey[TT_CLASSES] =
{
	"none", "scout", "sniper", "soldier", "demoman", "medic", "hwguy", "pyro", "spy", "engineer"
};
static const char *g_clsTitle[TT_CLASSES] =
{
	"No class", "Scout", "Sniper", "Soldier", "Demoman", "Medic", "HWGuy", "Pyro", "Spy", "Engineer"
};

static int TT_ClassByKey(const char *k)
{
	for (int c = 0; c < TT_CLASSES; c++)
		if (!strcasecmp(g_clsKey[c], k))
			return c;
	if (!strcasecmp(k, "hw") || !strcasecmp(k, "heavy"))
		return 6;
	if (!strcasecmp(k, "demo"))
		return 4;
	if (!strcasecmp(k, "engy") || !strcasecmp(k, "engie"))
		return 9;
	return -1;
}

static void CountsAdd(TTCounts &a, const TTCounts &b)
{
	a.kills += b.kills; a.deaths += b.deaths; a.suicides += b.suicides; a.teamkills += b.teamkills;
	a.caps += b.caps; a.pickups += b.pickups; a.carrierKills += b.carrierKills; a.carrierDeaths += b.carrierDeaths;
	a.shots += b.shots; a.hits += b.hits; a.headshots += b.headshots;
	a.builds += b.builds; a.buildKills += b.buildKills;
	a.heals += b.heals; a.cures += b.cures; a.healed += b.healed;
	if (b.bestStreak > a.bestStreak)
		a.bestStreak = b.bestStreak;
	a.damage += b.damage; a.seconds += b.seconds;
	for (int i = 0; i < W_COUNT; i++)
	{
		a.w[i].kills += b.w[i].kills; a.w[i].deaths += b.w[i].deaths;
		a.w[i].shots += b.w[i].shots; a.w[i].hits += b.w[i].hits;
		a.w[i].headshots += b.w[i].headshots; a.w[i].damage += b.w[i].damage;
	}
}

static void BlockAdd(TTBlock &a, const TTBlock &b)
{
	CountsAdd(a, b);
	for (int c = 0; c < TT_CLASSES; c++)
		CountsAdd(a.cls[c], b.cls[c]);
}

static bool CountsUsed(const TTCounts &b)
{
	return b.seconds >= 1.0f || b.kills || b.deaths || b.shots || b.hits || b.caps || b.builds || b.heals || b.cures;
}

struct TTRecord
{
	char  key[48];     // SteamID, "BOT:<name>" or "NAME:<name>" on a LAN server
	char  name[32];    // last name seen
	bool  bot;
	float rating;      // < 0 = no rating yet
	int   maps;        // maps rated
	long  lastSeen;    // unix time
	TTBlock life;
};

static std::vector<TTRecord> g_db;
static bool  g_dbDirty = false;
static float g_nextSave = 0;

// HEAD-TO-HEAD. Who killed whom, per pair of players (record indexes a < b):
// ab = times a killed b, ba = times b killed a; mab/mba the same for this map.
// Kept for pairs where at least one of the two is a person (bot v bot pairs
// would be most of the table and nobody looks at them). Saved as "V" lines.
struct TTVs { int a, b; int ab, ba, mab, mba; };
static std::vector<TTVs> g_vs;

static TTVs *TT_VsFind(int ra, int rb, bool create)
{
	int a = ra < rb ? ra : rb, b = ra < rb ? rb : ra;
	for (size_t i = 0; i < g_vs.size(); i++)
		if (g_vs[i].a == a && g_vs[i].b == b)
			return &g_vs[i];
	if (!create)
		return NULL;
	TTVs v;
	memset(&v, 0, sizeof(v));
	v.a = a;
	v.b = b;
	g_vs.push_back(v);
	return &g_vs.back();
}

// Per player slot, this map.
struct TTSession
{
	int     rec;             // index into g_db, -1 = not resolved yet
	TTBlock map;             // everything this map (for !stats and the summary)
	TTBlock pending;         // not yet added into the lifetime record
	bool    carrying;        // carrying a goal item last frame
	float   lostItemAt;      // lost it while alive at this time (cap candidate)
	float   capLoggedAt;     // a "Captured_" log line already counted this cap
	float   trigAt;          // they last activated a goal ("triggered" log line) at this time
	bool    trigCap;         // ...and within that moment one had the cap word in its name
	bool    trigReturn;      // ...or the return word
	int     lostItemTeam;
	int     lastEventW;      // last weapon they fired, by event
	float   lastEventAt;
	int     lastNailW;       // last nail-firing weapon (nailgun/super/rail share the nail)
	float   lastNailAt;
	int     lastHitInflictor;// one hit per inflictor per frame
	float   lastHitAt;
	int     lastDmgFrom;     // who last damaged this player, with what, when
	int     lastDmgW;
	float   lastDmgAt;
	float   consumedTake;    // dmg_take already counted at a kill, this frame
	bool    ratedThisMap;
	int     forceCls;        // > 0: count the next numbers for this class (a sentry kill is the engineer's)
	int     streak;          // enemy kills since they last died
	float   hpSnap;          // health at the start of this frame (how much a heal gave)
};
static TTSession g_ss[TT_MAX_PLAYERS + 1];

// Owner and weapon of every owned entity, noted each frame - see TT_NoteEntities.
#define TT_MAX_EDICTS 4096
struct TTEntNote { int serial; short owner; short w; };
static TTEntNote g_note[TT_MAX_EDICTS];

static edict_t *g_ignoreDeaths = NULL;
static bool  g_intermission = false;
static bool  g_summaryDone = false;
static bool  g_mapEnded = false;
static float g_teamScore[TT_MAX_TEAMS + 1];
static float g_teamScoreUpAt[TT_MAX_TEAMS + 1];
static unsigned short g_eventW[1024];   // event index -> weapon + 1 (0 = not a weapon event)

// ---------------------------------------------------------------------------
// Settings (kept here, read from the same ini by tt_config.cpp)
TTStatsConfig g_st;

void TT_StatsConfigDefaults(void)
{
	g_st.enabled        = 1;
	g_st.minMapMinutes  = 3.0f;
	g_st.minRankMinutes = 10.0f;
	g_st.saveInterval   = 300.0f;
	g_st.summary        = 2;    // chat lines + the awards window at intermission
	g_st.wKill          = 1.0f;
	g_st.wCarrierKill   = 2.0f;
	g_st.wCap           = 5.0f;
	g_st.wPickup        = 1.0f;
	g_st.wTeamkill      = -2.0f;
	g_st.wHeal          = 0.0f;     // per 100 health healed
	g_st.newRating      = 0.0f; // 0 = median of everyone known
	g_st.window         = 1;    // MOTD window (2 = paged menu)
	g_st.menuTime       = 60.0f;
	strcpy(g_st.capWord, "cap");
	strcpy(g_st.returnWord, "return");
	g_st.debugCaps      = 1;    // on while caps are being worked out on real maps
	g_st.rivals         = 1;
}

// ---------------------------------------------------------------------------
// Database file: text, one "P" line per player then its "W" lines.
static bool TT_StatsPath(char *out, size_t len)
{
	char dir[400];
	if (!TT_ModuleDir(dir, sizeof(dir)))
		return false;
	_snprintf_wc(out, len - 1, "%s/tt_stats.txt", dir);
	out[len - 1] = 0;
	return true;
}

static void Clean(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

static int TT_FindRecord(const char *key)
{
	for (size_t i = 0; i < g_db.size(); i++)
		if (!strcmp(g_db[i].key, key))
			return (int)i;
	return -1;
}

static void TT_StatsLoad(void)
{
	g_db.clear();
	char path[450];
	if (!TT_StatsPath(path, sizeof(path)))
		return;
	FILE *f = fopen(path, "r");
	if (!f)
	{
		TT_Trace("Stats: no %s yet - starting a fresh database", path);
		return;
	}
	char line[1024];
	int cur = -1, bad = 0;
	g_vs.clear();
	struct TTVsLine { char a[48], b[48]; int ab, ba; };
	std::vector<TTVsLine> vsLines;
	bool v1 = false; // v1 files carry the old trace-based headshots - dropped below
	while (fgets(line, sizeof(line), f))
	{
		if (!strncmp(line, "# TFC Teams player stats v1", 27))
			v1 = true;
		line[strcspn(line, "\r\n")] = 0;
		char *fld[32];
		int n = 0;
		char *p = line;
		while (n < 32)
		{
			fld[n++] = p;
			char *t = strchr(p, '\t');
			if (!t)
				break;
			*t = 0;
			p = t + 1;
		}
		if (n >= 20 && !strcmp(fld[0], "P"))
		{
			TTRecord r;
			memset(&r, 0, sizeof(r));
			strncpy(r.key, fld[1], sizeof(r.key) - 1);
			strncpy(r.name, fld[2], sizeof(r.name) - 1);
			r.bot = atoi(fld[3]) != 0;
			r.rating = (float)atof(fld[4]);
			r.maps = atoi(fld[5]);
			r.lastSeen = atol(fld[6]);
			TTBlock &b = r.life;
			b.seconds = (float)atof(fld[7]);
			b.kills = atoi(fld[8]); b.deaths = atoi(fld[9]); b.suicides = atoi(fld[10]);
			b.teamkills = atoi(fld[11]); b.caps = atoi(fld[12]); b.pickups = atoi(fld[13]);
			b.carrierKills = atoi(fld[14]); b.carrierDeaths = atoi(fld[15]);
			b.shots = atoi(fld[16]); b.hits = atoi(fld[17]); b.headshots = atoi(fld[18]);
			b.damage = (float)atof(fld[19]);
			if (n >= 22)
			{
				b.builds = atoi(fld[20]);
				b.buildKills = atoi(fld[21]);
			}
			if (n >= 26)
			{
				b.heals = atoi(fld[22]); b.cures = atoi(fld[23]);
				b.healed = (float)atof(fld[24]); b.bestStreak = atoi(fld[25]);
			}
			if (!r.key[0] || TT_FindRecord(r.key) >= 0)
			{
				bad++;
				cur = -1;
				continue;
			}
			g_db.push_back(r);
			cur = (int)g_db.size() - 1;
		}
		else if (n >= 8 && !strcmp(fld[0], "W") && cur >= 0)
		{
			int w = TT_WeaponByKey(fld[1]);
			if (w < 0)
				continue;
			TTWeaponStat &s = g_db[cur].life.w[w];
			s.kills = atoi(fld[2]); s.deaths = atoi(fld[3]); s.shots = atoi(fld[4]);
			s.hits = atoi(fld[5]); s.headshots = atoi(fld[6]); s.damage = (float)atof(fld[7]);
		}
		else if (n >= 17 && !strcmp(fld[0], "C") && cur >= 0)
		{
			int c = TT_ClassByKey(fld[1]);
			if (c < 0)
				continue;
			TTCounts &b = g_db[cur].life.cls[c];
			b.seconds = (float)atof(fld[2]);
			b.kills = atoi(fld[3]); b.deaths = atoi(fld[4]); b.suicides = atoi(fld[5]);
			b.teamkills = atoi(fld[6]); b.caps = atoi(fld[7]); b.pickups = atoi(fld[8]);
			b.carrierKills = atoi(fld[9]); b.carrierDeaths = atoi(fld[10]);
			b.shots = atoi(fld[11]); b.hits = atoi(fld[12]); b.headshots = atoi(fld[13]);
			b.damage = (float)atof(fld[14]); b.builds = atoi(fld[15]); b.buildKills = atoi(fld[16]);
			if (n >= 21)
			{
				b.heals = atoi(fld[17]); b.cures = atoi(fld[18]);
				b.healed = (float)atof(fld[19]); b.bestStreak = atoi(fld[20]);
			}
		}
		else if (n >= 9 && !strcmp(fld[0], "CW") && cur >= 0)
		{
			int c = TT_ClassByKey(fld[1]);
			int w = TT_WeaponByKey(fld[2]);
			if (c < 0 || w < 0)
				continue;
			TTWeaponStat &s = g_db[cur].life.cls[c].w[w];
			s.kills = atoi(fld[3]); s.deaths = atoi(fld[4]); s.shots = atoi(fld[5]);
			s.hits = atoi(fld[6]); s.headshots = atoi(fld[7]); s.damage = (float)atof(fld[8]);
		}
		else if (n >= 5 && !strcmp(fld[0], "V"))
		{
			TTVsLine v;
			memset(&v, 0, sizeof(v));
			strncpy(v.a, fld[1], sizeof(v.a) - 1);
			strncpy(v.b, fld[2], sizeof(v.b) - 1);
			v.ab = atoi(fld[3]);
			v.ba = atoi(fld[4]);
			vsLines.push_back(v);
		}
		else if (line[0] && line[0] != '#' && strcmp(fld[0], "W"))
			bad++;
	}
	fclose(f);
	for (size_t i = 0; i < vsLines.size(); i++)
	{
		int ra = TT_FindRecord(vsLines[i].a), rb = TT_FindRecord(vsLines[i].b);
		if (ra < 0 || rb < 0 || ra == rb)
			continue;
		TTVs *v = TT_VsFind(ra, rb, true);
		// stored as "a killed b" for the file's order
		if (v->a == ra) { v->ab += vsLines[i].ab; v->ba += vsLines[i].ba; }
		else            { v->ab += vsLines[i].ba; v->ba += vsLines[i].ab; }
	}
	if (v1)
	{
		for (size_t i = 0; i < g_db.size(); i++)
		{
			g_db[i].life.headshots = 0;
			for (int w = 0; w < W_COUNT; w++)
				g_db[i].life.w[w].headshots = 0;
		}
		g_dbDirty = true;
		TT_Trace("Stats: old (v1) file - headshot counts reset; from now on only TFC's own sniper headshots count");
	}
	TT_Trace("Stats: loaded %d players from %s%s", (int)g_db.size(), path, bad ? " (some lines skipped)" : "");
}

static void TT_StatsSave(const char *why)
{
	char path[450], tmp[470];
	if (!TT_StatsPath(path, sizeof(path)))
		return;
	_snprintf_wc(tmp, sizeof(tmp) - 1, "%s.tmp", path);
	tmp[sizeof(tmp) - 1] = 0;
	FILE *f = fopen(tmp, "w");
	if (!f)
	{
		TT_Trace("Stats: could not write %s", tmp);
		return;
	}
	fprintf(f, "# TFC Teams player stats v4 - P key name bot rating maps lastseen seconds kills deaths suicides teamkills caps pickups carrierkills carrierdeaths shots hits headshots damage builds buildkills heals cures healed beststreak\n");
	fprintf(f, "#                           W weapon kills deaths shots hits headshots damage\n");
	fprintf(f, "#                           C class seconds kills deaths suicides teamkills caps pickups carrierkills carrierdeaths shots hits headshots damage builds buildkills heals cures healed beststreak\n");
	fprintf(f, "#                           CW class weapon kills deaths shots hits headshots damage\n");
	fprintf(f, "#                           V key1 key2 key1_killed_key2 key2_killed_key1 (at the end)\n");
	int written = 0;
	for (size_t i = 0; i < g_db.size(); i++)
	{
		TTRecord &r = g_db[i];
		// Someone who connected and never played has nothing worth keeping.
		if (r.rating < 0 && r.life.seconds < 1.0f && !r.life.kills && !r.life.deaths && !r.life.shots)
			continue;
		written++;
		Clean(r.name);
		const TTBlock &b = r.life;
		fprintf(f, "P\t%s\t%s\t%d\t%.3f\t%d\t%ld\t%.0f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.0f\t%d\t%d\t%d\t%d\t%.0f\t%d\n",
			r.key, r.name, r.bot ? 1 : 0, r.rating, r.maps, r.lastSeen, b.seconds,
			b.kills, b.deaths, b.suicides, b.teamkills, b.caps, b.pickups, b.carrierKills,
			b.carrierDeaths, b.shots, b.hits, b.headshots, b.damage, b.builds, b.buildKills,
			b.heals, b.cures, b.healed, b.bestStreak);
		for (int w = 0; w < W_COUNT; w++)
		{
			const TTWeaponStat &s = b.w[w];
			if (s.kills || s.deaths || s.shots || s.hits)
				fprintf(f, "W\t%s\t%d\t%d\t%d\t%d\t%d\t%.0f\n", g_wName[w], s.kills, s.deaths,
					s.shots, s.hits, s.headshots, s.damage);
		}
		for (int c = 0; c < TT_CLASSES; c++)
		{
			const TTCounts &k = b.cls[c];
			if (!CountsUsed(k))
				continue;
			fprintf(f, "C\t%s\t%.0f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.0f\t%d\t%d\t%d\t%d\t%.0f\t%d\n",
				g_clsKey[c], k.seconds, k.kills, k.deaths, k.suicides, k.teamkills, k.caps, k.pickups,
				k.carrierKills, k.carrierDeaths, k.shots, k.hits, k.headshots, k.damage, k.builds, k.buildKills,
				k.heals, k.cures, k.healed, k.bestStreak);
			for (int w = 0; w < W_COUNT; w++)
			{
				const TTWeaponStat &s = k.w[w];
				if (s.kills || s.deaths || s.shots || s.hits)
					fprintf(f, "CW\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%.0f\n", g_clsKey[c], g_wName[w],
						s.kills, s.deaths, s.shots, s.hits, s.headshots, s.damage);
			}
		}
	}
	for (size_t i = 0; i < g_vs.size(); i++)
	{
		const TTVs &v = g_vs[i];
		if (v.ab + v.ba <= 0 || v.a < 0 || v.b < 0 || v.a >= (int)g_db.size() || v.b >= (int)g_db.size())
			continue;
		fprintf(f, "V\t%s\t%s\t%d\t%d\n", g_db[v.a].key, g_db[v.b].key, v.ab, v.ba);
	}
	fclose(f);
	remove(path);
	if (rename(tmp, path) != 0)
	{
		TT_Trace("Stats: could not replace %s (the new data is in %s)", path, tmp);
		return;
	}
	g_dbDirty = false;
	TT_Trace("Stats: saved %d players (%s)", written, why);
}

// ---------------------------------------------------------------------------
// Resolving a slot to its record
static void TT_KeyFor(edict_t *p, char *out, size_t len)
{
	out[0] = 0;
	const char *name = STRING(p->v.netname);
	if (TT_IsBot(p))
	{
		_snprintf_wc(out, len - 1, "BOT:%s", name);
		out[len - 1] = 0;
		return;
	}
	const char *auth = GETPLAYERAUTHID(p);
	if (!auth || !auth[0] || !strcasecmp(auth, "STEAM_ID_PENDING"))
		return; // not yet - try again next frame
	if (!strncasecmp(auth, "STEAM_", 6) && strcasecmp(auth, "STEAM_ID_LAN") && strcasecmp(auth, "STEAM_666:88:666"))
	{
		// The universe digit differs between engine builds for one account.
		const char *c = strchr(auth, ':');
		_snprintf_wc(out, len - 1, "STEAM_0%s", c ? c : auth + 6);
	}
	else
		_snprintf_wc(out, len - 1, "NAME:%s", name); // LAN / no Steam: best we can do
	out[len - 1] = 0;
}

static float TT_MedianRating(void)
{
	std::vector<float> v;
	for (size_t i = 0; i < g_db.size(); i++)
		if (g_db[i].rating >= 0 && !g_db[i].bot)
			v.push_back(g_db[i].rating);
	if (v.empty())
		return 10.0f;
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

static void TT_Resolve(int idx, edict_t *p)
{
	TTSession &s = g_ss[idx];
	if (s.rec >= 0)
		return;
	char key[48];
	TT_KeyFor(p, key, sizeof(key));
	if (!key[0])
		return;
	int r = TT_FindRecord(key);
	if (r < 0)
	{
		TTRecord rec;
		memset(&rec, 0, sizeof(rec));
		strncpy(rec.key, key, sizeof(rec.key) - 1);
		rec.bot = TT_IsBot(p);
		rec.rating = -1.0f;
		g_db.push_back(rec);
		r = (int)g_db.size() - 1;
		TT_Trace("Stats: new player %s (%s)", STRING(p->v.netname), key);
	}
	strncpy(g_db[r].name, STRING(p->v.netname), sizeof(g_db[r].name) - 1);
	g_db[r].name[sizeof(g_db[r].name) - 1] = 0;
	g_db[r].lastSeen = (long)time(NULL);
	s.rec = r;
	g_dbDirty = true;
}

// Fold what this slot has gathered since the last flush into its record.
static void TT_Flush(int idx)
{
	TTSession &s = g_ss[idx];
	if (s.rec < 0)
		return;
	BlockAdd(g_db[s.rec].life, s.pending);
	memset(&s.pending, 0, sizeof(s.pending));
	g_dbDirty = true;
}

static float TT_Points(const TTCounts &b)
{
	return b.kills * g_st.wKill + b.carrierKills * g_st.wCarrierKill + b.caps * g_st.wCap
		+ b.pickups * g_st.wPickup + b.teamkills * g_st.wTeamkill + b.healed * g_st.wHeal / 100.0f;
}

// Points per 10 minutes on a team.
static float TT_Perf(const TTCounts &b)
{
	float minutes = b.seconds / 60.0f;
	if (minutes < 1.0f)
		minutes = 1.0f;
	return TT_Points(b) * 10.0f / minutes;
}

// Fold this map into a player's rating (once per map).
static void TT_RateMap(int idx)
{
	TTSession &s = g_ss[idx];
	if (s.rec < 0 || s.ratedThisMap || s.map.seconds / 60.0f < g_st.minMapMinutes)
		return;
	TTRecord &r = g_db[s.rec];
	float perf = TT_Perf(s.map);
	float old = r.rating;
	r.rating = (old < 0) ? perf : old * 0.7f + perf * 0.3f;
	r.maps++;
	s.ratedThisMap = true;
	g_dbDirty = true;
	TT_Trace("Stats: rated %s - this map %.1f pts/10min, rating %.1f -> %.1f", r.name, perf, old, r.rating);
}

float TT_StatsRating(edict_t *p)
{
	if (FNullEnt(p))
		return 0;
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return 0;
	TTSession &s = g_ss[idx];
	float base = (s.rec >= 0 && g_db[s.rec].rating >= 0) ? g_db[s.rec].rating
	           : (g_st.newRating > 0 ? g_st.newRating : TT_MedianRating());
	// This map counts once they have been on a team long enough for it to mean
	// something - the same weight it will get when the map is folded in.
	if (s.map.seconds / 60.0f >= g_st.minMapMinutes)
		return base * 0.7f + TT_Perf(s.map) * 0.3f;
	return base;
}

// ---------------------------------------------------------------------------
// Counting helpers (map + pending together)
// Every number also goes to the class the player is at the time. (A kill by
// a sentry gun or dispenser is set to the engineer by the kill code, since
// its owner may have changed class since building it.)
static int TT_EvClass(int idx, int w)
{
	if (g_ss[idx].forceCls > 0 && g_ss[idx].forceCls < TT_CLASSES)
		return g_ss[idx].forceCls;
	edict_t *e = INDEXENT(idx);
	int c = e ? (int)e->v.playerclass : 0;
	return (c >= 1 && c <= 9) ? c : 0;
}
#define TT_BUMP(idx, field, n) do { TTSession &s_ = g_ss[idx]; int c_ = TT_EvClass(idx, -1); \
	s_.map.field += (n); s_.pending.field += (n); s_.map.cls[c_].field += (n); s_.pending.cls[c_].field += (n); } while (0)
#define TT_BUMPW(idx, wi, field, n) do { TTSession &s_ = g_ss[idx]; int c_ = TT_EvClass(idx, wi); \
	s_.map.w[wi].field += (n); s_.pending.w[wi].field += (n); \
	s_.map.cls[c_].w[wi].field += (n); s_.pending.cls[c_].w[wi].field += (n); } while (0)

// A kill streak: the best so far this map, since the last save, and for the class.
static void TT_StreakNote(int idx)
{
	TTSession &s = g_ss[idx];
	int c = TT_EvClass(idx, -1);
	TTCounts *t[4] = { &s.map, &s.pending, &s.map.cls[c], &s.pending.cls[c] };
	for (int j = 0; j < 4; j++)
		if (s.streak > t[j]->bestStreak)
			t[j]->bestStreak = s.streak;
}

// Kills with a melee weapon: crowbar/umbrella, spanner, knife, and the medikit
// itself (not its infection).
static int TT_MeleeKills(const TTCounts &b)
{
	return b.w[W_AXE].kills + b.w[W_SPANNER].kills + b.w[W_KNIFE].kills + b.w[W_MEDIKIT].kills;
}

static int TT_IdxOf(const edict_t *e)
{
	if (!e)
		return 0;
	int i = ENTINDEX((edict_t *)e);
	if (i < 1 || i > gpGlobals->maxClients || i > TT_MAX_PLAYERS)
		return 0;
	return TT_Player(i) ? i : 0;
}

// ---------------------------------------------------------------------------
// Lifecycle
void TT_StatsInit(void)
{
	memset(g_ss, 0, sizeof(g_ss));
	for (int i = 0; i <= TT_MAX_PLAYERS; i++)
		g_ss[i].rec = -1;
	TT_StatsLoad();
}

void TT_StatsShutdown(void)
{
	for (int i = 1; i <= TT_MAX_PLAYERS; i++)
		TT_Flush(i);
	if (g_dbDirty)
		TT_StatsSave("plugin unloading");
}

void TT_StatsMapStart(void)
{
	// Resetting only the per-map parts: records are kept, and players who are
	// still connected across the change get a fresh map block.
	for (int i = 0; i <= TT_MAX_PLAYERS; i++)
	{
		int rec = g_ss[i].rec;
		memset(&g_ss[i], 0, sizeof(g_ss[i]));
		g_ss[i].rec = rec;
	}
	memset(g_eventW, 0, sizeof(g_eventW)); // refilled as this map precaches
	for (size_t i = 0; i < g_vs.size(); i++)
		g_vs[i].mab = g_vs[i].mba = 0;
	for (int i = 0; i < TT_MAX_EDICTS; i++)
		g_note[i].serial = -1;
	g_intermission = g_summaryDone = g_mapEnded = false;
	TT_StatsMenuGone(NULL);
	memset(g_teamScore, 0, sizeof(g_teamScore));
	memset(g_teamScoreUpAt, 0, sizeof(g_teamScoreUpAt));
	g_nextSave = gpGlobals->time + g_st.saveInterval;
}

void TT_StatsMapEnd(void)
{
	if (g_mapEnded)
		return;
	g_mapEnded = true;
	for (int i = 1; i <= TT_MAX_PLAYERS; i++)
	{
		TT_Flush(i);
		TT_RateMap(i);
	}
	TT_StatsSave("map end");
}

void TT_StatsPlayerConnect(edict_t *p)
{
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return;
	memset(&g_ss[idx], 0, sizeof(g_ss[idx]));
	g_ss[idx].rec = -1;
	TT_Resolve(idx, p);
}

void TT_StatsPlayerDisconnect(edict_t *p)
{
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return;
	TT_Flush(idx);
	TT_RateMap(idx);
	memset(&g_ss[idx], 0, sizeof(g_ss[idx]));
	g_ss[idx].rec = -1;
	TT_StatsMenuGone(p);
}

void TT_StatsIgnoreDeathsOf(edict_t *p) { g_ignoreDeaths = p; }

// ---------------------------------------------------------------------------
// Weapons: which one did this?
static int TT_MeleeByModel(edict_t *att)
{
	const char *m = att->v.weaponmodel ? STRING(att->v.weaponmodel) : "";
	if (ContainsI(m, "p_spanner"))  return W_SPANNER;
	if (ContainsI(m, "p_knife"))    return W_KNIFE;
	if (ContainsI(m, "p_medkit"))   return W_MEDIKIT;
	if (ContainsI(m, "p_crowbar") || ContainsI(m, "p_umbrella")) return W_AXE;
	return -1;
}

static int TT_GunByModel(edict_t *att)
{
	const char *m = att->v.weaponmodel ? STRING(att->v.weaponmodel) : "";
	if (ContainsI(m, "p_sniper"))   return W_SNIPER;
	if (ContainsI(m, "p_mini"))     return W_AC;
	if (ContainsI(m, "p_shotgun"))  return W_SUPERSHOTGUN;
	if (ContainsI(m, "p_smallshotgun")) return W_SHOTGUN;
	if (ContainsI(m, "p_9mmhandgun")) return W_TRANQ;
	return W_OTHER;
}

// What hit them, given who and through what.
// THE PYRO'S ROCKETS. CTFIncendiaryCRocket::Spawn (tfc.so 0xf29f0) gives the
// incendiary cannon's rocket the same classname as the soldier's -
// "tf_rpg_rocket", netname "rocket" - so its hits, damage and kills (the kill
// log says "rocket") looked like the rocket launcher's. The badlands run as a
// pyro put 106 hits and 1071 damage on the rocket launcher with no rocket
// fired. Only a soldier has the rocket launcher and only a pyro the
// incendiary cannon, so the owner's class tells them apart.
static int TT_FixRocket(int w, edict_t *owner)
{
	if (w == W_ROCKET && owner && (int)owner->v.playerclass == 7)
		return W_INCENDIARY;
	return w;
}

static int TT_WeaponOf(int attIdx, edict_t *att, edict_t *inflictor)
{
	float now = gpGlobals->time;
	TTSession &a = g_ss[attIdx];
	if (inflictor == att)
	{
		// Hitscan or melee: the event of the shot that just happened names it.
		int melee = TT_MeleeByModel(att);
		if (melee >= 0)
			return melee;
		if (a.lastEventW > 0 && now - a.lastEventAt < 1.0f)
			return a.lastEventW;
		return TT_GunByModel(att);
	}
	const char *cls = STRING(inflictor->v.classname);
	if (!strcasecmp(cls, "tf_nailgun_nail"))
	{
		// Nailgun, super nailgun and railgun all fire this entity.
		if (a.lastNailW > 0 && now - a.lastNailAt < 3.0f)
			return a.lastNailW;
		return W_NAILGUN;
	}
	int w = TT_ClassWeapon(cls);
	if (w == W_AXE)
	{
		int m = TT_MeleeByModel(att);   // spanner/umbrella share the axe's class family
		if (m >= 0)
			w = m;
	}
	return TT_FixRocket(w, att);
}

// ---------------------------------------------------------------------------
// Engine events
void TT_StatsEventPrecached(const char *name, unsigned short index)
{
	if (!name || index >= 1024)
		return;
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	int w = TT_MapName(g_eventMap, base);
	if (w > 0)
		g_eventW[index] = (unsigned short)(w + 1);
}

void TT_StatsEventPlayed(const edict_t *invoker, unsigned short index)
{
	if (!g_st.enabled || index >= 1024 || !g_eventW[index])
		return;
	int idx = TT_IdxOf(invoker);
	if (!idx)
		return;
	int w = g_eventW[index] - 1;
	edict_t *e = INDEXENT(idx);
	// The engineer's spanner and the civilian's umbrella swing with the axe's event.
	if (w == W_AXE)
	{
		int m = TT_MeleeByModel(e);
		if (m >= 0)
			w = m;
	}
	TT_BUMP(idx, shots, 1);
	TT_BUMPW(idx, w, shots, 1);
	g_ss[idx].lastEventW = w;
	g_ss[idx].lastEventAt = gpGlobals->time;
	if (w == W_NAILGUN || w == W_SUPERNAILGUN || w == W_RAILGUN)
	{
		g_ss[idx].lastNailW = w;
		g_ss[idx].lastNailAt = gpGlobals->time;
	}
}

// HEADSHOTS. TFC has exactly one: the sniper rifle's charged shot (damage
// flag 0x10000000) landing in hitgroup 1 - CBasePlayer::TraceAttack (tfc.so
// 0xc5320) doubles the damage, tells the sniper "#Sniper_headshot", and makes
// the kill log say "headshot". No other weapon has a head multiplier. The
// first version counted any trace ending in a head, which also caught the
// sniper's laser dot and bots' line-of-sight checks and credited shotguns with
// hundreds of "headshots"; this counts TFC's own verdict instead.
void TT_StatsPrivateText(edict_t *to, const char *msg)
{
	if (!g_st.enabled || !msg || strcasecmp(msg, "#Sniper_headshot"))
		return;
	int idx = TT_IdxOf(to);
	if (!idx)
		return;
	TT_BUMP(idx, headshots, 1);
	TT_BUMPW(idx, W_SNIPER, headshots, 1);
}

static edict_t *g_carriedItem[TT_MAX_PLAYERS + 1];   // this frame
static edict_t *g_lastItem[TT_MAX_PLAYERS + 1];      // the last item each player carried

// CAPTURE DEBUGGING. Caps differ from map to map (flags carried to a point,
// command points, keys...), so while the detection is being checked against
// real maps every related event goes to tt_trace.log with a "CAPDBG" prefix:
// item pickups and losses (with the item's netname/model/goal numbers), every
// TeamScore change, every "triggered" log line, and every broadcast TextMsg
// ("%s CAPTURED the BLUE flag!" and the like). stats_debug_caps 0 turns it off.
static const char *TT_ItemDesc(edict_t *it)
{
	static char buf[160];
	if (!it || it->free)
		return "(none)";
	_snprintf_wc(buf, sizeof(buf) - 1, "#%d %s netname=\"%s\" model=\"%s\" team=%d owner=%d aiment=%d movetype=%d",
		ENTINDEX(it), it->v.classname ? STRING(it->v.classname) : "?",
		it->v.netname ? STRING(it->v.netname) : "", it->v.model ? STRING(it->v.model) : "",
		it->v.team, it->v.owner ? ENTINDEX(it->v.owner) : 0, it->v.aiment ? ENTINDEX(it->v.aiment) : 0,
		it->v.movetype);
	buf[sizeof(buf) - 1] = 0;
	return buf;
}

void TT_StatsTeamScore(const char *team, int score)
{
	if (!team)
		return;
	static const char *defaults[] = { "", "blue", "red", "yellow", "green" };
	int t = 0;
	for (int k = 1; k <= TT_MAX_TEAMS && !t; k++)
		if (!strcasecmp(team, defaults[k]) || (g_map.name[k][0] && !strcasecmp(team, g_map.name[k])))
			t = k;
	if (t)
	{
		if (g_st.debugCaps && score != (int)g_teamScore[t])
		{
			TT_Trace("CAPDBG TeamScore \"%s\" (team %d): %d -> %d", team, t, (int)g_teamScore[t], score);
			if (score > g_teamScore[t])
				for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
				{
					edict_t *e = TT_Player(i);
					if (!e || TT_PlayerTeam(e) != t)
						continue;
					TT_Trace("CAPDBG   team %d member %s: carrying=%d lostItemAt=%.2f alive=%d item=%s", t,
						STRING(e->v.netname), g_ss[i].carrying ? 1 : 0, g_ss[i].lostItemAt,
						(e->v.deadflag == DEAD_NO && e->v.health > 0) ? 1 : 0, TT_ItemDesc(g_lastItem[i]));
				}
		}
		if (score > g_teamScore[t])
			g_teamScoreUpAt[t] = gpGlobals->time;
		g_teamScore[t] = (float)score;
		return;
	}
	if (g_st.debugCaps)
		TT_Trace("CAPDBG TeamScore for a team name not matched to a number: \"%s\" = %d", team, score);
	// A team name we cannot place: remember the score by name and let any
	// team's carrier match a rise.
	static char names[8][32];
	static int scores[8], n = 0;
	int k = 0;
	while (k < n && strcasecmp(names[k], team))
		k++;
	if (k == n && n < 8)
	{
		strncpy(names[n], team, 31);
		names[n][31] = 0;
		scores[n] = score;
		n++;
		return;
	}
	if (k < n)
	{
		if (score > scores[k])
			for (int q = 1; q <= TT_MAX_TEAMS; q++)
				g_teamScoreUpAt[q] = gpGlobals->time;
		scores[k] = score;
	}
}

void TT_StatsIntermission(void)
{
	g_intermission = true;
}

// ---------------------------------------------------------------------------
// Damage, read once per frame per victim.
// PROJECTILES THAT ARE ALREADY GONE. A rocket or grenade damages its victims
// as it explodes and is freed in the same frame - so by the victim's next
// PreThink its edict is free. The first live run showed exactly that: 746
// rockets fired, 14 hits counted. So every frame the owner and weapon of each
// owned entity is noted here, keyed by edict and serial number; a freed edict
// has serial+1, and the engine does not hand a freed edict out again for half
// a second, so the note is still the right one when the damage is read.

static void TT_NoteEntities(void)
{
	int maxEnt = gpGlobals->maxEntities;
	if (maxEnt > TT_MAX_EDICTS)
		maxEnt = TT_MAX_EDICTS;
	for (int i = gpGlobals->maxClients + 1; i < maxEnt; i++)
	{
		edict_t *e = INDEXENT(i);
		if (!e || e->free || !e->v.owner)
		{
			if (e && !e->free)
				g_note[i].serial = -1;
			continue;
		}
		int o = TT_IdxOf(e->v.owner);
		if (!o)
		{
			g_note[i].serial = -1;
			continue;
		}
		bool fresh = (g_note[i].serial != e->serialnumber || g_note[i].owner != o);
		if (fresh)
		{
			int w = TT_WeaponOf(o, e->v.owner, e);
			g_note[i].serial = e->serialnumber;
			g_note[i].owner = (short)o;
			g_note[i].w = (short)w;
			// A thrown grenade/detpack appearing is its "shot" - there is no
			// weapon event for a throw. Not the pieces a grenade makes when it
			// goes off: a MIRV splits into bomblets (CTFBomblet::setName, tfc.so
			// 0xf17b0: "tf_weapon_mirvbomblet") and a nail grenade sprays nails
			// (CreateNailGrenNail 0xf35e0: "tf_weapon_nailgrenadenail"), both
			// owned by the thrower. The avanti run counted 40 MIRV "shots" for 8
			// throws - each bomblet was being counted as a throw.
			const char *ncls = STRING(e->v.classname);
			if (TT_IsThrown(w) && !ContainsI(ncls, "bomblet") && !ContainsI(ncls, "grenadenail"))
			{
				TT_BUMP(o, shots, 1);
				TT_BUMPW(o, w, shots, 1);
			}
		}
	}
}

// Who and what, for an inflictor that may have been freed since.
static bool TT_Attribute(edict_t *inf, int *attIdx, int *w)
{
	int i = ENTINDEX(inf);
	*attIdx = TT_IdxOf(inf);
	if (*attIdx)
	{
		*w = TT_WeaponOf(*attIdx, inf, inf);
		return true;
	}
	if (i > 0 && i < TT_MAX_EDICTS && g_note[i].serial >= 0
		&& (g_note[i].serial == inf->serialnumber || g_note[i].serial + 1 == inf->serialnumber))
	{
		*attIdx = g_note[i].owner;
		*w = g_note[i].w;
		return TT_Player(*attIdx) != NULL;
	}
	// Not noted (fired and exploded within one frame): the engine's ED_Free
	// clears model, origin and the like but not owner or classname, so those
	// are still readable on a just-freed edict.
	if (inf->v.owner && (*attIdx = TT_IdxOf(inf->v.owner)) != 0)
	{
		*w = TT_WeaponOf(*attIdx, inf->v.owner, inf);
		return true;
	}
	return false;
}

static void TT_RecordDamage(int vic, edict_t *victim, float take, float save)
{
	edict_t *inf = victim->v.dmg_inflictor;
	if (!inf)
		return;
	float now = gpGlobals->time;
	int attIdx = 0, w = W_OTHER;
	if (!TT_Attribute(inf, &attIdx, &w))
	{
		// World, falling, a sentry (whose owner TFC keeps privately)...
		const char *cls = inf->v.classname ? STRING(inf->v.classname) : "worldspawn";
		int ww = TT_ClassWeapon(cls);
		g_ss[vic].lastDmgFrom = 0;
		g_ss[vic].lastDmgW = ww >= 0 ? ww : W_OTHER;
		g_ss[vic].lastDmgAt = now;
		return;
	}
	edict_t *att = INDEXENT(attIdx);
	g_ss[vic].lastDmgFrom = attIdx;
	g_ss[vic].lastDmgW = w;
	g_ss[vic].lastDmgAt = now;

	// Self and team damage are not hits.
	if (attIdx == vic || (int)att->v.team == (int)victim->v.team)
		return;
	float dmg = take + save;
	TT_BUMP(attIdx, damage, dmg);
	TT_BUMPW(attIdx, w, damage, dmg);
	TTSession &a = g_ss[attIdx];
	int infIdx = ENTINDEX(inf);
	// One shot that hurts several people is still one hit.
	if (!(a.lastHitInflictor == infIdx && fabs(a.lastHitAt - now) < 0.05f))
	{
		TT_BUMP(attIdx, hits, 1);
		TT_BUMPW(attIdx, w, hits, 1);
		a.lastHitInflictor = infIdx;
		a.lastHitAt = now;
	}
}

void TT_StatsPreThink(edict_t *p)
{
	if (!g_st.enabled)
		return;
	int idx = TT_IdxOf(p);
	if (!idx)
		return;
	float take = p->v.dmg_take - g_ss[idx].consumedTake;
	g_ss[idx].consumedTake = 0;
	if (take < 0)
		take = 0;
	float save = p->v.dmg_save;
	if (take + save > 0.0f)
		TT_RecordDamage(idx, p, take, save);
}

// ---------------------------------------------------------------------------
// Log lines: kills, suicides, 2fort-style captures.
//
// A player looks like  Name<uid><STEAMID><Team>  - parse from the RIGHT, since
// names may contain anything. Returns the slot for that uid, or 0.
static int TT_ParsePlayerRef(const char *start, const char *end)
{
	// end points at the closing '>' of <Team>. Walk back over three <...> groups.
	const char *p = end;
	const char *groups[3];
	for (int g = 0; g < 3; g++)
	{
		while (p > start && *p != '<')
			p--;
		if (p <= start)
			return 0;
		groups[g] = p;
		p--;
	}
	int uid = atoi(groups[2] + 1);
	if (uid <= 0)
		return 0;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (e && GETPLAYERUSERID(e) == uid)
			return i;
	}
	return 0;
}

static int TT_KillWeapon(int killer, int victim, const char *logName)
{
	TTSession &v = g_ss[victim];
	if (killer && v.lastDmgFrom == killer && gpGlobals->time - v.lastDmgAt < 2.5f)
		return v.lastDmgW;
	int w = TT_MapName(g_classMap, logName);
	if (w >= 0)
		return killer ? TT_FixRocket(w, INDEXENT(killer)) : w;
	static char seen[16][32];
	static int nSeen = 0;
	for (int i = 0; i < nSeen; i++)
		if (!strcasecmp(seen[i], logName))
			return W_OTHER;
	if (nSeen < 16)
	{
		strncpy(seen[nSeen], logName, 31);
		seen[nSeen][31] = 0;
		nSeen++;
		TT_Trace("Stats: kill weapon \"%s\" is not in the table - counted as \"other\"", logName);
	}
	return W_OTHER;
}

// The victim's damage of this frame, before TFC's death code can clear it.
static void TT_FlushVictimDamage(int vic)
{
	edict_t *v = INDEXENT(vic);
	float take = v->v.dmg_take - g_ss[vic].consumedTake;
	if (take > 0.0f || v->v.dmg_save > 0.0f)
	{
		TT_RecordDamage(vic, v, take > 0 ? take : 0, v->v.dmg_save);
		g_ss[vic].consumedTake += take > 0 ? take : 0;
	}
}

void TT_StatsLogLine(const char *line)
{
	if (!g_st.enabled || !line)
		return;

	if (g_st.debugCaps && strstr(line, " triggered \""))
	{
		char l[300];
		strncpy(l, line, sizeof(l) - 1);
		l[sizeof(l) - 1] = 0;
		l[strcspn(l, "\r\n")] = 0;
		TT_Trace("CAPDBG log: %s", l);
	}

	// "A" killed "B" with "w"
	const char *k = strstr(line, "\" killed \"");
	if (k)
	{
		const char *with = strstr(k, "\" with \"");
		if (!with)
			return;
		int killer = TT_ParsePlayerRef(line, k - 1);
		int victim = TT_ParsePlayerRef(k + 10, with - 1);
		char wname[48];
		const char *ws = with + 8;
		size_t n = strcspn(ws, "\"");
		if (n >= sizeof(wname))
			n = sizeof(wname) - 1;
		memcpy(wname, ws, n);
		wname[n] = 0;
		if (!victim)
			return;
		TT_FlushVictimDamage(victim);
		int w = TT_KillWeapon(killer, victim, wname);
		bool carrier = g_ss[victim].carrying;
		edict_t *ke = killer ? INDEXENT(killer) : NULL;
		edict_t *ve = INDEXENT(victim);
		TT_BUMP(victim, deaths, 1);
		TT_BUMPW(victim, w, deaths, 1);
		g_ss[victim].streak = 0;
		if (killer && killer != victim)
		{
			if (w == W_SENTRY || w == W_DISPENSER)
				g_ss[killer].forceCls = 9;
			bool team = (int)ke->v.team == (int)ve->v.team;
			if (team)
				TT_BUMP(killer, teamkills, 1);
			else
			{
				TT_BUMP(killer, kills, 1);
				TT_BUMPW(killer, w, kills, 1);
				g_ss[killer].streak++;
				TT_StreakNote(killer);
				if (g_st.rivals && g_ss[killer].rec >= 0 && g_ss[victim].rec >= 0
					&& g_ss[killer].rec != g_ss[victim].rec
					&& (!g_db[g_ss[killer].rec].bot || !g_db[g_ss[victim].rec].bot))
				{
					int rk = g_ss[killer].rec, rv = g_ss[victim].rec;
					TTVs *v = TT_VsFind(rk, rv, true);
					if (v->a == rk) { v->ab++; v->mab++; }
					else            { v->ba++; v->mba++; }
				}
				if (carrier)
				{
					TT_BUMP(killer, carrierKills, 1);
					TT_BUMP(victim, carrierDeaths, 1);
					TT_Trace("Stats: %s killed flag carrier %s", STRING(ke->v.netname), STRING(ve->v.netname));
				}
			}
			g_ss[killer].forceCls = 0;
		}
		return;
	}

	// "A" committed suicide with "w"
	const char *s = strstr(line, "\" committed suicide with \"");
	if (s)
	{
		int who = TT_ParsePlayerRef(line, s - 1);
		if (!who || INDEXENT(who) == g_ignoreDeaths)
			return; // our own team change, not their doing
		TT_BUMP(who, suicides, 1);
		TT_BUMP(who, deaths, 1);
		g_ss[who].streak = 0;
		return;
	}

	// Engineer buildings (TFC's own log lines, tfc.so strings):
	//   "A" triggered "Built_Dispenser" / "Sentry_Built_Level_1"
	//   "A" triggered "Sentry_Destroyed" / "Dispenser_Destroyed" against "B" with "w"
	// (with no "against", the engineer blew up or lost their own - not counted).
	{
		const char *bt = strstr(line, "\" triggered \"");
		// Medic (CTFMedikit::AxeHit, tfc.so 0x125830):
		//   "A" triggered "Medic_Heal" against "B"  - only logged when B was below
		//     max health (healed to max) or within 50 over it (+5, the overheal)
		//   "A" triggered "Medic_Cured_Concussion" / "_Hallucinations" /
		//     "_Tranquilisation" / "_Infection" / "Medic_Doused_Fire" against "B"
		// The log line has no amount: it is B's health now minus at the start of
		// the frame.
		if (bt && (!strncmp(bt + 13, "Medic_Heal\" against \"", 21)
			|| !strncmp(bt + 13, "Medic_Cured_", 12) || !strncmp(bt + 13, "Medic_Doused_Fire\"", 18)))
		{
			int who = TT_ParsePlayerRef(line, bt - 1);
			const char *ag = strstr(bt + 13, "\" against \"");
			const char *lastQ = strrchr(line, '"');
			int to = (ag && lastQ > ag + 11) ? TT_ParsePlayerRef(ag + 11, lastQ - 1) : 0;
			if (who && to && who != to)
			{
				if (bt[19] == 'H') // Medic_Heal
				{
					TT_BUMP(who, heals, 1);
					edict_t *te = INDEXENT(to);
					float gave = te->v.health - g_ss[to].hpSnap;
					if (gave > 0 && gave <= 300.0f)
						TT_BUMP(who, healed, gave);
					g_ss[to].hpSnap = te->v.health;
				}
				else
					TT_BUMP(who, cures, 1);
			}
			return;
		}
		if (bt && (!strncmp(bt + 13, "Built_Dispenser\"", 16) || !strncmp(bt + 13, "Sentry_Built_Level_1\"", 21)))
		{
			int who = TT_ParsePlayerRef(line, bt - 1);
			if (who)
			{
				g_ss[who].forceCls = 9;
				TT_BUMP(who, builds, 1);
				g_ss[who].forceCls = 0;
			}
			return;
		}
		if (bt && (!strncmp(bt + 13, "Sentry_Destroyed\" against \"", 27)
			|| !strncmp(bt + 13, "Dispenser_Destroyed\" against \"", 30)))
		{
			int who = TT_ParsePlayerRef(line, bt - 1);
			const char *ag = strstr(bt, "\" against \"") + 11;
			const char *wi = strstr(ag, "\" with \"");
			int owner = wi ? TT_ParsePlayerRef(ag, wi - 1) : 0;
			if (who && owner && who != owner
				&& (int)INDEXENT(who)->v.team != (int)INDEXENT(owner)->v.team)
			{
				TT_BUMP(who, buildKills, 1);
				TT_Trace("Stats: %s destroyed %s's %s", STRING(INDEXENT(who)->v.netname),
					STRING(INDEXENT(owner)->v.netname), bt[13] == 'S' ? "sentry gun" : "dispenser");
			}
			return;
		}
	}

	// Any goal a player activates: remembered for the cap check in the frame
	// loop (the goal's netname is what TFC logs - see DoResults, tfc.so 0xffa96).
	const char *tr = strstr(line, "\" triggered \"");
	if (tr && !strstr(line, "\" against \""))
	{
		int who = TT_ParsePlayerRef(line, tr - 1);
		if (who)
		{
			char name[96];
			const char *ns = tr + 13;
			size_t n2 = strcspn(ns, "\"");
			if (n2 >= sizeof(name))
				n2 = sizeof(name) - 1;
			memcpy(name, ns, n2);
			name[n2] = 0;
			TTSession &s2 = g_ss[who];
			if (gpGlobals->time - s2.trigAt > 0.25f)
				s2.trigCap = s2.trigReturn = false;
			s2.trigAt = gpGlobals->time;
			if (g_st.capWord[0] && ContainsI(name, g_st.capWord))
				s2.trigCap = true;
			if (g_st.returnWord[0] && ContainsI(name, g_st.returnWord))
				s2.trigReturn = true;
		}
	}

	// "A" triggered "Captured_Red_Flag" / "Captured_Blue_Flag" (2fort-style maps)
	const char *t = strstr(line, "\" triggered \"Captured_");
	if (t)
	{
		int who = TT_ParsePlayerRef(line, t - 1);
		if (who)
		{
			TT_BUMP(who, caps, 1);
			g_ss[who].capLoggedAt = gpGlobals->time; // do not count it again from the score
			TT_Trace("Stats: %s captured (log)", STRING(INDEXENT(who)->v.netname));
		}
	}
}

// ---------------------------------------------------------------------------
// Frame: time on a team, carriers, caps, the summary, periodic saves.

static void TT_UpdateCarriers(bool nowCarrying[TT_MAX_PLAYERS + 1])
{
	memset(nowCarrying, 0, sizeof(bool) * (TT_MAX_PLAYERS + 1));
	memset(g_carriedItem, 0, sizeof(g_carriedItem));
	static const char *names[] = { "item_tfgoal", "i_t_g" };
	for (int n = 0; n < 2; n++)
	{
		edict_t *it = NULL;
		while (!FNullEnt(it = FIND_ENTITY_BY_STRING(it, "classname", names[n])))
		{
			if (it->v.movetype != MOVETYPE_FOLLOW || it->v.aiment != it->v.owner)
				continue;
			int i = TT_IdxOf(it->v.aiment);
			if (i)
			{
				nowCarrying[i] = true;
				g_carriedItem[i] = it;
				g_lastItem[i] = it;
			}
		}
	}
}

static void TT_Summary(void);

void TT_StatsBroadcastText(const char *a, const char *b)
{
	if (!g_st.enabled || !g_st.debugCaps || !a)
		return;
	TT_Trace("CAPDBG TextMsg to all: \"%s\"%s%s%s", a, b && b[0] ? " \"" : "", b && b[0] ? b : "", b && b[0] ? "\"" : "");
}

void TT_StatsFrame(void)
{
	if (!g_st.enabled)
		return;
	float now = gpGlobals->time;
	float dt = gpGlobals->frametime;
	if (dt < 0 || dt > 1.0f)
		dt = 0;

	TT_NoteEntities();

	bool carrying[TT_MAX_PLAYERS + 1];
	TT_UpdateCarriers(carrying);

	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (!e || TT_IsHLTV(e))
			continue;
		TTSession &s = g_ss[i];
		if (s.rec < 0)
			TT_Resolve(i, e);
		s.hpSnap = e->v.health;
		int team = TT_PlayerTeam(e);
		if (team && !g_intermission)
			TT_BUMP(i, seconds, dt);

		bool alive = e->v.deadflag == DEAD_NO && e->v.health > 0;
		if (carrying[i] && !s.carrying && alive)
		{
			TT_BUMP(i, pickups, 1);
			TT_Trace("Stats: %s picked up a goal item", STRING(e->v.netname));
			if (g_st.debugCaps)
				TT_Trace("CAPDBG pickup: %s (team %d) now carries %s", STRING(e->v.netname), team, TT_ItemDesc(g_carriedItem[i]));
		}
		if (!carrying[i] && s.carrying)
		{
			if (alive)
			{
				s.lostItemAt = now;         // captured - if the score goes up
				s.lostItemTeam = team;
			}
			if (g_st.debugCaps)
				TT_Trace("CAPDBG item lost: %s (team %d) %s - item now %s", STRING(e->v.netname), team,
					alive ? "ALIVE (cap candidate)" : "dead/dying (dropped)", TT_ItemDesc(g_lastItem[i]));
		}
		s.carrying = carrying[i];

		// Cap candidate. Every capture in the first live run (2fort, ravelin,
		// flagrun, avanti, baconbowl, copper3, cz2) happened in ONE frame: the
		// carrier activated a goal (a "triggered" log line), lost the item, and -
		// on most maps - the team score went up. cz2's zone captures do not
		// score on the spot (the score ticks later for zones held), and
		// flagrun's "return result" puts your own flag back without scoring.
		// So: score up at that moment = cap; otherwise a goal with the cap
		// word in its name (and not the return word) = cap; otherwise not.
		if (s.lostItemAt > 0 && s.capLoggedAt > 0 && fabs(s.capLoggedAt - s.lostItemAt) < 3.0f)
			s.lostItemAt = 0; // the log line already counted this one
		if (s.lostItemAt > 0 && s.lostItemTeam >= 1 && s.lostItemTeam <= TT_MAX_TEAMS)
		{
			float up = g_teamScoreUpAt[s.lostItemTeam];
			bool scored = up > 0 && fabs(up - s.lostItemAt) <= 0.5f;
			bool goal = s.trigAt > 0 && fabs(s.trigAt - s.lostItemAt) <= 0.3f;
			const char *how = NULL;
			if (scored)
				how = goal ? "item delivered to a goal, team score rose" : "item gone, team score rose";
			else if (goal && s.trigCap && !s.trigReturn)
				how = "item delivered to a capture goal (no score on the spot)";
			if (how)
			{
				TT_BUMP(i, caps, 1);
				TT_Trace("Stats: %s captured (%s)", STRING(e->v.netname), how);
				s.lostItemAt = 0;
			}
			else if (now - s.lostItemAt > 0.6f)
			{
				if (g_st.debugCaps)
					TT_Trace("CAPDBG NOT counted: %s lost their item alive at t=%.2f - %s (team %d last score rise t=%.2f, goal at t=%.2f cap=%d return=%d)",
						STRING(e->v.netname), s.lostItemAt,
						(goal && s.trigReturn) ? "a flag RETURN" : "no score and no capture goal",
						s.lostItemTeam, up, s.trigAt, s.trigCap ? 1 : 0, s.trigReturn ? 1 : 0);
				s.lostItemAt = 0;
			}
		}
	}

	if (g_intermission && !g_summaryDone)
	{
		g_summaryDone = true;
		TT_Summary();
		TT_StatsMapEnd();
	}

	if (now >= g_nextSave)
	{
		g_nextSave = now + g_st.saveInterval;
		for (int i = 1; i <= TT_MAX_PLAYERS; i++)
			TT_Flush(i);
		if (g_dbDirty)
			TT_StatsSave("periodic");
	}
}

// ---------------------------------------------------------------------------
// Output
static int TT_Acc(int hits, int shots)
{
	if (shots <= 0)
		return 0;
	int a = (int)(100.0f * hits / shots + 0.5f);
	return a > 100 ? 100 : a;
}

// Lifetime = record + what has not been flushed into it yet.
static void TT_Lifetime(int idx, TTBlock &out)
{
	memset(&out, 0, sizeof(out));
	if (g_ss[idx].rec >= 0)
		out = g_db[g_ss[idx].rec].life;
	BlockAdd(out, g_ss[idx].pending);
}

static int TT_RankOf(int rec, int *total)
{
	float r = g_db[rec].rating;
	int better = 0, n = 0;
	for (size_t i = 0; i < g_db.size(); i++)
	{
		const TTRecord &o = g_db[i];
		if (o.bot || o.rating < 0 || o.life.seconds / 60.0f < g_st.minRankMinutes)
			continue;
		n++;
		if (o.rating > r)
			better++;
	}
	*total = n;
	return better + 1;
}

static edict_t *TT_FindByName(const char *arg)
{
	edict_t *found = NULL;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (!e)
			continue;
		if (!strcasecmp(STRING(e->v.netname), arg))
			return e;
		if (ContainsI(STRING(e->v.netname), arg))
		{
			if (found)
				return NULL;
			found = e;
		}
	}
	return found;
}

// Window text is built into one buffer and sent as the MOTD panel's text
// (TT_ShowWindow). The panel uses a proportional font, so this is written as
// "label: value" lines rather than aligned columns.
struct TTText
{
	char   buf[1536];
	size_t len;
	TTText() : len(0) { buf[0] = 0; }
	void add(const char *fmt, ...)
	{
		if (len >= sizeof(buf) - 1)
			return;
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
		va_end(ap);
		if (n < 0)
			return;
		len += (size_t)n;
		if (len > sizeof(buf) - 1)
			len = sizeof(buf) - 1;
		buf[len] = 0;
	}
};

static const char *TT_WeaponTitle(int w)
{
	static const char *t[W_COUNT] =
	{
		"Other", "Shotgun", "Super shotgun", "Nailgun", "Super nailgun", "Railgun",
		"Rocket launcher", "Incendiary cannon", "Grenade launcher", "Pipebomb launcher", "Sniper rifle", "Autorifle",
		"Assault cannon", "Flamethrower", "Tranquiliser gun", "Axe", "Spanner", "Knife", "Medikit",
		"Hand grenade", "Concussion grenade", "Nail grenade", "MIRV grenade", "Napalm grenade",
		"Gas grenade", "EMP grenade", "Caltrops", "Detpack", "Sentry gun", "Dispenser",
		"Infection", "Burning", "Telefrag", "World / falling",
	};
	return (w >= 0 && w < W_COUNT) ? t[w] : "?";
}

static void TT_TimeText(float seconds, char *out, size_t len)
{
	if (seconds < 60.0f)
		_snprintf_wc(out, len - 1, "%d s", (int)seconds);
	else if (seconds < 3600.0f)
		_snprintf_wc(out, len - 1, "%d min", (int)(seconds / 60.0f));
	else
		_snprintf_wc(out, len - 1, "%.1f h", seconds / 3600.0f);
	out[len - 1] = 0;
}

static void TT_RankText(int idx, char *out, size_t len)
{
	int rec = g_ss[idx].rec;
	out[0] = 0;
	if (rec >= 0 && !g_db[rec].bot && g_db[rec].rating >= 0
		&& g_db[rec].life.seconds / 60.0f >= g_st.minRankMinutes)
	{
		int total;
		int r = TT_RankOf(rec, &total);
		_snprintf_wc(out, len - 1, "#%d of %d", r, total);
	}
	else if (rec >= 0 && g_db[rec].bot)
		_snprintf_wc(out, len - 1, "bots are not ranked");
	else
		_snprintf_wc(out, len - 1, "not ranked yet (%.0f minutes on a team and a finished map)", g_st.minRankMinutes);
	out[len - 1] = 0;
}

// ---------------------------------------------------------------------------
// Whose numbers a window or menu shows: a player in the server now (this map
// plus lifetime), or - once they have left - just their stored record.
struct TTView
{
	const char   *name;
	const TTBlock *map;      // NULL = not in the server any more
	TTBlock       life;
	int           rec;
	int           slot;
};

static bool TT_ViewOf(int slot, int rec, TTView &v)
{
	v.map = NULL;
	v.slot = 0;
	v.rec = rec;
	if (slot >= 1 && slot <= TT_MAX_PLAYERS && TT_Player(slot) && g_ss[slot].rec == rec && rec >= 0)
	{
		v.slot = slot;
		v.map = &g_ss[slot].map;
		TT_Lifetime(slot, v.life);
		v.name = STRING(INDEXENT(slot)->v.netname);
		return true;
	}
	if (rec < 0 || rec >= (int)g_db.size())
		return false;
	v.life = g_db[rec].life;
	v.name = g_db[rec].name;
	return true;
}

static float TT_ViewRating(const TTView &v)
{
	if (v.slot)
		return TT_StatsRating(INDEXENT(v.slot));
	return g_db[v.rec].rating < 0 ? 0.0f : g_db[v.rec].rating;
}

// Short rank text for the menu.
static void TT_RankShort(int rec, char *out, size_t len)
{
	out[0] = 0;
	if (rec < 0)
		_snprintf_wc(out, len - 1, "not ranked yet");
	else if (g_db[rec].bot)
		_snprintf_wc(out, len - 1, "bots are not ranked");
	else if (g_db[rec].rating >= 0 && g_db[rec].life.seconds / 60.0f >= g_st.minRankMinutes)
	{
		int total;
		int r = TT_RankOf(rec, &total);
		_snprintf_wc(out, len - 1, "rank #%d of %d", r, total);
	}
	else
		_snprintf_wc(out, len - 1, "not ranked yet");
	out[len - 1] = 0;
}

// Weapons of a set of counters, most kills first.
static int TT_WeaponOrder(const TTCounts &b, int *order)
{
	int n = 0;
	for (int w = 0; w < W_COUNT; w++)
		if (b.w[w].kills || b.w[w].shots || b.w[w].hits)
			order[n++] = w;
	std::sort(order, order + n, [&b](int x, int y) {
		if (b.w[x].kills != b.w[y].kills) return b.w[x].kills > b.w[y].kills;
		if (b.w[x].damage != b.w[y].damage) return b.w[x].damage > b.w[y].damage;
		return b.w[x].shots > b.w[y].shots;
	});
	return n;
}

// Classes someone has played, most time first.
static int TT_ClassOrder(const TTBlock &life, int *order)
{
	int n = 0;
	for (int c = 1; c < TT_CLASSES; c++)
		if (life.cls[c].seconds >= 30.0f || life.cls[c].kills || life.cls[c].deaths)
			order[n++] = c;
	std::sort(order, order + n, [&life](int x, int y) { return life.cls[x].seconds > life.cls[y].seconds; });
	return n;
}

static float TT_KD(int k, int d)
{
	return k / (float)(d ? d : 1);
}

// ---------------------------------------------------------------------------
// THE PAGED MENU. !stats and !top10 open a numbered menu on the left of the
// screen (TT_ShowMenu): 8 = back, 9 = next, 0 = close. The game carries on
// underneath it. !stats pages: overview, one page per class played, all
// weapons. !top10 pages: 7 players each.
enum { TT_MENU_NONE = 0, TT_MENU_STATS, TT_MENU_TOP };
struct TTMenu
{
	int   kind;
	int   slot, rec;     // whose stats
	int   page;
	float until;         // gone by then (0 = until closed)
};
static TTMenu g_menu[TT_MAX_PLAYERS + 1];

#define TT_MENU_BODY 440   // text before the key line; the client holds 512

static void TT_MenuSend(edict_t *to, TTText &t, int page, int pages)
{
	int keys = 1 << 9; // 0 = close
	// Keep the body inside the client's buffer: cut at a line end.
	if (t.len > TT_MENU_BODY)
	{
		char *cut = t.buf + TT_MENU_BODY;
		while (cut > t.buf && *cut != '\n')
			cut--;
		*cut = 0;
		t.len = (size_t)(cut - t.buf);
		t.add("\n");
	}
	t.add("\n");
	if (page > 0)
	{
		t.add("\\r8.\\w Back   ");
		keys |= 1 << 7;
	}
	if (page < pages - 1)
	{
		t.add("\\r9.\\w Next   ");
		keys |= 1 << 8;
	}
	t.add("\\r0.\\w Close");
	int secs = g_st.menuTime > 0 ? (int)g_st.menuTime : -1;
	if (secs > 120)
		secs = 120;
	TT_ShowMenu(to, keys, secs, t.buf);
}

static void TT_StatsPage(edict_t *to, TTMenu &m)
{
	TTView v;
	if (!TT_ViewOf(m.slot, m.rec, v))
	{
		m.kind = TT_MENU_NONE;
		return;
	}
	int classes[TT_CLASSES];
	int nc = TT_ClassOrder(v.life, classes);
	int pages = 1 + nc + 1;           // overview, classes, all weapons
	if (m.page < 0) m.page = 0;
	if (m.page >= pages) m.page = pages - 1;

	static const TTBlock empty = TTBlock();
	const TTBlock &mp = v.map ? *v.map : empty;
	const TTBlock &lf = v.life;
	TTText t;
	t.add("\\y%s\\w   \\dpage %d/%d\\w\n", v.name, m.page + 1, pages);
	char rank[48], a[24], b[24];

	if (m.page == 0)
	{
		TT_RankShort(v.rec, rank, sizeof(rank));
		t.add("\\yOverview\\w  \\d(this map / lifetime)\\w\n");
		t.add("Rating %.1f - %s\n", TT_ViewRating(v), rank);
		t.add("Kills %d / %d   Deaths %d / %d\n", mp.kills, lf.kills, mp.deaths, lf.deaths);
		t.add("K/D %.2f / %.2f   Best streak %d / %d\n", TT_KD(mp.kills, mp.deaths), TT_KD(lf.kills, lf.deaths),
			mp.bestStreak, lf.bestStreak);
		t.add("Melee kills %d / %d\n", TT_MeleeKills(mp), TT_MeleeKills(lf));
		if (lf.heals || lf.cures)
			t.add("Healed %.0f / %.0f   Cures %d / %d\n", mp.healed, lf.healed, mp.cures, lf.cures);
		t.add("Caps %d / %d   Pickups %d / %d\n", mp.caps, lf.caps, mp.pickups, lf.pickups);
		t.add("Carrier kills %d / %d\n", mp.carrierKills, lf.carrierKills);
		t.add("Accuracy %d%% / %d%%   Headshots %d / %d\n", TT_Acc(mp.hits, mp.shots),
			TT_Acc(lf.hits, lf.shots), mp.headshots, lf.headshots);
		t.add("Damage %.0f / %.0f\n", mp.damage, lf.damage);
		if (lf.builds || lf.buildKills)
			t.add("Built %d / %d   Enemy buildings %d / %d\n", mp.builds, lf.builds, mp.buildKills, lf.buildKills);
		TT_TimeText(mp.seconds, a, sizeof(a));
		TT_TimeText(lf.seconds, b, sizeof(b));
		t.add("Time %s / %s\n", a, b);
	}
	else if (m.page <= nc)
	{
		int c = classes[m.page - 1];
		const TTCounts &cm = mp.cls[c], &cl = lf.cls[c];
		t.add("\\y%s\\w  \\d(this map / lifetime)\\w\n", g_clsTitle[c]);
		TT_TimeText(cm.seconds, a, sizeof(a));
		TT_TimeText(cl.seconds, b, sizeof(b));
		t.add("Time %s / %s   %.1f points per 10 min\n", a, b, TT_Perf(cl));
		t.add("Kills %d / %d   Deaths %d / %d\n", cm.kills, cl.kills, cm.deaths, cl.deaths);
		t.add("Best streak %d / %d   Melee kills %d / %d\n", cm.bestStreak, cl.bestStreak,
			TT_MeleeKills(cm), TT_MeleeKills(cl));
		if (cl.heals || cl.cures)
			t.add("Healed %.0f / %.0f   Cures %d / %d\n", cm.healed, cl.healed, cm.cures, cl.cures);
		if (cl.caps || cl.carrierKills || cl.pickups)
			t.add("Caps %d / %d   Carrier kills %d / %d\n", cm.caps, cl.caps, cm.carrierKills, cl.carrierKills);
		t.add("Accuracy %d%% / %d%%   Damage %.0f / %.0f\n", TT_Acc(cm.hits, cm.shots), TT_Acc(cl.hits, cl.shots),
			cm.damage, cl.damage);
		if (cl.headshots)
			t.add("Headshots %d / %d\n", cm.headshots, cl.headshots);
		if (cl.builds || cl.buildKills)
			t.add("Built %d / %d   Enemy buildings %d / %d\n", cm.builds, cl.builds, cm.buildKills, cl.buildKills);
		int order[W_COUNT];
		int n = TT_WeaponOrder(cl, order);
		if (n)
			t.add("\\dWeapons (lifetime): kills, accuracy\\w\n");
		for (int j = 0; j < n && j < 6; j++)
		{
			const TTWeaponStat &s = cl.w[order[j]];
			if (s.shots)
				t.add("%s  %d  -  %d%%\n", TT_WeaponTitle(order[j]), s.kills, TT_Acc(s.hits, s.shots));
			else
				t.add("%s  %d\n", TT_WeaponTitle(order[j]), s.kills);
		}
	}
	else
	{
		t.add("\\yAll weapons\\w  \\d(lifetime: kills, accuracy)\\w\n");
		int order[W_COUNT];
		int n = TT_WeaponOrder(lf, order);
		for (int j = 0; j < n && j < 10; j++)
		{
			const TTWeaponStat &s = lf.w[order[j]];
			if (s.shots)
				t.add("%s  %d  -  %d%%\n", TT_WeaponTitle(order[j]), s.kills, TT_Acc(s.hits, s.shots));
			else
				t.add("%s  %d\n", TT_WeaponTitle(order[j]), s.kills);
		}
		if (n > 10)
			t.add("\\d(say !weapons for all of them in your console)\\w\n");
	}
	TT_MenuSend(to, t, m.page, pages);
}

static void TT_RankedList(std::vector<int> &v)
{
	v.clear();
	for (size_t i = 0; i < g_db.size(); i++)
	{
		const TTRecord &o = g_db[i];
		if (!o.bot && o.rating >= 0 && o.life.seconds / 60.0f >= g_st.minRankMinutes)
			v.push_back((int)i);
	}
	std::sort(v.begin(), v.end(), [](int a, int b) { return g_db[a].rating > g_db[b].rating; });
}

#define TT_TOP_PER_PAGE 7
static void TT_TopPage(edict_t *to, TTMenu &m)
{
	std::vector<int> v;
	TT_RankedList(v);
	int pages = v.empty() ? 1 : (int)((v.size() + TT_TOP_PER_PAGE - 1) / TT_TOP_PER_PAGE);
	if (m.page < 0) m.page = 0;
	if (m.page >= pages) m.page = pages - 1;
	int me = g_ss[ENTINDEX(to)].rec;
	TTText t;
	t.add("\\yRankings\\w   \\dpage %d/%d\\w\n", m.page + 1, pages);
	t.add("\\drating = points per 10 minutes on a team\\w\n");
	if (v.empty())
		t.add("Nobody is ranked yet - it takes %.0f minutes\non a team and a finished map.\n", g_st.minRankMinutes);
	for (int j = m.page * TT_TOP_PER_PAGE; j < (int)v.size() && j < (m.page + 1) * TT_TOP_PER_PAGE; j++)
	{
		const TTRecord &o = g_db[v[j]];
		char nm[21];
		strncpy(nm, o.name, sizeof(nm) - 1);
		nm[sizeof(nm) - 1] = 0;
		t.add("%s%d. %s  %.1f  \\d%d/%d\\w\n", v[j] == me ? "\\y" : "", j + 1, nm, o.rating,
			o.life.kills, o.life.deaths);
	}
	char rank[48];
	TT_RankShort(me, rank, sizeof(rank));
	t.add("\\dYou: %s, rating %.1f\\w\n", rank, TT_StatsRating(to));
	TT_MenuSend(to, t, m.page, pages);
}

static void TT_MenuRender(edict_t *to)
{
	int idx = ENTINDEX(to);
	TTMenu &m = g_menu[idx];
	if (m.kind == TT_MENU_STATS)
		TT_StatsPage(to, m);
	else if (m.kind == TT_MENU_TOP)
		TT_TopPage(to, m);
	if (m.kind)
		m.until = g_st.menuTime > 0 ? gpGlobals->time + (g_st.menuTime > 120 ? 120 : g_st.menuTime) + 0.5f : 0;
}

static void TT_MenuOpen(edict_t *to, int kind, int slot, int rec, int page)
{
	int idx = ENTINDEX(to);
	if (idx < 1 || idx > TT_MAX_PLAYERS || TT_IsBot(to))
		return;
	TTMenu &m = g_menu[idx];
	m.kind = kind;
	m.slot = slot;
	m.rec = rec;
	m.page = page;
	TT_MenuRender(to);
}

bool TT_StatsMenuSelect(edict_t *p, int key)
{
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return false;
	TTMenu &m = g_menu[idx];
	if (!m.kind)
		return false;
	if (m.until > 0 && gpGlobals->time > m.until)
	{
		m.kind = TT_MENU_NONE; // timed out on their screen - not ours any more
		return false;
	}
	if (key == 8)
	{
		m.page--;
		TT_MenuRender(p);
	}
	else if (key == 9)
	{
		m.page++;
		TT_MenuRender(p);
	}
	else
		m.kind = TT_MENU_NONE; // 0 (sent as 10) closes
	return true;
}

void TT_StatsMenuGone(edict_t *p)
{
	if (!p)
	{
		memset(g_menu, 0, sizeof(g_menu));
		return;
	}
	int idx = ENTINDEX(p);
	if (idx >= 1 && idx <= TT_MAX_PLAYERS)
		g_menu[idx].kind = TT_MENU_NONE;
}

// ---------------------------------------------------------------------------
// The MOTD-window and chat versions (stats_window 1 and 0).
// The window (TFC's MOTD panel): the overview, or with cls 1-9 one class.
// "!stats soldier" / "!stats <name> soldier" open a class's window; the
// overview lists the classes played so you know which to ask for.
static void TT_ShowStatsWindow(edict_t *to, edict_t *who, int cls)
{
	int idx = ENTINDEX(who);
	const TTBlock &mb = g_ss[idx].map;
	TTBlock life;
	TT_Lifetime(idx, life);
	char rank[96], tm[24], tl[24];
	TT_RankText(idx, rank, sizeof(rank));

	if (!g_st.window)
	{
		TT_Say(to, "%s %s this map: %d kills, %d deaths, %d caps, %d carrier kills, acc %d%%, %d headshots",
			TT_TAG, STRING(who->v.netname), mb.kills, mb.deaths, mb.caps, mb.carrierKills,
			TT_Acc(mb.hits, mb.shots), mb.headshots);
		TT_Say(to, "%s lifetime: %d kills, %d deaths, %d caps, rating %.1f, %s",
			TT_TAG, life.kills, life.deaths, life.caps, TT_StatsRating(who), rank);
		return;
	}

	// The numbers shown: everything, or one class.
	const TTCounts &m = (cls >= 1 && cls < TT_CLASSES) ? (const TTCounts &)mb.cls[cls] : (const TTCounts &)mb;
	const TTCounts &l = (cls >= 1 && cls < TT_CLASSES) ? (const TTCounts &)life.cls[cls] : (const TTCounts &)life;
	TT_TimeText(m.seconds, tm, sizeof(tm));
	TT_TimeText(l.seconds, tl, sizeof(tl));

	TTText t;
	if (cls >= 1 && cls < TT_CLASSES)
	{
		t.add("%s  -  %s\n\n", STRING(who->v.netname), g_clsTitle[cls]);
		t.add("Points per 10 minutes as %s:  %.1f  (overall rating %.1f)\n\n", g_clsTitle[cls], TT_Perf(l),
			TT_StatsRating(who));
	}
	else
	{
		t.add("PLAYER STATS - %s\n\n", STRING(who->v.netname));
		t.add("Rating: %.1f points per 10 minutes\n", TT_StatsRating(who));
		t.add("Rank: %s\n\n", rank);
	}
	t.add("                      THIS MAP  /  LIFETIME\n");
	t.add("Time played:  %s  /  %s\n", tm, tl);
	t.add("Kills:  %d  /  %d\n", m.kills, l.kills);
	t.add("Deaths:  %d  /  %d\n", m.deaths, l.deaths);
	t.add("Kills per death:  %.2f  /  %.2f\n", TT_KD(m.kills, m.deaths), TT_KD(l.kills, l.deaths));
	t.add("Best kill streak:  %d  /  %d\n", m.bestStreak, l.bestStreak);
	if (cls < 1 || TT_MeleeKills(l))
		t.add("Melee kills:  %d  /  %d\n", TT_MeleeKills(m), TT_MeleeKills(l));
	if (l.heals || l.cures)
	{
		t.add("Health given to team-mates:  %.0f  /  %.0f\n", m.healed, l.healed);
		t.add("Heals / cures:  %d / %d  -  %d / %d\n", m.heals, m.cures, l.heals, l.cures);
	}
	if (cls < 1 || l.caps || l.pickups || l.carrierKills)
	{
		t.add("Flag captures:  %d  /  %d\n", m.caps, l.caps);
		t.add("Flag pickups:  %d  /  %d\n", m.pickups, l.pickups);
		t.add("Flag carriers killed:  %d  /  %d\n", m.carrierKills, l.carrierKills);
	}
	t.add("Accuracy:  %d%%  /  %d%%   (%d / %d shots hit)\n", TT_Acc(m.hits, m.shots),
		TT_Acc(l.hits, l.shots), l.hits, l.shots);
	if (cls < 1 || l.headshots)
		t.add("Headshots:  %d  /  %d\n", m.headshots, l.headshots);
	t.add("Damage dealt:  %.0f  /  %.0f\n", m.damage, l.damage);
	if (l.builds || l.buildKills)
	{
		t.add("Sentries / dispensers built:  %d  /  %d\n", m.builds, l.builds);
		t.add("Enemy buildings destroyed:  %d  /  %d\n", m.buildKills, l.buildKills);
	}
	if (cls < 1)
		t.add("Suicides / teamkills:  %d / %d  -  %d / %d\n", m.suicides, m.teamkills, l.suicides, l.teamkills);

	int order[W_COUNT];
	int n = TT_WeaponOrder(l, order);
	int maxW = cls >= 1 ? 10 : 6;
	if (n)
	{
		t.add("\nWEAPONS (lifetime)\n");
		for (int j = 0; j < n && j < maxW; j++)
		{
			const TTWeaponStat &s = l.w[order[j]];
			if (s.shots)
				t.add("%s:  %d kills, %d%% accuracy, %.0f damage\n", TT_WeaponTitle(order[j]), s.kills,
					TT_Acc(s.hits, s.shots), s.damage);
			else
				t.add("%s:  %d kills\n", TT_WeaponTitle(order[j]), s.kills);
		}
	}

	if (cls < 1)
	{
		int classes[TT_CLASSES];
		int nc = TT_ClassOrder(life, classes);
		if (nc)
		{
			t.add("\nCLASSES (lifetime)  -  say !stats <class> for one class\n");
			for (int j = 0; j < nc; j++)
			{
				const TTCounts &c = life.cls[classes[j]];
				TT_TimeText(c.seconds, tl, sizeof(tl));
				t.add("%s:  %s, %d kills, %.1f points per 10 min\n", g_clsTitle[classes[j]], tl,
					c.kills, TT_Perf(c));
			}
		}
	}
	else
		t.add("\nsay !stats for the overview, !stats <class> for another class\n");
	TT_ShowWindow(to, t.buf);
}

static void TT_ShowWeapons(edict_t *to, edict_t *who)
{
	int idx = ENTINDEX(who);
	TTBlock life;
	TT_Lifetime(idx, life);
	char line[160];
	_snprintf_wc(line, sizeof(line) - 1, "---- %s: weapons this map (lifetime) ----\n", STRING(who->v.netname));
	line[sizeof(line) - 1] = 0;
	CLIENT_PRINTF(to, print_console, line);
	CLIENT_PRINTF(to, print_console, "weapon            kills  deaths  shots   hits  acc  hs    damage\n");
	const TTBlock &m = g_ss[idx].map;
	for (int w = 0; w < W_COUNT; w++)
	{
		const TTWeaponStat &a = m.w[w], &b = life.w[w];
		if (!(b.kills || b.deaths || b.shots || b.hits))
			continue;
		_snprintf_wc(line, sizeof(line) - 1, "%-16s %3d(%4d) %3d(%4d) %4d(%5d) %4d(%5d) %3d%% %2d(%3d) %5.0f\n",
			g_wName[w], a.kills, b.kills, a.deaths, b.deaths, a.shots, b.shots, a.hits, b.hits,
			TT_Acc(b.hits, b.shots), a.headshots, b.headshots, b.damage);
		line[sizeof(line) - 1] = 0;
		CLIENT_PRINTF(to, print_console, line);
	}
	TT_Say(to, "%s Weapon breakdown for %s printed to your console.", TT_TAG, STRING(who->v.netname));
}

static void TT_ShowTopWindow(edict_t *to, bool rankOnly)
{
	std::vector<int> v;
	TT_RankedList(v);
	int me = g_ss[ENTINDEX(to)].rec;
	char mine[96];
	TT_RankText(ENTINDEX(to), mine, sizeof(mine));

	if (!g_st.window || rankOnly)
	{
		if (rankOnly)
			TT_Say(to, "%s Your rank: %s (rating %.1f).", TT_TAG, mine, TT_StatsRating(to));
		for (size_t j = 0; !rankOnly && j < v.size() && j < 3; j++)
			TT_Say(to, "%s #%d %s (%.1f)", TT_TAG, (int)j + 1, g_db[v[j]].name, g_db[v[j]].rating);
		return;
	}

	TTText t;
	t.add("RANKINGS\n");
	t.add("Rating = points per 10 minutes on a team: kill %g, flag carrier kill +%g, capture %g, pickup %g\n\n",
		g_st.wKill, g_st.wCarrierKill, g_st.wCap, g_st.wPickup);
	t.add("You: %s  -  rating %.1f\n\n", mine, TT_StatsRating(to));
	if (v.empty())
		t.add("Nobody is ranked yet - it takes %.0f minutes on a team and a finished map.\n", g_st.minRankMinutes);
	int shown = 0;
	for (size_t j = 0; j < v.size() && shown < 15; j++, shown++)
	{
		const TTRecord &o = g_db[v[j]];
		t.add("%s%d. %s  -  %.1f   (K %d / D %d, caps %d, carrier kills %d, acc %d%%)\n",
			v[j] == me ? ">> " : "", (int)j + 1, o.name, o.rating, o.life.kills, o.life.deaths,
			o.life.caps, o.life.carrierKills, TT_Acc(o.life.hits, o.life.shots));
	}
	for (size_t j = 15; j < v.size(); j++)
		if (v[j] == me)
		{
			const TTRecord &o = g_db[me];
			t.add("...\n>> %d. %s  -  %.1f   (K %d / D %d, caps %d)\n", (int)j + 1, o.name, o.rating,
				o.life.kills, o.life.deaths, o.life.caps);
			break;
		}
	TT_ShowWindow(to, t.buf);
}

// The last map's awards, for !awards (and the window at intermission).
static char g_awards[1536];
static char g_awardsShort[1536];   // without the best of each class, for when the full one won't fit

// !rival: the player who has killed you most, the one you have killed most,
// and your most even opponent (lifetime, this map included).
static void TT_RivalText(edict_t *to, edict_t *who)
{
	int idx = ENTINDEX(who);
	int rec = (idx >= 1 && idx <= TT_MAX_PLAYERS) ? g_ss[idx].rec : -1;
	const char *name = STRING(who->v.netname);
	if (!g_st.rivals)
	{
		TT_Say(to, "%s Head-to-head tracking is off on this server.", TT_TAG);
		return;
	}
	if (rec < 0)
	{
		TT_Say(to, "%s No stats for %s yet.", TT_TAG, name);
		return;
	}
	int nem = -1, nemThey = 0, nemMe = 0;
	int prey = -1, preyMe = 0, preyThey = 0;
	int even = -1, evenMe = 0, evenThey = 0;
	float evenScore = 1e9f;
	for (size_t i = 0; i < g_vs.size(); i++)
	{
		const TTVs &v = g_vs[i];
		if (v.a != rec && v.b != rec)
			continue;
		bool amA = v.a == rec;
		int other = amA ? v.b : v.a;
		int me = amA ? v.ab : v.ba, they = amA ? v.ba : v.ab;
		if (they > 0 && (they > nemThey || (they == nemThey && me < nemMe)))
		{ nem = other; nemThey = they; nemMe = me; }
		if (me > 0 && (me > preyMe || (me == preyMe && they < preyThey)))
		{ prey = other; preyMe = me; preyThey = they; }
		int total = me + they;
		if (total >= 6)
		{
			// Closest to even, then most played.
			float sc = (float)abs(me - they) / (float)total - total * 0.0001f;
			if (sc < evenScore) { evenScore = sc; even = other; evenMe = me; evenThey = they; }
		}
	}
	if (nem < 0 && prey < 0)
	{
		TT_Say(to, "%s No head-to-head for %s yet.", TT_TAG, name);
		return;
	}
	bool self = who == to;
	char whose[48], you[40], them[8];
	_snprintf_wc(whose, sizeof(whose) - 1, self ? "Your" : "%s's", name);
	whose[sizeof(whose) - 1] = 0;
	_snprintf_wc(you, sizeof(you) - 1, "%s", self ? "you" : name);
	you[sizeof(you) - 1] = 0;
	strcpy(them, "them");
	if (nem >= 0)
		TT_Say(to, "%s %s nemesis: %s - killed %s %d time%s, %s got %s %d", TT_TAG, whose, g_db[nem].name,
			you, nemThey, nemThey == 1 ? "" : "s", you, them, nemMe);
	if (prey >= 0 && prey != nem)
		TT_Say(to, "%s %s favourite target: %s - %s killed %s %d time%s, %s got %s %d", TT_TAG, whose,
			g_db[prey].name, you, them, preyMe, preyMe == 1 ? "" : "s", them, self ? "you" : name, preyThey);
	if (even >= 0 && even != nem && even != prey)
	{
		if (evenMe == evenThey)
			TT_Say(to, "%s %s even match: %s - %d kills each way", TT_TAG, whose, g_db[even].name, evenMe);
		else
			TT_Say(to, "%s %s closest match: %s - %s %d, %s %d", TT_TAG, whose, g_db[even].name,
				self ? "you" : name, evenMe, them, evenThey);
	}
}

bool TT_StatsChat(edict_t *p, const char *word, const char *rest)
{
	if (!g_st.enabled)
		return false;
	// "!" only: AMX Mod X's stats.amxx already answers /rank, /stats and /top15,
	// and both replying to the same line would be noise.
	if (word[0] != '!')
		return false;
	const char *w = word + 1;
	if (!strcasecmp(w, "stats") || !strcasecmp(w, "weapons"))
	{
		edict_t *who = p;
		int cls = 0;
		if (rest && rest[0])
		{
			// "!stats soldier", "!stats <name>", "!stats <name> soldier".
			char arg[96];
			strncpy(arg, rest, sizeof(arg) - 1);
			arg[sizeof(arg) - 1] = 0;
			size_t al = strlen(arg);
			while (al && arg[al - 1] == ' ')
				arg[--al] = 0;
			char *last = strrchr(arg, ' ');
			int c = TT_ClassByKey(last ? last + 1 : arg);
			if (c >= 1 && !strcasecmp(w, "stats"))
			{
				cls = c;
				if (last)
					*last = 0;
				else
					arg[0] = 0;
			}
			if (arg[0])
			{
				who = TT_FindByName(arg);
				if (!who)
				{
					TT_Say(p, "%s No single player matches \"%s\".", TT_TAG, arg);
					return true;
				}
			}
		}
		int widx = ENTINDEX(who);
		if (cls)
		{
			TTBlock life;
			TT_Lifetime(widx, life);
			if (!CountsUsed(life.cls[cls]))
			{
				TT_Say(p, "%s %s has no %s stats yet.", TT_TAG, STRING(who->v.netname), g_clsTitle[cls]);
				return true;
			}
		}
		if (!strcasecmp(w, "weapons"))
			TT_ShowWeapons(p, who);
		else if (g_st.window == 2 && !TT_IsBot(p))
		{
			if (g_ss[widx].rec < 0)
			{
				TT_Say(p, "%s No stats for %s yet.", TT_TAG, STRING(who->v.netname));
				return true;
			}
			int page = 0;
			if (cls)
			{
				TTBlock life;
				TT_Lifetime(widx, life);
				int classes[TT_CLASSES];
				int nc = TT_ClassOrder(life, classes);
				for (int j = 0; j < nc; j++)
					if (classes[j] == cls)
						page = j + 1;
			}
			TT_MenuOpen(p, TT_MENU_STATS, widx, g_ss[widx].rec, page);
		}
		else
			TT_ShowStatsWindow(p, who, cls);
		return true;
	}
	if (!strcasecmp(w, "rank"))
	{
		TT_ShowTopWindow(p, true);
		return true;
	}
	if (!strcasecmp(w, "top10") || !strcasecmp(w, "top") || !strcasecmp(w, "rankings"))
	{
		if (g_st.window == 2)
			TT_MenuOpen(p, TT_MENU_TOP, 0, -1, 0);
		else
			TT_ShowTopWindow(p, false);
		return true;
	}
	if (!strcasecmp(w, "rival") || !strcasecmp(w, "rivals") || !strcasecmp(w, "nemesis"))
	{
		edict_t *who = p;
		if (rest && rest[0])
		{
			who = TT_FindByName(rest);
			if (!who)
			{
				TT_Say(p, "%s No single player matches \"%s\".", TT_TAG, rest);
				return true;
			}
		}
		TT_RivalText(p, who);
		return true;
	}
	if (!strcasecmp(w, "awards"))
	{
		if (g_awards[0])
			TT_ShowWindow(p, g_awards);
		else
			TT_Say(p, "%s No map has finished since the server started.", TT_TAG);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// END OF MAP. At intermission everyone gets a window (the MOTD panel, which
// the client opens even during intermission - TeamFortressViewport::ShowVGUIMenu,
// client.so 0x8a370, lets menu 5 through when it refuses the others): the
// map's awards, the best player of each class, and their own map. Plus a line
// across the top of the screen and the awards in chat.
// The pair with the most kills both ways this map (each at least 2 on the other).
static bool TT_MapRivalry(int *ra, int *rb, int *ab, int *ba)
{
	int best = -1, bestTotal = 0;
	for (size_t i = 0; i < g_vs.size(); i++)
	{
		const TTVs &v = g_vs[i];
		if (v.mab < 2 || v.mba < 2)
			continue;
		int t = v.mab + v.mba;
		if (t > bestTotal) { bestTotal = t; best = (int)i; }
	}
	if (best < 0)
		return false;
	const TTVs &v = g_vs[(size_t)best];
	// The one with more kills first.
	if (v.mab >= v.mba) { *ra = v.a; *rb = v.b; *ab = v.mab; *ba = v.mba; }
	else                { *ra = v.b; *rb = v.a; *ab = v.mba; *ba = v.mab; }
	return true;
}

struct TTFeedSort { int idx; float pts; };
static int TT_FeedSortCmp(const void *a, const void *b)
{
	const TTFeedSort *x = (const TTFeedSort *)a, *y = (const TTFeedSort *)b;
	if (x->pts != y->pts)
		return x->pts > y->pts ? -1 : 1;
	return x->idx - y->idx;
}

static void TT_Summary(void)
{
	enum { A_MVP, A_KILLS, A_STREAK, A_CAPS, A_CARRIER, A_ACC, A_HS, A_DMG, A_MELEE, A_HEAL, A_BUILD, A_N };
	int best[A_N];
	float val[A_N];
	for (int k = 0; k < A_N; k++)
	{
		best[k] = 0;
		val[k] = 0;
	}
	val[A_MVP] = -1e9f;
	int bestCls[TT_CLASSES];
	float valCls[TT_CLASSES];
	for (int c = 0; c < TT_CLASSES; c++)
	{
		bestCls[c] = 0;
		valCls[c] = 0;
	}
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (!e || TT_IsHLTV(e))
			continue;
		const TTBlock &m = g_ss[i].map;
		if (m.seconds < 60.0f)
			continue;
		float mvp = TT_Points(m);
		if (!best[A_MVP] || mvp > val[A_MVP]) { best[A_MVP] = i; val[A_MVP] = mvp; }
		if (m.kills > val[A_KILLS]) { best[A_KILLS] = i; val[A_KILLS] = (float)m.kills; }
		if (m.caps > val[A_CAPS]) { best[A_CAPS] = i; val[A_CAPS] = (float)m.caps; }
		if (m.carrierKills > val[A_CARRIER]) { best[A_CARRIER] = i; val[A_CARRIER] = (float)m.carrierKills; }
		if (m.shots >= 30 && TT_Acc(m.hits, m.shots) > val[A_ACC]) { best[A_ACC] = i; val[A_ACC] = (float)TT_Acc(m.hits, m.shots); }
		if (m.headshots > val[A_HS]) { best[A_HS] = i; val[A_HS] = (float)m.headshots; }
		if (m.damage > val[A_DMG]) { best[A_DMG] = i; val[A_DMG] = m.damage; }
		if (m.buildKills > val[A_BUILD]) { best[A_BUILD] = i; val[A_BUILD] = (float)m.buildKills; }
		// A streak of 1 is no streak.
		if (m.bestStreak >= 2 && m.bestStreak > val[A_STREAK]) { best[A_STREAK] = i; val[A_STREAK] = (float)m.bestStreak; }
		int melee = TT_MeleeKills(m);
		if (melee > val[A_MELEE]) { best[A_MELEE] = i; val[A_MELEE] = (float)melee; }
		if (m.healed >= 1.0f && m.healed > val[A_HEAL]) { best[A_HEAL] = i; val[A_HEAL] = m.healed; }
		for (int c = 1; c < TT_CLASSES; c++)
		{
			const TTCounts &k = m.cls[c];
			float pts = TT_Points(k);
			if (k.seconds >= 60.0f && pts >= 1.0f && pts > valCls[c]) { bestCls[c] = i; valCls[c] = pts; }
		}
	}
	static const char *title[A_N] = { "MVP", "Most kills", "Longest kill streak", "Most caps",
		"Most flag carrier kills", "Best accuracy", "Most headshots", "Most damage", "Most melee kills",
		"Most healing", "Most enemy buildings destroyed" };
	static const char *unit[A_N] = { " points", "", " in a row", "", "", "%", "", "", "", " health", "" };
	int rvA = -1, rvB = -1, rvAB = 0, rvBA = 0;
	bool rivalry = g_st.rivals && TT_MapRivalry(&rvA, &rvB, &rvAB, &rvBA);

	// Discord (tt_feed.cpp) - whatever stats_summary is set to.
	{
		static TTFeedMapEnd fm;
		memset(&fm, 0, sizeof(fm));
		strncpy(fm.map, STRING(gpGlobals->mapname), sizeof(fm.map) - 1);
		for (int t = 1; t <= TT_MAX_TEAMS; t++)
			fm.teamScore[t] = (int)g_teamScore[t];
		for (int k = 0; k < A_N && fm.awardCount < 16; k++)
			if (best[k])
			{
				TTFeedAward &a = fm.awards[fm.awardCount++];
				strncpy(a.title, title[k], sizeof(a.title) - 1);
				strncpy(a.name, STRING(INDEXENT(best[k])->v.netname), sizeof(a.name) - 1);
				a.value = val[k];
				strncpy(a.unit, unit[k], sizeof(a.unit) - 1);
			}
		if (rivalry)
		{
			strncpy(fm.rivalA, g_db[rvA].name, sizeof(fm.rivalA) - 1);
			strncpy(fm.rivalB, g_db[rvB].name, sizeof(fm.rivalB) - 1);
			fm.rivalAB = rvAB;
			fm.rivalBA = rvBA;
		}
		TTFeedSort order[TT_MAX_PLAYERS];
		int no = 0;
		for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
		{
			edict_t *e = TT_Player(i);
			if (!e || TT_IsHLTV(e) || g_ss[i].map.seconds < 60.0f)
				continue;
			order[no].idx = i;
			order[no].pts = TT_Points(g_ss[i].map);
			no++;
		}
		qsort(order, (size_t)no, sizeof(order[0]), TT_FeedSortCmp);
		for (int j = 0; j < no && fm.playerCount < 32; j++)
		{
			int i = order[j].idx;
			edict_t *e = INDEXENT(i);
			TTFeedPlayer &fp = fm.players[fm.playerCount++];
			strncpy(fp.name, STRING(e->v.netname), sizeof(fp.name) - 1);
			if (g_ss[i].rec >= 0)
				strncpy(fp.key, g_db[g_ss[i].rec].key, sizeof(fp.key) - 1);
			fp.bot = TT_IsBot(e);
			fp.team = TT_PlayerTeam(e);
			fp.kills = g_ss[i].map.kills;
			fp.deaths = g_ss[i].map.deaths;
			fp.caps = g_ss[i].map.caps;
			fp.points = order[j].pts;
		}
		TT_FeedMapEnd(fm);
	}

	if (!g_st.summary || !best[A_MVP])
		return;
	#define TT_AWNAME(k) STRING(INDEXENT(best[k])->v.netname)

	// Chat and the top of the screen.
	TT_SayAll("%s ---- Map awards ----", TT_TAG);
	for (int k = 0; k < A_N; k++)
		if (best[k])
			TT_SayAll("%s %s: %s (%.0f%s)", TT_TAG, title[k], TT_AWNAME(k), val[k], unit[k]);
	char banner[160];
	_snprintf_wc(banner, sizeof(banner) - 1, "MAP AWARDS\nMVP: %s  (%.0f points)", TT_AWNAME(A_MVP), val[A_MVP]);
	banner[sizeof(banner) - 1] = 0;
	TT_HudText(NULL, 3, -1.0f, 0.02f, 255, 200, 40, 15.0f, banner);

	// The shared part of the window.
	// TFC's window takes about 1500 characters, so with long names, every award
	// and every class played, the per-person window drops the class list.
	TTText head, cls, riv;
	head.add("MAP AWARDS  -  %s\n\n", STRING(gpGlobals->mapname));
	for (int k = 0; k < A_N; k++)
		if (best[k])
			head.add("%s:  %s  (%.0f%s)\n", title[k], TT_AWNAME(k), val[k], unit[k]);
	for (int c = 1; c < TT_CLASSES; c++)
		if (bestCls[c])
		{
			if (!cls.len)
				cls.add("\nBEST OF EACH CLASS\n");
			cls.add("%s:  %s  (%.0f points)\n", g_clsTitle[c], STRING(INDEXENT(bestCls[c])->v.netname), valCls[c]);
		}
	#undef TT_AWNAME
	if (rivalry)
	{
		riv.add("\nRIVALRY OF THE MAP\n%s  %d - %d  %s\n", g_db[rvA].name, rvAB, rvBA, g_db[rvB].name);
		TT_SayAll("%s Rivalry of the map: %s %d - %d %s", TT_TAG, g_db[rvA].name, rvAB, rvBA, g_db[rvB].name);
	}
	_snprintf_wc(g_awards, sizeof(g_awards) - 1, "%s%s%s", head.buf, cls.buf, riv.buf);
	g_awards[sizeof(g_awards) - 1] = 0;
	_snprintf_wc(g_awardsShort, sizeof(g_awardsShort) - 1, "%s%s%s", head.buf, riv.buf,
		cls.len ? "(say !awards for the best of each class)\n" : "");
	g_awardsShort[sizeof(g_awardsShort) - 1] = 0;
	if (g_st.summary < 2)
		return;

	// Each person's window: the awards and their own map.
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (!e || TT_IsBot(e) || TT_IsHLTV(e))
			continue;
		TTText w;
		const TTSession &s = g_ss[i];
		const TTBlock &m = s.map;
		w.add("\nYOUR MAP\n");
		w.add("Kills %d   Deaths %d   Caps %d   Carrier kills %d\n", m.kills, m.deaths, m.caps, m.carrierKills);
		w.add("Accuracy %d%%   Damage %.0f", TT_Acc(m.hits, m.shots), m.damage);
		if (m.headshots)
			w.add("   Headshots %d", m.headshots);
		if (m.builds || m.buildKills)
			w.add("   Built %d   Enemy buildings %d", m.builds, m.buildKills);
		w.add("\nBest streak %d   Melee kills %d", m.bestStreak, TT_MeleeKills(m));
		if (m.heals || m.cures)
			w.add("   Healed %.0f health (%d cures)", m.healed, m.cures);
		w.add("\n");
		char tm[24];
		TT_TimeText(m.seconds, tm, sizeof(tm));
		w.add("%.0f points in %s = %.1f per 10 minutes\n", TT_Points(m), tm, TT_Perf(m));
		int classes[TT_CLASSES];
		int nc = 0;
		for (int c = 1; c < TT_CLASSES; c++)
			if (m.cls[c].seconds >= 30.0f)
				classes[nc++] = c;
		for (int j = 0; j < nc; j++)
		{
			const TTCounts &k = m.cls[classes[j]];
			TT_TimeText(k.seconds, tm, sizeof(tm));
			w.add("%s  %s: %d kills, %d deaths, %.0f points\n", g_clsTitle[classes[j]], tm, k.kills, k.deaths, TT_Points(k));
		}
		if (s.rec >= 0 && !s.ratedThisMap && m.seconds / 60.0f >= g_st.minMapMinutes)
		{
			float old = g_db[s.rec].rating;
			float perf = TT_Perf(m);
			float nw = old < 0 ? perf : old * 0.7f + perf * 0.3f;
			if (old < 0)
				w.add("Rating: %.1f (your first rated map)\n", nw);
			else
				w.add("Rating: %.1f -> %.1f\n", old, nw);
		}
		else
			w.add("Rating: unchanged (%.0f minutes on a team make a map count)\n", g_st.minMapMinutes);
		w.add("\nsay !stats for your stats page by page, !top10 for the rankings, !awards to see this again");
		TTText all;
		all.add("%s", strlen(g_awards) + w.len <= 1490 ? g_awards : g_awardsShort);
		all.add("%s", w.buf);
		TT_ShowWindow(e, all.buf);
	}
}

// ---------------------------------------------------------------------------
static void TT_Cmd_StatsSave(void)
{
	for (int i = 1; i <= TT_MAX_PLAYERS; i++)
		TT_Flush(i);
	TT_StatsSave("tt_stats_save");
	SERVER_PRINT("[Teams] stats saved.\n");
}

void TT_StatsRegisterCommands(void)
{
	REG_SVR_COMMAND("tt_stats_save", TT_Cmd_StatsSave);
}

// ---------------------------------------------------------------------------
// For the name tracker and the feed.
void TT_StatsKeyOf(edict_t *p, char *out, size_t len)
{
	TT_KeyFor(p, out, len);
}

void TT_StatsMapNumbers(int idx, int *kills, int *deaths, int *caps, float *points)
{
	*kills = *deaths = *caps = 0;
	*points = 0;
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return;
	const TTBlock &m = g_ss[idx].map;
	*kills = m.kills;
	*deaths = m.deaths;
	*caps = m.caps;
	*points = TT_Points(m);
}

int TT_StatsTeamScoreOf(int team)
{
	return (team >= 1 && team <= TT_MAX_TEAMS) ? (int)g_teamScore[team] : 0;
}
