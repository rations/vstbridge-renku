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

// See `editor-host-haiku.h` for why the editor is shown this way, and why the
// `BView` is defined in this file rather than in that header.
//
// Everything in `HostedEditorViewImpl` runs on the host window's looper
// thread, with the window locked: the looper holds that lock while it
// dispatches to the hooks and filters below, and `HostedEditorView` takes it
// itself for the rest. The driver's messages arrive on the same looper, as
// they are addressed to the view.
//
// Nothing here ever waits on the Wine host. Input is sent with a zero timeout,
// so a Wine host that has stopped reading loses input rather than stalling the
// host's window, and a Wine host that has died just makes every send fail.

#include "editor-host-haiku.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

#include <Bitmap.h>
#include <Message.h>
#include <MessageFilter.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <OS.h>
#include <Region.h>
#include <View.h>
#include <Window.h>

#include "../include/renku/renku_host.h"

namespace {

/**
 * How long the hello and the goodbye may wait for room in the driver's port,
 * in microseconds. These are sent once per editor, so they can afford to wait
 * a little where input cannot.
 */
constexpr bigtime_t handshake_timeout = 100000;

/**
 * The largest width or height the driver gives a window's pixels. Anything
 * larger in a message is not from a driver that works.
 */
constexpr int32 max_side = 16384;

/**
 * The `what` code of the message that has the view check whether the Wine
 * host is still there. Only ever sent to the view itself, so it only has to be
 * distinct from the protocol's and the BeAPI's own codes.
 */
constexpr uint32 check_driver_message = 'vbCk';

/**
 * How often the view checks that the Wine host is still there, in
 * microseconds. A Wine host that dies sends no goodbye, and the view would
 * otherwise only notice on its next send, showing the editor's last frame
 * until then.
 */
constexpr bigtime_t check_driver_interval = 1000000;

/**
 * Haiku's mouse buttons, in the order the protocol numbers them.
 */
constexpr uint32 mouse_buttons[] = {
    B_PRIMARY_MOUSE_BUTTON, B_SECONDARY_MOUSE_BUTTON, B_TERTIARY_MOUSE_BUTTON,
    B_MOUSE_BUTTON(4), B_MOUSE_BUTTON(5)};

}  // namespace

/**
 * The view the editor is shown in.
 */
class HostedEditorViewImpl : public BView {
   public:
    HostedEditorViewImpl(BRect frame, Logger& logger, const EditorHost& host)
        // `B_WILL_DRAW` because we paint every pixel ourselves. The view colour
        // is transparent so app_server does not erase to a background colour
        // first, which would show as a flicker on every update.
        : BView(frame,
                "vstbridge editor",
                B_FOLLOW_ALL_SIDES,
                B_WILL_DRAW | B_FRAME_EVENTS),
          logger_(logger),
          driver_(RENKU_HOST_SIGNATURE, host.team),
          cookie_(host.cookie),
          key_filter_(new KeyFilter(*this)) {
        SetViewColor(B_TRANSPARENT_COLOR);
        SetLowColor(0, 0, 0);

        // Handler filters run before `BWindow::DispatchMessage()`, which is
        // where the window takes keys for itself: its shortcuts, and Escape
        // closing a window with `B_CLOSE_ON_ESCAPE`. The view owns the filter
        // and deletes it with itself.
        AddFilter(key_filter_);
    }

    ~HostedEditorViewImpl() override { drop_surface(); }

    void AttachedToWindow() override {
        BView::AttachedToWindow();

        // The window's moves only reach its views as a change of their screen
        // position, which is not a hook, so they are picked up from the
        // window's own message
        window_filter_ = new WindowMovedFilter(*this);
        Window()->AddCommonFilter(window_filter_);

        if (!driver_.IsValid()) {
            logger_.log(
                "Editor view: the Wine host's graphics driver cannot be "
                "reached, so the editor will not be shown");
            return;
        }

        // A `BMessenger` for a view is only valid once it has a looper, so the
        // hello cannot be sent any earlier than this
        BMessage hello(RENKU_HOST_HELLO);
        hello.AddInt32(RENKU_HOST_FIELD_VERSION, RENKU_HOST_VERSION);
        hello.AddMessenger(RENKU_HOST_FIELD_HOST, BMessenger(this));
        connected_ = true;
        if (!send(&hello, handshake_timeout)) {
            connected_ = false;
            logger_.log(
                "Editor view: could not reach the Wine host's graphics "
                "driver, so the editor will not be shown");
            return;
        }

        BMessage check(check_driver_message);
        check_runner_ = std::make_unique<BMessageRunner>(
            BMessenger(this), &check, check_driver_interval);
        if (check_runner_->InitCheck() != B_OK) {
            check_runner_.reset();
        }

        report_origin();
        update_focus();
    }

