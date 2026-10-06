// tt_secret.cpp - where the Discord webhook is kept.
//
// A Discord webhook link IS the password to post in that channel, so it is
// never written into tfc_teams.ini (which gets copied around, pasted in
// forums and put on GitHub). Instead:
//
//   - it is set from the server console or rcon:  tt_discord <id> <token>
//     (or dropped into tt_secret_import.txt, which the plugin reads, encrypts
//     and deletes - that keeps it out of the server's rcon log too);
//   - it is stored in tt_secret.dat, encrypted with a key made from this
//     machine's own ID (Windows MachineGuid, Linux /etc/machine-id). Read on
//     another machine - a copied server folder, a backup, an FTP download -
//     the file is just noise.
//
// What this cannot do: someone who can run programs on the server machine
// itself as the server's user can work the key out, as the plugin does. If
// the link gets out, delete the webhook in Discord and make a new one.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

#include "tt_common.h"
#include "tt_crypto.h"
#include "tt_net.h"

TTSecret g_secret;

void TT_Printf(const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	b[sizeof(b) - 1] = 0;
	SERVER_PRINT(b);
}

static char g_keySource[16] = "";   // where the machine key came from (for messages)

static bool TT_SecretPath(const char *name, char *out, size_t len)
{
	char dir[400];
	if (!TT_ModuleDir(dir, sizeof(dir)))
		return false;
	_snprintf_wc(out, len - 1, "%s/%s", dir, name);
	out[len - 1] = 0;
	return true;
}

static void TT_Trim(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = 0;
	size_t a = 0;
	while (s[a] == ' ' || s[a] == '\t')
		a++;
	if (a)
		memmove(s, s + a, n - a + 1);
	// Quotes, or the <...> the instructions write around a placeholder, are not part of it.
	for (int pass = 0; pass < 2; pass++)
	{
		n = strlen(s);
		if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\'')
			|| (s[0] == '<' && s[n - 1] == '>')))
		{
			memmove(s, s + 1, n - 2);
			s[n - 2] = 0;
		}
	}
}

// ---------------------------------------------------------------------------
// The machine key
static bool TT_MachineMaterial(char *out, size_t len)
{
	out[0] = 0;
#ifdef _WIN32
	HKEY k;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0,
		KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS)
	{
		char v[128];
		DWORD type = 0, n = sizeof(v) - 1;
		if (RegQueryValueExA(k, "MachineGuid", NULL, &type, (LPBYTE)v, &n) == ERROR_SUCCESS && type == REG_SZ)
		{
			v[n < sizeof(v) ? n : sizeof(v) - 1] = 0;
			TT_Trim(v);
			if (v[0])
			{
				_snprintf_wc(out, len - 1, "win:%s", v);
				out[len - 1] = 0;
				strcpy(g_keySource, "machine");
			}
		}
		RegCloseKey(k);
	}
	if (!out[0])
	{
		// No MachineGuid (should not happen): a random key kept next to the DLL.
		char path[450];
		if (!TT_SecretPath("tt_secret.key", path, sizeof(path)))
			return false;
		FILE *f = fopen(path, "r");
		char v[80] = "";
		if (f) { if (!fgets(v, sizeof(v), f)) v[0] = 0; fclose(f); }
		TT_Trim(v);
		if (strlen(v) != 64)
		{
			unsigned char r[32];
			if (!TT_RandomBytes(r, 32))
				return false;
			TT_Hex(r, 32, v);
			f = fopen(path, "w");
			if (!f)
				return false;
			fprintf(f, "%s\n", v);
			fclose(f);
		}
		_snprintf_wc(out, len - 1, "file:%s", v);
		out[len - 1] = 0;
		strcpy(g_keySource, "local file");
	}
	return true;
