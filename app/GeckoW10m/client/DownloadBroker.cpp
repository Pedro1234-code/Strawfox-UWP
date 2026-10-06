#include "pch.h"
#include "client/DownloadBroker.h"

#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Pickers.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "client/Log.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::AccessCache;
using namespace winrt::Windows::Storage::Pickers;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace gecko_w10m::client {
namespace {

constexpr wchar_t kDestinationToken[] = L"Gecko.Downloads.v1";

std::wstring gStagingPath;
std::wstring gUploadStagingPath;
std::wstring gSaveStagingPath;
CoreDispatcher gDispatcher{nullptr};
std::once_flag gInitializeOnce;
std::atomic<bool> gPickerStarted{false};

struct SeenFile {
  uint64_t size = 0;
  unsigned stableScans = 0;
};

bool EndsWith(std::wstring const& value, std::wstring_view suffix) {
  return value.size() >= suffix.size() &&
         _wcsicmp(value.c_str() + value.size() - suffix.size(),
                   std::wstring(suffix).c_str()) == 0;
}

bool HasDestination() {
  try {
    return StorageApplicationPermissions::FutureAccessList().ContainsItem(
        kDestinationToken);
  } catch (...) {
    return false;
  }
}

StorageFolder GetDestination() {
  try {
    return StorageApplicationPermissions::FutureAccessList()
        .GetFolderAsync(kDestinationToken)
        .get();
  } catch (winrt::hresult_error const& error) {
    Log::Write(L"download: saved folder permission is no longer valid",
               std::wstring(error.message()));
    try {
      StorageApplicationPermissions::FutureAccessList().Remove(
          kDestinationToken);
    } catch (...) {
    }
    return nullptr;
  }
}

void RequestDestination();

std::wstring Widen(const char* value) {
  if (!value || !*value) return {};
  int size = ::MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
  if (size <= 1) return {};
  std::wstring result(static_cast<size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), size);
  result.resize(static_cast<size_t>(size - 1));
  return result;
}

std::string Narrow(std::wstring_view value) {
  if (value.empty()) return {};
  int size = ::WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0,
                                   nullptr, nullptr);
  std::string result(static_cast<size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
  return result;
}

std::vector<hstring> ParseExtensions(const char* encoded) {
  std::vector<hstring> result;
  std::wstring text = Widen(encoded);
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(L';', start);
    std::wstring extension = text.substr(
        start, end == std::wstring::npos ? std::wstring::npos : end - start);
    extension.erase(std::remove_if(extension.begin(), extension.end(),
                                   [](wchar_t ch) { return iswspace(ch) != 0; }),
                    extension.end());
    if (!extension.empty() && extension != L"*" && extension != L"*.*") {
      if (extension.front() == L'*') extension.erase(extension.begin());
      if (!extension.empty() && extension.front() != L'.') {
        extension.insert(extension.begin(), L'.');
      }
      if (extension.size() > 1) result.emplace_back(extension);
    }
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  return result;
}

void ExportSaveWhenReady(std::wstring stagingPath, StorageFile destination) {
  std::thread([path = std::move(stagingPath), destination]() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    uint64_t previousSize = UINT64_MAX;
    unsigned stable = 0;
    // Gecko creates and closes the selected path after the picker returns.
    // Wait for two identical observations before replacing the brokered file.
    for (unsigned attempt = 0; attempt < 900; ++attempt) {
      ::Sleep(1000);
      try {
        auto file = StorageFile::GetFileFromPathAsync(path).get();
        uint64_t size = file.GetBasicPropertiesAsync().get().Size();
        stable = size == previousSize ? stable + 1 : 0;
        previousSize = size;
        if (stable < 2) continue;
        file.CopyAndReplaceAsync(destination).get();
        file.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
        Log::Write(L"save as: exported", std::wstring(destination.Path()));
        return;
      } catch (winrt::hresult_error const& error) {
        if (error.code() != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
          Log::Write(L"save as: waiting for Gecko output",
                     std::wstring(error.message()));
        }
      }
    }
    Log::Write(L"save as: timed out waiting for Gecko output", path);
  }).detach();
}

