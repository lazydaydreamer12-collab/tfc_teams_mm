// tt_scramble.cpp - the !scramble vote, the score-balanced scramble itself,
// and the admin/server commands.
//
// THE VOTE works like rock-the-vote: saying !scramble adds your vote, and the
// scramble happens as soon as vote_percent of the eligible players have said
// it (and at least vote_min_votes). Eligible = humans on a team (spectators
// too if vote_count_spectators), not AFK. Going AFK drops your vote; leaving
// drops it; both re-check the threshold, since a smaller population can tip
// it over. No voting in the first vote_map_start_delay seconds of a map or
// within vote_cooldown seconds of the last scramble.
//
// THE SCRAMBLE deals players out by score, strongest first, each to whichever
// team has the lowest frag total among those still short of an even share
// (so team sizes end up at most one apart and frag totals as close as a
// greedy deal gets them). Which pile becomes which team is then chosen to
// keep as many players where they already are as possible - the piles are
// what matter, not their colours, and every avoided move is one less death.
//
// Nobody is killed for it: each player who has to switch is told where they
// are going and moved the next time they die (see TT_MovableNow). Moves still
// pending after scramble_max_wait seconds are dropped, and the auto-balancer
// (paused while a scramble is running) takes it from there. An admin can
// force the whole thing to happen at once with "now", which is a real TFC
// team change for everyone moved - living players die, as they would picking
// "change team" themselves.

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>

#include "tt_common.h"
#include "tt_net.h"
#include "tt_stats.h"

float g_mapStartTime = 0;

static bool  g_active = false;
static float g_nextAdvert = 0;      // next chat tip (see TT_AdvertFrame)
static float g_resetAt = 0;      // reset scramble: when everyone is placed (0 = not resetting)
static float g_nextCountdown = 0;
static float g_startedAt = 0;
static float g_lastEnd = -100000.0f;
static float g_nextCheck = 0;

bool TT_ScrambleActive(void) { return g_active; }
bool TT_ScrambleResetting(void) { return g_active && g_resetAt > 0; }

void TT_ScrambleReset(void)
{
	g_nextAdvert = gpGlobals->time + g_tt.advertFirst;
	g_active = false;
	g_resetAt = 0;
	g_startedAt = 0;
	g_lastEnd = -100000.0f;
	g_nextCheck = 0;
	for (int i = 0; i <= TT_MAX_PLAYERS; i++)
	{
		g_pl[i].voted = false;
		g_pl[i].pendingTeam = 0;
		g_pl[i].pendingFrom = 0;
		g_pl[i].inReset = false;
	}
}

// ---------------------------------------------------------------------------
// Vote bookkeeping
static bool TT_VoteEligible(edict_t *e, int idx)
{
	if (!e || TT_IsBot(e) || TT_IsHLTV(e) || g_pl[idx].afk)
		return false;
	if (!g_tt.voteCountSpectators && !TT_PlayerTeam(e))
		return false;
	return true;
}

static void TT_VoteTally(int *votes, int *needed, int *population)
{
	int pop = 0, v = 0;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (!TT_VoteEligible(e, i))
			continue;
		pop++;
		if (g_pl[i].voted)
			v++;
	}
	int need = (int)ceil(pop * g_tt.votePercent / 100.0f - 0.0001f);
	if (need < g_tt.voteMinVotes)
		need = g_tt.voteMinVotes;
	if (need < 1)
		need = 1;
	*votes = v;
	*needed = need;
	*population = pop;
}

static void TT_ClearVotes(void)
{
	for (int i = 0; i <= TT_MAX_PLAYERS; i++)
		g_pl[i].voted = false;
}

