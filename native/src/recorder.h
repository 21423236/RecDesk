#pragma once

#include "audio.h"
#include "encoder_process.h"
#include <memory>

namespace recdesk {

enum class State { Idle, Starting, Recording, Stopping, Exporting, Completed, Failed };

struct RecordingOptions {
  RECT region{};
  int maximumHeight = 1080;
  int frameRate = 30;
  bool systemAudio = true;
  bool microphone = false;
  bool cursor = true;
  std::filesystem::path output;
};

struct Snapshot {
  State state = State::Idle;
  std::wstring message = L"准备就绪";
  std::filesystem::path source, output, log;
  std::int64_t elapsedSeconds = 0;
  std::uint64_t bytes = 0;
  std::uint64_t frames = 0;
  std::int64_t exportedSeconds = 0;
};

struct Recovery {
  std::filesystem::path source, suggestedOutput;
  std::uint64_t bytes = 0;
};

std::vector<std::wstring> recordingArguments(const RecordingOptions& options, const std::wstring& audioPipe,
  const std::filesystem::path& source);
std::vector<std::wstring> exportArguments(const std::filesystem::path& source, const std::filesystem::path& destination);
std::vector<Recovery> recoverableRecordings();
bool busy(State state);

class Recorder {
public:
  ~Recorder();
  void start(const RecordingOptions& options);
  void recover(const std::filesystem::path& source, const std::filesystem::path& destination);
  void requestStop();
  void cancelExport();
  Snapshot snapshot() const;
private:
  void record(RecordingOptions options);
  void exportFile(const std::filesystem::path& source, const std::filesystem::path& destination,
    const std::filesystem::path& log);
  void setState(State state, const std::wstring& message);
  void fail(const std::exception& error);
  void prepare();
  mutable std::mutex mutex_;
  Snapshot snapshot_;
  std::atomic<bool> stop_{false}, cancel_{false};
  std::thread worker_;
};

}
