#include "heaphook.h"

#define NOMINMAX
#include <Windows.h>
#include <cstring>

namespace heaphook
{
Counts gCounts;

static thread_local bool tArmed = false;

using HeapAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapFreeFn = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID);
using HeapReAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);

static HeapAllocFn sHeapAlloc = nullptr;
static HeapFreeFn sHeapFree = nullptr;
static HeapReAllocFn sHeapReAlloc = nullptr;

static LPVOID WINAPI HookHeapAlloc(HANDLE h, DWORD flags, SIZE_T n)
{
  LPVOID p = sHeapAlloc(h, flags, n);
  if (p != nullptr)
  {
    gCounts.totalAlloc++;
    if (tArmed)
      gCounts.armedAlloc++;
  }
  return p;
}

static BOOL WINAPI HookHeapFree(HANDLE h, DWORD flags, LPVOID p)
{
  const BOOL ok = sHeapFree(h, flags, p);
  if (ok && p != nullptr)
  {
    gCounts.totalFree++;
    if (tArmed)
      gCounts.armedFree++;
  }
  return ok;
}

static LPVOID WINAPI HookHeapReAlloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T n)
{
  if (tArmed)
    gCounts.armedRealloc++;
  return sHeapReAlloc(h, flags, p, n);
}

void Arm(bool on) { tArmed = on; }

bool Install(const wchar_t* moduleFileName)
{
  HMODULE mod = GetModuleHandleW(moduleFileName);
  if (mod == nullptr)
    return false;
  auto* base = reinterpret_cast<BYTE*>(mod);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (dir.VirtualAddress == 0)
    return false;

  int patched = 0;
  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name != 0; imp++)
  {
    const char* dll = reinterpret_cast<const char*>(base + imp->Name);
    if (_stricmp(dll, "KERNEL32.dll") != 0)
      continue;
    auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    auto* addrThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; nameThunk->u1.AddressOfData != 0; nameThunk++, addrThunk++)
    {
      if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal))
        continue;
      auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
      void* hook = nullptr;
      void** original = nullptr;
      if (strcmp(byName->Name, "HeapAlloc") == 0)
      {
        hook = reinterpret_cast<void*>(&HookHeapAlloc);
        original = reinterpret_cast<void**>(&sHeapAlloc);
      }
      else if (strcmp(byName->Name, "HeapFree") == 0)
      {
        hook = reinterpret_cast<void*>(&HookHeapFree);
        original = reinterpret_cast<void**>(&sHeapFree);
      }
      else if (strcmp(byName->Name, "HeapReAlloc") == 0)
      {
        hook = reinterpret_cast<void*>(&HookHeapReAlloc);
        original = reinterpret_cast<void**>(&sHeapReAlloc);
      }
      if (hook == nullptr)
        continue;
      DWORD old;
      VirtualProtect(&addrThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
      *original = reinterpret_cast<void*>(addrThunk->u1.Function);
      addrThunk->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
      VirtualProtect(&addrThunk->u1.Function, sizeof(void*), old, &old);
      patched++;
    }
  }
  return patched == 3;
}
} // namespace heaphook
