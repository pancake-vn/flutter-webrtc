//
//  FlutterSocketConnectionFrameReader.m
//  RCTWebRTC
//
//  Created by Alex-Dan Bumbu on 06/01/2021.
//

#include <mach/mach_time.h>

#import <ReplayKit/ReplayKit.h>
#import <WebRTC/RTCCVPixelBuffer.h>
#import <WebRTC/RTCVideoFrameBuffer.h>

#import "FlutterSocketConnection.h"
#import "FlutterSocketConnectionFrameReader.h"

const NSUInteger kMaxReadLength = 10 * 1024;

@interface Message : NSObject

@property(nonatomic, assign, readonly) CVImageBufferRef imageBuffer;
@property(nonatomic, copy, nullable) void (^didComplete)(BOOL succes, Message* message);

- (NSInteger)appendBytes:(UInt8*)buffer length:(NSUInteger)length;

@end

@interface Message ()

@property(nonatomic, assign) CVImageBufferRef imageBuffer;
@property(nonatomic, assign) int imageOrientation;
@property(nonatomic, assign) CFHTTPMessageRef framedMessage;

@end

@implementation Message

- (instancetype)init {
  self = [super init];
  if (self) {
    self.imageBuffer = NULL;
  }

  return self;
}

- (void)dealloc {
  CVPixelBufferRelease(_imageBuffer);
}

/** Returns the amount of missing bytes to complete the message, or -1 when not enough bytes were
 * provided to compute the message length */
- (NSInteger)appendBytes:(UInt8*)buffer length:(NSUInteger)length {
  if (!_framedMessage) {
    _framedMessage = CFHTTPMessageCreateEmpty(kCFAllocatorDefault, false);
  }

  CFHTTPMessageAppendBytes(_framedMessage, buffer, length);
  if (!CFHTTPMessageIsHeaderComplete(_framedMessage)) {
    return -1;
  }

  NSInteger contentLength = [CFBridgingRelease(CFHTTPMessageCopyHeaderFieldValue(
      _framedMessage, (__bridge CFStringRef) @"Content-Length")) integerValue];
  NSInteger bodyLength =
      (NSInteger)[CFBridgingRelease(CFHTTPMessageCopyBody(_framedMessage)) length];

  NSInteger missingBytesCount = contentLength - bodyLength;
  if (missingBytesCount == 0) {
    BOOL success = [self unwrapMessage:self.framedMessage];
    self.didComplete(success, self);

    CFRelease(self.framedMessage);
    self.framedMessage = NULL;
  }

  return missingBytesCount;
}

// MARK: Private Methods

- (CIContext*)imageContext {
  // Initializing a CIContext object is costly, so we use a singleton instead
  static CIContext* imageContext = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    imageContext = [[CIContext alloc] initWithOptions:nil];
  });

  return imageContext;
}

- (BOOL)unwrapMessage:(CFHTTPMessageRef)framedMessage {
  size_t width = [CFBridgingRelease(CFHTTPMessageCopyHeaderFieldValue(
      _framedMessage, (__bridge CFStringRef) @"Buffer-Width")) integerValue];
  size_t height = [CFBridgingRelease(CFHTTPMessageCopyHeaderFieldValue(
      _framedMessage, (__bridge CFStringRef) @"Buffer-Height")) integerValue];
  _imageOrientation = [CFBridgingRelease(CFHTTPMessageCopyHeaderFieldValue(
      _framedMessage, (__bridge CFStringRef) @"Buffer-Orientation")) intValue];

  NSData* messageData = CFBridgingRelease(CFHTTPMessageCopyBody(_framedMessage));

  // Copy the pixel buffer
  CVReturn status = CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                                        kCVPixelFormatType_32BGRA, NULL, &_imageBuffer);
  if (status != kCVReturnSuccess) {
    NSLog(@"CVPixelBufferCreate failed");
    return false;
  }

  [self copyImageData:messageData toPixelBuffer:&_imageBuffer];

  return true;
}

- (void)copyImageData:(NSData*)data toPixelBuffer:(CVPixelBufferRef*)pixelBuffer {
  CVPixelBufferLockBaseAddress(*pixelBuffer, 0);

  CIImage* image = [CIImage imageWithData:data];
  [self.imageContext render:image toCVPixelBuffer:*pixelBuffer];

  CVPixelBufferUnlockBaseAddress(*pixelBuffer, 0);
}

@end

// MARK: -

@interface FlutterSocketConnectionFrameReader () <NSStreamDelegate>

@property(nonatomic, strong) FlutterSocketConnection* connection;
@property(nonatomic, strong) Message* message;

@end

// A still screen brings no frames, and a viewer takes a share that sends none for a dead one: past this long
// without a frame, the last one is sent again.
static const int64_t kStillFrameIntervalNs = NSEC_PER_SEC;

@implementation FlutterSocketConnectionFrameReader {
  mach_timebase_info_data_t _timebaseInfo;
  NSInteger _readLength;
  int64_t _startTimeStampNs;
  // Under @synchronized(self): frames come on the socket's thread, and again from the timer's queue.
  id<RTCVideoFrameBuffer> _lastBuffer;
  RTCVideoRotation _lastRotation;
  int64_t _lastTimeStampNs;
  dispatch_source_t _stillFrameTimer;
}

- (instancetype)initWithDelegate:(__weak id<RTCVideoCapturerDelegate>)delegate {
  self = [super initWithDelegate:delegate];
  if (self) {
    mach_timebase_info(&_timebaseInfo);
  }

  return self;
}

