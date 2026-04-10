#include "apprtc_call.h"

#include <curl/curl.h>

#include <algorithm>
#include <deque>
#include <optional>

#include "absl/cleanup/cleanup.h"
#include "absl/log/initialize.h"
#include "api/make_ref_counted.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "api/units/time_delta.h"
#include "apprtc_peer_connection.h"
#include "apprtc_signaling_client.h"
#include "file_video_source.h"
#include "ixwebsocket/IXNetSystem.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/task_utils/repeating_task.h"
#include "rtc_base/thread.h"

namespace webrtc_apprtc_cli {

namespace {

enum class CallState {
  kStarting,
  kJoiningRoom,
  kConnectingWebSocket,
  kRegistering,
  kNegotiating,
  kConnected,
  kStopping,
  kFinished,
};

enum class TerminalReason {
  kLocalDurationReached,
  kRemoteByeAfterEstablished,
  kRemoteByeBeforeEstablished,
  kStartupTimeout,
  kSignalingError,
  kPeerConnectionError,
  kMediaError,
};

int QueuedMessagePriority(SignalingMessageType type) {
  switch (type) {
    case SignalingMessageType::kOffer:
    case SignalingMessageType::kAnswer:
      return 0;
    case SignalingMessageType::kCandidate:
      return 1;
    case SignalingMessageType::kBye:
      return 2;
    case SignalingMessageType::kUnknown:
      return 3;
  }
  return 3;
}

absl::StatusOr<webrtc::LoggingSeverity> LogSeverityFromConfig(
    const AppConfig& config) {
  if (config.log_severity == "verbose") {
    return webrtc::LS_VERBOSE;
  }
  if (config.log_severity == "info") {
    return webrtc::LS_INFO;
  }
  if (config.log_severity == "warning") {
    return webrtc::LS_WARNING;
  }
  if (config.log_severity == "error") {
    return webrtc::LS_ERROR;
  }
  if (config.log_severity == "none") {
    return webrtc::LS_NONE;
  }
  return absl::InvalidArgumentError("invalid log severity");
}

const char* CallStateToString(CallState state) {
  switch (state) {
    case CallState::kStarting:
      return "starting";
    case CallState::kJoiningRoom:
      return "joining_room";
    case CallState::kConnectingWebSocket:
      return "connecting_websocket";
    case CallState::kRegistering:
      return "registering";
    case CallState::kNegotiating:
      return "negotiating";
    case CallState::kConnected:
      return "connected";
    case CallState::kStopping:
      return "stopping";
    case CallState::kFinished:
      return "finished";
  }
  return "unknown";
}

const char* TerminalReasonToString(TerminalReason reason) {
  switch (reason) {
    case TerminalReason::kLocalDurationReached:
      return "local_duration_reached";
    case TerminalReason::kRemoteByeAfterEstablished:
      return "remote_bye_after_established";
    case TerminalReason::kRemoteByeBeforeEstablished:
      return "remote_bye_before_established";
    case TerminalReason::kStartupTimeout:
      return "startup_timeout";
    case TerminalReason::kSignalingError:
      return "signaling_error";
    case TerminalReason::kPeerConnectionError:
      return "peer_connection_error";
    case TerminalReason::kMediaError:
      return "media_error";
  }
  return "unknown";
}

ExitCode ExitCodeForReason(TerminalReason reason) {
  switch (reason) {
    case TerminalReason::kLocalDurationReached:
    case TerminalReason::kRemoteByeAfterEstablished:
      return ExitCode::kSuccess;
    case TerminalReason::kRemoteByeBeforeEstablished:
    case TerminalReason::kSignalingError:
      return ExitCode::kSignalingFailed;
    case TerminalReason::kStartupTimeout:
      return ExitCode::kTimeout;
    case TerminalReason::kPeerConnectionError:
      return ExitCode::kPeerConnectionFailed;
    case TerminalReason::kMediaError:
      return ExitCode::kMediaFailed;
  }
  return ExitCode::kRuntimeFailed;
}

std::string BuildByeMessage() {
  return R"({"type":"bye"})";
}

class CallController {
 public:
  CallController(const AppConfig& config, webrtc::Thread* signaling_thread)
      : config_(config),
        signaling_thread_(signaling_thread),
        signaling_client_(config.trace_signaling, signaling_thread_),
        peer_client_(config, signaling_thread_) {
    RTC_CHECK(signaling_thread_ != nullptr);
    RTC_CHECK(signaling_thread_->IsCurrent());
    signaling_client_.SetEventCallbacks(
        [this] { OnWebSocketOpen(); },
        [this](SignalingMessage message) {
          OnInboundSignalingMessage(std::move(message));
        },
        [this](absl::Status status) { OnSignalingError(std::move(status)); });
    peer_client_.SetMessageCallback([this](const std::string& payload) {
      OnLocalSignalingMessage(payload);
    });
    peer_client_.SetConnectionCallback(
        [this](bool connected) { OnPeerConnectionChanged(connected); });
    peer_client_.SetRemoteMediaCallback([this] { OnRemoteMediaSeen(); });
    peer_client_.SetErrorCallback([this](absl::Status status) {
      OnPeerConnectionError(std::move(status));
    });
  }

