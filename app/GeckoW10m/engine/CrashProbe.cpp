#include "pch.h"

#include "CrashProbe.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winstring.h>
#include <inspectable.h>
#include <winrt/Windows.System.h>

#include <delayimp.h>

#include <atomic>
#include <setjmp.h>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "../client/Log.h"

// The app partition of errhandlingapi.h hides this one, though kernel32 exports
// it and an app container may call it. Declared here rather than compiling the
// whole file for the desktop partition, which would drag the A/W macros in
// ahead of everything else.
extern "C" {
using GeckoW10mVectoredHandler = LONG(CALLBACK*)(PEXCEPTION_POINTERS);
__declspec(dllimport) PVOID WINAPI
AddVectoredExceptionHandler(ULONG First, GeckoW10mVectoredHandler Handler);
}

namespace gecko_w10m::engine {
namespace {

using gecko_w10m::client::Log;

std::wstring Widen(const char* s) {
  if (!s) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (n <= 1) return std::wstring();
  std::wstring out(static_cast<size_t>(n - 1), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
  return out;
}

std::wstring Hex(uintptr_t v) {
  wchar_t buf[19];
  ::swprintf_s(buf, L"0x%08llx", static_cast<unsigned long long>(v));
  return buf;
}

// An address on its own says nothing across runs -- ASLR moves every module.
// Named against its module and offset it stays meaningful, and an offset into
// xul.dll resolves through tools/symbolize.py against that build's map.
std::wstring DescribeAddress(const void* addr) {
  HMODULE mod = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &mod) ||
      !mod) {
    return Hex(reinterpret_cast<uintptr_t>(addr)) + L" (no module)";
  }

  wchar_t path[MAX_PATH] = {};
  DWORD n = ::GetModuleFileNameW(mod, path, MAX_PATH);
  std::wstring name(path, n);
  auto slash = name.find_last_of(L'\\');
  if (slash != std::wstring::npos) name = name.substr(slash + 1);

  uintptr_t rva =
      reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
  return name + L"+" + Hex(rva);
}

// RtlCaptureStackBackTrace returns one frame here and stops: 32-bit ARM
// Windows unwinds from tables rather than a frame pointer chain, and without
// consulting them there is no second frame to find. Walking those tables gives
// a real trace -- and, unlike the capture, one that can start from the context
// of the thread that faulted rather than from the handler's own stack.
#if defined(_M_X64)
using ContextAddress = DWORD64;
using LookupFunctionEntryFn = PVOID(WINAPI*)(DWORD64, PDWORD64, PVOID);
using VirtualUnwindFn = PVOID(WINAPI*)(DWORD, DWORD64, DWORD64, PVOID,
                                       PCONTEXT, PVOID*, PDWORD64, PVOID);
ContextAddress& ContextPc(CONTEXT& context) { return context.Rip; }
ContextAddress ContextPc(const CONTEXT& context) { return context.Rip; }
ContextAddress ContextSp(const CONTEXT& context) { return context.Rsp; }
ContextAddress ContextThis(const CONTEXT& context) { return context.Rcx; }
#else
using ContextAddress = DWORD;
using LookupFunctionEntryFn = PVOID(WINAPI*)(DWORD, PDWORD, PVOID);
using VirtualUnwindFn = PVOID(WINAPI*)(DWORD, DWORD, DWORD, PVOID, PCONTEXT,
                                       PVOID*, PDWORD, PVOID);
ContextAddress& ContextPc(CONTEXT& context) { return context.Pc; }
ContextAddress ContextPc(const CONTEXT& context) { return context.Pc; }
ContextAddress ContextSp(const CONTEXT& context) { return context.Sp; }
ContextAddress ContextThis(const CONTEXT& context) { return context.R0; }
#endif

LookupFunctionEntryFn gLookupFunctionEntry = nullptr;
VirtualUnwindFn gVirtualUnwind = nullptr;

void ResolveUnwindApis() {
  HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return;
  gLookupFunctionEntry = reinterpret_cast<LookupFunctionEntryFn>(
      ::GetProcAddress(ntdll, "RtlLookupFunctionEntry"));
  gVirtualUnwind = reinterpret_cast<VirtualUnwindFn>(
      ::GetProcAddress(ntdll, "RtlVirtualUnwind"));
}

// Takes the context by value: unwinding rewrites it, and the caller's copy
// belongs to the exception record.
void LogStack(const wchar_t* tag, CONTEXT context) {
  if (!gLookupFunctionEntry || !gVirtualUnwind) {
    Log::WriteFromFault(std::wstring(tag) + L"   (no unwind support)");
    return;
  }

  for (int depth = 0; depth < 32; ++depth) {
    ContextAddress& pc = ContextPc(context);
    if (!pc) return;
    Log::WriteFromFault(std::wstring(tag) + L"   " +
               DescribeAddress(reinterpret_cast<void*>(pc)));

    ContextAddress imageBase = 0;
    PVOID entry = gLookupFunctionEntry(pc, &imageBase, nullptr);
    if (!entry) return;

    PVOID handlerData = nullptr;
    ContextAddress establisher = 0;
    ContextAddress previous = pc;
    gVirtualUnwind(0 /*UNW_FLAG_NHANDLER*/, imageBase, pc, entry,
                   &context, &handlerData, &establisher, nullptr);
    // A frame that does not move is a frame that will not move again.
    if (ContextPc(context) == previous) return;
  }
}

// When the walk above stops at once -- a fault mid-prologue leaves the unwinder
// with nothing but lr, and lr is not always a return address -- this is what is
// left: read the raw stack and keep the words that RtlLookupFunctionEntry
// recognizes as belonging to a function. That filter is what makes it worth
// reading; a plausible-looking integer is not a return address unless something
// claims to be able to unwind it. Stale frames survive on a stack, so these are
// candidates, not a call chain.
void LogStackScan(const wchar_t* tag, ContextAddress sp) {
  if (!gLookupFunctionEntry) return;

  MEMORY_BASIC_INFORMATION region{};
  const uintptr_t* start = reinterpret_cast<const uintptr_t*>(sp);
  if (!::VirtualQuery(start, &region, sizeof(region))) return;
  const uintptr_t* limit = reinterpret_cast<const uintptr_t*>(
      reinterpret_cast<const char*>(region.BaseAddress) + region.RegionSize);

  Log::WriteFromFault(std::wstring(tag) + L"   -- stack scan (candidates) --");
  int printed = 0;
  for (const uintptr_t* word = start; word < limit && printed < 24; ++word) {
    const uintptr_t value = *word;
    if (value < 0x10000) continue;
    ContextAddress imageBase = 0;
    if (!gLookupFunctionEntry(static_cast<ContextAddress>(value), &imageBase,
                              nullptr)) {
      continue;
    }
    Log::WriteFromFault(std::wstring(tag) + L"   ?? " +
               DescribeAddress(reinterpret_cast<void*>(value)));
    ++printed;
  }
}

// For the hooks, which have no exception context of their own.
// A phone gives an app a hard memory ceiling, and 132 MB of engine plus
// everything Gecko allocates on startup is a real candidate for reaching it.
// Whatever else a crash report says, it should say how close this was.
// How much of the process's address space is left, and how large the biggest
// unbroken piece of it is.
//
// AppMemoryUsage, which is all this has ever reported, is the commit charge
// the system bills the app for. It is not the address space, and on a 32-bit
// process the address space is the smaller of the two: two gigabytes, shared
// between a 132 MB xul.dll, a 32 MB JIT reserve, Gecko's heaps, and every
// texture a Direct3D driver maps into it. An allocation does not fail because
// the total ran out; it fails because no single free run was big enough. That
// distinction has never been measured here and the fault is an allocation
// coming back null.
struct FreeSpace {
  unsigned totalMB;
  unsigned largestMB;
  unsigned pieces;
};

FreeSpace SurveyAddressSpace() {
  FreeSpace out{0, 0, 0};
  uint64_t total = 0;
  uint64_t largest = 0;
  uintptr_t at = 0x10000;
  MEMORY_BASIC_INFORMATION info{};
  while (::VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) ==
         sizeof(info)) {
    if (info.State == MEM_FREE) {
      total += info.RegionSize;
      if (info.RegionSize > largest) largest = info.RegionSize;
      ++out.pieces;
    }
    const uintptr_t next =
        reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (next <= at) break;
    at = next;
  }
  out.totalMB = static_cast<unsigned>(total / (1024 * 1024));
  out.largestMB = static_cast<unsigned>(largest / (1024 * 1024));
  return out;
}

void LogAddressSpace(const wchar_t* tag) {
  const FreeSpace free = SurveyAddressSpace();
  Log::WriteFromFault(std::wstring(tag) + L" address space " +
                      std::to_wstring(free.totalMB) + L" MB free in " +
                      std::to_wstring(free.pieces) + L" pieces, largest " +
                      std::to_wstring(free.largestMB) + L" MB");
}

void LogMemory(const wchar_t* tag) {
  auto used = winrt::Windows::System::MemoryManager::AppMemoryUsage();
  auto limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
  Log::WriteFromFault(std::wstring(tag) + L" memory " +
             std::to_wstring(used / (1024 * 1024)) + L" MB of " +
             std::to_wstring(limit / (1024 * 1024)) + L" MB");
}

void LogBacktrace(const wchar_t* tag) {
  CONTEXT context{};
  ::RtlCaptureContext(&context);
  LogStack(tag, context);
}

// ---------------------------------------------------------------- exceptions

// MOZ_CRASH lands here as a breakpoint, which is why 0x80000003 counts as
// fatal: it is how Gecko says "this is unrecoverable" on Windows.
bool IsFatal(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_BREAKPOINT:
    case STATUS_HEAP_CORRUPTION:
    // The fail-fast family, which this filter has been dropping on the floor
    // for the whole project. A stack cookie check, a Control Flow Guard
    // violation, abort(), an invalid CRT parameter and every MOZ_RELEASE_ASSERT
    // that goes through RaiseFailFastException end the process with one of
    // these -- and being outside this switch, they were reported as nothing at
    // all. The hardware build dies with no first-chance of any kind before the
    // compositor's teardown, and this is the most likely reason we cannot see
    // it.
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN -- also __fastfail
    case 0xC0000602:  // STATUS_FAIL_FAST_EXCEPTION
    case 0xC0000417:  // STATUS_INVALID_CRUNTIME_PARAMETER
    case 0xC0000420:  // STATUS_ASSERTION_FAILURE
    case 0xC000041D:  // STATUS_FATAL_USER_CALLBACK_EXCEPTION
      return true;
    default:
      return false;
  }
}

// An access violation carries the address it was reaching for and whether it
// was reading or writing. Null says a missing object; a small offset off null
// says a field of one; an address that looks like data says something worse.
// The exception address alone distinguishes none of those.
std::wstring FaultDetail(const EXCEPTION_RECORD& record) {
  if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      record.NumberParameters < 2) {
    return std::wstring();
  }
  const ULONG_PTR kind = record.ExceptionInformation[0];
  const wchar_t* verb = kind == 0   ? L"reading "
                        : kind == 1 ? L"writing "
                                    : L"executing ";
  return std::wstring(L", ") + verb +
         DescribeAddress(
             reinterpret_cast<const void*>(record.ExceptionInformation[1]));
}

// Which register held the address that faulted, and what the instruction was.
// With no symbols and no debugger this is what is left, and between the two it
// is usually enough to find the faulting line in the source.
std::wstring Registers(const CONTEXT& context) {
#if defined(_M_X64)
  const DWORD64 values[] = {
      context.Rax, context.Rbx, context.Rcx, context.Rdx, context.Rsi,
      context.Rdi, context.R8,  context.R9,  context.R10, context.R11,
      context.R12, context.R13, context.R14, context.R15};
  static const wchar_t* names[] = {
      L" rax=", L" rbx=", L" rcx=", L" rdx=", L" rsi=", L" rdi=",
      L" r8=",  L" r9=",  L" r10=", L" r11=", L" r12=", L" r13=",
      L" r14=", L" r15="};
  std::wstring out;
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    out += names[i] + Hex(values[i]);
  }
  return out + L" rsp=" + Hex(context.Rsp) + L" rbp=" + Hex(context.Rbp);
#else
  const DWORD values[] = {context.R0,  context.R1,  context.R2,  context.R3,
                          context.R4,  context.R5,  context.R6,  context.R7,
                          context.R8,  context.R9,  context.R10, context.R11,
                          context.R12};
  std::wstring out;
  for (int i = 0; i < 13; ++i) {
    out += L" r" + std::to_wstring(i) + L"=" + Hex(values[i]);
  }
  return out + L" sp=" + Hex(context.Sp) + L" lr=" + Hex(context.Lr);
#endif
}