void MonitorDownloads() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  std::map<std::wstring, SeenFile> seen;

  for (;;) {
    ::Sleep(2000);

    try {
      auto staging = StorageFolder::GetFolderFromPathAsync(gStagingPath).get();
      auto files = staging.GetFilesAsync().get();
      if (!HasDestination()) {
        if (files.Size() > 0) {
          RequestDestination();
        }
        continue;
      }
      std::map<std::wstring, SeenFile> next;
      std::set<std::wstring> names;
      for (auto const& file : files) {
        names.emplace(file.Name());
      }

      for (auto const& file : files) {
        const std::wstring name(file.Name());
        // Firefox writes an adjacent .part while the transfer is active.
        // Other temporary suffixes are ignored defensively as well.
        if (EndsWith(name, L".part") || EndsWith(name, L".tmp") ||
            EndsWith(name, L".download") || names.count(name + L".part")) {
          continue;
        }

        const uint64_t size =
            file.GetBasicPropertiesAsync().get().Size();
        SeenFile current{size, 1};
        if (auto old = seen.find(name);
            old != seen.end() && old->second.size == size) {
          current.stableScans = old->second.stableScans + 1;
        }
        next.emplace(name, current);

        // Two equal observations, two seconds apart, prevent exporting a
        // target that a server or download backend is still extending.
        if (current.stableScans < 2) {
          continue;
        }

        auto destination = GetDestination();
        if (!destination) {
          break;
        }
        try {
          auto copied = file.CopyAsync(
                                destination, file.Name(),
                                NameCollisionOption::GenerateUniqueName)
                            .get();
          file.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
          next.erase(name);
          Log::Write(L"download: saved", std::wstring(copied.Path()));
        } catch (winrt::hresult_error const& error) {
          Log::Write(L"download: could not export " + name,
                     std::wstring(error.message()));
        }
      }
      seen.swap(next);
    } catch (winrt::hresult_error const& error) {
      Log::Write(L"download: staging scan failed",
                 std::wstring(error.message()));
    } catch (...) {
      Log::Write(L"download: staging scan failed");
    }
  }
}

void PickDestination() {
  try {
    FolderPicker picker;
    picker.ViewMode(PickerViewMode::List);
    picker.SuggestedStartLocation(PickerLocationId::Downloads);
    picker.SettingsIdentifier(L"GeckoDownloads");
    picker.CommitButtonText(L"Use for downloads");
    // FolderPicker requires at least one filter even though it selects a
    // folder rather than a file.
    picker.FileTypeFilter().Append(L"*");

    auto operation = picker.PickSingleFolderAsync();
    operation.Completed([](auto const& completed, AsyncStatus status) {
      if (status != AsyncStatus::Completed) {
        Log::Write(L"download: no destination folder selected");
        return;
      }
      try {
        auto folder = completed.GetResults();
        if (!folder) {
          Log::Write(L"download: folder selection cancelled");
          return;
        }
        StorageApplicationPermissions::FutureAccessList().AddOrReplace(
            kDestinationToken, folder);
        ApplicationData::Current().LocalSettings().Values().Insert(
            L"DownloadFolderName", box_value(folder.DisplayName()));
        Log::Write(L"download: destination authorised",
                   std::wstring(folder.Path()));
      } catch (winrt::hresult_error const& error) {
        Log::Write(L"download: could not remember destination",
                   std::wstring(error.message()));
      }
    });
  } catch (winrt::hresult_error const& error) {
    Log::Write(L"download: FolderPicker failed",
               std::wstring(error.message()));
  }
}

void RequestDestination() {
  if (HasDestination() || !gDispatcher || gPickerStarted.exchange(true)) {
    return;
  }

  gDispatcher.RunAsync(CoreDispatcherPriority::Normal, [] {
    try {
      ContentDialog dialog;
      dialog.Title(box_value(hstring(L"Choose where to save downloads")));
      dialog.Content(box_value(hstring(
          L"Gecko keeps a download inside the app until it is complete. "
          L"Choose a folder now so completed files can be saved somewhere "
          L"you can access. Windows will remember this permission.")));
      dialog.PrimaryButtonText(L"Choose folder");
      dialog.CloseButtonText(L"Not now");
      dialog.DefaultButton(ContentDialogButton::Primary);

      auto shown = dialog.ShowAsync();
      shown.Completed([](auto const& completed, AsyncStatus status) {
        if (status == AsyncStatus::Completed &&
            completed.GetResults() == ContentDialogResult::Primary) {
          // Let the dialog leave the visual tree before presenting the
          // system picker, and guarantee that the picker starts on the UI
          // thread even if this completion is delivered elsewhere.
          gDispatcher.RunAsync(CoreDispatcherPriority::Normal,
                               [] { PickDestination(); });
        } else {
          Log::Write(L"download: destination choice postponed");
        }
      });
    } catch (winrt::hresult_error const& error) {
      Log::Write(L"download: explanation dialog failed",
                 std::wstring(error.message()));
    }
  });
}

}  // namespace

void DownloadBroker::Initialize(std::wstring_view localStatePath) {
  std::call_once(gInitializeOnce, [localState = std::wstring(localStatePath)] {
    gStagingPath = localState + L"\\profile\\download-staging";
    gUploadStagingPath = localState + L"\\profile\\upload-staging";
    gSaveStagingPath = localState + L"\\profile\\save-staging";
    ::CreateDirectoryW((localState + L"\\profile").c_str(), nullptr);
    ::CreateDirectoryW(gStagingPath.c_str(), nullptr);
    ::CreateDirectoryW(gUploadStagingPath.c_str(), nullptr);
    ::CreateDirectoryW(gSaveStagingPath.c_str(), nullptr);
    gDispatcher = Window::Current().Dispatcher();
    Log::Write(L"download: Gecko staging", gStagingPath);
    std::thread(MonitorDownloads).detach();
  });
}

