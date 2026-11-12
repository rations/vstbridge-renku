/*
 * RenkuOS driver: showing a window in a view of another program
 *
 * Copyright 2026 rations
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* The protocol between the driver and a host: a native program that shows one Wine window in
 * a view of its own, as a plug-in host shows a plug-in's editor. app_server lets a program
 * create views only in its own windows (ServerWindow::_CreateView in src/servers/app), so a
 * Wine window cannot be put into another program's window. Instead the driver gives a window
 * that asks for it no BWindow, keeps its pixels in an area the host clones, and takes the
 * host's input as if it came from a BWindow of its own.
 *
 * This header is plain C with no Wine or Be types, so that a host can carry a copy. A host
 * that changes it must change RENKU_HOST_VERSION.
 *
 * On the Wine side, a program asks for it per window: it sets the window property
 * RENKU_HOST_PROP_HOSTED (any non-zero value) on a top-level window before showing it. When
 * the window is first shown, the driver sets RENKU_HOST_PROP_TEAM on it to the team id of the
 * process; the program hands that and the window handle (the cookie) to the host. A window
 * without the property is not affected at all.
 *
 * Messages are BMessages. The host sends them to the process's BApplication,
 * BMessenger(RENKU_HOST_SIGNATURE, team); the driver sends them to the host's BMessenger from
 * RENKU_HOST_HELLO. Every message carries the cookie, as int64 RENKU_HOST_FIELD_COOKIE.
 * Coordinates are screen coordinates. Either side may vanish at any time: every send may fail,
 * and the driver never waits for a host. */

#ifndef __WINE_RENKU_HOST_H
#define __WINE_RENKU_HOST_H

#define RENKU_HOST_VERSION      1
#define RENKU_HOST_SIGNATURE    "application/x-vnd.wine"

/* Window property names, as the Win32 side spells them (SetPropW). */
#define RENKU_HOST_PROP_HOSTED  "__wine_renku_hosted"
#define RENKU_HOST_PROP_TEAM    "__wine_renku_team"

/* Field names. */
#define RENKU_HOST_FIELD_COOKIE  "cookie"   /* int64: the window handle */
#define RENKU_HOST_FIELD_VERSION "version"  /* int32 */
#define RENKU_HOST_FIELD_HOST    "host"     /* BMessenger: where the driver sends to */
#define RENKU_HOST_FIELD_AREA    "area"     /* int32 area_id: B_RGB32 pixels, top row first */
#define RENKU_HOST_FIELD_WIDTH   "width"    /* int32 */
#define RENKU_HOST_FIELD_HEIGHT  "height"   /* int32 */
#define RENKU_HOST_FIELD_STRIDE  "stride"   /* int32: bytes per row */
#define RENKU_HOST_FIELD_SERIAL  "serial"   /* int32: which surface a message is about */
#define RENKU_HOST_FIELD_X       "x"        /* int32 */
#define RENKU_HOST_FIELD_Y       "y"        /* int32 */
#define RENKU_HOST_FIELD_RIGHT   "right"    /* int32, exclusive */
#define RENKU_HOST_FIELD_BOTTOM  "bottom"   /* int32, exclusive */
#define RENKU_HOST_FIELD_BUTTON  "button"   /* int32: 0 left, 1 right, 2 middle, 3 and 4 extra */
#define RENKU_HOST_FIELD_DOWN    "down"     /* bool */
#define RENKU_HOST_FIELD_KEY     "key"      /* int32: a raw key code, the "key" of B_KEY_DOWN */
#define RENKU_HOST_FIELD_WHEEL_X "wheel_x"  /* float: notches, positive is right */
#define RENKU_HOST_FIELD_WHEEL_Y "wheel_y"  /* float: notches, positive is down */
#define RENKU_HOST_FIELD_FOCUS   "focus"    /* bool */

/* Host -> driver. */
enum
{
    /* version, host: start showing the window. The driver answers with RENKU_HOST_SURFACE
     * once it has drawn, or RENKU_HOST_REFUSED. A second hello replaces the first host. */
    RENKU_HOST_HELLO        = 'rhHi',
    /* The host stops showing the window; the driver keeps drawing until a new hello. */
    RENKU_HOST_BYE          = 'rhBy',
    /* serial: the host has copied what the last RENKU_HOST_SURFACE or RENKU_HOST_DIRTY said;
     * the driver sends the next change only after this. */
    RENKU_HOST_ACK          = 'rhAk',
    /* x, y: where the window's top-left pixel is on screen now. */
    RENKU_HOST_MOVED        = 'rhMv',
    RENKU_HOST_MOUSE_MOVE   = 'rhMm',  /* x, y */
    RENKU_HOST_MOUSE_BUTTON = 'rhMb',  /* x, y, button, down */
    RENKU_HOST_MOUSE_WHEEL  = 'rhMw',  /* wheel_x, wheel_y */
    RENKU_HOST_KEY          = 'rhKy',  /* key, down */
    /* focus: the host's view has the keyboard focus in an active window, or no longer has. */
    RENKU_HOST_FOCUS        = 'rhFc',
};

/* Driver -> host. */
enum
{
    /* serial, area, width, height, stride, x, y: the window's pixels are now in this area,
     * with their top-left pixel at (x, y) in the window. Clone it, copy all of it, ack. An
     * older area is not written to any more. */
    RENKU_HOST_SURFACE      = 'rhSf',
    /* serial, x, y, right, bottom: this rectangle of the area changed (area coordinates). */
    RENKU_HOST_DIRTY        = 'rhDy',
    /* The window is gone. */
    RENKU_HOST_GONE         = 'rhGn',
    /* version: the hello's version is not this driver's. */
    RENKU_HOST_REFUSED      = 'rhNo',
};

#endif /* __WINE_RENKU_HOST_H */
