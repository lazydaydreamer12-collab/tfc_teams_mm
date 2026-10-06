// tt_net.h - posting the end-of-map awards to a Discord channel (a webhook).
// See tt_net.cpp (sending), tt_secret.cpp (where the webhook is kept) and
// tt_feed.cpp (the message).
#ifndef TFC_TEAMS_NET_H
#define TFC_TEAMS_NET_H

#include <stddef.h>

// ---------------------------------------------------------------------------
// Settings (tfc_teams.ini). The webhook itself is NOT a setting: it is set with
// the tt_discord console command (or tt_secret_import.txt) and kept encrypted.
struct TTNetConfig
{
	int   enabled;          // net_enabled: post the end-of-map awards to Discord
	char  serverName[64];   // shown in Discord; empty = the hostname cvar
	char  caFile[256];      // Linux: certificate bundle (empty = find the system one)
};
extern TTNetConfig g_net;
void TT_NetConfigDefaults(void);
bool TT_NetConfigKey(const char *key, const char *val);   // true = it was one of ours

// ---------------------------------------------------------------------------
// The stored webhook - tt_secret.cpp
struct TTSecret
{
	char discord[512];     // https://discord.com/api/webhooks/<id>/<token>
};
extern TTSecret g_secret;
void TT_SecretLoad(void);                 // also takes in tt_secret_import.txt if present
bool TT_SecretSave(void);
void TT_SecretRegisterCommands(void);
void TT_SecretMask(const char *url, char *out, size_t len);   // "https://discord.com/api/webhooks/1234/****"
const char *TT_SecretKeySource(void);
void TT_Printf(const char *fmt, ...);     // server console (and the rcon reply)

// ---------------------------------------------------------------------------
// Sending - tt_net.cpp. Requests run on a worker thread so the game never
// waits on the network.
void TT_NetInit(void);
void TT_NetShutdown(void);
void TT_NetFrame(void);                   // results back from the worker
bool TT_NetSend(const char *body);        // a Discord webhook message (JSON); body is copied
void TT_NetPrintStatus(void);             // tt_net
void TT_NetTestStarted(void);             // tt_net_test: report the result to admins in game
const char *TT_NetServerName(void);

// ---------------------------------------------------------------------------
// Growable text buffer (no std::string: the Linux build has no C++ runtime).
struct TTBuf
{
	char  *p;
	size_t len, cap;
	TTBuf() : p(0), len(0), cap(0) {}
	~TTBuf();
	void add(const char *s);
	void addn(const char *s, size_t n);
	void addf(const char *fmt, ...);
	void json(const char *s);             // a JSON string, quotes included
	void discord(const char *s);          // text with Discord's markdown characters escaped
	void clear() { len = 0; if (p) p[0] = 0; }
	const char *c_str() const { return p ? p : ""; }
};

// ---------------------------------------------------------------------------
// The end-of-map post - tt_feed.cpp
struct TTFeedPlayer
{
	char  name[32];
	char  key[48];
	bool  bot;
	int   team;
	int   kills, deaths, caps;
	float points;
};
struct TTFeedAward { char title[40]; char name[32]; float value; char unit[12]; };
struct TTFeedMapEnd
{
	char   map[64];
	int    teamScore[5];
	int    awardCount;
	TTFeedAward awards[16];
	char   rivalA[32], rivalB[32];
	int    rivalAB, rivalBA;
	int    playerCount;
	TTFeedPlayer players[32];
};

void TT_FeedFrame(void);
void TT_FeedMapEnd(const TTFeedMapEnd &m);
void TT_FeedTest(void);                   // tt_net_test

#endif
