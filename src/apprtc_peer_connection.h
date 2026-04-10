#ifndef WEBRTC_APPRTC_CLI_SRC_APPRTC_PEER_CONNECTION_H_
#define WEBRTC_APPRTC_CLI_SRC_APPRTC_PEER_CONNECTION_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "api/audio/audio_device.h"
#include "api/jsep.h"
#include "api/scoped_refptr.h"
#include "api/sequence_checker.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "api/video/video_sink_interface.h"
#include "apprtc_common.h"
#include "file_video_source.h"
#include "rtc_base/thread.h"

namespace webrtc_apprtc_cli {

class PeerConnectionClient : public webrtc::PeerConnectionObserver {
 public:
  using StatusCallback = absl::AnyInvocable<void(absl::Status) &&>;
  using DescriptionCallback =
      absl::AnyInvocable<void(absl::StatusOr<std::string>) &&>;

  PeerConnectionClient(const AppConfig& config,
                       webrtc::Thread* signaling_thread);
  ~PeerConnectionClient() override;

  absl::Status Initialize(const JoinResponse& join_response);
  absl::Status StartMedia();
  void CreateOffer(DescriptionCallback callback);
  void SetRemoteOfferAndCreateAnswer(const SignalingMessage& message,
                                     DescriptionCallback callback);
  void SetRemoteAnswer(const SignalingMessage& message,
                       StatusCallback callback);
  absl::Status AddRemoteCandidate(const SignalingMessage& message);
  void Close();
  void LogStats();

  void SetMessageCallback(std::function<void(const std::string&)> callback) {
    message_callback_ = std::move(callback);
  }
  void SetConnectionCallback(std::function<void(bool)> callback) {
    connection_callback_ = std::move(callback);
  }
  void SetRemoteMediaCallback(std::function<void()> callback) {
    remote_media_callback_ = std::move(callback);
  }
  void SetErrorCallback(std::function<void(absl::Status)> callback) {
    error_callback_ = std::move(callback);
  }

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override;
  void OnAddStream(
      webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
  void OnRemoveStream(
      webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnRenegotiationNeeded() override;
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>
                   transceiver) override;

 private:
  class SetLocalObserver;
  class SetRemoteObserver;
  class CreateDescriptionObserver;
  class StatsCollector;
  class RemoteFrameSink;

  void CreateAnswer(DescriptionCallback callback);
  void ApplyLocalDescription(
      std::unique_ptr<webrtc::SessionDescriptionInterface> description,
      StatusCallback callback);
  void ApplyRemoteDescription(
      std::unique_ptr<webrtc::SessionDescriptionInterface> description,
      StatusCallback callback);
  bool HasRemoteDescription() const;
  void QueueRemoteCandidate(const SignalingMessage& message);
  absl::Status FlushPendingRemoteCandidates();
  void ReportError(absl::Status status);

  AppConfig config_;
  JoinResponse join_response_;
  webrtc::Thread* const signaling_thread_;
  RTC_NO_UNIQUE_ADDRESS webrtc::SequenceChecker signaling_sequence_;
  webrtc::ScopedTaskSafety task_safety_;
  webrtc::Environment env_;
  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> connection_;
  webrtc::scoped_refptr<FileVideoSource> video_source_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_module_;
  std::unique_ptr<RemoteFrameSink> remote_frame_sink_;
  bool connected_ = false;
  bool remote_media_seen_ = false;
  bool remote_description_set_ = false;
  std::function<void(const std::string&)> message_callback_;
  std::function<void(bool)> connection_callback_;
  std::function<void()> remote_media_callback_;
  std::function<void(absl::Status)> error_callback_;
  std::vector<SignalingMessage> pending_remote_candidates_;
};

}  // namespace webrtc_apprtc_cli

#endif  // WEBRTC_APPRTC_CLI_SRC_APPRTC_PEER_CONNECTION_H_
