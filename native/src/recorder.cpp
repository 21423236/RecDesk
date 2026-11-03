#include "recorder.h"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace recdesk {
namespace {

std::uint64_t fileSize(const fs::path& file) {
  std::error_code error;
  auto size = fs::file_size(file, error);
  return error ? 0 : size;
}

bool legacyWebm(const fs::path& file) {
  auto extension = file.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return extension == L".webm";
}

void validateOptions(const RecordingOptions& options) {
  if (options.output.empty() || options.output.extension() != L".mp4") throw std::runtime_error("请选择 MP4 输出路径");
  if (options.frameRate != 15 && options.frameRate != 24 && options.frameRate != 30 && options.frameRate != 60)
    throw std::runtime_error("不支持的录制帧率");
  RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
  desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  RECT intersection{};
  if (!IntersectRect(&intersection, &options.region, &desktop) || !EqualRect(&intersection, &options.region))
    throw std::runtime_error("录制区域已超出桌面，请重新选择显示器或区域");
  fitDimensions(options.region.right - options.region.left, options.region.bottom - options.region.top, options.maximumHeight);
}

}

bool busy(State state) {
  return state == State::Starting || state == State::Recording || state == State::Stopping || state == State::Exporting;
}

std::vector<std::wstring> recordingArguments(const RecordingOptions& options, const std::wstring& audioPipe, const fs::path& source) {
  int width = options.region.right - options.region.left;
  int height = options.region.bottom - options.region.top;
  auto output = fitDimensions(width, height, options.maximumHeight);
  std::vector<std::wstring> args{
    L"-hide_banner", L"-loglevel", L"warning", L"-nostats", L"-stats_period", L"0.5", L"-progress", L"pipe:1", L"-y",
    L"-thread_queue_size", L"8", L"-f", L"gdigrab", L"-draw_mouse", options.cursor ? L"1" : L"0",
    L"-framerate", std::to_wstring(options.frameRate), L"-offset_x", std::to_wstring(options.region.left),
    L"-offset_y", std::to_wstring(options.region.top), L"-video_size", std::to_wstring(width) + L"x" + std::to_wstring(height),
    L"-i", L"desktop"
  };
  if (!audioPipe.empty()) {
    std::vector<std::wstring> audio{L"-thread_queue_size", L"64", L"-f", L"f32le", L"-ar", L"48000", L"-ac", L"2",
      L"-probesize", L"32", L"-analyzeduration", L"0", L"-i", audioPipe};
    args.insert(args.end(), audio.begin(), audio.end());
  }
  std::vector<std::wstring> video{L"-map", L"0:v:0", L"-vf", L"scale=" + std::to_wstring(output.width) + L":" +
    std::to_wstring(output.height) + L":flags=fast_bilinear", L"-c:v", L"libx264", L"-preset", L"ultrafast",
    L"-tune", L"zerolatency", L"-crf", L"23", L"-pix_fmt", L"yuv420p", L"-threads", L"4",
    L"-g", std::to_wstring(options.frameRate * 2)};
  args.insert(args.end(), video.begin(), video.end());
  if (!audioPipe.empty()) {
    std::vector<std::wstring> audio{L"-map", L"1:a:0", L"-af", L"aresample=async=1:first_pts=0",
      L"-c:a", L"aac", L"-b:a", L"160k", L"-ar", L"48000"};
    args.insert(args.end(), audio.begin(), audio.end());
  }
  // Matroska remains recoverable without a final MP4 moov atom, even on process failure.
  std::vector<std::wstring> mux{L"-max_muxing_queue_size", L"256", L"-flush_packets", L"1",
    L"-cluster_time_limit", L"1000", L"-f", L"matroska", source.wstring()};
  args.insert(args.end(), mux.begin(), mux.end());
  return args;
}

std::vector<std::wstring> exportArguments(const fs::path& source, const fs::path& destination) {
  std::vector<std::wstring> args{L"-hide_banner", L"-loglevel", L"warning", L"-nostats", L"-stats_period", L"0.5",
    L"-progress", L"pipe:1", L"-y", L"-i", source.wstring(), L"-map", L"0:v:0", L"-map", L"0:a:0?"};
  // Only legacy Electron WebM recordings need video transcoding.
  std::vector<std::wstring> codec = legacyWebm(source) ? std::vector<std::wstring>{
    L"-vf", L"scale=trunc(iw/2)*2:trunc(ih/2)*2", L"-c:v", L"libx264", L"-preset", L"ultrafast",
    L"-pix_fmt", L"yuv420p", L"-threads", L"4", L"-c:a", L"aac", L"-b:a", L"160k"
  } : std::vector<std::wstring>{L"-c", L"copy"};
  args.insert(args.end(), codec.begin(), codec.end());
  // No +faststart: avoid a second full-file rewrite at the end of a long recording.
  args.insert(args.end(), {L"-f", L"mp4", destination.wstring()});
  return args;
}

