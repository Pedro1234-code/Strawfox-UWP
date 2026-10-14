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

MIDL_INTERFACE("5CA296B2-2C25-4D22-B785-B885C8201E6A")
IStorageItemHandleAccess : public ::IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE Create(DWORD access, DWORD sharing,
                                           DWORD options, ::IUnknown* oplock,
                                           HANDLE* handle) = 0;
};

namespace gecko_w10m::client {
namespace {

constexpr wchar_t kDestinationToken[] = L"Gecko.Downloads.v1";

std::wstring gStagingPath;
std::wstring gUploadStagingPath;
std::wstring gSaveStagingPath;
CoreDispatcher gDispatcher{nullptr};
std::once_flag gInitializeOnce;
std::atomic<bool> gPickerStarted{false};
std::atomic<DownloadBroker::DirectoryChangedFn> gDirectoryChanged{nullptr};
std::mutex gDestinationMutex;
std::wstring gAuthorizedRoot;

void CacheDestinationPath(std::wstring path) {
  std::lock_guard<std::mutex> lock(gDestinationMutex);
  gAuthorizedRoot = std::move(path);
}

bool IsCachedAuthorizedPath(const wchar_t* path) {
  if (!path) return false;
  std::lock_guard<std::mutex> lock(gDestinationMutex);
  if (gAuthorizedRoot.empty()) return false;
  const size_t rootLength = gAuthorizedRoot.size();
  return _wcsicmp(gAuthorizedRoot.c_str(), path) == 0 ||
         (wcslen(path) > rootLength &&
          _wcsnicmp(gAuthorizedRoot.c_str(), path, rootLength) == 0 &&
          (path[rootLength] == L'\\' || path[rootLength] == L'/'));
}

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
    auto folder = StorageApplicationPermissions::FutureAccessList()
                      .GetFolderAsync(kDestinationToken)
                      .get();
    ApplicationData::Current().LocalSettings().Values().Insert(
        L"DownloadFolderPath", box_value(folder.Path()));
    return folder;
  } catch (winrt::hresult_error const& error) {
    Log::Write(L"download: saved folder permission is no longer valid",
               std::wstring(error.message()));
    try {
      StorageApplicationPermissions::FutureAccessList().Remove(
          kDestinationToken);
    } catch (...) {
    }
    CacheDestinationPath({});
    return nullptr;
  }
}

std::wstring SavedDestinationPath() {
  if (!HasDestination()) return {};
  try {
    auto value = ApplicationData::Current().LocalSettings().Values().TryLookup(
        L"DownloadFolderPath");
    return value ? std::wstring(unbox_value<hstring>(value)) : std::wstring{};
  } catch (...) {
    return {};
  }
}

bool IsAuthorizedChild(StorageFolder const& folder, const wchar_t* path,
                       std::wstring* leaf) {
  if (!path || !leaf) return false;
  std::wstring root(folder.Path());
  std::wstring candidate(path);
  if (root.empty() || candidate.size() <= root.size() ||
      _wcsnicmp(candidate.c_str(), root.c_str(), root.size()) != 0 ||
      (candidate[root.size()] != L'\\' && candidate[root.size()] != L'/')) {
    return false;
  }
  *leaf = candidate.substr(root.size() + 1);
  return !leaf->empty() && leaf->find_first_of(L"\\/") == std::wstring::npos;
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

void RememberDestination(StorageFolder const& folder) {
  StorageApplicationPermissions::FutureAccessList().AddOrReplace(
      kDestinationToken, folder);
  ApplicationData::Current().LocalSettings().Values().Insert(
      L"DownloadFolderName", box_value(folder.DisplayName()));
  ApplicationData::Current().LocalSettings().Values().Insert(
      L"DownloadFolderPath", box_value(folder.Path()));
  CacheDestinationPath(std::wstring(folder.Path()));
  Log::Write(L"download: destination authorised", std::wstring(folder.Path()));
  if (auto changed = gDirectoryChanged.load()) {
    changed(folder.Path().c_str());
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
        RememberDestination(folder);
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
          L"Choose a folder for downloads. Gecko will write files directly "
          L"there, and Windows will remember this permission.")));
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

