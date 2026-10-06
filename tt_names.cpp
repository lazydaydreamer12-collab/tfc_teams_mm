// tt_names.cpp - the name tracker, and who joins and leaves.
//
// Every name a SteamID has played under is kept in tt_names.txt, with when it
// was first and last seen and how often. Admins look them up with
// !names <player> (in game) or tt_names <player|SteamID> (server console).
// Bots and LAN players (no SteamID) are not tracked - their "ID" is their name.
//
// The same file keeps how each player gets onto a team: picking one in the
// team menu ("jointeam 1"-"4") or Auto Assign ("jointeam 5").
//
// Joining and leaving are worked out here too (visits, for the name list).
// A map change is not a leave and a join: the engine puts everyone back in
// the server for the new map, so a player who comes back within a short
// while of the map starting is the same visit carrying on.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <vector>

#include "tt_common.h"
#include "tt_stats.h"
#include "tt_net.h"

struct TTNameRec
{
	char key[48];
	char name[32];
	long first, last;
	int  times;
};
struct TTJoinRec
{
	char key[48];
	int  picks, autos;
};
static std::vector<TTNameRec> g_names;
static std::vector<TTJoinRec> g_joins;
static bool  g_namesDirty = false;
static long  g_nextNamesSave = 0;

// One per player slot. Kept across map changes (see above).
struct TTNameSlot
{
	char  key[48];        // "" = nobody known in this slot
	char  name[32];
	long  since;          // unix time they joined (this visit)
	bool  carried;        // from the previous map, not seen back yet
	bool  bot;
};
static TTNameSlot g_ns[TT_MAX_PLAYERS + 1];

typedef void (*TTText_Out)(void *ctx, const char *fmt, ...);
static long  g_mapStartedAt = 0;     // unix time of this map's start
static bool  g_mapChanging = false;  // between ServerDeactivate and the next map
static float g_nextScan = 0;

#define TT_CARRY_GRACE 120   // seconds into a map to come back before it counts as leaving
#define TT_MAX_NAMES_PER_ID 30

static bool TT_NamesPath(char *out, size_t len)
{
	char dir[400];
	if (!TT_ModuleDir(dir, sizeof(dir)))
		return false;
	_snprintf_wc(out, len - 1, "%s/tt_names.txt", dir);
	out[len - 1] = 0;
	return true;
}

static void TT_CleanName(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\r' || *s == '\n')
			*s = ' ';
}

static bool TT_Trackable(const char *key)
{
	return key && !strncmp(key, "STEAM_", 6);
}

static void TT_NamesLoad(void)
{
	g_names.clear();
	g_joins.clear();
	char path[450];
	if (!TT_NamesPath(path, sizeof(path)))
		return;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[300];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#')
			continue;
		char *fld[8];
		int n = 0;
		char *p = line;
		while (n < 8)
		{
			fld[n++] = p;
			char *t = strchr(p, '\t');
			if (!t)
				break;
			*t = 0;
			p = t + 1;
		}
		if (n >= 6 && !strcmp(fld[0], "N"))
		{
			TTNameRec r;
			memset(&r, 0, sizeof(r));
			strncpy(r.key, fld[1], sizeof(r.key) - 1);
			r.first = atol(fld[2]);
			r.last = atol(fld[3]);
			r.times = atoi(fld[4]);
			strncpy(r.name, fld[5], sizeof(r.name) - 1);
			if (r.key[0] && r.name[0])
				g_names.push_back(r);
		}
		else if (n >= 4 && !strcmp(fld[0], "J"))
		{
			TTJoinRec j;
			memset(&j, 0, sizeof(j));
			strncpy(j.key, fld[1], sizeof(j.key) - 1);
			j.picks = atoi(fld[2]);
			j.autos = atoi(fld[3]);
			if (j.key[0])
				g_joins.push_back(j);
		}
	}
	fclose(f);
	TT_Trace("Names: loaded %d names, %d team-choice records", (int)g_names.size(), (int)g_joins.size());
}

