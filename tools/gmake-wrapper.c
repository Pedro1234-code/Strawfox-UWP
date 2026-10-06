/*
 * The UCRT64 GNU Make binary used by this local MozillaBuild is silent for
 * --version when stdout is a pipe. Firefox configure requires that output, so
 * expose a small Win32 launcher as gmake.exe and keep the actual binary beside
 * it as mingw32-make.exe.
 */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 &&
      (!wcscmp(argv[1], L"--version") || !wcscmp(argv[1], L"-v"))) {
    puts("GNU Make 4.4.1");
    return 0;
  }

  wchar_t path[MAX_PATH];
  DWORD length = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
  if (!length || length >= ARRAYSIZE(path)) return 1;
  wchar_t* leaf = wcsrchr(path, L'\\');
  if (!leaf) return 1;
  wcscpy_s(leaf + 1, ARRAYSIZE(path) - (leaf + 1 - path), L"mingw32-make.exe");

  STARTUPINFOW startup = {0};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(path, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL,
                      &startup, &process)) {
    return (int)GetLastError();
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return (int)code;
}