std::wstring DownloadBroker::Initialize(std::wstring_view localStatePath) {
  std::call_once(gInitializeOnce, [localState = std::wstring(localStatePath)] {
    gStagingPath = localState + L"\\profile\\download-staging";
    gUploadStagingPath = localState + L"\\profile\\upload-staging";
    gSaveStagingPath = localState + L"\\profile\\save-staging";
    ::CreateDirectoryW((localState + L"\\profile").c_str(), nullptr);
    ::CreateDirectoryW(gStagingPath.c_str(), nullptr);
    ::CreateDirectoryW(gUploadStagingPath.c_str(), nullptr);
    ::CreateDirectoryW(gSaveStagingPath.c_str(), nullptr);
    gDispatcher = Window::Current().Dispatcher();
    Log::Write(L"download: fallback staging", gStagingPath);
    std::thread(MonitorDownloads).detach();
  });

  std::wstring path = SavedDestinationPath();
  if (!path.empty()) {
    CacheDestinationPath(path);
    Log::Write(L"download: using authorized folder directly", path);
    return path;
  }
  Log::Write(L"download: destination not chosen; using fallback staging",
             gStagingPath);
  RequestDestination();
  return gStagingPath;
}

void DownloadBroker::SetDirectoryChangedSink(DirectoryChangedFn sink) {
  gDirectoryChanged.store(sink);
  if (!sink) return;
  std::wstring path = SavedDestinationPath();
  if (!path.empty()) {
    sink(path.c_str());
    return;
  }
  std::thread([sink] {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (auto destination = GetDestination()) {
      const std::wstring restored(destination.Path());
      if (!restored.empty()) sink(restored.c_str());
    }
  }).detach();
}

intptr_t DownloadBroker::OpenAuthorizedFile(const wchar_t* path,
                                            int32_t flags) {
  if (!IsCachedAuthorizedPath(path)) return -1;
  try {
    auto folder = GetDestination();
    std::wstring leaf;
    if (!folder || !IsAuthorizedChild(folder, path, &leaf)) return -1;
    StorageFile file{nullptr};
    if (flags & 4) {
      const auto collision = (flags & 32)
                                 ? CreationCollisionOption::FailIfExists
                                 : CreationCollisionOption::OpenIfExists;
      file = folder.CreateFileAsync(leaf, collision).get();
    } else {
      file = folder.GetFileAsync(leaf).get();
    }
    winrt::com_ptr<IStorageItemHandleAccess> access;
    winrt::check_hresult(reinterpret_cast<::IUnknown*>(winrt::get_abi(file))
                             ->QueryInterface(__uuidof(IStorageItemHandleAccess),
                                              access.put_void()));
    // IStorageItemHandleAccess::Create takes HANDLE_ACCESS_OPTIONS, not a
    // native ACCESS_MASK. Passing FILE_GENERIC_READ/WRITE-style masks makes
    // the WinRT implementation reject the call with E_INVALIDARG.
    DWORD desired = 0;
    if (flags & 1) desired |= 1;  // HAO_READ
    if (flags & 2) desired |= 2;  // HAO_WRITE
    if (!desired) desired = 1;
    HANDLE handle = INVALID_HANDLE_VALUE;
    winrt::check_hresult(access->Create(desired, 1 | 2 | 4, 0, nullptr, &handle));
    if ((flags & 8) && (flags & 2)) {
      LARGE_INTEGER zero{};
      ::SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN);
      ::SetEndOfFile(handle);
    } else if ((flags & 16) && (flags & 2)) {
      LARGE_INTEGER zero{};
      ::SetFilePointerEx(handle, zero, nullptr, FILE_END);
    }
    return reinterpret_cast<intptr_t>(handle);
  } catch (winrt::hresult_error const& error) {
    Log::Write(L"download: brokered open failed", std::wstring(error.message()));
    return -1;
  }
}

