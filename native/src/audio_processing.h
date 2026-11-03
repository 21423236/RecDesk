#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace recdesk {

inline constexpr std::int64_t audioSampleRate = 48000;
inline constexpr std::int64_t audioRingFrames = audioSampleRate * 2;

struct StereoFrame { float left = 0; float right = 0; };

// Each source owns its contribution. Rewriting a source/frame must never amplify it.
class AudioTimeline {
public:
  explicit AudioTimeline(std::size_t sourceCount);
  void write(std::size_t source, std::int64_t frame, StereoFrame sample, std::int64_t consumed);
  void read(std::int64_t firstFrame, std::span<float> output) const;
private:
  std::size_t sourceCount_;
  std::vector<StereoFrame> samples_;
  std::vector<std::int64_t> tags_;
};

struct AudioPacketTiming {
  std::uint64_t devicePosition = 0;
  std::int64_t relativeTime100ns = 0;
  bool timestampValid = true;
  bool discontinuity = false;
};

// Streaming interpolation carries phase and the previous sample across packets.
// Device-frame continuity, not independently rounded packet timestamps, drives samples.
class AudioResampler {
public:
  explicit AudioResampler(std::uint32_t inputRate);
  void append(std::span<const StereoFrame> input, AudioPacketTiming timing,
    AudioTimeline& timeline, std::size_t source, std::int64_t consumed);
  std::int64_t nextOutputFrame() const { return nextOutputFrame_; }
private:
  void anchor(std::int64_t time100ns);
  std::uint32_t inputRate_;
  std::uint64_t expectedDevicePosition_ = 0;
  std::int64_t nextOutputFrame_ = 0;
  double position_ = 0, step_ = 1, filteredClockError_ = 0;
  StereoFrame previous_{};
  bool initialized_ = false, havePrevious_ = false;
};

}