- (void)startCaptureWithConnection:(FlutterSocketConnection*)connection {
  _startTimeStampNs = -1;

  self.connection = connection;
  self.message = nil;

  [self startStillFrameTimer];
  [self.connection openWithStreamDelegate:self];
}

- (void)stopCapture {
  [self stopStillFrameTimer];
  [self.connection close];
}

- (void)dealloc {
  if (_stillFrameTimer) {
    dispatch_source_cancel(_stillFrameTimer);
  }
}

// MARK: Private Methods

- (void)readBytesFromStream:(NSInputStream*)stream {
  if (!stream.hasBytesAvailable) {
    return;
  }

  if (!self.message) {
    self.message = [[Message alloc] init];
    _readLength = kMaxReadLength;

    __weak __typeof__(self) weakSelf = self;
    self.message.didComplete = ^(BOOL success, Message* message) {
      if (success) {
        [weakSelf didCaptureVideoFrame:message.imageBuffer
                       withOrientation:message.imageOrientation];
      }

      weakSelf.message = nil;
    };
  }

  uint8_t buffer[_readLength];
  NSInteger numberOfBytesRead = [stream read:buffer maxLength:_readLength];
  if (numberOfBytesRead < 0) {
    NSLog(@"error reading bytes from stream");
    return;
  }

  _readLength = [self.message appendBytes:buffer length:numberOfBytesRead];
  if (_readLength == -1 || _readLength > kMaxReadLength) {
    _readLength = kMaxReadLength;
  }
}

- (void)startStillFrameTimer {
  [self stopStillFrameTimer];

  dispatch_queue_t queue =
      dispatch_queue_create("com.cloudwebrtc.flutterwebrtc.broadcast.still-frame", DISPATCH_QUEUE_SERIAL);
  dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
  dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, kStillFrameIntervalNs / 2),
                            kStillFrameIntervalNs / 2, kStillFrameIntervalNs / 10);
  __weak __typeof__(self) weakSelf = self;
  dispatch_source_set_event_handler(timer, ^{
    [weakSelf sendLastFrameAgainIfStill];
  });
  dispatch_resume(timer);

  @synchronized(self) {
    _stillFrameTimer = timer;
  }
}

- (void)stopStillFrameTimer {
  @synchronized(self) {
    if (_stillFrameTimer) {
      dispatch_source_cancel(_stillFrameTimer);
      _stillFrameTimer = nil;
    }
    _lastBuffer = nil;
  }
}

- (void)sendLastFrameAgainIfStill {
  @synchronized(self) {
    if (!_lastBuffer || [self currentTimeStampNs] - _lastTimeStampNs < kStillFrameIntervalNs) {
      return;
    }
    [self deliverBuffer:_lastBuffer rotation:_lastRotation];
  }
}

- (int64_t)currentTimeStampNs {
  return mach_absolute_time() * _timebaseInfo.numer / _timebaseInfo.denom;
}

// Under @synchronized(self).
- (void)deliverBuffer:(id<RTCVideoFrameBuffer>)buffer rotation:(RTCVideoRotation)rotation {
  int64_t currentTimeStampNs = [self currentTimeStampNs];
  if (_startTimeStampNs < 0) {
    _startTimeStampNs = currentTimeStampNs;
  }

  _lastBuffer = buffer;
  _lastRotation = rotation;
  _lastTimeStampNs = currentTimeStampNs;

  RTCVideoFrame* videoFrame = [[RTCVideoFrame alloc] initWithBuffer:buffer
                                                           rotation:rotation
                                                        timeStampNs:currentTimeStampNs - _startTimeStampNs];
  [self.delegate capturer:self didCaptureVideoFrame:videoFrame];
}

- (void)didCaptureVideoFrame:(CVPixelBufferRef)pixelBuffer
             withOrientation:(CGImagePropertyOrientation)orientation {
  RTCCVPixelBuffer* rtcPixelBuffer = [[RTCCVPixelBuffer alloc] initWithPixelBuffer:pixelBuffer];

  RTCVideoRotation rotation;
  switch (orientation) {
    case kCGImagePropertyOrientationLeft:
      rotation = RTCVideoRotation_90;
      break;
    case kCGImagePropertyOrientationDown:
      rotation = RTCVideoRotation_180;
      break;
    case kCGImagePropertyOrientationRight:
      rotation = RTCVideoRotation_270;
      break;
    default:
      rotation = RTCVideoRotation_0;
      break;
  }

  @synchronized(self) {
    [self deliverBuffer:[rtcPixelBuffer toI420] rotation:rotation];
  }
}

@end

@implementation FlutterSocketConnectionFrameReader (NSStreamDelegate)

- (void)stream:(NSStream*)aStream handleEvent:(NSStreamEvent)eventCode {
  switch (eventCode) {
    case NSStreamEventOpenCompleted:
      NSLog(@"server stream open completed");
      break;
    case NSStreamEventHasBytesAvailable:
      [self readBytesFromStream:(NSInputStream*)aStream];
      break;
    case NSStreamEventEndEncountered:
      NSLog(@"server stream end encountered");
      [self stopCapture];
      break;
    case NSStreamEventErrorOccurred:
      NSLog(@"server stream error encountered: %@", aStream.streamError.localizedDescription);
      break;

    default:
      break;
  }
}

@end
