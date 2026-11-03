#include "recorder.h"
#include <chrono>
#include <iostream>
#include <limits>
#include <sstream>

using namespace recdesk;
namespace fs = std::filesystem;

int wmain(int argc, wchar_t** argv) {
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ComScope com;
    RecordingOptions options;
    auto screens = displays();
    if (screens.empty()) throw std::runtime_error("No interactive desktop");
    options.region = screens.front().bounds;
    options.maximumHeight = 480;
    options.frameRate = 15;
    options.systemAudio = false;
    int seconds = 5;
    fs::path recover;
    bool cancel = false;
    for (int i = 1; i < argc; ++i) {
      std::wstring argument = argv[i];
      if (argument == L"--cancel-export") { cancel = true; continue; }
      if (i + 1 >= argc) throw std::runtime_error("Missing option value");
      std::wstring value = argv[++i];
      if (argument == L"--output") options.output = value;
      else if (argument == L"--seconds") seconds = std::stoi(value);
      else if (argument == L"--height") options.maximumHeight = std::stoi(value);
      else if (argument == L"--fps") options.frameRate = std::stoi(value);
      else if (argument == L"--region") {
        std::wistringstream input(value);
        std::int64_t x = 0, y = 0, width = 0, height = 0;
        wchar_t a = 0, b = 0, c = 0;
        if (!(input >> x >> a >> y >> b >> width >> c >> height) || a != L',' || b != L',' || c != L',' ||
            width < 2 || height < 2 || x < std::numeric_limits<LONG>::min() || y < std::numeric_limits<LONG>::min() ||
            x > std::numeric_limits<LONG>::max() || y > std::numeric_limits<LONG>::max() ||
            width > std::numeric_limits<LONG>::max() || height > std::numeric_limits<LONG>::max() ||
            x + width > std::numeric_limits<LONG>::max() || y + height > std::numeric_limits<LONG>::max())
          throw std::runtime_error("Use --region x,y,width,height in physical pixels");
        options.region = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
      }
      else if (argument == L"--audio") {
        if (value != L"none" && value != L"system" && value != L"mic" && value != L"both") throw std::runtime_error("Invalid audio mode");
        options.systemAudio = value == L"system" || value == L"both";
        options.microphone = value == L"mic" || value == L"both";
      } else if (argument == L"--recover") recover = value;
      else throw std::runtime_error("Unknown option");
    }
    if (options.output.empty()) throw std::runtime_error("Use --output <path.mp4>; this tool records the live desktop");
    Recorder recorder;
    if (recover.empty()) recorder.start(options);
    else recorder.recover(recover, options.output);
    auto started = clock100ns();
    State last = State::Idle;
    bool requested = false;
    for (;;) {
      auto state = recorder.snapshot();
      if (last != state.state) {
        std::cout << "state=" << static_cast<int>(state.state) << " " << utf8(state.message) << '\n';
        last = state.state;
      }
      if (!requested && state.state == State::Recording && state.elapsedSeconds >= seconds) {
        recorder.requestStop(); requested = true;
      }
      if (cancel && state.state == State::Exporting) recorder.cancelExport();
      if (state.state == State::Completed) {
        std::cout << "output=" << utf8(options.output.wstring()) << '\n'; return 0;
      }
      if (state.state == State::Failed) {
        std::cerr << "source=" << utf8(state.source.wstring()) << "\nlog=" << utf8(state.log.wstring()) << '\n';
        return 1;
      }
      if (clock100ns() - started > static_cast<std::int64_t>(seconds + 120) * 10000000) {
        recorder.requestStop(); recorder.cancelExport(); throw std::runtime_error("Smoke test timed out");
      }
      Sleep(50);
    }
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
