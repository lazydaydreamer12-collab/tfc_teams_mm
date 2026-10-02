// tt_balance.cpp - automatic team balance, and refusing joins that would
// unbalance the teams in the first place.
//
// WHEN TEAMS ARE UNEVEN (biggest - smallest >= balance_threshold) for
// balance_delay seconds, players on the biggest team are ranked:
//
//   1. bots before humans (balance_prefer_bots)
//   2. "new" players - who picked this team within balance_new_window
//      seconds - before everyone else, newest first
//   3. then by score (also the tie-break between equally new players): the
//      player whose move brings the two teams' frag totals closest together. Moving a player with f frags from the big
//      team (total B) to the small one (total S) leaves a gap of |B - S - 2f|,
//      so if the big team is also winning its better players rank first, and
//      if it is losing its weaker ones do.
//
// The top balance_candidates of that list are "preferred". Whenever one of
// them is dead (and only then), they are moved to the smallest team. If
// balance_patience seconds pass without that happening, anyone eligible on the
// big team who dies is moved instead - so an imbalance can never be kept alive
// just because the preferred players are good at not dying.
//
// Never moved: HLTV, admins when admin_immunity is on, bots when
// balance_include_bots is off, and anyone we moved in the last
// balance_immunity seconds. Nothing happens while a scramble is under way -
// the scramble is itself rebalancing everybody.
//
// JOIN BLOCK. "jointeam N" from a real client is refused when it would put
// team N balance_threshold or more ahead of the smallest team - exactly the
// joins that would otherwise trigger a balance straight away. The team menu is
// reopened so they can pick again. "jointeam 5" (auto-assign) is left to TFC,
// which already puts people on the smallest team.

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tt_common.h"

static float g_imbalSince = 0;   // 0 = teams are even
static float g_nextEval = 0;
static int   g_bigTeam = 0, g_smallTeam = 0;
static int   g_counts[TT_MAX_TEAMS + 1];
static float g_scores[TT_MAX_TEAMS + 1];
static int   g_pref[TT_MAX_PLAYERS];
static int   g_prefCount = 0;
static bool  g_announced = false;
static int   g_rank[TT_MAX_PLAYERS];   // the whole big team, best pick first
static int   g_rankCount = 0;
static int   g_forceIdx = 0;           // warned and about to be moved alive (0 = nobody)
static float g_forceAt = 0;

static void TT_ClearForce(const char *why)
{
	if (g_forceIdx && why)
	{
		edict_t *e = TT_Player(g_forceIdx);
		TT_Trace("Balance: forced move of %s called off (%s)", e ? STRING(e->v.netname) : "?", why);
		if (e && !strcmp(why, "teams even"))
			TT_Center(e, "Teams are even again - you stay on your team");
		else if (e && !strcmp(why, "someone else died first"))
			TT_Center(e, "Someone else was moved - you stay on your team");
	}
	g_forceIdx = 0;
	g_forceAt = 0;
}

void TT_BalanceReset(void)
{
	g_imbalSince = 0;
	g_nextEval = 0;
	g_bigTeam = g_smallTeam = 0;
	g_prefCount = 0;
	g_rankCount = 0;
	g_announced = false;
	g_forceIdx = 0;
	g_forceAt = 0;
}

// Carrying a flag or other goal item. tfgoalitem_GiveToPlayer (tfc.so
// 0x1021c0) makes a carried item follow its carrier: aiment = owner = the
// player, movetype MOVETYPE_FOLLOW. The short map name "i_t_g" is checked too.
static bool TT_CarryingGoalItem(edict_t *p)
{
	static const char *names[] = { "item_tfgoal", "i_t_g" };
	for (int n = 0; n < 2; n++)
	{
		edict_t *it = NULL;
		while (!FNullEnt(it = FIND_ENTITY_BY_STRING(it, "classname", names[n])))
		{
			if (it->v.aiment == p && it->v.owner == p && it->v.movetype == MOVETYPE_FOLLOW)
				return true;
		}
	}
	return false;
}

static bool TT_Eligible(int idx, edict_t *e)
{
	if (!e || TT_IsHLTV(e))
		return false;
	if (TT_IsBot(e) && !g_tt.balanceIncludeBots)
		return false;
	if (g_tt.adminImmunity && TT_IsAdmin(e))
		return false;
	if (g_pl[idx].movedAt > 0 && gpGlobals->time - g_pl[idx].movedAt < g_tt.balanceImmunity)
		return false;
	return true;
}