// Thumb-2, so an instruction is two bytes or four and there is no telling
// which without decoding it. Sixteen bytes covers the faulting one and its
// neighbours, which is what makes it readable.
std::wstring CodeAt(const void* pc) {
  // The low bit of a Thumb address is the mode flag, not part of the address.
  auto* bytes = reinterpret_cast<const unsigned char*>(
      reinterpret_cast<uintptr_t>(pc) & ~static_cast<uintptr_t>(1));
  std::wstring out;
  for (int i = 0; i < 16; ++i) {
    wchar_t pair[4];
    ::swprintf_s(pair, L"%02x ", bytes[i]);
    out += pair;
  }
  return out;
}

std::atomic<int> gReported{0};
constexpr int kMaxReports = 4;

// The bytes at the faulting instruction. Every report so far has said where
// the fault was and what the registers held, and none has said what the
// instruction actually does -- which is the one thing that would name the
// register that was null instead of leaving it to be guessed at.
void LogInstruction(const wchar_t* tag, void* address) {
  if (!address) {
    return;
  }
  const uint8_t* at = reinterpret_cast<const uint8_t*>(
      reinterpret_cast<uintptr_t>(address) & ~uintptr_t(1));
  // Thumb-2 is two or four bytes; eight covers the faulting one and its
  // neighbour, which is usually enough to read the addressing mode.
  // Check the page is really there first. There is no IsBadReadPtr in an app
  // container, and faulting inside the fault handler would be the end of it.
  MEMORY_BASIC_INFORMATION info = {};
  if (!::VirtualQuery(at, &info, sizeof(info)) ||
      info.State != MEM_COMMIT ||
      (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
    return;
  }
  const uintptr_t end =
      reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
  wchar_t bytes[64] = {};
  wchar_t* out = bytes;
  for (int i = 0; i < 8; ++i) {
    if (reinterpret_cast<uintptr_t>(at + i) >= end) {
      break;
    }
    out += swprintf(out, 4, L"%02x ", at[i]);
  }
  Log::WriteFromFault(std::wstring(tag) + L"   instruction at " +
                      (reinterpret_cast<uintptr_t>(address) & 1 ? L"thumb "
                                                                : L"arm ") +
                      bytes);
}

// The UI thread's frame count, and the thread itself, so a fault can say
// whether it happened where the frames are drawn.
std::atomic<unsigned> gUiFrames{0};
std::atomic<DWORD> gUiThread{0};
std::atomic<HANDLE> gUiThreadHandle{nullptr};
std::atomic<ULONGLONG> gLastFrameAt{0};
std::atomic<HRESULT> gLastRemovedReason{S_OK};
ID3D11Device* gProbeDevice = nullptr;

// The engine's own device and renderer-thread counters, resolved from xul.dll
// once it is loaded. The hardware path hides the window while the renderer
// thread is inside its first content frame; polling from here tells whether
// that thread is stuck in a GL call and whether the engine's device was reset.
using DeviceFn = void* (*)();
using CounterFn = uint32_t (*)();
DeviceFn gGeckoDeviceFn = nullptr;
CounterFn gWrPulseFn = nullptr;
CounterFn gWrWhereFn = nullptr;
CounterFn gWrThreadFn = nullptr;
std::atomic<HRESULT> gGeckoRemovedReason{S_OK};
std::atomic<bool> gAutopsyDone{false};
void AutopsyRendererThread(const wchar_t* tag);
std::atomic<uint32_t> gLastWrPulse{0};
std::atomic<unsigned> gWrStillBeats{0};

void ResolveEngineProbes() {
  if (gGeckoDeviceFn) {
    return;
  }
  HMODULE xul = ::GetModuleHandleW(L"xul.dll");
  if (!xul) {
    return;
  }
  gGeckoDeviceFn = reinterpret_cast<DeviceFn>(::GetProcAddress(xul, "gecko_w10m_gecko_device"));
  gWrPulseFn = reinterpret_cast<CounterFn>(::GetProcAddress(xul, "gecko_w10m_wr_pulse"));
  gWrWhereFn = reinterpret_cast<CounterFn>(::GetProcAddress(xul, "gecko_w10m_wr_where"));
  gWrThreadFn = reinterpret_cast<CounterFn>(::GetProcAddress(xul, "gecko_w10m_wr_thread"));
  Log::WriteFromFault(std::wstring(L"engine probes: ") +
                      (gGeckoDeviceFn ? L"device " : L"NO device ") +
                      (gWrPulseFn ? L"pulse " : L"NO pulse ") +
                      (gWrWhereFn ? L"where" : L"NO where"));
}

HRESULT GeckoDeviceRemovedReason() {
  if (!gGeckoDeviceFn) {
    return S_OK;
  }
  auto* device = static_cast<ID3D11Device*>(gGeckoDeviceFn());
  return device ? device->GetDeviceRemovedReason() : S_OK;
}

DWORD WINAPI HeartbeatThread(LPVOID) {
  unsigned last = 0;
  unsigned beat = 0;
  // Quiet (the default): every two seconds, and it speaks only when a device
  // was reset or the process was frozen -- no line per beat, no flush, no
  // survey of the address space, no renderer autopsy.
  const bool verbose = Log::Verbose();
  const DWORD interval = verbose ? 250 : 2000;
  while (true) {
    // Quarter-second, and carrying how long ago the last frame was. At half a
    // second with only a count, a beat that lands sixteen milliseconds after a
    // fault reports the same thirty frames whether the thread died at the
    // fault or drew straight through it -- which is exactly the reading I got
    // wrong.
    const ULONGLONG before = ::GetTickCount64();
    ::Sleep(interval);
    const ULONGLONG slept = ::GetTickCount64() - before;
    const unsigned frames = gUiFrames.load();
    const ULONGLONG age = ::GetTickCount64() - gLastFrameAt.load();
    // A quarter-second sleep that took two seconds is not a slow thread, it is
    // a stopped process: the last run drew frames for two and a half seconds
    // past the mended fault and then everything, this thread included, went
    // quiet at once. That is a suspend or a kill, not a fault, and the two
    // should not look alike in the log.
    if (slept > interval + 750) {
      Log::WriteFromFault(L"alive: the whole process was frozen for " +
                          std::to_wstring(slept) +
                          L" ms -- nothing ran, not even this");
    }
    // The fault-safe path: the ordinary one ends by posting to the UI thread,
    // and a pulse that adds work to the thread it is watching measures itself.
    ++beat;
    std::wstring line = L"alive: beat " + std::to_wstring(beat) +
                        L", ui frames " + std::to_wstring(frames) + L" (+" +
                        std::to_wstring(frames - last) + L"), last one " +
                        std::to_wstring(age) + L" ms ago";
    if (gProbeDevice) {
      const HRESULT removed = gProbeDevice->GetDeviceRemovedReason();
      if (removed != gLastRemovedReason.load()) {
        gLastRemovedReason.store(removed);
        Log::WriteFromFault(L"gpu: the shell's device now says " +
                            Hex(static_cast<uintptr_t>(removed)) +
                            (removed == S_OK ? L" (alive)"
                                             : L" -- THE GPU WAS RESET"));
      }
    }
    ResolveEngineProbes();
    if (gGeckoDeviceFn) {
      const HRESULT removed = GeckoDeviceRemovedReason();
      if (removed != gGeckoRemovedReason.load()) {
        gGeckoRemovedReason.store(removed);
        Log::WriteFromFault(L"gpu: the ENGINE's device now says " +
                            Hex(static_cast<uintptr_t>(removed)) +
                            (removed == S_OK ? L" (alive)"
                                             : L" -- THE ENGINE'S DEVICE WAS RESET"));
      }
    }
    if (!verbose) {
      continue;
    }
    if (gWrPulseFn && gWrWhereFn) {
      const uint32_t pulse = gWrPulseFn();
      const uint32_t where = gWrWhereFn();
      if (pulse == gLastWrPulse.load()) {
        ++gWrStillBeats;
      } else {
        gWrStillBeats = 0;
      }
      gLastWrPulse.store(pulse);
      line += L", wr pulse " + std::to_wstring(pulse) + L" where " +
              std::to_wstring(where);
      if (where != 0 && gWrStillBeats.load() >= 2) {
        line += L" -- THE RENDERER THREAD IS STUCK THERE";
      }
      // Only a thread that stopped inside a GL call is worth opening up; on
      // the software path the pulse simply never moves.
      if (pulse > 0 && where != 0 && gWrStillBeats.load() == 8) {
        AutopsyRendererThread(L"autopsy(quiet):");
      }
    }
    if (beat % 4 == 0) {
      const FreeSpace free = SurveyAddressSpace();
      line += L", address space " + std::to_wstring(free.totalMB) +
              L" MB free in " + std::to_wstring(free.pieces) +
              L" pieces, largest " + std::to_wstring(free.largestMB) + L" MB";
    }
    Log::WriteFromFault(line);
    // Flushed every beat now. The log has ended mid-thought twice and there is
    // no way to tell a process that stopped from a line that never reached the
    // disk.
    Log::FlushFromFault();
    last = frames;
  }
}

// Somewhere to put the word CoreUIComponents wants to write, since it has
// nowhere of its own. A bump pointer over a megabyte, sixteen bytes at a time:
// sixty-five thousand repairs, which is far more than a run has frames.
std::atomic<uintptr_t> gScratchNext{0};
uintptr_t gScratchEnd = 0;
std::atomic<int> gRepairs{0};

void ReserveScratch() {
  void* block = ::VirtualAllocFromApp(nullptr, 1u << 20, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_READWRITE);
  if (!block) {
    Log::WriteNum(L"repair: no scratch, err", ::GetLastError());
    return;
  }
  gScratchNext.store(reinterpret_cast<uintptr_t>(block));
  gScratchEnd = reinterpret_cast<uintptr_t>(block) + (1u << 20);
}

// The fault is one instruction: STR.W r8, [r8] with r8 zero, which is a list
// head being made to point at itself -- head->next = head -- in memory that
// was asked for and not given. Seven measurements have now shown that nothing
// about this process is short of anything: not memory, not the address space,
// not the GPU, which went on taking work for a full second after the UI thread
// had stopped. The allocation that failed is the system's own, inside a
// component we cannot build, and there is no version of this browser in which
// we fix it.
//
// What we can do is give the store an address. r8 is popped off the stack by
// the very next instruction, so the substitution lives for exactly one write
// and changes nothing else; and what it writes is a list head pointing at
// itself, which is precisely what an empty list looks like. If that word was
// all the component needed, the compositor carries on. If it needed the object
// the word was supposed to live in, it will fault again somewhere new -- which
// is a different address, and a different thing to chase.
bool TryRepair(PEXCEPTION_POINTERS info) {
#if defined(_M_X64)
  // This workaround recognizes and modifies one specific ARM Thumb-2
  // instruction/register state. It is neither applicable nor safe on x64.
  return false;
#else
  const EXCEPTION_RECORD& record = *info->ExceptionRecord;
  if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return false;
  if (record.NumberParameters < 2) return false;
  if (record.ExceptionInformation[0] != 1) return false;   // a write
  if (record.ExceptionInformation[1] != 0) return false;   // through zero
  if (info->ContextRecord->R8 != 0) return false;

  // And it is that instruction, not merely a write through zero that happens
  // to look like it.
  const uint8_t* at = reinterpret_cast<const uint8_t*>(
      reinterpret_cast<uintptr_t>(record.ExceptionAddress) & ~uintptr_t(1));
  MEMORY_BASIC_INFORMATION page = {};
  if (!::VirtualQuery(at, &page, sizeof(page)) || page.State != MEM_COMMIT) {
    return false;
  }
  if (!(at[0] == 0xc8 && at[1] == 0xf8 && at[2] == 0x00 && at[3] == 0x80)) {
    return false;
  }

  const uintptr_t where = gScratchNext.fetch_add(16);
  if (!where || where + 16 > gScratchEnd) return false;

  info->ContextRecord->R8 = static_cast<DWORD>(where);
  const int nth = gRepairs.fetch_add(1) + 1;
  if (nth <= 3 || nth % 100 == 0) {
    Log::WriteFromFault(L"repair: gave the compositor somewhere to write, " +
                        std::to_wstring(nth) + L" so far");
    Log::FlushFromFault();
  }
  return true;
#endif
}

// Words of an object, each resolved to module+offset when it points into
// one, and followed one level when it points at something that itself points
// into a module -- which is what a vtable pointer looks like from outside.
void DumpObject(const wchar_t* tag, uintptr_t at, size_t bytes) {
  MEMORY_BASIC_INFORMATION page = {};
  if (!at || !::VirtualQuery(reinterpret_cast<void*>(at), &page, sizeof(page)) ||
      page.State != MEM_COMMIT) {
    Log::WriteFromFault(std::wstring(L"object ") + tag + L": unreadable");
    return;
  }
  const uintptr_t end = reinterpret_cast<uintptr_t>(page.BaseAddress) + page.RegionSize;
  Log::WriteFromFault(std::wstring(L"object ") + tag + L" at " + Hex(at));
  for (size_t off = 0; off < bytes && at + off + 4 <= end; off += 4) {
    const uintptr_t word = *reinterpret_cast<const uintptr_t*>(at + off);
    if (!word) continue;
    std::wstring where = DescribeAddress(reinterpret_cast<void*>(word));
    std::wstring line = L"  +" + Hex(off) + L"  " + Hex(word);
    if (where.find(L"(no module)") == std::wstring::npos) {
      line += L"  -> " + where;
    } else {
      // A heap pointer: is its first word a vtable?
      MEMORY_BASIC_INFORMATION inner = {};
      if (::VirtualQuery(reinterpret_cast<void*>(word), &inner, sizeof(inner)) &&
          inner.State == MEM_COMMIT && !(inner.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        const uintptr_t first = *reinterpret_cast<const uintptr_t*>(word);
        std::wstring vt = DescribeAddress(reinterpret_cast<void*>(first));
        if (first && vt.find(L"(no module)") == std::wstring::npos) {
          line += L"  -> object whose vtable is " + vt;
        }
      }
    }
    Log::WriteFromFault(line);
  }
  Log::FlushFromFault();
}

LONG CALLBACK OnException(PEXCEPTION_POINTERS info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (!IsFatal(code)) return EXCEPTION_CONTINUE_SEARCH;
  // Reported in full the first time, then mended every time. Without the
  // report there is no evidence it is still the same fault; without the mend
  // there is no browser.
  const bool mendable = TryRepair(info);
  if (mendable && gReported.load() > 0) {
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  const int nth = gReported.fetch_add(1);
  if (nth == kMaxReports) {
    Log::WriteFromFault(
        L"first-chance: further reports suppressed -- the cap is reached, so "
        L"do not read later silence as no further faults");
  }
  if (nth >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;

  // First chance means exactly that: the process may well have a handler for
  // this and carry on. Said plainly so it is not read as a cause of death.
  Log::WriteFromFault(L"first-chance: code " + Hex(code) + L" at " +
             DescribeAddress(info->ExceptionRecord->ExceptionAddress) +
             FaultDetail(*info->ExceptionRecord) + L", thread " +
             std::to_wstring(::GetCurrentThreadId()) +
             (::GetCurrentThreadId() == gUiThread.load() ? L" (the UI thread)"
                                                         : L""));
  LogMemory(L"first-chance:");
  LogAddressSpace(L"first-chance:");
  if (gProbeDevice) {
    Log::WriteFromFault(L"first-chance: gpu device removed reason " +
                        Hex(static_cast<uintptr_t>(gProbeDevice->GetDeviceRemovedReason())));
  }
  if (gGeckoDeviceFn) {
    Log::WriteFromFault(L"first-chance: the ENGINE's device removed reason " +
                        Hex(static_cast<uintptr_t>(GeckoDeviceRemovedReason())));
  }
  if (gWrPulseFn && gWrWhereFn) {
    Log::WriteFromFault(L"first-chance: wr pulse " + std::to_wstring(gWrPulseFn()) +
                        L" where " + std::to_wstring(gWrWhereFn()));
  }
  AutopsyRendererThread(L"autopsy(fault):");
  Log::WriteFromFault(L"first-chance: code" + Registers(*info->ContextRecord));
  LogInstruction(L"first-chance:", info->ExceptionRecord->ExceptionAddress);
  LogStack(L"first-chance:", *info->ContextRecord);
  LogStackScan(L"first-chance:", ContextSp(*info->ContextRecord));
  Log::FlushFromFault();

  if (mendable) {
    // The object the assertion is about. r0 is `this` for the state setter
    // and r6 and r4 hold copies of it. Its fields are the pointers that name
    // things: +0x12c the state, +0xb4 the interface asked whether the view is
    // shown, +0xc0 an object with a vtable, +0x128 a window handle. Every
    // word that points into a module is written as module+offset, and a word
    // at a vtable is a class once the module's RTTI is read against it.
    DumpObject(L"this", ContextThis(*info->ContextRecord), 0x140);
    Log::WriteFromFault(
        L"repair: r8 had nowhere to point, so it was given somewhere -- "
        L"carrying on from the instruction that faulted");
    Log::FlushFromFault();
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  // Not ours to handle -- only to record.
  return EXCEPTION_CONTINUE_SEARCH;
}

// -------------------------------------------------------------- import hooks
//
// Two jobs, one mechanism. A process ended through abort, _exit,
// TerminateProcess or ExitProcess raises nothing, so no handler of any kind
// sees it go -- those get hooked so the caller can be named while its stack is
// still standing. And with the control run showing that the XAML fault only
// happens when the engine runs, the rest are here to say what the engine was
// asking the system for on its way there: which libraries it pulled in, and
// what it was raising, since every trace so far has ended inside
// RaiseException.
//
// Every hook records and calls straight through. None of them change what
// happens.

using AbortFn = void(__cdecl*)();
using ExitFn = void(__cdecl*)(int);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
using ExitProcessFn = void(WINAPI*)(UINT);
using RaiseExceptionFn = void(WINAPI*)(DWORD, DWORD, DWORD, const ULONG_PTR*);
using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadPackagedLibraryFn = HMODULE(WINAPI*)(LPCWSTR, DWORD);

AbortFn gRealAbort = nullptr;
ExitFn gRealExit = nullptr;
TerminateProcessFn gRealTerminateProcess = nullptr;
ExitProcessFn gRealExitProcess = nullptr;
RaiseExceptionFn gRealRaiseException = nullptr;
LoadLibraryExWFn gRealLoadLibraryExW = nullptr;
LoadPackagedLibraryFn gRealLoadPackagedLibrary = nullptr;

void __cdecl HookAbort() {
  Log::WriteFromFault(L"kill: the engine called abort()");
  LogBacktrace(L"kill:");
  if (gRealAbort) gRealAbort();
  ::TerminateProcess(::GetCurrentProcess(), 3);
}

void __cdecl HookExit(int code) {
  Log::WriteNum(L"kill: the engine called _exit", code);
  LogBacktrace(L"kill:");
  if (gRealExit) gRealExit(code);
}

BOOL WINAPI HookTerminateProcess(HANDLE process, UINT code) {
  Log::WriteNum(L"kill: the engine called TerminateProcess, code", code);
  LogBacktrace(L"kill:");
  return gRealTerminateProcess ? gRealTerminateProcess(process, code) : FALSE;
}

void WINAPI HookExitProcess(UINT code) {
  Log::WriteNum(L"kill: the engine called ExitProcess, code", code);
  LogBacktrace(L"kill:");
  if (gRealExitProcess) gRealExitProcess(code);
}

// Naming a thread is done by raising an exception a debugger is meant to
// swallow, and Gecko names a lot of threads. Reporting those would bury
// everything else.
constexpr DWORD kThreadNameException = 0x406D1388;

std::atomic<int> gRaiseReports{0};

// The delay-load helper reports a missing library or function by raising, and
// the argument it raises with names both. Without reading it, all a delay-load
// failure says is that something somewhere was not there.
constexpr DWORD kDelayLoadModuleMissing = 0xC06D007E;
constexpr DWORD kDelayLoadProcMissing = 0xC06D007F;

void LogDelayLoadFailure(DWORD code, DWORD count, const ULONG_PTR* args) {
  if (!count || !args) return;
  auto* info = reinterpret_cast<const DelayLoadInfo*>(args[0]);
  if (!info) return;

  std::wstring dll = info->szDll ? Widen(info->szDll) : L"(unnamed)";
  std::wstring what =
      code == kDelayLoadModuleMissing
          ? L" did not load"
          : (info->dlp.fImportByName
                 ? L"!" + Widen(info->dlp.szProcName) + L" not found"
                 : L" ordinal not found");
  Log::WriteFromFault(L"delay-load: " + dll + what + L", err " +
             std::to_wstring(info->dwLastError));
}

void WINAPI HookRaiseException(DWORD code, DWORD flags, DWORD count,
                               const ULONG_PTR* args) {
  if (code == kDelayLoadModuleMissing || code == kDelayLoadProcMissing) {
    LogDelayLoadFailure(code, count, args);
  } else if (code != kThreadNameException && gRaiseReports.fetch_add(1) < 12) {
    Log::WriteFromFault(L"raise: the engine raised " + Hex(code));
    LogBacktrace(L"raise:");
  }
  if (gRealRaiseException) gRealRaiseException(code, flags, count, args);
}

HMODULE WINAPI HookLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
  HMODULE module = gRealLoadLibraryExW(name, file, flags);
  Log::WriteFromFault(L"load: " + std::wstring(name ? name : L"(null)") +
             (module ? L"" : L"  FAILED"));
  return module;
}

HMODULE WINAPI HookLoadPackagedLibrary(LPCWSTR name, DWORD reserved) {
  HMODULE module = gRealLoadPackagedLibrary(name, reserved);
  Log::WriteFromFault(L"load: " + std::wstring(name ? name : L"(null)") +
             (module ? L" (packaged)" : L" (packaged)  FAILED"));
  return module;
}

// longjmp on 32-bit ARM unwinds the stack frame by frame on its way back to
// the setjmp, the way a C++ exception would, and it trusts the unwind tables
// to describe every frame in between. clang's do not, on this target, and
// the unwinder stops with STATUS_BAD_STACK -- a fatal that no handler sees.
// libpng and libjpeg report a bad image exactly this way (png_error ->
// longjmp), so any truncated PNG on a page ended the browser: 0.2.5.2 died on
// a Spotify page in MOZ_PNG_longjmp.
//
// x86 never unwound on longjmp and Gecko is written for that, so the unwind
// is simply switched off: a jump buffer whose Frame is zero is restored
// register by register, and nothing in between is visited.
using LongjmpFn = void(__cdecl*)(jmp_buf, int);
LongjmpFn gRealLongjmp = nullptr;
std::atomic<int> gLongjmpsTaken{0};

__declspec(noreturn) void __cdecl HookLongjmp(jmp_buf buffer, int value) {
  reinterpret_cast<_JUMP_BUFFER*>(buffer)->Frame = 0;
  if (gLongjmpsTaken.fetch_add(1) < 5) {
    Log::WriteFromFault(L"longjmp: taken without unwinding the stack");
  }
  gRealLongjmp(buffer, value);
  ::TerminateProcess(::GetCurrentProcess(), 3);  // not reached
}

struct Hook {
  const char* name;
  void* replacement;
  void** original;
};

const Hook kHooks[] = {
    {"abort", reinterpret_cast<void*>(&HookAbort),
     reinterpret_cast<void**>(&gRealAbort)},
    {"_exit", reinterpret_cast<void*>(&HookExit),
     reinterpret_cast<void**>(&gRealExit)},
    {"TerminateProcess", reinterpret_cast<void*>(&HookTerminateProcess),
     reinterpret_cast<void**>(&gRealTerminateProcess)},
    {"ExitProcess", reinterpret_cast<void*>(&HookExitProcess),
     reinterpret_cast<void**>(&gRealExitProcess)},
    {"RaiseException", reinterpret_cast<void*>(&HookRaiseException),
     reinterpret_cast<void**>(&gRealRaiseException)},
    {"LoadLibraryExW", reinterpret_cast<void*>(&HookLoadLibraryExW),
     reinterpret_cast<void**>(&gRealLoadLibraryExW)},
    {"LoadPackagedLibrary", reinterpret_cast<void*>(&HookLoadPackagedLibrary),
     reinterpret_cast<void**>(&gRealLoadPackagedLibrary)},
    {"longjmp", reinterpret_cast<void*>(&HookLongjmp),
     reinterpret_cast<void**>(&gRealLongjmp)},
};

// Writes one import slot, saving what was there. The import table sits in
// read-only memory, and VirtualProtectFromApp is the only way an app container
// may change that.
bool ReplaceSlot(void** slot, void* replacement, void** original) {
  ULONG previous = 0;
  if (!::VirtualProtectFromApp(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
    return false;
  }
  if (!*original) *original = *slot;
  *slot = replacement;
  ULONG ignored = 0;
  ::VirtualProtectFromApp(slot, sizeof(void*), previous, &ignored);
  return true;
}

// --- windows -----------------------------------------------------------------
//
// The window is hidden with nothing raised and none of the shell's own signals
// -- no EnteredBackground, no Consolidated, the splash dismissed in the first
// second. On a phone one top-level window is visible at a time, and the one
// thing that makes a visible window stop being visible without the shell's
// hand in it is another window being shown in front of it. The engine creates
// windows: USER32!CreateWindowExW is among the delayed imports it has actually
// resolved. These say what it creates, shows and raises, and when.
//
// Delay-loaded, so the slots live in the delay-load IAT, and a slot not yet
// resolved holds the address of the resolver thunk rather than of USER32. The
// real function is therefore looked up directly and the slot's old value is
// never called.
using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
                                        int, int, HWND, HMENU, HINSTANCE, LPVOID);
using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using HwndToBoolFn = BOOL(WINAPI*)(HWND);
using HwndToHwndFn = HWND(WINAPI*)(HWND);
using SetWindowLongWFn = LONG(WINAPI*)(HWND, int, LONG);

CreateWindowExWFn gRealCreateWindowExW = nullptr;
ShowWindowFn gRealShowWindow = nullptr;
ShowWindowFn gRealShowWindowAsync = nullptr;
SetWindowPosFn gRealSetWindowPos = nullptr;
HwndToBoolFn gRealSetForegroundWindow = nullptr;
HwndToHwndFn gRealSetActiveWindow = nullptr;
HwndToHwndFn gRealSetFocus = nullptr;
HwndToBoolFn gRealDestroyWindow = nullptr;
HwndToBoolFn gRealBringWindowToTop = nullptr;
SetWindowLongWFn gRealSetWindowLongW = nullptr;

std::wstring HwndLabel(HWND hwnd) { return Hex(reinterpret_cast<uintptr_t>(hwnd)); }

std::wstring ClassLabel(LPCWSTR cls) {
  if (!cls) return L"(null)";
  // An atom rather than a string, when the high word is clear.
  if ((reinterpret_cast<uintptr_t>(cls) >> 16) == 0) {
    return L"atom " + std::to_wstring(reinterpret_cast<uintptr_t>(cls));
  }
  return L"[" + std::wstring(cls) + L"]";
}

void WinNote(const std::wstring& line) {
  Log::WriteFromFault(L"win: " + line + L", thread " +
                      std::to_wstring(::GetCurrentThreadId()));
  Log::FlushFromFault();
}

HWND WINAPI HookCreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR name,
                                DWORD style, int x, int y, int w, int h,
                                HWND parent, HMENU menu, HINSTANCE inst,
                                LPVOID param) {
  HWND made = gRealCreateWindowExW
                  ? gRealCreateWindowExW(exStyle, cls, name, style, x, y, w, h,
                                         parent, menu, inst, param)
                  : nullptr;
  WinNote(L"CreateWindowExW class " + ClassLabel(cls) + L" title " +
          (name ? L"[" + std::wstring(name) + L"]" : L"(null)") + L" style " +
          Hex(style) + L" ex " + Hex(exStyle) + L" parent " + HwndLabel(parent) +
          L" at " + std::to_wstring(x) + L"," + std::to_wstring(y) + L" " +
          std::to_wstring(w) + L"x" + std::to_wstring(h) + L" -> " +
          HwndLabel(made) +
          (style & 0x10000000u ? L"  VISIBLE FROM BIRTH" : L""));
  return made;
}

BOOL WINAPI HookShowWindow(HWND hwnd, int cmd) {
  WinNote(L"ShowWindow " + HwndLabel(hwnd) + L" cmd " + std::to_wstring(cmd));
  return gRealShowWindow ? gRealShowWindow(hwnd, cmd) : FALSE;
}

BOOL WINAPI HookShowWindowAsync(HWND hwnd, int cmd) {
  WinNote(L"ShowWindowAsync " + HwndLabel(hwnd) + L" cmd " + std::to_wstring(cmd));
  return gRealShowWindowAsync ? gRealShowWindowAsync(hwnd, cmd) : FALSE;
}

BOOL WINAPI HookSetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy,
                             UINT flags) {
  WinNote(L"SetWindowPos " + HwndLabel(hwnd) + L" after " + HwndLabel(after) +
          L" at " + std::to_wstring(x) + L"," + std::to_wstring(y) + L" " +
          std::to_wstring(cx) + L"x" + std::to_wstring(cy) + L" flags " +
          Hex(flags));
  return gRealSetWindowPos ? gRealSetWindowPos(hwnd, after, x, y, cx, cy, flags)
                           : FALSE;
}

BOOL WINAPI HookSetForegroundWindow(HWND hwnd) {
  WinNote(L"SetForegroundWindow " + HwndLabel(hwnd));
  return gRealSetForegroundWindow ? gRealSetForegroundWindow(hwnd) : FALSE;
}

HWND WINAPI HookSetActiveWindow(HWND hwnd) {
  WinNote(L"SetActiveWindow " + HwndLabel(hwnd));
  return gRealSetActiveWindow ? gRealSetActiveWindow(hwnd) : nullptr;
}

HWND WINAPI HookSetFocus(HWND hwnd) {
  WinNote(L"SetFocus " + HwndLabel(hwnd));
  return gRealSetFocus ? gRealSetFocus(hwnd) : nullptr;
}

BOOL WINAPI HookDestroyWindow(HWND hwnd) {
  WinNote(L"DestroyWindow " + HwndLabel(hwnd));
  return gRealDestroyWindow ? gRealDestroyWindow(hwnd) : FALSE;
}

BOOL WINAPI HookBringWindowToTop(HWND hwnd) {
  WinNote(L"BringWindowToTop " + HwndLabel(hwnd));
  return gRealBringWindowToTop ? gRealBringWindowToTop(hwnd) : FALSE;
}

LONG WINAPI HookSetWindowLongW(HWND hwnd, int index, LONG value) {
  WinNote(L"SetWindowLongW " + HwndLabel(hwnd) + L" index " +
          std::to_wstring(index) + L" value " + Hex(static_cast<uintptr_t>(value)));
  return gRealSetWindowLongW ? gRealSetWindowLongW(hwnd, index, value) : 0;
}

struct WindowHook {
  const char* name;
  void* replacement;
  void** real;
};

const WindowHook kWindowHooks[] = {
    {"CreateWindowExW", reinterpret_cast<void*>(&HookCreateWindowExW),
     reinterpret_cast<void**>(&gRealCreateWindowExW)},
    {"ShowWindow", reinterpret_cast<void*>(&HookShowWindow),
     reinterpret_cast<void**>(&gRealShowWindow)},
    {"ShowWindowAsync", reinterpret_cast<void*>(&HookShowWindowAsync),
     reinterpret_cast<void**>(&gRealShowWindowAsync)},
    {"SetWindowPos", reinterpret_cast<void*>(&HookSetWindowPos),
     reinterpret_cast<void**>(&gRealSetWindowPos)},
    {"SetForegroundWindow", reinterpret_cast<void*>(&HookSetForegroundWindow),
     reinterpret_cast<void**>(&gRealSetForegroundWindow)},
    {"SetActiveWindow", reinterpret_cast<void*>(&HookSetActiveWindow),
     reinterpret_cast<void**>(&gRealSetActiveWindow)},
    {"SetFocus", reinterpret_cast<void*>(&HookSetFocus),
     reinterpret_cast<void**>(&gRealSetFocus)},
    {"DestroyWindow", reinterpret_cast<void*>(&HookDestroyWindow),
     reinterpret_cast<void**>(&gRealDestroyWindow)},
    {"BringWindowToTop", reinterpret_cast<void*>(&HookBringWindowToTop),
     reinterpret_cast<void**>(&gRealBringWindowToTop)},
    {"SetWindowLongW", reinterpret_cast<void*>(&HookSetWindowLongW),
     reinterpret_cast<void**>(&gRealSetWindowLongW)},
};

// The same eight words as DelayDescriptor below, which is declared after this
// is needed.
struct DelayDescriptorEarly {
  DWORD attributes, nameRva, moduleHandleRva, iatRva, intRva, boundIatRva,
      unloadIatRva, timestamp;
};

// --- WinRT activations --------------------------------------------------------
//
// The engine cannot make a window here, and the shell's own composition swap
// chain hid nothing. What is left that reaches the shell from this process is
// WinRT: the engine loads api-ms-win-core-winrt-l1-1-0 by name at start-up and
// activates classes through it -- UISettings, UIViewSettings, InputPane,
// SystemMediaTransportControls, the notification manager, and whatever
// nsSystemInfo asks for at delayed start-up. Activating the wrong one, from the
// wrong thread, a second after the browser window first paints, is the shape
// of what is left. Every activation is written down: the class, and the thread.
using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using RoActivateInstanceFn = HRESULT(WINAPI*)(HSTRING, IInspectable**);
RoGetActivationFactoryFn gRealRoGetActivationFactory = nullptr;
RoActivateInstanceFn gRealRoActivateInstance = nullptr;

std::wstring ClassName(HSTRING name) {
  UINT32 length = 0;
  const wchar_t* raw = ::WindowsGetStringRawBuffer(name, &length);
  return raw ? std::wstring(raw, length) : L"(null)";
}

HRESULT WINAPI HookRoGetActivationFactory(HSTRING cls, REFIID iid, void** out) {
  HRESULT hr = gRealRoGetActivationFactory
                   ? gRealRoGetActivationFactory(cls, iid, out)
                   : E_NOTIMPL;
  Log::WriteFromFault(L"winrt: factory for " + ClassName(cls) + L" -> " +
                      Hex(static_cast<uintptr_t>(hr)) + L", thread " +
                      std::to_wstring(::GetCurrentThreadId()));
  Log::FlushFromFault();
  return hr;
}

HRESULT WINAPI HookRoActivateInstance(HSTRING cls, IInspectable** out) {
  HRESULT hr = gRealRoActivateInstance ? gRealRoActivateInstance(cls, out)
                                       : E_NOTIMPL;
  Log::WriteFromFault(L"winrt: activated " + ClassName(cls) + L" -> " +
                      Hex(static_cast<uintptr_t>(hr)) + L", thread " +
                      std::to_wstring(::GetCurrentThreadId()));
  Log::FlushFromFault();
  return hr;
}

const WindowHook kWinRtHooks[] = {
    {"RoGetActivationFactory", reinterpret_cast<void*>(&HookRoGetActivationFactory),
     reinterpret_cast<void**>(&gRealRoGetActivationFactory)},
    {"RoActivateInstance", reinterpret_cast<void*>(&HookRoActivateInstance),
     reinterpret_cast<void**>(&gRealRoActivateInstance)},
};

// --- COM ----------------------------------------------------------------------
//
// WinRT turned out to be two harmless activations and nothing else, and both
// Ro* entry points are delay-loaded, so that list is complete. CoCreateInstance
// is the other door out of the process to the system, it is delay-loaded from
// ole32 too, and it has never been watched. The class is written as a GUID;
// naming it is a lookup afterwards.
using CoCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, IUnknown*, DWORD, REFIID, void**);
CoCreateInstanceFn gRealCoCreateInstance = nullptr;

std::wstring GuidText(const GUID& g) {
  wchar_t out[48];
  ::swprintf_s(out, L"{%08lx-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx}",
               static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0],
               g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
               g.Data4[6], g.Data4[7]);
  return out;
}

