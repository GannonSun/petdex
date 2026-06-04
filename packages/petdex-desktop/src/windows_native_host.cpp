#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <wininet.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr int kDefaultX = 80;
constexpr int kDefaultY = 120;
constexpr int kCols = 8;
constexpr int kRows = 9;
constexpr UINT_PTR kAnimTimer = 1;
constexpr UINT_PTR kPanelDismissTimer = 2;
constexpr UINT_PTR kPanelLoadTimer = 3;
constexpr UINT kPetMenuBase = 1000;
constexpr UINT kPetMenuQuit = 9000;
constexpr int kDragDirectionThresholdPx = 8;
constexpr DWORD kDragDirectionLockMs = 220;
constexpr int kIdleFrames[] = {0, 1, 2, 3, 4, 5};
constexpr UINT kIdleDurationsMs[] = {1680, 660, 660, 840, 840, 1920};
constexpr int kPanelWidthDip = 360;
constexpr int kPanelHeightDip = 344;
constexpr int kPanelCols = 3;
constexpr int kPanelHeaderDip = 42;
constexpr int kPanelFooterDip = 40;
constexpr int kPanelPaddingDip = 10;
constexpr int kPanelCellDip = 84;
constexpr int kPanelThumbDip = 48;
constexpr int kPanelGapDip = 8;
constexpr int kPanelQuitId = -2;
constexpr int kPanelInstallId = -3;
constexpr int kInstallEditId = 2001;
constexpr int kInstallOkId = 2002;
constexpr int kInstallCancelId = 2003;
constexpr int kInstallWidthDip = 420;
constexpr int kInstallHeightDip = 150;
const wchar_t* kManifestUrl = L"https://petdex.crafter.run/api/manifest";

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

struct PetMenuItem {
  std::wstring slug;
  std::wstring display_name;
};

struct Thumbnail {
  std::wstring slug;
  Image image;
};

struct Host {
  HWND hwnd = nullptr;
  HWND input_hwnd = nullptr;
  HWND picker_hwnd = nullptr;
  HWND install_hwnd = nullptr;
  HWND install_edit_hwnd = nullptr;
  int width = 140;
  int height = 180;
  std::wstring asset_root;
  std::wstring config_dir;
  Image sheet;
  std::vector<PetMenuItem> picker_items;
  std::vector<Thumbnail> thumbnail_cache;
  int picker_scroll_row = 0;
  int picker_load_index = 0;
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

std::string parseJsonStringUtf8(const std::string& text, const char* key, size_t from, size_t* next) {
  if (!key) return std::string();
  const std::string quoted = std::string("\"") + key + "\"";
  const size_t key_pos = text.find(quoted, from);
  if (key_pos == std::string::npos) return std::string();
  const size_t colon = text.find(':', key_pos + quoted.size());
  if (colon == std::string::npos) return std::string();
  const size_t open = text.find('"', colon + 1);
  if (open == std::string::npos) return std::string();

  std::string value;
  for (size_t i = open + 1; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"') {
      if (next) *next = i + 1;
      return value;
    }
    if (c == '\\' && i + 1 < text.size()) {
      const char e = text[++i];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          value.push_back(e);
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        case 'u':
          // The producer only emits ASCII-safe escapes such as \u003c,
          // \u003e, \u2028, and \u2029. They are not useful in labels, so
          // keep parsing bounded and substitute a plain space.
          if (i + 4 < text.size()) i += 4;
          value.push_back(' ');
          break;
        default:
          value.push_back(e);
          break;
      }
      continue;
    }
    value.push_back(c);
  }
  return std::string();
}

std::wstring utf8ToWide(const std::string& value) {
  if (value.empty()) return std::wstring();
  const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
  if (len <= 0) return std::wstring(value.begin(), value.end());
  std::wstring wide(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()), wide.data(), len);
  return wide;
}

std::string wideToUtf8(const std::wstring& value) {
  if (value.empty()) return std::string();
  const int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (len <= 0) return std::string(value.begin(), value.end());
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), len, nullptr, nullptr);
  return out;
}

