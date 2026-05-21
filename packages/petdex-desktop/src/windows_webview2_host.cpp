#define NOMINMAX
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kDefaultX = 80;
constexpr int kDefaultY = 120;
constexpr int kCompactW = 140;
constexpr int kCompactH = 180;
constexpr UINT kNativePetMenuBase = 1000;
constexpr UINT kNativePetMenuQuit = 9000;

struct PetMenuItem {
  std::wstring slug;
};

struct Host {
  HWND hwnd = nullptr;
  HMONITOR monitor = nullptr;
  int width = kCompactW;
  int height = kCompactH;
  std::wstring asset_root;
  std::wstring config_dir;
  std::wstring user_data_dir;
  ComPtr<ICoreWebView2Environment> environment;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
};

void switchActivePet(Host* host, const std::wstring& slug);

template <typename Interface>
class ComHandler : public Interface {
 public:
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == iid_) {
      *object = static_cast<Interface*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

 protected:
  explicit ComHandler(REFIID iid) : iid_(iid) {}
  virtual ~ComHandler() = default;

 private:
  const IID& iid_;
  volatile LONG refs_ = 1;
};

std::wstring widen(const wchar_t* value) {
  return value ? std::wstring(value) : std::wstring();
}

std::wstring fileUriFor(const std::wstring& path) {
  std::filesystem::path absolute = std::filesystem::absolute(path);
  std::wstring uri = L"file:///";
  for (wchar_t ch : absolute.native()) uri.push_back(ch == L'\\' ? L'/' : ch);
  return uri;
}

bool contains(const std::wstring& haystack, const wchar_t* needle) {
  return haystack.find(needle) != std::wstring::npos;
}

double numberAfter(const std::wstring& json, const wchar_t* key, double fallback) {
  const std::wstring needle = std::wstring(L"\"") + key + L"\":";
  const size_t start = json.find(needle);
  if (start == std::wstring::npos) return fallback;
  const wchar_t* begin = json.c_str() + start + needle.size();
  wchar_t* end = nullptr;
  const double value = wcstod(begin, &end);
  return end == begin ? fallback : value;
}

std::wstring stringAfter(const std::wstring& json, const wchar_t* key) {
  const std::wstring needle = std::wstring(L"\"") + key + L"\":\"";
  const size_t start = json.find(needle);
  if (start == std::wstring::npos) return L"";
  const size_t value_start = start + needle.size();
  const size_t value_end = json.find(L"\"", value_start);
  if (value_end == std::wstring::npos) return L"";
  return json.substr(value_start, value_end - value_start);
}

std::vector<PetMenuItem> petItemsFromJson(const std::wstring& json) {
  std::vector<PetMenuItem> items;
  size_t pos = 0;
  while (true) {
    const size_t slug_key = json.find(L"\"slug\":\"", pos);
    if (slug_key == std::wstring::npos) break;
    const size_t value_start = slug_key + 8;
    const size_t value_end = json.find(L"\"", value_start);
    if (value_end == std::wstring::npos) break;
    const std::wstring slug = json.substr(value_start, value_end - value_start);
    if (!slug.empty()) items.push_back({slug});
    pos = value_end + 1;
  }
  return items;
}

void navigateToIndex(Host* host) {
  if (!host || !host->webview) return;
  const auto path = (std::filesystem::path(host->asset_root) / L"index.html").native();
  host->webview->Navigate(fileUriFor(path).c_str());
}

double dpiScaleForWindow(HWND hwnd) {
  const UINT dpi = GetDpiForWindow(hwnd);
  return dpi == 0 ? 1.0 : static_cast<double>(dpi) / 96.0;
}

int dipToPhysical(HWND hwnd, int value) {
  return static_cast<int>(value * dpiScaleForWindow(hwnd) + 0.5);
}

void applyTransparentBackground(Host* host) {
  if (!host || !host->controller) return;
  ComPtr<ICoreWebView2Controller2> controller2;
  if (SUCCEEDED(host->controller->QueryInterface(IID_ICoreWebView2Controller2, reinterpret_cast<void**>(controller2.GetAddressOf())))) {
    COREWEBVIEW2_COLOR transparent{0, 0, 0, 0};
    controller2->put_DefaultBackgroundColor(transparent);
  }
}

void applyDpiSettings(Host* host) {
  if (!host || !host->controller || !host->hwnd) return;
  ComPtr<ICoreWebView2Controller3> controller3;
  if (SUCCEEDED(host->controller->QueryInterface(IID_ICoreWebView2Controller3, reinterpret_cast<void**>(controller3.GetAddressOf())))) {
    controller3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RASTERIZATION_SCALE);
    controller3->put_RasterizationScale(dpiScaleForWindow(host->hwnd));
    controller3->put_ShouldDetectMonitorScaleChanges(TRUE);
  }
}

void applyWebViewBounds(Host* host) {
  if (!host || !host->controller) return;
  applyDpiSettings(host);
  RECT bounds{0, 0, host->width, host->height};
  host->controller->put_Bounds(bounds);
  applyTransparentBackground(host);
}

