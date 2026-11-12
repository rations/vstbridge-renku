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
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <cstdint>

/**
 * Where the native side can find a plugin editor that the Wine host's graphics
 * driver shows in another program's view.
 *
 * On Haiku-family systems an editor cannot be embedded into the host's window
 * the way X11 reparenting does it: app_server only lets a program create views
 * in its own windows. RenkuOS's Wine graphics driver, winerenku.drv, instead
 * offers to show one of its top-level windows in a view that another program
 * draws, keeping the pixels in an area that program clones and taking its input
 * as if it came from a window of the driver's own. The protocol is in
 * `src/include/renku/renku_host.h`, a copy of the driver's own header.
 *
 * The Wine host opts its editor window in and reads these two values back off
 * the window; the native side then addresses the driver with them.
 */
struct EditorHost {
    /**
     * The team id of the Wine host process, which the native side needs in
     * order to reach the driver's `BApplication`.
     */
    int32_t team = -1;

    /**
     * The editor window's handle, which names it in every message.
     */
    uint64_t cookie = 0;

    template <typename S>
    void serialize(S& s) {
        s.value4b(team);
        s.value8b(cookie);
    }
};
