// NahIDWinCore.cpp - M0: sonda de golpes (so regista, nao altera nada).
//
// Entra na funcao de processamento de golpe do Skyrim LE 1.9.32 (0x006E4770,
// thiscall: ecx = Actor atingido, 1.o argumento = HitData*, ret 4), regista os dados
// em bruto e chama sempre a funcao original sem mexer em nada.
// Objetivo: descobrir com certeza o que cada campo do HitData contem antes de construir
// o bloqueio/parry/poise por cima.

#include "skse/PluginAPI.h"
#include "skse/skse_version.h"
#include "Log.h"
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>

#define PLUGIN_NAME        "NahIDWinCore"
#define PLUGIN_VERSION_STR "0.1.0 (M0 hit probe)"
#define PLUGIN_LOG_FILE    "\\My Games\\Skyrim\\SKSE\\NahIDWinCore.log"

namespace
{
	const UInt32 kHitFuncAddr   = 0x006E4770;   // processamento de golpe (LE 1.9.32)
	const UInt32 kPlayerPtrAddr = 0x01310588;   // ponteiro para o Player
	const UInt32 kStolenBytes   = 6;            // 81 EC 5C 02 00 00 (uma instrucao completa)
	const UInt8  kExpected[11]  = { 0x81, 0xEC, 0x5C, 0x02, 0x00, 0x00, 0x53, 0x55, 0x56, 0x8B, 0xF1 };
	const UInt32 kDumpDwords    = 0x70 / 4;     // campos 0x00..0x6C do HitData

	typedef void (__fastcall * HitFn)(void * self, void * edx, void * hit);

	HitFn            g_orig      = NULL;
	char             g_iniPath[MAX_PATH] = "";
	CRITICAL_SECTION g_lock;
	bool             g_lockInit  = false;

	int  g_enable  = 1;
	int  g_maxLogs = 300;
	int  g_onlyPlayerInvolved = 0;   // 1 = so golpes em que o jogador e vitima (ou atacante)
	int  g_logged  = 0;
	int  g_seen    = 0;
	bool g_capNoted = false;

