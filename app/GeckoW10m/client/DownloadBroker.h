#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace gecko_w10m::client {

// Bridges Gecko's ordinary file downloads out of the app container. Gecko
// writes into LocalState; the shell copies completed files into a folder the
// user selected with FolderPicker. The FutureAccessList token, rather than a
// raw path, is the authority to write there.
class DownloadBroker {
 public:
  using DirectoryChangedFn = void (*)(const wchar_t* path);
  static std::wstring Initialize(std::wstring_view localStatePath);
  static void SetDirectoryChangedSink(DirectoryChangedFn sink);
  static intptr_t OpenAuthorizedFile(const wchar_t* path, int32_t flags);
  static int32_t AuthorizedFileExists(const wchar_t* path);
  static int32_t StatAuthorizedFile(const wchar_t* path, int64_t* size);
  static int32_t MoveAuthorizedFile(const wchar_t* source,
                                    const wchar_t* destination);
  static int32_t RemoveAuthorizedFile(const wchar_t* path);

  // Called synchronously from Gecko's thread. The actual picker runs on the
  // XAML UI thread; this call waits like a desktop modal file dialog. Modes
  // match nsIFilePicker: 0 open, 1 save, 2 folder, 3 open multiple.
  static int32_t PickFile(int32_t mode, const char* title,
                          const char* defaultName, const char* extensions,
                          char* result, int32_t resultCapacity);
};

}  // namespace gecko_w10m::client