  int Run() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    Start();
    signaling_thread_->ProcessMessages(webrtc::Thread::kForever);
    return exit_code_;
  }

 private:
  void Start() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    SetState(CallState::kJoiningRoom);
    ScheduleStartupTimeout();
    signaling_client_.JoinRoomAsync(
        config_.room_server_url, config_.room_id,
        [this](absl::StatusOr<JoinResponse> join_response) mutable {
          OnJoinComplete(std::move(join_response));
        });
  }

  void SetState(CallState state) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (state_ == state) {
      return;
    }
    state_ = state;
    if (config_.trace_signaling) {
      RTC_LOG(LS_INFO) << "state=" << CallStateToString(state_);
    }
  }

  void ScheduleStartupTimeout() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    ++startup_timeout_generation_;
    const int generation = startup_timeout_generation_;
    signaling_thread_->PostDelayedTask(
        webrtc::SafeTask(task_safety_.flag(),
                         [this, generation] {
                           RTC_CHECK(signaling_thread_->IsCurrent());
                           if (finished_ ||
                               generation != startup_timeout_generation_ ||
                               call_established_) {
                             return;
                           }
                           Finish(TerminalReason::kStartupTimeout,
                                  absl::DeadlineExceededError(
                                      "timed out waiting for call completion"));
                         }),
        webrtc::TimeDelta::Seconds(config_.timeout_seconds));
  }

  void CancelStartupTimeout() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    ++startup_timeout_generation_;
  }

  void ScheduleCallDurationTimer() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    ++call_duration_generation_;
    const int generation = call_duration_generation_;
    signaling_thread_->PostDelayedTask(
        webrtc::SafeTask(
            task_safety_.flag(),
            [this, generation] {
              RTC_CHECK(signaling_thread_->IsCurrent());
              if (finished_ || generation != call_duration_generation_ ||
                  !call_established_) {
                return;
              }
              RTC_LOG(LS_INFO) << "call duration reached";
              Finish(TerminalReason::kLocalDurationReached, absl::OkStatus());
            }),
        webrtc::TimeDelta::Seconds(config_.call_duration_seconds));
  }

  void CancelCallDurationTimer() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    ++call_duration_generation_;
  }

  void StartStatsTimer() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (stats_task_.Running()) {
      return;
    }
    stats_task_ = webrtc::RepeatingTaskHandle::DelayedStart(
        signaling_thread_,
        webrtc::TimeDelta::Seconds(config_.stats_interval_seconds), [this] {
          RTC_CHECK(signaling_thread_->IsCurrent());
          if (!finished_) {
            peer_client_.LogStats();
          }
          return webrtc::TimeDelta::Seconds(config_.stats_interval_seconds);
        });
  }

  void StopStatsTimer() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (stats_task_.Running()) {
      stats_task_.Stop();
    }
  }

  void OnJoinComplete(absl::StatusOr<JoinResponse> join_response) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_) {
      return;
    }
    if (!join_response.ok()) {
      Finish(TerminalReason::kSignalingError, join_response.status());
      return;
    }

    join_response_ = *join_response;
    if (config_.trace_signaling) {
      RTC_LOG(LS_INFO) << "joined as "
                       << (join_response_->is_initiator ? "initiator"
                                                        : "non-initiator");
    }

    if (absl::Status status = peer_client_.Initialize(*join_response_);
        !status.ok()) {
      Finish(TerminalReason::kPeerConnectionError, std::move(status));
      return;
    }
    if (absl::Status status = peer_client_.StartMedia(); !status.ok()) {
      Finish(TerminalReason::kMediaError, std::move(status));
      return;
    }

    QueueJoinMessages();
    SetState(CallState::kConnectingWebSocket);
    signaling_client_.ConnectWebSocketAsync(*join_response_);
  }

  void QueueJoinMessages() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    RTC_CHECK(join_response_.has_value());
    if (config_.trace_signaling) {
      RTC_LOG(LS_INFO) << "processing queued signaling messages count="
                       << join_response_->messages.size();
    }
    std::vector<SignalingMessage> queued_messages;
    queued_messages.reserve(join_response_->messages.size());
    for (const std::string& queued : join_response_->messages) {
      auto parsed = ParseSignalingMessage(queued);
      if (!parsed.ok()) {
        RTC_LOG(LS_WARNING) << parsed.status().message();
        continue;
      }
      if (config_.trace_signaling) {
        RTC_LOG(LS_INFO) << "queued signaling message type="
                         << SignalingMessageTypeToString(parsed->type);
      }
      queued_messages.push_back(*parsed);
    }
    std::stable_sort(
        queued_messages.begin(), queued_messages.end(),
        [](const SignalingMessage& lhs, const SignalingMessage& rhs) {
          return QueuedMessagePriority(lhs.type) <
                 QueuedMessagePriority(rhs.type);
        });
    for (SignalingMessage& message : queued_messages) {
      pending_signaling_messages_.push_back(std::move(message));
    }
  }

  void OnWebSocketOpen() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_ || !join_response_.has_value()) {
      return;
    }
    SetState(CallState::kRegistering);
    signaling_client_.RegisterAsync(*join_response_,
                                    [this](absl::Status status) mutable {
                                      OnRegisterComplete(std::move(status));
                                    });
  }

  void OnRegisterComplete(absl::Status status) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_) {
      return;
    }
    if (!status.ok()) {
      Finish(TerminalReason::kSignalingError, std::move(status));
      return;
    }
    negotiation_ready_ = true;
    StartStatsTimer();
    PumpSignalingMessages();
  }

  void OnInboundSignalingMessage(SignalingMessage message) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_) {
      return;
    }
    if (config_.trace_signaling) {
      RTC_LOG(LS_INFO) << "handling websocket signaling type="
                       << SignalingMessageTypeToString(message.type);
    }
    pending_signaling_messages_.push_back(std::move(message));
    PumpSignalingMessages();
  }

  void PumpSignalingMessages() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_ || !negotiation_ready_ || signaling_action_in_flight_ ||
        !join_response_.has_value()) {
      return;
    }

    while (!finished_ && negotiation_ready_ && !signaling_action_in_flight_) {
      if (!pending_signaling_messages_.empty()) {
        SignalingMessage message =
            std::move(pending_signaling_messages_.front());
        pending_signaling_messages_.pop_front();
        if (message.type == SignalingMessageType::kBye) {
          if (call_established_) {
            RTC_LOG(LS_INFO) << "remote hangup after established call";
            Finish(
                TerminalReason::kRemoteByeAfterEstablished,
                absl::CancelledError("remote hangup after established call"));
          } else {
            Finish(TerminalReason::kRemoteByeBeforeEstablished,
                   absl::FailedPreconditionError(
                       "remote hangup before call establishment"));
          }
          return;
        }
        if (message.type == SignalingMessageType::kCandidate) {
          absl::Status status = peer_client_.AddRemoteCandidate(message);
          if (!status.ok()) {
            Finish(TerminalReason::kPeerConnectionError, std::move(status));
            return;
          }
          continue;
        }
        if (message.type == SignalingMessageType::kAnswer) {
          SetState(CallState::kNegotiating);
          signaling_action_in_flight_ = true;
          peer_client_.SetRemoteAnswer(
              message, [this](absl::Status status) mutable {
                RTC_CHECK(signaling_thread_->IsCurrent());
                signaling_action_in_flight_ = false;
                if (!status.ok()) {
                  Finish(TerminalReason::kPeerConnectionError,
                         std::move(status));
                  return;
                }
                PumpSignalingMessages();
              });
          return;
        }
        if (message.type == SignalingMessageType::kOffer) {
          SetState(CallState::kNegotiating);
          signaling_action_in_flight_ = true;
          peer_client_.SetRemoteOfferAndCreateAnswer(
              message, [this](absl::StatusOr<std::string> answer) mutable {
                RTC_CHECK(signaling_thread_->IsCurrent());
                if (!answer.ok()) {
                  signaling_action_in_flight_ = false;
                  Finish(TerminalReason::kPeerConnectionError, answer.status());
                  return;
                }
                SendLocalDescription(std::move(*answer));
              });
          return;
        }
        Finish(TerminalReason::kSignalingError,
               absl::UnimplementedError("unknown signaling message"));
        return;
      }

      if (join_response_->is_initiator && !offer_started_) {
        SetState(CallState::kNegotiating);
        offer_started_ = true;
        signaling_action_in_flight_ = true;
        peer_client_.CreateOffer(
            [this](absl::StatusOr<std::string> offer) mutable {
              RTC_CHECK(signaling_thread_->IsCurrent());
              if (!offer.ok()) {
                signaling_action_in_flight_ = false;
                Finish(TerminalReason::kPeerConnectionError, offer.status());
                return;
              }
              SendLocalDescription(std::move(*offer));
            });
      }
      return;
    }
  }

  void SendLocalDescription(std::string payload) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    RTC_CHECK(join_response_.has_value());
    if (join_response_->is_initiator) {
      signaling_client_.SendToRoomAsync(
          config_.room_server_url, join_response_->room_id,
          join_response_->client_id, payload,
          [this](absl::Status status) mutable {
            RTC_CHECK(signaling_thread_->IsCurrent());
            signaling_action_in_flight_ = false;
            if (!status.ok()) {
              Finish(TerminalReason::kSignalingError, std::move(status));
              return;
            }
            PumpSignalingMessages();
          });
      return;
    }

    signaling_client_.SendWebSocketMessageAsync(
        payload, [this](absl::Status status) mutable {
          RTC_CHECK(signaling_thread_->IsCurrent());
          signaling_action_in_flight_ = false;
          if (!status.ok()) {
            Finish(TerminalReason::kSignalingError, std::move(status));
            return;
          }
          PumpSignalingMessages();
        });
  }

  void OnLocalSignalingMessage(const std::string& payload) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_ || !join_response_.has_value()) {
      return;
    }
    if (join_response_->is_initiator) {
      signaling_client_.SendToRoomAsync(
          config_.room_server_url, join_response_->room_id,
          join_response_->client_id, payload,
          [this](absl::Status status) mutable {
            RTC_CHECK(signaling_thread_->IsCurrent());
            if (!status.ok()) {
              Finish(TerminalReason::kSignalingError, std::move(status));
            }
          });
      return;
    }

    signaling_client_.SendWebSocketMessageAsync(
        payload, [this](absl::Status status) mutable {
          RTC_CHECK(signaling_thread_->IsCurrent());
          if (!status.ok()) {
            Finish(TerminalReason::kSignalingError, std::move(status));
          }
        });
  }

  void OnPeerConnectionChanged(bool connected) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_) {
      return;
    }
    peer_connected_ = connected;
    MaybeMarkCallEstablished();
  }

  void OnRemoteMediaSeen() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_) {
      return;
    }
    remote_media_seen_ = true;
    MaybeMarkCallEstablished();
  }

  void MaybeMarkCallEstablished() {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (call_established_ || !peer_connected_ || !remote_media_seen_) {
      return;
    }
    call_established_ = true;
    CancelStartupTimeout();
    SetState(CallState::kConnected);
    RTC_LOG(LS_INFO) << "call established";
    ScheduleCallDurationTimer();
  }

  void OnSignalingError(absl::Status status) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_) {
      return;
    }
    Finish(TerminalReason::kSignalingError, std::move(status));
  }

  void OnPeerConnectionError(absl::Status status) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_) {
      return;
    }
    Finish(TerminalReason::kPeerConnectionError, std::move(status));
  }

  void Finish(TerminalReason reason, absl::Status status) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    if (finished_) {
      return;
    }

    finished_ = true;
    SetState(CallState::kStopping);
    CancelStartupTimeout();
    CancelCallDurationTimer();
    StopStatsTimer();

    if (reason == TerminalReason::kLocalDurationReached &&
        join_response_.has_value()) {
      SendByeAndFinish(std::move(status));
      return;
    }

    CompleteFinish(reason, std::move(status));
  }

  void SendByeAndFinish(absl::Status status) {
    RTC_CHECK(signaling_thread_->IsCurrent());
    RTC_CHECK(join_response_.has_value());
    const std::string bye_message = BuildByeMessage();
    auto callback = [this, status = std::move(status)](
                        absl::Status send_status) mutable {
      RTC_CHECK(signaling_thread_->IsCurrent());
      if (!send_status.ok()) {
        RTC_LOG(LS_WARNING)
            << "failed to send bye before shutdown: " << send_status.message();
      }
      CompleteFinish(TerminalReason::kLocalDurationReached, std::move(status));
    };
    if (join_response_->is_initiator) {
      signaling_client_.SendToRoomAsync(
          config_.room_server_url, join_response_->room_id,
          join_response_->client_id, bye_message, std::move(callback));
      return;
    }
    signaling_client_.SendWebSocketMessageAsync(bye_message,
                                                std::move(callback));
  }

  void CompleteFinish(TerminalReason reason, absl::Status status) {
    RTC_CHECK(signaling_thread_->IsCurrent());

    if (status.ok()) {
      RTC_LOG(LS_INFO) << "terminal reason=" << TerminalReasonToString(reason);
    } else if (ExitCodeForReason(reason) == ExitCode::kSuccess) {
      RTC_LOG(LS_INFO) << "terminal reason=" << TerminalReasonToString(reason)
                       << " detail=" << status.message();
    } else {
      RTC_LOG(LS_ERROR) << "terminal reason=" << TerminalReasonToString(reason)
                        << " detail=" << status.message();
    }

    peer_client_.Close();
    if (join_response_.has_value()) {
      signaling_client_.CloseAsync(*join_response_);
    }

    exit_code_ = static_cast<int>(ExitCodeForReason(reason));
    SetState(CallState::kFinished);
    signaling_thread_->Quit();
  }

  const AppConfig config_;
  webrtc::Thread* const signaling_thread_;
  webrtc::ScopedTaskSafety task_safety_;
  AppRtcSignalingClient signaling_client_;
  PeerConnectionClient peer_client_;
  std::optional<JoinResponse> join_response_;
  std::deque<SignalingMessage> pending_signaling_messages_;
  webrtc::RepeatingTaskHandle stats_task_;
  CallState state_ = CallState::kStarting;
  int exit_code_ = static_cast<int>(ExitCode::kRuntimeFailed);
  int startup_timeout_generation_ = 0;
  int call_duration_generation_ = 0;
  bool negotiation_ready_ = false;
  bool signaling_action_in_flight_ = false;
  bool offer_started_ = false;
  bool peer_connected_ = false;
  bool remote_media_seen_ = false;
  bool call_established_ = false;
  bool finished_ = false;
};

}  // namespace