// Why a vote cannot be cast right now, or NULL if it can.
static const char *TT_VoteBlockedReason(char *buf, size_t len)
{
	float now = gpGlobals->time;
	if (!g_tt.enabled || !g_tt.voteEnabled)
		return "Scramble voting is turned off on this server.";
	if (g_map.playableCount < 2)
		return "This map does not have two teams to scramble.";
	if (g_active)
		return "A scramble is already under way.";
	float opens = g_mapStartTime + g_tt.voteMapStartDelay;
	if (now < opens)
	{
		_snprintf_wc(buf, len - 1, "Scramble voting opens %d seconds into the map (%d s to go).",
			(int)g_tt.voteMapStartDelay, (int)ceil(opens - now));
		buf[len - 1] = 0;
		return buf;
	}
	if (now - g_lastEnd < g_tt.voteCooldown)
	{
		_snprintf_wc(buf, len - 1, "The teams were just scrambled - voting reopens in %d s.",
			(int)ceil(g_tt.voteCooldown - (now - g_lastEnd)));
		buf[len - 1] = 0;
		return buf;
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Planning
struct TTPlan
{
	int   idx;
	int   from;
	int   to;
	float score;      // what the deal goes by: rank, or a random number (pick_mode random)
	float strength;   // the player's real rank, for the log
	int   rnd;
	bool  fixed;
};

static int TT_PlanCompare(const void *a, const void *b)
{
	const TTPlan *x = (const TTPlan *)a, *y = (const TTPlan *)b;
	if (x->score > y->score) return -1;
	if (x->score < y->score) return 1;
	return x->rnd - y->rnd; // equal scores: shuffled
}

// Fills plan[].to. Returns false (with a reason) when there is nothing sane to do.
static bool TT_BuildPlan(TTPlan *plan, int n, const char **why)
{
	int k = g_map.playableCount;
	if (k < 2) { *why = "this map does not have two teams"; return false; }
	if (n < 2) { *why = "there are not enough players on teams"; return false; }

	int base = n / k, rem = n % k, extras = 0;
	int size[TT_MAX_TEAMS + 1] = { 0 };
	float sum[TT_MAX_TEAMS + 1] = { 0 };
	bool anyFixed = false;

	// Players who stay put (bots, when scramble_include_bots is 0) first.
	for (int j = 0; j < n; j++)
	{
		if (!plan[j].fixed)
			continue;
		anyFixed = true;
		int t = plan[j].from;
		plan[j].to = t;
		if (size[t] == base)
			extras++;
		size[t]++;
		sum[t] += plan[j].score;
	}

	qsort(plan, n, sizeof(plan[0]), TT_PlanCompare);
	for (int j = 0; j < n; j++)
	{
		if (plan[j].fixed)
			continue;
		int best = 0;
		for (int pass = 0; pass < 2 && !best; pass++)
		{
			for (int a = 0; a < k; a++)
			{
				int t = g_map.playable[a];
				int lim = TT_TeamLimit(t);
				if (lim > 0 && size[t] >= lim)
					continue;
				// pass 0 keeps sizes even; pass 1 (only reached when fixed
				// players already overfilled things) just takes the smallest.
				if (pass == 0 && !(size[t] < base || (size[t] == base && extras < rem)))
					continue;
				if (!best
					|| (pass == 0 && (sum[t] < sum[best] || (sum[t] == sum[best] && size[t] < size[best])))
					|| (pass == 1 && size[t] < size[best]))
					best = t;
			}
		}
		if (!best)
		{
			*why = "every team is at its player limit";
			return false;
		}
		if (size[best] == base)
			extras++;
		size[best]++;
		sum[best] += plan[j].score;
		plan[j].to = best;
	}

	// Relabel the piles to keep the most people where they are. With fixed
	// players the piles are already tied to their teams, so leave them.
	if (!anyFixed)
	{
		int perm[TT_MAX_TEAMS], bestPerm[TT_MAX_TEAMS];
		for (int a = 0; a < k; a++)
			perm[a] = bestPerm[a] = a;
		int bestStay = -1;
		do
		{
			// Pile a (currently labelled playable[a]) -> team playable[perm[a]].
			bool ok = true;
			for (int a = 0; a < k && ok; a++)
			{
				int lim = TT_TeamLimit(g_map.playable[perm[a]]);
				if (lim > 0 && size[g_map.playable[a]] > lim)
					ok = false;
			}
			if (!ok)
				continue;
			int stay = 0;
			for (int j = 0; j < n; j++)
			{
				int a = 0;
				while (a < k && g_map.playable[a] != plan[j].to)
					a++;
				if (a < k && g_map.playable[perm[a]] == plan[j].from)
					stay++;
			}
			if (stay > bestStay)
			{
				bestStay = stay;
				memcpy(bestPerm, perm, sizeof(perm));
			}
		} while (std::next_permutation(perm, perm + k));

		for (int j = 0; j < n; j++)
		{
			int a = 0;
			while (a < k && g_map.playable[a] != plan[j].to)
				a++;
			if (a < k)
				plan[j].to = g_map.playable[bestPerm[a]];
		}
	}
	return true;
}

// pick_mode mixed: the rank deal decides who SHOULD swap; a dice roll decides
// whether each swap happens. Movers from team a to team b are paired with
// movers from b to a, best with best (the plan is in rank order by now), so a
// skipped swap takes one player off each side and the team sizes stay as the
// deal made them. Moves with no partner going the other way - the ones that
// even out the sizes - always happen.
static void TT_MixedRolls(TTPlan *plan, int n, int *pairs, int *kept)
{
	*pairs = *kept = 0;
	int k = g_map.playableCount;
	for (int ia = 0; ia < k; ia++)
		for (int ib = ia + 1; ib < k; ib++)
		{
			int a = g_map.playable[ia], b = g_map.playable[ib];
			int ab[TT_MAX_PLAYERS], ba[TT_MAX_PLAYERS], nab = 0, nba = 0;
			for (int j = 0; j < n; j++)
			{
				if (plan[j].fixed)
					continue;
				if (plan[j].from == a && plan[j].to == b)
					ab[nab++] = j;
				else if (plan[j].from == b && plan[j].to == a)
					ba[nba++] = j;
			}
			for (int p = 0; p < nab && p < nba; p++)
			{
				(*pairs)++;
				bool happens = RANDOM_LONG(1, 100) <= g_tt.pickMixedChance;
				TT_Trace("Scramble (mixed): swap %s <-> %s - rolled %s",
					STRING(INDEXENT(plan[ab[p]].idx)->v.netname), STRING(INDEXENT(plan[ba[p]].idx)->v.netname),
					happens ? "IN" : "out, both stay");
				if (happens)
				{
					(*kept)++;
					continue;
				}
				plan[ab[p]].to = a;
				plan[ba[p]].to = b;
			}
		}
}

static const char *TT_ModeName(int mode)
{
	return mode == TT_SCR_RESET ? "spectator reset" : (mode == TT_SCR_NOW ? "now" : "at respawn");
}

void TT_ScrambleStart(int mode, const char *who)
{
	bool now = (mode == TT_SCR_NOW);
	TTPlan plan[TT_MAX_PLAYERS];
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		int t = TT_PlayerTeam(e);
		if (!t)
			continue;
		plan[n].idx = i;
		plan[n].from = t;
		plan[n].to = t;
		// Dealt by skill rating (tt_stats.cpp) - always for the reset scramble,
		// and for the others with balance_by_skill 1; otherwise by this map's
		// frags. (This map's frags stay in as a tiny tiebreak, so equal ratings -
		// everyone new, say - still get dealt by how this map is going.)
		plan[n].strength = ((mode == TT_SCR_RESET || g_tt.balanceBySkill) && g_st.enabled)
			? TT_StatsRating(e) + TT_Score(e) * 0.001f : TT_Score(e);
		// pick_mode random: the same deal, by a random number instead - team
		// sizes still come out even, who is on which side is pure chance.
		plan[n].score = (g_tt.pickMode == TT_PICK_RANDOM) ? (float)RANDOM_LONG(0, 1000000) : plan[n].strength;
		plan[n].rnd = RANDOM_LONG(0, 0x7fff);
		plan[n].fixed = TT_IsBot(e) && !g_tt.scrambleIncludeBots;
		n++;
	}

	const char *why = "";
	TT_ClearVotes();
	if (!TT_BuildPlan(plan, n, &why))
	{
		TT_SayAll("%s Scramble called off: %s.", TT_TAG, why);
		TT_Trace("Scramble (%s): not possible - %s", who, why);
		return; // nothing happened, so no cooldown
	}

	int mixPairs = 0, mixKept = 0;
	if (g_tt.pickMode == TT_PICK_MIXED)
		TT_MixedRolls(plan, n, &mixPairs, &mixKept);

	int moves = 0;
	float sums[TT_MAX_TEAMS + 1] = { 0 };
	int sizes[TT_MAX_TEAMS + 1] = { 0 };
	for (int j = 0; j < n; j++)
	{
		sums[plan[j].to] += plan[j].strength;
		sizes[plan[j].to]++;
		g_pl[plan[j].idx].pendingTeam = 0;
		if (plan[j].to != plan[j].from)
		{
			g_pl[plan[j].idx].pendingTeam = plan[j].to;
			g_pl[plan[j].idx].pendingFrom = plan[j].from;
			moves++;
		}
	}
	TT_Trace("Scramble (%s, %s, pick %s): %d players, %d to move. New teams: %s %d (%.0f) | %s %d (%.0f) | %s %d (%.0f) | %s %d (%.0f)",
		who, TT_ModeName(mode), TT_PickModeName(g_tt.pickMode), n, moves,
		TT_TeamName(1), sizes[1], sums[1], TT_TeamName(2), sizes[2], sums[2],
		TT_TeamName(3), sizes[3], sums[3], TT_TeamName(4), sizes[4], sums[4]);

	// Mixed: say how the dice fell, so a scramble that moves fewer people
	// than expected (or nobody) does not look broken.
	if (mixPairs)
		TT_SayAll("%s Mixed scramble: %d of %d swap%s rolled in (%d%% chance each).", TT_TAG,
			mixKept, mixPairs, mixPairs == 1 ? "" : "s", g_tt.pickMixedChance);

	if (mode == TT_SCR_RESET)
	{
		// Everybody goes to spectator - the ones staying on their team too,
		// that is the point of a reset - and comes back together.
		g_active = true;
		g_startedAt = gpGlobals->time;
		g_resetAt = gpGlobals->time + g_tt.scrambleResetDelay;
		g_nextCountdown = 0;
		TT_SayAll("%s Team reset! Everyone to spectator - back in %d seconds on scrambled teams.",
			TT_TAG, (int)ceil(g_tt.scrambleResetDelay));
		for (int j = 0; j < n; j++)
		{
			int i = plan[j].idx;
			edict_t *e = INDEXENT(i);
			TTPlayer &s = g_pl[i];
			int cls = (int)e->v.playerclass;
			s.inReset = true;
			s.resetClass = (cls >= 1 && cls <= 9) ? cls : 0;
			s.resetFrags = e->v.frags;
			s.pendingTeam = plan[j].to;   // for projected counts / the join block
			s.pendingFrom = plan[j].from;
			s.nextMoveTry = 0;
			TT_SendToSpectator(e);
			TT_Say(e, "%s You will be on %s.", TT_TAG, TT_TeamName(plan[j].to));
		}
		return;
	}

	if (moves == 0)
	{
		if (mixPairs)
			TT_SayAll("%s Scramble: the dice kept everyone where they are.", TT_TAG);
		else
			TT_SayAll("%s Scramble: the teams are already as even as they can be - nobody needs to move.", TT_TAG);
		g_lastEnd = gpGlobals->time;
		return;
	}

	g_active = true;
	g_startedAt = gpGlobals->time;
	if (now)
		TT_SayAll("%s Scrambling the teams now (%d player%s switching).", TT_TAG, moves, moves == 1 ? "" : "s");
	else
		TT_SayAll("%s Teams will be scrambled! %d player%s will switch when they next respawn.",
			TT_TAG, moves, moves == 1 ? "" : "s");

	for (int j = 0; j < n; j++)
	{
		int i = plan[j].idx;
		if (!g_pl[i].pendingTeam)
			continue;
		edict_t *e = INDEXENT(i);
		if (now)
			g_pl[i].nextMoveTry = 0;
		else
		{
			TT_Say(e, "%s Scramble: you are moving to %s when you next respawn.", TT_TAG, TT_TeamName(g_pl[i].pendingTeam));
			TT_Center(e, "Scramble: you join %s when you next respawn", TT_TeamName(g_pl[i].pendingTeam));
		}
	}
	if (now)
	{
		for (int j = 0; j < n; j++)
		{
			int i = plan[j].idx;
			edict_t *e = INDEXENT(i);
			int to = g_pl[i].pendingTeam;
			if (!to || e->v.deadflag == DEAD_DYING)
				continue; // mid-death-animation: picked up by the frame loop
			if (TT_MovePlayer(e, to, "scramble now"))
			{
				g_pl[i].pendingTeam = 0;
				TT_Center(e, "Scramble: you are now on %s", TT_TeamName(to));
			}
		}
	}
}

void TT_ScrambleCancel(const char *who)
{
	if (TT_ScrambleResetting())
	{
		// Everyone is sitting in spectator: "cancel" can only mean "put
		// them back now", or they would be stranded there.
		g_resetAt = gpGlobals->time;
		TT_SayAll("%s %s ended the countdown - placing everyone now.", TT_TAG, who);
		TT_Trace("Reset scramble: countdown ended early by %s", who);
		return;
	}
	int dropped = 0;
	for (int i = 1; i <= TT_MAX_PLAYERS; i++)
		if (g_pl[i].pendingTeam)
		{
			g_pl[i].pendingTeam = 0;
			dropped++;
		}
	TT_ClearVotes();
	if (g_active)
	{
		g_active = false;
		g_lastEnd = gpGlobals->time;
		TT_SayAll("%s Scramble cancelled by %s.", TT_TAG, who);
	}
	TT_Trace("Scramble cancelled by %s (%d pending moves dropped)", who, dropped);
}

// ---------------------------------------------------------------------------
// Reset scramble, second half: the countdown, then everyone onto their team.
static void TT_ResetFrame(float now)
{
	if (now < g_resetAt)
	{
		if (now >= g_nextCountdown)
		{
			g_nextCountdown = now + 1.0f;
			for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
			{
				edict_t *e = TT_Player(i);
				if (e && g_pl[i].inReset)
					TT_Center(e, "Team reset: you join %s in %d",
						TT_TeamName(g_pl[i].pendingTeam), (int)ceil(g_resetAt - now));
			}
		}
		return;
	}

	int left = 0;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		TTPlayer &s = g_pl[i];
		if (!s.inReset)
			continue;
		edict_t *e = TT_Player(i);
		if (!e)
		{
			s.inReset = false;
			s.pendingTeam = 0;
			continue;
		}
		int to = s.pendingTeam;
		if (now < s.nextMoveTry)
		{
			left++;
			continue;
		}
		bool ok = TT_MovePlayerEx(e, to, s.resetClass, "scramble reset");
		// Already on the right team with no class (a bot that rejoined by
		// itself, or spectate was refused and they never left): just the class.
		if (ok && e->v.playerclass == 0 && s.resetClass)
			TT_RestoreClass(e, s.resetClass);
		if (!ok)
		{
			if (now - g_resetAt < 5.0f)
			{
				left++;
				continue; // TT_MovePlayerEx set a retry time
			}
			TT_Trace("Reset scramble: gave up placing %s on %d - team menu shown", STRING(e->v.netname), to);
			TT_FakeClientCommand(e, "changeteam", NULL);
		}
		else
		{
			if (g_tt.scrambleResetFrags)
			{
				// The scoreboard's ScoreInfo is sent from pev->frags whenever it
				// changes (UpdateLowPriorityClientData, tfc.so 0xc14f0).
				e->v.frags = s.resetFrags;
				TT_Trace("Reset scramble: %s frags restored to %.0f", STRING(e->v.netname), s.resetFrags);
			}
			TT_Center(e, "You are on %s", TT_TeamName(to));
		}
		s.inReset = false;
		s.pendingTeam = 0;
	}
	if (left == 0)
	{
		g_active = false;
		g_resetAt = 0;
		g_lastEnd = now;
		TT_SayAll("%s Teams reset and scrambled.", TT_TAG);
		TT_Trace("Reset scramble complete after %.1fs", now - g_startedAt);
	}
}