HRESULT WINAPI HookCoCreateInstance(REFCLSID clsid, IUnknown* outer, DWORD context,
                                    REFIID iid, void** out) {
  HRESULT hr = gRealCoCreateInstance
                   ? gRealCoCreateInstance(clsid, outer, context, iid, out)
                   : REGDB_E_CLASSNOTREG;
  Log::WriteFromFault(L"com: CoCreateInstance " + GuidText(clsid) + L" context " +
                      Hex(context) + L" -> " + Hex(static_cast<uintptr_t>(hr)) +
                      L", thread " + std::to_wstring(::GetCurrentThreadId()));
  Log::FlushFromFault();
  return hr;
}

const WindowHook kComHooks[] = {
    {"CoCreateInstance", reinterpret_cast<void*>(&HookCoCreateInstance),
     reinterpret_cast<void**>(&gRealCoCreateInstance)},
};

int HookDelayImports(HMODULE module, const char* targetModule,
                     const WindowHook* hooks, size_t count, const wchar_t* tag);

int HookWindowImports(HMODULE module) {
  int n = HookDelayImports(module, "USER32.dll", kWindowHooks,
                           sizeof(kWindowHooks) / sizeof(kWindowHooks[0]), L"win");
  n += HookDelayImports(module, "api-ms-win-core-winrt-l1-1-0.dll", kWinRtHooks,
                        sizeof(kWinRtHooks) / sizeof(kWinRtHooks[0]), L"winrt");
  n += HookDelayImports(module, "ole32.dll", kComHooks,
                        sizeof(kComHooks) / sizeof(kComHooks[0]), L"com");
  return n;
}

