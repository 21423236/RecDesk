#include "recorder.h"
#include "region_selector.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>
#include <optional>

namespace fs = std::filesystem;
using namespace recdesk;

namespace {

constexpr int idStart = 100, idStop = 101, idRegion = 102, idRecovery = 103, idFolder = 104,
  idLog = 105, idCancel = 106, idDisplay = 107, idIndicatorStop = 108;
constexpr COLORREF background = RGB(24, 29, 39), foreground = RGB(227, 233, 243);

std::wstring instanceMutexName() {
  auto objectName = [](HANDLE object) {
    std::array<wchar_t, 256> name{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(object, UOI_NAME, name.data(), static_cast<DWORD>(sizeof(name)), &needed))
      throw std::runtime_error("无法确定当前桌面: " + utf8(errorMessage()));
    std::wstring result(name.data());
    std::replace(result.begin(), result.end(), L'\\', L'_');
    return result;
  };
  // Test/remote desktops must not suppress the user's visible interactive instance.
  return L"Local\\RecDeskNative-DesktopRecorder-" + objectName(GetProcessWindowStation()) + L"-" +
    objectName(GetThreadDesktop(GetCurrentThreadId()));
}

void restoreSettingsWindow(HWND window) {
  RECT work{};
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor)) work = monitor.rcWork;
  else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  ShowWindow(window, SW_RESTORE);
  RECT bounds{}; GetWindowRect(window, &bounds);
  if (bounds.left < work.left || bounds.top < work.top || bounds.right > work.right || bounds.bottom > work.bottom) {
    int width = std::min(static_cast<int>(bounds.right - bounds.left), static_cast<int>(work.right - work.left));
    int height = std::min(static_cast<int>(bounds.bottom - bounds.top), static_cast<int>(work.bottom - work.top));
    SetWindowPos(window, nullptr, work.left + (work.right - work.left - width) / 2,
      work.top + (work.bottom - work.top - height) / 2, width, height, SWP_NOZORDER | SWP_SHOWWINDOW);
  }
  SetForegroundWindow(window);
}

std::optional<fs::path> chooseOutput(HWND owner, fs::path suggested = {}) {
  if (suggested.empty()) {
    SYSTEMTIME time{}; GetLocalTime(&time);
    wchar_t name[100]{};
    swprintf_s(name, L"RecDesk-%04u%02u%02u-%02u%02u%02u.mp4", time.wYear, time.wMonth, time.wDay,
      time.wHour, time.wMinute, time.wSecond);
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &raw))) {
      suggested = fs::path(raw) / name; CoTaskMemFree(raw);
    } else suggested = fs::path(name);
  }
  std::array<wchar_t, 32768> buffer{};
  wcsncpy_s(buffer.data(), buffer.size(), suggested.c_str(), _TRUNCATE);
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"MP4 视频 (*.mp4)\0*.mp4\0\0";
  dialog.lpstrFile = buffer.data(); dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"mp4"; dialog.lpstrTitle = L"选择录制文件保存位置";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&dialog)) return std::nullopt;
  fs::path result(buffer.data());
  auto extension = result.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  if (extension != L".mp4") result += L".mp4";
  else result.replace_extension(L".mp4");
  if (result != fs::path(buffer.data()) && fs::exists(result) &&
      MessageBoxW(owner, (L"最终 MP4 文件已存在，是否覆盖？\r\n" + result.wstring()).c_str(),
        L"确认覆盖", MB_YESNO | MB_ICONQUESTION) != IDYES) return std::nullopt;
  return result;
}

