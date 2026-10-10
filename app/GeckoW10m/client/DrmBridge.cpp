#include "pch.h"
#include "client/DrmBridge.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Protection.h>
#include <winrt/Windows.Media.Protection.PlayReady.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "client/Log.h"

namespace gecko_w10m::client {

namespace {

using namespace winrt;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Media::Protection::PlayReady;
using winrt::Windows::Security::Cryptography::CryptographicBuffer;
using winrt::Windows::Storage::Streams::IBuffer;

void (*gReply)(const char*) = nullptr;
std::mutex gLock;
// One license-acquisition request per session, kept until the response.
std::map<std::wstring, PlayReadyLicenseAcquisitionServiceRequest> gRequests;

std::string Narrow(std::wstring_view w) {
  if (w.empty()) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
  std::string out(n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n,
                        nullptr, nullptr);
  return out;
}

std::wstring Widen(const char* s) {
  if (!s || !*s) return {};
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring out(n ? n - 1 : 0, L'\0');
  if (n) ::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
  return out;
}

std::vector<uint8_t> FromBase64(hstring const& text) {
  IBuffer buf = CryptographicBuffer::DecodeFromBase64String(text);
  com_array<uint8_t> bytes;
  CryptographicBuffer::CopyToByteArray(buf, bytes);
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

hstring ToBase64(array_view<uint8_t const> bytes) {
  IBuffer buf = CryptographicBuffer::CreateFromByteArray(bytes);
  return CryptographicBuffer::EncodeToBase64String(buf);
}

void Send(JsonObject const& obj) {
  if (gReply) {
    gReply(Narrow(obj.Stringify()).c_str());
  }
}

JsonObject Failure(double id, std::wstring const& what, hresult code = 0) {
  JsonObject o;
  o.SetNamedValue(L"id", JsonValue::CreateNumberValue(id));
  o.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(false));
  wchar_t buf[512];
  swprintf_s(buf, 512, L"%s (0x%08x)", what.c_str(),
             static_cast<unsigned>(code));
  o.SetNamedValue(L"error", JsonValue::CreateStringValue(buf));
  Log::Write(L"drm: " + std::wstring(buf));
  return o;
}

// The PlayReady Object out of a 'pssh' box, if that is what arrived. A PSSH
// box: size(4) 'pssh'(4) version(1) flags(3) systemId(16) [v1: kidCount(4)
// kids(16 each)] dataSize(4) data. Anything else is handed on as it is.
std::vector<uint8_t> UnwrapPssh(std::vector<uint8_t> const& in,
                                std::wstring& note) {
  static const uint8_t kPlayReady[16] = {0x9a, 0x04, 0xf0, 0x79, 0x98, 0x40,
                                         0x42, 0x86, 0xab, 0x92, 0xe6, 0x5b,
                                         0xe0, 0x88, 0x5f, 0x95};
  size_t off = 0;
  while (off + 32 <= in.size()) {
    uint32_t size = (in[off] << 24) | (in[off + 1] << 16) | (in[off + 2] << 8) | in[off + 3];
    if (size < 32 || off + size > in.size() ||
        memcmp(&in[off + 4], "pssh", 4) != 0) {
      break;
    }
    uint8_t version = in[off + 8];
    size_t p = off + 12;
    bool ours = memcmp(&in[p], kPlayReady, 16) == 0;
    p += 16;
    if (version > 0) {
      uint32_t kidCount = (in[p] << 24) | (in[p + 1] << 16) | (in[p + 2] << 8) | in[p + 3];
      p += 4 + 16 * (size_t)kidCount;
    }
    if (p + 4 > off + size) break;
    uint32_t dataSize = (in[p] << 24) | (in[p + 1] << 16) | (in[p + 2] << 8) | in[p + 3];
    p += 4;
    if (ours && p + dataSize <= off + size) {
      note += L"pssh(PlayReady," + std::to_wstring(dataSize) + L"B) ";
      return std::vector<uint8_t>(in.begin() + p, in.begin() + p + dataSize);
    }
    note += ours ? L"pssh(PlayReady,malformed) " : L"pssh(other) ";
    off += size;
  }
  if (off == 0) note += L"raw ";
  return in;
}

// MSPR_E_NEEDS_INDIVIDUALIZATION: this phone has never done PlayReady. One
// network round trip to Microsoft, once per device, and then every PlayReady
// call works. On the Elite X3 even asking the security version threw it.
bool IndividualizeIfNeeded(hresult_error const& e, JsonObject& o) {
  if (static_cast<uint32_t>(e.code()) != 0x8004B822u) {
    return false;
  }
  try {
    Log::Write(L"drm: the phone needs individualization -- doing it now");
    PlayReadyIndividualizationServiceRequest indiv;
    indiv.BeginServiceRequest().get();
    Log::Write(L"drm: individualized");
    o.SetNamedValue(L"individualized", JsonValue::CreateBooleanValue(true));
    return true;
  } catch (hresult_error const& e2) {
    Log::Write(L"drm: individualization FAILED: " + std::wstring(e2.message()) +
               L" (" + std::to_wstring(static_cast<uint32_t>(e2.code())) + L")");
    return false;
  }
}

void HandleInfo(double id) {
  JsonObject o;
  o.SetNamedValue(L"id", JsonValue::CreateNumberValue(id));
  for (int attempt = 0; attempt < 2; ++attempt) {
  try {
    o.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(true));
    o.SetNamedValue(L"securityVersion",
                    JsonValue::CreateNumberValue(PlayReadyStatics::PlayReadySecurityVersion()));
    o.SetNamedValue(L"hardwareSupported",
                    JsonValue::CreateBooleanValue(PlayReadyStatics::CheckSupportedHardware(
                        PlayReadyHardwareDRMFeatures::HardwareDRM)));
    Log::Write(L"drm: PlayReady security version " +
               std::to_wstring(PlayReadyStatics::PlayReadySecurityVersion()));
    break;
  } catch (hresult_error const& e) {
    if (attempt == 0 && IndividualizeIfNeeded(e, o)) {
      continue;
    }
    o = Failure(id, L"PlayReady is not available: " + std::wstring(e.message()), e.code());
    break;
  }
  }
  Send(o);
}

void HandleChallenge(double id, JsonObject const& msg) {
  std::wstring session(msg.GetNamedString(L"session", L""));
  std::wstring note;
  try {
    std::vector<uint8_t> initData = FromBase64(msg.GetNamedString(L"initData"));
    std::vector<uint8_t> header = UnwrapPssh(initData, note);
    Log::Write(L"drm: challenge for session " + session + L": initData " +
               std::to_wstring(initData.size()) + L" bytes, " + note);

    PlayReadyLicenseAcquisitionServiceRequest request;
    request.ContentHeader(PlayReadyContentHeader(
        array_view<uint8_t const>(header.data(), header.data() + header.size())));

    PlayReadySoapMessage soap{nullptr};
    try {
      soap = request.GenerateManualEnablingChallenge();
    } catch (hresult_error const& e) {
      JsonObject scratch;
      if (IndividualizeIfNeeded(e, scratch)) {
        soap = request.GenerateManualEnablingChallenge();
      } else {
        throw;
      }
    }
    com_array<uint8_t> body = soap.GetMessageBody();
    JsonObject headers;
    for (auto const& kv : soap.MessageHeaders()) {
      headers.SetNamedValue(kv.Key(),
                            JsonValue::CreateStringValue(unbox_value_or<hstring>(kv.Value(), L"")));
    }
    {
      std::lock_guard<std::mutex> lock(gLock);
      gRequests[session] = request;
    }
    JsonObject o;
    o.SetNamedValue(L"id", JsonValue::CreateNumberValue(id));
    o.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(true));
    o.SetNamedValue(L"challenge", JsonValue::CreateStringValue(
                                       ToBase64(array_view<uint8_t const>(body.begin(), body.end()))));
    o.SetNamedValue(L"uri", JsonValue::CreateStringValue(soap.Uri() ? soap.Uri().AbsoluteUri() : L""));
    o.SetNamedValue(L"headers", headers);
    Log::Write(L"drm: challenge is " + std::to_wstring(body.size()) +
               L" bytes; PlayReady supplied a license URI");
    Send(o);
  } catch (hresult_error const& e) {
    Send(Failure(id, L"challenge failed after [" + note + L"]: " + std::wstring(e.message()), e.code()));
  }
}