	void ResolveIniPath()
	{
		HMODULE hm = NULL;
		if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							   (LPCSTR)&ResolveIniPath, &hm) || !hm)
		{
			strcpy_s(g_iniPath, sizeof(g_iniPath), "Data\\SKSE\\Plugins\\NahIDWinCore.ini");
			return;
		}
		GetModuleFileNameA(hm, g_iniPath, MAX_PATH);
		char * dot = strrchr(g_iniPath, '.');
		if(dot) strcpy_s(dot, (size_t)(g_iniPath + MAX_PATH - dot), ".ini");
		else strcat_s(g_iniPath, sizeof(g_iniPath), ".ini");
	}

	void LoadConfig()
	{
		g_enable  = (int)GetPrivateProfileIntA("General", "Enable", 1, g_iniPath);
		g_maxLogs = (int)GetPrivateProfileIntA("General", "MaxLogs", 300, g_iniPath);
		g_onlyPlayerInvolved = (int)GetPrivateProfileIntA("General", "OnlyPlayerInvolved", 0, g_iniPath);
		_MESSAGE("config: enable=%d maxLogs=%d onlyPlayerInvolved=%d  [%s]", g_enable, g_maxLogs, g_onlyPlayerInvolved, g_iniPath);
	}

	// Leitura protegida: se o endereco nao for legivel devolve false em vez de crashar.
	bool SafeRead32(UInt32 addr, UInt32 * out)
	{
		__try { *out = *(volatile UInt32 *)addr; return true; }
		__except(EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool SafeRead8(UInt32 addr, UInt8 * out)
	{
		__try { *out = *(volatile UInt8 *)addr; return true; }
		__except(EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Descricao curta de um TESForm* (formID em +0x0C, tipo em +0x04).
	void DescribeForm(UInt32 p, char * buf, size_t n)
	{
		UInt32 id = 0; UInt8 type = 0;
		if(!p) { _snprintf_s(buf, n, _TRUNCATE, "null"); return; }
		if(!SafeRead32(p + 0x0C, &id) || !SafeRead8(p + 0x04, &type))
		{ _snprintf_s(buf, n, _TRUNCATE, "%08X(ilegivel)", p); return; }
		_snprintf_s(buf, n, _TRUNCATE, "%08X form=%08X tipo=%u", p, id, (unsigned)type);
	}

	void LogHit(void * self, void * hit, const UInt32 * before, const UInt32 * after, bool haveAfter, bool readable)
	{
		UInt32 player = 0; SafeRead32(kPlayerPtrAddr, &player);
		char vb[96];
		DescribeForm((UInt32)self, vb, sizeof(vb));

		_MESSAGE("---- golpe #%d  t=%lu ms  vitima=%s%s  HitData=%08X ----",
				 g_logged + 1, (unsigned long)GetTickCount(), vb,
				 ((UInt32)self == player) ? "  [JOGADOR]" : "", (UInt32)hit);
		if(!readable) { _MESSAGE("  HitData ilegivel"); return; }

		UInt32 attackerHandle = before[0x18 / 4];
		UInt32 weapon = before[0x28 / 4];
		char wb[96]; DescribeForm(weapon, wb, sizeof(wb));
		UInt8 wtype = 0xFF; if(weapon) SafeRead8(weapon + 0xF1, &wtype);
		UInt32 flags = before[0x68 / 4];

		_MESSAGE("  atacante(handle)=%08X%s  arma=%s  tipoArma(+F1)=%u  flags(+68)=%08X [bloqueado(bit3)=%d, arco(bit11)=%d]",
				 attackerHandle, attackerHandle == 0x00100000 ? " [JOGADOR]" : "",
				 wb, (unsigned)wtype, flags, (flags >> 3) & 1, (flags >> 11) & 1);

		// Dump em bruto: hex + leitura como float quando for plausivel.
		for(UInt32 i = 0; i < kDumpDwords; i += 4)
		{
			char line[256]; line[0] = 0;
			for(UInt32 j = i; j < i + 4 && j < kDumpDwords; j++)
			{
				UInt32 v = before[j];
				float f; memcpy(&f, &v, 4);
				char cell[64];
				if(v != 0 && _finite(f) && fabs(f) > 0.0001f && fabs(f) < 100000.0f)
					_snprintf_s(cell, sizeof(cell), _TRUNCATE, " +%02X=%08X(%.3f)", j * 4, v, f);
				else
					_snprintf_s(cell, sizeof(cell), _TRUNCATE, " +%02X=%08X", j * 4, v);
				strncat_s(line, sizeof(line), cell, _TRUNCATE);
			}
			_MESSAGE(" %s", line);
		}

		if(haveAfter)
		{
			char chg[512]; chg[0] = 0; int n = 0;
			for(UInt32 j = 0; j < kDumpDwords; j++)
				if(before[j] != after[j])
				{
					char cell[48];
					_snprintf_s(cell, sizeof(cell), _TRUNCATE, " +%02X:%08X->%08X", j * 4, before[j], after[j]);
					strncat_s(chg, sizeof(chg), cell, _TRUNCATE); n++;
				}
			_MESSAGE("  apos a funcao original: %s", n ? chg : "sem alteracoes no HitData");
		}
	}

	void __fastcall HitHook(void * self, void * edx, void * hit)
	{
		bool doLog = false;
		UInt32 before[kDumpDwords]; memset(before, 0, sizeof(before));
		bool readable = false;

		if(g_enable && hit && g_lockInit)
		{
			g_seen++;
			readable = true;
			for(UInt32 i = 0; i < kDumpDwords; i++)
				if(!SafeRead32((UInt32)hit + i * 4, &before[i])) { readable = false; break; }

			bool ok = true;
			if(g_onlyPlayerInvolved)
			{
				UInt32 player = 0; SafeRead32(kPlayerPtrAddr, &player);
				ok = ((UInt32)self == player) || (readable && before[0x18 / 4] == 0x00100000);
			}
			if(ok)
			{
				if(g_logged < g_maxLogs) doLog = true;
				else if(!g_capNoted) { g_capNoted = true; EnterCriticalSection(&g_lock); _MESSAGE("limite de %d golpes atingido - a parar de registar (MaxLogs no INI)", g_maxLogs); LeaveCriticalSection(&g_lock); }
			}
		}

		g_orig(self, edx, hit);   // comportamento original, sempre

		if(doLog)
		{
			UInt32 after[kDumpDwords]; memset(after, 0, sizeof(after));
			bool haveAfter = readable;
			for(UInt32 i = 0; i < kDumpDwords && haveAfter; i++)
				if(!SafeRead32((UInt32)hit + i * 4, &after[i])) haveAfter = false;
			EnterCriticalSection(&g_lock);
			LogHit(self, hit, before, after, haveAfter, readable);
			g_logged++;
			LeaveCriticalSection(&g_lock);
		}
	}

	bool InstallHook()
	{
		UInt8 * target = (UInt8 *)kHitFuncAddr;
		if(memcmp(target, kExpected, sizeof(kExpected)) != 0)
		{
			char hex[64]; hex[0] = 0;
			for(int i = 0; i < 11; i++) { char c[8]; _snprintf_s(c, sizeof(c), _TRUNCATE, "%02X ", target[i]); strncat_s(hex, sizeof(hex), c, _TRUNCATE); }
			_MESSAGE("ABORTA: bytes inesperados em %08X: %s(esperado 81 EC 5C 02 00 00 53 55 56 8B F1) - outro mod ja mexeu aqui ou o executavel e diferente", kHitFuncAddr, hex);
			return false;
		}

		UInt8 * tramp = (UInt8 *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if(!tramp) { _MESSAGE("ABORTA: VirtualAlloc falhou"); return false; }
		memcpy(tramp, target, kStolenBytes);
		tramp[kStolenBytes] = 0xE9;
		*(UInt32 *)(tramp + kStolenBytes + 1) = (UInt32)(target + kStolenBytes) - (UInt32)(tramp + kStolenBytes + 5);

		DWORD old = 0;
		if(!VirtualProtect(target, kStolenBytes, PAGE_EXECUTE_READWRITE, &old))
		{ _MESSAGE("ABORTA: VirtualProtect falhou"); return false; }
		g_orig = (HitFn)tramp;
		target[0] = 0xE9;
		*(UInt32 *)(target + 1) = (UInt32)&HitHook - (UInt32)(target + 5);
		target[5] = 0x90;
		VirtualProtect(target, kStolenBytes, old, &old);
		FlushInstructionCache(GetCurrentProcess(), target, kStolenBytes);
		_MESSAGE("hook instalado em %08X -> %08X (trampolim %08X)", kHitFuncAddr, (UInt32)&HitHook, (UInt32)tramp);
		return true;
	}
}

extern "C"
{

bool SKSEPlugin_Query(const SKSEInterface * skse, PluginInfo * info)
{
	gLog.OpenRelative(CSIDL_MYDOCUMENTS, PLUGIN_LOG_FILE);
	_MESSAGE("NahIDWinCore " PLUGIN_VERSION_STR " (built " __DATE__ " " __TIME__ ")");

	info->infoVersion = PluginInfo::kInfoVersion;
	info->name        = PLUGIN_NAME;
	info->version     = 1;

	if(skse->isEditor) { _MESSAGE("ABORTA: carregado no editor"); return false; }
	_MESSAGE("runtime %08X (1.9.32.0 e %08X)", skse->runtimeVersion, RUNTIME_VERSION_1_9_32_0);
	if(skse->runtimeVersion != RUNTIME_VERSION_1_9_32_0)
	{ _MESSAGE("ABORTA: runtime nao suportado"); return false; }
	return true;
}

bool SKSEPlugin_Load(const SKSEInterface * skse)
{
	InitializeCriticalSection(&g_lock);
	g_lockInit = true;
	ResolveIniPath();
	LoadConfig();
	if(!g_enable) { _MESSAGE("Enable=0 - nada instalado"); return true; }
	if(!InstallHook()) return true;   // nao impede o jogo de arrancar
	_MESSAGE("M0 ativo: so regista, nao altera nenhum golpe");
	return true;
}

};
