#include "platform.h"

#include <shlobj.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace recdesk {

ComScope::ComScope() { checkHr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM 初始化失败"); }
ComScope::~ComScope() { CoUninitialize(); }

std::string utf8(const std::wstring& text) {
  if (text.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
  return result;
}

std::wstring wide(const std::string& text) {
  if (text.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::wstring errorMessage(DWORD code) {
  wchar_t* buffer = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring result = buffer ? buffer : L"未知系统错误";
  if (buffer) LocalFree(buffer);
  return result + L" (" + std::to_wstring(code) + L")";
}

void checkHr(HRESULT result, const char* operation) {
  if (FAILED(result)) throw std::runtime_error(std::string(operation) + ": " + utf8(errorMessage(static_cast<DWORD>(result))));
}

std::wstring quoteArgument(const std::wstring& argument) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t ch : argument) {
    if (ch == L'\\') { ++slashes; continue; }
    if (ch == L'"') result.append(slashes * 2 + 1, L'\\');
    else result.append(slashes, L'\\');
    slashes = 0;
    result += ch;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}

std::wstring commandLine(const fs::path& executable, const std::vector<std::wstring>& args) {
  std::wstring result = quoteArgument(executable.wstring());
  for (const auto& arg : args) result += L" " + quoteArgument(arg);
  return result;
}

fs::path executableDirectory() {
  std::wstring buffer(32768, L'\0');
  DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (!size || size >= buffer.size()) throw std::runtime_error("无法获取程序目录");
  buffer.resize(size);
  return fs::path(buffer).parent_path();
}

fs::path sessionsDirectory() {
  PWSTR raw = nullptr;
  checkHr(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw), "无法获取数据目录");
  fs::path directory = fs::path(raw) / L"RecDeskNative" / L"Sessions";
  CoTaskMemFree(raw);
  fs::create_directories(directory);
  return directory;
}

fs::path findFfmpeg() {
  auto local = executableDirectory() / L"ffmpeg.exe";
  if (fs::is_regular_file(local)) return local;
  wchar_t buffer[32768]{};
  DWORD size = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, 32768, buffer, nullptr);
  if (size && size < 32768) return fs::path(buffer);
  throw std::runtime_error("未找到 FFmpeg。请将 ffmpeg.exe 放到 RecDesk.exe 同目录，或加入 PATH。");
}

std::wstring uniqueId() {
  GUID guid{};
  checkHr(CoCreateGuid(&guid), "无法创建录制编号");
  wchar_t text[40]{};
  StringFromGUID2(guid, text, 40);
  return std::wstring(text + 1, 36);
}

std::int64_t clock100ns() {
  LARGE_INTEGER value{}, frequency{};
  QueryPerformanceCounter(&value);
  QueryPerformanceFrequency(&frequency);
  return value.QuadPart / frequency.QuadPart * 10000000 +
    value.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart;
}

std::vector<Display> displays() {
  std::vector<Display> result;
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
    auto& list = *reinterpret_cast<std::vector<Display>*>(parameter);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) list.push_back({info.szDevice, info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
  }, reinterpret_cast<LPARAM>(&result));
  std::stable_sort(result.begin(), result.end(), [](const Display& a, const Display& b) { return a.primary && !b.primary; });
  return result;
}

Dimensions fitDimensions(int width, int height, int maximumHeight) {
  if (width < 2 || height < 2) throw std::runtime_error("录制区域至少需要 2×2 像素");
  double scale = maximumHeight > 0 ? std::min(1.0, static_cast<double>(maximumHeight) / height) : 1.0;
  return {std::max(2, static_cast<int>(width * scale) / 2 * 2), std::max(2, static_cast<int>(height * scale) / 2 * 2)};
}

std::wstring formatDuration(std::int64_t seconds) {
  std::wostringstream output;
  output << std::setfill(L'0') << std::setw(2) << seconds / 3600 << L":"
    << std::setw(2) << seconds / 60 % 60 << L":" << std::setw(2) << seconds % 60;
  return output.str();
}

void requireFreeSpace(const fs::path& directory, std::uint64_t minimumBytes) {
  ULARGE_INTEGER available{};
  if (!GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr))
    throw std::runtime_error("无法检查磁盘空间: " + utf8(errorMessage()));
  if (available.QuadPart < minimumBytes) throw std::runtime_error("磁盘剩余空间不足，请释放空间后重试。录制源文件将保留。");
}

}