    void DetachedFromWindow() override {
        // The runner targets this view, so it must not outlive the attachment
        check_runner_.reset();

        if (window_filter_) {
            Window()->RemoveCommonFilter(window_filter_);
            delete window_filter_;
            window_filter_ = nullptr;
        }

        // The driver keeps the editor window drawing, and hands it to whoever
        // says hello next. A goodbye that does not arrive costs nothing: the
        // driver's messages to a view that is gone are dropped by the looper.
        if (connected_) {
            BMessage bye(RENKU_HOST_BYE);
            static_cast<void>(send(&bye, handshake_timeout));
            connected_ = false;
        }

        BView::DetachedFromWindow();
    }

    void MessageReceived(BMessage* message) override {
        switch (message->what) {
            case RENKU_HOST_SURFACE:
                if (from_driver(message)) {
                    on_surface(message);
                }
                return;
            case RENKU_HOST_DIRTY:
                if (from_driver(message)) {
                    on_dirty(message);
                }
                return;
            case RENKU_HOST_GONE:
                if (from_driver(message)) {
                    // The Wine host closed the window, or never had it
                    disconnect();
                }
                return;
            case check_driver_message:
                if (connected_ && !driver_.IsValid()) {
                    disconnect();
                    logger_.log(
                        "Editor view: the Wine host has gone away, so the "
                        "editor is no longer shown");
                }
                return;
            case RENKU_HOST_REFUSED:
                if (from_driver(message)) {
                    connected_ = false;
                    logger_.log(
                        "Editor view: the Wine host's graphics driver speaks "
                        "protocol version " +
                        std::to_string(
                            message->GetInt32(RENKU_HOST_FIELD_VERSION, -1)) +
                        " and this bridge version " +
                        std::to_string(RENKU_HOST_VERSION) +
                        ", so the editor cannot be shown. Install matching "
                        "versions of Wine and vstbridge.");
                }
                return;
            case B_MOUSE_WHEEL_CHANGED: {
                BMessage wheel(RENKU_HOST_MOUSE_WHEEL);
                wheel.AddFloat(RENKU_HOST_FIELD_WHEEL_X,
                               message->GetFloat("be:wheel_delta_x", 0.0f));
                wheel.AddFloat(RENKU_HOST_FIELD_WHEEL_Y,
                               message->GetFloat("be:wheel_delta_y", 0.0f));
                send(&wheel);
                return;
            }
        }

        BView::MessageReceived(message);
    }

    void Draw(BRect update_rect) override {
        BRegion rest(update_rect);

        if (bitmap_) {
            const BRect shown = bitmap_->Bounds().OffsetByCopy(origin_);
            const BRect overlap = update_rect & shown;
            if (overlap.IsValid()) {
                DrawBitmapAsync(bitmap_.get(),
                                overlap.OffsetByCopy(-origin_.x, -origin_.y),
                                overlap);
                rest.Exclude(overlap);
            }
        }

        // Before the first frame, after the editor is gone, and wherever the
        // view is larger than the editor
        if (rest.CountRects() > 0) {
            FillRegion(&rest, B_SOLID_LOW);
        }
    }

    void FrameMoved(BPoint /*new_position*/) override { report_origin(); }

    void MouseDown(BPoint where) override {
        // Keep the moves and the release coming while the button is held
        // outside the view, as a Win32 program's `SetCapture()` expects.
        // app_server ends this on release.
        SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);

        // Plugins that accept typed input need the keyboard to come here
        if (!IsFocus()) {
            MakeFocus(true);
        }

