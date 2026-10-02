// tt_engine.cpp - engine-side glue: running the game's own commands for a
// player, hiding the class menu while we restore a class, chat output, and
// the trace log.
//
// FAKE CLIENT COMMANDS. The game DLL's ClientCommand() takes no command
// string - it reads the command back out of the engine with CMD_ARGV(),
// CMD_ARGC() and CMD_ARGS(). We hook those three and, only while our own call
// is on the stack, answer them with the command we want, then call the game's
// ClientCommand directly (MDLL_ClientCommand). The game cannot tell it from a
// typed command. This is how Weapon Changer Plus switches bots' weapons, and
// the same technique AMX Mod X uses for amx_client_cmd. CLIENT_COMMAND() is no
// use here: it asks the CLIENT to send the command back, which a bot never
// does and a human does a network round trip later.
//
// Because MDLL_ClientCommand goes straight to the game DLL, no metamod
// plugin's ClientCommand hook sees these commands - including our own
// "jointeam" block, which is what we want.
//
// HIDING THE CLASS MENU. TeamSet ends with Menu_Class(), which sends the
// VGUIMenu user message (id 3) to the player (tfc.so 0x96cd0). We give the
// class back one call later, so a menu popping up would be asking a question
// that has already been answered - and picking from it would queue a second
// class change. While a move is in progress, any VGUIMenu to that one player
// is swallowed. If the class could NOT be restored (the new team is at its
// limit for that class), TT_MovePlayer sends the class menu itself so the
// player can choose.

#include <extdll.h>
#include <meta_api.h>
#include <dllapi.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include "tt_common.h"
#include "tt_stats.h"

// After tt_common.h: extdll.h has already pulled windows.h in with
// WIN32_LEAN_AND_MEAN, so this is a no-op there, as in wc_trace.cpp.
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define TT_FAKE_MAX_ARGV 3
#define TT_FAKE_ARG_LEN  64

static bool g_fakeActive = false;
static char g_fakeArgv[TT_FAKE_MAX_ARGV][TT_FAKE_ARG_LEN];
static char g_fakeArgs[TT_FAKE_ARG_LEN * TT_FAKE_MAX_ARGV];
static int  g_fakeArgc = 0;

static const char *TT_Cmd_Args(void)
{
	if (g_fakeActive)
		RETURN_META_VALUE(MRES_SUPERCEDE, g_fakeArgs);
	RETURN_META_VALUE(MRES_IGNORED, NULL);
}

static const char *TT_Cmd_Argv(int argc)
{
	if (g_fakeActive)
	{
		if (argc >= 0 && argc < g_fakeArgc)
			RETURN_META_VALUE(MRES_SUPERCEDE, g_fakeArgv[argc]);
		RETURN_META_VALUE(MRES_SUPERCEDE, "");
	}
	RETURN_META_VALUE(MRES_IGNORED, NULL);
}