// ---------------------------------------------------------------------------
// Chat tips: how to !scramble, !teams, !stats. Every advert_interval seconds
// (the first at advert_first into the map) a line to everyone, taking turns;
// and once per server session, a tip to each player advert_join seconds after
// they join. "advert" lines in tfc_teams.ini replace the built-in tips.
static int   g_advertTurn = 0;
#define TT_MAX_GREETED 512
static char  g_greeted[TT_MAX_GREETED][40];
static int   g_greetedCount = 0;

static void TT_JoinTip(edict_t *e)
{
	const char *auth = GETPLAYERAUTHID(e);
	char id[40];
	if (auth && auth[0] && strcasecmp(auth, "STEAM_ID_LAN") && strcasecmp(auth, "STEAM_ID_PENDING"))
		_snprintf_wc(id, sizeof(id) - 1, "%s", auth);
	else
		_snprintf_wc(id, sizeof(id) - 1, "NAME:%s", STRING(e->v.netname));
	id[sizeof(id) - 1] = 0;
	for (int k = 0; k < g_greetedCount; k++)
		if (!strcasecmp(g_greeted[k], id))
			return; // already told this session
	if (g_greetedCount < TT_MAX_GREETED)
		strcpy(g_greeted[g_greetedCount++], id);
	if (g_tt.voteEnabled)
		TT_Say(e, "%s Teams uneven? Say !scramble to vote to scramble them. !teams shows the team counts.", TT_TAG);
	if (g_st.enabled)
		TT_Say(e, "%s Say !stats for your stats, !rank for your rank and !top10 for the best players.", TT_TAG);
}