        update_buttons(where);
    }

    void MouseUp(BPoint where) override { update_buttons(where); }

    void MouseMoved(BPoint where,
                    uint32 /*transit*/,
                    const BMessage* /*drag*/) override {
        // The view may have been moved along with its parent, which has no
        // hook of its own here
        report_origin();

        const BPoint screen = ConvertToScreen(where);
        BMessage move(RENKU_HOST_MOUSE_MOVE);
        move.AddInt32(RENKU_HOST_FIELD_X,
                      static_cast<int32>(std::floor(screen.x)));
        move.AddInt32(RENKU_HOST_FIELD_Y,
                      static_cast<int32>(std::floor(screen.y)));
        send(&move);
    }

    void MakeFocus(bool focus) override {
        BView::MakeFocus(focus);
        update_focus();
    }

    void WindowActivated(bool active) override {
        BView::WindowActivated(active);
        update_focus();
    }

    /**
     * Pass a key on to the driver. Called by the key filter.
     *
     * @return Whether the key went to the driver. When it did not, the window
     *   handles it as usual, so the host's own shortcuts keep working while no
     *   editor is shown.
     */
    bool forward_key(const BMessage* message) {
        int32 key = 0;
        if (!connected_ || message->FindInt32("key", &key) != B_OK) {
            return false;
        }

        // Modifier keys arrive as `B_UNMAPPED_KEY_DOWN` and `B_UNMAPPED_KEY_UP`
        // like any other key without a character, and held keys repeat as more
        // `B_KEY_DOWN`s, which is exactly what the driver wants
        BMessage forwarded(RENKU_HOST_KEY);
        forwarded.AddInt32(RENKU_HOST_FIELD_KEY, key);
        forwarded.AddBool(RENKU_HOST_FIELD_DOWN,
                          message->what == B_KEY_DOWN ||
                              message->what == B_UNMAPPED_KEY_DOWN);
        send(&forwarded);

        return true;
    }

    /**
     * The window has been moved so that its top left is at `window_origin` on
     * screen. Called by the window filter, before the window has necessarily
     * updated its own frame.
     */
    void window_moved(BPoint window_origin) {
        // `ConvertToScreen()` and `Frame()` both go by the window's frame as
        // the window last knew it, so their difference is where this view sits
        // in the window either way
        const BPoint in_window =
            ConvertToScreen(B_ORIGIN) - Window()->Frame().LeftTop();
        report_origin(window_origin + in_window);
    }

   private:
    /**
     * Sends every key the view gets to `forward_key()`. Only sees the keys
     * meant for this view, which is the focus view for as long as it gets
     * them.
     */
    class KeyFilter : public BMessageFilter {
       public:
        explicit KeyFilter(HostedEditorViewImpl& view)
            : BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE), view_(view) {}

        filter_result Filter(BMessage* message,
                             BHandler** /*target*/) override {
            switch (message->what) {
                case B_KEY_DOWN:
                case B_KEY_UP:
                case B_UNMAPPED_KEY_DOWN:
                case B_UNMAPPED_KEY_UP:
                    return view_.forward_key(message) ? B_SKIP_MESSAGE
                                                      : B_DISPATCH_MESSAGE;
                default:
                    return B_DISPATCH_MESSAGE;
            }
        }

       private:
        HostedEditorViewImpl& view_;
    };

    /**
     * Tells the view about the window's moves. Installed on the window for as
     * long as the view is attached to it.
     */
    class WindowMovedFilter : public BMessageFilter {
       public:
        explicit WindowMovedFilter(HostedEditorViewImpl& view)
            : BMessageFilter(B_WINDOW_MOVED), view_(view) {}

        filter_result Filter(BMessage* message,
                             BHandler** /*target*/) override {
            BPoint where;
            if (message->FindPoint("where", &where) == B_OK) {
                view_.window_moved(where);
            }

            return B_DISPATCH_MESSAGE;
        }

       private:
        HostedEditorViewImpl& view_;
    };

    /**
     * Send a message to the driver, with the cookie that names the editor
     * window. Does nothing once the driver has said the window is gone, or
     * after the goodbye.
     *
     * @return Whether the message was delivered.
     */
    bool send(BMessage* message, bigtime_t timeout = 0) {
        if (!connected_) {
            return false;
        }

        message->AddInt64(RENKU_HOST_FIELD_COOKIE, static_cast<int64>(cookie_));
        const status_t result = driver_.SendMessage(
            message, static_cast<BHandler*>(nullptr), timeout);
        if (result == B_BAD_PORT_ID) {
            // The Wine host has exited or crashed. Its editor is gone with it.
            disconnect();
            logger_.log(
                "Editor view: the Wine host has gone away, so the editor is "
                "no longer shown");
        }

        return result == B_OK;
    }

    /**
     * Stop talking to the driver and show nothing any more.
     */
    void disconnect() {
        connected_ = false;
        drop_surface();
        Invalidate();
    }

    /**
     * Whether a message names the editor window this view shows. Anyone can
     * send to the view, so the driver's messages are told apart by the cookie.
     */
    bool from_driver(const BMessage* message) const {
        int64 cookie = 0;
        return message->FindInt64(RENKU_HOST_FIELD_COOKIE, &cookie) == B_OK &&
               static_cast<uint64_t>(cookie) == cookie_;
    }

    /**
     * Tell the driver that what it said about this area has been copied, so it
     * can send the next change.
     */
    void ack(int32 serial) {
        BMessage message(RENKU_HOST_ACK);
        message.AddInt32(RENKU_HOST_FIELD_SERIAL, serial);
        send(&message);
    }

    /**
     * The editor window's pixels now live in a new area. Map it, copy all of
     * it, and acknowledge it -- also when it cannot be shown, so the driver is
     * not left waiting.
     */
    void on_surface(const BMessage* message) {
        const int32 serial = message->GetInt32(RENKU_HOST_FIELD_SERIAL, -1);
        const area_id area = message->GetInt32(RENKU_HOST_FIELD_AREA, -1);
        const int32 width = message->GetInt32(RENKU_HOST_FIELD_WIDTH, 0);
        const int32 height = message->GetInt32(RENKU_HOST_FIELD_HEIGHT, 0);
        const int32 stride = message->GetInt32(RENKU_HOST_FIELD_STRIDE, 0);
        const int32 x = message->GetInt32(RENKU_HOST_FIELD_X, 0);
        const int32 y = message->GetInt32(RENKU_HOST_FIELD_Y, 0);

        drop_surface();
        serial_ = serial;

        // From another program, so all of this is checked before anything is
        // mapped or copied
        if (width <= 0 || height <= 0 || width > max_side ||
            height > max_side || stride / 4 < width || x < -max_side ||
            x > max_side || y < -max_side || y > max_side) {
            log_once(
                "the graphics driver described the editor's pixels "
                "wrongly (" +
                std::to_string(width) + "x" + std::to_string(height) +
                ", stride " + std::to_string(stride) + ")");
            ack(serial);
            Invalidate();
            return;
        }

        void* address = nullptr;
        clone_ = clone_area("vstbridge editor", &address, B_ANY_ADDRESS,
                            B_READ_AREA, area);
        area_info info{};
        if (clone_ < B_OK || get_area_info(clone_, &info) != B_OK ||
            info.size < static_cast<size_t>(stride) * height) {
            log_once("could not map the editor's pixels");
            drop_surface();
            ack(serial);
            Invalidate();
            return;
        }

        // `BRect` is inclusive on both edges, hence the -1s
        auto bitmap =
            std::make_unique<BBitmap>(BRect(0, 0, static_cast<float>(width) - 1,
                                            static_cast<float>(height) - 1),
                                      B_RGB32);
        if (bitmap->InitCheck() != B_OK || !bitmap->Bits() ||
            bitmap->BytesPerRow() / 4 < width) {
            log_once("could not allocate a " + std::to_string(width) + "x" +
                     std::to_string(height) + " bitmap for the editor");
            drop_surface();
            ack(serial);
            Invalidate();
            return;
        }

        bitmap_ = std::move(bitmap);
        pixels_ = static_cast<const uint8_t*>(address);
        width_ = width;
        height_ = height;
        stride_ = stride;
        origin_ = BPoint(static_cast<float>(x), static_cast<float>(y));

        copy_rows(0, 0, width_, height_);
        Invalidate();
        ack(serial);
    }

    /**
     * A rectangle of the current area has changed: copy it and show it.
     */
    void on_dirty(const BMessage* message) {
        const int32 serial = message->GetInt32(RENKU_HOST_FIELD_SERIAL, -1);

        // A change to an older area is already in the newer one
        if (serial == serial_ && bitmap_) {
            const int32 left =
                std::max(message->GetInt32(RENKU_HOST_FIELD_X, 0), int32{0});
            const int32 top =
                std::max(message->GetInt32(RENKU_HOST_FIELD_Y, 0), int32{0});
            const int32 right =
                std::min(message->GetInt32(RENKU_HOST_FIELD_RIGHT, 0), width_);
            const int32 bottom = std::min(
                message->GetInt32(RENKU_HOST_FIELD_BOTTOM, 0), height_);

            if (left < right && top < bottom) {
                copy_rows(left, top, right, bottom);
                Invalidate(BRect(origin_.x + left, origin_.y + top,
                                 origin_.x + right - 1,
                                 origin_.y + bottom - 1));
            }
        }

        ack(serial);
    }

    /**
     * Copy a rectangle of the area into `bitmap_`, in area coordinates,
     * already clipped to both.
     */
    void copy_rows(int32 left, int32 top, int32 right, int32 bottom) {
        auto* const destination = static_cast<uint8_t*>(bitmap_->Bits());
        const size_t destination_stride =
            static_cast<size_t>(bitmap_->BytesPerRow());
        const size_t row_bytes = static_cast<size_t>(right - left) * 4;

        for (int32 y = top; y < bottom; y++) {
            std::memcpy(destination + (y * destination_stride) + (left * 4),
                        pixels_ + (static_cast<size_t>(y) * stride_) +
                            (static_cast<size_t>(left) * 4),
                        row_bytes);
        }
    }

    /**
     * Unmap the area and drop the bitmap. The view shows black until the next
     * surface.
     */
    void drop_surface() {
        bitmap_.reset();
        if (clone_ >= B_OK) {
            delete_area(clone_);
        }
        clone_ = -1;
        pixels_ = nullptr;
        width_ = 0;
        height_ = 0;
        stride_ = 0;
    }

    /**
     * Send the buttons that changed. `B_MOUSE_DOWN` and `B_MOUSE_UP` carry the
     * buttons held after the change, but not which one changed.
     */
    void update_buttons(BPoint where) {
        // A popup the plugin opens on this click is placed by where the driver
        // thinks the editor is
        report_origin();

        const BMessage* const current = Window()->CurrentMessage();
        const auto buttons =
            static_cast<uint32>(current ? current->GetInt32("buttons", 0) : 0);
        const uint32 changed = buttons ^ buttons_;
        const BPoint screen = ConvertToScreen(where);

        for (size_t i = 0; i < std::size(mouse_buttons); i++) {
            if (!(changed & mouse_buttons[i])) {
                continue;
            }

            BMessage button(RENKU_HOST_MOUSE_BUTTON);
            button.AddInt32(RENKU_HOST_FIELD_X,
                            static_cast<int32>(std::floor(screen.x)));
            button.AddInt32(RENKU_HOST_FIELD_Y,
                            static_cast<int32>(std::floor(screen.y)));
            button.AddInt32(RENKU_HOST_FIELD_BUTTON, static_cast<int32>(i));
            button.AddBool(RENKU_HOST_FIELD_DOWN,
                           (buttons & mouse_buttons[i]) != 0);
            send(&button);
        }

        buttons_ = buttons;
    }

    /**
     * Tell the driver where the editor window is on screen, if that changed.
     */
    void report_origin() { report_origin(ConvertToScreen(B_ORIGIN)); }

    void report_origin(BPoint screen) {
        const auto x = static_cast<int32>(std::floor(screen.x));
        const auto y = static_cast<int32>(std::floor(screen.y));
        if (reported_origin_ && x == reported_x_ && y == reported_y_) {
            return;
        }

        BMessage moved(RENKU_HOST_MOVED);
        moved.AddInt32(RENKU_HOST_FIELD_X, x);
        moved.AddInt32(RENKU_HOST_FIELD_Y, y);
        if (send(&moved)) {
            reported_origin_ = true;
            reported_x_ = x;
            reported_y_ = y;
        }
    }

    /**
     * Tell the driver whether the editor has the keyboard, if that changed: it
     * does while this view is the focus view of the active window.
     */
    void update_focus() {
        const bool focus = IsFocus() && Window() && Window()->IsActive();
        if (focus == focused_) {
            return;
        }

        BMessage message(RENKU_HOST_FOCUS);
        message.AddBool(RENKU_HOST_FIELD_FOCUS, focus);
        if (send(&message)) {
            focused_ = focus;
        }
    }

    /**
     * Log a problem with the driver's messages, but only the first one, so a
     * broken driver cannot flood the log at the frame rate.
     */
    void log_once(const std::string& what) {
        if (!logged_problem_) {
            logger_.log("Editor view: " + what +
                        ", so the editor will not be shown");
            logged_problem_ = true;
        }
    }

    Logger& logger_;

    /**
     * The Wine host's `BApplication`, where the graphics driver listens.
     */
    const BMessenger driver_;

    /**
     * The editor window's handle, which names it in every message.
     */
    const uint64_t cookie_;

    /**
     * Whether the driver is being talked to: from the hello until the goodbye,
     * or until the driver says the window is gone or the Wine host turns out
     * to have exited.
     */
    bool connected_ = false;

    /**
     * Owned by the view, which deletes its filters with itself.
     */
    KeyFilter* key_filter_;

    /**
     * Owned here, and only installed while the view is attached.
     */
    WindowMovedFilter* window_filter_ = nullptr;

    /**
     * Sends `check_driver_message` while the view is attached.
     */
    std::unique_ptr<BMessageRunner> check_runner_;

    /**
     * The clone of the driver's current area, the pixels in it, and how they
     * are laid out. `pixels_` is null when nothing is mapped.
     */
    area_id clone_ = -1;
    const uint8_t* pixels_ = nullptr;
    int32 width_ = 0;
    int32 height_ = 0;
    int32 stride_ = 0;

    /**
     * Which area the driver's messages are about.
     */
    int32 serial_ = -1;

    /**
     * Where the area's top-left pixel goes in the view.
     */
    BPoint origin_;

    /**
     * The editor's pixels as they are shown. The same size as the area; null
     * before the first surface and after the editor is gone.
     */
    std::unique_ptr<BBitmap> bitmap_;

    /**
     * The mouse buttons the driver was last told were held.
     */
    uint32 buttons_ = 0;

    /**
     * What the driver was last told about the editor's position and focus.
     */
    bool reported_origin_ = false;
    int32 reported_x_ = 0;
    int32 reported_y_ = 0;
    bool focused_ = false;

    /**
     * See `log_once()`.
     */
    bool logged_problem_ = false;
};

