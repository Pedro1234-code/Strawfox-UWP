/* gecko_capi.h
 *
 * Flat C ABI exported by the patched Gecko engine static/dynamic library
 * (built by tools/build-gecko-uwp.ps1). The C++/WinRT shell links against this;
 * it is the W10M analogue of the Swift `GeckoView` layer on iOS.
 *
 * Keeping the boundary a flat C ABI means the SpiderMonkey/Gecko side can stay
 * pure C++ with no WinRT dependency, and the shell can be rebuilt without
 * relinking the engine.
 */
#ifndef GECKO_W10M_GECKO_CAPI_H
#define GECKO_W10M_GECKO_CAPI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gecko_runtime gecko_runtime;
typedef struct gecko_session gecko_session;

/* ---- Runtime --------------------------------------------------------------
 * One runtime per process. `profile_dir` is a real filesystem path made
 * inside the app's LocalState. Single-process: the
 * runtime does NOT spawn children. */
typedef struct {
  const char* profile_dir;   /* UTF-8 */
  int32_t     jit_enabled;   /* 1 -> Baseline+Ion+Wasm (requires codeGeneration) */
  int32_t     device_dpi;
} gecko_runtime_config;

gecko_runtime* gecko_runtime_create(const gecko_runtime_config* config);
void           gecko_runtime_shutdown(gecko_runtime* rt);

/* Pump the Gecko event loop from the UWP UI thread (CoreDispatcher tick).
 * Single-process build has no dedicated main-thread process, so the shell
 * drives it. Returns the number of events processed. */
int32_t        gecko_runtime_pump(gecko_runtime* rt);

/* ---- Session (≈ a tab) ---------------------------------------------------- */
gecko_session* gecko_session_create(gecko_runtime* rt, int32_t is_private);
void           gecko_session_close(gecko_session* s);

void gecko_session_load_uri(gecko_session* s, const char* uri);
void gecko_session_reload(gecko_session* s);
void gecko_session_stop(gecko_session* s);
void gecko_session_go_back(gecko_session* s);
void gecko_session_go_forward(gecko_session* s);
int32_t gecko_session_can_go_back(gecko_session* s);
int32_t gecko_session_can_go_forward(gecko_session* s);

/* Bind the session to a Direct3D swapchain surface owned by a XAML
 * SwapChainPanel. Gecko renders compositor output here. */
void gecko_session_set_surface(gecko_session* s, void* idxgi_swapchain,
                               int32_t width, int32_t height, float scale);
void gecko_session_resize(gecko_session* s, int32_t width, int32_t height,
                          float scale);

/* Input forwarding from the UWP shell. */
void gecko_session_touch(gecko_session* s, int32_t phase, int32_t pointer_id,
                         float x, float y);
void gecko_session_key(gecko_session* s, int32_t down, int32_t vkey,
                       uint32_t modifiers);
void gecko_session_scroll(gecko_session* s, float dx, float dy);

/* ---- Delegates (engine -> shell callbacks) -------------------------------- */
typedef struct {
  void* user_data;
  void (*on_location_change)(void* user_data, const char* uri);
  void (*on_title_change)(void* user_data, const char* title);
  void (*on_progress)(void* user_data, float progress /* 0..1 */);
  void (*on_can_go_back)(void* user_data, int32_t can);
  void (*on_can_go_forward)(void* user_data, int32_t can);
  void (*on_new_session)(void* user_data, gecko_session* opened);
} gecko_session_delegate;

void gecko_session_set_delegate(gecko_session* s,
                                const gecko_session_delegate* d);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* GECKO_W10M_GECKO_CAPI_H */
