#include "platform.h"

#include <array>
#include <iostream>

using namespace recdesk;

namespace {

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class GuiChild {
public:
  explicit GuiChild(const std::filesystem::path& executable) {
    job_.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(job_ && SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Create GUI test job");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    auto command = commandLine(executable, {});
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
      nullptr, nullptr, &startup, &process) != FALSE, "Launch GUI with hidden startup hint");
    process_.reset(process.hProcess);
    Handle thread(process.hThread);
    if (!AssignProcessToJobObject(job_.get(), process_.get())) {
      TerminateProcess(process_.get(), 1);
      throw std::runtime_error("Assign GUI test job");
    }
    require(ResumeThread(thread.get()) != static_cast<DWORD>(-1), "Resume GUI process");
  }
  DWORD id() const { return GetProcessId(process_.get()); }
  bool wait(DWORD timeout) const { return WaitForSingleObject(process_.get(), timeout) == WAIT_OBJECT_0; }
private:
  Handle process_, job_;
};

HWND findSettings(DWORD processId) {
  struct Search { DWORD processId; HWND window = nullptr; } search{processId};
  EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
    auto& search = *reinterpret_cast<Search*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != search.processId) return TRUE;
    std::array<wchar_t, 128> type{};
    GetClassNameW(window, type.data(), static_cast<int>(type.size()));
    if (std::wstring(type.data()) != L"RecDeskNative") return TRUE;
    search.window = window;
    return FALSE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.window;
}

bool onScreen(HWND window) {
  RECT bounds{};
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (!GetWindowRect(window, &bounds) ||
      !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor)) return false;
  return bounds.left >= monitor.rcWork.left && bounds.top >= monitor.rcWork.top &&
    bounds.right <= monitor.rcWork.right && bounds.bottom <= monitor.rcWork.bottom;
}

}

int wmain() {
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FindWindowW(L"RecDeskNative", nullptr)) {
      std::cout << "SKIP: RecDesk is already running on this desktop\n";
      return 77;
    }
    auto executable = executableDirectory() / L"RecDesk.exe";
    GuiChild app(executable);
    HWND window = nullptr;
    auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
      window = findSettings(app.id());
      if (window && IsWindowVisible(window) && !IsIconic(window)) break;
      if (app.wait(0)) throw std::runtime_error("GUI exited without visible settings");
      Sleep(50);
    }
    require(window && IsWindowVisible(window) && !IsIconic(window), "Settings must show even when launcher requests SW_HIDE");
    DWORD affinity = 0;
    require(GetWindowDisplayAffinity(window, &affinity) && affinity == WDA_NONE, "Default settings must be visible to remote/screen capture tools");
    require(onScreen(window), "Startup settings must be inside a monitor work area");
    struct Controls { int combos = 0; bool resolution = false, frameRate = false, audio = false; } controls;
    EnumChildWindows(window, [](HWND child, LPARAM parameter) -> BOOL {
      auto& controls = *reinterpret_cast<Controls*>(parameter);
      std::array<wchar_t, 128> type{};
      GetClassNameW(child, type.data(), static_cast<int>(type.size()));
      if (_wcsicmp(type.data(), L"ComboBox") == 0 && IsWindowVisible(child)) ++controls.combos;
      std::array<wchar_t, 512> text{};
      DWORD_PTR result = 0;
      SendMessageTimeoutW(child, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()), SMTO_ABORTIFHUNG, 1000, &result);
      auto label = std::wstring(text.data());
      if (label.find(L"输出分辨率") != std::wstring::npos) controls.resolution = true;
      if (label == L"帧率") controls.frameRate = true;
      if (label.find(L"系统声音") != std::wstring::npos) controls.audio = true;
      return TRUE;
    }, reinterpret_cast<LPARAM>(&controls));
    require(controls.combos >= 3 && controls.resolution && controls.frameRate && controls.audio,
      "Display, resolution, FPS and audio parameter controls must exist");
    ShowWindow(window, SW_MINIMIZE);
    SetWindowPos(window, nullptr, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    GuiChild duplicate(executable);
    require(duplicate.wait(10000), "Second launch must activate existing instance and exit");
    require(IsWindowVisible(window) && !IsIconic(window) && onScreen(window), "Second launch must restore minimized/off-screen settings");
    DWORD_PTR ignored = 0;
    require(SendMessageTimeoutW(window, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000, &ignored) != 0, "Close idle settings window");
    require(app.wait(5000), "GUI must close cleanly");
    std::cout << "UI checks passed: default visible controls, remote capture, hidden launcher, off-screen restore, duplicate launch.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