HostedEditorView::HostedEditorView(Logger& logger) : logger_(logger) {}

HostedEditorView::~HostedEditorView() {
    detach();
}

bool HostedEditorView::attach(void* parent, const EditorHost& host) {
    detach();

    auto* const parent_view = static_cast<BView*>(parent);
    if (!parent_view) {
        return false;
    }

    BWindow* const window = parent_view->Window();
    if (!window) {
        return false;
    }

    // This runs on whichever thread the host called `IPlugView::attached()`
    // or `effEditOpen` from, which need not be the looper, so the window is
    // locked by hand. The parent's size can only be read under the lock.
    if (!window->Lock()) {
        return false;
    }

    impl_ = std::make_unique<HostedEditorViewImpl>(parent_view->Bounds(),
                                                   logger_, host);
    parent_view->AddChild(impl_.get());
    window->Unlock();

    return true;
}

void HostedEditorView::detach() {
    if (!impl_) {
        return;
    }

    if (BWindow* const window = impl_->Window()) {
        if (!window->Lock()) {
            // Deleting a view that still belongs to a window calls
            // `debugger()`, which would take the host down with it. A window
            // that cannot be locked is being torn down, so the view goes with
            // it; leaking it is the lesser evil.
            logger_.log(
                "Editor view: could not lock the host's window to remove the "
                "editor from it");
            static_cast<void>(impl_.release());
            return;
        }

        impl_->RemoveSelf();
        window->Unlock();
    }

    // Not in a window any more, so it can be deleted as is
    impl_.reset();
}
