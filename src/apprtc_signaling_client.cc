#include "apprtc_signaling_client.h"

#include <curl/curl.h>

#include <sstream>
#include <utility>

#include "absl/cleanup/cleanup.h"
#include "json/json.h"
#include "rtc_base/logging.h"

namespace webrtc_apprtc_cli {

namespace {

bool ParseInitiatorField(const Json::Value& value) {
  if (value.isBool()) {
    return value.asBool();
  }
  if (value.isString()) {
    return value.asString() == "true";
  }
  return false;
}

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  std::string* response = static_cast<std::string*>(userdata);
  response->append(ptr, size * nmemb);
  return size * nmemb;
}

absl::Status CurlCodeToStatus(CURLcode code, const std::string& context) {
  if (code == CURLE_OK) {
    return absl::OkStatus();
  }
  return absl::InternalError(context + ": " + curl_easy_strerror(code));
}

absl::StatusOr<Json::Value> ParseJson(const std::string& text) {
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream stream(text);
  if (!Json::parseFromStream(builder, stream, &root, &errors)) {
    return absl::InvalidArgumentError("failed to parse json: " + errors);
  }
  return root;
}

std::string JoinUrl(const std::string& base, const std::string& suffix) {
  if (!base.empty() && base.back() == '/') {
    return base.substr(0, base.size() - 1) + suffix;
  }
  return base + suffix;
}

absl::Status ParseMessageResponse(const std::string& response_text,
                                  bool trace_signaling) {
  auto root = ParseJson(response_text);
  if (!root.ok()) {
    return root.status();
  }
  const std::string result = (*root)["result"].asString();
  std::string error;
  if ((*root)["error"].isString()) {
    error = (*root)["error"].asString();
  }
  if (trace_signaling) {
    RTC_LOG(LS_INFO) << "posted room message result=" << result
                     << " error=" << error;
  }
  if (result != "SUCCESS") {
    if (error.empty()) {
      error = "room message failed with result: " + result;
    }
    return absl::InternalError(error);
  }
  return absl::OkStatus();
}

}  // namespace

AppRtcSignalingClient::AppRtcSignalingClient(bool trace_signaling,
                                             webrtc::Thread* callback_thread)
    : callback_thread_(callback_thread), trace_signaling_(trace_signaling) {
  RTC_CHECK(callback_thread_ != nullptr);
  network_thread_ = webrtc::Thread::Create();
  network_thread_->SetName("apprtc-signaling-io", nullptr);
  RTC_CHECK(network_thread_->Start());
}

AppRtcSignalingClient::~AppRtcSignalingClient() {
  absl::MutexLock lock(mutex_);
  closing_ = true;
}

void AppRtcSignalingClient::SetEventCallbacks(
    EventCallback on_websocket_open,
    MessageCallback on_signaling_message,
    std::function<void(absl::Status)> on_error) {
  absl::MutexLock lock(mutex_);
  on_websocket_open_ = std::move(on_websocket_open);
  on_signaling_message_ = std::move(on_signaling_message);
  on_error_ = std::move(on_error);
}

absl::StatusOr<std::string> AppRtcSignalingClient::PostJson(
    const std::string& url,
    const std::string& body) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return absl::InternalError("curl_easy_init failed");
  }
  auto cleanup = absl::Cleanup([curl] { curl_easy_cleanup(curl); });

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  auto headers_cleanup = absl::Cleanup([headers] {
    if (headers != nullptr) {
      curl_slist_free_all(headers);
    }
  });

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  CURLcode code = curl_easy_perform(curl);
  if (absl::Status status = CurlCodeToStatus(code, "HTTP POST failed");
      !status.ok()) {
    return status;
  }

  long response_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  if (response_code < 200 || response_code >= 300) {
    return absl::InternalError("unexpected HTTP response code: " +
                               std::to_string(response_code));
  }
  return response;
}

absl::StatusOr<std::string> AppRtcSignalingClient::PostEmpty(
    const std::string& url) {
  return PostJson(url, "");
}

absl::StatusOr<std::string> AppRtcSignalingClient::Delete(
    const std::string& url) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return absl::InternalError("curl_easy_init failed");
  }
  auto cleanup = absl::Cleanup([curl] { curl_easy_cleanup(curl); });

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  CURLcode code = curl_easy_perform(curl);
  if (absl::Status status = CurlCodeToStatus(code, "HTTP DELETE failed");
      !status.ok()) {
    return status;
  }
  return response;
}

void AppRtcSignalingClient::PostToCallbackThread(
    absl::AnyInvocable<void() &&> task) {
  callback_thread_->PostTask(std::move(task));
}

void AppRtcSignalingClient::NotifyError(absl::Status status) {
  std::function<void(absl::Status)> on_error;
  {
    absl::MutexLock lock(mutex_);
    on_error = on_error_;
  }
  if (!on_error) {
    return;
  }
  PostToCallbackThread(
      [on_error = std::move(on_error), status = std::move(status)]() mutable {
        on_error(std::move(status));
      });
}

