#include "audio_processing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

using namespace recdesk;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void near(float actual, float expected, const char* message, float tolerance = 0.00002f) {
  if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) throw std::runtime_error(message);
}

void timelineChecks() {
  AudioTimeline timeline(2);
  timeline.write(0, 0, {0.4f, -0.4f}, 0);
  timeline.write(0, 0, {0.4f, -0.4f}, 0);
  timeline.write(1, 0, {0.2f, -0.2f}, 0);
  std::array<float, 2> output{};
  timeline.read(0, output);
  near(output[0], 0.3f, "Repeated source samples must not be added twice");
  near(output[1], -0.3f, "Stereo channels must remain independent");
  timeline.write(0, 0, {0.6f, -0.6f}, 0);
  timeline.read(0, output);
  near(output[0], 0.4f, "Replacing microphone contribution must not overwrite system audio");
  timeline.write(0, audioRingFrames, {1, 1}, 0);
  timeline.read(0, output);
  near(output[0], 0.4f, "Future samples outside bounded ring must not overwrite current audio");
  timeline.write(0, audioRingFrames, {0.8f, -0.8f}, audioRingFrames);
  timeline.read(0, output);
  near(output[0], 0.1f, "Old ring tags must not replay stale source samples");
  timeline.read(audioRingFrames, output);
  near(output[0], 0.4f, "Unwritten second source must mix as silence after ring wrap");
  timeline.write(0, audioRingFrames + 1, {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}, audioRingFrames);
  timeline.read(audioRingFrames + 1, output);
  near(output[0], 0, "Nonfinite audio must not poison encoder output");
  near(output[1], 0, "Infinite audio must be sanitized");
}

void boundaryChecks() {
  int oldOverlaps = 0;
  AudioTimeline timeline(1);
  AudioResampler resampler(48000);
  std::array<StereoFrame, 480> packet{};
  packet.fill({0.4f, -0.4f});
  std::array<float, 960> output{};
  for (std::int64_t i = 0; i < 6000; ++i) {
    auto ticks = i * 100000;
    // Reproduce the removed calculation, including its independently rounded packet end.
    volatile double oldEndTime = static_cast<double>(ticks) / 10000000.0 + 480.0 / 48000.0;
    auto oldLast = static_cast<std::int64_t>(std::ceil(oldEndTime * 48000.0));
    auto oldNext = static_cast<std::int64_t>(std::ceil(static_cast<double>(ticks + 100000) * 48000.0 / 10000000.0));
    if (oldLast > oldNext) ++oldOverlaps;
    auto first = i * 480;
    resampler.append(packet, {static_cast<std::uint64_t>(first), ticks}, timeline, 0, first);
    require(resampler.nextOutputFrame() == first + 480, "Continuous 48 kHz packets must produce exactly 480 unique frames");
    timeline.read(first, output);
    for (std::size_t j = 0; j < output.size(); j += 2) {
      near(output[j], 0.4f, "No packet-boundary amplitude spikes or holes");
      near(output[j + 1], -0.4f, "No packet-boundary stereo spikes");
    }
  }
  require(oldOverlaps > 0, "Regression fixture must reproduce old floating-point packet overlaps");
  std::cout << "  60 s / 48 kHz: old overlapping boundaries=" << oldOverlaps << ", new unique frames=2880000\n";
}

StereoFrame tone(std::int64_t frame, std::uint32_t rate) {
  auto phase = 2.0 * std::numbers::pi * static_cast<double>(frame) / rate;
  return {static_cast<float>(0.4 * std::sin(997.0 * phase)), static_cast<float>(0.3 * std::sin(337.0 * phase))};
}

