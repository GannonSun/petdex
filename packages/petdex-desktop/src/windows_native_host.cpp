#define NOMINMAX
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int kDefaultX = 80;
constexpr int kDefaultY = 120;
constexpr int kCols = 8;
constexpr int kRows = 9;
constexpr UINT_PTR kAnimTimer = 1;
constexpr UINT kPetMenuBase = 1000;
constexpr UINT kPetMenuQuit = 9000;
constexpr int kDragDirectionThresholdPx = 8;
constexpr DWORD kDragDirectionLockMs = 220;
constexpr int kIdleFrames[] = {0, 1, 2, 3, 4, 5};
constexpr UINT kIdleDurationsMs[] = {1680, 660, 660, 840, 840, 1920};

enum class PetAnimState {
  Idle,
  RunningRight,
  RunningLeft,
  Waving,
  Jumping,
  Failed,
  Waiting,
  Running,
  Review,
};

struct AnimationDef {
  int row = 0;
  int frame_count = 1;
  UINT frame_ms = 140;
  UINT last_frame_ms = 240;
  bool loop = true;
};

struct Image {
  UINT width = 0;
  UINT height = 0;
  std::vector<unsigned char> pixels;
};

struct Host {
  HWND hwnd = nullptr;
  HWND input_hwnd = nullptr;
  int width = 140;
  int height = 180;
  std::wstring asset_root;
  std::wstring config_dir;
  Image sheet;
  PetAnimState anim_state = PetAnimState::Idle;
  int frame_index = 0;
  bool dragging = false;
  POINT drag_offset{};
  POINT last_drag_cursor{};
  bool has_last_drag_cursor = false;
  int drag_dx_accum = 0;
  DWORD drag_direction_locked_until = 0;
};

std::wstring widen(const wchar_t* value) {
  return value ? std::wstring(value) : std::wstring();
}

double dpiScaleForWindow(HWND hwnd) {
  const UINT dpi = GetDpiForWindow(hwnd);
  return dpi == 0 ? 1.0 : static_cast<double>(dpi) / 96.0;
}

int dipToPhysical(HWND hwnd, int value) {
  return static_cast<int>(value * dpiScaleForWindow(hwnd) + 0.5);
}

std::filesystem::path desktopStatePath(const Host* host) {
  return std::filesystem::path(host->config_dir) / L"desktop-state.json";
}

bool parseJsonInt(const std::string& text, const char* key, int* out) {
  if (!key || !out) return false;
  const std::string quoted = std::string("\"") + key + "\"";
  const size_t key_pos = text.find(quoted);
  if (key_pos == std::string::npos) return false;
  const size_t colon = text.find(':', key_pos + quoted.size());
  if (colon == std::string::npos) return false;

  const char* start = text.c_str() + colon + 1;
  char* end = nullptr;
  const long value = std::strtol(start, &end, 10);
  if (end == start) return false;
  *out = static_cast<int>(value);
  return true;
}

std::wstring parseJsonString(const std::string& text, const char* key) {
  if (!key) return std::wstring();
  const std::string quoted = std::string("\"") + key + "\"";
  const size_t key_pos = text.find(quoted);
  if (key_pos == std::string::npos) return std::wstring();
  const size_t colon = text.find(':', key_pos + quoted.size());
  if (colon == std::string::npos) return std::wstring();
  const size_t open = text.find('"', colon + 1);
  if (open == std::string::npos) return std::wstring();
  const size_t close = text.find('"', open + 1);
  if (close == std::string::npos) return std::wstring();

  const std::string value = text.substr(open + 1, close - open - 1);
  if (value.empty()) return std::wstring();
  const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
  if (len <= 0) return std::wstring(value.begin(), value.end());
  std::wstring wide(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()), wide.data(), len);
  return wide;
}

struct MonitorSearch {
  RECT rect{};
  bool visible = false;
};

BOOL CALLBACK monitorIntersectsRect(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  auto* search = reinterpret_cast<MonitorSearch*>(data);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return TRUE;

  RECT intersection{};
  if (IntersectRect(&intersection, &search->rect, &info.rcWork)) {
    const int width = intersection.right - intersection.left;
    const int height = intersection.bottom - intersection.top;
    if (width >= 16 && height >= 16) {
      search->visible = true;
      return FALSE;
    }
  }
  return TRUE;
}

