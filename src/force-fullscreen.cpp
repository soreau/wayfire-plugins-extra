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

#include <map>
#include <unordered_set>
#include <wayfire/core.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/compositor-view.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>

class fullscreen_background : public wf::color_rect_view_t
{
  public:
    wayfire_view view;
    wf::view_2D *transformer;
    wf::geometry_t saved_geometry;

    fullscreen_background(wf::output_t *output, wayfire_view view)
        : wf::color_rect_view_t()
    {
        auto og = output->get_relative_geometry();
        this->view = view;

        set_output(output);
        set_geometry(og);

        this->role = wf::VIEW_ROLE_TOPLEVEL;
        output->workspace->add_view(self(), wf::LAYER_TOP);
    }

    ~fullscreen_background() { }

    bool accepts_input(int32_t sx, int32_t sy) override
    {
        return true;
    }

    void simple_render(const wf::framebuffer_t& fb, int x, int y,
        const wf::region_t& damage) override
    {
        OpenGL::render_begin(fb);
        for (const auto& box : damage)
        {
            fb.logic_scissor(wlr_box_from_pixman_box(box));
            OpenGL::clear({0, 0, 0, 1});
        }
        OpenGL::render_end();
    }

    void update()
    {
        auto output = view->get_output();

        if (!output)
        {
            return;
        }

        auto og = output->get_relative_geometry();

        set_output(output);
        set_geometry(og);
    }
};

class wayfire_force_fullscreen;

std::map<wf::output_t*, wayfire_force_fullscreen*> wayfire_force_fullscreen_instances;

class wayfire_force_fullscreen : public wf::plugin_interface_t
{
    std::string background_name;
    std::map<wayfire_view, fullscreen_background*> backgrounds;
    wf::option_wrapper_t<bool> preserve_aspect{"force-fullscreen/preserve_aspect"};
    wf::option_wrapper_t<wf::keybinding_t> key_toggle_fullscreen{"force-fullscreen/key_toggle_fullscreen"};

  public:
    void init() override
    {
        this->grab_interface->name = "force-fullscreen";
        this->grab_interface->capabilities = wf::CAPABILITY_MANAGE_COMPOSITOR;
        background_name = this->grab_interface->name;

        output->add_key(key_toggle_fullscreen, &on_toggle_fullscreen);
        preserve_aspect.set_callback(preserve_aspect_option_changed);
        wayfire_force_fullscreen_instances[output] = this;
    }

    void setup_transform(wayfire_view view)
    {
        auto og = output->get_relative_geometry();
        auto vg = view->get_wm_geometry();

        double scale_x = (double) og.width / vg.width;
        double scale_y = (double) og.height / vg.height;
        double translation_x = (og.width - vg.width) / 2.0;
        double translation_y = (og.height - vg.height) / 2.0;

        if (preserve_aspect)
        {
            if (scale_x > scale_y)
            {
                scale_x = scale_y;
            }
            else if (scale_x < scale_y)
            {
                scale_y = scale_x;
            }
        }

        backgrounds[view]->transformer->scale_x = scale_x;
        backgrounds[view]->transformer->scale_y = scale_y;
        backgrounds[view]->transformer->translation_x = translation_x;
        backgrounds[view]->transformer->translation_y = translation_y;

        view->damage();
    }

    void update_backgrounds()
    {
        std::map<wayfire_view, fullscreen_background*>::iterator it = backgrounds.begin();
 
	while (it != backgrounds.end())
	{
            setup_transform(it->first);
            it->second->update();
	}
    }

    wf::config::option_base_t::updated_callback_t preserve_aspect_option_changed = [=] ()
    {
        update_backgrounds();
    };

    bool toggle_fullscreen(wayfire_view view)
    {
        if (!output->activate_plugin(grab_interface))
        {
            return false;
        }

        wlr_box saved_geometry;
        auto background = backgrounds.find(view);
        bool fullscreen = background == backgrounds.end() ? true : false;

        wlr_box vg = view->get_output_geometry();

        if (fullscreen)
        {
            saved_geometry = vg;
        }

        view->set_fullscreen(fullscreen);

        if (fullscreen)
        {
            vg = view->get_wm_geometry();
            saved_geometry.width = vg.width;
            saved_geometry.height = vg.height;
        }
        else
        {
            deactivate(view);
            return true;
        }

        activate(view);

        background = backgrounds.find(view);
        if (background != backgrounds.end())
        {
            background->second->saved_geometry = saved_geometry;
        }

        return true;
    }

