#include "audio.h"
#include "audio_processing.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace recdesk {
namespace {

class Source {
public:
  explicit Source(bool loopback) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    checkHr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "无法枚举音频设备");
    ComPtr<IMMDevice> device;
    checkHr(enumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &device),
      loopback ? "没有可用的系统声音设备" : "没有可用的麦克风，请检查 Windows 麦克风隐私权限");
    checkHr(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_), "无法打开音频设备");
    WAVEFORMATEX* raw = nullptr;
    checkHr(client_->GetMixFormat(&raw), "无法读取音频格式");
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(raw, CoTaskMemFree);
    rate_ = raw->nSamplesPerSec;
    channels_ = raw->nChannels;
    bits_ = raw->wBitsPerSample;
    block_ = raw->nBlockAlign;
    WORD tag = raw->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && raw->cbSize >= 22) {
      auto extended = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(raw);
      if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
      else if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
    }
    floating_ = tag == WAVE_FORMAT_IEEE_FLOAT;
    if (!rate_ || rate_ > 768000 || !channels_ || channels_ > 32 || block_ != channels_ * (bits_ / 8) ||
        !((floating_ && bits_ == 32) || (tag == WAVE_FORMAT_PCM && (bits_ == 16 || bits_ == 24 || bits_ == 32))))
      throw std::runtime_error("音频设备格式不受支持，请将设备格式设为 PCM 16/24/32 位或浮点 32 位");
    checkHr(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
      1000000, 0, raw, nullptr), "无法初始化音频录制（设备可能被独占）");
    checkHr(client_->GetService(IID_PPV_ARGS(&capture_)), "无法创建音频捕获服务");
    UINT32 capacity = 0;
    checkHr(client_->GetBufferSize(&capacity), "无法读取音频缓冲区大小");
    if (!capacity || capacity > static_cast<std::uint64_t>(rate_) * 2)
      throw std::runtime_error("音频设备缓冲区大小不受支持");
    decoded_.resize(capacity);
    resampler_ = std::make_unique<AudioResampler>(rate_);
  }
  ~Source() { if (started_) client_->Stop(); }
  void start() { checkHr(client_->Start(), "无法开始捕获音频"); started_ = true; }
  void drain(AudioTimeline& timeline, std::size_t source, std::int64_t epoch, std::int64_t consumed) {
    UINT32 packet = 0;
    checkHr(capture_->GetNextPacketSize(&packet), "音频设备已断开");
    int packets = 0;
    while (packet) {
      if (++packets > 256) throw std::runtime_error("音频设备持续积压，已停止录制并保留文件");
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 devicePosition = 0, qpc = 0;
      checkHr(capture_->GetBuffer(&data, &frames, &flags, &devicePosition, &qpc), "无法读取音频数据");
      if (!frames) break;
      struct BufferRelease {
        IAudioCaptureClient* capture;
        UINT32 frames;
        ~BufferRelease() { if (frames) capture->ReleaseBuffer(frames); }
        void finish() {
          auto count = frames;
          frames = 0;
          checkHr(capture->ReleaseBuffer(count), "无法释放音频缓冲区");
        }
      } release{capture_.Get(), frames};
      if (frames > decoded_.size()) throw std::runtime_error("音频包超过设备缓冲区大小");
      // Timestamps are in the same QPC 100-ns domain as the recording timeline.
      std::int64_t startTime = static_cast<std::int64_t>(qpc);
      bool validTimestamp = !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc != 0;
      if (!validTimestamp)
        startTime = clock100ns() - static_cast<std::int64_t>(frames) * 10000000 / rate_;
      for (UINT32 frame = 0; frame < frames; ++frame) {
        auto channelValue = [&](WORD channel) {
          if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) return 0.0f;
          return sample(data + frame * block_, channel);
        };
        float left = channelValue(0);
        float right = channelValue(channels_ == 1 ? 0 : 1);
        // Include centre/surround channels without requiring a Stereo Mix driver.
        if (channels_ > 2) {
          float rest = 0;
          for (WORD channel = 2; channel < channels_; ++channel) rest += channelValue(channel);
          float extra = rest / channels_;
          left = left * 0.7f + extra;
          right = right * 0.7f + extra;
        }
        decoded_[frame] = {left, right};
      }
      // Release the WASAPI packet before interpolation/mixing, even if either throws.
      release.finish();
      resampler_->append(std::span<const StereoFrame>(decoded_.data(), frames),
        {devicePosition, startTime - epoch, validTimestamp, (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0},
        timeline, source, consumed);
      checkHr(capture_->GetNextPacketSize(&packet), "音频设备已断开");
    }
  }
private:
  float sample(const BYTE* frame, WORD channel) const {
    const BYTE* p = frame + channel * (bits_ / 8);
    if (floating_) { float value; std::memcpy(&value, p, 4); return std::isfinite(value) ? value : 0; }
    if (bits_ == 16) { std::int16_t value; std::memcpy(&value, p, 2); return value / 32768.0f; }
    if (bits_ == 24) {
      std::int32_t value = p[0] | (p[1] << 8) | (p[2] << 16);
      if (value & 0x800000) value -= 0x1000000;
      return value / 8388608.0f;
    }
    std::int32_t value; std::memcpy(&value, p, 4); return static_cast<float>(value / 2147483648.0);
  }
  ComPtr<IAudioClient> client_;
  ComPtr<IAudioCaptureClient> capture_;
  std::vector<StereoFrame> decoded_;
  std::unique_ptr<AudioResampler> resampler_;
  DWORD rate_ = 0;
  WORD channels_ = 0, bits_ = 0, block_ = 0;
  bool floating_ = false, started_ = false;
};

}

