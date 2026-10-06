// EngineView.h -- carries what Gecko painted to the screen, and what the
// phone did back to Gecko.
//
// There is no window for web content on Windows 10 Mobile: an app container
// gets no HWND it can hand to a foreign engine, so Gecko runs headless and its
// compositor paints into a buffer instead of a surface. This is the other end
// of that buffer, and the near end of touch and typing.
//
// The entry points come from the loaded xul.dll by name rather than by
// linking: the shell is built before the engine and has to keep working when
// the engine is not there at all.
#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <utility>
#include <vector>

#include "winrt/Windows.UI.Xaml.Controls.h"
#include "winrt/Windows.UI.Xaml.Media.Imaging.h"
#include "winrt/Windows.UI.Core.h"
#include "winrt/Windows.UI.ViewManagement.h"

namespace gecko_w10m::client {

class EngineView {
 public:
  // The window size in physical pixels and the number of them Windows puts in
  // a view pixel -- the same two numbers the engine was started with, because
  // the keyboard is measured in one and the window in the other.
  // withPanel false makes no SwapChainPanel at all: the one structural thing
  // this shell has that the builds which lived did not.
  EngineView(int32_t pixelWidth, int32_t pixelHeight, double rawPerView,
             bool withPanel, bool usePhysicalScreenRoom = false);

  winrt::Windows::UI::Xaml::Controls::Image Surface() const { return image_; }
  // What the engine presents to when it is drawing on the GPU. It sits under
  // the picture and stays empty while the software path is in use, so whichever
  // of the two produces a frame is the one that shows.
  winrt::Windows::UI::Xaml::Controls::SwapChainPanel Panel() const {
    return panel_;
  }
  // The panel under that one. A playing video is handed to it as a swap
  // chain of its own, which the display shows as a hardware overlay, and the
  // browser leaves a transparent hole over it. Collapsed until the engine
  // asks; goes into the tree directly below Panel().
  winrt::Windows::UI::Xaml::Controls::SwapChainPanel VideoPanel() const {
    return video_panel_;
  }
  // Hands the panel to the engine. Must happen before the engine starts, since
  // EGL asks for it as soon as it makes a surface.
  void GivePanelToEngine();
  // The display in view pixels, as the window sees it; sent on to the engine
  // in device pixels so the headless screen turns with the phone.
  void SetScreen(double viewWidth, double viewHeight);
  // The display's density. APZ measures how far a finger may wander and still
  // count as a tap against this; left to itself it assumes a 96 dpi monitor
  // and lets a tap move nine pixels, which on this screen is a fifth of a
  // millimetre -- so ordinary taps became tiny pans and never arrived.
  void SetDpi(double dpi);
  // Makes the room match the keyboard as it actually is. The pane's Hiding
  // event is not delivered when the keyboard goes away with the app -- a
  // suspend with it open, a focus change the shell never sees -- and the
  // room then stayed short, with the browser in the top half of the screen
  // and the placeholder showing under it.
  void SyncKeyboardMargin();
  // Hands a URL to the engine as soon as it can take one. Safe to call before
  // the engine has started.
  void OpenUrl(std::wstring_view url);
  // Handed to the engine so it can open mailto:, tel: and ms-settings: URIs
  // through the system. Static because the engine keeps a plain pointer.
  static void LaunchSystemUri(const char* utf8);
  // Handed to the engine the same way: it says when a page goes fullscreen,
  // and the shell takes the status bar and the navigation bar off the screen.
  // The engine never resizes itself -- the room this frees up comes back to it
  // through the ordinary resize, so the window and the swap chain change
  // shape together.
  static void FullscreenChanged(int32_t on);
  static int32_t PickFile(int32_t mode, const char* title,
                          const char* defaultName, const char* extensions,
                          char* result, int32_t resultCapacity);
  // The video layer, driven by the engine's compositor thread (see
  // gecko_w10m_set_video_layer_sink). All three post to the UI thread.
  static void VideoLayerAttach(void* surface);
  static void VideoLayerPlace(uint32_t generation, int32_t x, int32_t y,
                              int32_t width, int32_t height, int32_t clipX,
                              int32_t clipY, int32_t clipWidth,
                              int32_t clipHeight, int32_t chainWidth,
                              int32_t chainHeight, int32_t rotation);
  static void VideoLayerShow(int32_t visible);
  static void VideoLayerStack(int32_t above, int32_t trimTop,
                              int32_t trimBottom, int32_t trimLeft,
                              int32_t trimRight);
  // Which half of the swap-chain hand-over this launch leaves out, if any:
  // 0 nothing, 1 the panel never gets the chain, 2 the chain is never
  // presented. Passed on to ANGLE as soon as it can be reached.
  void SetExperimentMode(int mode) { experimentMode_ = mode; }
  // Whether the engine is given the panel at all. Without it ANGLE has no
  // window, no EGL surface is made, and WebRender cannot draw on the GPU --
  // while every other thing the engine does at first paint still happens.
  void SetPanelWithheld(bool withheld) { panelWithheld_ = withheld; }
  // Invisible, and focused only when Gecko says something takes text. It is
  // what the on-screen keyboard types into and the only way its keys can be
  // caught at all.
  winrt::Windows::UI::Xaml::Controls::TextBox TextSink() const {
    return sink_;
  }

