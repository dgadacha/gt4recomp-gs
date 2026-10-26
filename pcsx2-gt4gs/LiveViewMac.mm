// SPDX-License-Identifier: MIT
// GT4Recomp fork, macOS: see LiveViewMac.h. Built with ARC (pcsx2-gt4gs/CMakeLists.txt).
#import "LiveViewMac.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cstdio>

// Private Core Animation and WindowServer interfaces, declared as Chromium declares them for the same purpose
// (ui/base/cocoa/remote_layer_api.h): a CAContext made on this process's WindowServer connection can be shown by
// another process of the same login session through a CALayerHost given its contextId.
typedef uint32_t CGSConnectionID;
extern "C" CGSConnectionID CGSMainConnectionID(void);

@interface CAContext : NSObject
+ (instancetype)contextWithCGSConnection:(CGSConnectionID)connection options:(NSDictionary*)options;
@property(readonly) uint32_t contextId;
@property(retain) CALayer* layer;
@end

namespace
{
	CAContext* s_context = nil;
	NSView* s_view = nil;
	id<NSObject> s_activity = nil;
	std::atomic<std::uint32_t> s_context_id{0};
	std::atomic<std::uint32_t> s_width{0};
	std::atomic<std::uint32_t> s_height{0};

	// Main thread: one point per pixel, anchored at the origin like the game's host layer.
	void ApplyGeometry(CALayer* layer, std::uint32_t width, std::uint32_t height)
	{
		layer.anchorPoint = CGPointZero;
		layer.position = CGPointZero;
		layer.bounds = CGRectMake(0, 0, width, height);
		layer.contentsScale = 1.0;
	}
} // namespace

// GSDeviceMTL::AttachSurfaceOnMainThread makes its CAMetalLayer the view's layer (again after every UpdateWindow) and
// DetachSurfaceOnMainThread clears it; the view hands each one to the remote context, whose id never changes.
@interface GT4GSLiveView : NSView
@end

@implementation GT4GSLiveView
- (void)setLayer:(CALayer*)layer
{
	[super setLayer:layer];
	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	if (layer)
		ApplyGeometry(layer, s_width.load(), s_height.load());
	s_context.layer = layer;
	[CATransaction commit];
}
@end

void* gt4gs::mac::CreateLiveView(std::uint32_t width, std::uint32_t height)
{
	@autoreleasepool
	{
		[NSApplication sharedApplication];
		// No Dock icon and no menu bar: the picture is shown in the game's window.
		[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
		// A process without a visible window is a candidate for App Nap, which would throttle the GS threads.
		s_activity = [[NSProcessInfo processInfo]
			beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
							  reason:@"gt4-gs draws the game's picture"];
		s_context = [CAContext contextWithCGSConnection:CGSMainConnectionID() options:@{}];
		if (!s_context || s_context.contextId == 0)
		{
			std::fprintf(stderr, "gt4-gs: cannot create the remote layer context\n");
			s_context = nil;
			return nullptr;
		}
		s_context_id.store(s_context.contextId);
		s_width.store(width);
		s_height.store(height);
		s_view = [[GT4GSLiveView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
		return (__bridge void*)s_view;
	}
}

void gt4gs::mac::DestroyLiveView()
{
	@autoreleasepool
	{
		s_context.layer = nil;
		s_context = nil;
		s_view = nil;
		s_context_id.store(0);
		if (s_activity)
		{
			[[NSProcessInfo processInfo] endActivity:s_activity];
			s_activity = nil;
		}
	}
}

std::uint32_t gt4gs::mac::LiveContextId()
{
	return s_context_id.load();
}

void gt4gs::mac::GetLiveViewSize(std::uint32_t* width, std::uint32_t* height)
{
	*width = s_width.load();
	*height = s_height.load();
}

void gt4gs::mac::ResizeLiveView(std::uint32_t width, std::uint32_t height, bool wait)
{
	s_width.store(width);
	s_height.store(height);
	void (^apply)(void) = ^{
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		[s_view setFrame:NSMakeRect(0, 0, width, height)];
		if (CALayer* layer = s_view.layer)
			ApplyGeometry(layer, width, height);
		[CATransaction commit];
	};
	if ([NSThread isMainThread])
		apply();
	else if (wait)
		dispatch_sync(dispatch_get_main_queue(), apply);
	else
		dispatch_async(dispatch_get_main_queue(), apply);
}
