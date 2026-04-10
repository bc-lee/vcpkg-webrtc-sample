#include "file_video_source.h"

#include <algorithm>
#include <chrono>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "rtc_base/logging.h"

namespace webrtc_apprtc_cli {

FileVideoSource::FileVideoSource(std::string path) : path_(std::move(path)) {}

FileVideoSource::~FileVideoSource() {
  stop_.store(true);
  if (decoder_thread_.joinable()) {
    decoder_thread_.join();
  }
  CleanupDecoder();
  state_ = kEnded;
}

absl::Status FileVideoSource::InitializeDecoder() {
  int result =
      avformat_open_input(&format_context_, path_.c_str(), nullptr, nullptr);
  if (result < 0) {
    return absl::NotFoundError("avformat_open_input failed");
  }
  if (avformat_find_stream_info(format_context_, nullptr) < 0) {
    return absl::InternalError("avformat_find_stream_info failed");
  }

  video_stream_index_ = av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO,
                                            -1, -1, nullptr, 0);
  if (video_stream_index_ < 0) {
    return absl::NotFoundError("no video stream found");
  }

  AVStream* stream = format_context_->streams[video_stream_index_];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == nullptr) {
    return absl::UnimplementedError("no decoder available for input video");
  }

  codec_context_ = avcodec_alloc_context3(codec);
  if (codec_context_ == nullptr) {
    return absl::InternalError("avcodec_alloc_context3 failed");
  }
  if (avcodec_parameters_to_context(codec_context_, stream->codecpar) < 0) {
    return absl::InternalError("avcodec_parameters_to_context failed");
  }
  if (avcodec_open2(codec_context_, codec, nullptr) < 0) {
    return absl::InternalError("avcodec_open2 failed");
  }

  decoded_frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
  if (decoded_frame_ == nullptr || packet_ == nullptr) {
    return absl::InternalError("failed to allocate FFmpeg frame state");
  }

  width_ = codec_context_->width;
  height_ = codec_context_->height;
  if (width_ <= 0 || height_ <= 0) {
    return absl::InvalidArgumentError("decoded dimensions are invalid");
  }

  sws_context_ = sws_getContext(width_, height_, codec_context_->pix_fmt,
                                width_, height_, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (sws_context_ == nullptr) {
    return absl::InternalError("sws_getContext failed");
  }

  AVRational rate = stream->avg_frame_rate.num != 0 ? stream->avg_frame_rate
                                                    : stream->r_frame_rate;
  if (rate.num > 0 && rate.den > 0) {
    const double fps = av_q2d(rate);
    if (fps > 1.0) {
      frame_interval_us_ = static_cast<int64_t>(1000000.0 / fps);
    }
  }
  return absl::OkStatus();
}

void FileVideoSource::CleanupDecoder() {
  if (packet_ != nullptr) {
    av_packet_free(&packet_);
  }
  if (decoded_frame_ != nullptr) {
    av_frame_free(&decoded_frame_);
  }
  if (codec_context_ != nullptr) {
    avcodec_free_context(&codec_context_);
  }
  if (format_context_ != nullptr) {
    avformat_close_input(&format_context_);
  }
  if (sws_context_ != nullptr) {
    sws_freeContext(sws_context_);
    sws_context_ = nullptr;
  }
}

absl::Status FileVideoSource::Start() {
  absl::Status status = InitializeDecoder();
  if (!status.ok()) {
    state_ = kEnded;
    return status;
  }
  state_ = kLive;
  decoder_thread_ = std::thread([this] { DecodeLoop(); });
  return absl::OkStatus();
}

bool FileVideoSource::GetStats(Stats* stats) {
  if (stats == nullptr) {
    return false;
  }
  stats->input_width = width_;
  stats->input_height = height_;
  return true;
}

void FileVideoSource::AddOrUpdateSink(
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
    const webrtc::VideoSinkWants& wants) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : sinks_) {
    if (entry.sink == sink) {
      entry.wants = wants;
      return;
    }
  }
  sinks_.push_back({sink, wants});
}

void FileVideoSource::RemoveSink(
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  sinks_.erase(std::remove_if(sinks_.begin(), sinks_.end(),
                              [sink](const SinkEntry& entry) {
                                return entry.sink == sink;
                              }),
               sinks_.end());
}

void FileVideoSource::BroadcastFrame(const webrtc::VideoFrame& frame) {
  std::vector<SinkEntry> sinks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks = sinks_;
  }
  for (const SinkEntry& entry : sinks) {
    if (entry.sink != nullptr) {
      entry.sink->OnFrame(frame);
    }
  }
}

void FileVideoSource::DecodeLoop() {
  while (!stop_.load()) {
    int result = av_read_frame(format_context_, packet_);
    if (result < 0) {
      av_seek_frame(format_context_, video_stream_index_, 0,
                    AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(codec_context_);
      continue;
    }
    if (packet_->stream_index != video_stream_index_) {
      av_packet_unref(packet_);
      continue;
    }
    if (avcodec_send_packet(codec_context_, packet_) < 0) {
      RTC_LOG(LS_WARNING) << "avcodec_send_packet failed";
      av_packet_unref(packet_);
      continue;
    }
    av_packet_unref(packet_);

    while (!stop_.load()) {
      result = avcodec_receive_frame(codec_context_, decoded_frame_);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        break;
      }
      if (result < 0) {
        RTC_LOG(LS_WARNING) << "avcodec_receive_frame failed";
        break;
      }

      auto buffer = webrtc::I420Buffer::Create(width_, height_);
      uint8_t* dst_data[4] = {buffer->MutableDataY(), buffer->MutableDataU(),
                              buffer->MutableDataV(), nullptr};
      int dst_linesize[4] = {buffer->StrideY(), buffer->StrideU(),
                             buffer->StrideV(), 0};
      sws_scale(sws_context_, decoded_frame_->data, decoded_frame_->linesize, 0,
                height_, dst_data, dst_linesize);

      const int64_t timestamp_us =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                     .set_video_frame_buffer(buffer)
                                     .set_timestamp_us(timestamp_us)
                                     .set_rotation(webrtc::kVideoRotation_0)
                                     .build();
      BroadcastFrame(frame);
      std::this_thread::sleep_for(
          std::chrono::microseconds(frame_interval_us_));
    }
  }
}

}  // namespace webrtc_apprtc_cli
