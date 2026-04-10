#include "apprtc_common.h"

#include <fstream>
#include <sstream>

#include "api/jsep.h"
#include "json/json.h"
#include "rtc_base/logging.h"

namespace webrtc_apprtc_cli {

namespace {

absl::Status JsonError(const std::string& context, const std::string& details) {
  return absl::InvalidArgumentError(context + ": " + details);
}

}  // namespace

absl::Status ValidateConfig(const AppConfig& config) {
  if (config.room_id.empty()) {
    return absl::InvalidArgumentError("--room_id must not be empty");
  }
  if (config.video_path.empty()) {
    return absl::InvalidArgumentError("--video_path must not be empty");
  }
  if (config.log_severity != "verbose" && config.log_severity != "info" &&
      config.log_severity != "warning" && config.log_severity != "error" &&
      config.log_severity != "none") {
    return absl::InvalidArgumentError(
        "--log_severity must be one of: verbose, info, warning, error, none");
  }
  if (config.timeout_seconds <= 0) {
    return absl::InvalidArgumentError("--timeout_seconds must be > 0");
  }
  if (config.call_duration_seconds <= 0) {
    return absl::InvalidArgumentError("--call_duration_seconds must be > 0");
  }
  if (config.stats_interval_seconds <= 0) {
    return absl::InvalidArgumentError("--stats_interval_seconds must be > 0");
  }
  if (config.audio_frequency_hz <= 0) {
    return absl::InvalidArgumentError("--audio_frequency_hz must be > 0");
  }
  if (config.audio_sample_rate_hz <= 0) {
    return absl::InvalidArgumentError("--audio_sample_rate_hz must be > 0");
  }
  if (config.audio_channels <= 0) {
    return absl::InvalidArgumentError("--audio_channels must be > 0");
  }
  if (config.call_duration_seconds >= config.timeout_seconds) {
    return absl::InvalidArgumentError(
        "--call_duration_seconds must be less than --timeout_seconds");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> ReadFileToString(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return absl::NotFoundError("failed to open file: " + path);
  }
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

absl::StatusOr<webrtc::PeerConnectionInterface::IceServers> ParseIceServers(
    const std::string& pc_config_json) {
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream stream(pc_config_json);
  if (!Json::parseFromStream(builder, stream, &root, &errors)) {
    return JsonError("failed to parse pc_config", errors);
  }

  webrtc::PeerConnectionInterface::IceServers servers;
  const Json::Value& ice_servers = root["iceServers"];
  if (!ice_servers.isArray()) {
    return absl::InvalidArgumentError("pc_config does not contain iceServers");
  }

  for (const Json::Value& server : ice_servers) {
    webrtc::PeerConnectionInterface::IceServer ice_server;
    const Json::Value& urls = server["urls"];
    if (urls.isString()) {
      ice_server.urls.push_back(urls.asString());
    } else if (urls.isArray()) {
      for (const Json::Value& url : urls) {
        if (url.isString()) {
          ice_server.urls.push_back(url.asString());
        }
      }
    }
    if (server["username"].isString()) {
      ice_server.username = server["username"].asString();
    }
    if (server["credential"].isString()) {
      ice_server.password = server["credential"].asString();
    }
    if (!ice_server.urls.empty()) {
      servers.push_back(std::move(ice_server));
    }
  }
  return servers;
}

absl::StatusOr<SignalingMessage> ParseSignalingMessage(
    const std::string& json_text) {
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream stream(json_text);
  if (!Json::parseFromStream(builder, stream, &root, &errors)) {
    return JsonError("failed to parse signaling message", errors);
  }

  SignalingMessage message;
  message.raw_json = json_text;
  const std::string type = root["type"].asString();
  if (type == "offer") {
    message.type = SignalingMessageType::kOffer;
    message.sdp_type = type;
    message.sdp = root["sdp"].asString();
  } else if (type == "answer") {
    message.type = SignalingMessageType::kAnswer;
    message.sdp_type = type;
    message.sdp = root["sdp"].asString();
  } else if (type == "candidate") {
    message.type = SignalingMessageType::kCandidate;
    message.candidate = root["candidate"].asString();
    message.sdp_mid = root["id"].asString();
    message.sdp_mline_index = root["label"].asInt();
  } else if (type == "bye") {
    message.type = SignalingMessageType::kBye;
  } else {
    message.type = SignalingMessageType::kUnknown;
  }
  return message;
}

const char* SignalingMessageTypeToString(SignalingMessageType type) {
  switch (type) {
    case SignalingMessageType::kOffer:
      return "offer";
    case SignalingMessageType::kAnswer:
      return "answer";
    case SignalingMessageType::kCandidate:
      return "candidate";
    case SignalingMessageType::kBye:
      return "bye";
    case SignalingMessageType::kUnknown:
      return "unknown";
  }
  return "unknown";
}

absl::StatusOr<std::string> SerializeSessionDescription(
    const webrtc::SessionDescriptionInterface& description) {
  std::string sdp;
  if (!description.ToString(&sdp)) {
    return absl::InternalError("failed to serialize session description");
  }

  Json::Value root;
  root["type"] = webrtc::SdpTypeToString(description.GetType());
  root["sdp"] = sdp;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, root);
}

absl::StatusOr<std::string> SerializeIceCandidate(
    const webrtc::IceCandidateInterface& candidate) {
  std::string sdp;
  if (!candidate.ToString(&sdp)) {
    return absl::InternalError("failed to serialize ice candidate");
  }

  Json::Value root;
  root["type"] = "candidate";
  root["label"] = candidate.sdp_mline_index();
  root["id"] = candidate.sdp_mid();
  root["candidate"] = sdp;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, root);
}

}  // namespace webrtc_apprtc_cli