#else
	static const char *ids[] = { "/etc/machine-id", "/var/lib/dbus/machine-id" };
	for (int i = 0; i < 2 && !out[0]; i++)
	{
		FILE *f = fopen(ids[i], "r");
		if (!f)
			continue;
		char v[80] = "";
		if (!fgets(v, sizeof(v), f))
			v[0] = 0;
		fclose(f);
		TT_Trim(v);
		if (strlen(v) >= 16)
		{
			_snprintf_wc(out, len - 1, "mid:%s", v);
			out[len - 1] = 0;
			strcpy(g_keySource, "machine");
		}
	}
	if (out[0])
		return true;
	// No machine ID (some containers): a random key in the server user's home
	// folder - outside the game folder, so copying the server does not copy it.
	char path[450] = "";
	const char *home = getenv("HOME");
	if (home && home[0])
		_snprintf_wc(path, sizeof(path) - 1, "%s/.tfc_teams_mm.key", home);
	else if (!TT_SecretPath("tt_secret.key", path, sizeof(path)))
		return false;
	path[sizeof(path) - 1] = 0;
	char v[80] = "";
	FILE *f = fopen(path, "r");
	if (f) { if (!fgets(v, sizeof(v), f)) v[0] = 0; fclose(f); }
	TT_Trim(v);
	if (strlen(v) != 64)
	{
		unsigned char r[32];
		if (!TT_RandomBytes(r, 32))
			return false;
		TT_Hex(r, 32, v);
		f = fopen(path, "w");
		if (!f)
			return false;
		fprintf(f, "%s\n", v);
		fclose(f);
		chmod(path, 0600);
		TT_Trace("Secret: no machine ID here - made a key file in %s", home && home[0] ? "the server user's home folder" : "the plugin folder");
	}
	_snprintf_wc(out, len - 1, "file:%s", v);
	out[len - 1] = 0;
	strcpy(g_keySource, home && home[0] ? "home key file" : "local key file");
	return true;
#endif
}

static bool TT_MachineKey(unsigned char key[32])
{
	char mat[200];
	if (!TT_MachineMaterial(mat, sizeof(mat)))
		return false;
	TTSha256 c;
	TT_Sha256Init(&c);
	static const char label[] = "tfc_teams_mm secret v1";
	TT_Sha256Update(&c, label, sizeof(label));
	TT_Sha256Update(&c, mat, strlen(mat));
	TT_Sha256Final(&c, key);
	memset(mat, 0, sizeof(mat));
	return true;
}

// Keystream block i = HMAC(key, "ks" | nonce | i); tag = HMAC(key, "tag" | nonce | ciphertext).
static void TT_Crypt(const unsigned char key[32], const unsigned char nonce[16], unsigned char *buf, size_t len)
{
	unsigned char in[2 + 16 + 4], ks[32];
	memcpy(in, "ks", 2);
	memcpy(in + 2, nonce, 16);
	for (size_t off = 0, blk = 0; off < len; off += 32, blk++)
	{
		in[18] = (unsigned char)(blk >> 24); in[19] = (unsigned char)(blk >> 16);
		in[20] = (unsigned char)(blk >> 8);  in[21] = (unsigned char)blk;
		TT_HmacSha256(key, 32, in, sizeof(in), ks);
		for (size_t j = 0; j < 32 && off + j < len; j++)
			buf[off + j] ^= ks[j];
	}
	memset(ks, 0, sizeof(ks));
}

static void TT_Tag(const unsigned char key[32], const unsigned char nonce[16], const unsigned char *c, size_t len, unsigned char tag[32])
{
	unsigned char *m = (unsigned char *)malloc(3 + 16 + len);
	if (!m) { memset(tag, 0, 32); return; }
	memcpy(m, "tag", 3);
	memcpy(m + 3, nonce, 16);
	memcpy(m + 19, c, len);
	TT_HmacSha256(key, 32, m, 19 + len, tag);
	free(m);
}

// ---------------------------------------------------------------------------
static void TT_SecretParse(char *text)
{
	char *line = text;
	while (line && *line)
	{
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		char *eq = strchr(line, '=');
		if (eq)
		{
			*eq = 0;
			char *k = line, *v = eq + 1;
			TT_Trim(k);
			TT_Trim(v);
			if (!strcmp(k, "discord")) { strncpy(g_secret.discord, v, sizeof(g_secret.discord) - 1); g_secret.discord[sizeof(g_secret.discord) - 1] = 0; }
			else if (k[0] && k[0] != '#' && strcmp(k, "web") && strcmp(k, "webkey"))
				TT_Trace("Secret: \"%s=\" is not used - the line is discord=<webhook link>", k);
		}
		line = nl ? nl + 1 : NULL;
	}
}