void AppRtcSignalingClient::JoinRoomAsync(const std::string& room_server_url,
                                          const std::string& room_id,
                                          JoinCallback callback) {
  network_thread_->PostTask([this, room_server_url, room_id,
                             callback = std::move(callback)]() mutable {
    const std::string url = JoinUrl(room_server_url, "/join/" + room_id);
    auto response = PostEmpty(url);
    if (!response.ok()) {
      PostToCallbackThread([callback = std::move(callback),
                            status = response.status()]() mutable {
        std::move(callback)(status);
      });
      return;
    }

    auto root = ParseJson(*response);
    if (!root.ok()) {
      PostToCallbackThread(
          [callback = std::move(callback), status = root.status()]() mutable {
            std::move(callback)(status);
          });
      return;
    }

    JoinResponse join_response;
    join_response.result = (*root)["result"].asString();
    const Json::Value& params = (*root)["params"];
    join_response.room_id = params["room_id"].asString();
    join_response.client_id = params["client_id"].asString();
    join_response.is_initiator = ParseInitiatorField(params["is_initiator"]);
    join_response.wss_url = params["wss_url"].asString();
    join_response.wss_post_url = params["wss_post_url"].asString();
    join_response.pc_config_json = params["pc_config"].asString();
    const Json::Value& messages = params["messages"];
    if (messages.isArray()) {
      for (const Json::Value& value : messages) {
        if (value.isString()) {
          join_response.messages.push_back(value.asString());
        } else if (value.isObject()) {
          Json::StreamWriterBuilder builder;
          builder["indentation"] = "";
          join_response.messages.push_back(Json::writeString(builder, value));
        }
      }
    }

    auto ice_servers = ParseIceServers(join_response.pc_config_json);
    if (!ice_servers.ok()) {
      PostToCallbackThread([callback = std::move(callback),
                            status = ice_servers.status()]() mutable {
        std::move(callback)(status);
      });
      return;
    }
    for (const auto& server : *ice_servers) {
      IceServerConfig config;
      config.urls = server.urls;
      config.username = server.username;
      config.credential = server.password;
      join_response.ice_servers.push_back(std::move(config));
    }

    if (join_response.result != "SUCCESS") {
      PostToCallbackThread(
          [callback = std::move(callback),
           status = absl::FailedPreconditionError(
               "join failed with result: " + join_response.result)]() mutable {
            std::move(callback)(status);
          });
      return;
    }
    if (trace_signaling_) {
      RTC_LOG(LS_INFO) << "joined room_id=" << join_response.room_id
                       << " client_id=" << join_response.client_id
                       << " initiator=" << join_response.is_initiator
                       << " queued_messages=" << join_response.messages.size()
                       << " ice_servers=" << join_response.ice_servers.size()
                       << " wss_url=" << join_response.wss_url;
    }

    PostToCallbackThread([callback = std::move(callback),
                          join_response = std::move(join_response)]() mutable {
      std::move(callback)(std::move(join_response));
    });
  });
}

void AppRtcSignalingClient::ConnectWebSocketAsync(
    const JoinResponse& join_response) {
  network_thread_->PostTask([this, join_response] {
    {
      absl::MutexLock lock(mutex_);
      closing_ = false;
      websocket_open_ = false;
    }

    websocket_.stop();
    websocket_.setUrl(join_response.wss_url);
    websocket_.setOnMessageCallback(
        [this](const ix::WebSocketMessagePtr& message) {
          if (message->type == ix::WebSocketMessageType::Open) {
            EventCallback on_websocket_open;
            {
              absl::MutexLock lock(mutex_);
              websocket_open_ = true;
              on_websocket_open = on_websocket_open_;
            }
            RTC_LOG(LS_INFO) << "WebSocket opened";
            if (on_websocket_open) {
              PostToCallbackThread(
                  [on_websocket_open = std::move(on_websocket_open)]() mutable {
                    on_websocket_open();
                  });
            }
            return;
          }

          if (message->type == ix::WebSocketMessageType::Close) {
            bool closing = false;
            {
              absl::MutexLock lock(mutex_);
              websocket_open_ = false;
              closing = closing_;
            }
            RTC_LOG(LS_INFO) << "WebSocket closed";
            if (!closing) {
              NotifyError(absl::InternalError("websocket closed"));
            }
            return;
          }

          if (message->type == ix::WebSocketMessageType::Error) {
            {
              absl::MutexLock lock(mutex_);
              websocket_open_ = false;
            }
            RTC_LOG(LS_ERROR)
                << "WebSocket error: " << message->errorInfo.reason;
            NotifyError(absl::InternalError("websocket error: " +
                                            message->errorInfo.reason));
            return;
          }

          if (message->type != ix::WebSocketMessageType::Message) {
            return;
          }

          auto outer = ParseJson(message->str);
          if (!outer.ok()) {
            NotifyError(outer.status());
            return;
          }
          if ((*outer)["error"].isString() &&
              !(*outer)["error"].asString().empty()) {
            NotifyError(absl::InternalError("websocket server error: " +
                                            (*outer)["error"].asString()));
            return;
          }
          const std::string payload = (*outer)["msg"].asString();
          auto signaling_message = ParseSignalingMessage(payload);
          if (!signaling_message.ok()) {
            NotifyError(signaling_message.status());
            return;
          }
          if (trace_signaling_) {
            RTC_LOG(LS_INFO)
                << "received signaling message type="
                << SignalingMessageTypeToString(signaling_message->type);
          }
          MessageCallback on_signaling_message;
          {
            absl::MutexLock lock(mutex_);
            on_signaling_message = on_signaling_message_;
          }
          if (!on_signaling_message) {
            return;
          }
          PostToCallbackThread(
              [on_signaling_message = std::move(on_signaling_message),
               signaling_message = *signaling_message]() mutable {
                on_signaling_message(signaling_message);
              });
        });
    websocket_.start();
  });
}

