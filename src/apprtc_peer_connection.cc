#include "apprtc_peer_connection.h"

#include <memory>
#include <utility>

#include "api/audio_options.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media_with_defaults.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "apprtc_common.h"
#include "media/engine/internal_decoder_factory.h"
#include "media/engine/internal_encoder_factory.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "sine_wave_capturer.h"

namespace webrtc_apprtc_cli {

namespace {

absl::StatusOr<std::unique_ptr<webrtc::SessionDescriptionInterface>>
ParseSessionDescription(const SignalingMessage& message,
                        webrtc::SdpType expected_type) {
  webrtc::SdpParseError error;
  auto description =
      webrtc::CreateSessionDescription(expected_type, message.sdp, &error);
  if (!description) {
    return absl::InvalidArgumentError(error.description);
  }
  return description;
}

}  // namespace

class PeerConnectionClient::RemoteFrameSink final
    : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit RemoteFrameSink(PeerConnectionClient* owner) : owner_(owner) {}

  void OnFrame(const webrtc::VideoFrame& frame) override {
    (void)frame;
    RTC_DCHECK(owner_ != nullptr);
    owner_->signaling_thread_->PostTask(
        webrtc::SafeTask(owner_->task_safety_.flag(), [owner = owner_]() {
          RTC_DCHECK_RUN_ON(&owner->signaling_sequence_);
          if (owner->remote_media_seen_) {
            return;
          }
          owner->remote_media_seen_ = true;
          if (owner->remote_media_callback_) {
            owner->remote_media_callback_();
          }
        }));
  }

 private:
  PeerConnectionClient* const owner_;
};

class PeerConnectionClient::CreateDescriptionObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  using Callback = absl::AnyInvocable<void(
      absl::StatusOr<std::unique_ptr<webrtc::SessionDescriptionInterface>>) &&>;

  CreateDescriptionObserver(
      webrtc::Thread* signaling_thread,
      webrtc::scoped_refptr<webrtc::PendingTaskSafetyFlag> safety,
      Callback callback)
      : signaling_thread_(signaling_thread),
        safety_(std::move(safety)),
        callback_(std::move(callback)) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    signaling_thread_->PostTask(webrtc::SafeTask(
        safety_,
        [callback = std::move(callback_),
         description = std::unique_ptr<webrtc::SessionDescriptionInterface>(
             desc)]() mutable {
          std::move(callback)(std::move(description));
        }));
  }

  void OnFailure(webrtc::RTCError error) override {
    signaling_thread_->PostTask(webrtc::SafeTask(
        safety_, [callback = std::move(callback_),
                  status = absl::InternalError(error.message())]() mutable {
          std::move(callback)(status);
        }));
  }

 private:
  webrtc::Thread* const signaling_thread_;
  webrtc::scoped_refptr<webrtc::PendingTaskSafetyFlag> safety_;
  Callback callback_;
};

class PeerConnectionClient::SetLocalObserver
    : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  SetLocalObserver(webrtc::Thread* signaling_thread,
                   webrtc::scoped_refptr<webrtc::PendingTaskSafetyFlag> safety,
                   StatusCallback callback)
      : signaling_thread_(signaling_thread),
        safety_(std::move(safety)),
        callback_(std::move(callback)) {}

  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    signaling_thread_->PostTask(webrtc::SafeTask(
        safety_,
        [callback = std::move(callback_),
         status = error.ok() ? absl::OkStatus()
                             : absl::InternalError(error.message())]() mutable {
          std::move(callback)(std::move(status));
        }));
  }

 private:
  webrtc::Thread* const signaling_thread_;
  webrtc::scoped_refptr<webrtc::PendingTaskSafetyFlag> safety_;
  StatusCallback callback_;
};

class PeerConnectionClient::SetRemoteObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  SetRemoteObserver(webrtc::Thread* signaling_thread,
                    webrtc::scoped_refptr<webrtc::PendingTaskSafetyFlag> safety,
                    StatusCallback callback)
      : signaling_thread_(signaling_thread),
        safety_(std::move(safety)),
        callback_(std::move(callback)) {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    signaling_thread_->PostTask(webrtc::SafeTask(
        safety_,
        [callback = std::move(callback_),
         status = error.ok() ? absl::OkStatus()
                             : absl::InternalError(error.message())]() mutable {
          std::move(callback)(std::move(status));
        }));
  }

 private:
  webrtc::Thread* const signaling_thread_;
  webrtc::scoped_refptr<webrtc::PendingTaskSafetyFlag> safety_;
  StatusCallback callback_;
};