void HandleResponse(double id, JsonObject const& msg) {
  std::wstring session(msg.GetNamedString(L"session", L""));
  try {
    PlayReadyLicenseAcquisitionServiceRequest request{nullptr};
    {
      std::lock_guard<std::mutex> lock(gLock);
      auto it = gRequests.find(session);
      if (it == gRequests.end()) {
        Send(Failure(id, L"no request for session " + session));
        return;
      }
      request = it->second;
    }
    std::vector<uint8_t> license = FromBase64(msg.GetNamedString(L"license"));
    Log::Write(L"drm: license response for session " + session + L": " +
               std::to_wstring(license.size()) + L" bytes");
    request.ProcessManualEnablingResponse(
        array_view<uint8_t const>(license.data(), license.data() + license.size()));
    JsonObject o;
    o.SetNamedValue(L"id", JsonValue::CreateNumberValue(id));
    o.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(true));
    Log::Write(L"drm: LICENSE ACCEPTED for session " + session);
    Send(o);
  } catch (hresult_error const& e) {
    Send(Failure(id, L"license rejected: " + std::wstring(e.message()), e.code()));
  }
}

void Handle(std::string json) {
  try {
    init_apartment(apartment_type::multi_threaded);
  } catch (...) {
  }
  double id = 0;
  try {
    JsonObject msg = JsonObject::Parse(Widen(json.c_str()));
    id = msg.GetNamedNumber(L"id", 0);
    std::wstring op(msg.GetNamedString(L"op", L""));
    if (op == L"drm.info") {
      HandleInfo(id);
    } else if (op == L"drm.challenge") {
      HandleChallenge(id, msg);
    } else if (op == L"drm.response") {
      HandleResponse(id, msg);
    } else {
      Send(Failure(id, L"unknown op " + op));
    }
  } catch (hresult_error const& e) {
    Send(Failure(id, L"bad message: " + std::wstring(e.message()), e.code()));
  } catch (...) {
    Send(Failure(id, L"bad message"));
  }
}

}  // namespace

void DrmBridge::SetReply(void (*reply)(const char*)) { gReply = reply; }

void DrmBridge::OnMessage(const char* json) {
  if (!json) return;
  std::thread(Handle, std::string(json)).detach();
}

}  // namespace gecko_w10m::client