void syncWindowForDpi(Host* host) {
  if (!host || !host->hwnd) return;
  SetWindowPos(
      host->hwnd,
      HWND_TOPMOST,
      0,
      0,
      dipToPhysical(host->hwnd, host->width),
      dipToPhysical(host->hwnd, host->height),
      SWP_NOMOVE | SWP_NOACTIVATE);
  applyWebViewBounds(host);
}

void openNativeMenu(Host* host, const std::wstring& json) {
  if (!host || !host->hwnd) return;
  const auto pets = petItemsFromJson(json);
  const std::wstring active = stringAfter(json, L"active");
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  UINT id = kNativePetMenuBase;
  for (const auto& pet : pets) {
    UINT flags = MF_STRING;
    if (pet.slug == active) flags |= MF_CHECKED;
    AppendMenuW(menu, flags, id++, pet.slug.c_str());
  }
  if (!pets.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kNativePetMenuQuit, L"Quit");
  POINT cursor{kDefaultX, kDefaultY};
  GetCursorPos(&cursor);
  SetForegroundWindow(host->hwnd);
  const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x, cursor.y, 0, host->hwnd, nullptr);
  DestroyMenu(menu);
  if (selected == kNativePetMenuQuit) DestroyWindow(host->hwnd);
  else if (selected >= kNativePetMenuBase && selected < kNativePetMenuBase + pets.size()) switchActivePet(host, pets[selected - kNativePetMenuBase].slug);
}

void moveHostWindow(Host* host, double dx, double dy) {
  if (!host || !host->hwnd) return;
  RECT rect{};
  if (!GetWindowRect(host->hwnd, &rect)) return;
  const double scale = dpiScaleForWindow(host->hwnd);
  SetWindowPos(
      host->hwnd,
      HWND_TOPMOST,
      rect.left + static_cast<int>(dx * scale + (dx >= 0 ? 0.5 : -0.5)),
      rect.top + static_cast<int>(dy * scale + (dy >= 0 ? 0.5 : -0.5)),
      0,
      0,
      SWP_NOSIZE | SWP_NOACTIVATE);
  host->monitor = MonitorFromWindow(host->hwnd, MONITOR_DEFAULTTONEAREST);
  syncWindowForDpi(host);
}

void switchActivePet(Host* host, const std::wstring& slug) {
  if (!host || slug.empty()) return;
  const auto root = std::filesystem::path(host->asset_root);
  const auto pet_dir = root / slug;
  const auto source_webp = pet_dir / L"spritesheet.webp";
  const auto source_png = pet_dir / L"spritesheet.png";
  const auto source = std::filesystem::exists(source_webp) ? source_webp : source_png;
  if (!std::filesystem::exists(source)) return;
  std::error_code ec;
  std::filesystem::copy_file(source, root / L"spritesheet.webp", std::filesystem::copy_options::overwrite_existing, ec);
  std::filesystem::create_directories(std::filesystem::path(host->config_dir), ec);
  std::wofstream active(std::filesystem::path(host->config_dir) / L"active.json", std::ios::trunc);
  if (active) active << L"{\"slug\":\"" << slug << L"\"}\n";
  navigateToIndex(host);
}

void handleMessage(Host* host, const std::wstring& json) {
  if (contains(json, L"petdex.quit")) {
    DestroyWindow(host->hwnd);
    return;
  }
  if (contains(json, L"zero-native.window.move")) {
    moveHostWindow(host, numberAfter(json, L"dx", 0), numberAfter(json, L"dy", 0));
    return;
  }
  if (contains(json, L"zero-native.window.resize")) {
    host->width = static_cast<int>(numberAfter(json, L"width", host->width));
    host->height = static_cast<int>(numberAfter(json, L"height", host->height));
    syncWindowForDpi(host);
    return;
  }
  if (contains(json, L"petdex.set_active")) {
    switchActivePet(host, stringAfter(json, L"slug"));
    return;
  }
  if (contains(json, L"petdex.open_native_menu")) {
    openNativeMenu(host, json);
    return;
  }
}

