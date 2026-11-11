#include "flutter_screen_capture.h"
#include "flutter_utf8_sanitize.h"
#include "task_runner.h"

#include <stdexcept>

namespace flutter_webrtc_plugin {

FlutterScreenCapture::FlutterScreenCapture(FlutterWebRTCBase* base)
    : base_(base) {}

FlutterScreenCapture::~FlutterScreenCapture() {
  alive_.reset();
  {
    std::lock_guard<std::mutex> lock(lister_mutex_);
    lister_stopping_ = true;
    lister_tasks_.clear();
  }
  lister_wake_.notify_all();
  if (lister_.joinable()) lister_.join();
}

bool FlutterScreenCapture::UpdateMediaLists(const EncodableList& types,
                                            bool force_reload,
                                            SourceList* sources) {
  size_t size = types.size();
  for (size_t i = 0; i < size; i++) {
    std::string type_str = GetValue<std::string>(types[i]);
    DesktopType desktop_type = DesktopType::kScreen;
    if (type_str == "screen") {
      desktop_type = DesktopType::kScreen;
    } else if (type_str == "window") {
      desktop_type = DesktopType::kWindow;
    } else {
      return false;
    }
    scoped_refptr<RTCDesktopMediaList> source_list;
    auto it = medialist_.find(desktop_type);
    if (it != medialist_.end()) {
      source_list = (*it).second;
    } else {
      source_list = base_->desktop_device_->GetDesktopMediaList(desktop_type);
      source_list->RegisterMediaListObserver(this);
      medialist_[desktop_type] = source_list;
    }
#ifdef __linux__
    try {
      source_list->UpdateSourceList(force_reload, false);
    } catch (...) {
      continue;
    }
#else
    source_list->UpdateSourceList(force_reload, false);
#endif
    int count = source_list->GetSourceCount();
    for (int j = 0; j < count; j++) {
      sources->push_back(source_list->GetSource(j));
    }
  }
  return true;
}

bool FlutterScreenCapture::BuildDesktopSourcesList(const EncodableList& types,
                                                   bool force_reload) {
  SourceList sources;
  bool ok = UpdateMediaLists(types, force_reload, &sources);
  sources_ = std::move(sources);
  return ok;
}

void FlutterScreenCapture::RunLister() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(lister_mutex_);
      lister_wake_.wait(
          lock, [this] { return lister_stopping_ || !lister_tasks_.empty(); });
      if (lister_stopping_) return;
      task = std::move(lister_tasks_.front());
      lister_tasks_.pop_front();
    }
    task();
  }
}

void FlutterScreenCapture::ListSources(const EncodableList& types,
                                       bool force_reload,
                                       std::function<void(bool ok)> done) {
#ifdef _WIN32
  TaskRunner* platform = base_->platform_task_runner();
  if (platform) {
    std::weak_ptr<bool> alive = alive_;
    std::lock_guard<std::mutex> lock(lister_mutex_);
    if (!lister_.joinable()) lister_ = std::thread([this] { RunLister(); });
    lister_tasks_.push_back([this, platform, alive, types, force_reload,
                             done = std::move(done)]() {
      auto sources = std::make_shared<SourceList>();
      bool ok = UpdateMediaLists(types, force_reload, sources.get());
      // Checked under the lock the destructor sets it with, so nothing is
      // posted to a platform runner that is going away.
      std::lock_guard<std::mutex> stop_lock(lister_mutex_);
      if (lister_stopping_) return;
      platform->EnqueueTask([this, alive, sources, ok, done]() {
        if (alive.expired()) return;
        sources_ = std::move(*sources);
        done(ok);
      });
    });
    lister_wake_.notify_one();
    return;
  }
#endif
  done(BuildDesktopSourcesList(types, force_reload));
}

