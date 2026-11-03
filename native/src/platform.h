#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace recdesk {

class Handle {
public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  ~Handle() { reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.release()) {}
  Handle& operator=(Handle&& other) noexcept { reset(other.release()); return *this; }
  HANDLE get() const { return value_; }
  explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
  HANDLE release() { auto result = value_; value_ = nullptr; return result; }
  void reset(HANDLE value = nullptr) { if (*this) CloseHandle(value_); value_ = value; }
private:
  HANDLE value_ = nullptr;
};

class ComScope {
public:
  ComScope();
  ~ComScope();
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
};

struct Display {
  std::wstring name;
  RECT bounds{};
  bool primary = false;
};

struct Dimensions { int width; int height; };

std::string utf8(const std::wstring& text);
std::wstring wide(const std::string& text);
std::wstring errorMessage(DWORD code = GetLastError());
void checkHr(HRESULT result, const char* operation);
std::wstring quoteArgument(const std::wstring& argument);
std::wstring commandLine(const std::filesystem::path& executable, const std::vector<std::wstring>& args);
std::filesystem::path executableDirectory();
std::filesystem::path sessionsDirectory();
std::filesystem::path findFfmpeg();
std::wstring uniqueId();
std::int64_t clock100ns();
std::vector<Display> displays();
Dimensions fitDimensions(int width, int height, int maximumHeight);
std::wstring formatDuration(std::int64_t seconds);
void requireFreeSpace(const std::filesystem::path& directory, std::uint64_t minimumBytes);

}
