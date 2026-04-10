#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "apprtc_call.h"

ABSL_FLAG(std::string,
          room_server_url,
          "",
          "AppRTC-compatible room server base URL. Required in practice.");
ABSL_FLAG(std::string, room_id, "", "Room identifier.");
ABSL_FLAG(std::string, video_path, "", "Path to MP4 video input.");
ABSL_FLAG(int, timeout_seconds, 20, "Overall timeout in seconds.");
ABSL_FLAG(int, call_duration_seconds, 4, "Call duration in seconds.");
ABSL_FLAG(int, stats_interval_seconds, 1, "Stats log interval in seconds.");
ABSL_FLAG(int, audio_frequency_hz, 440, "Generated sine-wave frequency.");
ABSL_FLAG(int, audio_sample_rate_hz, 48000, "Generated audio sample rate.");
ABSL_FLAG(int, audio_channels, 1, "Generated audio channel count.");
ABSL_FLAG(std::string,
          log_severity,
          "info",
          "WebRTC log severity: verbose, info, warning, error, none.");
ABSL_FLAG(bool,
          trace_signaling,
          false,
          "Log AppRTC signaling flow details for debugging.");
ABSL_FLAG(bool,
          validate_inputs_only,
          false,
          "Validate media inputs and configuration, then exit.");

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage(
      "Headless AppRTC-style WebRTC CLI sample using file video and synthetic "
      "audio.");
  absl::ParseCommandLine(argc, argv);

  webrtc_apprtc_cli::AppConfig config;
  config.room_server_url = absl::GetFlag(FLAGS_room_server_url);
  config.room_id = absl::GetFlag(FLAGS_room_id);
  config.video_path = absl::GetFlag(FLAGS_video_path);
  config.log_severity = absl::GetFlag(FLAGS_log_severity);
  config.timeout_seconds = absl::GetFlag(FLAGS_timeout_seconds);
  config.call_duration_seconds = absl::GetFlag(FLAGS_call_duration_seconds);
  config.stats_interval_seconds = absl::GetFlag(FLAGS_stats_interval_seconds);
  config.audio_frequency_hz = absl::GetFlag(FLAGS_audio_frequency_hz);
  config.audio_sample_rate_hz = absl::GetFlag(FLAGS_audio_sample_rate_hz);
  config.audio_channels = absl::GetFlag(FLAGS_audio_channels);
  config.trace_signaling = absl::GetFlag(FLAGS_trace_signaling);
  config.validate_inputs_only = absl::GetFlag(FLAGS_validate_inputs_only);

  return webrtc_apprtc_cli::RunAppRtcCall(config);
}