  // Begins watching for frames. Cheap when the engine is idle: one lock and a
  // comparison per display refresh, no copy.
  // The element that holds the picture. Its size is the room the window has,
  // and it is watched for as long as the view lives.
  void WatchRoom(winrt::Windows::UI::Xaml::FrameworkElement const& host);

  void Start();
  void Stop();
  // Attach Xbox/desktop CoreWindow mouse input only after the app window has
  // completed activation. Calling more than once is harmless.
  void EnableMouse();

  // Called once, when the engine has drawn something. It is the only
  // evidence that a start succeeded, so it is what takes the splash down
  // and what clears the failed-attempt count.
  void OnFirstFrame(std::function<void()> handler) {
    firstFrame_ = std::move(handler);
  }

 private:
  void Tick();
  bool Resolve();
  void EnsureBitmap(int32_t width, int32_t height);

  // A drag scrolls and a tap clicks, which is what a phone means by touch. The
  // engine is told in the pixels of the frame it drew, so every point has to
  // come back through the letterbox the image is shown in.
  //
  // Every finger is its own touch point, named by its pointer id, so two of
  // them make a pinch. A finger that loses its capture is cancelled.
  void OnPressed(uint32_t id, winrt::Windows::Foundation::Point const& point,
                 bool mouse);
  void OnMoved(uint32_t id, winrt::Windows::Foundation::Point const& point,
               bool mouse);
  void OnReleased(uint32_t id, winrt::Windows::Foundation::Point const& point,
                  bool mouse);
  void OnCaptureLost(uint32_t id, bool mouse);
  void OnWheel(winrt::Windows::Foundation::Point const& point, int32_t delta);
  void OnMouseButton(winrt::Windows::Foundation::Point const& point,
                     int32_t button, bool pressed);
  void WireCoreMouse();
  // UWP exposes Xbox controllers through Windows.Gaming.Input rather than the
  // CoreWindow pointer stream. Polling here keeps the stick continuous and
  // converts it to the same wheel input used by a desktop mouse.
  void PollGamepadScroll();
  winrt::Windows::Foundation::Point CoreMousePoint(
      winrt::Windows::Foundation::Point const& point) const;
  bool ToFrame(winrt::Windows::Foundation::Point const& point, int32_t* x,
               int32_t* y) const;

  void FollowTextInput();
  // Tells the engine how much room the picture has, whenever that changes.
  void PushSize();
  // Runs work on a later turn of the UI loop, never inside the handler that
  // asked for it.
  static void PostToUi(std::function<void()> work);
  void WireKeyboard();