AudioPipe::AudioPipe(bool systemAudio, bool microphone) : systemAudio_(systemAudio), microphone_(microphone) {
  name_ = L"\\\\.\\pipe\\RecDesk-" + uniqueId();
  stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  pipe_.reset(CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 38400, 0, 0, nullptr));
  if (!stopEvent_ || !pipe_) throw std::runtime_error("无法创建音频管道: " + utf8(errorMessage()));
}

AudioPipe::~AudioPipe() { stop(); }
void AudioPipe::start() {
  std::promise<void> ready;
  auto result = ready.get_future();
  thread_ = std::thread([this, ready = std::move(ready)]() mutable { run(std::move(ready)); });
  result.get();
}
void AudioPipe::stop() {
  SetEvent(stopEvent_.get());
  if (thread_.joinable()) thread_.join();
  pipe_.reset();
}
std::wstring AudioPipe::error() const { std::lock_guard lock(mutex_); return error_; }

bool AudioPipe::completeIo(OVERLAPPED& operation, DWORD timeout, DWORD& transferred) {
  HANDLE events[] = {stopEvent_.get(), operation.hEvent};
  DWORD result = WaitForMultipleObjects(2, events, FALSE, timeout);
  if (result != WAIT_OBJECT_0 + 1) {
    CancelIoEx(pipe_.get(), &operation);
    GetOverlappedResult(pipe_.get(), &operation, &transferred, TRUE);
    if (result == WAIT_TIMEOUT) throw std::runtime_error("音频管道超时，编码器可能已卡住。源文件已保留。");
    return false;
  }
  if (!GetOverlappedResult(pipe_.get(), &operation, &transferred, FALSE)) {
    if (WaitForSingleObject(stopEvent_.get(), 0) == WAIT_OBJECT_0) return false;
    throw std::runtime_error("音频管道已断开: " + utf8(errorMessage()));
  }
  return true;
}

bool AudioPipe::connect() {
  Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) throw std::runtime_error("无法创建管道事件");
  OVERLAPPED operation{};
  operation.hEvent = event.get();
  if (ConnectNamedPipe(pipe_.get(), &operation)) return true;
  DWORD error = GetLastError();
  if (error == ERROR_PIPE_CONNECTED) return true;
  if (error != ERROR_IO_PENDING) throw std::runtime_error("无法连接编码器音频管道");
  DWORD transferred = 0;
  return completeIo(operation, 20000, transferred);
}

bool AudioPipe::write(const void* bytes, DWORD size) {
  Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) throw std::runtime_error("无法创建音频写入事件");
  DWORD offset = 0;
  while (offset < size) {
    ResetEvent(event.get());
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD transferred = 0;
    if (!WriteFile(pipe_.get(), static_cast<const BYTE*>(bytes) + offset, size - offset, &transferred, &operation)) {
      if (GetLastError() != ERROR_IO_PENDING) {
        if (WaitForSingleObject(stopEvent_.get(), 0) == WAIT_OBJECT_0) return false;
        throw std::runtime_error("编码器音频通道已关闭");
      }
      if (!completeIo(operation, 3000, transferred)) return false;
    }
    if (!transferred) throw std::runtime_error("无法继续写入音频");
    offset += transferred;
  }
  return true;
}

void AudioPipe::run(std::promise<void> ready) {
  bool initialized = false;
  try {
    ComScope com;
    std::vector<std::unique_ptr<Source>> sources;
    if (systemAudio_) sources.push_back(std::make_unique<Source>(true));
    if (microphone_) sources.push_back(std::make_unique<Source>(false));
    AudioTimeline timeline(sources.size());
    ready.set_value();
    initialized = true;
    if (!connect()) return;
    auto epoch = epoch_.load();
    if (!epoch) throw std::runtime_error("音频时间线尚未初始化");
    for (auto& source : sources) source->start();
    std::int64_t emitted = 0;
    std::array<float, 960> samples{};
    while (WaitForSingleObject(stopEvent_.get(), 5) == WAIT_TIMEOUT) {
      for (std::size_t i = 0; i < sources.size(); ++i) sources[i]->drain(timeline, i, epoch, emitted);
      auto due = (clock100ns() - epoch - 1000000) * audioSampleRate / 10000000;
      if (due - emitted > audioSampleRate) throw std::runtime_error("音频编码积压超过 1 秒，已停止录制以防止内存持续增长");
      while (emitted + 480 <= due) {
        timeline.read(emitted, samples);
        if (!write(samples.data(), static_cast<DWORD>(sizeof(samples)))) return;
        emitted += 480;
      }
    }
  } catch (const std::exception& error) {
    if (!initialized) ready.set_exception(std::current_exception());
    else { std::lock_guard lock(mutex_); error_ = wide(error.what()); }
  }
  // Closing the server handle sends EOF, including after an asynchronous failure.
  pipe_.reset();
}

}
