#ifndef FLUTTER_SCRREN_CAPTURE_HXX
#define FLUTTER_SCRREN_CAPTURE_HXX

#include "flutter_common.h"
#include "flutter_webrtc_base.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "loopback_capturer.h"
#include "rtc_audio_source.h"
#include "rtc_audio_track.h"
#include "rtc_desktop_capturer.h"
#include "rtc_desktop_media_list.h"

namespace flutter_webrtc_plugin {

class FlutterScreenCapture : public MediaListObserver,
                             public DesktopCapturerObserver {
 public:
  FlutterScreenCapture(FlutterWebRTCBase* base);
  ~FlutterScreenCapture();

  void GetDisplayMedia(const EncodableMap& constraints,
                       std::unique_ptr<MethodResultProxy> result);

  void GetDesktopSources(const EncodableList& types,
                         std::unique_ptr<MethodResultProxy> result);

  void UpdateDesktopSources(const EncodableList& types,
                            std::unique_ptr<MethodResultProxy> result);

  void GetDesktopSourceThumbnail(std::string source_id,
                                 int width,
                                 int height,
                                 std::unique_ptr<MethodResultProxy> result);

 protected:
  void OnMediaSourceAdded(scoped_refptr<MediaSource> source) override;

  void OnMediaSourceRemoved(scoped_refptr<MediaSource> source) override;

  void OnMediaSourceNameChanged(scoped_refptr<MediaSource> source) override;

  void OnMediaSourceThumbnailChanged(
      scoped_refptr<MediaSource> source) override;

  void OnStart(scoped_refptr<RTCDesktopCapturer> capturer) override;

  void OnPaused(scoped_refptr<RTCDesktopCapturer> capturer) override;

  void OnStop(scoped_refptr<RTCDesktopCapturer> capturer) override;

  void OnError(scoped_refptr<RTCDesktopCapturer> capturer) override;

 private:
  using SourceList = std::vector<scoped_refptr<MediaSource>>;

  bool BuildDesktopSourcesList(const EncodableList& types, bool force_reload);

  // Updates the media lists for [types] and returns their sources in
  // [sources]. Touches only the media lists, never [sources_].
  bool UpdateMediaLists(const EncodableList& types,
                        bool force_reload,
                        SourceList* sources);

  // Lists the sources for [types], then calls [done] on the platform thread
  // with [sources_] already replaced.
  //
  // On Windows the listing runs on [lister_]: WebRTC skips any window that
  // does not answer WM_NULL within 50 ms, and this app's own windows belong
  // to the platform thread, so listing from it left all of them out.
  void ListSources(const EncodableList& types,
                   bool force_reload,
                   std::function<void(bool ok)> done);

  void RunLister();

 private:
  FlutterWebRTCBase* base_;
  std::map<DesktopType, scoped_refptr<RTCDesktopMediaList>> medialist_;
  SourceList sources_;

  // The only thread that touches [medialist_] once it has started.
  std::thread lister_;
  std::mutex lister_mutex_;
  std::condition_variable lister_wake_;
  std::deque<std::function<void()>> lister_tasks_;
  bool lister_stopping_ = false;

  // Dropped on destruction, so a reply already queued on the platform thread
  // does nothing once this is gone.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  // Loopback audio capturer active during a screen-share session.
  // Null when not capturing or on platforms without loopback support.
  std::unique_ptr<LoopbackCapturer> loopback_capturer_;
  // The custom audio source fed by the loopback capturer.
  scoped_refptr<RTCAudioSource> loopback_audio_source_;
};

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_SCRREN_CAPTURE_HXX