bool TT_SecretSave(void)
{
	char path[450];
	if (!TT_SecretPath("tt_secret.dat", path, sizeof(path)))
		return false;
	if (!g_secret.discord[0])
	{
		remove(path);
		return true;
	}
	unsigned char key[32], nonce[16], tag[32];
	if (!TT_MachineKey(key) || !TT_RandomBytes(nonce, 16))
	{
		TT_Trace("Secret: could not make a key - links not saved");
		return false;
	}
	char plain[1200];
	_snprintf_wc(plain, sizeof(plain) - 1, "discord=%s\n", g_secret.discord);
	plain[sizeof(plain) - 1] = 0;
	size_t n = strlen(plain);
	TT_Crypt(key, nonce, (unsigned char *)plain, n);
	TT_Tag(key, nonce, (unsigned char *)plain, n, tag);
	char *hex = (char *)malloc(n * 2 + 1);
	char nh[33], th[65];
	if (!hex)
		return false;
	TT_Hex((unsigned char *)plain, n, hex);
	TT_Hex(nonce, 16, nh);
	TT_Hex(tag, 32, th);
	char tmp[470];
	_snprintf_wc(tmp, sizeof(tmp) - 1, "%s.tmp", path);
	tmp[sizeof(tmp) - 1] = 0;
	FILE *f = fopen(tmp, "w");
	bool ok = false;
	if (f)
	{
		fprintf(f, "# TFC Teams: the Discord webhook, encrypted for this machine only.\n");
		fprintf(f, "# Change it with tt_discord in the server console. Do not edit.\n");
		fprintf(f, "TTSECRET1 %s %s %s\n", nh, hex, th);
		ok = fclose(f) == 0;
	}
#ifndef _WIN32
	if (ok)
		chmod(tmp, 0600);
#endif
	if (ok)
	{
		remove(path);
		ok = rename(tmp, path) == 0;
	}
	memset(plain, 0, sizeof(plain));
	memset(key, 0, sizeof(key));
	free(hex);
	if (!ok)
		TT_Trace("Secret: could not write tt_secret.dat");
	return ok;
}

static bool TT_SecretLoadFile(void)
{
	char path[450];
	if (!TT_SecretPath("tt_secret.dat", path, sizeof(path)))
		return false;
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	char *line = (char *)malloc(4096);
	bool found = false, ok = false;
	while (line && fgets(line, 4096, f))
	{
		if (strncmp(line, "TTSECRET1 ", 10))
			continue;
		found = true;
		char *nh = line + 10;
		char *ch = strchr(nh, ' ');
		if (!ch) break;
		*ch++ = 0;
		char *th = strchr(ch, ' ');
		if (!th) break;
		*th++ = 0;
		TT_Trim(th);
		unsigned char nonce[16], tag[32], want[32], key[32];
		size_t cap = strlen(ch) / 2 + 1;
		unsigned char *c = (unsigned char *)malloc(cap + 1);
		int n = c ? TT_Unhex(ch, c, cap) : -1;
		if (n < 0 || TT_Unhex(nh, nonce, 16) != 16 || TT_Unhex(th, tag, 32) != 32 || !TT_MachineKey(key))
		{
			free(c);
			break;
		}
		TT_Tag(key, nonce, c, (size_t)n, want);
		if (!TT_SameBytes(tag, want, 32))
		{
			TT_Trace("Secret: tt_secret.dat was made on another machine (or changed) - set the webhook again with tt_discord");
			free(c);
			memset(key, 0, sizeof(key));
			break;
		}
		TT_Crypt(key, nonce, c, (size_t)n);
		c[n] = 0;
		TT_SecretParse((char *)c);
		memset(c, 0, (size_t)n);
		free(c);
		memset(key, 0, sizeof(key));
		ok = true;
		break;
	}
	free(line);
	fclose(f);
	if (found && !ok)
		TT_Trace("Secret: could not read tt_secret.dat");
	return ok;
}