class PeerConnectionClient::StatsCollector
    : public webrtc::RTCStatsCollectorCallback {
 public:
  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report)
      override {
    if (!report) {
      return;
    }

    auto outbound = report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>();
    for (const auto* stats : outbound) {
      RTC_LOG(LS_INFO) << "outbound kind=" << (stats->kind ? *stats->kind : "")
                       << " bytes_sent="
                       << (stats->bytes_sent
                               ? std::to_string(*stats->bytes_sent)
                               : "n/a")
                       << " fps="
                       << (stats->frames_per_second
                               ? std::to_string(*stats->frames_per_second)
                               : "n/a");
    }

    auto remote_inbound =
        report->GetStatsOfType<webrtc::RTCRemoteInboundRtpStreamStats>();
    for (const auto* stats : remote_inbound) {
      RTC_LOG(LS_INFO) << "remote_inbound kind="
                       << (stats->kind ? *stats->kind : "") << " rtt="
                       << (stats->round_trip_time
                               ? std::to_string(*stats->round_trip_time)
                               : "n/a")
                       << " packets_lost="
                       << (stats->packets_lost
                               ? std::to_string(*stats->packets_lost)
                               : "n/a");
    }

    auto transports = report->GetStatsOfType<webrtc::RTCTransportStats>();
    for (const auto* stats : transports) {
      RTC_LOG(LS_INFO) << "transport dtls_state="
                       << (stats->dtls_state ? *stats->dtls_state : "")
                       << " ice_state="
                       << (stats->ice_state ? *stats->ice_state : "")
                       << " selected_pair="
                       << (stats->selected_candidate_pair_id
                               ? *stats->selected_candidate_pair_id
                               : "");
    }
  }
};

PeerConnectionClient::PeerConnectionClient(const AppConfig& config,
                                           webrtc::Thread* signaling_thread)
    : config_(config),
      signaling_thread_(signaling_thread),
      env_(webrtc::CreateEnvironment()) {
  RTC_CHECK(signaling_thread_ != nullptr);
}

PeerConnectionClient::~PeerConnectionClient() {
  Close();
}

absl::Status PeerConnectionClient::Initialize(
    const JoinResponse& join_response) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  join_response_ = join_response;

  network_thread_ = webrtc::Thread::CreateWithSocketServer();
  worker_thread_ = webrtc::Thread::Create();
  network_thread_->SetName("apprtc-network", nullptr);
  worker_thread_->SetName("apprtc-worker", nullptr);

  if (!network_thread_->Start() || !worker_thread_->Start()) {
    return absl::InternalError("failed to start webrtc threads");
  }

  webrtc::PeerConnectionFactoryDependencies deps;
  deps.env = env_;
  deps.network_thread = network_thread_.get();
  deps.worker_thread = worker_thread_.get();
  deps.signaling_thread = signaling_thread_;
  deps.video_encoder_factory =
      std::make_unique<webrtc::InternalEncoderFactory>();
  deps.video_decoder_factory =
      std::make_unique<webrtc::InternalDecoderFactory>();
  webrtc::EnableMediaWithDefaults(deps);
  deps.adm = CreateSineWaveAudioDevice(env_, config_.audio_sample_rate_hz,
                                       config_.audio_channels,
                                       config_.audio_frequency_hz);

  factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  if (!factory_) {
    return absl::InternalError("failed to create peer connection factory");
  }

  auto ice_servers = ParseIceServers(join_response.pc_config_json);
  if (!ice_servers.ok()) {
    return ice_servers.status();
  }

  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
  rtc_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  rtc_config.servers = *ice_servers;
  webrtc::PeerConnectionDependencies peer_deps(this);
  auto result =
      factory_->CreatePeerConnectionOrError(rtc_config, std::move(peer_deps));
  if (!result.ok()) {
    return absl::InternalError(result.error().message());
  }
  connection_ = result.MoveValue();
  remote_frame_sink_ = std::make_unique<RemoteFrameSink>(this);
  connected_ = false;
  remote_media_seen_ = false;
  remote_description_set_ = false;
  pending_remote_candidates_.clear();
  return absl::OkStatus();
}

absl::Status PeerConnectionClient::StartMedia() {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  video_source_ = webrtc::make_ref_counted<FileVideoSource>(config_.video_path);
  absl::Status status = video_source_->Start();
  if (!status.ok()) {
    return status;
  }

  auto video_track = factory_->CreateVideoTrack(video_source_, "video-file");
  auto video_result = connection_->AddTrack(video_track, {"media-stream"});
  if (!video_result.ok()) {
    return absl::InternalError(video_result.error().message());
  }

  auto audio_source = factory_->CreateAudioSource(webrtc::AudioOptions());
  auto audio_track =
      factory_->CreateAudioTrack("tone-audio", audio_source.get());
  auto audio_result = connection_->AddTrack(audio_track, {"media-stream"});
  if (!audio_result.ok()) {
    return absl::InternalError(audio_result.error().message());
  }
  return absl::OkStatus();
}