static int TT_Cmd_Argc(void)
{
	if (g_fakeActive)
		RETURN_META_VALUE(MRES_SUPERCEDE, g_fakeArgc);
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

void TT_FakeClientCommand(edict_t *p, const char *cmd, const char *arg1)
{
	if (FNullEnt(p) || !cmd || !cmd[0] || g_fakeActive)
		return;

	strncpy(g_fakeArgv[0], cmd, TT_FAKE_ARG_LEN - 1);
	g_fakeArgv[0][TT_FAKE_ARG_LEN - 1] = 0;
	g_fakeArgc = 1;
	g_fakeArgs[0] = 0;
	if (arg1 && arg1[0])
	{
		strncpy(g_fakeArgv[1], arg1, TT_FAKE_ARG_LEN - 1);
		g_fakeArgv[1][TT_FAKE_ARG_LEN - 1] = 0;
		strncpy(g_fakeArgs, arg1, sizeof(g_fakeArgs) - 1);
		g_fakeArgs[sizeof(g_fakeArgs) - 1] = 0;
		g_fakeArgc = 2;
	}

	g_fakeActive = true;
	MDLL_ClientCommand(p);
	g_fakeActive = false;
}

// ---------------------------------------------------------------------------
// VGUIMenu suppression.
static edict_t *g_suppressFor = NULL;
static bool     g_swallowing = false;
static int      g_msgVGUIMenu = 0;

static int TT_VGUIMenuId(void)
{
	// User messages are registered during the game's own startup, so this is
	// resolved on first use rather than at attach.
	if (g_msgVGUIMenu <= 0)
		g_msgVGUIMenu = GET_USER_MSG_ID(PLID, "VGUIMenu", NULL);
	return g_msgVGUIMenu;
}

void TT_SuppressVGUIMenus(edict_t *p)
{
	g_suppressFor = p;
	if (!p)
		g_swallowing = false;
}

void TT_ShowClassMenu(edict_t *p)
{
	int id = TT_VGUIMenuId();
	if (FNullEnt(p) || id <= 0)
		return;
	MESSAGE_BEGIN(MSG_ONE, id, NULL, p);
	WRITE_BYTE(3); // the class menu, as Menu_Class() sends it
	MESSAGE_END();
}

// TeamScore capture, for flag-cap detection (tt_stats.cpp): the message is
// WRITE_STRING(team name), WRITE_SHORT(score), ... (TeamFortress_TeamIncreaseScore,
// tfc.so 0x1046e0). And SVC_INTERMISSION, which CTeamFortress::GoToIntermission
// (0xe92a0) sends to everyone when the map ends.
static bool g_capTeamScore = false;
static char g_tsName[64];
static int  g_tsScore = 0, g_tsShorts = 0;

// Broadcast TextMsg capture, for the CAPDBG trace (flag messages such as
// "%s CAPTURED the BLUE flag!" go out this way). Our own messages are skipped.
static bool g_capText = false;
// TextMsg to ONE player: TFC tells a sniper "#Sniper_headshot" from inside
// CBasePlayer::TraceAttack (tfc.so 0xc55a8) exactly when it doubles the
// damage for a head hit - that message is the headshot.
static bool     g_capPrivText = false;
static edict_t *g_privTo = NULL;
static bool g_sendingOwn = false;
static char g_txt[2][128];
static int  g_txtN = 0;
static int TT_TextMsgId(void);
static int TT_VGUIMenuId(void);
static int TT_ShowMenuId(void)
{
	static int id = 0;
	if (id <= 0)
		id = GET_USER_MSG_ID(PLID, "ShowMenu", NULL);
	return id;
}

static void TT_MessageBegin(int msg_dest, int msg_type, const float *pOrigin, edict_t *ed)
{
	static int msgTeamScore = 0;
	if (msgTeamScore <= 0)
		msgTeamScore = GET_USER_MSG_ID(PLID, "TeamScore", NULL);
	g_capTeamScore = (msgTeamScore > 0 && msg_type == msgTeamScore);
	if (g_capTeamScore)
	{
		g_tsName[0] = 0;
		g_tsScore = g_tsShorts = 0;
	}
	if (msg_type == SVC_INTERMISSION)
		TT_StatsIntermission();
	// Someone else's menu (TFC's, AMX Mod X's) replaces ours on that screen -
	// from then on the number keys are theirs.
	if (!g_sendingOwn && (msg_type == TT_ShowMenuId() || msg_type == TT_VGUIMenuId()))
	{
		if (ed && (msg_dest == MSG_ONE || msg_dest == MSG_ONE_UNRELIABLE))
			TT_StatsMenuGone(ed);
		else if (msg_dest == MSG_ALL || msg_dest == MSG_BROADCAST)
			TT_StatsMenuGone(NULL);
	}
	g_capText = (!g_sendingOwn && g_st.debugCaps && msg_type == TT_TextMsgId()
		&& (msg_dest == MSG_ALL || msg_dest == MSG_BROADCAST));
	if (g_capText)
	{
		g_txt[0][0] = g_txt[1][0] = 0;
		g_txtN = 0;
	}
	g_capPrivText = (!g_sendingOwn && ed && msg_type == TT_TextMsgId()
		&& (msg_dest == MSG_ONE || msg_dest == MSG_ONE_UNRELIABLE));
	g_privTo = g_capPrivText ? ed : NULL;

	if (g_suppressFor && ed == g_suppressFor && msg_type == TT_VGUIMenuId()
		&& (msg_dest == MSG_ONE || msg_dest == MSG_ONE_UNRELIABLE))
	{
		g_swallowing = true;
		RETURN_META(MRES_SUPERCEDE);
	}
	RETURN_META(MRES_IGNORED);
}

static void TT_MessageEnd(void)
{
	if (g_capTeamScore)
	{
		g_capTeamScore = false;
		TT_StatsTeamScore(g_tsName, g_tsScore);
	}
	if (g_capText)
	{
		g_capText = false;
		TT_StatsBroadcastText(g_txt[0], g_txt[1]);
	}
	g_capPrivText = false;
	if (g_swallowing)
	{
		g_swallowing = false;
		RETURN_META(MRES_SUPERCEDE);
	}
	RETURN_META(MRES_IGNORED);
}

static unsigned short TT_FixedUnsigned16(float value, float scale)
{
	int output = (int)(value * scale);
	if (output < 0) output = 0;
	if (output > 0xFFFF) output = 0xFFFF;
	return (unsigned short)output;
}

#define TT_SWALLOW() do { if (g_swallowing) RETURN_META(MRES_SUPERCEDE); RETURN_META(MRES_IGNORED); } while (0)
static void TT_WriteByte(int v)          { TT_SWALLOW(); }
static void TT_WriteChar(int v)          { TT_SWALLOW(); }
static void TT_WriteShort(int v)
{
	if (g_capTeamScore && g_tsShorts++ == 0)
		g_tsScore = (short)v;
	TT_SWALLOW();
}
static void TT_WriteLong(int v)          { TT_SWALLOW(); }
static void TT_WriteAngle(float v)       { TT_SWALLOW(); }
static void TT_WriteCoord(float v)       { TT_SWALLOW(); }
static void TT_WriteString(const char *s)
{
	if (g_capTeamScore && s && !g_tsName[0])
	{
		strncpy(g_tsName, s, sizeof(g_tsName) - 1);
		g_tsName[sizeof(g_tsName) - 1] = 0;
	}
	if (g_capPrivText && s && s[0] == '#')
	{
		g_capPrivText = false; // only the first string is the message
		TT_StatsPrivateText(g_privTo, s);
	}
	if (g_capText && s && g_txtN < 2)
	{
		strncpy(g_txt[g_txtN], s, sizeof(g_txt[0]) - 1);
		g_txt[g_txtN][sizeof(g_txt[0]) - 1] = 0;
		g_txtN++;
	}
	TT_SWALLOW();
}

// Every UTIL_LogPrintf line of the game arrives here as at_logged (metamod
// hands plugins the already-formatted text as ("%s", text)).
static void TT_AlertMessage(ALERT_TYPE atype, char *szFmt, ...)
{
	if (atype == at_logged && szFmt)
	{
		char buf[512];
		va_list ap;
		va_start(ap, szFmt);
		vsnprintf(buf, sizeof(buf), szFmt, ap);
		va_end(ap);
		buf[sizeof(buf) - 1] = 0;
		TT_StatsLogLine(buf);
	}
	RETURN_META(MRES_IGNORED);
}

// One call per weapon shot (and per other event - only weapon events count).
static void TT_PlaybackEvent(int flags, const edict_t *pInvoker, unsigned short eventindex,
	float delay, float *origin, float *angles, float fparam1, float fparam2,
	int iparam1, int iparam2, int bparam1, int bparam2)
{
	TT_StatsEventPlayed(pInvoker, eventindex);
	RETURN_META(MRES_IGNORED);
}

// Post: the event's index is the engine's return value.
static unsigned short TT_PrecacheEvent_Post(int type, const char *psz)
{
	TT_StatsEventPrecached(psz, META_RESULT_ORIG_RET(unsigned short));
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

static void TT_WriteEntity(int v)        { TT_SWALLOW(); }

static enginefuncs_t gEngineFunctions;

C_DLLEXPORT FORCE_STACK_ALIGN int GetEngineFunctions(enginefuncs_t *pengfuncsFromEngine,
		int *interfaceVersion)
{
	if (!pengfuncsFromEngine)
	{
		LOG_ERROR(PLID, "GetEngineFunctions called with null pengfuncsFromEngine");
		return FALSE;
	}
	else if (*interfaceVersion != ENGINE_INTERFACE_VERSION)
	{
		LOG_ERROR(PLID, "GetEngineFunctions version mismatch; requested=%d ours=%d",
			*interfaceVersion, ENGINE_INTERFACE_VERSION);
		*interfaceVersion = ENGINE_INTERFACE_VERSION;
		return FALSE;
	}
	// By name: the table has ~150 slots and a miscounted positional
	// initialiser would silently hook the wrong function.
	memset(&gEngineFunctions, 0, sizeof(gEngineFunctions));
	gEngineFunctions.pfnCmd_Args     = TT_Cmd_Args;
	gEngineFunctions.pfnCmd_Argv     = TT_Cmd_Argv;
	gEngineFunctions.pfnCmd_Argc     = TT_Cmd_Argc;
	gEngineFunctions.pfnMessageBegin = TT_MessageBegin;
	gEngineFunctions.pfnMessageEnd   = TT_MessageEnd;
	gEngineFunctions.pfnWriteByte    = TT_WriteByte;
	gEngineFunctions.pfnWriteChar    = TT_WriteChar;
	gEngineFunctions.pfnWriteShort   = TT_WriteShort;
	gEngineFunctions.pfnWriteLong    = TT_WriteLong;
	gEngineFunctions.pfnWriteAngle   = TT_WriteAngle;
	gEngineFunctions.pfnWriteCoord   = TT_WriteCoord;
	gEngineFunctions.pfnWriteString  = TT_WriteString;
	gEngineFunctions.pfnWriteEntity  = TT_WriteEntity;
	gEngineFunctions.pfnAlertMessage = TT_AlertMessage;
	gEngineFunctions.pfnPlaybackEvent = TT_PlaybackEvent;
	memcpy(pengfuncsFromEngine, &gEngineFunctions, sizeof(enginefuncs_t));
	return TRUE;
}

static enginefuncs_t gEngineFunctions_Post;

C_DLLEXPORT FORCE_STACK_ALIGN int GetEngineFunctions_Post(enginefuncs_t *pengfuncsFromEngine,
		int *interfaceVersion)
{
	if (!pengfuncsFromEngine)
		return FALSE;
	if (*interfaceVersion != ENGINE_INTERFACE_VERSION)
	{
		*interfaceVersion = ENGINE_INTERFACE_VERSION;
		return FALSE;
	}
	memset(&gEngineFunctions_Post, 0, sizeof(gEngineFunctions_Post));
	gEngineFunctions_Post.pfnPrecacheEvent = TT_PrecacheEvent_Post;
	memcpy(pengfuncsFromEngine, &gEngineFunctions_Post, sizeof(enginefuncs_t));
	return TRUE;
}

// ---------------------------------------------------------------------------
// Chat and centre text, through the game's own TextMsg message - the one
// TFC's ClientPrint() uses, so it looks like every other game message.
#define TT_HUD_PRINTTALK   3
#define TT_HUD_PRINTCENTER 4

static int TT_TextMsgId(void)
{
	// (declared above for the TextMsg capture)
	static int id = 0;
	if (id <= 0)
		id = GET_USER_MSG_ID(PLID, "TextMsg", NULL);
	return id;
}

static void TT_SendText(edict_t *p, int dest, const char *text)
{
	int id = TT_TextMsgId();
	if (id <= 0 || !text || !text[0])
		return;
	// A leading '#' would make the client look it up as a titles.txt key.
	char buf[190];
	_snprintf_wc(buf, sizeof(buf) - 1, "%s%s\n", text[0] == '#' ? " " : "", text);
	buf[sizeof(buf) - 1] = 0;
	if (p)
	{
		if (FNullEnt(p) || TT_IsBot(p))
			return;
		MESSAGE_BEGIN(MSG_ONE, id, NULL, p);
	}
	else
		MESSAGE_BEGIN(MSG_ALL, id, NULL, (edict_t *)NULL);
	g_sendingOwn = true;
	WRITE_BYTE(dest);
	WRITE_STRING(buf);
	MESSAGE_END();
	g_sendingOwn = false;
}

// A window on the player's screen: TFC's own MOTD panel. The client
// (TeamFortressViewport::MsgFunc_MOTD, client.so 0x8af40) appends each chunk
// to a 1536-byte buffer and, on the chunk flagged final, opens the MOTD
// window (VGUI menu 5) with that text - unless that window is already open,
// in which case it is left alone. A chunk per message: WRITE_BYTE(final),
// WRITE_STRING(text), kept well under the 192-byte user-message limit.
// A numbered menu on the left of the screen: the game's ShowMenu message
// (CHudMenu::MsgFunc_ShowMenu, client.so 0x674d0). SHORT valid keys (bit 0 =
// key 1 ... bit 9 = key 0), CHAR seconds to show (-1 = until closed), BYTE
// "more to come", STRING text - joined into a 512-byte buffer, so a page is
// kept under 500 bytes. \\y \\w \\r \\d set the colour. Picking a key sends
// "menuselect <n>" (see dllapi.cpp).
void TT_ShowMenu(edict_t *p, int keys, int seconds, const char *text)
{
	int id = TT_ShowMenuId();
	if (id <= 0 || FNullEnt(p) || TT_IsBot(p) || !text)
		return;
	size_t len = strlen(text);
	if (len > 500)
		len = 500;
	size_t pos = 0;
	char chunk[176];
	g_sendingOwn = true;
	do
	{
		size_t n = len - pos;
		if (n > sizeof(chunk) - 1)
			n = sizeof(chunk) - 1;
		memcpy(chunk, text + pos, n);
		chunk[n] = 0;
		pos += n;
		MESSAGE_BEGIN(MSG_ONE, id, NULL, p);
		WRITE_SHORT(keys);
		WRITE_CHAR(seconds);
		WRITE_BYTE(pos < len ? 1 : 0);
		WRITE_STRING(chunk);
		MESSAGE_END();
	} while (pos < len);
	g_sendingOwn = false;
}

// Text on the HUD (TE_TEXTMESSAGE, as UTIL_HudMessage builds it). HUD text is
// drawn during intermission too. p = NULL for everyone.
void TT_HudText(edict_t *p, int channel, float x, float y, int r, int g, int b,
	float hold, const char *text)
{
	if (p && (FNullEnt(p) || TT_IsBot(p)))
		return;
	g_sendingOwn = true;
	if (p)
		MESSAGE_BEGIN(MSG_ONE, SVC_TEMPENTITY, NULL, p);
	else
		MESSAGE_BEGIN(MSG_ALL, SVC_TEMPENTITY, NULL, (edict_t *)NULL);
	WRITE_BYTE(TE_TEXTMESSAGE);
	WRITE_BYTE(channel & 0xFF);
	WRITE_SHORT((short)(x < -1 ? -32768 : x * 8192));
	WRITE_SHORT((short)(y < -1 ? -32768 : y * 8192));
	WRITE_BYTE(2);                          // effect: fade in letter by letter
	WRITE_BYTE(r); WRITE_BYTE(g); WRITE_BYTE(b); WRITE_BYTE(255);
	WRITE_BYTE(255); WRITE_BYTE(255); WRITE_BYTE(255); WRITE_BYTE(255); // highlight colour
	WRITE_SHORT(TT_FixedUnsigned16(0.02f, 1 << 8));   // fade in (per character)
	WRITE_SHORT(TT_FixedUnsigned16(1.0f, 1 << 8));    // fade out
	WRITE_SHORT(TT_FixedUnsigned16(hold, 1 << 8));    // hold
	WRITE_SHORT(TT_FixedUnsigned16(0.5f, 1 << 8));    // fx time (effect 2)
	WRITE_STRING(text);
	MESSAGE_END();
	g_sendingOwn = false;
}

void TT_ShowWindow(edict_t *p, const char *text)
{
	static int motd = 0;
	if (motd <= 0)
		motd = GET_USER_MSG_ID(PLID, "MOTD", NULL);
	if (motd <= 0 || FNullEnt(p) || TT_IsBot(p) || !text)
		return;
	size_t len = strlen(text);
	if (len > 1500)
		len = 1500;
	size_t pos = 0;
	char chunk[176];
	do
	{
		size_t n = len - pos;
		if (n > sizeof(chunk) - 1)
			n = sizeof(chunk) - 1;
		memcpy(chunk, text + pos, n);
		chunk[n] = 0;
		pos += n;
		MESSAGE_BEGIN(MSG_ONE, motd, NULL, p);
		WRITE_BYTE(pos >= len ? 1 : 0);
		WRITE_STRING(chunk);
		MESSAGE_END();
	} while (pos < len);
}

void TT_Say(edict_t *p, const char *fmt, ...)
{
	char msg[180];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	msg[sizeof(msg) - 1] = 0;
	if (!p)
	{
		SERVER_PRINT(msg);
		SERVER_PRINT("\n");
		return;
	}
	TT_SendText(p, TT_HUD_PRINTTALK, msg);
}

void TT_SayAll(const char *fmt, ...)
{
	char msg[180];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	msg[sizeof(msg) - 1] = 0;
	TT_SendText(NULL, TT_HUD_PRINTTALK, msg);
	// Chat is gone once it scrolls away; the server log keeps it.
	if (gpMetaUtilFuncs)
		LOG_MESSAGE(PLID, "%s", msg);
}

void TT_Center(edict_t *p, const char *fmt, ...)
{
	char msg[180];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	msg[sizeof(msg) - 1] = 0;
	TT_SendText(p, TT_HUD_PRINTCENTER, msg);
}

// ---------------------------------------------------------------------------
// Trace log. Same arrangement as Weapon Changer Plus's wc_trace.log: beside
// our own DLL (whose folder is known to be writable, because every build
// deploys into it), located by asking the OS where this module was loaded
// from rather than trusting GET_GAME_DIR().
bool TT_ModuleDir(char *out, size_t outLen)
{
#ifdef _WIN32
	HMODULE hModule = NULL;
	if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&TT_ModuleDir, &hModule))
		return false;
	char path[MAX_PATH];
	DWORD len = GetModuleFileNameA(hModule, path, sizeof(path));
	if (len == 0 || len >= sizeof(path))
		return false;
	char *slash = strrchr(path, '\\');
	if (!slash)
		return false;
	*slash = 0;
#else
	Dl_info info;
	if (!dladdr((void *)&TT_ModuleDir, &info) || !info.dli_fname)
		return false;
	char path[512];
	strncpy(path, info.dli_fname, sizeof(path) - 1);
	path[sizeof(path) - 1] = 0;
	char *slash = strrchr(path, '/');
	if (!slash)
		return false;
	*slash = 0;
#endif
	strncpy(out, path, outLen - 1);
	out[outLen - 1] = 0;
	return true;
}