const wchar_t* bridgeScript() {
  return LR"JS(
(() => {
  if (window.zero && window.zero.invoke) return;
  function post(command, payload) {
    try {
      if (window.chrome && window.chrome.webview) window.chrome.webview.postMessage({ command, payload: payload || {} });
    } catch (_) {}
  }
  window.zero = {
    invoke(command, payload = {}) {
      if (command === 'petdex.read_runtime_state') return Promise.resolve({ state: 'idle', counter: 0 });
      if (command === 'petdex.read_runtime_bubble') return Promise.resolve({ text: '', counter: 0 });
      if (command === 'petdex.read_update_info') return Promise.resolve({ available: false, status: 'idle' });
      if (command === 'petdex.read_init_status') return Promise.resolve({ needsInit: false, reason: null, checkedAt: 0 });
      if (command === 'petdex.read_incoming_url') return Promise.resolve({ slug: '' });
      if (command === 'petdex.open_native_menu') { post(command, payload); return Promise.resolve({ native: true }); }
      post(command, payload);
      if (command === 'zero-native.window.move') return Promise.resolve({ hitX: false, hitY: false });
      return Promise.resolve({ ok: true });
    }
  };
})();
)JS";
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Host* host = reinterpret_cast<Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      host = reinterpret_cast<Host*>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
      return TRUE;
    }
    case WM_SIZE:
      if (host && host->controller) applyWebViewBounds(host);
      return 0;
    case WM_DPICHANGED:
      if (host) {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        const UINT new_dpi = HIWORD(wparam);
        if (suggested) {
          SetWindowPos(hwnd, HWND_TOPMOST, suggested->left, suggested->top, MulDiv(host->width, new_dpi, 96), MulDiv(host->height, new_dpi, 96), SWP_NOACTIVATE);
        }
        host->monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        syncWindowForDpi(host);
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

bool registerClass(HINSTANCE hinst) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wndProc;
  wc.hInstance = hinst;
  wc.lpszClassName = L"PetdexWebView2Window";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

class WebMessageReceivedHandler final : public ComHandler<ICoreWebView2WebMessageReceivedEventHandler> {
 public:
  explicit WebMessageReceivedHandler(Host* host) : ComHandler(IID_ICoreWebView2WebMessageReceivedEventHandler), host_(host) {}
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) override {
    LPWSTR raw = nullptr;
    if (args && SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
      handleMessage(host_, raw);
      CoTaskMemFree(raw);
    }
    return S_OK;
  }

 private:
  Host* host_;
};

class ControllerCompletedHandler final : public ComHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
 public:
  explicit ControllerCompletedHandler(Host* host) : ComHandler(IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler), host_(host) {}
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
    if (FAILED(result) || !controller) return result;
    host_->controller = controller;
    host_->controller->get_CoreWebView2(&host_->webview);
    applyWebViewBounds(host_);
    host_->webview->AddScriptToExecuteOnDocumentCreated(bridgeScript(), nullptr);
    auto* message_handler = new WebMessageReceivedHandler(host_);
    EventRegistrationToken token{};
    host_->webview->add_WebMessageReceived(message_handler, &token);
    message_handler->Release();
    navigateToIndex(host_);
    return S_OK;
  }

 private:
  Host* host_;
};

class EnvironmentCompletedHandler final : public ComHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
 public:
  explicit EnvironmentCompletedHandler(Host* host) : ComHandler(IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler), host_(host) {}
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
    if (FAILED(result) || !env) return result;
    host_->environment = env;
    auto* controller_handler = new ControllerCompletedHandler(host_);
    const HRESULT hr = env->CreateCoreWebView2Controller(host_->hwnd, controller_handler);
    controller_handler->Release();
    return hr;
  }

 private:
  Host* host_;
};

}  // namespace

extern "C" Host* petdex_webview2_create(const wchar_t* asset_root, const wchar_t* config_dir, int width, int height) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  auto* host = new Host();
  host->asset_root = widen(asset_root);
  host->config_dir = widen(config_dir);
  host->width = width;
  host->height = height;
  HINSTANCE hinst = GetModuleHandleW(nullptr);
  if (!registerClass(hinst)) {
    delete host;
    return nullptr;
  }
  const UINT dpi = GetDpiForSystem();
  host->hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
      L"PetdexWebView2Window",
      L"Petdex",
      WS_POPUP,
      kDefaultX,
      kDefaultY,
      MulDiv(width, dpi, 96),
      MulDiv(height, dpi, 96),
      nullptr,
      nullptr,
      hinst,
      host);
  if (!host->hwnd) {
    delete host;
    return nullptr;
  }
  host->monitor = MonitorFromWindow(host->hwnd, MONITOR_DEFAULTTONEAREST);
  SetLayeredWindowAttributes(host->hwnd, 0, 255, LWA_ALPHA);
  return host;
}

extern "C" void petdex_webview2_destroy(Host* host) {
  if (!host) return;
  if (host->controller) host->controller->Close();
  if (host->hwnd) DestroyWindow(host->hwnd);
  delete host;
}

extern "C" int petdex_webview2_run(Host* host) {
  if (!host || !host->hwnd) return 1;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 2;
  ShowWindow(host->hwnd, SW_SHOWNOACTIVATE);
  UpdateWindow(host->hwnd);
  std::error_code ec;
  const auto user_data_path = std::filesystem::path(host->config_dir) / L"webview2";
  std::filesystem::create_directories(user_data_path, ec);
  host->user_data_dir = user_data_path.native();
  auto* environment_handler = new EnvironmentCompletedHandler(host);
  hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, host->user_data_dir.c_str(), nullptr, environment_handler);
  environment_handler->Release();
  if (FAILED(hr)) {
    CoUninitialize();
    return 3;
  }
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  CoUninitialize();
  return 0;
}