static void TT_NamesSave(void)
{
	char path[450], tmp[470];
	if (!TT_NamesPath(path, sizeof(path)))
		return;
	_snprintf_wc(tmp, sizeof(tmp) - 1, "%s.tmp", path);
	tmp[sizeof(tmp) - 1] = 0;
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "# TFC Teams name history v1\n");
	fprintf(f, "# N <SteamID> <first seen> <last seen> <times> <name>      (unix times)\n");
	fprintf(f, "# J <SteamID> <picked a team> <used auto-assign>\n");
	for (size_t i = 0; i < g_names.size(); i++)
	{
		TTNameRec &r = g_names[i];
		TT_CleanName(r.name);
		fprintf(f, "N\t%s\t%ld\t%ld\t%d\t%s\n", r.key, r.first, r.last, r.times, r.name);
	}
	for (size_t i = 0; i < g_joins.size(); i++)
		fprintf(f, "J\t%s\t%d\t%d\n", g_joins[i].key, g_joins[i].picks, g_joins[i].autos);
	fclose(f);
	remove(path);
	if (rename(tmp, path) == 0)
		g_namesDirty = false;
}

// Remember a name for a SteamID. Returns true if it was the first time this
// ID was ever seen.
static bool TT_NoteName(const char *key, const char *name, bool newVisit)
{
	if (!g_tt.namesTrack || !TT_Trackable(key) || !name || !name[0])
		return false;
	long now = (long)time(NULL);
	bool known = false;
	TTNameRec *hit = NULL;
	int count = 0;
	for (size_t i = 0; i < g_names.size(); i++)
	{
		if (strcmp(g_names[i].key, key))
			continue;
		known = true;
		count++;
		if (!strcmp(g_names[i].name, name))
			hit = &g_names[i];
	}
	if (hit)
	{
		hit->last = now;
		if (newVisit)
			hit->times++;
	}
	else
	{
		if (count >= TT_MAX_NAMES_PER_ID)
		{
			// Drop this ID's least recently used name.
			size_t oldest = (size_t)-1;
			for (size_t i = 0; i < g_names.size(); i++)
				if (!strcmp(g_names[i].key, key) && (oldest == (size_t)-1 || g_names[i].last < g_names[oldest].last))
					oldest = i;
			if (oldest != (size_t)-1)
				g_names.erase(g_names.begin() + (long)oldest);
		}
		TTNameRec r;
		memset(&r, 0, sizeof(r));
		strncpy(r.key, key, sizeof(r.key) - 1);
		strncpy(r.name, name, sizeof(r.name) - 1);
		TT_CleanName(r.name);
		r.first = r.last = now;
		r.times = 1;
		g_names.push_back(r);
	}
	g_namesDirty = true;
	return !known;
}

static int TT_NameCompareRecent(const void *a, const void *b)
{
	const TTNameRec *x = *(const TTNameRec *const *)a, *y = *(const TTNameRec *const *)b;
	return (x->last < y->last) - (x->last > y->last);
}

// "a, b, c": the other names this ID has used, most recent first.
int TT_NamesOther(const char *key, const char *current, char *out, size_t len, int max)
{
	out[0] = 0;
	const TTNameRec *list[TT_MAX_NAMES_PER_ID + 4];
	int n = 0;
	for (size_t i = 0; i < g_names.size() && n < TT_MAX_NAMES_PER_ID + 4; i++)
		if (!strcmp(g_names[i].key, key) && (!current || strcmp(g_names[i].name, current)))
			list[n++] = &g_names[i];
	qsort(list, (size_t)n, sizeof(list[0]), TT_NameCompareRecent);
	size_t used = 0;
	int shown = 0;
	for (int i = 0; i < n && shown < max; i++, shown++)
	{
		int w = _snprintf_wc(out + used, len - used - 1, "%s%s", shown ? ", " : "", list[i]->name);
		if (w < 0 || (used += (size_t)w) >= len - 1)
		{
			out[len - 1] = 0;
			break;
		}
	}
	return n;
}

