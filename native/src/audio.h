#pragma once

#include "platform.h"
#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace recdesk {

// Fixed-size audio timeline + cancellable pipe: never an unbounded recording buffer.
class AudioPipe {
public:
  AudioPipe(bool systemAudio, bool microphone);
  ~AudioPipe();
  void start();
  void synchronize(std::int64_t epoch100ns) { epoch_ = epoch100ns; }
  void stop();
  const std::wstring& name() const { return name_; }
  std::wstring error() const;
private:
  void run(std::promise<void> ready);
  bool completeIo(OVERLAPPED& operation, DWORD timeout, DWORD& transferred);
  bool connect();
  bool write(const void* bytes, DWORD size);
  bool systemAudio_, microphone_;
  std::wstring name_;
  Handle pipe_, stopEvent_;
  std::thread thread_;
  std::atomic<std::int64_t> epoch_{0};
  mutable std::mutex mutex_;
  std::wstring error_;
};

}
