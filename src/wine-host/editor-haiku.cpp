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

// The Haiku counterpart to `editor.cpp`. See `editor-haiku.h` for why there is
// no embedding here and what replaces it.

#include "editor-haiku.h"

#include <chrono>
#include <cstdint>
#include <iostream>

#include "../include/renku/renku_host.h"

using namespace std::literals::chrono_literals;

namespace {

/**
 * The ID we'll use for our idle timer. This only has to be unique within the
 * window.
 */
constexpr size_t idle_timer_id = 1337;

/**
 * Say why the editor cannot be shown when the graphics driver did not take the
 * window into its hosted mode.
 *
 * That happens when the prefix uses another driver than winerenku.drv -- most
 * likely the null driver that vstbridge 0.3 told people to set, which this
 * version no longer wants -- or a winerenku.drv from before the hosted mode.
 */
void log_no_hosted_mode(Logger& logger) {
    logger.log("");
    logger.log("WARNING: Wine's graphics driver did not take the plugin's");
    logger.log("         editor window, so the editor cannot be shown in");
    logger.log("         the host. This needs the RenkuOS driver,");
    logger.log("         winerenku.drv, from Wine 11.21-10 or later. If");
    logger.log("         this prefix was set up for vstbridge 0.3, remove");
    logger.log("         the null driver setting it asked for:");
    logger.log("");
    logger.log("           wine reg delete 'HKCU\\Software\\Wine\\Drivers' \\");
    logger.log("             /v Graphics /f");
    logger.log("");
}

/**
 * The window procedure for our editor windows.
 *
 * The X11 build needs an elaborate one to deal with reparenting, coordinate
 * fixups and focus grabbing. None of that applies here: the graphics driver
 * places the window and delivers its input. This only has to drive the idle
 * timer that VST2 plugins rely on for `effEditIdle`.
 */
LRESULT CALLBACK window_proc(HWND handle,
                             UINT message,
                             WPARAM wParam,
                             LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            // Stash whatever the `Editor` handed us at creation time so
            // `WM_TIMER` can reach it without a global
            const auto* create_params = reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(
                handle, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create_params->lpCreateParams));
        } break;
        case WM_TIMER: {
            if (wParam != idle_timer_id) {
                break;
            }

            // `GWLP_USERDATA` only holds the `Editor` once its constructor has
            // gotten far enough to set it, and the timer cannot fire before it
            // has been created after that point, so this is null only during
            // window creation
            auto* editor = reinterpret_cast<Editor*>(
                GetWindowLongPtr(handle, GWLP_USERDATA));
            if (!editor) {
                break;
            }

            editor->run_idle_timer();

            return 0;
        } break;
    }

    return DefWindowProc(handle, message, wParam, lParam);
}

/**
 * Lazily register the window class the plugin's editor window uses.
 */
ATOM get_window_class() noexcept {
    static ATOM window_class_handle = 0;
    if (!window_class_handle) {
        WNDCLASSEX window_class{};

        window_class.cbSize = sizeof(WNDCLASSEX);
        window_class.style = CS_DBLCLKS;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = GetModuleHandle(nullptr);
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = vstbridge_window_class_name;

        window_class_handle = RegisterClassEx(&window_class);
    }

    return window_class_handle;
}

}  // namespace

DeferredWin32Window::DeferredWin32Window(MainContext& main_context,
                                         HWND window) noexcept
    : handle_(window), main_context_(main_context) {}

DeferredWin32Window::~DeferredWin32Window() noexcept {
    // As in the X11 build, closing is deferred twice over: `WM_CLOSE` is posted
    // rather than calling `DestroyWindow()` directly, because that can hang for
    // a second or two, and the post itself is delayed so a message loop tick
    // landing between `IPlugView::removed()` and `IPlugView::~IPlugView` cannot
    // stall the host. Nothing lingers on screen meanwhile: the window was only
    // ever shown in the native side's view, which is gone by now.
    try {
        std::shared_ptr<asio::steady_timer> destroy_timer =
            std::make_shared<asio::steady_timer>(main_context_.context_);
        destroy_timer->expires_after(1s);

        // The timer keeps itself alive by capturing a copy of its own shared
        // pointer, and cleans itself up once this lambda has run
        destroy_timer->async_wait(
            [destroy_timer, handle = handle_](const std::error_code& error) {
                if (error) {
                    return;
                }

                PostMessage(handle, WM_CLOSE, 0, 0);
            });
    } catch (const std::exception&) {
        // If the timer could not be set up then fall back to closing the window
        // immediately -- a delayed close is an optimization, leaking the window
        // is not acceptable
        PostMessage(handle_, WM_CLOSE, 0, 0);
    }
}