    wf::key_callback on_toggle_fullscreen = [=] (uint32_t key)
    {
        auto view = output->get_active_view();
        auto background = backgrounds.find(view);

        if (!view)
        {
            return false;
        }

        if (background != backgrounds.end() && view != background->first)
        {
            return false;
        }

        return toggle_fullscreen(view);
    };

    void activate(wayfire_view view)
    {
        view->move(0, 0);
        fullscreen_background *background = new fullscreen_background(output, view);
        background->transformer = new wf::view_2D(view);
        view->add_transformer(std::unique_ptr<wf::view_2D>(background->transformer), background_name);
        output->connect_signal("output-configuration-changed", &output_config_changed);
        wf::get_core().connect_signal("view-move-to-output", &view_output_changed);
        output->connect_signal("view-fullscreen-request", &view_fullscreened);
        view->connect_signal("geometry-changed", &view_geometry_changed);
        output->connect_signal("unmap-view", &view_unmapped);
        output->deactivate_plugin(grab_interface);
        backgrounds[view] = background;
        setup_transform(view);
    }

    void deactivate(wayfire_view view)
    {
        output->deactivate_plugin(grab_interface);
        auto background = backgrounds.find(view);

        if (background == backgrounds.end())
        {
            return;
        }

        if (backgrounds.size() == 1)
        {
            view_geometry_changed.disconnect();
            output_config_changed.disconnect();
            view_output_changed.disconnect();
            view_fullscreened.disconnect();
            view_unmapped.disconnect();
        }
        view->move(background->second->saved_geometry.x, background->second->saved_geometry.y);
        if (view->get_transformer(background_name))
        {
            view->pop_transformer(background_name);
        }
        background->second->close();
        backgrounds.erase(view);
    }

    wf::signal_connection_t output_config_changed{[this] (wf::signal_data_t *data)
    {
        wf::output_configuration_changed_signal *signal =
            static_cast<wf::output_configuration_changed_signal*>(data);

        if (!signal->changed_fields)
        {
            return;
        }

        if (signal->changed_fields & wf::OUTPUT_SOURCE_CHANGE)
        {
            return;
        }

        update_backgrounds();
    }};

    wf::signal_connection_t view_output_changed{[this] (wf::signal_data_t *data)
    {
        auto signal = static_cast<wf::view_move_to_output_signal*> (data);
        auto view = signal->view;
        auto background = backgrounds.find(view);

        if (background == backgrounds.end())
        {
            return;
        }

        background->second->close();
        toggle_fullscreen(view);
        backgrounds.erase(view);

        auto instance = wayfire_force_fullscreen_instances[signal->new_output];
        instance->toggle_fullscreen(view);
    }};

    wf::signal_connection_t view_unmapped{[this] (wf::signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        auto background = backgrounds.find(view);

        if (background == backgrounds.end())
        {
            return;
        }

        toggle_fullscreen(view);
    }};

    wf::signal_connection_t view_fullscreened{[this] (wf::signal_data_t *data)
    {
        auto signal = static_cast<view_fullscreen_signal*> (data);
        auto view = signal->view;
        auto background = backgrounds.find(view);

        if (background == backgrounds.end())
        {
            return;
        }

        if (signal->state || signal->carried_out)
        {
            return;
        }

        background->second->close();
        toggle_fullscreen(view);
        backgrounds.erase(view);

        signal->carried_out = true;
    }};

    wf::signal_connection_t view_geometry_changed{[this] (wf::signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        auto background = backgrounds.find(view);

        if (background == backgrounds.end())
        {
            return;
        }

        view->resize(background->second->saved_geometry.width, background->second->saved_geometry.height);
        setup_transform(background->second->view);
    }};

    void fini() override
    {
        output->rem_binding(&on_toggle_fullscreen);
        wayfire_force_fullscreen_instances.erase(output);

        std::map<wayfire_view, fullscreen_background*>::iterator it = backgrounds.begin();
 
	while (it != backgrounds.end())
	{
            it->second->close();
	}
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_force_fullscreen);