static void TT_AdvertFrame(float now)
{
	if (g_tt.advertJoin > 0)
		for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
		{
			TTPlayer &s = g_pl[i];
			if (!s.inGame || s.joinTipDone || now - s.connectedAt < g_tt.advertJoin)
				continue;
			s.joinTipDone = true;
			edict_t *e = TT_Player(i);
			if (e && !TT_IsBot(e) && !TT_IsHLTV(e))
				TT_JoinTip(e);
		}
	if (g_tt.advertInterval <= 0 || now < g_nextAdvert)
		return;
	g_nextAdvert = now + g_tt.advertInterval;
	if (g_active)
		return; // a scramble is under way
	bool humans = false;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		edict_t *e = TT_Player(i);
		if (e && !TT_IsBot(e) && !TT_IsHLTV(e))
			humans = true;
	}
	if (!humans)
		return;
	if (g_tt.advertCount)
	{
		TT_SayAll("%s", g_tt.adverts[g_advertTurn++ % g_tt.advertCount]);
		return;
	}
	// Built-in tips: whichever of the two apply, taking turns.
	const char *tips[2];
	int n = 0;
	if (g_tt.voteEnabled)
		tips[n++] = "Teams stacked? Say !scramble to vote to scramble them. !teams shows the team counts and votes.";
	if (g_st.enabled)
		tips[n++] = "Say !stats for your stats (!stats <class> for one class), !rank for your rank, !top10 for the best players.";
	if (n)
		TT_SayAll("%s %s", TT_TAG, tips[g_advertTurn++ % n]);
}

