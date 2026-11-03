#include "audio_processing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace recdesk {
namespace {

constexpr std::int64_t ticksPerSecond = 10000000;

std::int64_t firstFrameAt(std::int64_t time100ns) {
  // Exact signed ceil(time * rate / ticks), without a floating-point boundary round-up.
  auto whole = time100ns / ticksPerSecond * audioSampleRate;
  auto fraction = time100ns % ticksPerSecond * audioSampleRate;
  return whole + fraction / ticksPerSecond + (fraction % ticksPerSecond > 0 ? 1 : 0);
}

float finite(float sample) { return std::isfinite(sample) ? sample : 0.0f; }

}

AudioTimeline::AudioTimeline(std::size_t sourceCount) : sourceCount_(sourceCount) {
  if (!sourceCount || sourceCount > 2) throw std::invalid_argument("Audio timeline requires one or two sources");
  samples_.resize(sourceCount * static_cast<std::size_t>(audioRingFrames));
  tags_.resize(samples_.size(), -1);
}

void AudioTimeline::write(std::size_t source, std::int64_t frame, StereoFrame sample, std::int64_t consumed) {
  if (source >= sourceCount_) throw std::out_of_range("Invalid audio source");
  if (frame < 0 || frame < consumed || frame - consumed >= audioRingFrames) return;
  auto slot = source * static_cast<std::size_t>(audioRingFrames) + static_cast<std::size_t>(frame % audioRingFrames);
  tags_[slot] = frame;
  samples_[slot] = {finite(sample.left), finite(sample.right)};
}

void AudioTimeline::read(std::int64_t firstFrame, std::span<float> output) const {
  if (output.size() % 2 || firstFrame < 0) throw std::invalid_argument("Invalid stereo output buffer");
  for (std::size_t i = 0; i < output.size() / 2; ++i) {
    auto frame = firstFrame + static_cast<std::int64_t>(i);
    StereoFrame mixed{};
    for (std::size_t source = 0; source < sourceCount_; ++source) {
      auto slot = source * static_cast<std::size_t>(audioRingFrames) + static_cast<std::size_t>(frame % audioRingFrames);
      if (tags_[slot] != frame) continue;
      mixed.left += samples_[slot].left;
      mixed.right += samples_[slot].right;
    }
    float gain = 1.0f / static_cast<float>(sourceCount_);
    output[i * 2] = std::clamp(mixed.left * gain, -1.0f, 1.0f);
    output[i * 2 + 1] = std::clamp(mixed.right * gain, -1.0f, 1.0f);
  }
}

AudioResampler::AudioResampler(std::uint32_t inputRate) : inputRate_(inputRate) {
  if (!inputRate || inputRate > 768000) throw std::invalid_argument("Unsupported audio sample rate");
  step_ = static_cast<double>(inputRate_) / audioSampleRate;
}

void AudioResampler::anchor(std::int64_t time100ns) {
  auto first = firstFrameAt(time100ns);
  nextOutputFrame_ = initialized_ ? std::max(nextOutputFrame_, first) : first;
  position_ = 0;
  step_ = static_cast<double>(inputRate_) / audioSampleRate;
  filteredClockError_ = 0;
  havePrevious_ = false;
  initialized_ = true;
}

void AudioResampler::append(std::span<const StereoFrame> input, AudioPacketTiming timing,
  AudioTimeline& timeline, std::size_t source, std::int64_t consumed) {
  if (input.empty()) return;
  if (input.size() > static_cast<std::size_t>(inputRate_) * 2)
    throw std::invalid_argument("Audio packet exceeds bounded capture buffer");
  auto devicePosition = timing.timestampValid ? timing.devicePosition : expectedDevicePosition_;
  if (devicePosition > std::numeric_limits<std::uint64_t>::max() - input.size())
    throw std::invalid_argument("Audio device position overflow");
  if (initialized_ && timing.timestampValid && !timing.discontinuity && devicePosition < expectedDevicePosition_) {
    auto repeated = expectedDevicePosition_ - devicePosition;
    if (repeated >= input.size()) return;
    input = input.subspan(static_cast<std::size_t>(repeated));
    timing.relativeTime100ns += static_cast<std::int64_t>(repeated) * ticksPerSecond / inputRate_;
    devicePosition = expectedDevicePosition_;
  }
  if (!initialized_ || timing.discontinuity || devicePosition != expectedDevicePosition_) {
    anchor(timing.relativeTime100ns);
  } else if (timing.timestampValid) {
    // Ignore short packet timestamp jitter, while slowly tracking real device-clock drift.
    // This changes interpolation speed, never jumps/duplicates an output sample.
    double predictedStart = static_cast<double>(nextOutputFrame_) - position_ / step_;
    double reportedStart = static_cast<double>(timing.relativeTime100ns) * audioSampleRate / ticksPerSecond;
    double error = predictedStart - reportedStart;
    if (std::abs(error) > audioSampleRate / 4) {
      anchor(timing.relativeTime100ns);
    } else {
      double alpha = std::min(1.0, static_cast<double>(input.size()) / inputRate_);
      filteredClockError_ += alpha * (error - filteredClockError_);
      double adjustedError = std::copysign(std::max(0.0, std::abs(filteredClockError_) - 2.0), filteredClockError_);
      double correction = std::clamp(adjustedError / (audioSampleRate * 2.0), -0.001, 0.001);
      step_ = static_cast<double>(inputRate_) / audioSampleRate * (1.0 + correction);
    }
  }

  for (;;) {
    // Remove tiny floating-point residue only; phase is otherwise continuous across packets.
    double nearest = std::round(position_);
    if (std::abs(position_ - nearest) < 1e-9) position_ = nearest;
    auto index = static_cast<std::int64_t>(std::floor(position_));
    if (index >= static_cast<std::int64_t>(input.size())) break;
    double fraction = position_ - static_cast<double>(index);
    if (index < -1 || (index == -1 && !havePrevious_))
      throw std::logic_error("Invalid streaming audio phase");
    if (index + 1 >= static_cast<std::int64_t>(input.size()) && fraction > 0) break;
    auto a = index < 0 ? previous_ : input[static_cast<std::size_t>(index)];
    auto b = fraction == 0 ? a : input[static_cast<std::size_t>(index + 1)];
    float blend = static_cast<float>(fraction);
    timeline.write(source, nextOutputFrame_, {a.left + (b.left - a.left) * blend,
      a.right + (b.right - a.right) * blend}, consumed);
    ++nextOutputFrame_;
    position_ += step_;
  }
  position_ -= static_cast<double>(input.size());
  previous_ = input.back();
  havePrevious_ = true;
  expectedDevicePosition_ = devicePosition + input.size();
}

}