void TT_Trace(const char *fmt, ...)
{
	if (!g_tt.debug)
		return;
	char msg[320];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	msg[sizeof(msg) - 1] = 0;

	float t = gpGlobals ? gpGlobals->time : -1.0f;
	if (gpMetaUtilFuncs)
		LOG_MESSAGE(PLID, "[TEAMS t=%.2f] %s", t, msg);

	char dir[400], path[450];
	if (!TT_ModuleDir(dir, sizeof(dir)))
		return;
#ifdef _WIN32
	_snprintf_wc(path, sizeof(path) - 1, "%s\\tt_trace.log", dir);
#else
	_snprintf_wc(path, sizeof(path) - 1, "%s/tt_trace.log", dir);
#endif
	path[sizeof(path) - 1] = 0;
	FILE *f = fopen(path, "a");
	if (!f)
		return;
	fprintf(f, "[t=%.2f] %s\n", t, msg);
	fclose(f);
}

void TT_LogModuleIdentity(void)
{
#ifdef _WIN32
	HMODULE hModule = NULL;
	char path[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&TT_LogModuleIdentity, &hModule)
		|| !GetModuleFileNameA(hModule, path, sizeof(path))
		|| !GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
	{
		TT_Trace("==== build identity unavailable ====");
		return;
	}
	SYSTEMTIME utc, local;
	FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
	SystemTimeToTzSpecificLocalTime(NULL, &utc, &local);
	TT_Trace("==== build identity: %s written %04d-%02d-%02d %02d:%02d:%02d local, %lu bytes ====",
		path, (int)local.wYear, (int)local.wMonth, (int)local.wDay,
		(int)local.wHour, (int)local.wMinute, (int)local.wSecond,
		(unsigned long)fad.nFileSizeLow);
#else
	TT_Trace("==== build identity: not implemented on this platform ====");
#endif
}
