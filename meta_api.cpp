// meta_api.cpp - plugin identity and metamod attach/detach, from metamod-p's
// stub_plugin.

#include <extdll.h>
#include <meta_api.h>

#include "sdk_util.h"
#include "tt_common.h"
#include "tt_net.h"
#include "tt_stats.h"

C_DLLEXPORT int GetEngineFunctions_Post(enginefuncs_t *pengfuncsFromEngine, int *interfaceVersion);

#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dlfcn.h>
#include <link.h>
#endif

// ONE COPY ONLY. The first live test had the plugin listed twice in
// plugins.ini (tfc_teams_mm.dll and tfc_teams_mm_mm.dll), and two copies each
// planned and carried out their own scramble - which is how Red ended up
// stacked. The first copy to attach writes its own module address into a
// process environment variable (the OS's, not a CRT copy, so separately
// linked DLLs share it). A later copy refuses to attach only if that address
// is still a loaded module whose file name is ours and is NOT itself - so a
// value left behind by a copy that has since been unloaded (a listen server
// restarted inside the same hl.exe, where Meta_Detach may never run) does not
// lock the plugin out.
#define TT_INSTANCE_ENV "TFC_TEAMS_MM_MODULE"
static bool g_ownInstance = false;

static bool TT_OtherCopyLoaded(unsigned long long other)
{
#ifdef _WIN32
	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&TT_OtherCopyLoaded, &self);
	HMODULE h = (HMODULE)(uintptr_t)other;
	if (!h || h == self)
		return false;
	char path[MAX_PATH];
	if (!GetModuleFileNameA(h, path, sizeof(path)))
		return false; // gone
	return ContainsI(path, "tfc_teams_mm");
#else
	Dl_info mine, theirs;
	if (!dladdr((void *)&TT_OtherCopyLoaded, &mine) || !mine.dli_fname)
		return false;
	if (!other || !dladdr((void *)(uintptr_t)other, &theirs) || !theirs.dli_fname)
		return false; // gone
	if (!strcmp(mine.dli_fname, theirs.dli_fname))
		return false;
	return ContainsI(theirs.dli_fname, "tfc_teams_mm");
#endif
}

// The guard above only knows about copies that set the environment variable.
// An OLD build (before the guard existed) never sets it, so if plugins.ini
// still lists it the two run side by side - every scramble and balance move
// then happens twice. Walk the loaded modules and shout about any other file
// with tfc_teams_mm in its name. Called at attach and at every map start.
#ifndef _WIN32
static int TT_ForeignCopyCb(struct dl_phdr_info *info, size_t, void *selfBase)
{
	if (!info->dlpi_name || !info->dlpi_name[0] || (void *)info->dlpi_addr == selfBase)
		return 0;
	if (ContainsI(info->dlpi_name, "tfc_teams_mm"))
	{
		TT_Trace("==== WARNING: a second TFC Teams library is loaded: %s - every balance/scramble move will happen twice. "
			"Remove its line from addons/metamod/plugins.ini and delete the file. ====", info->dlpi_name);
		LOG_ERROR(PLID, "WARNING: second TFC Teams library loaded (%s) - remove it from plugins.ini and delete it", info->dlpi_name);
		SERVER_PRINT("[TFC Teams] WARNING: a second copy is loaded - check addons/metamod/plugins.ini\n");
	}
	return 0;
}
#endif

void TT_WarnForeignCopies(void)
{
#ifdef _WIN32
	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&TT_WarnForeignCopies, &self);
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE)
		return;
	MODULEENTRY32 me;
	me.dwSize = sizeof(me);
	for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me))
	{
		if (me.hModule == self || !ContainsI(me.szModule, "tfc_teams_mm"))
			continue;
		TT_Trace("==== WARNING: a second TFC Teams DLL is loaded: %s - every balance/scramble move will happen twice. "
			"Remove its line from addons/metamod/plugins.ini and delete the file. ====", me.szExePath);
		LOG_ERROR(PLID, "WARNING: second TFC Teams DLL loaded (%s) - remove it from plugins.ini and delete it", me.szExePath);
		SERVER_PRINT("[TFC Teams] WARNING: a second copy is loaded - check addons/metamod/plugins.ini\n");
	}
	CloseHandle(snap);
#else
	Dl_info mine;
	if (!dladdr((void *)&TT_WarnForeignCopies, &mine) || !mine.dli_fname)
		return;
	dl_iterate_phdr(TT_ForeignCopyCb, (void *)mine.dli_fbase);
#endif
}

static unsigned long long TT_SelfMarker(void)
{
#ifdef _WIN32
	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&TT_SelfMarker, &self);
	return (unsigned long long)(uintptr_t)self;
#else
	return (unsigned long long)(uintptr_t)&TT_OtherCopyLoaded;
#endif
}

