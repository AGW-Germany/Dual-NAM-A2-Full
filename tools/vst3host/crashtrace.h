// Prints a symbolised stack trace on an unhandled exception (access violation
// etc.) using dbghelp.dll, which ships with Windows. Needs the plugin's PDB next
// to its build output (NeuralAmpModeler/build-win/pdbs).
#pragma once
#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>
#include <cstdio>
#pragma comment(lib, "Dbghelp.lib")

inline LONG WINAPI CrashTraceFilter(EXCEPTION_POINTERS* ep)
{
  HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
  SymInitialize(proc, nullptr, TRUE);
  std::printf("\n*** unhandled exception 0x%08lX at %p\n", ep->ExceptionRecord->ExceptionCode,
              ep->ExceptionRecord->ExceptionAddress);
  CONTEXT ctx = *ep->ContextRecord;
  STACKFRAME64 sf{};
  sf.AddrPC.Offset = ctx.Rip;
  sf.AddrPC.Mode = AddrModeFlat;
  sf.AddrFrame.Offset = ctx.Rbp;
  sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = ctx.Rsp;
  sf.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 40; i++)
  {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &sf, &ctx, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr))
      break;
    char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD ld = 0;
    char mod[MAX_PATH] = "?";
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)sf.AddrPC.Offset, &hm))
      GetModuleFileNameA(hm, mod, MAX_PATH);
    const char* modName = strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod;
    const bool haveSym = SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym);
    const bool haveLine = SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &ld, &line);
    std::printf("  #%02d %s!%s", i, modName, haveSym ? sym->Name : "?");
    if (haveLine)
      std::printf("  (%s:%lu)", line.FileName, line.LineNumber);
    std::printf("\n");
  }
  std::fflush(stdout);
  return EXCEPTION_EXECUTE_HANDLER;
}

inline void InstallCrashTrace() { SetUnhandledExceptionFilter(CrashTraceFilter); }
