/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Scott Moreau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "wayfire/core.hpp"
#include "wayfire/view.hpp"
#include "wayfire/plugin.hpp"
#include "wayfire/output.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/workspace-manager.hpp"
#include "wayfire/output-layout.hpp"
#include "wayfire/cube/cube-control-signal.hpp"

#include <wayfire/util/log.hpp>

extern "C"
{
#include <wlr/config.h>
#if WLR_HAS_XWAYLAND
#define class class_t
#define static
#include <wlr/xwayland.h>
#undef static
#undef class
#endif
}

struct process
{
    std::string gtk_name;
    pid_t pid;
};

class wayfire_desktop_click : public wf::plugin_interface_t
{
    wf::option_wrapper_t<std::string> command{"desktop-click/command"};
    wf::option_wrapper_t<bool> gtk_app{"desktop-click/gtk_app"};
    wf::wl_idle_call idle_focus_output;
    process proc;

    public:
    void init() override
    {
        grab_interface->name = "desktop-click";
        grab_interface->capabilities = 0;

        output->add_button(
            wf::option_wrapper_t<wf::buttonbinding_t>{"desktop-click/run_command"},
            &run_command);
        output->add_button(
            wf::option_wrapper_t<wf::buttonbinding_t>{"desktop-click/activate_cube"},
            &activate_cube);

        output->connect_signal("map-view", &view_mapped);

        init_process();
    }

    void init_process()
    {
        proc.pid = -1;
    }

    wf::signal_connection_t view_mapped{[this] (wf::signal_data_t *data)
    {
        auto view = get_signaled_view(data);
#if WLR_HAS_XWAYLAND
        wlr_surface *wlr_surface = view->get_wlr_surface();
        bool is_xwayland_surface = wlr_surface_is_xwayland_surface(wlr_surface);
#endif
        /* Get pid for view */
        pid_t view_pid;
        wl_client_get_credentials(view->get_client(), &view_pid, 0, 0);
        LOGI(proc.gtk_name, " ?= ", view->get_app_id());

        if ((proc.pid != view_pid &&
            (proc.gtk_name.empty() || (proc.gtk_name != view->get_app_id())))
#if WLR_HAS_XWAYLAND
            || (is_xwayland_surface &&
            /* For this match to work, the client must set _NET_WM_PID */
            proc.pid != wlr_xwayland_surface_from_wlr_surface(wlr_surface)->pid)
#endif
            )
        {
            return;
        }

        auto cursor = wf::get_core().get_active_output()->get_cursor_position();

        /* Move so the top left corner corresponds to the mouse pointer */
        view->move(cursor.x, cursor.y);

        init_process();
    }};

    wf::button_callback run_command = [=] (uint32_t button, int32_t x, int32_t y)
    {
        if (!output->can_activate_plugin(grab_interface))
            return false;

        unsigned int layer = 0;

        auto view = wf::get_core().get_cursor_focus_view();
        if (view == nullptr)
        {
            goto call;
        }

        layer = output->workspace->get_view_layer(view);
        if (layer != wf::LAYER_BACKGROUND)
        {
            return false;
        }

call:
        std::string cmd = std::string(command);
        if (gtk_app)
        {
            static const char alphanum[] =
                "0123456789"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz";

            std::string random_name;
            for (int i = 0; i < 8; i++)
            {
                random_name += alphanum[rand() % (sizeof(alphanum) - 1)];
            }

            cmd += " --name=" + random_name;
            proc.gtk_name = random_name;
        }
        proc.pid = wf::get_core().run(cmd);
        LOGI("PID: ", proc.pid);

        return true;
    };

    wf::button_callback activate_cube = [=] (uint32_t button, int32_t x, int32_t y)
    {
        if (!output->can_activate_plugin(grab_interface))
            return false;

        unsigned int layer = 0;

        auto view = wf::get_core().get_cursor_focus_view();
        if (view == nullptr)
        {
            goto call;
        }

        layer = output->workspace->get_view_layer(view);
        if (layer != wf::LAYER_BACKGROUND)
        {
            return false;
        }

call:
        cube_grab_signal data;
        data.button = button;
        data.x = x;
        data.y = y;
        output->emit_signal("cube-grab", &data);

        if (data.carried_out)
            return true;

        return false;
    };

    void fini() override
    {
        output->rem_binding(&run_command);
        output->rem_binding(&activate_cube);
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_desktop_click);