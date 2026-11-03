#include "recorder.h"
#include <shellapi.h>
#include <array>
#include <algorithm>
#include <fstream>
#include <iostream>

using namespace recdesk;
namespace fs = std::filesystem;

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool contains(const std::vector<std::wstring>& args, const std::wstring& value) {
  return std::find(args.begin(), args.end(), value) != args.end();
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc > 1 && std::wstring(argv[1]) == L"--child-progress") {
    std::cout << "frame=42\nout_time_us=12345678\nprogress=end\n";
    return 0;
  }
  if (argc > 1 && std::wstring(argv[1]) == L"--child-stop") {
    std::string input;
    std::getline(std::cin, input);
    return input == "q" ? 0 : 2;
  }
  if (argc > 1 && std::wstring(argv[1]) == L"--child-wait") { Sleep(30000); return 0; }
  fs::path temporary;
  try {
    ComScope com;
    for (const std::wstring argument : {L"", L"simple", L"C:\\录制 文件\\", L"a\"b", L"\\\\server\\share\\", L"x\\\"y\\"}) {
      int count = 0;
      auto command = L"app " + quoteArgument(argument);
      LPWSTR* parsed = CommandLineToArgvW(command.c_str(), &count);
      require(parsed && count == 2, "argument count");
      bool equal = parsed[1] == argument;
      LocalFree(parsed);
      require(equal, "Windows argument quoting round trip");
    }
    auto size = fitDimensions(3840, 2160, 1080);
    require(size.width == 1920 && size.height == 1080, "4K downscale");
    size = fitDimensions(1365, 767, 1080);
    require(size.width == 1364 && size.height == 766, "even dimensions without upscaling");
    size = fitDimensions(2160, 3840, 1080);
    require(size.width == 606 && size.height == 1080, "portrait aspect ratio");
    bool invalid = false;
    try { fitDimensions(1, 100, 0); } catch (const std::exception&) { invalid = true; }
    require(invalid, "reject tiny regions");
    require(formatDuration(360001) == L"100:00:01", "recordings longer than 24 hours");
    RecordingOptions options;
    options.region = {-1920, -100, -1, 979};
    auto args = recordingArguments(options, L"\\\\.\\pipe\\test", L"source.mkv");
    require(contains(args, L"-1920") && contains(args, L"-100"), "negative multi-monitor coordinates");
    require(contains(args, L"scale=1918:1078:flags=fast_bilinear"), "even output scale");
    require(contains(args, L"f32le") && contains(args, L"aac"), "streaming native audio encoding");
    require(contains(args, L"matroska") && contains(args, L"-cluster_time_limit"), "recoverable on-disk container");
    args = recordingArguments(options, L"", L"source.mkv");
    require(!contains(args, L"f32le") && !contains(args, L"aac"), "silent recording has no audio pipe");
    args = exportArguments(L"source.mkv", L"result.partial.mp4");
    require(contains(args, L"copy") && !contains(args, L"libx264"), "native export must not reencode");
    require(!contains(args, L"+faststart"), "no final whole-file faststart rewrite");
    args = exportArguments(L"source.WEBM", L"result.partial.mp4");
    require(contains(args, L"libx264") && contains(args, L"aac"), "legacy WebM recovery transcodes");
    require(busy(State::Exporting) && !busy(State::Completed) && !busy(State::Failed), "UI state transitions");
    temporary = fs::temp_directory_path() / (L"recdesk-tests-" + uniqueId());
    fs::create_directory(temporary);
    wchar_t self[32768]{};
    GetModuleFileNameW(nullptr, self, 32768);
    {
      Process child;
      child.start(self, {L"--child-progress"}, temporary / L"progress.log");
      require(child.wait(5000), "progress process timeout");
      child.finish();
      require(child.exitCode() == 0 && child.frames() == 42 && child.mediaTimeUs() == 12345678, "drained progress parser");
    }
    {
      Process child;
      child.start(self, {L"--child-stop"}, temporary / L"stop.log");
      child.requestStop();
      require(child.wait(5000) && child.exitCode() == 0, "graceful stdin stop");
      child.finish();
    }
    {
      Process child;
      child.start(self, {L"--child-wait"}, temporary / L"terminate.log");
      child.terminate();
      require(child.wait(1000) && child.exitCode() != 0, "bounded forced stop");
      child.finish();
    }
    {
      AudioPipe audio(true, false);
      // Stopping an unstarted pipe must be safe and idempotent.
      audio.stop(); audio.stop();
    }
    fs::remove_all(temporary);
    std::cout << "All core checks passed (quoting, geometry, commands, progress, stop, timeout).\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    if (!temporary.empty()) { std::error_code ignored; fs::remove_all(temporary, ignored); }
    return 1;
  }
}