int HookDelayImports(HMODULE module, const char* targetModule,
                     const WindowHook* hooks, size_t count, const wchar_t* tag) {
  auto* base = reinterpret_cast<unsigned char*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  DWORD rva = nt->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
                  .VirtualAddress;
  if (!rva) return 0;

  const std::wstring targetWide = Widen(targetModule);
  HMODULE target = ::GetModuleHandleW(targetWide.c_str());
  if (!target) {
    target = ::LoadLibraryExW(targetWide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  }
  if (!target) {
    Log::WriteFromFault(std::wstring(tag) + L": " + targetWide +
                        L" is not loadable, nothing watched");
    return 0;
  }

  int hooked = 0;
  auto* desc = reinterpret_cast<DelayDescriptorEarly*>(base + rva);
  for (; desc->nameRva; ++desc) {
    if (!(desc->attributes & 1) || !desc->intRva || !desc->iatRva) continue;
    const char* moduleName = reinterpret_cast<const char*>(base + desc->nameRva);
    if (_stricmp(moduleName, targetModule) != 0) continue;

    auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->intRva);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->iatRva);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
      auto* byName =
          reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      for (size_t i = 0; i < count; ++i) {
        const WindowHook& hook = hooks[i];
        if (std::strcmp(byName->Name, hook.name) != 0) continue;
        FARPROC real = ::GetProcAddress(target, hook.name);
        if (!real) {
          // Quietly: the USER32 list is known to be mostly absent here.
          break;
        }
        *hook.real = reinterpret_cast<void*>(real);
        void* ignored = nullptr;
        void** slot = reinterpret_cast<void**>(&slots->u1.Function);
        if (ReplaceSlot(slot, hook.replacement, &ignored)) ++hooked;
        break;
      }
    }
  }
  Log::WriteFromFault(std::wstring(tag) + L": watching " +
                      std::to_wstring(hooked) + L" calls into " + targetWide);
  return hooked;
}