void PeerConnectionClient::CreateOffer(DescriptionCallback callback) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  auto observer = webrtc::make_ref_counted<CreateDescriptionObserver>(
      signaling_thread_, task_safety_.flag(),
      [this, callback = std::move(callback)](
          absl::StatusOr<std::unique_ptr<webrtc::SessionDescriptionInterface>>
              description) mutable {
        RTC_DCHECK_RUN_ON(&signaling_sequence_);
        if (!description.ok()) {
          std::move(callback)(description.status());
          return;
        }
        auto json = SerializeSessionDescription(**description);
        if (!json.ok()) {
          std::move(callback)(json.status());
          return;
        }
        ApplyLocalDescription(std::move(*description),
                              [callback = std::move(callback),
                               json = *json](absl::Status status) mutable {
                                if (!status.ok()) {
                                  std::move(callback)(std::move(status));
                                  return;
                                }
                                std::move(callback)(std::move(json));
                              });
      });
  connection_->CreateOffer(
      observer.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void PeerConnectionClient::CreateAnswer(DescriptionCallback callback) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  auto observer = webrtc::make_ref_counted<CreateDescriptionObserver>(
      signaling_thread_, task_safety_.flag(),
      [this, callback = std::move(callback)](
          absl::StatusOr<std::unique_ptr<webrtc::SessionDescriptionInterface>>
              description) mutable {
        RTC_DCHECK_RUN_ON(&signaling_sequence_);
        if (!description.ok()) {
          std::move(callback)(description.status());
          return;
        }
        auto json = SerializeSessionDescription(**description);
        if (!json.ok()) {
          std::move(callback)(json.status());
          return;
        }
        ApplyLocalDescription(std::move(*description),
                              [callback = std::move(callback),
                               json = *json](absl::Status status) mutable {
                                if (!status.ok()) {
                                  std::move(callback)(std::move(status));
                                  return;
                                }
                                std::move(callback)(std::move(json));
                              });
      });
  connection_->CreateAnswer(
      observer.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void PeerConnectionClient::ApplyLocalDescription(
    std::unique_ptr<webrtc::SessionDescriptionInterface> description,
    StatusCallback callback) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  auto observer = webrtc::make_ref_counted<SetLocalObserver>(
      signaling_thread_, task_safety_.flag(), std::move(callback));
  connection_->SetLocalDescription(std::move(description), observer);
}

void PeerConnectionClient::ApplyRemoteDescription(
    std::unique_ptr<webrtc::SessionDescriptionInterface> description,
    StatusCallback callback) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  auto observer = webrtc::make_ref_counted<SetRemoteObserver>(
      signaling_thread_, task_safety_.flag(),
      [this, callback = std::move(callback)](absl::Status status) mutable {
        RTC_DCHECK_RUN_ON(&signaling_sequence_);
        if (!status.ok()) {
          std::move(callback)(std::move(status));
          return;
        }
        remote_description_set_ = true;
        absl::Status flush_status = FlushPendingRemoteCandidates();
        if (!flush_status.ok()) {
          std::move(callback)(std::move(flush_status));
          return;
        }
        std::move(callback)(absl::OkStatus());
      });
  connection_->SetRemoteDescription(std::move(description), observer);
}

void PeerConnectionClient::SetRemoteOfferAndCreateAnswer(
    const SignalingMessage& message,
    DescriptionCallback callback) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  auto description = ParseSessionDescription(message, webrtc::SdpType::kOffer);
  if (!description.ok()) {
    std::move(callback)(description.status());
    return;
  }
  ApplyRemoteDescription(
      std::move(*description),
      [this, callback = std::move(callback)](absl::Status status) mutable {
        RTC_DCHECK_RUN_ON(&signaling_sequence_);
        if (!status.ok()) {
          std::move(callback)(std::move(status));
          return;
        }
        CreateAnswer(std::move(callback));
      });
}

void PeerConnectionClient::SetRemoteAnswer(const SignalingMessage& message,
                                           StatusCallback callback) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  auto description = ParseSessionDescription(message, webrtc::SdpType::kAnswer);
  if (!description.ok()) {
    std::move(callback)(description.status());
    return;
  }
  ApplyRemoteDescription(std::move(*description), std::move(callback));
}