class Application {
public:
  explicit Application(HINSTANCE instance, bool allowCapture) : instance_(instance), allowCapture_(allowCapture) {}
  ~Application() {
    if (indicator_) DestroyWindow(indicator_);
    if (font_) DeleteObject(font_);
    if (titleFont_) DeleteObject(titleFont_);
    if (brush_) DeleteObject(brush_);
  }
  int run(int show);
private:
  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK indicatorProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT dispatch(UINT message, WPARAM wParam, LPARAM lParam);
  HWND control(const wchar_t* type, const std::wstring& text, DWORD style, int id = 0);
  void initialize();
  void layout();
  void refreshDisplays();
  void refreshResolutions();
  void update();
  void start();
  void select();
  void recover();
  void showIndicator(bool show);
  int px(int value) const { return MulDiv(value, dpi_, 96); }
  bool checked(HWND window) const { return SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED; }
  void openPath(const fs::path& path) { if (!path.empty()) ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
  HINSTANCE instance_;
  HWND window_ = nullptr, indicator_ = nullptr, indicatorLabel_ = nullptr;
  HWND display_ = nullptr, resolution_ = nullptr, fps_ = nullptr, system_ = nullptr, mic_ = nullptr, cursor_ = nullptr,
    regionButton_ = nullptr, startButton_ = nullptr, stopButton_ = nullptr, recoveryButton_ = nullptr,
    folderButton_ = nullptr, logButton_ = nullptr, cancelButton_ = nullptr, status_ = nullptr, stats_ = nullptr,
    regionLabel_ = nullptr, description_ = nullptr, title_ = nullptr, displayLabel_ = nullptr,
    resolutionLabel_ = nullptr, fpsLabel_ = nullptr;
  HFONT font_ = nullptr, titleFont_ = nullptr;
  HBRUSH brush_ = nullptr;
  UINT dpi_ = 96;
  std::vector<Display> displays_;
  std::vector<int> resolutions_;
  std::optional<RECT> region_;
  Recorder recorder_;
  bool hotkey_ = false, closePending_ = false, allowCapture_ = true;
};

HWND Application::control(const wchar_t* type, const std::wstring& text, DWORD style, int id) {
  HWND result = CreateWindowExW(0, type, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
    window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
  if (!result) throw std::runtime_error("无法创建界面控件");
  SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  return result;
}

void Application::initialize() {
  dpi_ = GetDpiForWindow(window_);
  brush_ = CreateSolidBrush(background);
  font_ = CreateFontW(-MulDiv(11, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
  titleFont_ = CreateFontW(-MulDiv(24, dpi_, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  title_ = control(L"STATIC", L"RecDesk Native", 0);
  SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
  description_ = control(L"STATIC", L"C++ 原生录屏 · 实时编码落盘 · 失败保留源文件", 0);
  displayLabel_ = control(L"STATIC", L"录制显示器", 0);
  display_ = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, idDisplay);
  resolutionLabel_ = control(L"STATIC", L"输出分辨率（保持比例，不放大）", 0);
  resolution_ = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL);
  fpsLabel_ = control(L"STATIC", L"帧率", 0);
  fps_ = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP);
  for (const auto* text : {L"60 FPS", L"30 FPS", L"24 FPS", L"15 FPS"})
    SendMessageW(fps_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
  SendMessageW(fps_, CB_SETCURSEL, 1, 0);
  system_ = control(L"BUTTON", L"系统声音（WASAPI）", BS_AUTOCHECKBOX | WS_TABSTOP);
  mic_ = control(L"BUTTON", L"麦克风（默认设备）", BS_AUTOCHECKBOX | WS_TABSTOP);
  cursor_ = control(L"BUTTON", L"录制鼠标指针", BS_AUTOCHECKBOX | WS_TABSTOP);
  // Themed checkboxes force black text; classic checkbox drawing respects our dark palette.
  for (HWND check : {system_, mic_, cursor_}) SetWindowTheme(check, L"", L"");
  SendMessageW(system_, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(cursor_, BM_SETCHECK, BST_CHECKED, 0);
  regionButton_ = control(L"BUTTON", L"框选区域…", WS_TABSTOP, idRegion);
  regionLabel_ = control(L"STATIC", L"", 0);
  startButton_ = control(L"BUTTON", L"开始录制", BS_DEFPUSHBUTTON | WS_TABSTOP, idStart);
  stopButton_ = control(L"BUTTON", L"停止并保存", WS_TABSTOP, idStop);
  cancelButton_ = control(L"BUTTON", L"取消导出", WS_TABSTOP, idCancel);
  status_ = control(L"STATIC", L"准备就绪", SS_LEFT);
  stats_ = control(L"STATIC", L"00:00:00   |   0.0 MB", 0);
  recoveryButton_ = control(L"BUTTON", L"恢复录制 / 导入旧文件", WS_TABSTOP, idRecovery);
  folderButton_ = control(L"BUTTON", L"打开文件位置", WS_TABSTOP, idFolder);
  logButton_ = control(L"BUTTON", L"查看日志", WS_TABSTOP, idLog);
  refreshDisplays();
  hotkey_ = RegisterHotKey(window_, 1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F9) != FALSE;
  BOOL dark = TRUE; DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
  // Settings must remain visible at startup, including through remote/screen-sharing tools.
  SetWindowDisplayAffinity(window_, WDA_NONE);
  layout();
  SetTimer(window_, 1, 250, nullptr);
  update();
}

void Application::layout() {
  RECT client{}; GetClientRect(window_, &client);
  int width = MulDiv(client.right, 96, dpi_);
  int height = MulDiv(client.bottom, 96, dpi_);
  auto place = [&](HWND controlWindow, int x, int y, int w, int h) {
    MoveWindow(controlWindow, px(x), px(y), px(std::max(w, 10)), px(h), TRUE);
  };
  place(title_, 30, 22, width - 60, 48);
  place(description_, 32, 78, width - 64, 28);
  place(displayLabel_, 32, 130, 180, 24);
  place(display_, 32, 160, width - 64, 220);
  place(resolutionLabel_, 32, 214, 340, 24);
  place(resolution_, 32, 244, width - 260, 220);
  place(fpsLabel_, width - 200, 214, 160, 24);
  place(fps_, width - 200, 244, 168, 180);
  place(system_, 32, 300, 250, 30);
  place(mic_, 300, 300, 240, 30);
  place(cursor_, 32, 338, 230, 30);
  place(regionButton_, 32, 380, 150, 36);
  place(regionLabel_, 202, 386, width - 234, 48);
  place(startButton_, 32, 440, 180, 46);
  place(stopButton_, 230, 440, 180, 46);
  place(cancelButton_, 428, 440, 140, 46);
  place(stats_, 32, 500, width - 64, 30);
  place(status_, 32, 536, width - 64, std::max(60, height - 610));
  place(recoveryButton_, 32, height - 56, 232, 34);
  place(folderButton_, 280, height - 56, 160, 34);
  place(logButton_, 456, height - 56, 112, 34);
}

void Application::refreshDisplays() {
  displays_ = displays();
  SendMessageW(display_, CB_RESETCONTENT, 0, 0);
  for (const auto& display : displays_) {
    std::wstring text = (display.primary ? L"主显示器 · " : L"显示器 · ") + display.name + L"   " +
      std::to_wstring(display.bounds.right - display.bounds.left) + L" × " + std::to_wstring(display.bounds.bottom - display.bounds.top);
    SendMessageW(display_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
  }
  SendMessageW(display_, CB_SETCURSEL, 0, 0);
  region_.reset();
  refreshResolutions();
}

void Application::refreshResolutions() {
  auto index = static_cast<size_t>(SendMessageW(display_, CB_GETCURSEL, 0, 0));
  if (index >= displays_.size()) return;
  RECT area = region_.value_or(displays_[index].bounds);
  SendMessageW(resolution_, CB_RESETCONTENT, 0, 0);
  resolutions_.clear();
  for (int limit : {0, 2160, 1440, 1080, 720, 480}) {
    if (limit && area.bottom - area.top <= limit) continue;
    auto size = fitDimensions(area.right - area.left, area.bottom - area.top, limit);
    std::wstring text = (limit ? std::to_wstring(limit) + L"p · " : L"原始尺寸 · ") +
      std::to_wstring(size.width) + L" × " + std::to_wstring(size.height);
    SendMessageW(resolution_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    resolutions_.push_back(limit);
  }
  size_t selected = 0;
  for (size_t i = 0; i < resolutions_.size(); ++i) if (resolutions_[i] == 1080) selected = i;
  SendMessageW(resolution_, CB_SETCURSEL, selected, 0);
  std::wstring label = region_ ? L"区域 " + std::to_wstring(area.right - area.left) + L" × " +
    std::to_wstring(area.bottom - area.top) + L"   再次点击恢复全屏" : L"当前为整屏录制；也可框选任意区域";
  SetWindowTextW(regionLabel_, label.c_str());
}

void Application::select() {
  if (busy(recorder_.snapshot().state)) return;
  if (region_) { region_.reset(); refreshResolutions(); return; }
  ShowWindow(window_, SW_HIDE);
  try { region_ = selectRegion(instance_); }
  catch (...) { ShowWindow(window_, SW_SHOW); throw; }
  ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_);
  refreshResolutions();
}

void Application::start() {
  if (busy(recorder_.snapshot().state)) return;
  auto screen = static_cast<size_t>(SendMessageW(display_, CB_GETCURSEL, 0, 0));
  auto resolution = static_cast<size_t>(SendMessageW(resolution_, CB_GETCURSEL, 0, 0));
  auto rate = static_cast<size_t>(SendMessageW(fps_, CB_GETCURSEL, 0, 0));
  if (screen >= displays_.size() || resolution >= resolutions_.size() || rate >= 4)
    throw std::runtime_error("请选择有效的显示器、分辨率和帧率");
  auto output = chooseOutput(window_);
  if (!output) return;
  RecordingOptions options;
  options.region = region_.value_or(displays_[screen].bounds);
  options.maximumHeight = resolutions_[resolution];
  options.frameRate = std::array<int, 4>{60, 30, 24, 15}[rate];
  options.systemAudio = checked(system_); options.microphone = checked(mic_); options.cursor = checked(cursor_);
  options.output = *output;
  recorder_.start(options);
  update();
}

void Application::recover() {
  if (busy(recorder_.snapshot().state)) return;
  auto files = recoverableRecordings();
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1, L"导入旧版 WebM / MKV 文件…");
  AppendMenuW(menu, MF_STRING, 2, L"打开恢复文件夹");
  if (!files.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  auto count = std::min<size_t>(files.size(), 100);
  for (size_t i = 0; i < count; ++i) {
    std::wstring text = files[i].suggestedOutput.filename().wstring() + L"  (" +
      std::to_wstring(files[i].bytes / 1024 / 1024) + L" MB)";
    AppendMenuW(menu, MF_STRING, 1000 + i, text.c_str());
  }
  RECT anchor{}; GetWindowRect(recoveryButton_, &anchor);
  int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN,
    anchor.left, anchor.bottom, 0, window_, nullptr);
  DestroyMenu(menu);
  fs::path source, suggested;
  if (selected == 2) { openPath(sessionsDirectory()); return; }
  if (selected == 1) {
    std::array<wchar_t, 32768> buffer{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"录制文件 (*.mkv;*.webm;*.mp4)\0*.mkv;*.webm;*.mp4\0所有文件\0*.*\0\0";
    dialog.lpstrFile = buffer.data(); dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    source = buffer.data();
    suggested = source.parent_path() / (source.stem().wstring() + L"-recovered.mp4");
  } else if (selected >= 1000 && static_cast<size_t>(selected - 1000) < count) {
    const auto& file = files[static_cast<size_t>(selected - 1000)];
    source = file.source; suggested = file.suggestedOutput;
  } else return;
  auto output = chooseOutput(window_, suggested);
  if (!output) return;
  recorder_.recover(source, *output);
  update();
}

void Application::update() {
  auto state = recorder_.snapshot();
  bool active = busy(state.state);
  SetWindowDisplayAffinity(window_, !allowCapture_ && state.state == State::Recording ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
  for (HWND controlWindow : {display_, resolution_, fps_, system_, mic_, cursor_, regionButton_, startButton_, recoveryButton_})
    EnableWindow(controlWindow, !active);
  EnableWindow(stopButton_, state.state == State::Starting || state.state == State::Recording);
  EnableWindow(cancelButton_, state.state == State::Exporting);
  EnableWindow(folderButton_, !state.output.empty());
  EnableWindow(logButton_, !state.log.empty());
  auto statusText = state.message;
  if (!hotkey_ && !active) statusText += L"\r\n全局停止快捷键被其他程序占用，可使用停止按钮。";
  SetWindowTextW(status_, statusText.c_str());
  wchar_t size[100]{}; swprintf_s(size, L"   |   %.1f MB   |   %llu 帧", state.bytes / 1048576.0, state.frames);
  auto text = formatDuration(state.elapsedSeconds) + size;
  if (state.state == State::Exporting) text += L"   |   已处理 " + formatDuration(state.exportedSeconds);
  SetWindowTextW(stats_, text.c_str());
  showIndicator(state.state == State::Recording);
  if (indicatorLabel_) SetWindowTextW(indicatorLabel_, (L"● REC  " + formatDuration(state.elapsedSeconds)).c_str());
  if (closePending_ && !active) DestroyWindow(window_);
}

void Application::showIndicator(bool show) {
  if (!show) {
    if (indicator_) DestroyWindow(indicator_);
    indicator_ = nullptr; indicatorLabel_ = nullptr;
    return;
  }
  if (indicator_) return;
  auto screen = static_cast<size_t>(SendMessageW(display_, CB_GETCURSEL, 0, 0));
  RECT bounds = screen < displays_.size() ? displays_[screen].bounds : RECT{0, 0, 800, 600};
  indicator_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"RecDeskIndicator", L"录制中",
    WS_POPUP | WS_BORDER, bounds.right - px(310), bounds.top + px(20), px(290), px(48), window_, nullptr, instance_, this);
  if (!indicator_) return;
  if (!allowCapture_) SetWindowDisplayAffinity(indicator_, WDA_EXCLUDEFROMCAPTURE);
  indicatorLabel_ = CreateWindowW(L"STATIC", L"● REC", WS_CHILD | WS_VISIBLE,
    px(12), px(14), px(184), px(26), indicator_, nullptr, instance_, nullptr);
  HWND stop = CreateWindowW(L"BUTTON", L"停止", WS_CHILD | WS_VISIBLE,
    px(206), px(7), px(72), px(32), indicator_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(idIndicatorStop)), instance_, nullptr);
  SendMessageW(indicatorLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  SendMessageW(stop, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  ShowWindow(indicator_, SW_SHOWNOACTIVATE);
}

LRESULT CALLBACK Application::indicatorProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  if (app) {
    if (message == WM_COMMAND && LOWORD(wParam) == idIndicatorStop) { app->recorder_.requestStop(); return 0; }
    if (message == WM_CLOSE) { app->recorder_.requestStop(); return 0; }
    if (message == WM_NCHITTEST) {
      auto hit = DefWindowProcW(window, message, wParam, lParam);
      return hit == HTCLIENT ? HTCAPTION : hit;
    }
    if (message == WM_CTLCOLORSTATIC) {
      SetTextColor(reinterpret_cast<HDC>(wParam), RGB(255, 120, 130));
      SetBkColor(reinterpret_cast<HDC>(wParam), background);
      return reinterpret_cast<LRESULT>(app->brush_);
    }
    if (message == WM_ERASEBKGND) { RECT rect{}; GetClientRect(window, &rect); FillRect(reinterpret_cast<HDC>(wParam), &rect, app->brush_); return 1; }
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Application::dispatch(UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE: initialize(); return 0;
    case WM_SIZE: if (title_) layout(); return 0;
    case WM_GETMINMAXINFO: {
      auto info = reinterpret_cast<MINMAXINFO*>(lParam);
      info->ptMinTrackSize = {px(720), px(720)}; return 0;
    }
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wParam);
      const auto rect = reinterpret_cast<RECT*>(lParam);
      SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER);
      LOGFONTW font{};
      GetObjectW(font_, sizeof(font), &font);
      font.lfHeight = -MulDiv(11, dpi_, 72);
      HFONT replacement = CreateFontIndirectW(&font);
      EnumChildWindows(window_, [](HWND child, LPARAM value) -> BOOL { SendMessageW(child, WM_SETFONT, value, TRUE); return TRUE; }, reinterpret_cast<LPARAM>(replacement));
      if (indicator_) EnumChildWindows(indicator_, [](HWND child, LPARAM value) -> BOOL { SendMessageW(child, WM_SETFONT, value, TRUE); return TRUE; }, reinterpret_cast<LPARAM>(replacement));
      DeleteObject(font_); font_ = replacement;
      GetObjectW(titleFont_, sizeof(font), &font); font.lfHeight = -MulDiv(24, dpi_, 72);
      replacement = CreateFontIndirectW(&font);
      SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
      DeleteObject(titleFont_); titleFont_ = replacement;
      layout(); return 0;
    }
    case WM_DISPLAYCHANGE: if (!busy(recorder_.snapshot().state)) refreshDisplays(); return 0;
    case WM_TIMER: update(); return 0;
    case WM_HOTKEY: recorder_.requestStop(); return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case idStart: start(); break;
        case idStop: recorder_.requestStop(); break;
        case idCancel: recorder_.cancelExport(); break;
        case idRegion: select(); break;
        case idRecovery: recover(); break;
        case idFolder: openPath(recorder_.snapshot().output.parent_path()); break;
        case idLog: openPath(recorder_.snapshot().log); break;
        case idDisplay: if (HIWORD(wParam) == CBN_SELCHANGE) { region_.reset(); refreshResolutions(); } break;
      }
      return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      SetTextColor(reinterpret_cast<HDC>(wParam), foreground);
      SetBkColor(reinterpret_cast<HDC>(wParam), background);
      return reinterpret_cast<LRESULT>(brush_);
    case WM_ERASEBKGND: { RECT rect{}; GetClientRect(window_, &rect); FillRect(reinterpret_cast<HDC>(wParam), &rect, brush_); return 1; }
    case WM_CLOSE:
      if (busy(recorder_.snapshot().state)) {
        if (MessageBoxW(window_, L"录制或导出仍在进行。是否停止并在保存完成后退出？", L"RecDesk", MB_YESNO | MB_ICONQUESTION) == IDYES) {
          closePending_ = true; recorder_.requestStop();
        }
      } else DestroyWindow(window_);
      return 0;
    case WM_DESTROY:
      KillTimer(window_, 1);
      if (hotkey_) UnregisterHotKey(window_, 1);
      PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(window_, message, wParam, lParam);
}

LRESULT CALLBACK Application::procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
    app->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  if (!app) return DefWindowProcW(window, message, wParam, lParam);
  try { return app->dispatch(message, wParam, lParam); }
  catch (const std::exception& error) {
    MessageBoxW(window, wide(error.what()).c_str(), L"RecDesk", MB_OK | MB_ICONERROR);
    return message == WM_CREATE ? -1 : 0;
  }
}

int Application::run(int show) {
  WNDCLASSW type{};
  type.lpfnWndProc = procedure; type.hInstance = instance_; type.lpszClassName = L"RecDeskNative";
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    throw std::runtime_error("无法注册设置窗口: " + utf8(errorMessage()));
  type.lpfnWndProc = indicatorProcedure; type.lpszClassName = L"RecDeskIndicator";
  if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    throw std::runtime_error("无法注册录制指示窗口: " + utf8(errorMessage()));
  UINT dpi = GetDpiForSystem();
  RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int width = std::min(MulDiv(800, dpi, 96), static_cast<int>(work.right - work.left - 20));
  int height = std::min(MulDiv(840, dpi, 96), static_cast<int>(work.bottom - work.top - 20));
  int x = work.left + (work.right - work.left - width) / 2;
  int y = work.top + (work.bottom - work.top - height) / 2;
  HWND window = CreateWindowExW(0, L"RecDeskNative", L"RecDesk · C++ 原生录屏", WS_OVERLAPPEDWINDOW,
    x, y, width, height, nullptr, nullptr, instance_, this);
  if (!window) throw std::runtime_error("无法创建设置窗口: " + utf8(errorMessage()));
  ShowWindow(window, show == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
  // A launcher may supply SW_HIDE or SW_SHOWMINIMIZED on the first ShowWindow call.
  restoreSettingsWindow(window);
  UpdateWindow(window);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  return static_cast<int>(message.wParam);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  try {
    auto mutexName = instanceMutexName();
    Handle singleInstance(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
    if (!singleInstance) throw std::runtime_error("无法创建程序实例锁");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      HWND existing = FindWindowW(L"RecDeskNative", nullptr);
      if (existing) restoreSettingsWindow(existing);
      return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Common dialogs use STA; audio and encoder workers initialize their own MTA apartments.
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    checkHr(com, "界面 COM 初始化失败");
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    bool allowCapture = true;
    if (arguments) {
      for (int i = 1; i < argumentCount; ++i) {
        if (std::wstring(arguments[i]) == L"--exclude-controls") allowCapture = false;
        if (std::wstring(arguments[i]) == L"--allow-capture") allowCapture = true;
      }
      LocalFree(arguments);
    }
    Application app(instance, allowCapture);
    int result = app.run(show);
    CoUninitialize();
    return result;
  } catch (const std::exception& error) {
    MessageBoxW(nullptr, wide(error.what()).c_str(), L"RecDesk 启动失败", MB_OK | MB_ICONERROR);
    return 1;
  }
}