int HookImports(HMODULE module) {
  auto* base = reinterpret_cast<unsigned char*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);

  DWORD importRva =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
          .VirtualAddress;
  if (!importRva) return 0;

  // A loaded image is laid out, so an RVA is simply an offset from the base --
  // no section walk, unlike reading the same table out of the file.
  auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importRva);

  int hooked = 0;
  for (; desc->Name; ++desc) {
    if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;

    auto* names =
        reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->OriginalFirstThunk);
    auto* addresses =
        reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->FirstThunk);

    for (; names->u1.AddressOfData; ++names, ++addresses) {
      if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;

      auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
          base + names->u1.AddressOfData);

      for (const Hook& hook : kHooks) {
        if (std::strcmp(byName->Name, hook.name) != 0) continue;
        void** slot = reinterpret_cast<void**>(&addresses->u1.Function);
        // A slot already pointing at the replacement belongs to a module that
        // has been through here.
        if (*slot == hook.replacement) break;
        if (ReplaceSlot(slot, hook.replacement, hook.original)) ++hooked;
        break;
      }
    }
  }
  return hooked;
}

// --------------------------------------------------------- delay-load audit
//
// The engine delay-loads some forty desktop libraries, and this device ships
// reduced versions of most of them: the library is there, a given function is
// not. Each such miss ends the thread, because a delay-load thunk reports
// failure by raising and 32-bit ARM has no exception handling to catch it.
//
// Finding them one per build, one crash at a time, is no way to spend a day.
// This walks the delay-load directory and asks after every function in it at
// once, so the whole list arrives in a single run and the stubs can be written
// against it.

// delayimp's own descriptor, spelled out rather than including delayimp.h for
// it: the header's definition drags in the helper machinery too.
struct DelayDescriptor {
  DWORD attributes;
  DWORD nameRva;
  DWORD moduleHandleRva;
  DWORD iatRva;
  DWORD intRva;
  DWORD boundIatRva;
  DWORD unloadIatRva;
  DWORD timestamp;
};

void ProbeDelayLoads(HMODULE module) {
  auto* base = reinterpret_cast<unsigned char*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);

  DWORD rva = nt->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
                  .VirtualAddress;
  if (!rva) {
    Log::WriteFromFault(L"delay-probe: no delay-load directory");
    return;
  }

  auto* desc = reinterpret_cast<DelayDescriptor*>(base + rva);

  int modules = 0;
  int imports = 0;
  int missing = 0;
  for (; desc->nameRva; ++desc) {
    // The first attribute bit says the fields are RVAs. Anything older stores
    // absolute addresses, which no linker has produced in twenty years.
    if (!(desc->attributes & 1)) continue;

    const char* moduleName =
        reinterpret_cast<const char*>(base + desc->nameRva);
    ++modules;

    HMODULE target = ::LoadLibraryExW(Widen(moduleName).c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                          LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!target) {
      ++missing;
      Log::WriteFromFault(L"delay-probe: " + Widen(moduleName) + L" absent entirely");
      continue;
    }

    int moduleImports = 0;
    int moduleMissing = 0;

    if (!desc->intRva) continue;
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->intRva);
    for (; thunk->u1.AddressOfData; ++thunk) {
      ++imports;
      ++moduleImports;
      FARPROC proc = nullptr;
      std::wstring label;
      if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) {
        WORD ordinal = static_cast<WORD>(IMAGE_ORDINAL32(thunk->u1.Ordinal));
        proc = ::GetProcAddress(
            target, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(ordinal)));
        label = L"#" + std::to_wstring(ordinal);
      } else {
        auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
            base + thunk->u1.AddressOfData);
        proc = ::GetProcAddress(target, byName->Name);
        label = Widen(byName->Name);
      }
      if (proc) continue;
      ++missing;
      ++moduleMissing;
    }

    // One line per library rather than one per function: 221 names bury the
    // shape of it, and the shape is what matters -- whole subsystems this
    // device does not have.
    if (moduleMissing) {
      Log::WriteFromFault(L"delay-probe: " + Widen(moduleName) + L" missing " +
                 std::to_wstring(moduleMissing) + L" of " +
                 std::to_wstring(moduleImports));
    }
    ::FreeLibrary(target);
  }

  Log::WriteFromFault(L"delay-probe: " + std::to_wstring(modules) + L" modules, " +
             std::to_wstring(imports) + L" delayed imports, " +
             std::to_wstring(missing) + L" unavailable");
}

// ------------------------------------------------------------- pc sampling

// SuspendThread and GetThreadContext are outside the app partition of the
// headers, and unlike AddVectoredExceptionHandler they may genuinely be
// refused, so they are resolved at run time and their absence is reported
// rather than failing the link.
using SuspendThreadFn = DWORD(WINAPI*)(HANDLE);
using ResumeThreadFn = DWORD(WINAPI*)(HANDLE);
using GetThreadContextFn = BOOL(WINAPI*)(HANDLE, PCONTEXT);

SuspendThreadFn gSuspendThread = nullptr;
ResumeThreadFn gResumeThread = nullptr;
GetThreadContextFn gGetThreadContext = nullptr;

bool ResolveSamplingApis() {
  HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
  if (!k32) return false;
  gSuspendThread =
      reinterpret_cast<SuspendThreadFn>(::GetProcAddress(k32, "SuspendThread"));
  gResumeThread =
      reinterpret_cast<ResumeThreadFn>(::GetProcAddress(k32, "ResumeThread"));
  gGetThreadContext = reinterpret_cast<GetThreadContextFn>(
      ::GetProcAddress(k32, "GetThreadContext"));
  return gSuspendThread && gResumeThread && gGetThreadContext;
}

// The renderer thread went quiet between use_program(ps_clear) and the next
// GL call, and nothing -- no fault, no abort, no exit -- was ever reported
// for it. Suspend it, read its registers and walk its stack, so the log says
// where it is: parked in a kernel wait, deep in the driver, or gone.
using OpenThreadFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
void AutopsyRendererThread(const wchar_t* tag) {
  if (gAutopsyDone.exchange(true)) {
    return;
  }
  const std::wstring t(tag);
  if (!gWrThreadFn) {
    Log::WriteFromFault(t + L" no renderer thread id export");
    return;
  }
  const DWORD tid = gWrThreadFn();
  if (!tid) {
    Log::WriteFromFault(t + L" the renderer thread has not reported an id yet");
    return;
  }
  HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
  auto openThread = k32 ? reinterpret_cast<OpenThreadFn>(::GetProcAddress(k32, "OpenThread")) : nullptr;
  if (!openThread || !ResolveSamplingApis()) {
    Log::WriteFromFault(t + L" thread inspection unavailable in this container");
    return;
  }
  // THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | SYNCHRONIZE
  HANDLE thread = openThread(0x0040 | 0x0008 | 0x0002 | 0x00100000, FALSE, tid);
  if (!thread) {
    Log::WriteFromFault(t + L" OpenThread(" + std::to_wstring(tid) + L") failed, error " +
                        std::to_wstring(::GetLastError()));
    return;
  }
  DWORD exitCode = 0;
  const bool haveExit = ::GetExitCodeThread(thread, &exitCode) != 0;
  const DWORD wait = ::WaitForSingleObject(thread, 0);
  Log::WriteFromFault(t + L" renderer thread " + std::to_wstring(tid) +
                      (wait == WAIT_OBJECT_0 ? L" has EXITED" : L" is still alive") +
                      (haveExit ? L", exit code " + Hex(exitCode) : L""));
  if (wait == WAIT_OBJECT_0) {
    ::CloseHandle(thread);
    return;
  }
  const DWORD previous = gSuspendThread(thread);
  if (previous == static_cast<DWORD>(-1)) {
    Log::WriteFromFault(t + L" SuspendThread failed, error " + std::to_wstring(::GetLastError()));
    ::CloseHandle(thread);
    return;
  }
  Log::WriteFromFault(t + L" suspended (previous suspend count " + std::to_wstring(previous) +
                      (previous > 0 ? L" -- SOMEONE ELSE HAD ALREADY SUSPENDED IT)" : L")"));
  __declspec(align(8)) CONTEXT context{};
  context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  if (gGetThreadContext(thread, &context)) {
    Log::WriteFromFault(t + L" registers" + Registers(context));
    LogInstruction(tag, reinterpret_cast<void*>(ContextPc(context)));
    LogStack(tag, context);
    LogStackScan(tag, ContextSp(context));
  } else {
    Log::WriteFromFault(t + L" GetThreadContext failed, error " + std::to_wstring(::GetLastError()));
  }
  gResumeThread(thread);
  ::CloseHandle(thread);
}

// The trace file. A raw address is worthless in the next process -- ASLR moves
// every module -- so each sample is stored as a module index and an offset,
// with the names written alongside them.
constexpr unsigned kTraceMagic = 0x32525452;  // "RTR2" (pointer-sized x64 data)
constexpr unsigned kMaxModules = 16;
constexpr unsigned kCapacity = 8192;

struct TraceModule {
  char name[40];
  uintptr_t base;
};

struct TraceSample {
  unsigned module;  // index into modules, or kMaxModules when unknown
  uintptr_t offset;
};

struct TraceFile {
  unsigned magic;
  unsigned moduleCount;
  unsigned written;  // total samples ever written; the ring holds the last few
  unsigned capacity;
  TraceModule modules[kMaxModules];
  TraceSample samples[kCapacity];
};

TraceFile* gTrace = nullptr;
HANDLE gTraceMapping = nullptr;
HANDLE gTraceFile = INVALID_HANDLE_VALUE;

std::wstring TracePath(const std::wstring& localState) {
  return localState + L"\\pc-trace.bin";
}

TraceFile* MapTrace(const std::wstring& localState, bool createNew) {
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;

  gTraceFile = ::CreateFile2(TracePath(localState).c_str(),
                             GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                             createNew ? OPEN_ALWAYS : OPEN_EXISTING, &params);
  if (gTraceFile == INVALID_HANDLE_VALUE) return nullptr;

  gTraceMapping = ::CreateFileMappingFromApp(gTraceFile, nullptr, PAGE_READWRITE,
                                             sizeof(TraceFile), nullptr);
  if (!gTraceMapping) {
    ::CloseHandle(gTraceFile);
    gTraceFile = INVALID_HANDLE_VALUE;
    return nullptr;
  }

  auto* view = static_cast<TraceFile*>(::MapViewOfFileFromApp(
      gTraceMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, sizeof(TraceFile)));
  if (!view) {
    ::CloseHandle(gTraceMapping);
    ::CloseHandle(gTraceFile);
    gTraceMapping = nullptr;
    gTraceFile = INVALID_HANDLE_VALUE;
  }
  return view;
}

// Reports the previous run's trace before this run overwrites it. Only the
// tail matters: the ring is long enough that the interesting part is always at
// the end.
void ReportPreviousTrace(const std::wstring& localState) {
  // Silence here has been read twice as "the previous run left nothing", and
  // there are three quite different reasons for it. It says which now, because
  // this is about to be the only account of how the process died.
  TraceFile* trace = MapTrace(localState, /*createNew*/ false);
  if (!trace) {
    Log::WriteNum(L"trace: no trace file from the previous run, err",
                  ::GetLastError());
    return;
  }

  if (trace->magic != kTraceMagic) {
    Log::Write(L"trace: the previous run's trace file is not one");
    ::UnmapViewOfFile(trace);
    return;
  }
  if (trace->written == 0) {
    Log::Write(L"trace: the previous run wrote no samples");
    ::UnmapViewOfFile(trace);
    return;
  }

  constexpr unsigned kReport = 40;
  unsigned total = trace->written;
  unsigned have = total < kCapacity ? total : kCapacity;
  unsigned show = have < kReport ? have : kReport;

  Log::WriteNum(L"trace: samples from the previous run", total);
  for (unsigned i = have - show; i < have; ++i) {
    unsigned slot = (total - have + i) % kCapacity;
    const TraceSample& sample = trace->samples[slot];

    std::wstring where;
    if (sample.module < trace->moduleCount) {
      const char* name = trace->modules[sample.module].name;
      int n = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
      std::wstring wide(static_cast<size_t>(n > 1 ? n - 1 : 0), L'\0');
      if (n > 1) {
        ::MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), n);
      }
      where = wide + L"+" + Hex(sample.offset);
    } else {
      where = Hex(sample.offset) + L" (no module)";
    }
    Log::WriteFromFault(L"trace: " + where);
  }

  ::UnmapViewOfFile(trace);
  // Reopened for writing by the sampler.
  if (gTraceMapping) ::CloseHandle(gTraceMapping);
  if (gTraceFile != INVALID_HANDLE_VALUE) ::CloseHandle(gTraceFile);
  gTraceMapping = nullptr;
  gTraceFile = INVALID_HANDLE_VALUE;
}

