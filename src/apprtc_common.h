#ifndef WEBRTC_APPRTC_CLI_SRC_APPRTC_COMMON_H_
#define WEBRTC_APPRTC_CLI_SRC_APPRTC_COMMON_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "api/peer_connection_interface.h"

namespace webrtc_apprtc_cli {

enum class ExitCode {
  kSuccess = 0,
  kInvalidArgument = 2,
  kInitializationFailed = 3,
  kMediaFailed = 4,
  kSignalingFailed = 5,
  kPeerConnectionFailed = 6,
  kTimeout = 7,
  kRuntimeFailed = 8,
};

struct AppConfig {
  std::string room_server_url = "https://appr.tc";
  std::string room_id;
  std::string video_path;
  std::string log_severity = "info";
  int timeout_seconds = 20;
  int call_duration_seconds = 4;
  int stats_interval_seconds = 1;
  int audio_frequency_hz = 440;
  int audio_sample_rate_hz = 48000;
  int audio_channels = 1;
  bool trace_signaling = false;
  bool validate_inputs_only = false;
};

struct IceServerConfig {
  std::vector<std::string> urls;
  std::string username;
  std::string credential;
};

struct JoinResponse {
  std::string result;
  std::string room_id;
  std::string client_id;
  bool is_initiator = false;
  std::string wss_url;
  std::string wss_post_url;
  std::string pc_config_json;
  std::vector<std::string> messages;
  std::vector<IceServerConfig> ice_servers;
};

struct MessageResponse {
  std::string result;
  std::string error;
};

enum class SignalingMessageType {
  kOffer,
  kAnswer,
  kCandidate,
  kBye,
  kUnknown,
};

struct SignalingMessage {
  SignalingMessageType type = SignalingMessageType::kUnknown;
  std::string raw_json;
  std::string sdp_type;
  std::string sdp;
  std::string candidate;
  std::string sdp_mid;
  int sdp_mline_index = 0;
};

absl::Status ValidateConfig(const AppConfig& config);
absl::StatusOr<std::string> ReadFileToString(const std::string& path);
absl::StatusOr<webrtc::PeerConnectionInterface::IceServers> ParseIceServers(
    const std::string& pc_config_json);
absl::StatusOr<SignalingMessage> ParseSignalingMessage(
    const std::string& json_text);
const char* SignalingMessageTypeToString(SignalingMessageType type);
absl::StatusOr<std::string> SerializeSessionDescription(
    const webrtc::SessionDescriptionInterface& description);
absl::StatusOr<std::string> SerializeIceCandidate(
    const webrtc::IceCandidateInterface& candidate);

}  // namespace webrtc_apprtc_cli

#endif  // WEBRTC_APPRTC_CLI_SRC_APPRTC_COMMON_H_