absl::Status PeerConnectionClient::AddRemoteCandidate(
    const SignalingMessage& message) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  if (!HasRemoteDescription()) {
    QueueRemoteCandidate(message);
    if (config_.trace_signaling) {
      RTC_LOG(LS_INFO)
          << "buffered remote candidate until remote description is set";
    }
    return absl::OkStatus();
  }

  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::IceCandidateInterface> candidate(
      webrtc::CreateIceCandidate(message.sdp_mid, message.sdp_mline_index,
                                 message.candidate, &error));
  if (!candidate) {
    return absl::InvalidArgumentError(error.description);
  }
  if (!connection_->AddIceCandidate(candidate.get())) {
    return absl::InternalError("AddIceCandidate failed");
  }
  if (config_.trace_signaling) {
    RTC_LOG(LS_INFO) << "applied remote candidate mid=" << message.sdp_mid
                     << " mline=" << message.sdp_mline_index;
  }
  return absl::OkStatus();
}

bool PeerConnectionClient::HasRemoteDescription() const {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  return remote_description_set_;
}

void PeerConnectionClient::QueueRemoteCandidate(
    const SignalingMessage& message) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  pending_remote_candidates_.push_back(message);
}

absl::Status PeerConnectionClient::FlushPendingRemoteCandidates() {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  if (pending_remote_candidates_.empty()) {
    return absl::OkStatus();
  }

  RTC_LOG(LS_INFO) << "flushing " << pending_remote_candidates_.size()
                   << " buffered remote ICE candidates";
  for (const SignalingMessage& message : pending_remote_candidates_) {
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate(message.sdp_mid, message.sdp_mline_index,
                                   message.candidate, &error));
    if (!candidate) {
      return absl::InvalidArgumentError(error.description);
    }
    if (!connection_->AddIceCandidate(candidate.get())) {
      return absl::InternalError("AddIceCandidate failed while flushing");
    }
    if (config_.trace_signaling) {
      RTC_LOG(LS_INFO) << "flushed remote candidate mid=" << message.sdp_mid
                       << " mline=" << message.sdp_mline_index;
    }
  }
  pending_remote_candidates_.clear();
  return absl::OkStatus();
}

void PeerConnectionClient::ReportError(absl::Status status) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  if (error_callback_) {
    error_callback_(std::move(status));
  }
}

void PeerConnectionClient::Close() {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  task_safety_.reset();
  if (connection_) {
    connection_->Close();
    connection_ = nullptr;
  }
  pending_remote_candidates_.clear();
  remote_description_set_ = false;
  connected_ = false;
  remote_media_seen_ = false;
  video_source_ = nullptr;
  factory_ = nullptr;
  remote_frame_sink_.reset();
}

void PeerConnectionClient::LogStats() {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  if (!connection_) {
    return;
  }
  auto collector = webrtc::make_ref_counted<StatsCollector>();
  connection_->GetStats(collector.get());
}

void PeerConnectionClient::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  RTC_LOG(LS_INFO) << "signaling="
                   << webrtc::PeerConnectionInterface::AsString(new_state);
}

void PeerConnectionClient::OnRenegotiationNeeded() {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  RTC_LOG(LS_INFO) << "renegotiation needed";
}

void PeerConnectionClient::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  RTC_LOG(LS_INFO) << "ice_connection="
                   << webrtc::PeerConnectionInterface::AsString(new_state);
}

void PeerConnectionClient::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  RTC_LOG(LS_INFO) << "connection="
                   << webrtc::PeerConnectionInterface::AsString(new_state);
  const bool connected =
      new_state ==
      webrtc::PeerConnectionInterface::PeerConnectionState::kConnected;
  if (connected_ != connected) {
    connected_ = connected;
    if (connection_callback_) {
      connection_callback_(connected_);
    }
  }
  if (new_state ==
      webrtc::PeerConnectionInterface::PeerConnectionState::kFailed) {
    ReportError(absl::InternalError("peer connection entered failed state"));
  }
}

void PeerConnectionClient::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  RTC_LOG(LS_INFO) << "ice_gathering="
                   << webrtc::PeerConnectionInterface::AsString(new_state);
}

void PeerConnectionClient::OnIceCandidate(
    const webrtc::IceCandidateInterface* candidate) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  if (candidate == nullptr || !message_callback_) {
    return;
  }
  auto json = SerializeIceCandidate(*candidate);
  if (!json.ok()) {
    RTC_LOG(LS_WARNING) << json.status().message();
    return;
  }
  message_callback_(*json);
}

void PeerConnectionClient::OnTrack(
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  RTC_DCHECK_RUN_ON(&signaling_sequence_);
  auto track = transceiver->receiver()->track();
  if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind &&
      remote_frame_sink_) {
    static_cast<webrtc::VideoTrackInterface*>(track.get())
        ->AddOrUpdateSink(remote_frame_sink_.get(), webrtc::VideoSinkWants());
    RTC_LOG(LS_INFO) << "attached remote video sink";
  }
}

}  // namespace webrtc_apprtc_cli