// tt_secret_import.txt: discord=<webhook link> ("off" clears it). Read, saved
// encrypted, then the file is wiped and deleted.
static void TT_SecretImport(void)
{
	char path[450];
	if (!TT_SecretPath("tt_secret_import.txt", path, sizeof(path)))
		return;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char text[4096];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = 0;
	TTSecret before = g_secret;
	TT_SecretParse(text);
	if (!strcasecmp(g_secret.discord, "off")) g_secret.discord[0] = 0;
	bool changed = memcmp(&before, &g_secret, sizeof(before)) != 0;
	// Wipe it: overwrite with spaces, then delete.
	f = fopen(path, "r+b");
	if (f)
	{
		for (size_t i = 0; i < n; i++)
			fputc(' ', f);
		fclose(f);
	}
	memset(text, 0, sizeof(text));
	bool removed = remove(path) == 0;
	if (changed)
		TT_SecretSave();
	TT_Trace("Secret: took in tt_secret_import.txt (%s)%s", changed ? "webhook updated" : "nothing new",
		removed ? " and deleted it" : " - COULD NOT DELETE IT, delete it by hand");
	char m[160];
	if (g_secret.discord[0]) { TT_SecretMask(g_secret.discord, m, sizeof(m)); TT_Trace("Secret: Discord %s", m); }
}

void TT_SecretLoad(void)
{
	memset(&g_secret, 0, sizeof(g_secret));
	TT_SecretLoadFile();
	TT_SecretImport();
}

// ---------------------------------------------------------------------------
void TT_SecretMask(const char *url, char *out, size_t len)
{
	out[0] = 0;
	if (!url || !url[0])
	{
		_snprintf_wc(out, len - 1, "(not set)");
		out[len - 1] = 0;
		return;
	}
	const char *wh = strstr(url, "/api/webhooks/");
	if (wh)
	{
		// Keep the webhook ID (it says which webhook), hide the token.
		const char *id = wh + 14;
		size_t idLen = strcspn(id, "/?");
		_snprintf_wc(out, len - 1, "%.*s%.*s/****", (int)(id - url), url, (int)idLen, id);
		out[len - 1] = 0;
		return;
	}
	const char *host = strstr(url, "://");
	host = host ? host + 3 : url;
	size_t hostLen = strcspn(host, "/?");
	_snprintf_wc(out, len - 1, "%.*s/...", (int)(host - url + hostLen), url);
	out[len - 1] = 0;
}

static bool TT_IsDiscordWebhook(const char *u)
{
	static const char *ok[] = { "https://discord.com/api/webhooks/", "https://discordapp.com/api/webhooks/",
		"https://canary.discord.com/api/webhooks/", "https://ptb.discord.com/api/webhooks/" };
	for (int i = 0; i < 4; i++)
		if (!strncmp(u, ok[i], strlen(ok[i])))
		{
			const char *rest = u + strlen(ok[i]);
			const char *slash = strchr(rest, '/');
			return slash && slash > rest && slash[1];
		}
	return false;
}

// The whole argument line, quotes removed: the console splits ":" and drops
// everything after "//" when it tokenises, so CMD_ARGV would mangle a link.
static void TT_ArgsLine(char *out, size_t len)
{
	const char *a = CMD_ARGS();
	strncpy(out, a ? a : "", len - 1);
	out[len - 1] = 0;
	TT_Trim(out);
}