void TT_NamesTeamChoice(edict_t *p, bool autoAssign)
{
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return;
	g_pl[idx].joinedAuto = autoAssign;
	g_pl[idx].joinChoiceKnown = true;
	const char *key = g_ns[idx].key;
	if (!TT_Trackable(key))
		return;
	TTJoinRec *j = NULL;
	for (size_t i = 0; i < g_joins.size(); i++)
		if (!strcmp(g_joins[i].key, key)) { j = &g_joins[i]; break; }
	if (!j)
	{
		TTJoinRec r;
		memset(&r, 0, sizeof(r));
		strncpy(r.key, key, sizeof(r.key) - 1);
		g_joins.push_back(r);
		j = &g_joins.back();
	}
	if (autoAssign) j->autos++; else j->picks++;
	g_namesDirty = true;
}

static void TT_JoinCounts(const char *key, int *picks, int *autos)
{
	*picks = *autos = 0;
	for (size_t i = 0; i < g_joins.size(); i++)
		if (!strcmp(g_joins[i].key, key)) { *picks = g_joins[i].picks; *autos = g_joins[i].autos; return; }
}

// ---------------------------------------------------------------------------
void TT_NamesInit(void)
{
	memset(g_ns, 0, sizeof(g_ns));
	TT_NamesLoad();
	g_nextNamesSave = (long)time(NULL) + 300;
}

void TT_NamesShutdown(void)
{
	if (g_namesDirty)
		TT_NamesSave();
}

void TT_NamesMapStart(void)
{
	g_mapStartedAt = (long)time(NULL);
	g_mapChanging = false;
	g_nextScan = 0;
	// Everyone known from the last map is "carried" until they show up again.
	for (int i = 1; i <= TT_MAX_PLAYERS; i++)
		if (g_ns[i].key[0])
			g_ns[i].carried = true;
}

void TT_NamesMapEnd(void)
{
	g_mapChanging = true;
	if (g_namesDirty)
		TT_NamesSave();
}

static void TT_Left(int i, edict_t *e)
{
	TTNameSlot &s = g_ns[i];
	if (!s.key[0])
		return;
	TT_Trace("Names: %s (%s) left", s.name, s.key);
	memset(&s, 0, sizeof(s));
}

void TT_NamesDisconnect(edict_t *p)
{
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > TT_MAX_PLAYERS)
		return;
	if (g_mapChanging)
		return; // a map change: they come back on the next map (or are counted as gone then)
	if (!g_ns[idx].key[0])
		return;
	strncpy(g_ns[idx].name, STRING(p->v.netname), sizeof(g_ns[idx].name) - 1);
	TT_Left(idx, p);
}

