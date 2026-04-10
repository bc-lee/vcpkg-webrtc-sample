#ifndef WEBRTC_APPRTC_CLI_SRC_APPRTC_SIGNALING_CLIENT_H_
#define WEBRTC_APPRTC_CLI_SRC_APPRTC_SIGNALING_CLIENT_H_

#include <functional>
#include <memory>
#include <string>

#include "absl/base/thread_annotations.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "apprtc_common.h"
#include "ixwebsocket/IXWebSocket.h"
#include "rtc_base/thread.h"

namespace webrtc_apprtc_cli {

class AppRtcSignalingClient {
 public:
  using EventCallback = std::function<void()>;
  using MessageCallback = std::function<void(SignalingMessage)>;
  using StatusCallback = absl::AnyInvocable<void(absl::Status) &&>;
  using JoinCallback =
      absl::AnyInvocable<void(absl::StatusOr<JoinResponse>) &&>;

  AppRtcSignalingClient(bool trace_signaling, webrtc::Thread* callback_thread);
  ~AppRtcSignalingClient();

  void SetEventCallbacks(EventCallback on_websocket_open,
                         MessageCallback on_signaling_message,
                         std::function<void(absl::Status)> on_error);

  void JoinRoomAsync(const std::string& room_server_url,
                     const std::string& room_id,
                     JoinCallback callback);
  void ConnectWebSocketAsync(const JoinResponse& join_response);
  void RegisterAsync(const JoinResponse& join_response,
                     StatusCallback callback);
  void SendToRoomAsync(const std::string& room_server_url,
                       const std::string& room_id,
                       const std::string& client_id,
                       const std::string& message_json,
                       StatusCallback callback);
  void SendWebSocketMessageAsync(const std::string& message_json,
                                 StatusCallback callback);
  void CloseAsync(const JoinResponse& join_response);

 private:
  absl::StatusOr<std::string> PostJson(const std::string& url,
                                       const std::string& body);
  absl::StatusOr<std::string> PostEmpty(const std::string& url);
  absl::StatusOr<std::string> Delete(const std::string& url);
  void PostToCallbackThread(absl::AnyInvocable<void() &&> task);
  void NotifyError(absl::Status status);

  ix::WebSocket websocket_;
  std::unique_ptr<webrtc::Thread> network_thread_;
  webrtc::Thread* const callback_thread_;
  mutable absl::Mutex mutex_;
  EventCallback on_websocket_open_ ABSL_GUARDED_BY(mutex_);
  MessageCallback on_signaling_message_ ABSL_GUARDED_BY(mutex_);
  std::function<void(absl::Status)> on_error_ ABSL_GUARDED_BY(mutex_);
  bool websocket_open_ ABSL_GUARDED_BY(mutex_) = false;
  bool closing_ ABSL_GUARDED_BY(mutex_) = false;
  bool trace_signaling_ = false;
};

}  // namespace webrtc_apprtc_cli

#endif  // WEBRTC_APPRTC_CLI_SRC_APPRTC_SIGNALING_CLIENT_H_