int32_t DownloadBroker::PickFile(int32_t mode, const char* title,
                                 const char* defaultName,
                                 const char* extensions, char* result,
                                 int32_t resultCapacity) {
  if (!result || resultCapacity <= 0 || !gDispatcher) return 0;
  result[0] = '\0';

  HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!completed) return 0;
  auto output = std::make_shared<std::string>();
  auto extensionsList = ParseExtensions(extensions);
  std::wstring pickerTitle = Widen(title);
  std::wstring suggestedName = Widen(defaultName);

  gDispatcher.RunAsync(CoreDispatcherPriority::Normal,
                       [mode, pickerTitle = std::move(pickerTitle),
                        suggestedName = std::move(suggestedName),
                        extensionsList = std::move(extensionsList), output,
                        completed]() mutable {
    try {
      if (mode == 1) {
        FileSavePicker picker;
        picker.SuggestedStartLocation(PickerLocationId::Downloads);
        picker.SettingsIdentifier(L"GeckoSaveAs");
        if (!pickerTitle.empty()) picker.CommitButtonText(L"Save");
        if (!suggestedName.empty()) picker.SuggestedFileName(suggestedName);
        if (extensionsList.empty()) {
          auto dot = suggestedName.find_last_of(L'.');
          extensionsList.emplace_back(
              dot == std::wstring::npos ? L".html" : suggestedName.substr(dot));
        }
        picker.FileTypeChoices().Insert(
            L"File", single_threaded_vector<hstring>(std::move(extensionsList)));
        auto operation = picker.PickSaveFileAsync();
        operation.Completed([output, completed](auto const& async,
                                                AsyncStatus status) {
          try {
            if (status == AsyncStatus::Completed) {
              auto destination = async.GetResults();
              if (destination) {
                std::wstring staging = gSaveStagingPath + L"\\" +
                                       std::wstring(destination.Name());
                ::DeleteFileW(staging.c_str());
                *output = Narrow(staging);
                ExportSaveWhenReady(staging, destination);
              }
            }
          } catch (winrt::hresult_error const& error) {
            Log::Write(L"save as: picker completion failed",
                       std::wstring(error.message()));
          }
          ::SetEvent(completed);
        });
      } else {
        FileOpenPicker picker;
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.SettingsIdentifier(L"GeckoUpload");
        if (extensionsList.empty()) {
          picker.FileTypeFilter().Append(L"*");
        } else {
          for (auto const& extension : extensionsList) {
            picker.FileTypeFilter().Append(extension);
          }
        }
        if (mode == 3) {
          auto operation = picker.PickMultipleFilesAsync();
          operation.Completed([output, completed](auto const& async,
                                                  AsyncStatus status) {
            try {
              if (status == AsyncStatus::Completed) {
                auto sources = async.GetResults();
                std::thread([sources, output, completed]() {
                  winrt::init_apartment(winrt::apartment_type::multi_threaded);
                  try {
                    auto folder = StorageFolder::GetFolderFromPathAsync(
                                      gUploadStagingPath).get();
                    for (auto const& source : sources) {
                      auto copy = source.CopyAsync(
                                            folder, source.Name(),
                                            NameCollisionOption::GenerateUniqueName)
                                      .get();
                      if (!output->empty()) output->push_back('\n');
                      output->append(Narrow(std::wstring(copy.Path())));
                    }
                  } catch (winrt::hresult_error const& error) {
                    Log::Write(L"upload: copying selected files failed",
                               std::wstring(error.message()));
                  }
                  ::SetEvent(completed);
                }).detach();
                return;
              }
            } catch (winrt::hresult_error const& error) {
              Log::Write(L"upload: picker completion failed",
                         std::wstring(error.message()));
            }
            ::SetEvent(completed);
          });
        } else {
          auto single = picker.PickSingleFileAsync();
          single.Completed([output, completed](auto const& async,
                                               AsyncStatus status) {
            try {
              if (status == AsyncStatus::Completed) {
                auto source = async.GetResults();
                if (source) {
                  std::thread([source, output, completed]() {
                    winrt::init_apartment(
                        winrt::apartment_type::multi_threaded);
                    try {
                      auto folder = StorageFolder::GetFolderFromPathAsync(
                                        gUploadStagingPath).get();
                      auto copy = source.CopyAsync(
                                            folder, source.Name(),
                                            NameCollisionOption::GenerateUniqueName)
                                      .get();
                      *output = Narrow(std::wstring(copy.Path()));
                    } catch (winrt::hresult_error const& error) {
                      Log::Write(L"upload: copying selected file failed",
                                 std::wstring(error.message()));
                    }
                    ::SetEvent(completed);
                  }).detach();
                  return;
                }
              }
            } catch (winrt::hresult_error const& error) {
              Log::Write(L"upload: picker completion failed",
                         std::wstring(error.message()));
            }
            ::SetEvent(completed);
          });
        }
      }
    } catch (winrt::hresult_error const& error) {
      Log::Write(L"file picker: could not open", std::wstring(error.message()));
      ::SetEvent(completed);
    }
  });

  ::WaitForSingleObject(completed, INFINITE);
  ::CloseHandle(completed);
  if (output->empty() || output->size() >= static_cast<size_t>(resultCapacity)) {
    return 0;
  }
  std::memcpy(result, output->c_str(), output->size() + 1);
  return 1;
}

}  // namespace gecko_w10m::client