void TT_NamesFrame(void)
{
	float now = gpGlobals->time;
	if (now < g_nextScan)
		return;
	g_nextScan = now + 0.5f;
	long wall = (long)time(NULL);

	// Slots from the last map nobody came back to.
	bool grace = wall - g_mapStartedAt < TT_CARRY_GRACE;
	for (int i = 1; i <= TT_MAX_PLAYERS; i++)
	{
		TTNameSlot &s = g_ns[i];
		if (!s.key[0])
			continue;
		edict_t *e = (i <= gpGlobals->maxClients) ? TT_Player(i) : NULL;
		if (s.carried && !grace && !e)
			TT_Left(i, NULL);
		else if (!s.carried && !e)
			TT_Left(i, NULL); // gone without a disconnect call
	}

	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (!e || TT_IsHLTV(e))
			continue;
		TTNameSlot &s = g_ns[i];
		const char *name = STRING(e->v.netname);
		char key[48];
		TT_StatsKeyOf(e, key, sizeof(key));
		if (!key[0])
			continue; // SteamID not known yet
		if (s.key[0] && strcmp(s.key, key))
		{
			// Somebody else is in this slot now.
			TT_Left(i, NULL);
		}
		if (!s.key[0] || s.carried)
		{
			bool same = s.key[0] && s.carried && !strcmp(s.key, key);
			if (!same)
			{
				// Maybe they came back into a different slot.
				for (int k = 1; k <= TT_MAX_PLAYERS && !same; k++)
					if (k != i && g_ns[k].carried && !strcmp(g_ns[k].key, key))
					{
						s = g_ns[k];
						memset(&g_ns[k], 0, sizeof(g_ns[k]));
						same = true;
					}
			}
			if (same)
			{
				s.carried = false;
				if (strcmp(s.name, name))
				{
					if (!s.bot)
						TT_NoteName(key, name, false);
					strncpy(s.name, name, sizeof(s.name) - 1);
				}
				continue;
			}
			// A new visit.
			memset(&s, 0, sizeof(s));
			strncpy(s.key, key, sizeof(s.key) - 1);
			strncpy(s.name, name, sizeof(s.name) - 1);
			s.since = wall;
			s.bot = TT_IsBot(e);
			bool first = false;
			if (!s.bot)
				first = TT_NoteName(key, name, true);
			TT_Trace("Names: %s (%s) joined%s", name, key, first ? " - first time" : "");
			continue;
		}
		if (strcmp(s.name, name))
		{
			if (!s.bot)
			{
				TT_NoteName(key, name, false);
				TT_Trace("Names: %s is now %s (%s)", s.name, name, key);
			}
			strncpy(s.name, name, sizeof(s.name) - 1);
			s.name[sizeof(s.name) - 1] = 0;
		}
	}

	if (wall >= g_nextNamesSave)
	{
		g_nextNamesSave = wall + 300;
		if (g_namesDirty)
			TT_NamesSave();
	}
}

const char *TT_NamesKeyOfSlot(int idx)
{
	return (idx >= 1 && idx <= TT_MAX_PLAYERS) ? g_ns[idx].key : "";
}

// ---------------------------------------------------------------------------
// Looking someone up: a player here (part of a name or #userid), or a SteamID,
// or - for people not here - part of any name ever used.
static void TT_DateText(long t, char *out, size_t len)
{
	time_t tt = (time_t)t;
	struct tm *tm = localtime(&tt);
	if (!tm)
	{
		_snprintf_wc(out, len - 1, "?");
		out[len - 1] = 0;
		return;
	}
	strftime(out, len, "%Y-%m-%d", tm);
}

