#include "sine_wave_capturer.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include "api/make_ref_counted.h"
#include "rtc_base/checks.h"

namespace webrtc_apprtc_cli {

SineWaveAudioDeviceModule::SineWaveAudioDeviceModule(int sample_rate_hz,
                                                     int num_channels,
                                                     int frequency_hz)
    : sample_rate_hz_(sample_rate_hz),
      num_channels_(num_channels),
      frequency_hz_(frequency_hz) {}

SineWaveAudioDeviceModule::~SineWaveAudioDeviceModule() {
  StopRecording();
}

int32_t SineWaveAudioDeviceModule::RegisterAudioCallback(
    webrtc::AudioTransport* audio_callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  audio_callback_ = audio_callback;
  return 0;
}

int32_t SineWaveAudioDeviceModule::Init() {
  initialized_ = true;
  return 0;
}

int32_t SineWaveAudioDeviceModule::Terminate() {
  StopRecording();
  initialized_ = false;
  return 0;
}

bool SineWaveAudioDeviceModule::Initialized() const {
  return initialized_;
}

int32_t SineWaveAudioDeviceModule::RecordingIsAvailable(bool* available) {
  RTC_CHECK(available != nullptr);
  *available = true;
  return 0;
}

int32_t SineWaveAudioDeviceModule::InitRecording() {
  recording_initialized_ = true;
  return 0;
}

bool SineWaveAudioDeviceModule::RecordingIsInitialized() const {
  return recording_initialized_;
}

int32_t SineWaveAudioDeviceModule::StartRecording() {
  if (recording_.exchange(true)) {
    return 0;
  }
  capture_thread_ = std::thread([this] { CaptureLoop(); });
  return 0;
}

int32_t SineWaveAudioDeviceModule::StopRecording() {
  recording_.store(false);
  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
  return 0;
}

bool SineWaveAudioDeviceModule::Recording() const {
  return recording_.load();
}

int32_t SineWaveAudioDeviceModule::PlayoutIsAvailable(bool* available) {
  RTC_CHECK(available != nullptr);
  *available = true;
  return 0;
}

int32_t SineWaveAudioDeviceModule::InitPlayout() {
  playout_initialized_ = true;
  return 0;
}

bool SineWaveAudioDeviceModule::PlayoutIsInitialized() const {
  return playout_initialized_;
}

int32_t SineWaveAudioDeviceModule::StartPlayout() {
  playing_ = true;
  return 0;
}

int32_t SineWaveAudioDeviceModule::StopPlayout() {
  playing_ = false;
  return 0;
}

bool SineWaveAudioDeviceModule::Playing() const {
  return playing_;
}

int32_t SineWaveAudioDeviceModule::StereoRecordingIsAvailable(
    bool* available) const {
  RTC_CHECK(available != nullptr);
  *available = true;
  return 0;
}

int32_t SineWaveAudioDeviceModule::StereoRecording(bool* enabled) const {
  RTC_CHECK(enabled != nullptr);
  *enabled = stereo_recording_;
  return 0;
}

int32_t SineWaveAudioDeviceModule::SetStereoRecording(bool enable) {
  stereo_recording_ = enable;
  return 0;
}

void SineWaveAudioDeviceModule::CaptureLoop() {
  const size_t samples_per_channel = static_cast<size_t>(sample_rate_hz_ / 100);
  std::vector<int16_t> buffer(samples_per_channel *
                              static_cast<size_t>(num_channels_));

  constexpr double kTwoPi = 6.28318530717958647692;
  const double phase_increment = kTwoPi * static_cast<double>(frequency_hz_) /
                                 static_cast<double>(sample_rate_hz_);
  double phase = 0.0;
  auto next_tick = std::chrono::steady_clock::now();

  while (recording_.load()) {
    for (size_t i = 0; i < samples_per_channel; ++i) {
      const int16_t sample = static_cast<int16_t>(std::sin(phase) * 16000.0);
      phase += phase_increment;
      if (phase > kTwoPi) {
        phase -= kTwoPi;
      }
      for (int channel = 0; channel < num_channels_; ++channel) {
        buffer[i * static_cast<size_t>(num_channels_) +
               static_cast<size_t>(channel)] = sample;
      }
    }

    webrtc::AudioTransport* callback = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = audio_callback_;
    }
    if (callback != nullptr) {
      uint32_t new_mic_level = 0;
      callback->RecordedDataIsAvailable(
          buffer.data(), samples_per_channel, sizeof(int16_t),
          static_cast<size_t>(num_channels_),
          static_cast<uint32_t>(sample_rate_hz_), 0, 0, 0, false, new_mic_level,
          std::optional<int64_t>());
    }

    next_tick += std::chrono::milliseconds(10);
    std::this_thread::sleep_until(next_tick);
  }
}

webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreateSineWaveAudioDevice(
    const webrtc::Environment& env,
    int sample_rate_hz,
    int num_channels,
    int frequency_hz) {
  (void)env;
  return webrtc::make_ref_counted<SineWaveAudioDeviceModule>(
      sample_rate_hz, num_channels, frequency_hz);
}

}  // namespace webrtc_apprtc_cli