void resamplingChecks() {
  constexpr std::array<std::size_t, 6> sizes{1, 7, 127, 441, 480, 1023};
  for (std::uint32_t rate : {16000u, 32000u, 44100u, 48000u, 96000u, 192000u}) {
    AudioTimeline timeline(1);
    AudioResampler resampler(rate);
    std::vector<StereoFrame> packet;
    std::vector<float> output;
    std::int64_t position = 0, consumed = 0;
    std::size_t packetIndex = 0;
    auto total = static_cast<std::int64_t>(rate) * 2;
    while (position < total) {
      auto count = std::min<std::int64_t>(static_cast<std::int64_t>(sizes[packetIndex++ % sizes.size()]), total - position);
      packet.resize(static_cast<std::size_t>(count));
      for (std::int64_t i = 0; i < count; ++i) packet[static_cast<std::size_t>(i)] = tone(position + i, rate);
      auto ticks = static_cast<std::int64_t>(std::llround(static_cast<double>(position) * 10000000.0 / rate));
      resampler.append(packet, {static_cast<std::uint64_t>(position), ticks}, timeline, 0, consumed);
      auto ready = resampler.nextOutputFrame() - consumed;
      require(ready >= 0 && ready < audioRingFrames, "Resampler output must be bounded and monotonic");
      output.resize(static_cast<std::size_t>(ready) * 2);
      timeline.read(consumed, output);
      for (std::int64_t i = 0; i < ready; ++i) {
        double source = static_cast<double>(consumed + i) * rate / audioSampleRate;
        auto first = static_cast<std::int64_t>(std::floor(source));
        float fraction = static_cast<float>(source - static_cast<double>(first));
        auto a = tone(first, rate), b = tone(first + 1, rate);
        near(output[static_cast<std::size_t>(i) * 2], a.left + (b.left - a.left) * fraction, "Streaming left channel differs from continuous interpolation");
        near(output[static_cast<std::size_t>(i) * 2 + 1], a.right + (b.right - a.right) * fraction, "Streaming right channel differs at packet boundary");
      }
      consumed += ready;
      position += count;
    }
    auto expected = (total - 1) * audioSampleRate / rate + 1;
    require(std::abs(consumed - expected) <= 1, "No cumulative output-frame drift across irregular packets");
  }
}

void discontinuityChecks() {
  AudioTimeline timeline(1);
  AudioResampler resampler(48000);
  std::array<StereoFrame, 480> packet{};
  packet.fill({0.25f, -0.25f});
  resampler.append(packet, {0, 0}, timeline, 0, 0);
  resampler.append(packet, {0, 0}, timeline, 0, 0);
  require(resampler.nextOutputFrame() == 480, "Duplicate device packets must not replay into the future");
  resampler.append(packet, {240, 50000}, timeline, 0, 0);
  require(resampler.nextOutputFrame() == 720, "Overlapping packet prefix must be trimmed, not replayed");
  packet.fill({-0.25f, 0.25f});
  resampler.append(packet, {1200, 250000, true, true}, timeline, 0, 0);
  require(resampler.nextOutputFrame() == 1680, "Lost packets must preserve their timestamp gap");
  std::vector<float> output(1680 * 2);
  timeline.read(0, output);
  for (int i = 0; i < 1680; ++i) {
    float expected = i < 720 ? 0.25f : i < 1200 ? 0.0f : -0.25f;
    near(output[static_cast<std::size_t>(i) * 2], expected, "A real discontinuity must not stretch or amplify the last packet");
  }
  AudioTimeline clean(1);
  AudioResampler invalidTimestamp(48000);
  invalidTimestamp.append(packet, {0, 0}, clean, 0, 0);
  invalidTimestamp.append(packet, {999999, 999999999, false}, clean, 0, 0);
  invalidTimestamp.append(packet, {960, 200000}, clean, 0, 0);
  require(invalidTimestamp.nextOutputFrame() == 1440, "Invalid timestamps must not reset a continuous audio stream");
}

void mixingChecks() {
  AudioTimeline timeline(2);
  AudioResampler microphone(44100), system(48000);
  std::array<StereoFrame, 441> micPacket{};
  std::array<StereoFrame, 480> systemPacket{};
  micPacket.fill({0.8f, -0.8f});
  systemPacket.fill({0.8f, -0.8f});
  std::vector<float> output;
  std::int64_t consumed = 0;
  for (std::int64_t i = 0; i < 6000; ++i) {
    microphone.append(micPacket, {static_cast<std::uint64_t>(i * 441), i * 100000}, timeline, 0, consumed);
    system.append(systemPacket, {static_cast<std::uint64_t>(i * 480), i * 100000}, timeline, 1, consumed);
    auto ready = std::min(microphone.nextOutputFrame(), system.nextOutputFrame()) - consumed;
    output.resize(static_cast<std::size_t>(ready) * 2);
    timeline.read(consumed, output);
    for (std::size_t j = 0; j < output.size(); j += 2) {
      near(output[j], 0.8f, "44.1 kHz microphone plus 48 kHz system audio must not create clipping spikes");
      near(output[j + 1], -0.8f, "Dual-source mixing must keep independent stereo polarity");
    }
    consumed += ready;
  }
}