static void TT_NamesReport(const char *arg, TTText_Out out, void *ctx)
{
	char key[48] = "";
	char label[80] = "";
	// A player in the server?
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS && !key[0]; i++)
	{
		edict_t *e = TT_Player(i);
		if (!e || TT_IsBot(e) || TT_IsHLTV(e))
			continue;
		const char *n = STRING(e->v.netname);
		bool match = false;
		if (arg[0] == '#')
			match = GETPLAYERUSERID(e) == atoi(arg + 1);
		else
		{
			// case-insensitive substring
			size_t al = strlen(arg);
			for (const char *c = n; *c && !match; c++)
				match = !strncasecmp(c, arg, al);
		}
		if (match && TT_Trackable(g_ns[i].key))
		{
			strncpy(key, g_ns[i].key, sizeof(key) - 1);
			_snprintf_wc(label, sizeof(label) - 1, "%s (here now)", n);
		}
	}
	if (!key[0] && !strncasecmp(arg, "STEAM_", 6))
	{
		// Same normalising as the stats key: the universe digit is always 0.
		const char *c = strchr(arg, ':');
		_snprintf_wc(key, sizeof(key) - 1, "STEAM_0%s", c ? c : "");
	}
	if (!key[0])
	{
		// Anyone who ever used a name containing it (most recent first).
		const TTNameRec *best = NULL;
		size_t al = strlen(arg);
		for (size_t i = 0; i < g_names.size(); i++)
		{
			bool match = false;
			for (const char *c = g_names[i].name; *c && !match; c++)
				match = !strncasecmp(c, arg, al);
			if (match && (!best || g_names[i].last > best->last))
				best = &g_names[i];
		}
		if (best)
			strncpy(key, best->key, sizeof(key) - 1);
	}
	if (!key[0])
	{
		out(ctx, "No one found for \"%s\".\n", arg);
		return;
	}
	const TTNameRec *list[TT_MAX_NAMES_PER_ID + 4];
	int n = 0;
	for (size_t i = 0; i < g_names.size() && n < TT_MAX_NAMES_PER_ID + 4; i++)
		if (!strcmp(g_names[i].key, key))
			list[n++] = &g_names[i];
	qsort(list, (size_t)n, sizeof(list[0]), TT_NameCompareRecent);
	out(ctx, "NAMES USED BY %s%s%s\n\n", key, label[0] ? "  -  " : "", label);
	if (!n)
		out(ctx, "No names recorded for this SteamID.\n");
	for (int i = 0; i < n; i++)
	{
		char a[16], b[16];
		TT_DateText(list[i]->first, a, sizeof(a));
		TT_DateText(list[i]->last, b, sizeof(b));
		out(ctx, "%s   (%d time%s, first %s, last %s)\n", list[i]->name, list[i]->times,
			list[i]->times == 1 ? "" : "s", a, b);
	}
	int picks, autos;
	TT_JoinCounts(key, &picks, &autos);
	if (picks || autos)
		out(ctx, "\nJoining a team: picked one %d time%s, auto-assign %d time%s\n", picks, picks == 1 ? "" : "s",
			autos, autos == 1 ? "" : "s");
}

static void TT_OutConsole(void *, const char *fmt, ...)
{
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	b[sizeof(b) - 1] = 0;
	SERVER_PRINT(b);
}

struct TTWin { char buf[1500]; size_t used; };
static void TT_OutWindow(void *ctx, const char *fmt, ...)
{
	TTWin *w = (TTWin *)ctx;
	if (w->used >= sizeof(w->buf) - 1)
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(w->buf + w->used, sizeof(w->buf) - w->used, fmt, ap);
	va_end(ap);
	if (n > 0)
		w->used += (size_t)n;
	if (w->used >= sizeof(w->buf))
		w->used = sizeof(w->buf) - 1;
	w->buf[w->used] = 0;
}

// !names <player> - admins only.
bool TT_NamesChat(edict_t *p, const char *rest)
{
	if (!TT_IsAdmin(p))
	{
		TT_Say(p, "%s Only admins can look up names.", TT_TAG);
		return true;
	}
	if (!rest || !rest[0])
	{
		TT_Say(p, "%s Say !names <part of a name>, !names #<userid> or !names <SteamID>.", TT_TAG);
		return true;
	}
	TTWin w;
	w.used = 0;
	w.buf[0] = 0;
	TT_NamesReport(rest, TT_OutWindow, &w);
	TT_ShowWindow(p, w.buf);
	return true;
}

static void TT_Cmd_Names(void)
{
	const char *a = CMD_ARGS();
	char arg[96];
	strncpy(arg, a ? a : "", sizeof(arg) - 1);
	arg[sizeof(arg) - 1] = 0;
	// strip quotes and spaces
	char *s = arg;
	while (*s == ' ' || *s == '"') s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '"')) s[--n] = 0;
	if (!s[0])
	{
		SERVER_PRINT("[Teams] tt_names <part of a name | #userid | SteamID>\n");
		return;
	}
	TT_NamesReport(s, TT_OutConsole, NULL);
}

void TT_NamesRegisterCommands(void)
{
	REG_SVR_COMMAND("tt_names", TT_Cmd_Names);
}
