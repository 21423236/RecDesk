#include "recorder.h"
#include <fstream>
#include <iostream>

using namespace recdesk;
namespace fs = std::filesystem;

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Snapshot await(Recorder& recorder) {
  auto started = clock100ns();
  while (busy(recorder.snapshot().state)) {
    if (clock100ns() - started > 200000000) {
      recorder.cancelExport(); throw std::runtime_error("Export test timeout");
    }
    Sleep(10);
  }
  return recorder.snapshot();
}
std::string read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

int wmain() {
  fs::path temporary;
  try {
    ComScope com;
    fs::path ffmpeg;
    try { ffmpeg = findFfmpeg(); }
    catch (const std::exception&) { std::cout << "SKIP: FFmpeg not installed\n"; return 77; }
    temporary = fs::temp_directory_path() / (L"recdesk-export-tests-" + uniqueId());
    fs::create_directory(temporary);
    auto source = temporary / L"合成 测试.mkv";
    {
      Process generator;
      generator.start(ffmpeg, {L"-hide_banner", L"-loglevel", L"error", L"-f", L"lavfi", L"-i", L"testsrc2=size=160x90:rate=15",
        L"-f", L"lavfi", L"-i", L"sine=frequency=440:sample_rate=48000", L"-t", L"2", L"-c:v", L"libx264",
        L"-preset", L"ultrafast", L"-c:a", L"aac", source.wstring()}, temporary / L"generate.log");
      require(generator.wait(10000), "Synthetic generation timeout"); generator.finish();
      require(generator.exitCode() == 0, "Synthetic generation failed");
    }
    auto destination = temporary / L"导出 文件.mp4";
    Recorder recorder;
    recorder.recover(source, destination);
    auto state = await(recorder);
    require(state.state == State::Completed && fs::file_size(destination) > 512, "MP4 remux failed");
    require(fs::is_regular_file(source), "Imported source must survive successful export");
    {
      Process decoder;
      decoder.start(ffmpeg, {L"-v", L"error", L"-i", destination.wstring(), L"-f", L"null", L"-"}, temporary / L"decode.log");
      require(decoder.wait(10000), "Decode timeout"); decoder.finish();
      require(decoder.exitCode() == 0 && read(temporary / L"decode.log").empty(), "Export must fully decode without errors");
    }
    auto broken = temporary / L"broken.mkv";
    { std::ofstream output(broken); output << "not a recording"; }
    auto originalOutput = read(destination);
    recorder.recover(broken, destination);
    state = await(recorder);
    require(state.state == State::Failed, "Broken input must fail visibly");
    require(fs::is_regular_file(broken) && read(destination) == originalOutput, "Failed export must preserve source and existing output");
    recorder.recover(source, temporary / L"cancelled.mp4");
    recorder.cancelExport();
    state = await(recorder);
    require(state.state == State::Failed && fs::is_regular_file(source) && !fs::exists(temporary / L"cancelled.mp4"),
      "Cancellation must preserve source and not publish partial MP4");
    recorder.recover(source, source);
    require(await(recorder).state == State::Failed && fs::is_regular_file(source), "Reject overwriting input");
    for (const auto& file : fs::directory_iterator(temporary))
      require(file.path().filename().wstring().find(L".partial.mp4") == std::wstring::npos, "No abandoned partial exports");
    fs::remove_all(temporary);
    std::cout << "Export checks passed (synthetic media only; no screen or microphone capture).\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    if (!temporary.empty()) { std::error_code ignored; fs::remove_all(temporary, ignored); }
    return 1;
  }
}