bool windowRectVisibleOnAnyMonitor(const RECT& rect) {
  MonitorSearch search{rect, false};
  EnumDisplayMonitors(nullptr, nullptr, monitorIntersectsRect, reinterpret_cast<LPARAM>(&search));
  return search.visible;
}

POINT fallbackPosition(int width_px, int height_px) {
  RECT work{};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
    return POINT{kDefaultX, kDefaultY};
  }

  constexpr int margin = 48;
  const int x = std::max(work.left + margin, work.right - width_px - margin);
  const int y = std::max(work.top + margin, work.bottom - height_px - margin);
  return POINT{x, y};
}

POINT initialWindowPosition(const Host* host, int width_px, int height_px) {
  if (!host) return POINT{kDefaultX, kDefaultY};

  std::ifstream state(desktopStatePath(host));
  std::string text((std::istreambuf_iterator<char>(state)), std::istreambuf_iterator<char>());
  int x = 0;
  int y = 0;
  if (parseJsonInt(text, "x", &x) && parseJsonInt(text, "y", &y)) {
    RECT rect{x, y, x + width_px, y + height_px};
    if (windowRectVisibleOnAnyMonitor(rect)) return POINT{x, y};
  }

  return fallbackPosition(width_px, height_px);
}

void saveWindowPosition(Host* host) {
  if (!host || !host->hwnd || host->config_dir.empty()) return;

  RECT rect{};
  if (!GetWindowRect(host->hwnd, &rect)) return;

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(host->config_dir), ec);

  std::ofstream state(desktopStatePath(host), std::ios::trunc);
  if (!state) return;
  state << "{\n"
        << "  \"version\": 1,\n"
        << "  \"window\": {\n"
        << "    \"x\": " << rect.left << ",\n"
        << "    \"y\": " << rect.top << "\n"
        << "  }\n"
        << "}\n";
}

std::wstring readActiveSlug(const Host* host) {
  if (!host || host->config_dir.empty()) return std::wstring();
  std::ifstream active(std::filesystem::path(host->config_dir) / L"active.json");
  if (!active) return std::wstring();
  const std::string text((std::istreambuf_iterator<char>(active)), std::istreambuf_iterator<char>());
  return parseJsonString(text, "slug");
}

