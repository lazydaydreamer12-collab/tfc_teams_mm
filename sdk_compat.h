// sdk_compat.h - small helpers not already provided by the SDK.
//
// Note: STRING()/MAKE_STRING()/ENTINDEX()/FNullEnt() are already provided
// by metamod-p's bundled hlsdk/dlls/util.h (pulled in transitively via
// <meta_api.h> -> metamod/sdk_util.h -> <util.h>) - those are generic
// engine helpers, not TFC-specific, so the stock ones are correct and we
// don't redeclare them here (redeclaring them caused real ambiguous-
// overload compile errors - verified with a syntax-only build against
// the actual cloned SDK headers).

#ifndef TFC_TEAMS_SDK_COMPAT_H
#define TFC_TEAMS_SDK_COMPAT_H

#include <extdll.h>
#include <meta_api.h>

// Portability shims: MSVC spells these differently than glibc.
#ifdef _WIN32
	#include <string.h>
	#define strcasecmp _stricmp
	#define strncasecmp _strnicmp
	#define _snprintf_wc _snprintf
#else
	#include <strings.h>
	#define _snprintf_wc snprintf
#endif

// Portable case-insensitive substring test (mirrors Pawn's containi()).
inline bool ContainsI(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !*needle)
		return false;
	size_t needleLen = strlen(needle);
	for (const char *p = haystack; *p; p++)
	{
		if (strncasecmp(p, needle, needleLen) == 0)
			return true;
	}
	return false;
}

// Trim leading/trailing whitespace in place. Returns ptr to first
// non-whitespace char (may be later in the same buffer).
inline char *TrimInPlace(char *s)
{
	if (!s)
		return s;
	char *end;
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	if (*s == 0)
		return s;
	end = s + strlen(s) - 1;
	while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
		*end-- = 0;
	return s;
}

// Copy the first whitespace-delimited token from `text` into `tokenOut`
// (size tokenOutLen), and return a pointer (into `text`) to the trimmed
// remainder. `text` is modified in place (nul is inserted after the
// token). Mirrors the semantics AMX Pawn's strtok() gave the original
// plugin closely enough for our config-line grammar.
inline char *SplitFirstToken(char *text, char *tokenOut, size_t tokenOutLen)
{
	tokenOut[0] = 0;
	if (!text)
		return text;

	char *p = text;
	while (*p == ' ' || *p == '\t')
		p++;

	char *tokStart = p;
	while (*p && *p != ' ' && *p != '\t')
		p++;

	size_t len = (size_t)(p - tokStart);
	if (len >= tokenOutLen)
		len = tokenOutLen - 1;
	memcpy(tokenOut, tokStart, len);
	tokenOut[len] = 0;

	while (*p == ' ' || *p == '\t')
		p++;

	return p; // remainder, still inside `text`'s storage
}

#endif // TFC_TEAMS_SDK_COMPAT_H