  using CopyFn = int32_t (*)(void* dest, int32_t capacity, int32_t* width,
                             int32_t* height, uint64_t* serial);
  using MouseFn = void (*)(int32_t message, int32_t x, int32_t y);
  // Versioned mouse entry point. The original bridge only carried the left
  // button, so keep it for old engines and use this one when it is available.
  // button: 0 left, 1 middle, 2 right.
  using MouseButtonFn = void (*)(int32_t message, int32_t button, int32_t x,
                                 int32_t y);
  using WheelFn = void (*)(int32_t x, int32_t y, double dx, double dy);
  using WantedFn = int32_t (*)();
  using TextStateFn = uint32_t (*)();
  using OverlayFn = int32_t (*)();
  using TextFn = void (*)(const uint16_t* text, int32_t length);
  using KeyFn = void (*)(int32_t keyCode);
  // Unlike the original entry point, this keeps physical keydown/keyup and
  // modifiers separate.  That is required for browser shortcuts and for
  // Escape to leave pointer lock.
  using Key2Fn = void (*)(int32_t down, int32_t keyCode, uint32_t modifiers,
                          int32_t repeat);
  using ResizeFn = void (*)(int32_t width, int32_t height);
  using TouchFn = void (*)(int32_t pointerId, int32_t state, int32_t x, int32_t y);
  using ScreenFn = void (*)(int32_t width, int32_t height);
  using DpiFn = void (*)(int32_t dpi);
  using OpenUrlFn = int32_t (*)(const char* url);
  using SetLauncherFn = void (*)(void (*)(const char*));
  using SetFullscreenSinkFn = void (*)(void (*)(int32_t));
  using SetFilePickerSinkFn = void (*)(int32_t (*)(
      int32_t, const char*, const char*, const char*, char*, int32_t));
  using SetBridgeSinkFn = int32_t (*)(void (*)(const char*));
  using BridgeReplyFn = void (*)(const char*);
  using PanelFn = void (*)(void* panel);
  using PanelPresentingFn = int32_t (*)();
  using PanelSizeFn = void (*)(int32_t width, int32_t height);
  using PanelScaleFn = void (*)(float x, float y);
  using AngleLogFn = void (*)(void (*)(const char*));
  using AngleModeFn = void (*)(int32_t);
  // Same layout as GeckoW10mVideoLayerSink in the engine.
  struct VideoLayerSink {
    void (*attach)(void* surface);
    void (*place)(uint32_t generation, int32_t x, int32_t y, int32_t width,
                  int32_t height, int32_t clipX, int32_t clipY,
                  int32_t clipWidth, int32_t clipHeight, int32_t chainWidth,
                  int32_t chainHeight, int32_t rotation);
    void (*show)(int32_t visible);
    void (*stack)(int32_t above, int32_t trimTop, int32_t trimBottom,
                  int32_t trimLeft, int32_t trimRight);
  };
  using SetVideoLayerSinkFn = void (*)(const VideoLayerSink*);

  CopyFn copy_ = nullptr;
  MouseFn mouse_ = nullptr;
  MouseButtonFn mouseButton_ = nullptr;
  WheelFn wheel_ = nullptr;
  WantedFn wanted_ = nullptr;
  // The focused field's kind and the engine's focus serial (see
  // gecko_w10m_text_input_state); null with an engine that has no such export.
  TextStateFn text_state_ = nullptr;
  OverlayFn overlay_ = nullptr;
  TextFn text_ = nullptr;
  KeyFn key_ = nullptr;
  Key2Fn key2_ = nullptr;
  ResizeFn resize_ = nullptr;
  TouchFn touch_ = nullptr;
  ScreenFn screen_fn_ = nullptr;
  DpiFn dpi_fn_ = nullptr;
  int32_t dpi_ = 0;
  OpenUrlFn open_url_ = nullptr;
  SetLauncherFn set_launcher_ = nullptr;
  SetFullscreenSinkFn set_fullscreen_ = nullptr;
  SetFilePickerSinkFn set_file_picker_ = nullptr;
  SetVideoLayerSinkFn set_video_layer_ = nullptr;
  // The chrome-to-shell message bridge (client/DrmBridge). Armed once the
  // engine's main thread is up, which is later than the exports resolve.
  SetBridgeSinkFn set_bridge_ = nullptr;
  BridgeReplyFn bridge_reply_ = nullptr;
  bool bridgeArmed_ = false;
  unsigned long long lastBridgeAttempt_ = 0;
  void ArmBridge();
  // A URL the phone handed us -- from a tap on a link in another app, or from
  // this being the browser it opens links with. Kept until the engine has a
  // window to put it in.
  std::string pendingUrl_;
  unsigned long long lastOpenAttempt_ = 0;
  // The fingers on the glass, and where the last touch move actually sent for
  // each was, so a finger resting on the glass stops producing them.
  struct Finger {
    uint32_t id;
    int32_t x;
    int32_t y;
    int32_t startX;
    int32_t startY;
  };
  std::vector<Finger> fingers_;
  Finger* FindFinger(uint32_t id);
  int32_t screenWidth_ = 0;
  int32_t screenHeight_ = 0;
  PanelFn panel_fn_ = nullptr;
  PanelPresentingFn panel_presenting_fn_ = nullptr;
  PanelSizeFn panel_size_fn_ = nullptr;
  PanelScaleFn panel_scale_fn_ = nullptr;

