// tt_feed.cpp - the end-of-map post to Discord: the score, the next map, the
// MVP, the awards, the rivalry of the map and the top players, as one embed.
//
// Player names are escaped, and Discord is told not to turn anything into a
// ping (allowed_mentions: none), so a player called "@everyone" stays text.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tt_common.h"
#include "tt_stats.h"
#include "tt_net.h"

#define TT_DOT "\xC2\xB7"       // UTF-8 middle dot

static bool TT_FeedOn(void) { return g_net.enabled && g_secret.discord[0]; }

static void TT_IsoTime(long t, char *out, size_t len)
{
	time_t tt = (time_t)t;
	struct tm *g = gmtime(&tt);
	if (!g) { out[0] = 0; return; }
	strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", g);
}

// ---------------------------------------------------------------------------
// End of map
void TT_FeedMapEnd(const TTFeedMapEnd &m)
{
	if (!TT_FeedOn())
		return;
	char iso[32];
	TT_IsoTime((long)time(NULL), iso, sizeof(iso));

	// Scores, e.g. "Blue 3 - Red 1".
	TTBuf score;
	for (int k = 0; k < g_map.playableCount; k++)
	{
		int t = g_map.playable[k];
		score.addf("%s%s %d", k ? "  -  " : "", TT_TeamName(t), m.teamScore[t]);
	}

	// The next map, as AMX Mod X's amx_nextmap has it - the RTV plugin keeps
	// that set to the vote's winner (and nextmap.amxx to the mapcycle's next).
	char next[64] = "";
	const char *nm = CVAR_GET_POINTER("amx_nextmap") ? CVAR_GET_STRING("amx_nextmap") : "";
	if (nm && nm[0] && strcasecmp(nm, "[not yet voted on]"))
	{
		strncpy(next, nm, sizeof(next) - 1);
		next[strcspn(next, " \t\"")] = 0;
	}

	TTBuf awards, top;
	for (int i = 0; i < m.awardCount; i++)
	{
		const TTFeedAward &a = m.awards[i];
		if (!strcmp(a.title, "MVP"))
			continue;
		TTBuf line;
		line.addf("%s: **", a.title);
		line.discord(a.name);
		line.addf("** (%.0f%s)\n", a.value, a.unit);
		if (awards.len + line.len > 1000) // Discord refuses a field over 1024 characters
			break;
		awards.add(line.c_str());
	}
	int shown = 0;
	for (int i = 0; i < m.playerCount && shown < 8; i++)
	{
		const TTFeedPlayer &p = m.players[i];
		top.addf("%d. ", shown + 1);
		top.discord(p.name);
		top.addf("%s  %d/%d", p.bot ? " (bot)" : "", p.kills, p.deaths);
		if (p.caps)
			top.addf(", %d cap%s", p.caps, p.caps == 1 ? "" : "s");
		top.addf(", %.0f pts\n", p.points);
		shown++;
	}

	TTBuf b;
	b.add("{\"allowed_mentions\":{\"parse\":[]},\"embeds\":[{\"color\":15844367,\"title\":");
	char title[96];
	_snprintf_wc(title, sizeof(title) - 1, "Map over: %s", m.map);
	title[sizeof(title) - 1] = 0;
	b.json(title);
	b.add(",\"description\":");
	TTBuf desc;
	desc.add("**");
	desc.add(score.c_str());
	desc.add("**");
	if (next[0])
	{
		desc.add("\nNext map: **");
		desc.discord(next);
		desc.add("**");
	}
	b.json(desc.c_str());
	b.add(",\"fields\":[");
	bool any = false;
	for (int i = 0; i < m.awardCount; i++)
		if (!strcmp(m.awards[i].title, "MVP"))
		{
			TTBuf v;
			v.add("**");
			v.discord(m.awards[i].name);
			v.addf("** (%.0f points)", m.awards[i].value);
			b.add("{\"name\":\"MVP\",\"value\":");
			b.json(v.c_str());
			b.add(",\"inline\":true}");
			any = true;
		}
	if (m.rivalA[0])
	{
		TTBuf v;
		v.discord(m.rivalA);
		v.addf(" %d " TT_DOT " %d ", m.rivalAB, m.rivalBA);
		v.discord(m.rivalB);
		b.add(any ? "," : "");
		b.add("{\"name\":\"Rivalry of the map\",\"value\":");
		b.json(v.c_str());
		b.add(",\"inline\":true}");
		any = true;
	}
	if (awards.len)
	{
		b.add(any ? "," : "");
		b.add("{\"name\":\"Awards\",\"value\":");
		b.json(awards.c_str());
		b.add("}");
		any = true;
	}
	if (top.len)
	{
		b.add(any ? "," : "");
		b.add("{\"name\":\"Top players\",\"value\":");
		b.json(top.c_str());
		b.add("}");
	}
	b.add("],\"footer\":{\"text\":");
	b.json(TT_NetServerName());
	b.add("},\"timestamp\":");
	b.json(iso);
	b.add("}]}");
	TT_NetSend(b.c_str());
}

// ---------------------------------------------------------------------------
void TT_FeedFrame(void)
{
	TT_NetFrame();
}

void TT_FeedTest(void)
{
	if (!g_net.enabled)
	{
		TT_Printf("[Teams] Discord posting is off (net_enabled 0 in tfc_teams.ini).\n");
		return;
	}
	if (!g_secret.discord[0])
	{
		TT_Printf("[Teams] No Discord webhook yet: tt_discord <id> <token> first.\n");
		return;
	}
	char iso[32];
	TT_IsoTime((long)time(NULL), iso, sizeof(iso));
	TTBuf b;
	b.add("{\"allowed_mentions\":{\"parse\":[]},\"embeds\":[{\"color\":5763719,\"title\":");
	b.json(TT_NetServerName());
	b.add(",\"description\":\"Test message from TFC Teams - the webhook works. The end-of-map awards will be posted here.\",\"timestamp\":");
	b.json(iso);
	b.add("}]}");
	TT_NetSend(b.c_str());
	TT_NetTestStarted();
	TT_Printf("[Teams] Test sent - check Discord, then tt_net for the result.\n");
}

static void TT_Cmd_Net(void)     { TT_NetPrintStatus(); }
static void TT_Cmd_NetTest(void) { TT_FeedTest(); }

void TT_FeedRegisterCommands(void)
{
	REG_SVR_COMMAND("tt_net", TT_Cmd_Net);
	REG_SVR_COMMAND("tt_net_test", TT_Cmd_NetTest);
}
