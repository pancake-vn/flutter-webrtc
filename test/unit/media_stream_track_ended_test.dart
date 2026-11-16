import 'package:flutter/services.dart';

import 'package:flutter_test/flutter_test.dart';
import 'package:webrtc_interface/webrtc_interface.dart';

import 'package:flutter_webrtc/src/native/mediadevices_impl.dart';

/// Android reports a screen capture the OS stopped as `mediaStreamTrackEnded`
/// on the global event channel; the display track hears it as `onEnded`.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const methodChannel = MethodChannel('FlutterWebRTC.Method');
  const eventChannel = EventChannel('FlutterWebRTC.Event');
  late MockStreamHandlerEventSink events;
  var nextTrack = 0;

  setUpAll(() {
    messenger.setMockStreamHandler(
      eventChannel,
      MockStreamHandler.inline(onListen: (_, sink) => events = sink),
    );
    messenger.setMockMethodCallHandler(methodChannel, (call) async {
      if (call.method != 'getDisplayMedia') return null;
      return {
        'streamId': 'stream',
        'audioTracks': [],
        'videoTracks': [
          {
            'id': 'track-${nextTrack++}',
            'label': 'video',
            'kind': 'video',
            'enabled': true,
          },
        ],
      };
    });
  });

  Future<(MediaStreamTrack, List<void>)> share() async {
    final stream = await MediaDeviceNative.instance
        .getDisplayMedia(<String, dynamic>{'video': true});
    final track = stream.getVideoTracks().single;
    final ended = <void>[];
    track.onEnded = () => ended.add(null);
    return (track, ended);
  }

  Future<void> platformEnds(String trackId) async {
    events.success({'event': 'mediaStreamTrackEnded', 'trackId': trackId});
    await pumpEventQueue();
  }

  test('a display track the platform ended reports it once', () async {
    final (track, ended) = await share();

    await platformEnds(track.id!);
    await platformEnds(track.id!);

    expect(ended, hasLength(1));
  });

  test('a display track the app stopped reports nothing', () async {
    final (track, ended) = await share();

    await track.stop();
    await platformEnds(track.id!);

    expect(ended, isEmpty);
  });

  test('an end for another track leaves this one alone', () async {
    final (_, ended) = await share();

    await platformEnds('another');

    expect(ended, isEmpty);
  });
}
