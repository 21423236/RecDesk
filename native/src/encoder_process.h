#pragma once

#include "platform.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace recdesk {

class Process {
public:
  ~Process();
  void start(const std::filesystem::path& executable, const std::vector<std::wstring>& args,
    const std::filesystem::path& logFile);
  bool running() const;
  bool wait(DWORD milliseconds) const;
  DWORD exitCode() const;
  void requestStop();
  void terminate();
  void finish();
  std::wstring diagnostic() const;
  std::int64_t mediaTimeUs() const { return mediaTimeUs_.load(); }
  std::int64_t lastProgress() const { return lastProgress_.load(); }
  std::int64_t lastFrameProgress() const { return lastFrameProgress_.load(); }
  std::uint64_t frames() const { return frames_.load(); }
private:
  void readOutput(const std::filesystem::path& logFile);
  Handle process_, job_, input_, output_;
  std::thread reader_;
  mutable std::mutex mutex_;
  std::string tail_;
  std::atomic<std::int64_t> mediaTimeUs_{0}, lastProgress_{0}, lastFrameProgress_{0};
  std::atomic<std::uint64_t> frames_{0};
};

}