// Resolving a module per sample would cost more than the sample; the handles
// repeat, so a short table answers nearly every lookup.
unsigned ModuleIndexFor(const void* addr) {
  HMODULE mod = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &mod) ||
      !mod) {
    return kMaxModules;
  }

  uintptr_t base = reinterpret_cast<uintptr_t>(mod);
  for (unsigned i = 0; i < gTrace->moduleCount; ++i) {
    if (gTrace->modules[i].base == base) return i;
  }
  if (gTrace->moduleCount >= kMaxModules) return kMaxModules;

  wchar_t path[MAX_PATH] = {};
  DWORD n = ::GetModuleFileNameW(mod, path, MAX_PATH);
  std::wstring name(path, n);
  auto slash = name.find_last_of(L'\\');
  if (slash != std::wstring::npos) name = name.substr(slash + 1);

  unsigned index = gTrace->moduleCount;
  TraceModule& entry = gTrace->modules[index];
  entry.base = base;
  ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, entry.name,
                        sizeof(entry.name) - 1, nullptr, nullptr);
  gTrace->moduleCount = index + 1;
  return index;
}

DWORD WINAPI SamplerThread(LPVOID param) {
  HANDLE target = static_cast<HANDLE>(param);

  if (!gTrace || !ResolveSamplingApis()) {
    Log::WriteFromFault(L"sampler: thread inspection unavailable in this container");
    ::CloseHandle(target);
    return 0;
  }

  uintptr_t last = 0;
  DWORD nextReport = ::GetTickCount();
  while (::WaitForSingleObject(target, 0) != WAIT_OBJECT_0) {
    // Often enough to show a climb, rarely enough that the log stays readable.
    if (::GetTickCount() >= nextReport) {
      nextReport = ::GetTickCount() + 500;
      LogMemory(L"gecko:");
    }
    if (gSuspendThread(target) == static_cast<DWORD>(-1)) break;

    // CONTEXT wants 8-byte alignment on ARM and GetThreadContext refuses it
    // otherwise.
    __declspec(align(8)) CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    BOOL ok = gGetThreadContext(target, &context);
    gResumeThread(target);
    if (!ok) break;

    uintptr_t pc = static_cast<uintptr_t>(ContextPc(context));
    // Consecutive samples at one address say nothing new; only movement is
    // worth a slot in the ring.
    if (pc != last) {
      last = pc;
      unsigned index = ModuleIndexFor(reinterpret_cast<void*>(pc));
      TraceSample& sample = gTrace->samples[gTrace->written % kCapacity];
      sample.module = index;
      sample.offset = index < kMaxModules ? pc - gTrace->modules[index].base : pc;
      ++gTrace->written;
    }
  }

  Log::WriteNum(L"sampler: the engine start-up thread ended, samples",
                gTrace->written);
  ::CloseHandle(target);

  // And now the one that matters. The process is being ended without raising
  // anything at all -- no unhandled exception, no exit hook, no error reaching
  // the framework -- which is what a __fastfail looks like from inside, and
  // there is no way to catch one. The only way left to find out where it
  // happens is to have been watching the thread it happens on, and the fault
  // we do see says which thread that is: the UI thread.
  //
  // Slower than the first phase on purpose. Phase one watched a thread doing
  // nothing but starting the engine; this one suspends the thread that draws
  // the screen, and doing that in a tight loop would measure the measurement.
  HANDLE ui = gUiThreadHandle.load();
  if (!ui) {
    Log::WriteFromFault(L"sampler: no handle to the UI thread, not following it");
    return 0;
  }
  Log::WriteFromFault(L"sampler: now following the UI thread");
  uintptr_t lastPc = 0;
  while (::WaitForSingleObject(ui, 0) != WAIT_OBJECT_0) {
    ::Sleep(1);
    if (gSuspendThread(ui) == static_cast<DWORD>(-1)) break;
    __declspec(align(8)) CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    BOOL ok = gGetThreadContext(ui, &context);
    gResumeThread(ui);
    if (!ok) break;
    uintptr_t pc = static_cast<uintptr_t>(ContextPc(context));
    if (pc == lastPc) continue;
    lastPc = pc;
    unsigned index = ModuleIndexFor(reinterpret_cast<void*>(pc));
    TraceSample& sample = gTrace->samples[gTrace->written % kCapacity];
    sample.module = index;
    sample.offset = index < kMaxModules ? pc - gTrace->modules[index].base : pc;
    ++gTrace->written;
  }
  return 0;
}