// Smallest team that still has room. Ties: lower frag total, then lower number.
static int TT_PickSmallest(const int counts[], const float scores[], int skipTeam)
{
	int best = 0;
	for (int k = 0; k < g_map.playableCount; k++)
	{
		int t = g_map.playable[k];
		if (t == skipTeam)
			continue;
		int lim = TT_TeamLimit(t);
		if (lim > 0 && counts[t] >= lim)
			continue;
		if (!best || counts[t] < counts[best]
			|| (counts[t] == counts[best] && scores[t] < scores[best]))
			best = t;
	}
	return best;
}

struct TTCand { int idx; int group; float key; float key2; };

static int TT_CandCompare(const void *a, const void *b)
{
	const TTCand *x = (const TTCand *)a, *y = (const TTCand *)b;
	if (x->group != y->group)
		return x->group - y->group;
	if (x->key < y->key) return -1;
	if (x->key > y->key) return 1;
	if (x->key2 < y->key2) return -1;
	if (x->key2 > y->key2) return 1;
	return x->idx - y->idx;
}

static void TT_Evaluate(void)
{
	float now = gpGlobals->time;
	TT_TeamCounts(g_counts, g_scores, 0, false);

	int big = 0;
	for (int k = 0; k < g_map.playableCount; k++)
	{
		int t = g_map.playable[k];
		if (!big || g_counts[t] > g_counts[big]
			|| (g_counts[t] == g_counts[big] && g_scores[t] > g_scores[big]))
			big = t;
	}
	int small = TT_PickSmallest(g_counts, g_scores, big);
	if (!big || !small || g_counts[big] - g_counts[small] < g_tt.balanceThreshold)
	{
		if (g_imbalSince)
			TT_Trace("Balance: teams even again");
		TT_ClearForce("teams even");
		g_imbalSince = 0;
		g_prefCount = 0;
		g_rankCount = 0;
		g_announced = false;
		return;
	}
	if (!g_imbalSince)
	{
		g_imbalSince = now;
		TT_Trace("Balance: uneven - %s %d v %s %d (frags %.0f v %.0f)",
			TT_TeamName(big), g_counts[big], TT_TeamName(small), g_counts[small],
			g_scores[big], g_scores[small]);
	}
	g_bigTeam = big;
	g_smallTeam = small;

	// Rank the big team.
	TTCand c[TT_MAX_PLAYERS];
	int n = 0;
	bool anyBot = false;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (!e || TT_PlayerTeam(e) != big || !TT_Eligible(i, e))
			continue;
		if (TT_IsBot(e))
			anyBot = true;
		c[n].idx = i;
		c[n].group = 0;
		c[n].key = 0;
		c[n].key2 = 0;
		n++;
	}
	float gap = g_scores[big] - g_scores[small];
	for (int j = 0; j < n; j++)
	{
		edict_t *e = INDEXENT(c[j].idx);
		bool isNew = g_tt.balanceNewWindow > 0 && now - g_pl[c[j].idx].teamJoinedAt < g_tt.balanceNewWindow;
		int botGroup = (g_tt.balancePreferBots && anyBot && !TT_IsBot(e)) ? 2 : 0;
		// A person whose class is full (or not allowed) on the small team would
		// have to pick another one when moved - take them last. (A bot is just
		// given an open class, so this does not apply to bots.)
		int cls = (int)e->v.playerclass;
		int classGroup = (!TT_IsBot(e) && cls >= 1 && cls <= 9 && !TT_ClassAllowed(small, cls, e)) ? 4 : 0;
		c[j].group = classGroup + botGroup + (isNew ? 0 : 1);
		// New players: newest first (to the second - everybody present at
		// map start joined "at once"), then best score fit. Everyone else:
		// best score fit.
		float fit = (float)fabs(gap - 2.0f * TT_Score(e));
		c[j].key  = isNew ? -floorf(g_pl[c[j].idx].teamJoinedAt) : fit;
		c[j].key2 = isNew ? fit : 0.0f;
	}
	qsort(c, n, sizeof(c[0]), TT_CandCompare);
	int old[TT_MAX_PLAYERS], oldCount = g_prefCount;
	memcpy(old, g_pref, sizeof(int) * (size_t)oldCount);
	g_prefCount = 0;
	for (int j = 0; j < n && g_prefCount < g_tt.balanceCandidates; j++)
		g_pref[g_prefCount++] = c[j].idx;
	g_rankCount = n;
	for (int j = 0; j < n; j++)
		g_rank[j] = c[j].idx;

	// Log the preferred list whenever it changes, not every half second.
	if (g_prefCount != oldCount || memcmp(old, g_pref, sizeof(int) * (size_t)g_prefCount))
	{
		char line[200] = "";
		size_t used = 0;
		for (int j = 0; j < g_prefCount; j++)
		{
			edict_t *e = INDEXENT(g_pref[j]);
			int w = _snprintf_wc(line + used, sizeof(line) - used - 1, "%s%s(%.0f%s)", j ? ", " : "",
				STRING(e->v.netname), TT_Score(e), TT_IsBot(e) ? ",bot" : "");
			if (w < 0 || (used += (size_t)w) >= sizeof(line) - 1)
				break;
		}
		line[sizeof(line) - 1] = 0;
		TT_Trace("Balance: preferred to move from %s: %s", TT_TeamName(big), g_prefCount ? line : "(nobody eligible)");
	}
}