namespace {

bool popup_fix_tracing = false;
int popup_fix_users = 0;
HHOOK popup_fix_hook = nullptr;

LRESULT CALLBACK popup_fix_cbt_proc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_CREATEWND) {
        const HWND window = reinterpret_cast<HWND>(wParam);
        const LONG style = GetWindowLong(window, GWL_STYLE);
        const LONG ex_style = GetWindowLong(window, GWL_EXSTYLE);

        // Only captionless, non-resizable popup windows that are not supposed
        // to show up in the task bar. Regular plugin windows and dialogs are
        // left alone.
        if (!(style & WS_CHILD) && (style & WS_POPUP) &&
            (style & WS_CAPTION) != WS_CAPTION && !(style & WS_THICKFRAME) &&
            (style & WS_SYSMENU) && !(ex_style & WS_EX_APPWINDOW)) {
            SetWindowLong(window, GWL_STYLE, style & ~WS_SYSMENU);

            // This goes to STDERR instead of through a `Logger` since the hook
            // can outlive the editor that installed it. STDERR gets captured
            // and logged by the plugin side.
            if (popup_fix_tracing) {
                std::cerr << "DEBUG: Removed WS_SYSMENU from popup window "
                          << window << std::hex << " (style 0x" << style
                          << ", exstyle 0x" << ex_style << std::dec
                          << ") so Wine won't let a window manager manage it"
                          << std::endl;
            }
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace

PopupStyleFixer::PopupStyleFixer(bool enabled, Logger& logger)
    : enabled_(enabled) {
    if (!enabled_) {
        return;
    }

    // All editors live on the same GUI thread, so a single thread-local hook
    // shared between them is enough
    if (popup_fix_users++ == 0) {
        popup_fix_tracing = logger.editor_tracing_;
        popup_fix_hook = SetWindowsHookEx(WH_CBT, popup_fix_cbt_proc, nullptr,
                                          GetCurrentThreadId());
    }
}

PopupStyleFixer::~PopupStyleFixer() noexcept {
    if (!enabled_ || --popup_fix_users > 0) {
        return;
    }

    if (popup_fix_hook) {
        UnhookWindowsHookEx(popup_fix_hook);
        popup_fix_hook = nullptr;
    }
}

Editor::Editor(MainContext& main_context,
               const Configuration& config,
               Logger& logger,
               const size_t /*parent_window_handle*/,
               std::optional<fu2::unique_function<void()>> timer_proc)
    : logger_(logger),
      // A borderless popup: what the native side shows is exactly the area the
      // plugin draws, with no frame of Wine's around it. The size is set by
      // `resize()` once the plugin has reported one.
      //
      // NOTE: The window procedure reaches this `Editor` through the window's
      //       `GWLP_USERDATA` to service the idle timer, so everything the
      //       timer touches has to outlive the window. `idle_timer_proc_` is
      //       declared after `win32_window_` in the header for that reason --
      //       destruction runs in reverse.
      win32_window_(main_context,
                    CreateWindowEx(0,
                                   reinterpret_cast<LPCSTR>(get_window_class()),
                                   "vstbridge plugin",
                                   WS_POPUP,
                                   0,
                                   0,
                                   256,
                                   256,
                                   nullptr,
                                   nullptr,
                                   GetModuleHandle(nullptr),
                                   nullptr)),
      popup_style_fixer_(config.editor_popup_fix, logger) {
    if (!win32_window_.handle_) {
        logger_.log("Could not create the plugin's editor window");
        return;
    }

    // The graphics driver decides when the window is first shown, so this has
    // to be set before `show()`. Any non-null value opts in.
    if (!SetPropA(win32_window_.handle_, RENKU_HOST_PROP_HOSTED,
                  reinterpret_cast<HANDLE>(1))) {
        logger_.log("Could not mark the editor window for the host");
    }

    // The timer reaches this object through the window, so this has to be set
    // before it is created
    SetWindowLongPtr(win32_window_.handle_, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(this));

    if (timer_proc) {
        idle_timer_proc_ = std::move(*timer_proc);
        timer_.emplace(
            win32_window_.handle_, idle_timer_id,
            static_cast<unsigned int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    config.event_loop_interval())
                    .count()));
    }
}

void Editor::run_idle_timer() {
    if (idle_timer_proc_) {
        idle_timer_proc_();
    }
}

void Editor::resize(uint16_t width, uint16_t height) {
    if (!win32_window_.handle_) {
        return;
    }

    // A borderless popup has no decorations, so the plugin's client size is
    // the window's size
    SetWindowPos(win32_window_.handle_, nullptr, 0, 0, width, height,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
}

void Editor::show() noexcept {
    if (!win32_window_.handle_) {
        return;
    }

    ShowWindow(win32_window_.handle_, SW_SHOWNORMAL);

    // The driver answers the opt-in when the window is first shown, by setting
    // the team the native side has to address
    const auto team = static_cast<int32_t>(reinterpret_cast<INT_PTR>(
        GetPropA(win32_window_.handle_, RENKU_HOST_PROP_TEAM)));
    if (team <= 0) {
        // Another driver, or one without the hosted mode, would put the
        // editor on screen as a stray borderless window of its own instead
        ShowWindow(win32_window_.handle_, SW_HIDE);
        log_no_hosted_mode(logger_);
        return;
    }

    host_ = EditorHost{.team = team,
                       .cookie = static_cast<uint64_t>(
                           reinterpret_cast<uintptr_t>(win32_window_.handle_))};
}

void Editor::handle_x11_events() noexcept {
    // Nothing to pump: there is no X11 connection here, and Win32 messages are
    // dispatched by the regular message loop
}

HWND Editor::win32_handle() const noexcept {
    return win32_window_.handle_;
}

std::optional<EditorHost> Editor::host() const noexcept {
    return host_;
}