// Reached only when nothing in the process handled the exception, which makes
// this the one report that names a cause rather than an event.
LONG WINAPI OnUnhandledException(PEXCEPTION_POINTERS info) {
  Log::WriteFromFault(L"FATAL: unhandled " + Hex(info->ExceptionRecord->ExceptionCode) +
             L" at " + DescribeAddress(info->ExceptionRecord->ExceptionAddress) +
             FaultDetail(*info->ExceptionRecord));
  LogMemory(L"FATAL:");
  Log::WriteFromFault(L"FATAL: code" + Registers(*info->ContextRecord));
  Log::WriteFromFault(L"FATAL: at pc " +
             CodeAt(info->ExceptionRecord->ExceptionAddress));
  LogStack(L"FATAL:", *info->ContextRecord);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void NoteUiFrame() {
  gUiFrames.fetch_add(1, std::memory_order_relaxed);
  gLastFrameAt.store(::GetTickCount64(), std::memory_order_relaxed);
  if (gUiThread.load() != 0) return;
  // The first frame is also the moment the UI thread identifies itself, and
  // the only moment a handle to it can be had: an app container will not open
  // a thread by id, but a thread may always duplicate its own pseudo-handle
  // into a real one. The sampler needs that handle to follow it.
  HANDLE real = nullptr;
  ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                    ::GetCurrentProcess(), &real, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
  gUiThreadHandle.store(real);
  gUiThread.store(::GetCurrentThreadId());
}

std::atomic<int> gProbeWidth{1440};
std::atomic<int> gProbeHeight{2560};
ID3D11DeviceContext* gProbeContext = nullptr;

// The one thing the GPU path does that nothing else in this process does, and
// that every experiment so far has left in place: it creates a composition
// swap chain. Hand it to the panel or not, present to it or not, the window
// is hidden four tenths of a second after the chain exists -- and the module
// that hides it is the shell's navigation client, driven from the shell's
// side over a message proxy. A composition swap chain is a new connection
// from this process to the system compositor, and a phone shell that counts
// connections as views would see one appear, switch to it, find nothing
// there, and leave the real one behind.
//
// So the shell's own device makes one, three seconds in, long before the
// engine touches the GPU, with the very description ANGLE uses. If the window
// is hidden four tenths of a second after this, the engine is innocent and
// the answer is a different way of getting a frame into XAML.
DWORD WINAPI CompositionChainThread(LPVOID) {
  ::Sleep(3000);
  HMODULE dxgi = ::LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  auto createFactory = dxgi ? reinterpret_cast<CreateFactoryFn>(
                                  ::GetProcAddress(dxgi, "CreateDXGIFactory1"))
                            : nullptr;
  if (!createFactory) {
    Log::WriteFromFault(L"chain: no CreateDXGIFactory1");
    return 0;
  }
  IDXGIFactory2* factory = nullptr;
  HRESULT hr = createFactory(__uuidof(IDXGIFactory2),
                             reinterpret_cast<void**>(&factory));
  if (FAILED(hr) || !factory) {
    Log::WriteFromFault(L"chain: no IDXGIFactory2, " +
                        Hex(static_cast<uintptr_t>(hr)));
    return 0;
  }

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = static_cast<UINT>(gProbeWidth.load());
  desc.Height = static_cast<UINT>(gProbeHeight.load()) - 252;  // the room, as ANGLE gets it
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_BACK_BUFFER |
                     DXGI_USAGE_SHADER_INPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

  Log::WriteFromFault(L"chain: making a composition swap chain from the shell, " +
                      std::to_wstring(desc.Width) + L"x" +
                      std::to_wstring(desc.Height));
  Log::FlushFromFault();
  IDXGISwapChain1* chain = nullptr;
  hr = factory->CreateSwapChainForComposition(gProbeDevice, &desc, nullptr, &chain);
  if (FAILED(hr) || !chain) {
    Log::WriteFromFault(L"chain: refused, " + Hex(static_cast<uintptr_t>(hr)));
    return 0;
  }
  Log::WriteFromFault(L"chain: it exists -- if the window is hidden in the "
                      L"next half second, this is what hides it");
  Log::FlushFromFault();

  ::Sleep(1500);
  DXGI_PRESENT_PARAMETERS params{};
  hr = chain->Present1(1, 0, &params);
  Log::WriteFromFault(L"chain: presented once, " + Hex(static_cast<uintptr_t>(hr)));
  Log::FlushFromFault();
  // Kept alive for the whole run, like the engine's.
  return 0;
}

// --- ANGLE, step by step -------------------------------------------------------
//
// With software WebRender the browser lives: 85 seconds, typing, pages, the
// keyboard up and down, and a normal exit. Everything else in that
// configuration is what the dying builds had. The one thing missing is ANGLE
// coming up -- the EGL display and renderer the engine makes when its window's
// compositor starts, panel or no panel.
//
// So the shell brings ANGLE up itself, here, on the shell's own device, in the
// steps the engine takes, three seconds apart, saying each one. Whichever
// step the window is hidden after is the call to fix; if all of them pass,
// it is something the engine does on top of ANGLE and not ANGLE itself.
using EGLDisplayT = void*;
using EGLConfigT = void*;
using EGLContextT = void*;
using EGLSurfaceT = void*;
using EGLDeviceEXTT = void*;
using EGLBooleanT = unsigned;
using EGLintT = int;
using eglGetProcAddressFn = void* (*)(const char*);
using eglCreateDeviceANGLEFn = EGLDeviceEXTT (*)(EGLintT, void*, const intptr_t*);
using eglGetPlatformDisplayEXTFn = EGLDisplayT (*)(EGLintT, void*, const EGLintT*);
using eglInitializeFn = EGLBooleanT (*)(EGLDisplayT, EGLintT*, EGLintT*);
using eglChooseConfigFn = EGLBooleanT (*)(EGLDisplayT, const EGLintT*, EGLConfigT*, EGLintT, EGLintT*);
using eglCreateContextFn = EGLContextT (*)(EGLDisplayT, EGLConfigT, EGLContextT, const EGLintT*);
using eglCreatePbufferSurfaceFn = EGLSurfaceT (*)(EGLDisplayT, EGLConfigT, const EGLintT*);
using eglMakeCurrentFn = EGLBooleanT (*)(EGLDisplayT, EGLSurfaceT, EGLSurfaceT, EGLContextT);
using eglGetErrorFn = EGLintT (*)();
using glClearColorFn = void (*)(float, float, float, float);
using glClearFn = void (*)(unsigned);
using glFinishFn = void (*)();
using glCreateShaderFn = unsigned (*)(unsigned);
using glShaderSourceFn = void (*)(unsigned, int, const char* const*, const int*);
using glCompileShaderFn = void (*)(unsigned);
using glCreateProgramFn = unsigned (*)();
using glAttachShaderFn = void (*)(unsigned, unsigned);
using glLinkProgramFn = void (*)(unsigned);
using glUseProgramFn = void (*)(unsigned);
using glGetProgramivFn = void (*)(unsigned, unsigned, int*);
using glVertexAttribPointerFn = void (*)(unsigned, int, unsigned, unsigned char, int, const void*);
using glEnableVertexAttribArrayFn = void (*)(unsigned);
using glDrawArraysFn = void (*)(unsigned, int, int);
using glGetErrorFn = unsigned (*)();

DWORD WINAPI AngleStepsThread(LPVOID) {
  ::Sleep(3000);
  HMODULE egl = ::LoadPackagedLibrary(L"libEGL.dll", 0);
  HMODULE gles = ::LoadPackagedLibrary(L"libGLESv2.dll", 0);
  if (!egl || !gles) {
    Log::WriteFromFault(L"anglestep: libEGL or libGLESv2 would not load");
    return 0;
  }
  auto getProc = reinterpret_cast<eglGetProcAddressFn>(::GetProcAddress(egl, "eglGetProcAddress"));
  auto initialize = reinterpret_cast<eglInitializeFn>(::GetProcAddress(egl, "eglInitialize"));
  auto chooseConfig = reinterpret_cast<eglChooseConfigFn>(::GetProcAddress(egl, "eglChooseConfig"));
  auto createContext = reinterpret_cast<eglCreateContextFn>(::GetProcAddress(egl, "eglCreateContext"));
  auto createPbuffer = reinterpret_cast<eglCreatePbufferSurfaceFn>(::GetProcAddress(egl, "eglCreatePbufferSurface"));
  auto makeCurrent = reinterpret_cast<eglMakeCurrentFn>(::GetProcAddress(egl, "eglMakeCurrent"));
  auto getError = reinterpret_cast<eglGetErrorFn>(::GetProcAddress(egl, "eglGetError"));
  auto clearColor = reinterpret_cast<glClearColorFn>(::GetProcAddress(gles, "glClearColor"));
  auto clear = reinterpret_cast<glClearFn>(::GetProcAddress(gles, "glClear"));
  auto finish = reinterpret_cast<glFinishFn>(::GetProcAddress(gles, "glFinish"));
  if (!getProc || !initialize || !chooseConfig || !createContext || !createPbuffer ||
      !makeCurrent || !getError || !clearColor || !clear || !finish) {
    Log::WriteFromFault(L"anglestep: an EGL or GL entry point is missing");
    return 0;
  }
  auto createDevice = reinterpret_cast<eglCreateDeviceANGLEFn>(getProc("eglCreateDeviceANGLE"));
  auto getPlatformDisplay = reinterpret_cast<eglGetPlatformDisplayEXTFn>(getProc("eglGetPlatformDisplayEXT"));
  if (!createDevice || !getPlatformDisplay) {
    Log::WriteFromFault(L"anglestep: eglCreateDeviceANGLE or eglGetPlatformDisplayEXT is missing");
    return 0;
  }
  auto say = [&](const wchar_t* what) {
    Log::WriteFromFault(std::wstring(L"anglestep: ") + what + L" -- egl error " +
                        Hex(static_cast<uintptr_t>(getError())) +
                        L"; if the window is hidden in the next three seconds, this step did it");
    Log::FlushFromFault();
    ::Sleep(3000);
  };

  // 1. the display, the way the engine makes it: from the D3D11 device.
  const EGLintT kD3D11Device = 0x33A1;          // EGL_D3D11_DEVICE_ANGLE
  const EGLintT kPlatformDevice = 0x313F;        // EGL_PLATFORM_DEVICE_EXT
  EGLDeviceEXTT device = createDevice(kD3D11Device, gProbeDevice, nullptr);
  const EGLintT none[] = {0x3038};
  EGLDisplayT display = device ? getPlatformDisplay(kPlatformDevice, device, none) : nullptr;
  EGLintT major = 0, minor = 0;
  const bool inited = display && initialize(display, &major, &minor);
  say(inited ? L"1: display made and initialised" : L"1: display FAILED");
  if (!inited) return 0;

  // 2. a config and a context.
  const EGLintT configAttribs[] = {0x3033, 0x0001,   // SURFACE_TYPE: PBUFFER
                                   0x3040, 0x0004,   // RENDERABLE_TYPE: ES2
                                   0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8,  // RGBA 8
                                   0x3038};
  EGLConfigT config = nullptr; EGLintT count = 0;
  chooseConfig(display, configAttribs, &config, 1, &count);
  const EGLintT ctxAttribs[] = {0x3098, 2, 0x3038};  // CONTEXT_CLIENT_VERSION 2
  EGLContextT context = count ? createContext(display, config, nullptr, ctxAttribs) : nullptr;
  say(context ? L"2: context made" : L"2: context FAILED");
  if (!context) return 0;

  // 3. an offscreen surface.
  const EGLintT pbAttribs[] = {0x3057, 64, 0x3056, 64, 0x3038};  // WIDTH, HEIGHT
  EGLSurfaceT surface = createPbuffer(display, config, pbAttribs);
  say(surface ? L"3: pbuffer made" : L"3: pbuffer FAILED");
  if (!surface) return 0;

  // 4. current.
  const bool current = makeCurrent(display, surface, surface, context);
  say(current ? L"4: made current" : L"4: MakeCurrent FAILED");
  if (!current) return 0;

  // 5. a drawn frame.
  clearColor(0.2f, 0.4f, 0.6f, 1.0f);
  clear(0x00004000);  // GL_COLOR_BUFFER_BIT
  finish();
  say(L"5: cleared and finished");

  // 6. the context the engine actually asks for: ES 3, robust access with
  // lose-context-on-reset, no-error, not backwards compatible -- the attribs
  // GLContextProviderEGL builds. Then made current on the same pbuffer.
  const EGLintT geckoCtxAttribs[] = {
      0x3098, 3,            // CONTEXT_MAJOR_VERSION 3
      0x3483, 0,            // CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE false
      0x31B3, 1,            // CONTEXT_OPENGL_NO_ERROR_KHR true
      0x3138, 0x31BF,       // RESET_NOTIFICATION_STRATEGY_EXT = LOSE_CONTEXT_ON_RESET_EXT
      0x30BF, 1,            // CONTEXT_OPENGL_ROBUST_ACCESS_EXT true
      0x3038};
  EGLContextT geckoContext = createContext(display, config, nullptr, geckoCtxAttribs);
  bool geckoCurrent = geckoContext && makeCurrent(display, surface, surface, geckoContext);
  say(geckoCurrent ? L"6: the engine's kind of context made and current"
                   : L"6: the engine's kind of context FAILED");
  if (!geckoCurrent) return 0;

  // 7. a second context sharing the first, as WebRender's render thread and
  // the compositor's share.
  EGLContextT shared = createContext(display, config, geckoContext, geckoCtxAttribs);
  say(shared ? L"7: a shared context made" : L"7: shared context FAILED");

  // 8. a shader program through ANGLE's translator and d3dcompiler, and a
  // triangle drawn with it -- what WebRender does on its first frame.
  auto createShader = reinterpret_cast<glCreateShaderFn>(::GetProcAddress(gles, "glCreateShader"));
  auto shaderSource = reinterpret_cast<glShaderSourceFn>(::GetProcAddress(gles, "glShaderSource"));
  auto compileShader = reinterpret_cast<glCompileShaderFn>(::GetProcAddress(gles, "glCompileShader"));
  auto createProgram = reinterpret_cast<glCreateProgramFn>(::GetProcAddress(gles, "glCreateProgram"));
  auto attachShader = reinterpret_cast<glAttachShaderFn>(::GetProcAddress(gles, "glAttachShader"));
  auto linkProgram = reinterpret_cast<glLinkProgramFn>(::GetProcAddress(gles, "glLinkProgram"));
  auto useProgram = reinterpret_cast<glUseProgramFn>(::GetProcAddress(gles, "glUseProgram"));
  auto getProgramiv = reinterpret_cast<glGetProgramivFn>(::GetProcAddress(gles, "glGetProgramiv"));
  auto attribPointer = reinterpret_cast<glVertexAttribPointerFn>(::GetProcAddress(gles, "glVertexAttribPointer"));
  auto enableAttrib = reinterpret_cast<glEnableVertexAttribArrayFn>(::GetProcAddress(gles, "glEnableVertexAttribArray"));
  auto drawArrays = reinterpret_cast<glDrawArraysFn>(::GetProcAddress(gles, "glDrawArrays"));
  auto glError = reinterpret_cast<glGetErrorFn>(::GetProcAddress(gles, "glGetError"));
  if (createShader && shaderSource && compileShader && createProgram && attachShader &&
      linkProgram && useProgram && getProgramiv && attribPointer && enableAttrib &&
      drawArrays && glError) {
    const char* vs = "#version 300 es\nin vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }";
    const char* fs = "#version 300 es\nprecision mediump float; out vec4 c; void main(){ c = vec4(1.0, 0.5, 0.0, 1.0); }";
    unsigned v = createShader(0x8B31); shaderSource(v, 1, &vs, nullptr); compileShader(v);
    unsigned f = createShader(0x8B30); shaderSource(f, 1, &fs, nullptr); compileShader(f);
    unsigned prog = createProgram(); attachShader(prog, v); attachShader(prog, f); linkProgram(prog);
    int linked = 0; getProgramiv(prog, 0x8B82, &linked);  // GL_LINK_STATUS
    useProgram(prog);
    static const float tri[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
    attribPointer(0, 2, 0x1406, 0, 0, tri);  // GL_FLOAT
    enableAttrib(0);
    drawArrays(4, 0, 3);  // GL_TRIANGLES
    finish();
    Log::WriteFromFault(L"anglestep: 8: shaders compiled (linked " + std::to_wstring(linked) +
                        L"), a triangle drawn, gl error " + Hex(glError()));
    say(L"8: drawn through a compiled program");
  } else {
    Log::WriteFromFault(L"anglestep: 8: GL entry points missing, skipped");
  }

  Log::WriteFromFault(L"anglestep: all eight steps passed with the window still up");
  Log::FlushFromFault();
  return 0;
}

void MakeSecondD3DDevice() {
  HMODULE d3d11 = ::LoadLibraryExW(L"d3d11.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!d3d11) {
    Log::WriteNum(L"second device: d3d11.dll would not load, err",
                  ::GetLastError());
    return;
  }
  using CreateFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
                                    UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                    ID3D11Device**, D3D_FEATURE_LEVEL*,
                                    ID3D11DeviceContext**);
  auto create =
      reinterpret_cast<CreateFn>(::GetProcAddress(d3d11, "D3D11CreateDevice"));
  if (!create) {
    Log::Write(L"second device: d3d11.dll has no D3D11CreateDevice");
    return;
  }

  const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
  D3D_FEATURE_LEVEL got = static_cast<D3D_FEATURE_LEVEL>(0);
  HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                      D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                      ARRAYSIZE(levels), D3D11_SDK_VERSION, &gProbeDevice, &got,
                      &gProbeContext);
  if (FAILED(hr)) {
    Log::Write(L"second device: refused, " + Hex(static_cast<uintptr_t>(hr)));
    return;
  }
  Log::Write(L"second device: made, feature level " +
             Hex(static_cast<uintptr_t>(got)));

  // What the adapter says it has, which is the only figure available before
  // asking for memory and finding out.
  IDXGIDevice* dxgi = nullptr;
  if (SUCCEEDED(gProbeDevice->QueryInterface(__uuidof(IDXGIDevice),
                                             reinterpret_cast<void**>(&dxgi)))) {
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter) {
      DXGI_ADAPTER_DESC ad{};
      if (SUCCEEDED(adapter->GetDesc(&ad))) {
        wchar_t line[256];
        ::swprintf_s(line,
                     L"second device: adapter %s, dedicated %.0f MB, shared "
                     L"%.0f MB",
                     ad.Description,
                     double(ad.DedicatedVideoMemory) / (1024.0 * 1024.0),
                     double(ad.SharedSystemMemory) / (1024.0 * 1024.0));
        Log::Write(line);
      }
      adapter->Release();
    }
    dxgi->Release();
  }

  // No chain this time; the device is kept for one question only, asked
  // every quarter second by the heartbeat: has the GPU been reset. A device
  // that has been reset answers GetDeviceRemovedReason with something other
  // than S_OK, and every device in the process is reset together -- so the
  // shell's own, which does nothing, is a clean witness to what the engine's
  // does. WebRender drawing with real shaders on this Adreno is the one thing
  // all three modes did and the shell's probes never did; a hang and reset
  // would take the system compositor's device with it, and a shell rebuilding
  // its views after that is the shape of a navigation client asserting on a
  // hide.
  Log::Write(L"gpu: watching the shell's device for a reset");
  // The eight-step ANGLE replica stays as code but no longer runs: it proved
  // the shell's own use of ANGLE harmless (0.2.3.6), and the real fault was
  // found elsewhere. Keeping it running only costs a second GPU device and
  // twenty seconds of noise in the log.
  (void)&AngleStepsThread;
}

void SetProbeSurfaceSize(int width, int height) {
  if (width > 0 && height > 0) {
    gProbeWidth.store(width);
    gProbeHeight.store(height);
  }
}

void StartHeartbeat() {
  HANDLE beat = ::CreateThread(nullptr, 0, &HeartbeatThread, nullptr, 0, nullptr);
  if (!beat) {
    Log::WriteNum(L"alive: no heartbeat thread, err", ::GetLastError());
    return;
  }
  ::CloseHandle(beat);
}

void StartLastLocationSampler(void* thread) {
  HANDLE sampler =
      ::CreateThread(nullptr, 0, &SamplerThread, thread, 0, nullptr);
  if (!sampler) {
    Log::WriteNum(L"sampler: CreateThread failed, err", ::GetLastError());
    ::CloseHandle(static_cast<HANDLE>(thread));
    return;
  }
  ::CloseHandle(sampler);
}