int32_t DownloadBroker::AuthorizedFileExists(const wchar_t* path) {
  if (!IsCachedAuthorizedPath(path)) return -1;
  try {
    auto folder = GetDestination();
    if (!folder || !path) return -1;
    const std::wstring root(folder.Path());
    if (_wcsicmp(root.c_str(), path) == 0) return 2;
    std::wstring leaf;
    if (!IsAuthorizedChild(folder, path, &leaf)) return -1;
    auto item = folder.GetItemAsync(leaf).get();
    return item.IsOfType(StorageItemTypes::Folder) ? 2 : 1;
  } catch (winrt::hresult_error const& error) {
    const auto code = error.code();
    return code == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
                   code == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
               ? 0
               : -1;
  }
}

int32_t DownloadBroker::StatAuthorizedFile(const wchar_t* path, int64_t* size) {
  if (!size || !IsCachedAuthorizedPath(path)) return -1;
  *size = 0;
  try {
    auto folder = GetDestination();
    if (!folder || !path) return -1;
    const std::wstring root(folder.Path());
    if (_wcsicmp(root.c_str(), path) == 0) return 2;
    std::wstring leaf;
    if (!IsAuthorizedChild(folder, path, &leaf)) return -1;
    auto item = folder.GetItemAsync(leaf).get();
    if (item.IsOfType(StorageItemTypes::Folder)) return 2;
    auto file = item.as<StorageFile>();
    try {
      *size = static_cast<int64_t>(file.GetBasicPropertiesAsync().get().Size());
    } catch (winrt::hresult_error const&) {
      // An actively written .part can temporarily deny the metadata query.
      // It is still a valid file; report an unknown/zero size for this pass.
      *size = 0;
    }
    return 1;
  } catch (winrt::hresult_error const&) {
    return -1;
  }
}

int32_t DownloadBroker::MoveAuthorizedFile(const wchar_t* source,
                                           const wchar_t* destination) {
  if (!IsCachedAuthorizedPath(source) ||
      !IsCachedAuthorizedPath(destination)) return -1;
  try {
    auto folder = GetDestination();
    std::wstring sourceLeaf;
    std::wstring destinationLeaf;
    if (!folder || !IsAuthorizedChild(folder, source, &sourceLeaf) ||
        !IsAuthorizedChild(folder, destination, &destinationLeaf)) return -1;
    auto file = folder.GetFileAsync(sourceLeaf).get();
    file.RenameAsync(destinationLeaf, NameCollisionOption::ReplaceExisting).get();
    return 1;
  } catch (winrt::hresult_error const& error) {
    Log::Write(L"download: brokered rename failed", std::wstring(error.message()));
    return -1;
  }
}

int32_t DownloadBroker::RemoveAuthorizedFile(const wchar_t* path) {
  if (!IsCachedAuthorizedPath(path)) return -1;
  try {
    auto folder = GetDestination();
    std::wstring leaf;
    if (!folder || !IsAuthorizedChild(folder, path, &leaf)) return -1;
    folder.GetFileAsync(leaf).get().DeleteAsync(StorageDeleteOption::PermanentDelete)
        .get();
    return 1;
  } catch (winrt::hresult_error const& error) {
    return error.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? 1 : -1;
  }
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
      if (mode == 2) {
        FolderPicker picker;
        picker.ViewMode(PickerViewMode::List);
        picker.SuggestedStartLocation(PickerLocationId::Downloads);
        picker.SettingsIdentifier(L"GeckoDownloads");
        picker.CommitButtonText(L"Use for downloads");
        picker.FileTypeFilter().Append(L"*");
        auto operation = picker.PickSingleFolderAsync();
        operation.Completed([output, completed](auto const& async,
                                                AsyncStatus status) {
          try {
            if (status == AsyncStatus::Completed) {
              auto folder = async.GetResults();
              if (folder) {
                RememberDestination(folder);
                *output = Narrow(std::wstring(folder.Path()));
              }
            }
          } catch (winrt::hresult_error const& error) {
            Log::Write(L"download: folder picker completion failed",
                       std::wstring(error.message()));
          }
          ::SetEvent(completed);
        });
      } else if (mode == 1) {
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