  winrt::Windows::UI::Xaml::Controls::Image image_{nullptr};
  winrt::Windows::UI::Xaml::Controls::SwapChainPanel panel_{nullptr};
  winrt::Windows::UI::Xaml::Controls::SwapChainPanel video_panel_{nullptr};
  winrt::Windows::UI::Xaml::FrameworkElement host_{nullptr};
  winrt::Windows::UI::Xaml::Controls::TextBox sink_{nullptr};
  winrt::Windows::UI::Xaml::Media::Imaging::WriteableBitmap bitmap_{nullptr};
  winrt::event_token tick_{};

  uint64_t seen_ = 0;
  int32_t width_ = 0;
  int32_t height_ = 0;
  bool reported_ = false;
  bool panelGiven_ = false;
  bool sizeSent_ = false;
  bool textInputPending_ = false;
  bool touched_ = false;
  bool declinedUntilTouch_ = false;
  int experimentMode_ = 0;
  bool panelWithheld_ = false;
  bool saidWaiting_ = false;
  unsigned long long lastResolveAttempt_ = 0;
  std::function<void()> firstFrame_;

  // Without the engine's touch entry point, one finger only -- the first --
  // scrolls with the wheel and taps with the mouse.
  bool pressed_ = false;
  uint32_t pressedId_ = 0;
  double lastX_ = 0;
  double lastY_ = 0;
  double travelled_ = 0;

  // Xbox exposes its mouse through CoreWindow's pointer stream. XAML element
  // events are retained for touch, while these global events provide the
  // desktop-style mouse stream used by the FactoryOS shell as well.
  winrt::Windows::UI::Core::CoreWindow coreWindow_{nullptr};
  winrt::event_token coreMouseMoved_{};
  winrt::event_token coreMousePressed_{};
  winrt::event_token coreMouseReleased_{};
  winrt::event_token coreMouseWheel_{};
  bool coreLeftDown_ = false;
  bool coreMiddleDown_ = false;
  bool coreRightDown_ = false;
  // The most recent point inside Gecko. Controller scrolling uses it so an
  // open menu is scrolled under the cursor; before there is one, use center.
  int32_t lastInputX_ = 0;
  int32_t lastInputY_ = 0;
  bool haveInputPoint_ = false;
  unsigned long long lastGamepadPoll_ = 0;
  bool gamepadApiUnavailable_ = false;

  bool typing_ = false;    // text input is wanted right now
  // The engine's focus serial at the last raise, the keyboard the sink was
  // last given, and when to check after a tap whether the keyboard should be
  // up again (0: no check pending).
  uint32_t lastTextSerial_ = 0;
  int32_t sinkKind_ = -1;
  unsigned long long tapKeyboardCheckAt_ = 0;
  bool forceKeyboard_ = false;
  void ApplyInputKind(int32_t kind);
  bool clearing_ = false;  // emptying the sink, so ignore its own change
  // KeyUp must follow the same path as its KeyDown even if Gecko changes text
  // focus in between the two events.
  std::array<bool, 256> forwardedKeys_{};
  int32_t fullWidth_ = 0;
  int32_t fullHeight_ = 0;
  double rawPerView_ = 1.0;
  bool usePhysicalScreenRoom_ = false;
};

}  // namespace gecko_w10m::client