std::string jsonEscapeUtf8(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::wstring trimWide(std::wstring value) {
  const auto not_space = [](wchar_t c) { return c != L' ' && c != L'\t' && c != L'\r' && c != L'\n'; };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

bool isSlugChar(wchar_t c) {
  return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
}

std::wstring extractSlugFromInput(std::wstring input) {
  input = trimWide(input);
  if (input.empty()) return std::wstring();

  const std::wstring scheme = L"petdex://";
  const auto scheme_pos = input.find(scheme);
  if (scheme_pos != std::wstring::npos) {
    input = input.substr(scheme_pos + scheme.size());
  } else {
    const std::wstring pets_path = L"/pets/";
    const auto pets_pos = input.find(pets_path);
    if (pets_pos != std::wstring::npos) input = input.substr(pets_pos + pets_path.size());
  }

  const auto stop = input.find_first_of(L"?#/ \t\r\n");
  if (stop != std::wstring::npos) input = input.substr(0, stop);

  std::wstring slug;
  for (wchar_t c : input) {
    if (!isSlugChar(c)) break;
    slug.push_back(static_cast<wchar_t>(std::towlower(c)));
  }
  return slug;
}

std::vector<unsigned char> fetchUrlBytes(const std::wstring& url) {
  std::vector<unsigned char> bytes;
  HINTERNET session = InternetOpenW(L"PetdexDesktop/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
  if (!session) return bytes;

  HINTERNET handle = InternetOpenUrlW(
      session,
      url.c_str(),
      nullptr,
      0,
      INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE,
      0);
  if (!handle) {
    InternetCloseHandle(session);
    return bytes;
  }

  unsigned char buffer[16 * 1024];
  DWORD read = 0;
  while (InternetReadFile(handle, buffer, sizeof(buffer), &read) && read > 0) {
    bytes.insert(bytes.end(), buffer, buffer + read);
  }

  InternetCloseHandle(handle);
  InternetCloseHandle(session);
  return bytes;
}

std::string fetchUrlString(const std::wstring& url) {
  const auto bytes = fetchUrlBytes(url);
  return std::string(bytes.begin(), bytes.end());
}

std::string findManifestObjectForSlug(const std::string& manifest, const std::string& slug) {
  const std::string needle = "\"slug\":\"" + slug + "\"";
  const size_t slug_pos = manifest.find(needle);
  if (slug_pos == std::string::npos) return std::string();

  size_t start = manifest.rfind('{', slug_pos);
  size_t end = manifest.find('}', slug_pos);
  if (start == std::string::npos || end == std::string::npos || end <= start) return std::string();
  return manifest.substr(start, end - start + 1);
}

std::string jsonStringFieldFromObject(const std::string& object, const char* key) {
  return parseJsonStringUtf8(object, key, 0, nullptr);
}

std::wstring envPath(const wchar_t* name) {
  wchar_t buffer[MAX_PATH * 4]{};
  const DWORD len = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
  if (len == 0 || len >= std::size(buffer)) return std::wstring();
  return std::wstring(buffer, buffer + len);
}

std::filesystem::path primaryPetsDir() {
  const std::wstring local = envPath(L"LOCALAPPDATA");
  if (!local.empty()) return std::filesystem::path(local) / L".petdex" / L"pets";
  const std::wstring user = envPath(L"USERPROFILE");
  if (!user.empty()) return std::filesystem::path(user) / L".petdex" / L"pets";
  return std::filesystem::path();
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
void hidePicker(Host* host);
void activatePet(Host* host, const std::wstring& slug);
LRESULT CALLBACK pickerWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK installWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
std::vector<std::wstring> petSlugs(const std::wstring& asset_root);

COLORREF panelBg() {
  return RGB(28, 30, 34);
}

int scaleDip(HWND hwnd, int value) {
  const UINT dpi = GetDpiForWindow(hwnd);
  return MulDiv(value, dpi == 0 ? 96 : dpi, 96);
}

RECT panelBodyRect(HWND hwnd) {
  RECT rect{};
  GetClientRect(hwnd, &rect);
  rect.left += scaleDip(hwnd, kPanelPaddingDip);
  rect.right -= scaleDip(hwnd, kPanelPaddingDip);
  rect.top += scaleDip(hwnd, kPanelHeaderDip);
  rect.bottom -= scaleDip(hwnd, kPanelFooterDip);
  return rect;
}

int panelVisibleRows(HWND hwnd) {
  const RECT body = panelBodyRect(hwnd);
  const int cell = scaleDip(hwnd, kPanelCellDip);
  const int body_h = static_cast<int>(body.bottom - body.top);
  return std::max(1, body_h / std::max(1, cell));
}

int panelMaxScrollRow(HWND hwnd, int item_count) {
  const int rows = (item_count + kPanelCols - 1) / kPanelCols;
  return std::max(0, rows - panelVisibleRows(hwnd));
}

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

void blendFrameBgra(
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
      const unsigned int alpha = sheet.pixels[src_index + 3];
      const unsigned int inv = 255 - alpha;
      dst[dst_index + 0] = static_cast<unsigned char>(sheet.pixels[src_index + 0] + (dst[dst_index + 0] * inv) / 255);
      dst[dst_index + 1] = static_cast<unsigned char>(sheet.pixels[src_index + 1] + (dst[dst_index + 1] * inv) / 255);
      dst[dst_index + 2] = static_cast<unsigned char>(sheet.pixels[src_index + 2] + (dst[dst_index + 2] * inv) / 255);
      dst[dst_index + 3] = 255;
    }
  }
}

std::vector<PetMenuItem> readPetMenuItems(const Host* host) {
  std::vector<PetMenuItem> items;
  if (!host) return items;

  std::ifstream file(std::filesystem::path(host->asset_root) / L"petdex.json");
  if (file) {
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (true) {
      size_t next = 0;
      const std::string slug = parseJsonStringUtf8(text, "slug", pos, &next);
      if (slug.empty()) break;
      const std::string display = parseJsonStringUtf8(text, "displayName", next, &next);
      PetMenuItem item;
      item.slug = utf8ToWide(slug);
      item.display_name = display.empty() ? item.slug : utf8ToWide(display);
      if (!item.slug.empty()) items.push_back(std::move(item));
      pos = next == 0 ? pos + 1 : next;
      if (pos >= text.size()) break;
    }
  }

  if (!items.empty()) return items;

  for (const auto& slug : petSlugs(host->asset_root)) {
    items.push_back(PetMenuItem{slug, slug});
  }
  return items;
}

RECT itemRectForIndex(HWND hwnd, int visible_index) {
  const RECT body = panelBodyRect(hwnd);
  const int gap = scaleDip(hwnd, kPanelGapDip);
  const int cell_h = scaleDip(hwnd, kPanelCellDip);
  const int col_w = (body.right - body.left - gap * (kPanelCols - 1)) / kPanelCols;
  const int row = visible_index / kPanelCols;
  const int col = visible_index % kPanelCols;
  const int left = body.left + col * (col_w + gap);
  const int top = body.top + row * cell_h;
  return RECT{left, top, left + col_w, top + cell_h - gap};
}

int hitTestPicker(Host* host, int x, int y) {
  if (!host || !host->picker_hwnd) return -1;
  RECT client{};
  GetClientRect(host->picker_hwnd, &client);
  const int footer_top = client.bottom - scaleDip(host->picker_hwnd, kPanelFooterDip);
  if (y >= footer_top) {
    return x < client.right / 2 ? kPanelInstallId : kPanelQuitId;
  }

  const RECT body = panelBodyRect(host->picker_hwnd);
  if (x < body.left || x >= body.right || y < body.top || y >= body.bottom) return -1;

  const int gap = scaleDip(host->picker_hwnd, kPanelGapDip);
  const int cell_h = scaleDip(host->picker_hwnd, kPanelCellDip);
  const int col_w = (body.right - body.left - gap * (kPanelCols - 1)) / kPanelCols;
  const int col_span = col_w + gap;
  const int rel_x = x - body.left;
  const int col = rel_x / std::max(1, col_span);
  if (col < 0 || col >= kPanelCols || rel_x - col * col_span >= col_w) return -1;

  const int row = (y - body.top) / std::max(1, cell_h);
  const int index = (host->picker_scroll_row + row) * kPanelCols + col;
  return index >= 0 && index < static_cast<int>(host->picker_items.size()) ? index : -1;
}

const Image* cachedThumbnail(const Host* host, const std::wstring& slug) {
  if (!host) return nullptr;
  for (const auto& thumb : host->thumbnail_cache) {
    if (thumb.slug == slug) return &thumb.image;
  }
  return nullptr;
}

bool cacheThumbnail(Host* host, const std::wstring& slug) {
  if (!host || slug.empty() || cachedThumbnail(host, slug)) return false;

  Image image;
  const auto root = std::filesystem::path(host->asset_root) / slug;
  if (!(loadImageWic(root / L"spritesheet.webp", &image) || loadImageWic(root / L"spritesheet.png", &image))) {
    return false;
  }

  host->thumbnail_cache.push_back(Thumbnail{slug, std::move(image)});
  return true;
}

bool loadNextPickerThumbnail(Host* host) {
  if (!host || !host->picker_hwnd) return false;
  const int total = static_cast<int>(host->picker_items.size());
  if (total <= 0) return false;

  for (int attempts = 0; attempts < total; ++attempts) {
    const int index = (host->picker_load_index + attempts) % total;
    const auto& slug = host->picker_items[index].slug;
    if (cachedThumbnail(host, slug)) continue;

    host->picker_load_index = (index + 1) % total;
    return cacheThumbnail(host, slug);
  }

  return false;
}

void writeAssetPetdexJson(Host* host) {
  if (!host) return;
  const auto path = std::filesystem::path(host->asset_root) / L"petdex.json";
  std::ofstream out(path, std::ios::trunc);
  if (!out) return;

  const std::wstring active = readActiveSlug(host);
  out << "{\"pets\":[";
  for (size_t i = 0; i < host->picker_items.size(); ++i) {
    if (i > 0) out << ",";
    out << "{\"slug\":\"" << jsonEscapeUtf8(wideToUtf8(host->picker_items[i].slug)) << "\",\"displayName\":\"" << jsonEscapeUtf8(wideToUtf8(host->picker_items[i].display_name)) << "\"}";
  }
  out << "],\"active\":\"" << jsonEscapeUtf8(wideToUtf8(active)) << "\"}\n";
}

void upsertPickerItem(Host* host, const std::wstring& slug, const std::wstring& display_name) {
  if (!host || slug.empty()) return;
  for (auto& item : host->picker_items) {
    if (item.slug == slug) {
      if (!display_name.empty()) item.display_name = display_name;
      return;
    }
  }
  host->picker_items.push_back(PetMenuItem{slug, display_name.empty() ? slug : display_name});
  std::sort(host->picker_items.begin(), host->picker_items.end(), [](const PetMenuItem& a, const PetMenuItem& b) {
    return a.slug < b.slug;
  });
}

bool writeBytesFile(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

std::wstring installFromPetdex(Host* host, const std::wstring& raw_input) {
  if (!host) return L"Petdex is not ready.";
  const std::wstring slug_w = extractSlugFromInput(raw_input);
  if (slug_w.empty()) return L"Paste a Petdex slug or pet page URL.";

  const std::string slug = wideToUtf8(slug_w);
  const std::string manifest = fetchUrlString(kManifestUrl);
  if (manifest.empty()) return L"Could not reach Petdex manifest.";

  const std::string object = findManifestObjectForSlug(manifest, slug);
  if (object.empty()) return L"That pet slug is not approved in Petdex yet.";

  const std::string display = jsonStringFieldFromObject(object, "displayName");
  const std::string pet_json_url = jsonStringFieldFromObject(object, "petJsonUrl");
  const std::string sprite_url = jsonStringFieldFromObject(object, "spritesheetUrl");
  if (pet_json_url.empty() || sprite_url.empty()) return L"Petdex manifest is missing asset URLs.";

  const auto pet_json = fetchUrlBytes(utf8ToWide(pet_json_url));
  const auto sprite = fetchUrlBytes(utf8ToWide(sprite_url));
  if (pet_json.empty() || sprite.empty()) return L"Could not download pet assets.";

  const bool is_png = sprite_url.size() >= 4 && sprite_url.substr(sprite_url.size() - 4) == ".png";
  const std::wstring sprite_name = is_png ? L"spritesheet.png" : L"spritesheet.webp";
  const auto pets_root = primaryPetsDir();
  if (pets_root.empty()) return L"Could not find a user pets directory.";

  const auto pet_dir = pets_root / slug_w;
  if (!writeBytesFile(pet_dir / L"pet.json", pet_json)) return L"Could not write pet.json.";
  if (!writeBytesFile(pet_dir / sprite_name, sprite)) return L"Could not write spritesheet.";

  const auto asset_pet_dir = std::filesystem::path(host->asset_root) / slug_w;
  writeBytesFile(asset_pet_dir / L"pet.json", pet_json);
  writeBytesFile(asset_pet_dir / sprite_name, sprite);
  upsertPickerItem(host, slug_w, display.empty() ? slug_w : utf8ToWide(display));
  writeAssetPetdexJson(host);
  cacheThumbnail(host, slug_w);
  activatePet(host, slug_w);
  return std::wstring();
}

void drawPlaceholderThumb(unsigned char* pixels, int width, int height, const RECT& cell, int thumb, HWND hwnd) {
  const int tx = static_cast<int>(cell.left) + ((static_cast<int>(cell.right - cell.left)) - thumb) / 2;
  const int ty = static_cast<int>(cell.top) + scaleDip(hwnd, 7);
  const int radius = std::max(1, thumb / 2);
  const int cx = tx + radius;
  const int cy = ty + radius;
  const COLORREF fill = RGB(74, 78, 88);
  const COLORREF shine = RGB(106, 112, 126);

  for (int y = std::max(0, ty); y < std::min(height, ty + thumb); ++y) {
    for (int x = std::max(0, tx); x < std::min(width, tx + thumb); ++x) {
      const int dx = x - cx;
      const int dy = y - cy;
      if (dx * dx + dy * dy > radius * radius) continue;
      const bool upper = y < cy - radius / 5;
      const COLORREF color = upper ? shine : fill;
      const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
      pixels[idx + 0] = GetBValue(color);
      pixels[idx + 1] = GetGValue(color);
      pixels[idx + 2] = GetRValue(color);
      pixels[idx + 3] = 255;
    }
  }
}

void drawPanelBuffer(Host* host, HDC hdc, const RECT& client) {
  if (!host || !host->picker_hwnd) return;
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  if (width <= 0 || height <= 0) return;

  std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
  const COLORREF bg = panelBg();
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = GetBValue(bg);
    pixels[i + 1] = GetGValue(bg);
    pixels[i + 2] = GetRValue(bg);
    pixels[i + 3] = 255;
  }

  const std::wstring active = readActiveSlug(host);
  const int visible_rows = panelVisibleRows(host->picker_hwnd);
  const int first = host->picker_scroll_row * kPanelCols;
  const int max_visible = visible_rows * kPanelCols;
  const int thumb = scaleDip(host->picker_hwnd, kPanelThumbDip);
  const int thumb_top_pad = scaleDip(host->picker_hwnd, 7);

  for (int slot = 0; slot < max_visible; ++slot) {
    const int item_index = first + slot;
    if (item_index >= static_cast<int>(host->picker_items.size())) break;
    const RECT cell = itemRectForIndex(host->picker_hwnd, slot);
    const auto& item = host->picker_items[item_index];

    const bool selected = item.slug == active;
    const COLORREF cell_bg = selected ? RGB(51, 72, 104) : RGB(38, 40, 45);
    const int fill_top = std::max(0, static_cast<int>(cell.top));
    const int fill_bottom = std::min(height, static_cast<int>(cell.bottom));
    const int fill_left = std::max(0, static_cast<int>(cell.left));
    const int fill_right = std::min(width, static_cast<int>(cell.right));
    for (int py = fill_top; py < fill_bottom; ++py) {
      for (int px = fill_left; px < fill_right; ++px) {
        const size_t idx = (static_cast<size_t>(py) * width + px) * 4;
        pixels[idx + 0] = GetBValue(cell_bg);
        pixels[idx + 1] = GetGValue(cell_bg);
        pixels[idx + 2] = GetRValue(cell_bg);
      }
    }

    if (const Image* thumb_sheet = cachedThumbnail(host, item.slug)) {
      const int frame_w = static_cast<int>(thumb_sheet->width / kCols);
      const int frame_h = static_cast<int>(thumb_sheet->height / kRows);
      const int tx = static_cast<int>(cell.left) + ((static_cast<int>(cell.right - cell.left)) - thumb) / 2;
      const int ty = static_cast<int>(cell.top) + thumb_top_pad;
      blendFrameBgra(pixels.data(), width, height, tx, ty, thumb, thumb, *thumb_sheet, 0, 0, frame_w, frame_h);
    } else {
      drawPlaceholderThumb(pixels.data(), width, height, cell, thumb, host->picker_hwnd);
    }
  }

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  SetDIBitsToDevice(hdc, 0, 0, width, height, 0, 0, 0, height, pixels.data(), &info, DIB_RGB_COLORS);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(238, 241, 245));
  HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  HGDIOBJ old_font = SelectObject(hdc, font);

  RECT title{scaleDip(host->picker_hwnd, 14), scaleDip(host->picker_hwnd, 8), width - scaleDip(host->picker_hwnd, 14), scaleDip(host->picker_hwnd, 30)};
  DrawTextW(hdc, L"Petdex pets", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  const std::wstring count = std::to_wstring(host->picker_items.size()) + L" installed";
  SetTextColor(hdc, RGB(158, 166, 178));
  RECT count_rect{scaleDip(host->picker_hwnd, 14), scaleDip(host->picker_hwnd, 8), width - scaleDip(host->picker_hwnd, 14), scaleDip(host->picker_hwnd, 30)};
  DrawTextW(hdc, count.c_str(), -1, &count_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

  const int first_label = host->picker_scroll_row * kPanelCols;
  for (int slot = 0; slot < max_visible; ++slot) {
    const int item_index = first_label + slot;
    if (item_index >= static_cast<int>(host->picker_items.size())) break;
    const RECT cell = itemRectForIndex(host->picker_hwnd, slot);
    const bool selected = host->picker_items[item_index].slug == active;
    SetTextColor(hdc, selected ? RGB(255, 255, 255) : RGB(206, 212, 222));
    RECT label{cell.left + scaleDip(host->picker_hwnd, 4), cell.bottom - scaleDip(host->picker_hwnd, 23), cell.right - scaleDip(host->picker_hwnd, 4), cell.bottom - scaleDip(host->picker_hwnd, 5)};
    DrawTextW(hdc, host->picker_items[item_index].display_name.c_str(), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  RECT divider{scaleDip(host->picker_hwnd, 10), height - scaleDip(host->picker_hwnd, kPanelFooterDip), width - scaleDip(host->picker_hwnd, 10), height - scaleDip(host->picker_hwnd, kPanelFooterDip) + 1};
  HBRUSH divider_brush = CreateSolidBrush(RGB(58, 61, 68));
  FillRect(hdc, &divider, divider_brush);
  DeleteObject(divider_brush);

  SetTextColor(hdc, RGB(185, 214, 255));
  RECT install{scaleDip(host->picker_hwnd, 14), height - scaleDip(host->picker_hwnd, 32), width - scaleDip(host->picker_hwnd, 14), height - scaleDip(host->picker_hwnd, 8)};
  DrawTextW(hdc, L"Install from Petdex...", -1, &install, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  SetTextColor(hdc, RGB(255, 148, 148));
  RECT quit{scaleDip(host->picker_hwnd, 14), height - scaleDip(host->picker_hwnd, 32), width - scaleDip(host->picker_hwnd, 14), height - scaleDip(host->picker_hwnd, 8)};
  DrawTextW(hdc, L"Quit", -1, &quit, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

  SelectObject(hdc, old_font);
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
  hidePicker(host);
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

void hidePicker(Host* host) {
  if (!host || !host->picker_hwnd) return;
  ShowWindow(host->picker_hwnd, SW_HIDE);
  KillTimer(host->picker_hwnd, kPanelDismissTimer);
  KillTimer(host->picker_hwnd, kPanelLoadTimer);
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
  if (host->picker_hwnd) InvalidateRect(host->picker_hwnd, nullptr, FALSE);
}

POINT pickerPosition(Host* host, int panel_w, int panel_h) {
  RECT pet{};
  GetWindowRect(host->hwnd, &pet);
  HMONITOR mon = MonitorFromRect(&pet, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  RECT work{};
  if (mon && GetMonitorInfoW(mon, &info)) {
    work = info.rcWork;
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  }

  int x = pet.right + 10;
  int y = pet.top;
  if (x + panel_w > work.right) x = pet.left - panel_w - 10;
  if (x < work.left) x = work.left + 8;
  if (y + panel_h > work.bottom) y = work.bottom - panel_h - 8;
  if (y < work.top) y = work.top + 8;
  return POINT{x, y};
}

bool ensurePickerWindow(Host* host) {
  if (!host || !host->hwnd) return false;
  if (host->picker_hwnd) return true;

  const int panel_w = scaleDip(host->hwnd, kPanelWidthDip);
  const int panel_h = scaleDip(host->hwnd, kPanelHeightDip);
  const POINT pos = pickerPosition(host, panel_w, panel_h);
  host->picker_hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"PetdexNativePickerWindow",
      L"Petdex pets",
      WS_POPUP,
      pos.x,
      pos.y,
      panel_w,
      panel_h,
      host->hwnd,
      nullptr,
      GetModuleHandleW(nullptr),
      host);
  return host->picker_hwnd != nullptr;
}

POINT centeredPopupPosition(Host* host, int width, int height) {
  RECT pet{};
  if (host && host->hwnd) GetWindowRect(host->hwnd, &pet);
  HMONITOR mon = MonitorFromRect(&pet, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  RECT work{};
  if (mon && GetMonitorInfoW(mon, &info)) {
    work = info.rcWork;
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  }
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;
  return POINT{x, y};
}

bool ensureInstallWindow(Host* host) {
  if (!host || !host->hwnd) return false;
  if (host->install_hwnd) return true;

  const int w = scaleDip(host->hwnd, kInstallWidthDip);
  const int h = scaleDip(host->hwnd, kInstallHeightDip);
  const POINT pos = centeredPopupPosition(host, w, h);
  host->install_hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
      L"PetdexInstallWindow",
      L"Install from Petdex",
      WS_POPUP | WS_CAPTION | WS_SYSMENU,
      pos.x,
      pos.y,
      w,
      h,
      host->hwnd,
      nullptr,
      GetModuleHandleW(nullptr),
      host);
  return host->install_hwnd != nullptr;
}

void showInstallDialog(Host* host) {
  if (!host || !ensureInstallWindow(host)) return;
  hidePicker(host);

  const int w = scaleDip(host->hwnd, kInstallWidthDip);
  const int h = scaleDip(host->hwnd, kInstallHeightDip);
  const POINT pos = centeredPopupPosition(host, w, h);
  SetWindowPos(host->install_hwnd, HWND_TOPMOST, pos.x, pos.y, w, h, SWP_SHOWWINDOW);
  if (host->install_edit_hwnd) {
    SetFocus(host->install_edit_hwnd);
    SendMessageW(host->install_edit_hwnd, EM_SETSEL, 0, -1);
  }
}

void showMenu(Host* host) {
  if (!host || !host->hwnd) return;
  if (!ensurePickerWindow(host)) return;

  host->picker_items = readPetMenuItems(host);
  host->picker_scroll_row = std::clamp(host->picker_scroll_row, 0, panelMaxScrollRow(host->picker_hwnd, static_cast<int>(host->picker_items.size())));
  host->picker_load_index = 0;

  const int panel_w = scaleDip(host->hwnd, kPanelWidthDip);
  const int panel_h = scaleDip(host->hwnd, kPanelHeightDip);
  const POINT pos = pickerPosition(host, panel_w, panel_h);
  SetWindowPos(host->picker_hwnd, HWND_TOPMOST, pos.x, pos.y, panel_w, panel_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  SetTimer(host->picker_hwnd, kPanelDismissTimer, 120, nullptr);
  InvalidateRect(host->picker_hwnd, nullptr, FALSE);
  UpdateWindow(host->picker_hwnd);
  SetTimer(host->picker_hwnd, kPanelLoadTimer, 16, nullptr);
}

void maybeDismissPicker(Host* host) {
  if (!host || !host->picker_hwnd || !IsWindowVisible(host->picker_hwnd)) return;
  const bool mouse_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
  if (!mouse_down) return;

  POINT cursor{};
  GetCursorPos(&cursor);
  RECT picker{};
  RECT pet{};
  RECT input{};
  GetWindowRect(host->picker_hwnd, &picker);
  GetWindowRect(host->hwnd, &pet);
  GetWindowRect(host->input_hwnd, &input);
  if (!PtInRect(&picker, cursor) && !PtInRect(&pet, cursor) && !PtInRect(&input, cursor)) {
    hidePicker(host);
  }
}

LRESULT CALLBACK pickerWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Host* host = reinterpret_cast<Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      host = reinterpret_cast<Host*>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
      return TRUE;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT client{};
      GetClientRect(hwnd, &client);
      drawPanelBuffer(host, hdc, client);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      if (host) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        const int max_row = panelMaxScrollRow(hwnd, static_cast<int>(host->picker_items.size()));
        host->picker_scroll_row = std::clamp(host->picker_scroll_row + (delta < 0 ? 1 : -1), 0, max_row);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      if (host) {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int hit = hitTestPicker(host, x, y);
        if (hit == kPanelInstallId) {
          showInstallDialog(host);
          return 0;
        }
        if (hit == kPanelQuitId) {
          DestroyWindow(host->hwnd);
          return 0;
        }
        if (hit >= 0 && hit < static_cast<int>(host->picker_items.size())) {
          const std::wstring slug = host->picker_items[hit].slug;
          hidePicker(host);
          activatePet(host, slug);
          return 0;
        }
      }
      return 0;
    }
    case WM_RBUTTONUP:
    case WM_NCRBUTTONUP:
      hidePicker(host);
      return 0;
    case WM_TIMER:
      if (wparam == kPanelDismissTimer) {
        maybeDismissPicker(host);
      } else if (wparam == kPanelLoadTimer) {
        if (loadNextPickerThumbnail(host)) {
          InvalidateRect(hwnd, nullptr, FALSE);
        } else {
          KillTimer(hwnd, kPanelLoadTimer);
        }
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, kPanelDismissTimer);
      KillTimer(hwnd, kPanelLoadTimer);
      if (host && host->picker_hwnd == hwnd) host->picker_hwnd = nullptr;
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

void layoutInstallWindow(Host* host, HWND hwnd) {
  if (!host) return;
  RECT client{};
  GetClientRect(hwnd, &client);
  const int pad = scaleDip(hwnd, 14);
  const int label_h = scaleDip(hwnd, 22);
  const int edit_h = scaleDip(hwnd, 26);
  const int button_w = scaleDip(hwnd, 84);
  const int button_h = scaleDip(hwnd, 28);
  const int gap = scaleDip(hwnd, 8);

  HWND label = GetDlgItem(hwnd, 1);
  if (label) SetWindowPos(label, nullptr, pad, pad, client.right - pad * 2, label_h, SWP_NOZORDER);
  if (host->install_edit_hwnd) {
    SetWindowPos(host->install_edit_hwnd, nullptr, pad, pad + label_h, client.right - pad * 2, edit_h, SWP_NOZORDER);
  }
  HWND ok = GetDlgItem(hwnd, kInstallOkId);
  HWND cancel = GetDlgItem(hwnd, kInstallCancelId);
  const int y = client.bottom - pad - button_h;
  if (ok) SetWindowPos(ok, nullptr, client.right - pad - button_w * 2 - gap, y, button_w, button_h, SWP_NOZORDER);
  if (cancel) SetWindowPos(cancel, nullptr, client.right - pad - button_w, y, button_w, button_h, SWP_NOZORDER);
}

void runInstallFromDialog(Host* host) {
  if (!host || !host->install_edit_hwnd) return;
  const int len = GetWindowTextLengthW(host->install_edit_hwnd);
  std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(host->install_edit_hwnd, buffer.data(), static_cast<int>(buffer.size()));
  std::wstring input(buffer.data());

  ShowWindow(host->install_hwnd, SW_HIDE);
  setAnimation(host, PetAnimState::Waiting, true);
  const std::wstring error = installFromPetdex(host, input);
  if (error.empty()) {
    MessageBoxW(host->hwnd, L"Pet installed and activated.", L"Petdex", MB_OK | MB_ICONINFORMATION);
  } else {
    setAnimation(host, PetAnimState::Failed, true);
    MessageBoxW(host->hwnd, error.c_str(), L"Petdex install failed", MB_OK | MB_ICONERROR);
  }
}

LRESULT CALLBACK installWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Host* host = reinterpret_cast<Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      host = reinterpret_cast<Host*>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
      return TRUE;
    }
    case WM_CREATE: {
      host = reinterpret_cast<Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      CreateWindowExW(0, L"STATIC", L"Paste a Petdex slug or pet page URL:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
      if (host) {
        host->install_edit_hwnd = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(kInstallEditId),
            GetModuleHandleW(nullptr),
            nullptr);
      }
      CreateWindowExW(0, L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kInstallOkId), GetModuleHandleW(nullptr), nullptr);
      CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kInstallCancelId), GetModuleHandleW(nullptr), nullptr);
      layoutInstallWindow(host, hwnd);
      return 0;
    }
    case WM_SIZE:
      layoutInstallWindow(host, hwnd);
      return 0;
    case WM_COMMAND:
      if (LOWORD(wparam) == kInstallOkId) {
        runInstallFromDialog(host);
        return 0;
      }
      if (LOWORD(wparam) == kInstallCancelId) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
      return 0;
    case WM_CLOSE:
      ShowWindow(hwnd, SW_HIDE);
      return 0;
    case WM_DESTROY:
      if (host && host->install_hwnd == hwnd) {
        host->install_hwnd = nullptr;
        host->install_edit_hwnd = nullptr;
      }
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
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

  WNDCLASSEXW picker_wc{};
  picker_wc.cbSize = sizeof(picker_wc);
  picker_wc.lpfnWndProc = pickerWndProc;
  picker_wc.hInstance = hinst;
  picker_wc.lpszClassName = L"PetdexNativePickerWindow";
  picker_wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  const bool picker_ok = RegisterClassExW(&picker_wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

  WNDCLASSEXW install_wc{};
  install_wc.cbSize = sizeof(install_wc);
  install_wc.lpfnWndProc = installWndProc;
  install_wc.hInstance = hinst;
  install_wc.lpszClassName = L"PetdexInstallWindow";
  install_wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  install_wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  const bool install_ok = RegisterClassExW(&install_wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

  return visual_ok && input_ok && picker_ok && install_ok;
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
  if (host->install_hwnd && IsWindow(host->install_hwnd)) DestroyWindow(host->install_hwnd);
  if (host->picker_hwnd && IsWindow(host->picker_hwnd)) DestroyWindow(host->picker_hwnd);
  if (host->input_hwnd && IsWindow(host->input_hwnd)) DestroyWindow(host->input_hwnd);
  if (host->hwnd && IsWindow(host->hwnd)) DestroyWindow(host->hwnd);
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
