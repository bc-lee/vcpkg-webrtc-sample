#ifndef WEBRTC_APPRTC_CLI_SRC_FILE_VIDEO_SOURCE_H_
#define WEBRTC_APPRTC_CLI_SRC_FILE_VIDEO_SOURCE_H_

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "absl/status/status.h"
#include "api/notifier.h"
#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace webrtc_apprtc_cli {

class FileVideoSource
    : public webrtc::Notifier<webrtc::VideoTrackSourceInterface> {
 public:
  explicit FileVideoSource(std::string path);
  ~FileVideoSource() override;

  absl::Status Start();

  SourceState state() const override { return state_; }
  bool remote() const override { return false; }
  bool is_screencast() const override { return false; }
  std::optional<bool> needs_denoising() const override { return false; }
  bool GetStats(Stats* stats) override;
  bool SupportsEncodedOutput() const override { return false; }
  void GenerateKeyFrame() override {}
  void AddEncodedSink(
      webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>*) override {}
  void RemoveEncodedSink(
      webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>*) override {}

  void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                       const webrtc::VideoSinkWants& wants) override;
  void RemoveSink(
      webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override;

 private:
  struct SinkEntry {
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink;
    webrtc::VideoSinkWants wants;
  };

  void DecodeLoop();
  absl::Status InitializeDecoder();
  void CleanupDecoder();
  void BroadcastFrame(const webrtc::VideoFrame& frame);

  std::string path_;
  SourceState state_ = kInitializing;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::vector<SinkEntry> sinks_;
  std::thread decoder_thread_;

  AVFormatContext* format_context_ = nullptr;
  AVCodecContext* codec_context_ = nullptr;
  AVFrame* decoded_frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  SwsContext* sws_context_ = nullptr;
  int video_stream_index_ = -1;
  int width_ = 0;
  int height_ = 0;
  int64_t frame_interval_us_ = 33333;
};

}  // namespace webrtc_apprtc_cli

#endif  // WEBRTC_APPRTC_CLI_SRC_FILE_VIDEO_SOURCE_H_
