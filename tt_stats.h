// tt_stats.h - per-SteamID player statistics and the skill rating the
// spectator-reset scramble balances on. See tt_stats.cpp.
#ifndef TFC_TEAMS_STATS_H
#define TFC_TEAMS_STATS_H

struct TTStatsConfig
{
	int   enabled;
	float minMapMinutes;   // on a team this long before a map counts toward the rating
	float minRankMinutes;  // lifetime minutes before someone appears in !rank / !top10
	float saveInterval;    // seconds between saves of tt_stats.txt
	int   summary;         // end-of-map summary in chat
	float wKill, wCarrierKill, wCap, wPickup, wTeamkill;   // rating points
	float wHeal;                                         // rating points per 100 health healed (medic)
	float newRating;       // rating for someone never seen (0 = median of known players)
	int   window;          // !stats / !top10: 0 chat lines, 1 window (MOTD panel), 2 paged menu
	float menuTime;        // seconds the paged menu stays up (0 = until closed)
	int   debugCaps;       // CAPDBG lines in tt_trace.log
	char  capWord[24];     // a goal whose name contains this, reached with an item, is a capture
	char  returnWord[24];  // ...unless its name (or one activated with it) contains this
	int   rivals;          // head-to-head: who kills whom (!rival, rivalry of the map)
};
extern TTStatsConfig g_st;
void TT_StatsConfigDefaults(void);

// Lifetime database: loaded at attach, saved at every map end and every few
// minutes (stats_save_interval).
void TT_StatsInit(void);
void TT_StatsShutdown(void);

void TT_StatsMapStart(void);
void TT_StatsMapEnd(void);                 // ServerDeactivate
void TT_StatsPlayerConnect(edict_t *p);
void TT_StatsPlayerDisconnect(edict_t *p);
void TT_StatsFrame(void);                  // StartFrame
void TT_StatsPreThink(edict_t *p);         // before TFC's PreThink clears dmg_take

// Engine-side events (tt_engine.cpp forwards these).
void TT_StatsLogLine(const char *line);    // every UTIL_LogPrintf line
void TT_StatsEventPrecached(const char *name, unsigned short index);
void TT_StatsEventPlayed(const edict_t *invoker, unsigned short index);
void TT_StatsPrivateText(edict_t *to, const char *msg);   // TextMsg to one player (#Sniper_headshot)
void TT_StatsTeamScore(const char *team, int score);
void TT_StatsIntermission(void);
void TT_StatsBroadcastText(const char *a, const char *b);  // TextMsg to everyone (debug)

// While a move of ours is running, the suicide TeamSet logs is not the
// player's doing and is not counted.
void TT_StatsIgnoreDeathsOf(edict_t *p);   // NULL = stop ignoring

// The rating the reset scramble deals players by.
float TT_StatsRating(edict_t *p);

// Chat: !stats [name|class], !rank, !top10, !weapons [name], !awards. true = ours.
bool TT_StatsChat(edict_t *p, const char *word, const char *rest);

// The paged stats / rankings menu: "menuselect <key>" from the client (true =
// it was for our menu), and another menu taking the screen (NULL = everyone's).
bool TT_StatsMenuSelect(edict_t *p, int key);
void TT_StatsMenuGone(edict_t *p);

void TT_StatsRegisterCommands(void);

// For the name tracker.
void TT_StatsKeyOf(edict_t *p, char *out, size_t len);   // SteamID key, "BOT:<name>", "" = not known yet
void TT_StatsMapNumbers(int idx, int *kills, int *deaths, int *caps, float *points);
int  TT_StatsTeamScoreOf(int team);

#endif