void TT_ScrambleFrame(void)
{
	float now = gpGlobals->time;
	TT_AdvertFrame(now);

	if (g_active && g_resetAt > 0)
	{
		TT_ResetFrame(now);
		return;
	}

	if (g_active)
	{
		int left = 0;
		for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
		{
			TTPlayer &s = g_pl[i];
			if (!s.pendingTeam)
				continue;
			edict_t *e = TT_Player(i);
			if (!e)
			{
				s.pendingTeam = 0;
				continue;
			}
			int cur = (int)e->v.team;
			if (cur == s.pendingTeam || cur != s.pendingFrom)
			{
				// Got there some other way, or went somewhere else on their own.
				TT_Trace("Scramble: %s pending move dropped (now on %d, planned %d->%d)",
					STRING(e->v.netname), cur, s.pendingFrom, s.pendingTeam);
				s.pendingTeam = 0;
				continue;
			}
			if (TT_MovableNow(e) && now >= s.nextMoveTry)
			{
				int to = s.pendingTeam;
				if (TT_MovePlayer(e, to, "scramble"))
				{
					s.pendingTeam = 0;
					TT_Center(e, "Scramble: you are now on %s", TT_TeamName(to));
					continue;
				}
			}
			left++;
		}
		if (left == 0)
		{
			g_active = false;
			g_lastEnd = now;
			TT_SayAll("%s Scramble complete.", TT_TAG);
			TT_Trace("Scramble complete after %.1fs", now - g_startedAt);
		}
		else if (now - g_startedAt >= g_tt.scrambleMaxWait)
		{
			for (int i = 1; i <= TT_MAX_PLAYERS; i++)
				g_pl[i].pendingTeam = 0;
			g_active = false;
			g_lastEnd = now;
			TT_SayAll("%s Scramble finished; %d player%s never respawned in time and stay put.",
				TT_TAG, left, left == 1 ? "" : "s");
			TT_Trace("Scramble timed out with %d moves still pending", left);
		}
		return;
	}

	if (now < g_nextCheck)
		return;
	g_nextCheck = now + 1.0f;

	// Votes of players who went AFK or lost eligibility come off.
	bool anyVotes = false;
	for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
	{
		if (!g_pl[i].voted)
			continue;
		edict_t *e = TT_Player(i);
		if (!TT_VoteEligible(e, i))
		{
			g_pl[i].voted = false;
			if (e && g_pl[i].afk)
				TT_Say(e, "%s Your !scramble vote was removed because you went AFK. Say !scramble again when you are back.", TT_TAG);
			TT_Trace("Vote: %s's vote removed (%s)", e ? STRING(e->v.netname) : "?", e && g_pl[i].afk ? "AFK" : "no longer eligible");
			continue;
		}
		anyVotes = true;
	}
	if (!anyVotes)
		return;

	int votes, needed, pop;
	TT_VoteTally(&votes, &needed, &pop);
	if (votes >= needed)
	{
		char buf[128];
		if (TT_VoteBlockedReason(buf, sizeof(buf)))
			return; // cannot start right now - votes wait
		TT_Trace("Vote: threshold reached (%d/%d of %d eligible)", votes, needed, pop);
		char who[48];
		_snprintf_wc(who, sizeof(who) - 1, "vote %d/%d", votes, needed);
		who[sizeof(who) - 1] = 0;
		TT_ScrambleStart(g_tt.scrambleMode, who);
	}
}