// The trace and the crash reports are full of offsets into ntdll and
// KERNELBASE, and there is no map for either. Asking the loader where a
// handful of interesting functions live turns those offsets into names -- and
// in particular says whether an address is a wait, which is a thread doing
// nothing wrong, or a process-ending call, which is not.
void LogKnownOffsets() {
  struct Wanted {
    const wchar_t* module;
    const char* names[10];
  };
  const Wanted wanted[] = {
      {L"ntdll.dll",
       {"RtlExitUserProcess", "NtTerminateProcess", "RtlRaiseStatus",
        "RtlRaiseException", "NtWaitForSingleObject",
        "RtlWaitOnAddress", "RtlUserThreadStart", nullptr}},
      {L"KERNELBASE.dll",
       {"TerminateProcess", "ExitProcess", "RaiseFailFastException",
        "RaiseException", "WaitForSingleObjectEx", "WaitForMultipleObjectsEx",
        "SleepEx", nullptr}},
  };

  for (const Wanted& entry : wanted) {
    HMODULE module = ::GetModuleHandleW(entry.module);
    if (!module) continue;
    for (const char* const* name = entry.names; *name; ++name) {
      FARPROC proc = ::GetProcAddress(module, *name);
      if (!proc) continue;
      uintptr_t offset = reinterpret_cast<uintptr_t>(proc) -
                         reinterpret_cast<uintptr_t>(module);
      std::wstring wide(*name, *name + std::strlen(*name));
      Log::WriteFromFault(std::wstring(L"known: ") + entry.module + L"+" + Hex(offset) +
                 L" " + wide);
    }
  }
}

void InstallProcessProbes(const std::wstring& localStatePath) {
  ResolveUnwindApis();
  ReportPreviousTrace(localStatePath);

  gTrace = MapTrace(localStatePath, /*createNew*/ true);
  if (gTrace) {
    gTrace->magic = kTraceMagic;
    gTrace->capacity = kCapacity;
    gTrace->written = 0;
    gTrace->moduleCount = 0;
  } else {
    Log::WriteNum(L"trace: could not map the trace file, err", ::GetLastError());
  }

  ReserveScratch();
  PVOID handler = ::AddVectoredExceptionHandler(1, &OnException);
  Log::Write(L"probe crash: vectored handler",
             handler ? L"installed" : L"REFUSED");
  ::SetUnhandledExceptionFilter(&OnUnhandledException);
  LogKnownOffsets();
}

void InstallEngineProbes() {
  // mozglue and nss3 reach the same exits and load the same way, and a call
  // through either of them would otherwise pass unseen.
  int hooked = 0;
  // ucrtbase is on the list because abort() lives there, and abort() reaches
  // TerminateProcess through ucrtbase's own import table, not xul's -- so
  // every abort in the engine has passed these hooks untouched.
  for (const wchar_t* name :
       {L"xul.dll", L"mozglue.dll", L"nss3.dll", L"ucrtbase.dll", L"gkcodecs.dll", L"mozavcodec.dll"}) {
    // LoadPackagedLibrary only finds modules inside the package, and ucrtbase
    // is not one -- it is the system's. Asking it for ucrtbase reported "not
    // loaded" about a library that was loaded all along, so the hooks that
    // matter most for abort() were never placed.
    HMODULE module = ::LoadPackagedLibrary(name, 0);
    if (!module) module = ::GetModuleHandleW(name);
    if (!module) {
      Log::WriteFromFault(std::wstring(L"probe crash: ") + name + L" not loaded");
      continue;
    }
    hooked += HookImports(module);
  }
  Log::WriteNum(L"probe crash: imports hooked", hooked);

  HMODULE xul = ::LoadPackagedLibrary(L"xul.dll", 0);
  if (!xul) return;

  ProbeDelayLoads(xul);
  HookWindowImports(xul);

  // Anything the engine reports as a raw address -- and it has no way to
  // report anything else -- is only meaningful against the base it was loaded
  // at, which ASLR changes every run.
  for (const wchar_t* name : {L"xul.dll", L"mozglue.dll", L"nss3.dll"}) {
    if (HMODULE module = ::GetModuleHandleW(name)) {
      Log::WriteFromFault(std::wstring(L"base: ") + name + L" at " +
                 Hex(reinterpret_cast<uintptr_t>(module)));
    }
  }

  // Whether the loader is enforcing Control Flow Guard on the engine decides
  // whether an indirect call can end the process outright, so it is worth
  // stating in the log next to the crash it might explain.
  auto* base = reinterpret_cast<unsigned char*>(xul);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  WORD characteristics = nt->OptionalHeader.DllCharacteristics;
  Log::Write(L"probe crash: xul.dll CFG",
             (characteristics & 0x4000) ? L"ENFORCED" : L"off");
}

// ---------------------------------------------------------------------------
// What the process's memory is made of, page by page.
//
// xul.dll is linked for 0x10000000 and the phone loads it wherever ASLR says;
// on ARM32 nearly every code page holds an absolute address (movw/movt pairs),
// so 98% of its 87 MB of code needs fixing up on load. A code page that still
// matches the file is free memory to the system -- dropped when memory is
// short and read back from flash when touched. A fixed-up page is not, and if
// the fix-up happened in the process, copy on write, it is the app's own
// memory: in the commit charge, in the ceiling, written to the pagefile
// instead of dropped. Which of the two this phone does decides whether loading
// the engine at a fixed address is worth anything, and the working set says
// it: every resident page carries a "shared" bit.
namespace {

// psapi.h is desktop-partition only; kernelbase exports the calls to apps
// all the same.
struct GeckoW10mProcessMemoryCounters {
  DWORD cb;
  DWORD PageFaultCount;
  SIZE_T PeakWorkingSetSize;
  SIZE_T WorkingSetSize;
  SIZE_T QuotaPeakPagedPoolUsage;
  SIZE_T QuotaPagedPoolUsage;
  SIZE_T QuotaPeakNonPagedPoolUsage;
  SIZE_T QuotaNonPagedPoolUsage;
  SIZE_T PagefileUsage;
  SIZE_T PeakPagefileUsage;
  SIZE_T PrivateUsage;
};
using QueryWorkingSetFn = BOOL(WINAPI*)(HANDLE, PVOID, DWORD);
using GetProcessMemoryInfoFn = BOOL(WINAPI*)(HANDLE, void*, DWORD);

FARPROC FindPsapi(const char* name) {
  for (const wchar_t* dll : {L"kernelbase.dll", L"kernel32.dll"}) {
    if (HMODULE module = ::GetModuleHandleW(dll)) {
      if (FARPROC fn = ::GetProcAddress(module, name)) return fn;
    }
  }
  return nullptr;
}

std::wstring MB(uint64_t pages) {
  wchar_t buf[32];
  ::swprintf_s(buf, L"%.1f", pages * 4096.0 / (1024 * 1024));
  return buf;
}

struct PageCount {
  uint64_t shared = 0;
  uint64_t priv = 0;
  uint64_t total() const { return shared + priv; }
  void add(bool isShared) { (isShared ? shared : priv)++; }
};

}  // namespace

void LogMemoryMap() {
  static const auto queryWorkingSet =
      reinterpret_cast<QueryWorkingSetFn>(FindPsapi("K32QueryWorkingSet"));
  static const auto getMemoryInfo = reinterpret_cast<GetProcessMemoryInfoFn>(
      FindPsapi("K32GetProcessMemoryInfo"));
  if (!queryWorkingSet) {
    Log::Write(L"memmap: QueryWorkingSet is not available");
    return;
  }

  // The working set: a count, then one word per resident page -- the page
  // number in the top 20 bits, bit 8 set if the page is shareable.
  static std::vector<ULONG_PTR> buffer(64 * 1024);
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (queryWorkingSet(::GetCurrentProcess(), buffer.data(),
                        static_cast<DWORD>(buffer.size() * sizeof(ULONG_PTR)))) {
      break;
    }
    if (::GetLastError() != ERROR_BAD_LENGTH) {
      Log::WriteNum(L"memmap: QueryWorkingSet failed", ::GetLastError());
      return;
    }
    buffer.resize(static_cast<size_t>(buffer[0]) + 8192);
    if (attempt == 3) return;
  }
  const size_t count = static_cast<size_t>(buffer[0]);
  std::vector<ULONG_PTR> pages(buffer.begin() + 1, buffer.begin() + 1 + count);
  std::sort(pages.begin(), pages.end());

  // xul.dll's sections, from its own headers.
  struct Section {
    wchar_t name[9];
    uintptr_t start, end;
    PageCount resident;
  };
  std::vector<Section> sections;
  uintptr_t xulStart = 0, xulEnd = 0, xulPreferred = 0;
  if (HMODULE xul = ::GetModuleHandleW(L"xul.dll")) {
    auto* base = reinterpret_cast<unsigned char*>(xul);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(
        base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    xulStart = reinterpret_cast<uintptr_t>(base);
    xulEnd = xulStart + nt->OptionalHeader.SizeOfImage;
    xulPreferred = nt->OptionalHeader.ImageBase;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
      Section s{};
      for (int c = 0; c < 8 && sec->Name[c]; ++c) s.name[c] = sec->Name[c];
      s.start = xulStart + sec->VirtualAddress;
      s.end = s.start + ((sec->Misc.VirtualSize + 4095) & ~4095u);
      sections.push_back(s);
    }
  }

  PageCount xulOther, images, mapped, privateMem;
  MEMORY_BASIC_INFORMATION region{};
  uintptr_t regionEnd = 0;
  for (ULONG_PTR entry : pages) {
    const uintptr_t addr = entry & ~uintptr_t(4095);
    const bool isShared = (entry & 0x100) != 0;
    if (addr >= xulStart && addr < xulEnd) {
      bool found = false;
      for (Section& s : sections) {
        if (addr >= s.start && addr < s.end) {
          s.resident.add(isShared);
          found = true;
          break;
        }
      }
      if (!found) xulOther.add(isShared);
      continue;
    }
    if (addr >= regionEnd || addr < reinterpret_cast<uintptr_t>(region.BaseAddress)) {
      if (::VirtualQuery(reinterpret_cast<void*>(addr), &region,
                         sizeof(region)) != sizeof(region)) {
        region = {};
        regionEnd = 0;
        privateMem.add(isShared);
        continue;
      }
      regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    }
    if (region.Type == MEM_IMAGE) {
      images.add(isShared);
    } else if (region.Type == MEM_MAPPED) {
      mapped.add(isShared);
    } else {
      privateMem.add(isShared);
    }
  }

  // How the loader left xul.dll's pages: execute-read as linked, or
  // write-copy -- the mark of a fix-up done in the process.
  uint64_t protBytes[5] = {};  // RX, R, RW/WC, XWC/XRW, other
  for (uintptr_t at = xulStart; at && at < xulEnd;) {
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) !=
        sizeof(info)) {
      break;
    }
    const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    const uint64_t bytes = (std::min(next, xulEnd) - at);
    if (info.State == MEM_COMMIT) {
      switch (info.Protect & 0xff) {
        case PAGE_EXECUTE_READ: protBytes[0] += bytes; break;
        case PAGE_READONLY: protBytes[1] += bytes; break;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY: protBytes[2] += bytes; break;
        case PAGE_EXECUTE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE: protBytes[3] += bytes; break;
        default: protBytes[4] += bytes; break;
      }
    }
    if (next <= at) break;
    at = next;
  }

  if (xulStart) {
    std::wstring line = L"memmap: xul.dll at " + Hex(xulStart) + L", linked for " +
                        Hex(xulPreferred) +
                        (xulStart == xulPreferred ? L" (in place)" : L" (moved)") +
                        L"; in memory, shared+private MB:";
    for (const Section& s : sections) {
      if (!s.resident.total()) continue;
      line += std::wstring(L" ") + s.name + L" " + MB(s.resident.shared) + L"+" +
              MB(s.resident.priv) + L" of " + MB((s.end - s.start) / 4096);
    }
    if (xulOther.total()) {
      line += L", headers " + MB(xulOther.shared) + L"+" + MB(xulOther.priv);
    }
    Log::Write(line);
    Log::Write(L"memmap: xul.dll mapped as execute-read " + MB(protBytes[0] / 4096) +
               L" MB, read-only " + MB(protBytes[1] / 4096) + L" MB, read-write " +
               MB(protBytes[2] / 4096) + L" MB, execute-write " +
               MB(protBytes[3] / 4096) + L" MB, other " + MB(protBytes[4] / 4096) +
               L" MB");
  }

  uint64_t xulShared = xulOther.shared, xulPriv = xulOther.priv;
  for (const Section& s : sections) {
    xulShared += s.resident.shared;
    xulPriv += s.resident.priv;
  }
  std::wstring line = L"memmap: working set " + MB(count) +
                      L" MB, shared+private MB: xul.dll " + MB(xulShared) + L"+" +
                      MB(xulPriv) + L", other images " + MB(images.shared) + L"+" +
                      MB(images.priv) + L", mapped " + MB(mapped.shared) + L"+" +
                      MB(mapped.priv) + L", allocated " + MB(privateMem.shared) +
                      L"+" + MB(privateMem.priv);
  GeckoW10mProcessMemoryCounters counters{};
  counters.cb = sizeof(counters);
  if (getMemoryInfo &&
      getMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters))) {
    line += L"; commit charge " + MB(counters.PrivateUsage / 4096) + L" MB";
  }
  Log::Write(line);
}

}  // namespace gecko_w10m::engine