void FlutterScreenCapture::GetDesktopSources(
    const EncodableList& types,
    std::unique_ptr<MethodResultProxy> result) {
  std::shared_ptr<MethodResultProxy> reply = std::move(result);
  ListSources(types, true, [this, reply](bool ok) {
    if (!ok) {
      reply->Error("Bad Arguments", "Failed to get desktop sources");
      return;
    }

    EncodableList sources;
    for (auto source : sources_) {
      EncodableMap info;
      info[EncodableValue("id")] = EncodableValue(source->id().std_string());
      info[EncodableValue("name")] =
          EncodableValue(SanitizeUtf8ForFlutter(source->name().std_string()));
      info[EncodableValue("type")] =
          EncodableValue(source->type() == kWindow ? "window" : "screen");
      // TODO "thumbnailSize"
      info[EncodableValue("thumbnailSize")] = EncodableMap{
          {EncodableValue("width"), EncodableValue(0)},
          {EncodableValue("height"), EncodableValue(0)},
      };
      sources.push_back(EncodableValue(info));
    }

    auto map = EncodableMap();
    map[EncodableValue("sources")] = sources;
    reply->Success(EncodableValue(map));
  });
}

void FlutterScreenCapture::UpdateDesktopSources(
    const EncodableList& types,
    std::unique_ptr<MethodResultProxy> result) {
  std::shared_ptr<MethodResultProxy> reply = std::move(result);
  ListSources(types, false, [reply](bool ok) {
    if (!ok) {
      reply->Error("Bad Arguments", "Failed to update desktop sources");
      return;
    }
    auto map = EncodableMap();
    map[EncodableValue("result")] = true;
    reply->Success(EncodableValue(map));
  });
}

void FlutterScreenCapture::OnMediaSourceAdded(
    scoped_refptr<MediaSource> source) {
  EncodableMap info;
  info[EncodableValue("event")] = "desktopSourceAdded";
  info[EncodableValue("id")] = EncodableValue(source->id().std_string());
  info[EncodableValue("name")] =
      EncodableValue(SanitizeUtf8ForFlutter(source->name().std_string()));
  info[EncodableValue("type")] =
      EncodableValue(source->type() == kWindow ? "window" : "screen");
  // TODO "thumbnailSize"
  info[EncodableValue("thumbnailSize")] = EncodableMap{
      {EncodableValue("width"), EncodableValue(0)},
      {EncodableValue("height"), EncodableValue(0)},
  };
  base_->event_channel()->Success(EncodableValue(info));
}

void FlutterScreenCapture::OnMediaSourceRemoved(
    scoped_refptr<MediaSource> source) {
  EncodableMap info;
  info[EncodableValue("event")] = "desktopSourceRemoved";
  info[EncodableValue("id")] = EncodableValue(source->id().std_string());
  base_->event_channel()->Success(EncodableValue(info));
}

void FlutterScreenCapture::OnMediaSourceNameChanged(
    scoped_refptr<MediaSource> source) {
  EncodableMap info;
  info[EncodableValue("event")] = "desktopSourceNameChanged";
  info[EncodableValue("id")] = EncodableValue(source->id().std_string());
  info[EncodableValue("name")] =
      EncodableValue(SanitizeUtf8ForFlutter(source->name().std_string()));
  base_->event_channel()->Success(EncodableValue(info));
}

void FlutterScreenCapture::OnMediaSourceThumbnailChanged(
    scoped_refptr<MediaSource> source) {
  EncodableMap info;
  info[EncodableValue("event")] = "desktopSourceThumbnailChanged";
  info[EncodableValue("id")] = EncodableValue(source->id().std_string());
  info[EncodableValue("thumbnail")] =
      EncodableValue(source->thumbnail().std_vector());
  base_->event_channel()->Success(EncodableValue(info));
}

void FlutterScreenCapture::OnStart(scoped_refptr<RTCDesktopCapturer> capturer) {
}

void FlutterScreenCapture::OnPaused(
    scoped_refptr<RTCDesktopCapturer> capturer) {}

void FlutterScreenCapture::OnStop(scoped_refptr<RTCDesktopCapturer> capturer) {
  if (loopback_capturer_) {
    loopback_capturer_->Stop();
    loopback_capturer_.reset();
    loopback_audio_source_ = nullptr;
  }
}

void FlutterScreenCapture::OnError(scoped_refptr<RTCDesktopCapturer> capturer) {
}