// ---------------------------------------------------------------------------
// Chat
void TT_PrintStatus(edict_t *to)
{
	int counts[TT_MAX_TEAMS + 1];
	float scores[TT_MAX_TEAMS + 1];
	TT_TeamCounts(counts, scores, 0, false);
	char line[180] = "";
	size_t used = 0;
	for (int k = 0; k < g_map.playableCount; k++)
	{
		int t = g_map.playable[k];
		int w = _snprintf_wc(line + used, sizeof(line) - used - 1, "%s%s %d (%.0f)",
			k ? " | " : "", TT_TeamName(t), counts[t], scores[t]);
		if (w < 0)
			break;
		used += (size_t)w;
		if (used >= sizeof(line) - 1)
			break;
	}
	line[sizeof(line) - 1] = 0;
	TT_Say(to, "%s %s", TT_TAG, g_map.playableCount ? line : "no playable teams on this map");

	int votes, needed, pop;
	TT_VoteTally(&votes, &needed, &pop);
	if (g_active)
	{
		int left = 0;
		for (int i = 1; i <= TT_MAX_PLAYERS; i++)
			if (g_pl[i].pendingTeam)
				left++;
		TT_Say(to, "%s Scramble under way: %d move%s still waiting for a respawn.", TT_TAG, left, left == 1 ? "" : "s");
	}
	else
	{
		TT_Say(to, "%s Scramble votes: %d of %d needed (%d eligible). Mode on this map: %s.", TT_TAG, votes, needed, pop,
			g_tt.scrambleMode == TT_SCR_RESET ? "reset (everyone to spectator, then placed)" : "respawn (moved at next death)");
		if (g_tt.pickMode == TT_PICK_RANDOM)
			TT_Say(to, "%s Teams are picked at random here, not by rank.", TT_TAG);
		else if (g_tt.pickMode == TT_PICK_MIXED)
			TT_Say(to, "%s Teams are picked by rank, then each swap rolls a %d%% chance.", TT_TAG, g_tt.pickMixedChance);
	}
}