void TT_BalanceFrame(void)
{
	float now = gpGlobals->time;
	if (!g_tt.enabled || !g_tt.balanceEnabled || g_map.playableCount < 2 || TT_ScrambleActive())
	{
		g_imbalSince = 0;
		g_prefCount = 0;
		g_rankCount = 0;
		g_announced = false;
		TT_ClearForce(TT_ScrambleActive() ? "!scramble started" : NULL);
		return;
	}
	if (now >= g_nextEval)
	{
		g_nextEval = now + 0.5f;
		TT_Evaluate();
	}
	if (!g_imbalSince || now - g_imbalSince < g_tt.balanceDelay)
		return;

	if (!g_announced)
	{
		g_announced = true;
		TT_SayAll("%s Teams are uneven (%s %d v %s %d) - evening them out as players respawn.",
			TT_TAG, TT_TeamName(g_bigTeam), g_counts[g_bigTeam],
			TT_TeamName(g_smallTeam), g_counts[g_smallTeam]);
	}

	bool anyone = (now - g_imbalSince - g_tt.balanceDelay) >= g_tt.balancePatience;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (!e || TT_PlayerTeam(e) != g_bigTeam || !TT_MovableNow(e))
			continue;
		if (now < g_pl[i].nextMoveTry || !TT_Eligible(i, e))
			continue;
		// TeamSet refuses a second change within 1s (the live log showed
		// bots being picked the instant they joined, and refused).
		if (now - g_pl[i].teamJoinedAt < 1.2f)
			continue;
		bool preferred = false;
		for (int j = 0; j < g_prefCount; j++)
			if (g_pref[j] == i) { preferred = true; break; }
		if (!preferred && !anyone)
			continue;

		int from = g_bigTeam, to = g_smallTeam;
		if (TT_MovePlayer(e, to, preferred ? "balance: preferred pick" : "balance: patience ran out"))
		{
			int counts[TT_MAX_TEAMS + 1];
			TT_TeamCounts(counts, NULL, 0, false);
			TT_SayAll("%s %s was moved to %s to even the teams (%s %d v %s %d).",
				TT_TAG, STRING(e->v.netname), TT_TeamName(to),
				TT_TeamName(from), counts[from], TT_TeamName(to), counts[to]);
			TT_Center(e, "You were moved to %s to balance the teams", TT_TeamName(to));
			if (g_forceIdx == i)
				TT_ClearForce(NULL);
			else
				TT_ClearForce("someone else died first");
			// Start over from fresh counts; the next move (if one is still
			// needed) waits for the next evaluation.
			g_nextEval = 0;
			g_imbalSince = 0;
			g_announced = true; // do not re-announce the same imbalance
			TT_Evaluate();
			if (g_imbalSince)
				g_imbalSince = now - g_tt.balanceDelay; // still uneven: no second delay
			return;
		}
	}

	// STALEMATE BREAKER. Nobody on the big team has died for
	// balance_force_after seconds - someone AFK in spawn, or a defence that
	// never gets killed. Warn the best-ranked player who is not carrying a goal
	// item, give them balance_force_warn seconds (dying in that time moves
	// them anyway, through the loop above), then move them alive. TFC's
	// TeamSet kills a living player it moves, exactly as picking
	// "change team" does; the frag is handed back.
	if (g_tt.balanceForceAfter <= 0)
		return;
	float acting = now - g_imbalSince - g_tt.balanceDelay;
	if (acting < g_tt.balanceForceAfter)
		return;

	if (g_forceIdx)
	{
		edict_t *e = TT_Player(g_forceIdx);
		if (!e || TT_PlayerTeam(e) != g_bigTeam || !TT_Eligible(g_forceIdx, e))
		{
			TT_ClearForce("!target left the team or the server");
		}
		else if (TT_CarryingGoalItem(e))
		{
			TT_Trace("Balance: %s picked up a goal item - choosing someone else", STRING(e->v.netname));
			TT_Center(e, "You have the flag - you stay on %s", TT_TeamName(g_bigTeam));
			g_forceIdx = 0;
		}
		else if (now >= g_forceAt && now >= g_pl[g_forceIdx].nextMoveTry)
		{
			int from = g_bigTeam, to = g_smallTeam;
			if (TT_MovePlayer(e, to, "balance: forced after deadline"))
			{
				int counts[TT_MAX_TEAMS + 1];
				TT_TeamCounts(counts, NULL, 0, false);
				TT_SayAll("%s %s was moved to %s to even the teams (%s %d v %s %d).",
					TT_TAG, STRING(e->v.netname), TT_TeamName(to),
					TT_TeamName(from), counts[from], TT_TeamName(to), counts[to]);
				TT_Center(e, "You were moved to %s to balance the teams", TT_TeamName(to));
				g_forceIdx = 0;
				g_nextEval = 0;
				g_imbalSince = 0;
				g_announced = true;
				TT_Evaluate();
				if (g_imbalSince)
					g_imbalSince = now - g_tt.balanceDelay; // the next one waits the full deadline again
			}
			// Refused (dying this very frame, or changed team <1s ago):
			// TT_MovePlayer set a retry time; try again then.
		}
		return;
	}

	for (int j = 0; j < g_rankCount; j++)
	{
		int i = g_rank[j];
		edict_t *e = TT_Player(i);
		if (!e || TT_PlayerTeam(e) != g_bigTeam || !TT_Eligible(i, e) || TT_CarryingGoalItem(e))
			continue;
		g_forceIdx = i;
		g_forceAt = now + g_tt.balanceForceWarn;
		TT_Trace("Balance: nobody on %s has died in %.0fs - %s will be moved in %.0fs",
			TT_TeamName(g_bigTeam), acting, STRING(e->v.netname), g_tt.balanceForceWarn);
		TT_Say(e, "%s Teams are uneven: you will be moved to %s in %d seconds.",
			TT_TAG, TT_TeamName(g_smallTeam), (int)ceil(g_tt.balanceForceWarn));
		TT_Center(e, "Moving you to %s in %d seconds to balance the teams",
			TT_TeamName(g_smallTeam), (int)ceil(g_tt.balanceForceWarn));
		return;
	}
	// Everyone eligible is carrying something (or nobody is eligible): wait.
}

