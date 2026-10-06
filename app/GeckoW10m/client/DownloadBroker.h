#pragma once

#include <string_view>

namespace gecko_w10m::client {

// Bridges Gecko's ordinary file downloads out of the app container. Gecko
// writes into LocalState; the shell copies completed files into a folder the
// user selected with FolderPicker. The FutureAccessList token, rather than a
// raw path, is the authority to write there.
class DownloadBroker {
 public:
  static void Initialize(std::wstring_view localStatePath);

  // Called synchronously from Gecko's thread. The actual picker runs on the
  // XAML UI thread; this call waits like a desktop modal file dialog. Modes
  // match nsIFilePicker: 0 open, 1 save, 2 folder, 3 open multiple.
  static int32_t PickFile(int32_t mode, const char* title,
                          const char* defaultName, const char* extensions,
                          char* result, int32_t resultCapacity);
};

}  // namespace gecko_w10m::client