static void TT_CastVote(edict_t *p)
{
	int idx = ENTINDEX(p);
	char buf[128];
	const char *blocked = TT_VoteBlockedReason(buf, sizeof(buf));
	if (blocked)
	{
		TT_Say(p, "%s %s", TT_TAG, blocked);
		return;
	}
	if (!TT_VoteEligible(p, idx))
	{
		TT_Say(p, "%s Only players on a team can vote to scramble.", TT_TAG);
		return;
	}
	int votes, needed, pop;
	if (g_pl[idx].voted)
	{
		TT_VoteTally(&votes, &needed, &pop);
		TT_Say(p, "%s You already voted. %d of %d needed.", TT_TAG, votes, needed);
		return;
	}
	g_pl[idx].voted = true;
	TT_VoteTally(&votes, &needed, &pop);
	TT_Trace("Vote: %s voted (%d/%d of %d)", STRING(p->v.netname), votes, needed, pop);
	if (votes >= needed)
	{
		TT_SayAll("%s %s voted to scramble - that's enough votes (%d/%d).", TT_TAG, STRING(p->v.netname), votes, needed);
		char who[48];
		_snprintf_wc(who, sizeof(who) - 1, "vote %d/%d", votes, needed);
		who[sizeof(who) - 1] = 0;
		TT_ScrambleStart(g_tt.scrambleMode, who);
	}
	else
		TT_SayAll("%s %s wants to scramble the teams (%d/%d). Say !scramble to agree.",
			TT_TAG, STRING(p->v.netname), votes, needed);
}

static bool TT_Is(const char *text, const char *word)
{
	// "!word", "/word" or "word", any case.
	if (text[0] == '!' || text[0] == '/')
		text++;
	return !strcasecmp(text, word);
}

