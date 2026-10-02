// h_export.cpp - copied unmodified from metamod-p's stub_plugin.

#include <extdll.h>
#include <h_export.h>

enginefuncs_t g_engfuncs;
globalvars_t  *gpGlobals;

C_DLLEXPORT FORCE_STACK_ALIGN void WINAPI GiveFnptrsToDll(enginefuncs_t *pengfuncsFromEngine, globalvars_t *pGlobals)
{
	memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t));
	gpGlobals = pGlobals;
}