static bool TT_ClaimInstance(void)
{
	char buf[40] = "";
#ifdef _WIN32
	DWORD n = GetEnvironmentVariableA(TT_INSTANCE_ENV, buf, sizeof(buf));
	if (n == 0 || n >= sizeof(buf))
		buf[0] = 0;
#else
	const char *v = getenv(TT_INSTANCE_ENV);
	if (v)
	{
		strncpy(buf, v, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = 0;
	}
#endif
	if (buf[0] && TT_OtherCopyLoaded(strtoull(buf, NULL, 16)))
		return false;
	_snprintf_wc(buf, sizeof(buf) - 1, "%llx", TT_SelfMarker());
	buf[sizeof(buf) - 1] = 0;
#ifdef _WIN32
	SetEnvironmentVariableA(TT_INSTANCE_ENV, buf);
#else
	setenv(TT_INSTANCE_ENV, buf, 1);
#endif
	g_ownInstance = true;
	return true;
}

static void TT_ReleaseInstance(void)
{
	if (!g_ownInstance)
		return;
#ifdef _WIN32
	SetEnvironmentVariableA(TT_INSTANCE_ENV, NULL);
#else
	unsetenv(TT_INSTANCE_ENV);
#endif
	g_ownInstance = false;
}

void TT_OnMapStart(void); // dllapi.cpp

static META_FUNCTIONS gMetaFunctionTable =
{
	NULL,               // pfnGetEntityAPI
	NULL,               // pfnGetEntityAPI_Post
	GetEntityAPI2,      // pfnGetEntityAPI2 - dllapi.cpp
	NULL,               // pfnGetEntityAPI2_Post
	NULL,               // pfnGetNewDLLFunctions
	NULL,               // pfnGetNewDLLFunctions_Post
	GetEngineFunctions, // pfnGetEngineFunctions - tt_engine.cpp
	GetEngineFunctions_Post, // pfnGetEngineFunctions_Post - tt_engine.cpp (stats)
};

plugin_info_t Plugin_info =
{
	META_INTERFACE_VERSION,
	"TFC Teams (balance + scramble)",
	"1.1.3",
	"2026/10/06",
	"custom",
	"",
	"TFCTEAMS",
	PT_ANYTIME,
	PT_ANYPAUSE,
};

meta_globals_t *gpMetaGlobals;
gamedll_funcs_t *gpGamedllFuncs;
mutil_funcs_t *gpMetaUtilFuncs;

C_DLLEXPORT FORCE_STACK_ALIGN int Meta_Query(char * /*ifvers*/, plugin_info_t **pPlugInfo,
		mutil_funcs_t *pMetaUtilFuncs)
{
	*pPlugInfo = &Plugin_info;
	gpMetaUtilFuncs = pMetaUtilFuncs;
	return TRUE;
}

C_DLLEXPORT FORCE_STACK_ALIGN int Meta_Attach(PLUG_LOADTIME now,
		META_FUNCTIONS *pFunctionTable, meta_globals_t *pMGlobals,
		gamedll_funcs_t *pGamedllFuncs)
{
	TT_ConfigDefaults();
	TT_Trace("==== Meta_Attach (loadtime %d) ====", (int)now);
	TT_LogModuleIdentity();

	if (!TT_ClaimInstance())
	{
		TT_Trace("==== ANOTHER COPY IS ALREADY LOADED - this one refuses to attach. Check addons/metamod/plugins.ini for a second tfc_teams_mm line. ====");
		LOG_ERROR(PLID, "TFC Teams is already loaded from another DLL - not attaching this copy (duplicate line in plugins.ini?)");
		return FALSE;
	}

	if (!pMGlobals)
	{
		LOG_ERROR(PLID, "Meta_Attach called with null pMGlobals");
		return FALSE;
	}
	gpMetaGlobals = pMGlobals;
	if (!pFunctionTable)
	{
		LOG_ERROR(PLID, "Meta_Attach called with null pFunctionTable");
		return FALSE;
	}
	memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));
	gpGamedllFuncs = pGamedllFuncs;

	TT_WarnForeignCopies();
	TT_RegisterServerCommands();
	TT_StatsRegisterCommands();
	TT_StatsInit(); // before TT_OnMapStart below, which starts the map's stats
	TT_NamesRegisterCommands();
	TT_FeedRegisterCommands();
	TT_SecretRegisterCommands();
	TT_NamesInit();
	TT_NetInit();   // reads the stored Discord webhook

	// Loaded into a map that is already running ("meta load" mid-game):
	// ServerActivate has been and gone, so do its work now. The team layout
	// comes from the .bsp in that case - see tt_map.cpp.
	if (now != PT_STARTUP && gpGlobals && gpGlobals->mapname && STRING(gpGlobals->mapname)[0])
	{
		TT_OnMapStart();
		TT_PlayersAdoptExisting();
	}
	else
		TT_ConfigLoad();
	return TRUE;
}

C_DLLEXPORT FORCE_STACK_ALIGN int Meta_Detach(PLUG_LOADTIME /*now*/,
		PL_UNLOAD_REASON /*reason*/)
{
	// Nothing else persistent to undo: no vtable hooks, no entities, no
	// cvars. metamod-p disables our server commands itself when we unload.
	TT_NetShutdown();
	TT_NamesShutdown();
	TT_StatsShutdown();
	TT_ReleaseInstance();
	return TRUE;
}