int RunAppRtcCall(const AppConfig& config) {
  absl::InitializeLog();
  if (absl::Status status = ValidateConfig(config); !status.ok()) {
    RTC_LOG(LS_ERROR) << status.message();
    return static_cast<int>(ExitCode::kInvalidArgument);
  }
  auto log_severity = LogSeverityFromConfig(config);
  if (!log_severity.ok()) {
    RTC_LOG(LS_ERROR) << log_severity.status().message();
    return static_cast<int>(ExitCode::kInvalidArgument);
  }
  webrtc::LogMessage::LogToDebug(*log_severity);
  webrtc::LogMessage::SetLogToStderr(true);
  webrtc::LogMessage::LogThreads(true);
  webrtc::LogMessage::LogTimestamps(true);

  curl_global_init(CURL_GLOBAL_DEFAULT);
  auto curl_cleanup = absl::Cleanup([] { curl_global_cleanup(); });
  ix::initNetSystem();
  auto ix_cleanup = absl::Cleanup([] { ix::uninitNetSystem(); });
  if (!webrtc::InitializeSSL()) {
    RTC_LOG(LS_ERROR) << "InitializeSSL failed";
    return static_cast<int>(ExitCode::kInitializationFailed);
  }
  auto ssl_cleanup = absl::Cleanup([] { webrtc::CleanupSSL(); });

  if (config.validate_inputs_only) {
    auto source = webrtc::make_ref_counted<FileVideoSource>(config.video_path);
    if (absl::Status status = source->Start(); !status.ok()) {
      RTC_LOG(LS_ERROR) << status.message();
      return static_cast<int>(ExitCode::kMediaFailed);
    }
    RTC_LOG(LS_INFO) << "input validation succeeded";
    return static_cast<int>(ExitCode::kSuccess);
  }

  webrtc::Thread* signaling_thread = webrtc::Thread::Current();
  bool unwrap_current_thread = false;
  if (signaling_thread == nullptr) {
    signaling_thread = webrtc::ThreadManager::Instance()->WrapCurrentThread();
    if (signaling_thread == nullptr) {
      RTC_LOG(LS_ERROR) << "failed to wrap current thread";
      return static_cast<int>(ExitCode::kInitializationFailed);
    }
    unwrap_current_thread = true;
  }
  auto unwrap_cleanup = absl::Cleanup([unwrap_current_thread] {
    if (unwrap_current_thread) {
      webrtc::ThreadManager::Instance()->UnwrapCurrentThread();
    }
  });

  CallController controller(config, signaling_thread);
  return controller.Run();
}

}  // namespace webrtc_apprtc_cli