bool TT_ChatCommand(edict_t *p, const char *raw)
{
	if (!g_tt.enabled || FNullEnt(p) || !raw)
		return false;

	char text[128];
	strncpy(text, raw, sizeof(text) - 1);
	text[sizeof(text) - 1] = 0;
	char *s = TrimInPlace(text);
	size_t l = strlen(s);
	if (l >= 2 && s[0] == '"' && s[l - 1] == '"')
	{
		s[l - 1] = 0;
		s = TrimInPlace(s + 1);
	}
	if (!s[0])
		return false;

	char word[64];
	char *rest = SplitFirstToken(s, word, sizeof(word));
	rest = TrimInPlace(rest);

	if (TT_StatsChat(p, word, rest))
		return false; // show the line in chat, like !rank on AMXX servers

	if (TT_Is(word, "scramble") || TT_Is(word, "votescramble"))
	{
		TT_CastVote(p);
		return false; // let the line show in chat, like !rtv
	}
	// Everything below needs the ! or / in front, so ordinary chat that
	// happens to be one of these words is left alone.
	if (word[0] != '!' && word[0] != '/')
		return false;
	if (TT_Is(word, "teams"))
	{
		TT_PrintStatus(p);
		return false;
	}
	if (TT_Is(word, "names"))
		return TT_NamesChat(p, rest); // hidden from chat: an admin's lookup
	if (TT_Is(word, "forcescramble") || TT_Is(word, "cancelscramble"))
	{
		if (!TT_IsAdmin(p))
		{
			TT_Say(p, "%s That command is for admins.", TT_TAG);
			return true;
		}
		char who[64];
		_snprintf_wc(who, sizeof(who) - 1, "admin %s", STRING(p->v.netname));
		who[sizeof(who) - 1] = 0;
		if (TT_Is(word, "cancelscramble"))
		{
			bool wasActive = g_active;
			TT_ScrambleCancel(who);
			if (!wasActive)
				TT_Say(p, "%s No scramble was running; scramble votes cleared.", TT_TAG);
			return true;
		}
		if (g_active)
		{
			TT_Say(p, "%s A scramble is already under way (!cancelscramble to stop it).", TT_TAG);
			return true;
		}
		int mode = !strcasecmp(rest, "now") ? TT_SCR_NOW
		         : !strcasecmp(rest, "reset") ? TT_SCR_RESET
		         : !strcasecmp(rest, "respawn") ? TT_SCR_RESPAWN : g_tt.scrambleMode;
		TT_SayAll("%s %s forced a team scramble.", TT_TAG, STRING(p->v.netname));
		TT_ScrambleStart(mode, who);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Server console commands (also reachable through rcon and AMXX's amx_rcon).
static edict_t *TT_FindPlayerArg(const char *arg)
{
	if (!arg || !arg[0])
		return NULL;
	if (arg[0] == '#')
	{
		int uid = atoi(arg + 1);
		for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
		{
			edict_t *e = TT_Player(i);
			if (e && GETPLAYERUSERID(e) == uid)
				return e;
		}
		return NULL;
	}
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
				return NULL; // ambiguous
			found = e;
		}
	}
	return found;
}

static int TT_TeamArg(const char *arg)
{
	if (!arg || !arg[0])
		return 0;
	if (arg[0] >= '1' && arg[0] <= '4' && !arg[1])
		return arg[0] - '0';
	for (int t = 1; t <= TT_MAX_TEAMS; t++)
		if (!strcasecmp(arg, TT_TeamName(t)))
			return t;
	return 0;
}

static void TT_Cmd_Scramble(void)
{
	if (g_active)
	{
		SERVER_PRINT("[Teams] A scramble is already under way (tt_cancel to stop it).\n");
		return;
	}
	const char *a = CMD_ARGC() > 1 ? CMD_ARGV(1) : "";
	int mode = !strcasecmp(a, "now") ? TT_SCR_NOW
	         : !strcasecmp(a, "reset") ? TT_SCR_RESET
	         : !strcasecmp(a, "respawn") ? TT_SCR_RESPAWN : g_tt.scrambleMode;
	TT_SayAll("%s The server forced a team scramble.", TT_TAG);
	TT_ScrambleStart(mode, "server console");
}

static void TT_Cmd_Cancel(void)  { TT_ScrambleCancel("the server"); }
static void TT_Cmd_Status(void)  { TT_PrintStatus(NULL); }

static void TT_Cmd_Reload(void)
{
	TT_ConfigLoad();
	for (int i = 0; i <= TT_MAX_PLAYERS; i++)
		g_pl[i].adminKnown = false;
	SERVER_PRINT("[Teams] tfc_teams.ini reloaded.\n");
}

static void TT_Cmd_Move(void)
{
	if (CMD_ARGC() < 3)
	{
		SERVER_PRINT("Usage: tt_move <name|#userid> <1-4|team name>   (moves them now - a living player dies, as with changeteam)\n");
		return;
	}
	edict_t *e = TT_FindPlayerArg(CMD_ARGV(1));
	int team = TT_TeamArg(CMD_ARGV(2));
	if (!e)
	{
		SERVER_PRINT("[Teams] No single player matches that.\n");
		return;
	}
	if (!TT_TeamPlayable(team))
	{
		SERVER_PRINT("[Teams] That is not a playable team on this map.\n");
		return;
	}
	if (TT_MovePlayer(e, team, "tt_move"))
		TT_SayAll("%s %s was moved to %s by the server.", TT_TAG, STRING(e->v.netname), TT_TeamName(team));
	else
		SERVER_PRINT("[Teams] The game refused the move (dying right now, just changed team, or team full) - see tt_trace.log.\n");
}

void TT_RegisterServerCommands(void)
{
	REG_SVR_COMMAND("tt_scramble", TT_Cmd_Scramble);
	REG_SVR_COMMAND("tt_cancel",   TT_Cmd_Cancel);
	REG_SVR_COMMAND("tt_status",   TT_Cmd_Status);
	REG_SVR_COMMAND("tt_reload",   TT_Cmd_Reload);
	REG_SVR_COMMAND("tt_move",     TT_Cmd_Move);
}
