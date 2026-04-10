#ifndef WEBRTC_APPRTC_CLI_SRC_SINE_WAVE_CAPTURER_H_
#define WEBRTC_APPRTC_CLI_SRC_SINE_WAVE_CAPTURER_H_

#include <stdint.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/audio/audio_device_defines.h"
#include "api/environment/environment.h"
#include "api/scoped_refptr.h"
#include "modules/audio_device/include/audio_device_default.h"

namespace webrtc_apprtc_cli {

class SineWaveAudioDeviceModule
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<
          webrtc::AudioDeviceModule> {
 public:
  SineWaveAudioDeviceModule(int sample_rate_hz,
                            int num_channels,
                            int frequency_hz);
  ~SineWaveAudioDeviceModule() override;

  int32_t RegisterAudioCallback(
      webrtc::AudioTransport* audio_callback) override;
  int32_t Init() override;
  int32_t Terminate() override;
  bool Initialized() const override;
  int32_t RecordingIsAvailable(bool* available) override;
  int32_t InitRecording() override;
  bool RecordingIsInitialized() const override;
  int32_t StartRecording() override;
  int32_t StopRecording() override;
  bool Recording() const override;
  int32_t PlayoutIsAvailable(bool* available) override;
  int32_t InitPlayout() override;
  bool PlayoutIsInitialized() const override;
  int32_t StartPlayout() override;
  int32_t StopPlayout() override;
  bool Playing() const override;
  int32_t StereoRecordingIsAvailable(bool* available) const override;
  int32_t StereoRecording(bool* enabled) const override;
  int32_t SetStereoRecording(bool enable) override;

 private:
  void CaptureLoop();

  int sample_rate_hz_;
  int num_channels_;
  int frequency_hz_;
  mutable std::mutex mutex_;
  webrtc::AudioTransport* audio_callback_ = nullptr;
  std::thread capture_thread_;
  std::atomic<bool> recording_{false};
  bool initialized_ = false;
  bool recording_initialized_ = false;
  bool playout_initialized_ = false;
  bool playing_ = false;
  bool stereo_recording_ = false;
};

webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreateSineWaveAudioDevice(
    const webrtc::Environment& env,
    int sample_rate_hz,
    int num_channels,
    int frequency_hz);

}  // namespace webrtc_apprtc_cli

#endif  // WEBRTC_APPRTC_CLI_SRC_SINE_WAVE_CAPTURER_H_
