// vstbridge: a Wine VST/CLAP plugin bridge
// Copyright (C) 2020-2024 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

// Haiku-only. The native half of the editor bridge: a `BView` inside the
// host's editor view that shows the plugin's editor window.
//
// The editor runs in another process, and app_server only lets a program
// create views in its own windows, so it cannot be embedded here the way X11
// reparenting does it. Instead the Wine host's graphics driver, winerenku.drv,
// shows that one window through this view: it keeps the window's pixels in an
// area this view clones, and takes this view's mouse and keyboard input as if
// it came from a window of its own. The protocol is in
// `src/include/renku/renku_host.h`, a copy of the driver's header.
//
// **This header deliberately pulls in no BeAPI headers at all, and the `BView`
// itself is defined in the corresponding `.cpp`.** The Interface Kit and the
// VST3 SDK cannot coexist in one translation unit: `SupportDefs.h` and the
// SDK's `pluginterfaces/base/ftypes.h` both declare `int8`, `int16` and
// friends, and the compiler rejects the second one it sees. Everything that
// talks to the VST3 API therefore reaches the view through this class, whose
// interface names no BeAPI type -- the parent view arrives as a `void*`.

#include <memory>

#include "../common/editor-host.h"
#include "../common/logging/common.h"

/**
 * The `BView` and its state. Defined in `editor-host-haiku.cpp`, where BeAPI
 * headers are safe to include.
 */
class HostedEditorViewImpl;

/**
 * Shows a bridged plugin editor inside a view belonging to the host.
 *
 * The host has to call `IPlugView::removed()` or `effEditClose` before it
 * destroys the parent view, as both plugin APIs require, so that `detach()`
 * takes this view out first. Otherwise the parent would delete it as one of
 * its children.
 */
class HostedEditorView {
   public:
    /**
     * @param logger A logger instance. Used from the host window's looper
     *   thread, and only for events that happen once, never per frame.
     */
    explicit HostedEditorView(Logger& logger);

    /**
     * Removes the view from the host's window if it is still attached.
     */
    ~HostedEditorView();

    HostedEditorView(const HostedEditorView&) = delete;
    HostedEditorView& operator=(const HostedEditorView&) = delete;
    HostedEditorView(HostedEditorView&&) = delete;
    HostedEditorView& operator=(HostedEditorView&&) = delete;

    /**
     * Add a view filling `parent` and start showing the editor window `host`
     * names in it, locking the host's window around the change. Replaces a
     * view attached before.
     *
     * @param parent The `BView*` the host passed to `IPlugView::attached()` or
     *   `effEditOpen`, untyped here so this header stays free of BeAPI types.
     * @param host Where the Wine host's graphics driver can be reached, as the
     *   Wine host read it off the editor window.
     *
     * @return Whether the view could be attached. A parent that does not belong
     *   to a window cannot be attached to.
     */
    bool attach(void* parent, const EditorHost& host);

    /**
     * Stop showing the editor and remove the view from its parent again. Safe
     * to call when it was never attached.
     */
    void detach();

   private:
    Logger& logger_;

    /**
     * Owned here rather than by the parent view, so that this object's lifetime
     * governs the view's. `detach()` takes it out of the host's window first.
     */
    std::unique_ptr<HostedEditorViewImpl> impl_;
};