std::vector<Recovery> recoverableRecordings() {
  std::vector<Recovery> result;
  for (const auto& entry : fs::directory_iterator(sessionsDirectory())) {
    if (!entry.is_directory()) continue;
    auto source = entry.path() / L"recording.mkv";
    auto bytes = fileSize(source);
    if (!bytes) continue;
    std::ifstream marker(entry.path() / L"target.txt", std::ios::binary);
    std::string target((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
    result.push_back({source, fs::path(wide(target)), bytes});
  }
  std::sort(result.begin(), result.end(), [](const Recovery& a, const Recovery& b) { return a.source > b.source; });
  return result;
}

Recorder::~Recorder() {
  stop_ = true;
  cancel_ = true;
  if (worker_.joinable()) worker_.join();
}

Snapshot Recorder::snapshot() const { std::lock_guard lock(mutex_); return snapshot_; }
void Recorder::setState(State state, const std::wstring& message) {
  std::lock_guard lock(mutex_);
  snapshot_.state = state;
  snapshot_.message = message;
}
void Recorder::fail(const std::exception& error) {
  auto source = snapshot().source;
  auto suffix = !source.empty() && fileSize(source) > 0 ?
    L"\r\n未删除录制源文件，可在“恢复录制”中重试。" : L"\r\n未生成可恢复的录制数据，请检查设置和日志后重试。";
  setState(State::Failed, wide(error.what()) + suffix);
}
void Recorder::prepare() {
  if (busy(snapshot().state)) throw std::runtime_error("请等待当前任务完成");
  if (worker_.joinable()) worker_.join();
  stop_ = false;
  cancel_ = false;
  std::lock_guard lock(mutex_);
  snapshot_ = {};
  snapshot_.state = State::Starting;
  snapshot_.message = L"正在初始化录制设备…";
}
void Recorder::start(const RecordingOptions& options) {
  validateOptions(options);
  prepare();
  worker_ = std::thread([this, options] {
    try { record(options); }
    catch (const std::exception& error) { fail(error); }
  });
}
void Recorder::requestStop() { stop_ = true; }
void Recorder::cancelExport() { cancel_ = true; }

void Recorder::record(RecordingOptions options) {
  ComScope com;
  auto ffmpeg = findFfmpeg();
  auto directory = sessionsDirectory() / uniqueId();
  requireFreeSpace(directory.parent_path(), 512ull * 1024 * 1024);
  requireFreeSpace(options.output.parent_path(), 512ull * 1024 * 1024);
  fs::create_directory(directory);
  auto source = directory / L"recording.mkv";
  auto log = directory / L"recording.log";
  {
    std::ofstream marker(directory / L"target.txt", std::ios::binary);
    marker << utf8(options.output.wstring());
    marker.close();
    if (!marker) throw std::runtime_error("无法写入恢复信息，请检查磁盘权限");
  }
  {
    std::lock_guard lock(mutex_);
    snapshot_.source = source;
    snapshot_.output = options.output;
    snapshot_.log = log;
  }
  std::unique_ptr<AudioPipe> audio;
  if (options.systemAudio || options.microphone) {
    audio = std::make_unique<AudioPipe>(options.systemAudio, options.microphone);
    audio->start();
  }
  std::int64_t started = clock100ns();
  if (audio) audio->synchronize(started);
  Process encoder;
  encoder.start(ffmpeg, recordingArguments(options, audio ? audio->name() : L"", source), log);
  auto lastSpaceCheck = started;
  std::wstring recordingFailure;
  while (!stop_) {
    if (!encoder.running()) { recordingFailure = L"录制进程提前退出：\r\n" + encoder.diagnostic(); break; }
    if (audio && !audio->error().empty()) { recordingFailure = audio->error(); break; }
    auto now = clock100ns();
    if (now - encoder.lastFrameProgress() > 300000000) {
      recordingFailure = L"编码器超过 30 秒没有输出，已终止等待。请降低分辨率或帧率，并查看录制日志。";
      break;
    }
    if (now - lastSpaceCheck > 20000000) {
      try { requireFreeSpace(directory, 256ull * 1024 * 1024); }
      catch (const std::exception& error) { recordingFailure = wide(error.what()); break; }
      lastSpaceCheck = now;
    }
    {
      std::lock_guard lock(mutex_);
      if (encoder.frames() > 0) {
        snapshot_.state = State::Recording;
        snapshot_.message = L"正在录制 · 实时编码落盘 · Ctrl+Shift+F9 停止";
      }
      snapshot_.elapsedSeconds = (now - started) / 10000000;
      snapshot_.bytes = fileSize(source);
      snapshot_.frames = encoder.frames();
    }
    std::this_thread::sleep_for(100ms);
  }
  setState(State::Stopping, L"正在停止采集并写入最后的数据…");
  encoder.requestStop();
  // Closing the cancellable audio pipe unblocks FFmpeg's input thread during shutdown.
  if (audio) audio->stop();
  if (!encoder.wait(15000)) {
    encoder.terminate();
    recordingFailure += L"\r\n编码器停止超时，已保留可恢复的 MKV 文件。";
  }
  encoder.finish();
  if (!recordingFailure.empty()) throw std::runtime_error(utf8(recordingFailure));
  if (encoder.exitCode() != 0) throw std::runtime_error("录制失败：\n" + utf8(encoder.diagnostic()));
  if (encoder.frames() == 0 || fileSize(source) < 512) throw std::runtime_error("没有采集到有效画面，请检查录制区域及 FFmpeg 日志");
  exportFile(source, options.output, directory / L"export.log");
}

void Recorder::exportFile(const fs::path& source, const fs::path& destination, const fs::path& log) {
  if (!fs::is_regular_file(source) || !fileSize(source)) throw std::runtime_error("恢复源文件不存在或为空");
  if (fs::exists(destination) && fs::equivalent(source, destination)) throw std::runtime_error("不能将源文件覆盖为输出文件");
  requireFreeSpace(destination.parent_path(), fileSize(source) + 128ull * 1024 * 1024);
  auto partial = destination.parent_path() / (destination.stem().wstring() + L"." + uniqueId() + L".partial.mp4");
  {
    std::lock_guard lock(mutex_);
    snapshot_.source = source;
    snapshot_.output = destination;
    snapshot_.log = log;
    snapshot_.state = State::Exporting;
    snapshot_.message = legacyWebm(source) ? L"正在恢复旧版 WebM：需重新编码，请耐心等待…" : L"正在封装 MP4（不重新编码视频）…";
  }
  try {
    Process exporter;
    exporter.start(findFfmpeg(), exportArguments(source, partial), log);
    auto lastSpaceCheck = clock100ns();
    while (!exporter.wait(100)) {
      if (cancel_) { exporter.terminate(); throw std::runtime_error("已取消导出，源文件已保留，可稍后重试"); }
      if (clock100ns() - exporter.lastProgress() > 600000000) {
        exporter.terminate();
        throw std::runtime_error("导出超过 60 秒没有进展，已停止等待并保留源文件");
      }
      if (clock100ns() - lastSpaceCheck > 20000000) {
        requireFreeSpace(destination.parent_path(), 64ull * 1024 * 1024);
        lastSpaceCheck = clock100ns();
      }
      std::lock_guard lock(mutex_);
      snapshot_.exportedSeconds = std::max<std::int64_t>(0, exporter.mediaTimeUs() / 1000000);
    }
    exporter.finish();
    if (exporter.exitCode() != 0 || fileSize(partial) < 512)
      throw std::runtime_error("MP4 导出失败：\n" + utf8(exporter.diagnostic()));
    if (cancel_) throw std::runtime_error("已取消导出，源文件已保留");
    if (!MoveFileExW(partial.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("无法保存最终文件: " + utf8(errorMessage()));
  } catch (...) {
    std::error_code ignored;
    fs::remove(partial, ignored);
    throw;
  }
  // Delete only our own known session after successful, atomic export. Never delete imported files.
  if (source.filename() == L"recording.mkv" && source.parent_path().parent_path() == sessionsDirectory()) {
    std::error_code ignored;
    fs::remove(source, ignored);
    fs::remove(source.parent_path() / L"target.txt", ignored);
  }
  setState(State::Completed, L"录制已保存：" + destination.wstring());
}

void Recorder::recover(const fs::path& source, const fs::path& destination) {
  prepare();
  worker_ = std::thread([this, source, destination] {
    try {
      ComScope com;
      auto directory = sessionsDirectory() / uniqueId();
      fs::create_directory(directory);
      exportFile(source, destination, directory / L"recovery.log");
    } catch (const std::exception& error) { fail(error); }
  });
}

}