// A webhook is https://discord.com/api/webhooks/<id>/<token>: the id is a
// long number, the token about 68 letters, digits, "-" and "_".
static bool TT_WebhookParts(const char *id, size_t idLen, const char *tok, size_t tokLen)
{
	if (idLen < 15 || idLen > 22 || tokLen < 50 || tokLen > 120)
		return false;
	for (size_t i = 0; i < idLen; i++)
		if (id[i] < '0' || id[i] > '9')
			return false;
	for (size_t i = 0; i < tokLen; i++)
	{
		char c = tok[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
			return false;
	}
	return true;
}

static void TT_DiscordFail(const char *why)
{
	TT_Printf("[Teams] %s\n", why);
	TT_Trace("Secret: tt_discord refused - %s", why); // (the link itself is never logged)
}

// "<link>", "<id> <token>" or "<id>/<token>" -> the full webhook link. False
// (and the reason printed): not a webhook, or cut off.
static bool TT_WebhookFromArgs(const char *a, char *url, size_t urlLen)
{
	url[0] = 0;
	if (!strncmp(a, "http", 4))
	{
		if (!TT_IsDiscordWebhook(a))
		{
			TT_DiscordFail("That is not a Discord webhook link (https://discord.com/api/webhooks/<id>/<token>).");
			return false;
		}
		const char *id = strstr(a, "/api/webhooks/") + 14;
		const char *slash = strchr(id, '/');
		const char *tok = slash + 1;
		size_t tokLen = strcspn(tok, "/?\" ");
		if (!TT_WebhookParts(id, (size_t)(slash - id), tok, tokLen))
		{
			TT_DiscordFail("That webhook link looks cut off or wrong - copy it again from Discord, "
				"or use the short form: tt_discord <id> <token>");
			return false;
		}
		_snprintf_wc(url, urlLen - 1, "%.*s", (int)(tok + tokLen - a), a);
		url[urlLen - 1] = 0;
		return true;
	}
	size_t idLen = strspn(a, "0123456789");
	const char *tok = a + idLen;
	while (*tok == ' ' || *tok == '\t' || *tok == '/')
		tok++;
	size_t tokLen = strlen(tok);
	while (tokLen && (tok[tokLen - 1] == ' ' || tok[tokLen - 1] == '"'))
		tokLen--;
	if (tok == a + idLen || !TT_WebhookParts(a, idLen, tok, tokLen))
	{
		TT_DiscordFail("Use tt_discord \"<webhook link>\" or tt_discord <id> <token> "
			"(the long number and the long code after /api/webhooks/).");
		return false;
	}
	_snprintf_wc(url, urlLen - 1, "https://discord.com/api/webhooks/%.*s/%.*s", (int)idLen, a, (int)tokLen, tok);
	url[urlLen - 1] = 0;
	return true;
}

static void TT_Cmd_Discord(void)
{
	char a[600];
	const char *raw = CMD_ARGS();
	// An odd number of quotes: the line was cut off on the way in - AMX Mod X's
	// amx_rcon passes on only 127 characters.
	int quotes = 0;
	for (const char *q = raw; q && *q; q++)
		if (*q == '"')
			quotes++;
	TT_ArgsLine(a, sizeof(a));
	char m[200];
	if (!a[0])
	{
		TT_SecretMask(g_secret.discord, m, sizeof(m));
		TT_Printf("[Teams] Discord webhook (end-of-map awards): %s\n"
			"  Set:   tt_discord <id> <token>   (the long number and the long code after /api/webhooks/)\n"
			"     or  tt_discord \"https://discord.com/api/webhooks/<id>/<token>\"\n"
			"  Clear: tt_discord off\n", m);
		return;
	}
	if (!strcasecmp(a, "off") || !strcasecmp(a, "clear"))
	{
		g_secret.discord[0] = 0;
		TT_SecretSave();
		SERVER_PRINT("[Teams] Discord webhook cleared.\n");
		TT_Trace("Secret: Discord webhook cleared from the console");
		return;
	}
	if (quotes % 2)
	{
		TT_DiscordFail("The webhook link arrived cut off (amx_rcon passes on only 127 characters). "
			"Use the short form: tt_discord <id> <token> - the two parts after /api/webhooks/");
		return;
	}
	char url[600];
	if (!TT_WebhookFromArgs(a, url, sizeof(url)))
		return;
	strncpy(g_secret.discord, url, sizeof(g_secret.discord) - 1);
	g_secret.discord[sizeof(g_secret.discord) - 1] = 0;
	bool saved = TT_SecretSave();
	TT_SecretMask(g_secret.discord, m, sizeof(m));
	TT_Printf("[Teams] Discord webhook set: %s%s\n  tt_net_test sends a test message.\n", m,
		saved ? " (saved, encrypted)" : " (COULD NOT SAVE - it is lost when the server stops)");
	TT_Trace("Secret: Discord webhook set from the console: %s%s", m, saved ? "" : " - COULD NOT SAVE");
}

void TT_SecretRegisterCommands(void)
{
	REG_SVR_COMMAND("tt_discord", TT_Cmd_Discord);
}

const char *TT_SecretKeySource(void) { return g_keySource[0] ? g_keySource : "not made yet"; }
