// SPDX-License-Identifier: MIT
// GT4Recomp fork, macOS: the live-mode picture of gt4-gs. A macOS process cannot put a view inside another process's
// window, so gt4-gs has no window at all in live mode: the Metal device draws into the layer of an off-screen view,
// that layer is the root of a remote Core Animation context, and the game shows the context in its own window
// (CALayerHost) from the id that WINDOW restored carries (gs_stream::WindowPayload::layer_id). Sizes are pixels: the
// layer has one point per pixel and the game scales its host layer by the inverse of its backing scale.
#pragma once

#include <cstdint>

namespace gt4gs::mac
{
	// Main thread. Creates the view and the remote context; returns the NSView* for WindowInfo::window_handle, or
	// nullptr when the context cannot be created.
	void* CreateLiveView(std::uint32_t width, std::uint32_t height);
	// Main thread.
	void DestroyLiveView();
	// The remote context id, 0 without a live view.
	std::uint32_t LiveContextId();
	// The current size of the view in pixels (any thread).
	void GetLiveViewSize(std::uint32_t* width, std::uint32_t* height);
	// Any thread: the view and its layer take this size; with wait, returns once the main thread has applied it.
	void ResizeLiveView(std::uint32_t width, std::uint32_t height, bool wait);
} // namespace gt4gs::mac