bool TT_JoinTeamBlocked(edict_t *p, int team)
{
	if (!g_tt.enabled || FNullEnt(p))
		return false;
	{
		int ri = ENTINDEX(p);
		if (TT_ScrambleResetting() && ri >= 1 && ri <= TT_MAX_PLAYERS && g_pl[ri].inReset)
		{
			// They are about to be placed; letting them pick now would undo
			// the plan. (Applies whatever block_uneven_join is set to.)
			TT_Say(p, "%s Team reset in progress - you will be put on %s in a moment.",
				TT_TAG, TT_TeamName(g_pl[ri].pendingTeam));
			return true;
		}
	}
	if (!g_tt.enabled || !g_tt.blockUnevenJoin || g_map.playableCount < 2)
		return false;
	if (!TT_TeamPlayable(team) || FNullEnt(p) || TT_IsBot(p))
		return false;
	if (g_tt.adminImmunity && TT_IsAdmin(p))
		return false;
	int idx = ENTINDEX(p);
	if ((int)p->v.team == team)
		return false; // TFC ignores it anyway

	int counts[TT_MAX_TEAMS + 1];
	float scores[TT_MAX_TEAMS + 1];
	TT_TeamCounts(counts, scores, idx, TT_ScrambleActive());
	int smallest = -1;
	for (int k = 0; k < g_map.playableCount; k++)
	{
		int t = g_map.playable[k];
		int lim = TT_TeamLimit(t);
		if (t != team && lim > 0 && counts[t] >= lim)
			continue; // a full team cannot be the one they "should" join
		if (smallest < 0 || counts[t] < smallest)
			smallest = counts[t];
	}
	if (smallest < 0 || counts[team] + 1 - smallest < g_tt.balanceThreshold)
		return false;

	int suggest = TT_PickSmallest(counts, scores, 0);
	TT_Trace("Join: %s refused team %d (%s) - would be %d v %d",
		STRING(p->v.netname), team, TT_TeamName(team), counts[team] + 1, smallest);
	if (gpGlobals->time >= g_pl[idx].nextJoinMsg)
	{
		g_pl[idx].nextJoinMsg = gpGlobals->time + 1.0f;
		TT_Say(p, "%s %s has too many players (%d v %d). Please join %s.",
			TT_TAG, TT_TeamName(team), counts[team], smallest, TT_TeamName(suggest));
		TT_Center(p, "%s is full up - join %s", TT_TeamName(team), TT_TeamName(suggest));
	}
	// Put the team menu back up. "changeteam" is TFC's own command for that
	// (ClientCommand -> Menu_Team, which sends VGUIMenu 2).
	TT_FakeClientCommand(p, "changeteam", NULL);
	return true;
}