void clockChecks() {
  for (double ppm : {-300.0, 0.0, 300.0}) {
    AudioTimeline timeline(1);
    AudioResampler resampler(48000);
    std::array<StereoFrame, 480> packet{};
    std::vector<float> output;
    std::int64_t consumed = 0;
    float previous = 0;
    double actualRate = 48000.0 * (1.0 + ppm / 1000000.0);
    for (std::int64_t i = 0; i < 12000; ++i) {
      auto source = i * 480;
      for (std::size_t j = 0; j < packet.size(); ++j) {
        float value = static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 440.0 * (source + static_cast<std::int64_t>(j)) / 48000.0));
        packet[j] = {value, -value};
      }
      auto ticks = static_cast<std::int64_t>(std::llround(static_cast<double>(source) * 10000000.0 / actualRate));
      // Alternate +/-0.3 ms scheduling jitter without turning it into packet gaps.
      if (i) ticks += i % 2 ? 3000 : -3000;
      resampler.append(packet, {static_cast<std::uint64_t>(source), ticks}, timeline, 0, consumed);
      auto ready = resampler.nextOutputFrame() - consumed;
      output.resize(static_cast<std::size_t>(ready) * 2);
      timeline.read(consumed, output);
      for (std::size_t j = 0; j < output.size(); j += 2) {
        require(std::isfinite(output[j]) && std::abs(output[j] - previous) < 0.06f, "Clock tracking must not generate clicks or silent holes");
        near(output[j + 1], -output[j], "Clock correction must preserve stereo phase");
        previous = output[j];
      }
      consumed += ready;
    }
    auto expected = 12000.0 * 480.0 * 48000.0 / actualRate;
    require(std::abs(static_cast<double>(consumed) - expected) < 150, "Clock drift must be tracked without cumulative A/V skew");
    std::cout << "  120 s / clock " << ppm << " ppm: timing error=" << static_cast<double>(consumed) - expected << " frames\n";
  }
}

void offsetChecks() {
  for (auto ticks : {-1ll, 1ll, 288000000001ll}) {
    AudioTimeline timeline(1);
    AudioResampler resampler(48000);
    std::array<StereoFrame, 48> packet{};
    packet.fill({0.3f, 0.3f});
    auto first = ticks / 10000000 * 48000 + (ticks % 10000000 * 48000 + (ticks > 0 ? 9999999 : 0)) / 10000000;
    resampler.append(packet, {0, ticks}, timeline, 0, std::max<std::int64_t>(0, first));
    require(resampler.nextOutputFrame() == first + 48, "Exact timestamp anchoring must work before epoch and after eight hours");
    std::array<float, 96> output{};
    timeline.read(std::max<std::int64_t>(0, first), output);
    for (float value : output) near(value, 0.3f, "Timestamp anchoring must not add a sample-boundary spike");
  }
}

}

int main(int argc, char** argv) {
  struct Test { const char* name; void (*run)(); };
  const std::array tests{Test{"timeline", timelineChecks}, Test{"boundaries", boundaryChecks},
    Test{"resampling", resamplingChecks}, Test{"mixing", mixingChecks}, Test{"discontinuities", discontinuityChecks},
    Test{"clocks", clockChecks}, Test{"offsets", offsetChecks}};
  try {
    std::string selected;
    if (argc == 3 && std::string(argv[1]) == "--case") selected = argv[2];
    else if (argc != 1) throw std::invalid_argument("Use --case <test-name>");
    bool ran = false;
    for (const auto& test : tests) {
      if (!selected.empty() && selected != test.name) continue;
      test.run();
      std::cout << "PASS: " << test.name << '\n';
      ran = true;
    }
    require(ran, "Unknown audio test case");
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