bool loadImageWic(const std::filesystem::path& path, Image* out) {
  if (!out || !std::filesystem::exists(path)) return false;

  IWICImagingFactory* factory = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* converter = nullptr;

  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
  if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
  if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
  if (SUCCEEDED(hr)) {
    hr = converter->Initialize(
        frame,
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
  }

  UINT width = 0;
  UINT height = 0;
  if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
  std::vector<unsigned char> pixels;
  if (SUCCEEDED(hr)) {
    pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
  }

  if (converter) converter->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (factory) factory->Release();

  if (FAILED(hr) || width == 0 || height == 0) return false;
  out->width = width;
  out->height = height;
  out->pixels = std::move(pixels);
  return true;
}

bool loadSpritesheet(Host* host) {
  if (!host) return false;
  const auto root = std::filesystem::path(host->asset_root);
  if (loadImageWic(root / L"spritesheet.webp", &host->sheet)) return true;
  return loadImageWic(root / L"spritesheet.png", &host->sheet);
}

void render(Host* host);

AnimationDef animationDef(PetAnimState state) {
  switch (state) {
    case PetAnimState::Idle:
      return AnimationDef{0, 6, 280, 320, true};
    case PetAnimState::RunningRight:
      return AnimationDef{1, 8, 120, 220, true};
    case PetAnimState::RunningLeft:
      return AnimationDef{2, 8, 120, 220, true};
    case PetAnimState::Waving:
      return AnimationDef{3, 4, 140, 280, false};
    case PetAnimState::Jumping:
      return AnimationDef{4, 5, 140, 280, false};
    case PetAnimState::Failed:
      return AnimationDef{5, 8, 140, 240, false};
    case PetAnimState::Waiting:
      return AnimationDef{6, 6, 150, 260, true};
    case PetAnimState::Running:
      return AnimationDef{7, 6, 120, 220, true};
    case PetAnimState::Review:
      return AnimationDef{8, 6, 150, 280, true};
  }
  return AnimationDef{};
}

UINT frameDurationMs(const Host* host) {
  if (!host) return 140;
  if (host->anim_state == PetAnimState::Idle) {
    const int idle_index = std::clamp(host->frame_index, 0, static_cast<int>(sizeof(kIdleDurationsMs) / sizeof(kIdleDurationsMs[0])) - 1);
    return kIdleDurationsMs[idle_index];
  }

  const AnimationDef def = animationDef(host->anim_state);
  return host->frame_index >= def.frame_count - 1 ? def.last_frame_ms : def.frame_ms;
}

void scheduleNextFrame(Host* host) {
  if (!host || !host->hwnd) return;
  SetTimer(host->hwnd, kAnimTimer, frameDurationMs(host), nullptr);
}

void setAnimation(Host* host, PetAnimState state, bool restart) {
  if (!host) return;
  if (!restart && host->anim_state == state) return;
  host->anim_state = state;
  host->frame_index = 0;
  render(host);
  scheduleNextFrame(host);
}

void drawFrameBgra(
    unsigned char* dst,
    int dst_w,
    int dst_h,
    int dst_x,
    int dst_y,
    int draw_w,
    int draw_h,
    const Image& sheet,
    int frame_x,
    int frame_y,
    int frame_w,
    int frame_h) {
  if (!dst || draw_w <= 0 || draw_h <= 0 || frame_w <= 0 || frame_h <= 0) return;

  for (int y = 0; y < draw_h; ++y) {
    const int out_y = dst_y + y;
    if (out_y < 0 || out_y >= dst_h) continue;
    const int src_y = frame_y + std::clamp((y * frame_h) / draw_h, 0, frame_h - 1);

    for (int x = 0; x < draw_w; ++x) {
      const int out_x = dst_x + x;
      if (out_x < 0 || out_x >= dst_w) continue;
      const int src_x = frame_x + std::clamp((x * frame_w) / draw_w, 0, frame_w - 1);

      const size_t src_index = (static_cast<size_t>(src_y) * sheet.width + src_x) * 4;
      const size_t dst_index = (static_cast<size_t>(out_y) * dst_w + out_x) * 4;
      dst[dst_index + 0] = sheet.pixels[src_index + 0];
      dst[dst_index + 1] = sheet.pixels[src_index + 1];
      dst[dst_index + 2] = sheet.pixels[src_index + 2];
      dst[dst_index + 3] = sheet.pixels[src_index + 3];
    }
  }
}

void render(Host* host) {
  if (!host || !host->hwnd || host->sheet.pixels.empty()) return;

  const int physical_w = dipToPhysical(host->hwnd, host->width);
  const int physical_h = dipToPhysical(host->hwnd, host->height);
  const double scale = dpiScaleForWindow(host->hwnd);
  const int pet_w = static_cast<int>(96 * scale + 0.5);
  const int pet_h = static_cast<int>(104 * scale + 0.5);
  const int pet_x = static_cast<int>(8 * scale + 0.5);
  const int pet_y = static_cast<int>(34 * scale + 0.5);

  BITMAPINFO dst_info{};
  dst_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  dst_info.bmiHeader.biWidth = physical_w;
  dst_info.bmiHeader.biHeight = -physical_h;
  dst_info.bmiHeader.biPlanes = 1;
  dst_info.bmiHeader.biBitCount = 32;
  dst_info.bmiHeader.biCompression = BI_RGB;

  void* dst_bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC mem_dc = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(screen, &dst_info, DIB_RGB_COLORS, &dst_bits, nullptr, 0);
  HGDIOBJ old = SelectObject(mem_dc, dib);
  std::fill_n(static_cast<unsigned char*>(dst_bits), static_cast<size_t>(physical_w) * physical_h * 4, 0);

  const int frame_w = static_cast<int>(host->sheet.width / kCols);
  const int frame_h = static_cast<int>(host->sheet.height / kRows);
  const AnimationDef anim = animationDef(host->anim_state);
  const int row = std::clamp(anim.row, 0, kRows - 1);
  const int col = host->anim_state == PetAnimState::Idle
      ? std::clamp(kIdleFrames[std::clamp(host->frame_index, 0, static_cast<int>(sizeof(kIdleFrames) / sizeof(kIdleFrames[0])) - 1)], 0, kCols - 1)
      : std::clamp(host->frame_index, 0, kCols - 1);
  const int frame_x = col * frame_w;
  const int frame_y = row * frame_h;
  drawFrameBgra(
      static_cast<unsigned char*>(dst_bits),
      physical_w,
      physical_h,
      pet_x,
      pet_y,
      pet_w,
      pet_h,
      host->sheet,
      frame_x,
      frame_y,
      frame_w,
      frame_h);

  POINT dst_pos{};
  RECT rect{};
  GetWindowRect(host->hwnd, &rect);
  dst_pos.x = rect.left;
  dst_pos.y = rect.top;
  SIZE size{physical_w, physical_h};
  POINT src_pos{0, 0};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(host->hwnd, screen, &dst_pos, &size, mem_dc, &src_pos, 0, &blend, ULW_ALPHA);

  SelectObject(mem_dc, old);
  DeleteObject(dib);
  DeleteDC(mem_dc);
  ReleaseDC(nullptr, screen);
}

void syncInputWindow(Host* host) {
  if (!host || !host->hwnd || !host->input_hwnd) return;
  RECT rect{};
  GetWindowRect(host->hwnd, &rect);
  SetWindowPos(
      host->input_hwnd,
      HWND_TOPMOST,
      rect.left,
      rect.top,
      rect.right - rect.left,
      rect.bottom - rect.top,
      SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void advanceAnimation(Host* host) {
  if (!host) return;
  const AnimationDef def = animationDef(host->anim_state);
  if (host->frame_index + 1 >= def.frame_count) {
    if (def.loop) {
      host->frame_index = 0;
    } else {
      setAnimation(host, PetAnimState::Idle, true);
      return;
    }
  } else {
    host->frame_index += 1;
  }
  render(host);
  scheduleNextFrame(host);
}

void beginDrag(Host* host, HWND capture_hwnd) {
  if (!host || !host->hwnd) return;
  POINT cursor{};
  RECT rect{};
  GetCursorPos(&cursor);
  GetWindowRect(host->hwnd, &rect);
  host->dragging = true;
  host->drag_offset.x = cursor.x - rect.left;
  host->drag_offset.y = cursor.y - rect.top;
  host->last_drag_cursor = cursor;
  host->has_last_drag_cursor = true;
  host->drag_dx_accum = 0;
  host->drag_direction_locked_until = 0;
  SetCapture(capture_hwnd);
  setAnimation(host, PetAnimState::Running, true);
}

void updateDragAnimation(Host* host, const POINT& cursor) {
  if (!host) return;
  if (!host->has_last_drag_cursor) {
    host->last_drag_cursor = cursor;
    host->has_last_drag_cursor = true;
    return;
  }

  const int dx = cursor.x - host->last_drag_cursor.x;
  host->last_drag_cursor = cursor;
  host->drag_dx_accum += dx;

  const DWORD now = GetTickCount();
  if (now < host->drag_direction_locked_until) return;

  if (host->drag_dx_accum >= kDragDirectionThresholdPx) {
    setAnimation(host, PetAnimState::RunningRight, false);
    host->drag_dx_accum = 0;
    host->drag_direction_locked_until = now + kDragDirectionLockMs;
  } else if (host->drag_dx_accum <= -kDragDirectionThresholdPx) {
    setAnimation(host, PetAnimState::RunningLeft, false);
    host->drag_dx_accum = 0;
    host->drag_direction_locked_until = now + kDragDirectionLockMs;
  }
}

void finishDrag(Host* host) {
  if (!host || !host->dragging) return;
  saveWindowPosition(host);
  host->dragging = false;
  host->has_last_drag_cursor = false;
  host->drag_dx_accum = 0;
  host->drag_direction_locked_until = 0;
  setAnimation(host, PetAnimState::Waving, true);
}

std::vector<std::wstring> petSlugs(const std::wstring& asset_root) {
  std::vector<std::wstring> slugs;
  for (const auto& entry : std::filesystem::directory_iterator(asset_root)) {
    if (!entry.is_directory()) continue;
    const auto slug = entry.path().filename().wstring();
    if (std::filesystem::exists(entry.path() / L"spritesheet.webp") || std::filesystem::exists(entry.path() / L"spritesheet.png")) {
      slugs.push_back(slug);
    }
  }
  std::sort(slugs.begin(), slugs.end());
  return slugs;
}

void activatePet(Host* host, const std::wstring& slug) {
  if (!host || slug.empty()) return;
  const auto root = std::filesystem::path(host->asset_root);
  const auto pet_dir = root / slug;
  const auto src_webp = pet_dir / L"spritesheet.webp";
  const auto src_png = pet_dir / L"spritesheet.png";
  const auto source = std::filesystem::exists(src_webp) ? src_webp : src_png;
  if (!std::filesystem::exists(source)) return;

  std::error_code ec;
  std::filesystem::copy_file(source, root / L"spritesheet.webp", std::filesystem::copy_options::overwrite_existing, ec);
  std::filesystem::create_directories(std::filesystem::path(host->config_dir), ec);
  std::wofstream active(std::filesystem::path(host->config_dir) / L"active.json", std::ios::trunc);
  if (active) active << L"{\"slug\":\"" << slug << L"\"}\n";
  loadSpritesheet(host);
  setAnimation(host, PetAnimState::Waving, true);
}

void showMenu(Host* host) {
  if (!host || !host->hwnd) return;
  const auto slugs = petSlugs(host->asset_root);
  std::wstring active_slug = readActiveSlug(host);
  if (active_slug.empty() && !slugs.empty()) active_slug = slugs[0];

  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  UINT id = kPetMenuBase;
  for (const auto& slug : slugs) {
    const UINT flags = MF_STRING | (slug == active_slug ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(menu, flags, id++, slug.c_str());
  }
  if (!slugs.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kPetMenuQuit, L"Quit");
  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(host->hwnd);
  const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x, cursor.y, 0, host->hwnd, nullptr);
  DestroyMenu(menu);
  if (selected == kPetMenuQuit) DestroyWindow(host->hwnd);
  else if (selected >= kPetMenuBase && selected < kPetMenuBase + slugs.size()) activatePet(host, slugs[selected - kPetMenuBase]);
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
    case WM_NCHITTEST:
      return HTCLIENT;
    case WM_LBUTTONDOWN:
      beginDrag(host, hwnd);
      return 0;
    case WM_MOUSEMOVE:
      if (host && host->dragging && (wparam & MK_LBUTTON)) {
        POINT cursor{};
        GetCursorPos(&cursor);
        updateDragAnimation(host, cursor);
        SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            cursor.x - host->drag_offset.x,
            cursor.y - host->drag_offset.y,
            0,
            0,
            SWP_NOSIZE | SWP_NOACTIVATE);
        syncInputWindow(host);
      }
      return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      if (msg == WM_LBUTTONUP) finishDrag(host);
      else if (host) {
        host->dragging = false;
        host->has_last_drag_cursor = false;
        host->drag_dx_accum = 0;
        host->drag_direction_locked_until = 0;
      }
      if (msg == WM_LBUTTONUP && GetCapture() == hwnd) ReleaseCapture();
      return 0;
    case WM_NCRBUTTONUP:
    case WM_RBUTTONUP:
      showMenu(host);
      return 0;
    case WM_TIMER:
      if (host && wparam == kAnimTimer) {
        advanceAnimation(host);
      }
      return 0;
    case WM_DPICHANGED:
      if (host) {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        const UINT dpi = HIWORD(wparam);
        if (suggested) {
          SetWindowPos(hwnd, HWND_TOPMOST, suggested->left, suggested->top, MulDiv(host->width, dpi, 96), MulDiv(host->height, dpi, 96), SWP_NOACTIVATE);
        }
        render(host);
        syncInputWindow(host);
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, kAnimTimer);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

LRESULT CALLBACK inputWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Host* host = reinterpret_cast<Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      host = reinterpret_cast<Host*>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
      return TRUE;
    }
    case WM_NCHITTEST:
      return HTCLIENT;
    case WM_LBUTTONDOWN:
      beginDrag(host, hwnd);
      return 0;
    case WM_LBUTTONDBLCLK:
      if (host) {
        host->dragging = false;
        host->has_last_drag_cursor = false;
        host->drag_dx_accum = 0;
        host->drag_direction_locked_until = 0;
        if (GetCapture() == hwnd) ReleaseCapture();
        setAnimation(host, PetAnimState::Jumping, true);
      }
      return 0;
    case WM_MOUSEMOVE:
      if (host && host->dragging && (wparam & MK_LBUTTON)) {
        POINT cursor{};
        GetCursorPos(&cursor);
        updateDragAnimation(host, cursor);
        const int x = cursor.x - host->drag_offset.x;
        const int y = cursor.y - host->drag_offset.y;
        SetWindowPos(host->hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(host->input_hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
      }
      return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      if (msg == WM_LBUTTONUP) finishDrag(host);
      else if (host) {
        host->dragging = false;
        host->has_last_drag_cursor = false;
        host->drag_dx_accum = 0;
        host->drag_direction_locked_until = 0;
      }
      if (msg == WM_LBUTTONUP && GetCapture() == hwnd) ReleaseCapture();
      return 0;
    case WM_RBUTTONUP:
      showMenu(host);
      return 0;
    case WM_DESTROY:
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
  wc.lpszClassName = L"PetdexNativeWindow";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  const bool visual_ok = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

  WNDCLASSEXW input_wc{};
  input_wc.cbSize = sizeof(input_wc);
  input_wc.style = CS_DBLCLKS;
  input_wc.lpfnWndProc = inputWndProc;
  input_wc.hInstance = hinst;
  input_wc.lpszClassName = L"PetdexNativeInputWindow";
  input_wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  const bool input_ok = RegisterClassExW(&input_wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  return visual_ok && input_ok;
}

}  // namespace

extern "C" Host* petdex_native_create(const wchar_t* asset_root, const wchar_t* config_dir, int width, int height) {
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
  const int initial_w = MulDiv(width, dpi, 96);
  const int initial_h = MulDiv(height, dpi, 96);
  const POINT initial_pos = initialWindowPosition(host, initial_w, initial_h);
  host->hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
      L"PetdexNativeWindow",
      L"Petdex",
      WS_POPUP,
      initial_pos.x,
      initial_pos.y,
      initial_w,
      initial_h,
      nullptr,
      nullptr,
      hinst,
      host);
  if (!host->hwnd) {
    delete host;
    return nullptr;
  }
  host->input_hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
      L"PetdexNativeInputWindow",
      L"PetdexInput",
      WS_POPUP,
      initial_pos.x,
      initial_pos.y,
      initial_w,
      initial_h,
      nullptr,
      nullptr,
      hinst,
      host);
  if (!host->input_hwnd) {
    DestroyWindow(host->hwnd);
    delete host;
    return nullptr;
  }
  SetLayeredWindowAttributes(host->input_hwnd, RGB(0, 0, 0), 1, LWA_ALPHA);
  return host;
}

extern "C" void petdex_native_destroy(Host* host) {
  if (!host) return;
  if (host->input_hwnd) DestroyWindow(host->input_hwnd);
  if (host->hwnd) DestroyWindow(host->hwnd);
  delete host;
}

extern "C" int petdex_native_run(Host* host) {
  if (!host || !host->hwnd) return 1;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 2;
  if (!loadSpritesheet(host)) return 3;

  ShowWindow(host->hwnd, SW_SHOWNOACTIVATE);
  ShowWindow(host->input_hwnd, SW_SHOWNOACTIVATE);
  syncInputWindow(host);
  setAnimation(host, PetAnimState::Waving, true);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  CoUninitialize();
  return 0;
}
