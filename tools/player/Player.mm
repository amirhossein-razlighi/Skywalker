// The windowed player: a Cocoa window with a CAMetalLayer, keyboard / mouse / gamepad input, and the display loop.
//
//   display link (or a fixed 60 Hz timer in automated runs)
//     -> poll gamepads -> Engine::update(dt) (fixed-step simulation, audio) -> Engine::renderToSurface(layer)
//
// No SwiftUI, no editor code: this is the same engine the editor uses, driven by the C++ API directly.

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <exception>
#include <set>
#include <string>

#include "Player.h"
#include "skywalker/core/Log.h"
#include "skywalker/input/InputState.h"
#include "skywalker/render/Image.h"

using namespace sky;
using sky::player::Options;
using sky::player::Session;

@class SkyPlayer;

// ---------------------------------------------------------------------------------------------------------
// Crash reporting: a log with a backtrace, then the default handler (so macOS still files its crash report).
// ---------------------------------------------------------------------------------------------------------

namespace {

int gCrashFd = -1;

void writeCrash(const char* text) {
    if (gCrashFd >= 0) (void)!::write(gCrashFd, text, strlen(text));
    (void)!::write(STDERR_FILENO, text, strlen(text));
}

void crashHandler(int sig) {
    const char* name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : "SIGABRT";
    writeCrash("\nskywalker-player crashed with ");
    writeCrash(name);
    writeCrash("\n");
    void* frames[64];
    int n = backtrace(frames, 64);
    if (gCrashFd >= 0) backtrace_symbols_fd(frames, n, gCrashFd);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

void installCrashReporting(const std::string& gameName) {
    NSString* logs = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/SkywalkerGames"];
    [[NSFileManager defaultManager] createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
    std::string safe;
    for (char c : gameName) safe += (isalnum(static_cast<unsigned char>(c)) ? c : '_');
    NSString* path = [logs stringByAppendingPathComponent:[NSString stringWithFormat:@"%s-crash.log", safe.c_str()]];
    gCrashFd = ::open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) signal(sig, crashHandler);
    std::set_terminate([] {
        writeCrash("\nskywalker-player: unhandled C++ exception");
        try {
            if (auto e = std::current_exception()) std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            writeCrash(": ");
            writeCrash(ex.what());
        } catch (...) {
        }
        writeCrash("\n");
        std::abort();
    });
}

/// Key names the engine understands (input.json "key:<name>").
NSString* keyNameForEvent(NSEvent* event) {
    switch (event.keyCode) {
        case 49: return @"space";
        case 36: case 76: return @"enter";
        case 53: return @"escape";
        case 123: return @"left";
        case 124: return @"right";
        case 125: return @"down";
        case 126: return @"up";
        case 48: return @"tab";
        case 51: return @"backspace";
        case 117: return @"delete";
        case 115: return @"home";
        case 119: return @"end";
        case 116: return @"pageup";
        case 121: return @"pagedown";
        case 122: return @"f1";
        case 120: return @"f2";
        case 99: return @"f3";
        case 118: return @"f4";
        case 96: return @"f5";
        case 97: return @"f6";
        case 98: return @"f7";
        case 100: return @"f8";
        case 101: return @"f9";
        case 109: return @"f10";
        case 103: return @"f11";
        case 111: return @"f12";
        default: return [event.charactersIgnoringModifiers lowercaseString];
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// The game view
// ---------------------------------------------------------------------------------------------------------

@interface SkyGameView : NSView
@property(nonatomic, weak) SkyPlayer* player;
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
- (void)releaseAllInput;
@end

@interface SkyPlayer : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithSession:(Session*)session options:(const Options&)options;
- (void)run;
- (Engine&)engine;
- (BOOL)quitOnEscape;
- (void)quit;
@end

@implementation SkyGameView {
    NSTrackingArea* _tracking;
    NSEventModifierFlags _heldModifiers;
    NSMutableSet<NSString*>* _heldKeys;
    BOOL _heldMouse[3];
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        _heldKeys = [NSMutableSet set];
    }
    return self;
}

- (CALayer*)makeBackingLayer {
    CAMetalLayer* l = [CAMetalLayer layer];
    l.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    l.framebufferOnly = YES;
    l.maximumDrawableCount = 3;
    return l;
}

- (CAMetalLayer*)metalLayer {
    return (CAMetalLayer*)self.layer;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }

- (CGFloat)scale { return self.window ? self.window.backingScaleFactor : 2.0; }

- (void)updateDrawableSize {
    CGFloat s = [self scale];
    self.metalLayer.contentsScale = s;
    self.metalLayer.drawableSize = CGSizeMake(MAX(1.0, self.bounds.size.width * s), MAX(1.0, self.bounds.size.height * s));
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self updateDrawableSize];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:self.bounds
                                             options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                               owner:self
                                            userInfo:nil];
    [self addTrackingArea:_tracking];
}

// MARK: Keyboard

- (void)keyDown:(NSEvent*)event {
    if (event.modifierFlags & NSEventModifierFlagCommand) {
        [super keyDown:event];  // ⌘ shortcuts belong to the menu; the key-up would never arrive while ⌘ is held
        return;
    }
    if (event.keyCode == 53 && [self.player quitOnEscape]) {
        [self.player quit];
        return;
    }
    NSString* name = keyNameForEvent(event);
    if (name.length == 0 || event.isARepeat) return;
    [_heldKeys addObject:name];
    [self.player engine].input().keyEvent(name.UTF8String, true);
}

- (void)keyUp:(NSEvent*)event {
    NSString* name = keyNameForEvent(event);
    if (name.length == 0) return;
    [_heldKeys removeObject:name];
    [self.player engine].input().keyEvent(name.UTF8String, false);
}

/// shift / control / option / command are not key events: derive them from flag changes.
- (void)flagsChanged:(NSEvent*)event {
    NSEventModifierFlags now = event.modifierFlags & (NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagOption | NSEventModifierFlagCommand);
    struct Mod { NSEventModifierFlags flag; const char* name; };
    for (Mod m : {Mod{NSEventModifierFlagShift, "shift"}, Mod{NSEventModifierFlagControl, "ctrl"}, Mod{NSEventModifierFlagOption, "alt"}, Mod{NSEventModifierFlagCommand, "cmd"}}) {
        bool was = (_heldModifiers & m.flag) != 0, is = (now & m.flag) != 0;
        if (was != is) [self.player engine].input().keyEvent(m.name, is);
    }
    _heldModifiers = now;
}

// MARK: Mouse

- (void)forwardMouse:(NSEvent*)event {
    Engine& engine = [self.player engine];
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    float w = MAX((float)self.bounds.size.width, 1.f), h = MAX((float)self.bounds.size.height, 1.f);
    if (engine.cursorLocked()) {
        engine.input().mouseMove(0.5f, 0.5f, (float)event.deltaX, (float)event.deltaY);  // captured: only movement matters
    } else {
        engine.input().mouseMove((float)p.x / w, (float)p.y / h, (float)event.deltaX, (float)event.deltaY);
    }
}

- (void)mouseMoved:(NSEvent*)event { [self forwardMouse:event]; }
- (void)mouseDragged:(NSEvent*)event { [self forwardMouse:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self forwardMouse:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self forwardMouse:event]; }

- (void)button:(int)b down:(BOOL)down event:(NSEvent*)event {
    Engine& engine = [self.player engine];
    [self forwardMouse:event];
    _heldMouse[b] = down;
    engine.input().mouseButton(b, down);
    if (down && b == 0) {
        // `on click` handlers: the entity under the cursor (the screen center while the cursor is captured).
        CGFloat s = [self scale];
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        if (engine.cursorLocked()) p = NSMakePoint(self.bounds.size.width / 2, self.bounds.size.height / 2);
        EntityId hit = engine.pickAt((float)(p.x * s), (float)(p.y * s), (int)(self.bounds.size.width * s), (int)(self.bounds.size.height * s));
        if (hit != kNoEntity) engine.input().clicked.push_back(hit);
    }
}

- (void)mouseDown:(NSEvent*)event { [self.window makeFirstResponder:self]; [self button:0 down:YES event:event]; }
- (void)mouseUp:(NSEvent*)event { [self button:0 down:NO event:event]; }
- (void)rightMouseDown:(NSEvent*)event { [self button:1 down:YES event:event]; }
- (void)rightMouseUp:(NSEvent*)event { [self button:1 down:NO event:event]; }
- (void)otherMouseDown:(NSEvent*)event { [self button:2 down:YES event:event]; }
- (void)otherMouseUp:(NSEvent*)event { [self button:2 down:NO event:event]; }

- (void)scrollWheel:(NSEvent*)event {
    [self.player engine].input().scrollBy((float)event.scrollingDeltaX, (float)event.scrollingDeltaY);
}

/// Focus left the game: let go of everything so nothing stays "held" (a key released elsewhere never reports here).
- (void)releaseAllInput {
    Engine& engine = [self.player engine];
    for (NSString* k in _heldKeys) engine.input().keyEvent(k.UTF8String, false);
    [_heldKeys removeAllObjects];
    for (int b = 0; b < 3; ++b) {
        if (_heldMouse[b]) engine.input().mouseButton(b, false);
        _heldMouse[b] = NO;
    }
    for (const char* m : {"shift", "ctrl", "alt", "cmd"}) engine.input().keyEvent(m, false);
    _heldModifiers = 0;
}

@end

// ---------------------------------------------------------------------------------------------------------
// The player
// ---------------------------------------------------------------------------------------------------------

@implementation SkyPlayer {
    Session* _session;
    Options _options;
    NSWindow* _window;
    SkyGameView* _view;
    CADisplayLink* _link;
    NSTimer* _timer;
    double _lastTimestamp;
    uint64_t _frames;
    BOOL _pausedByFocus;
    BOOL _cursorCaptured;
    BOOL _cursorHidden;
    BOOL _terminating;
    int _renderFailures;
    std::set<int> _announcedPads;
    dispatch_source_t _sigterm, _sigint;
}

- (instancetype)initWithSession:(Session*)session options:(const Options&)options {
    if ((self = [super init])) {
        _session = session;
        _options = options;
    }
    return self;
}

- (Engine&)engine { return *_session->engine; }
- (BOOL)quitOnEscape { return _session->settings.quitOnEscape; }
- (void)quit { [NSApp terminate:nil]; }

- (NSString*)gameName { return [NSString stringWithUTF8String:_session->settings.displayName().c_str()]; }

// MARK: Setup

- (void)buildMenu {
    NSString* name = [self gameName];
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu* app = [[NSMenu alloc] init];
    [app addItemWithTitle:[@"About " stringByAppendingString:name] action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem* others = [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = app;

    NSMenuItem* viewItem = [[NSMenuItem alloc] init];
    [bar addItem:viewItem];
    NSMenu* view = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem* fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    viewItem.submenu = view;

    NSMenuItem* windowItem = [[NSMenuItem alloc] init];
    [bar addItem:windowItem];
    NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
    [window addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [window addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    windowItem.submenu = window;
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = window;
}

- (void)buildWindow {
    const game::WindowSettings& w = _session->settings.window;
    NSRect screen = NSScreen.mainScreen.visibleFrame;
    CGFloat width = MIN((CGFloat)w.width, screen.size.width), height = MIN((CGFloat)w.height, screen.size.height);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (w.resizable) style |= NSWindowStyleMaskResizable;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height) styleMask:style backing:NSBackingStoreBuffered defer:NO];
    _window.title = [self gameName];
    _window.delegate = self;
    _window.releasedWhenClosed = NO;
    _window.acceptsMouseMovedEvents = YES;
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _window.contentMinSize = NSMakeSize(320, 200);
    _view = [[SkyGameView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    _view.player = self;
    _view.metalLayer.displaySyncEnabled = w.vsync;
    _window.contentView = _view;
    [_window center];
    [_window makeFirstResponder:_view];
    [_window makeKeyAndOrderFront:nil];
}

- (void)installSignalHandlers {
    // SIGTERM / SIGINT (from `game_run` stop, a terminal, a launcher) quit like ⌘Q so audio and the cursor are restored.
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    __weak SkyPlayer* weakSelf = self;
    _sigterm = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    _sigint = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    for (dispatch_source_t s : {_sigterm, _sigint}) {
        dispatch_source_set_event_handler(s, ^{ [weakSelf quit]; });
        dispatch_resume(s);
    }
}

- (void)run {
    [NSApplication sharedApplication];
    NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
    NSApp.delegate = self;
    [self buildMenu];
    [self installSignalHandlers];
    GCController.shouldMonitorBackgroundEvents = NO;  // a game must not react to a controller used in another app

    Engine& engine = *_session->engine;
    if (!_options.agentSocket.empty()) {
        if (Status s = engine.startAgentServer(_options.agentSocket); !s) fprintf(stderr, "skywalker-player: agent server: %s\n", s.error().message.c_str());
    }
    [self buildWindow];
    if (_session->settings.window.fullscreen && !_options.automated()) [_window toggleFullScreen:nil];

    engine.play();
    fprintf(stderr, "skywalker-player: playing %s (%s, %s)\n", _session->scene.c_str(), engine.renderer().info().backend.c_str(),
            engine.renderer().info().device.c_str());

    if (_options.automated()) {
        // Automated runs use a fixed 60 Hz step: deterministic, and independent of a display.
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0 target:self selector:@selector(timerFrame:) userInfo:nil repeats:YES];
        [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
        if (_options.quitAfter > 0) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(_options.quitAfter * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self quit]; });
        }
    } else {
        _link = [_view displayLinkWithTarget:self selector:@selector(displayFrame:)];
        const float maxRate = (float)(NSScreen.mainScreen.maximumFramesPerSecond > 0 ? NSScreen.mainScreen.maximumFramesPerSecond : 60);
        _link.preferredFrameRateRange = CAFrameRateRangeMake(30, maxRate, maxRate);
        [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
}

// MARK: Frames

- (void)displayFrame:(CADisplayLink*)link {
    double now = link.timestamp;
    double dt = _lastTimestamp > 0 ? now - _lastTimestamp : 1.0 / 60.0;
    _lastTimestamp = now;
    [self frame:dt];
}

- (void)timerFrame:(NSTimer*)timer { [self frame:1.0 / 60.0]; }

- (void)pollGamepads {
    Engine& engine = *_session->engine;
    NSArray<GCController*>* controllers = GCController.controllers;
    for (int index = 0; index < input::kMaxGamepads; ++index) {
        GCExtendedGamepad* pad = (NSUInteger)index < controllers.count ? controllers[(NSUInteger)index].extendedGamepad : nil;
        if (pad) {
            // Bit order matches input::PadButton / SkyGamepad.buttons.
            const BOOL pressed[] = {pad.buttonA.isPressed, pad.buttonB.isPressed, pad.buttonX.isPressed, pad.buttonY.isPressed,
                                    pad.leftShoulder.isPressed, pad.rightShoulder.isPressed, pad.leftTrigger.isPressed, pad.rightTrigger.isPressed,
                                    pad.buttonOptions.isPressed, pad.buttonMenu.isPressed, pad.leftThumbstickButton.isPressed, pad.rightThumbstickButton.isPressed,
                                    pad.dpad.up.isPressed, pad.dpad.down.isPressed, pad.dpad.left.isPressed, pad.dpad.right.isPressed,
                                    pad.buttonHome.isPressed};
            uint32_t bits = 0;
            for (uint32_t i = 0; i < sizeof(pressed) / sizeof(pressed[0]); ++i) {
                if (pressed[i]) bits |= 1u << i;
            }
            NSString* name = controllers[(NSUInteger)index].vendorName ?: @"Controller";
            engine.input().gamepad(index, true, name.UTF8String, pad.leftThumbstick.xAxis.value, pad.leftThumbstick.yAxis.value,
                                   pad.rightThumbstick.xAxis.value, pad.rightThumbstick.yAxis.value, pad.leftTrigger.value, pad.rightTrigger.value, bits);
            _announcedPads.insert(index);
        } else if (_announcedPads.erase(index)) {
            engine.input().gamepad(index, false, "", 0, 0, 0, 0, 0, 0, 0);  // unplugged
        }
    }
}

- (void)frame:(double)dt {
    if (_terminating) return;
    try {
        Engine& engine = *_session->engine;
        if (!_pausedByFocus) [self pollGamepads];
        engine.update(dt);
        [self drainLog];
        [self applyCursorState];
        if (engine.quitRequested()) {
            [self quit];
            return;
        }
        [self render];
        ++_frames;
        if (!_options.capture.empty() && _frames >= (uint64_t)_options.frames) [self captureAndQuit];
    } catch (const std::exception& e) {
        [self fatal:[NSString stringWithFormat:@"%s", e.what()]];
    } catch (...) {
        [self fatal:@"unknown error"];
    }
}

- (void)render {
    CAMetalLayer* layer = _view.metalLayer;
    const int w = (int)layer.drawableSize.width, h = (int)layer.drawableSize.height;
    if (w < 2 || h < 2 || _window.isMiniaturized) return;
    Status s = _session->engine->renderToSurface((__bridge void*)layer, w, h);
    if (s) {
        _renderFailures = 0;
        return;
    }
    if (++_renderFailures == 1) fprintf(stderr, "skywalker-player: render error: %s\n", s.error().message.c_str());
    // A drawable can be briefly unavailable (display change, mission control); a persistent failure is fatal.
    if (_renderFailures > 300) [self fatal:[NSString stringWithFormat:@"The game cannot draw: %s", s.error().message.c_str()]];
}

/// Game output reaches stderr (and the log file of game_run).
- (void)drainLog {
    for (const auto& e : _session->engine->drainEvents()) {
        if (e.get("type").asString() != "log") continue;
        const std::string kind = e.get("kind").asString();
        fprintf(stderr, "[%s] %s\n", kind.c_str(), e.get("text").asString().c_str());
    }
}

- (void)captureAndQuit {
    // The image of the last presented frame: exactly what the window shows (readback blocks until the GPU is done).
    auto image = _session->engine->renderer().readback();
    int code = 0;
    if (!image) {
        fprintf(stderr, "skywalker-player: capture failed: %s\n", image.error().message.c_str());
        code = 1;
    } else if (Status s = writePng(*image, _options.capture); !s) {
        fprintf(stderr, "skywalker-player: capture failed: %s\n", s.error().message.c_str());
        code = 1;
    } else {
        fprintf(stderr, "skywalker-player: wrote %s (%dx%d)\n", _options.capture.c_str(), image->width, image->height);
    }
    _terminating = YES;
    [self shutdown];
    exit(code);
}

// MARK: Cursor, focus

- (void)applyCursorState {
    const BOOL want = _session->engine->cursorLocked() && NSApp.isActive && _window.isKeyWindow && !_pausedByFocus;
    if (want == _cursorCaptured) return;
    _cursorCaptured = want;
    if (want) {
        // Hide the cursor and stop it from moving; mouse deltas keep arriving.
        if (!_cursorHidden) {
            [NSCursor hide];
            _cursorHidden = YES;
        }
        NSRect inWindow = [_window convertRectToScreen:[_view convertRect:_view.bounds toView:nil]];
        CGDirectDisplayID display = CGMainDisplayID();
        CGFloat screenH = (CGFloat)CGDisplayPixelsHigh(display);
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(inWindow), screenH - NSMidY(inWindow)));
        CGAssociateMouseAndMouseCursorPosition(false);
    } else {
        CGAssociateMouseAndMouseCursorPosition(true);
        if (_cursorHidden) {
            [NSCursor unhide];
            _cursorHidden = NO;
        }
    }
}

- (void)applicationDidResignActive:(NSNotification*)note {
    [_view releaseAllInput];
    if (_session->settings.pauseOnFocusLoss && _session->engine->playState() == PlayState::Playing) {
        _session->engine->pause();
        _pausedByFocus = YES;
    }
    [self applyCursorState];
}

- (void)applicationDidBecomeActive:(NSNotification*)note {
    if (_pausedByFocus) {
        _session->engine->play();
        _pausedByFocus = NO;
    }
    _lastTimestamp = 0;  // no catch-up step for the time spent in the background
    [self applyCursorState];
}

- (void)windowDidResignKey:(NSNotification*)note { [_view releaseAllInput]; }

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { return YES; }

- (void)windowDidMiniaturize:(NSNotification*)note { [_view releaseAllInput]; }

// MARK: Shutdown, errors

/// Stops everything in the order that keeps audio and the display clean.
- (void)shutdown {
    [_link invalidate];
    _link = nil;
    [_timer invalidate];
    _timer = nil;
    _terminating = YES;
    if (_cursorCaptured) {
        CGAssociateMouseAndMouseCursorPosition(true);
        _cursorCaptured = NO;
    }
    if (_cursorHidden) {
        [NSCursor unhide];
        _cursorHidden = NO;
    }
    if (_session->engine) {
        _session->engine->stopAgentServer();
        _session->engine->stop();
    }
}

- (void)applicationWillTerminate:(NSNotification*)note { [self shutdown]; }

/// The crash-safe error screen: stops the game and tells the player what went wrong instead of vanishing.
- (void)fatal:(NSString*)message {
    if (_terminating) return;
    [self shutdown];
    fprintf(stderr, "skywalker-player: fatal: %s\n", message.UTF8String);
    if (_options.automated()) exit(1);
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = [NSString stringWithFormat:@"%@ has stopped", [self gameName]];
    alert.informativeText = [message stringByAppendingString:@"\n\nThe game cannot continue. Your progress may not have been saved."];
    [alert addButtonWithTitle:@"Quit"];
    [alert addButtonWithTitle:@"Copy Details"];
    while ([alert runModal] == NSAlertSecondButtonReturn) {
        [NSPasteboard.generalPasteboard clearContents];
        [NSPasteboard.generalPasteboard setString:message forType:NSPasteboardTypeString];
    }
    exit(1);
}

@end

// ---------------------------------------------------------------------------------------------------------

namespace sky::player {

int runWindowed(Session& session, const Options& options) {
    @autoreleasepool {
        installCrashReporting(session.settings.displayName());
        SkyPlayer* player = [[SkyPlayer alloc] initWithSession:&session options:options];
        [player run];
    }
    return 0;
}

void showStartupError(const std::string& title, const std::string& message) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        [NSApp activateIgnoringOtherApps:YES];
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = [NSString stringWithUTF8String:title.c_str()];
        alert.informativeText = [NSString stringWithUTF8String:message.c_str()];
        [alert addButtonWithTitle:@"Quit"];
        [alert runModal];
    }
}

}  // namespace sky::player