void AppRtcSignalingClient::RegisterAsync(const JoinResponse& join_response,
                                          StatusCallback callback) {
  network_thread_->PostTask([this, join_response,
                             callback = std::move(callback)]() mutable {
    bool websocket_open = false;
    {
      absl::MutexLock lock(mutex_);
      websocket_open = websocket_open_;
    }
    if (!websocket_open) {
      PostToCallbackThread([callback = std::move(callback)]() mutable {
        std::move(callback)(absl::FailedPreconditionError(
            "websocket not open during register"));
      });
      return;
    }

    Json::Value root;
    root["cmd"] = "register";
    root["roomid"] = join_response.room_id;
    root["clientid"] = join_response.client_id;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    if (trace_signaling_) {
      RTC_LOG(LS_INFO) << "sending websocket register room_id="
                       << join_response.room_id
                       << " client_id=" << join_response.client_id;
    }
    ix::WebSocketSendInfo info =
        websocket_.send(Json::writeString(builder, root));
    PostToCallbackThread([callback = std::move(callback), info]() mutable {
      if (!info.success) {
        std::move(callback)(absl::InternalError("websocket register failed"));
        return;
      }
      std::move(callback)(absl::OkStatus());
    });
  });
}

void AppRtcSignalingClient::SendToRoomAsync(const std::string& room_server_url,
                                            const std::string& room_id,
                                            const std::string& client_id,
                                            const std::string& message_json,
                                            StatusCallback callback) {
  network_thread_->PostTask([this, room_server_url, room_id, client_id,
                             message_json,
                             callback = std::move(callback)]() mutable {
    const std::string url =
        JoinUrl(room_server_url, "/message/" + room_id + "/" + client_id);
    auto response = PostJson(url, message_json);
    PostToCallbackThread([callback = std::move(callback),
                          response = std::move(response),
                          trace_signaling = trace_signaling_]() mutable {
      if (!response.ok()) {
        std::move(callback)(response.status());
        return;
      }
      std::move(callback)(ParseMessageResponse(*response, trace_signaling));
    });
  });
}

void AppRtcSignalingClient::SendWebSocketMessageAsync(
    const std::string& message_json,
    StatusCallback callback) {
  network_thread_->PostTask([this, message_json,
                             callback = std::move(callback)]() mutable {
    bool websocket_open = false;
    {
      absl::MutexLock lock(mutex_);
      websocket_open = websocket_open_;
    }
    if (!websocket_open) {
      PostToCallbackThread([callback = std::move(callback)]() mutable {
        std::move(callback)(
            absl::FailedPreconditionError("websocket not open"));
      });
      return;
    }

    auto signaling_message = ParseSignalingMessage(message_json);
    if (trace_signaling_ && signaling_message.ok()) {
      RTC_LOG(LS_INFO) << "sending websocket signaling type="
                       << SignalingMessageTypeToString(signaling_message->type);
    }
    Json::Value root;
    root["cmd"] = "send";
    root["msg"] = message_json;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    ix::WebSocketSendInfo info =
        websocket_.send(Json::writeString(builder, root));
    PostToCallbackThread([callback = std::move(callback), info]() mutable {
      if (!info.success) {
        std::move(callback)(absl::InternalError("websocket send failed"));
        return;
      }
      std::move(callback)(absl::OkStatus());
    });
  });
}

void AppRtcSignalingClient::CloseAsync(const JoinResponse& join_response) {
  network_thread_->PostTask([this, join_response] {
    {
      absl::MutexLock lock(mutex_);
      closing_ = true;
      websocket_open_ = false;
    }
    websocket_.stop();

    if (!join_response.wss_post_url.empty() && !join_response.room_id.empty() &&
        !join_response.client_id.empty()) {
      auto ignored_delete =
          Delete(join_response.wss_post_url + "/" + join_response.room_id +
                 "/" + join_response.client_id);
      (void)ignored_delete;
    }
  });
}

}  // namespace webrtc_apprtc_cli