void FlutterScreenCapture::GetDesktopSourceThumbnail(
    std::string source_id,
    int width,
    int height,
    std::unique_ptr<MethodResultProxy> result) {
  (void)width;
  (void)height;
  scoped_refptr<MediaSource> source;
  for (auto src : sources_) {
    if (src->id().std_string() == source_id) {
      source = src;
    }
  }
  if (source.get() == nullptr) {
    result->Error("Bad Arguments", "Failed to get desktop source thumbnail");
    return;
  }
  source->UpdateThumbnail();
  result->Success(EncodableValue(source->thumbnail().std_vector()));
}

void FlutterScreenCapture::GetDisplayMedia(
    const EncodableMap& constraints,
    std::unique_ptr<MethodResultProxy> result) {
  std::string source_id = "0";
  // DesktopType source_type = kScreen;
  double fps = 30.0;
  // Whether the OS cursor is composited into the captured frames, driven by the
  // getDisplayMedia "cursor" video constraint. Defaults to true so behaviour is
  // unchanged when the constraint is absent — that is libwebrtc's own default.
  bool show_cursor = true;

  const EncodableMap video = findMap(constraints, "video");
  if (video != EncodableMap()) {
    const EncodableMap deviceId = findMap(video, "deviceId");
    if (deviceId != EncodableMap()) {
      source_id = findString(deviceId, "exact");
      if (source_id.empty()) {
        result->Error("Bad Arguments", "Incorrect video->deviceId->exact");
        return;
      }
      if (source_id != "0") {
        // source_type = DesktopType::kWindow;
      }
    }
    const EncodableMap mandatory = findMap(video, "mandatory");
    if (mandatory != EncodableMap()) {
      double frameRate = findDouble(mandatory, "frameRate");
      if (frameRate != 0.0) {
        fps = frameRate;
      }
    }
    // Accept both the spec's string form ("always"/"never") and a plain bool.
    // Only an explicitly supplied constraint moves off the default, so callers
    // that pass no "cursor" key keep exactly the behaviour they have today.
    const std::string cursor = findString(video, "cursor");
    if (!cursor.empty()) {
      show_cursor = (cursor == "always");
    } else if (video.find(EncodableValue("cursor")) != video.end()) {
      show_cursor = findBoolean(video, "cursor");
    }
  }

  std::string uuid = base_->GenerateUUID();

  scoped_refptr<RTCMediaStream> stream =
      base_->factory_->CreateStream(uuid.c_str());

  EncodableMap params;
  params[EncodableValue("streamId")] = EncodableValue(uuid);

  // AUDIO

  bool capture_audio = false;
  {
    auto audio_it = constraints.find(EncodableValue("audio"));
    if (audio_it != constraints.end()) {
      if (TypeIs<bool>(audio_it->second)) {
        capture_audio = GetValue<bool>(audio_it->second);
      } else if (TypeIs<EncodableMap>(audio_it->second)) {
        capture_audio = true;
      }
    }
  }

  if (capture_audio) {
    // Stop any previous loopback session before starting a new one.
    if (loopback_capturer_) {
      loopback_capturer_->Stop();
      loopback_capturer_.reset();
    }

    // Disable all audio processing for loopback capture.  Echo cancellation,
    // AGC, and noise suppression are designed for microphone input; applied to
    // system audio they treat the captured content as echo/noise and destroy it.
    RTCAudioOptions loopback_opts;
    loopback_opts.echo_cancellation = false;
    loopback_opts.auto_gain_control = false;
    loopback_opts.noise_suppression = false;
    const std::string loopback_source_label =
      "screen_loopback_input_" + base_->GenerateUUID();
    loopback_audio_source_ = base_->factory_->CreateAudioSource(
      loopback_source_label.c_str(), RTCAudioSource::SourceType::kCustom,
        loopback_opts);

    std::string audio_uuid = base_->GenerateUUID();
    scoped_refptr<RTCAudioTrack> audio_track =
        base_->factory_->CreateAudioTrack(loopback_audio_source_,
                                          audio_uuid.c_str());

    loopback_capturer_ = CreateLoopbackCapturer(source_id);

    if (loopback_capturer_ && loopback_capturer_->Start(loopback_audio_source_)) {
      EncodableMap audio_info;
      audio_info[EncodableValue("id")] =
          EncodableValue(audio_track->id().std_string());
      audio_info[EncodableValue("label")] =
          EncodableValue(audio_track->id().std_string());
      audio_info[EncodableValue("kind")] =
          EncodableValue(audio_track->kind().std_string());
      audio_info[EncodableValue("enabled")] =
          EncodableValue(audio_track->enabled());

      EncodableList audioTracks;
      audioTracks.push_back(EncodableValue(audio_info));
      params[EncodableValue("audioTracks")] = EncodableValue(audioTracks);

      stream->AddTrack(audio_track);
      base_->local_tracks_[audio_track->id().std_string()] = audio_track;
    } else {
      // Loopback init failed or not supported — continue without audio.
      loopback_capturer_.reset();
      loopback_audio_source_ = nullptr;
      params[EncodableValue("audioTracks")] = EncodableValue(EncodableList());
    }
  } else {
    params[EncodableValue("audioTracks")] = EncodableValue(EncodableList());
  }

  // VIDEO

  EncodableMap video_constraints;
  auto it = constraints.find(EncodableValue("video"));
  if (it != constraints.end() && TypeIs<EncodableMap>(it->second)) {
    video_constraints = GetValue<EncodableMap>(it->second);
  }

  scoped_refptr<MediaSource> source;
  for (auto src : sources_) {
    if (src->id().std_string() == source_id) {
      source = src;
    }
  }

#ifdef __linux__
  // If the caller didn't specify a source (source_id == "0"), fall back to
  // the first available screen. When a specific source_id was requested but
  // isn't in the (possibly stale) cached list, rebuild the list and retry
  // the match instead of silently capturing the wrong source.
  if (!source.get() && !sources_.empty() && source_id == "0") {
    source = sources_.front();
  }
  if (!source.get()) {
    EncodableList types;
    types.push_back(EncodableValue(std::string("screen")));
    BuildDesktopSourcesList(types, true);
    for (auto src : sources_) {
      if (src->id().std_string() == source_id) {
        source = src;
      }
    }
    if (!source.get() && !sources_.empty() && source_id == "0") {
      source = sources_.front();
    }
  }
#endif

  if (!source.get()) {
    result->Error("Bad Arguments", "source not found!");
    return;
  }

  scoped_refptr<RTCDesktopCapturer> desktop_capturer =
      base_->desktop_device_->CreateDesktopCapturer(source, show_cursor);

  if (!desktop_capturer.get()) {
    result->Error("Bad Arguments", "CreateDesktopCapturer failed!");
    return;
  }

  desktop_capturer->RegisterDesktopCapturerObserver(this);

  const char* video_source_label = "screen_capture_input";

  scoped_refptr<RTCVideoSource> video_source =
      base_->factory_->CreateDesktopSource(
          desktop_capturer, video_source_label,
          base_->ParseMediaConstraints(video_constraints));

  // TODO: RTCVideoSource -> RTCVideoTrack

  scoped_refptr<RTCVideoTrack> track =
      base_->factory_->CreateVideoTrack(video_source, uuid.c_str());

  EncodableList videoTracks;
  EncodableMap info;
  info[EncodableValue("id")] = EncodableValue(track->id().std_string());
  info[EncodableValue("label")] = EncodableValue(track->id().std_string());
  info[EncodableValue("kind")] = EncodableValue(track->kind().std_string());
  info[EncodableValue("enabled")] = EncodableValue(track->enabled());
  videoTracks.push_back(EncodableValue(info));
  params[EncodableValue("videoTracks")] = EncodableValue(videoTracks);

  stream->AddTrack(track);

  base_->local_tracks_[track->id().std_string()] = track;

  base_->local_streams_[uuid] = stream;

  desktop_capturer->Start(uint32_t(fps));

  result->Success(EncodableValue(params));
}

}  // namespace flutter_webrtc_plugin
