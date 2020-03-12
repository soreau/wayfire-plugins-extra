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

/*
 * To set a workspace name, use the following option format:
 *
 * [workspace-names]
 * output_1_workspace_3 = Foo
 *
 * This will show Foo when switching to workspace 3 on output 1.
 * The numbering for outputs and workspaces start with 1, not 0.
 */

#include <map>
#include <math.h>
#include <cairo.h>
#include <wayfire/util.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/workspace-stream.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>

#define WIDGET_PADDING 20

std::map<wlr_output*, int> output_nums;

class workspace
{
    public:
    int x, y;
    int width, height;
    std::string name;
    wf::point_t ws;
    wf::framebuffer_t texture;
    cairo_t *cr;
    cairo_surface_t *cairo_surface;
    cairo_text_extents_t text_extents;
};

class wayfire_workspace_names_screen : public wf::plugin_interface_t
{
    wf::wl_timer timer;
    bool hook_set = false;
    bool timed_out = false;
    std::vector<std::vector<workspace>> workspaces;
    wf::option_wrapper_t<std::string> font{"workspace-names/font"};
    wf::option_wrapper_t<std::string> position{"workspace-names/position"};
    wf::option_wrapper_t<int> display_duration{"workspace-names/display_duration"};
    wf::option_wrapper_t<wf::color_t> text_color{"workspace-names/text_color"};
    wf::option_wrapper_t<wf::color_t> background_color{"workspace-names/background_color"};
    wf::animation::simple_animation_t alpha_fade{display_duration};

    public:
    void init() override
    {
        grab_interface->name = "workspace-names";
        grab_interface->capabilities = 0;

        alpha_fade.set(0, 0);
        timed_out = false;

        auto wsize = output->workspace->get_workspace_grid_size();
        workspaces.resize(wsize.width);
        for (int i = 0; i < wsize.width; i++)
        {
            workspaces[i].resize(wsize.height);
            for (int j = 0; j < wsize.height; j++)
            {
                workspaces[i][j].cr = nullptr;
                workspaces[i][j].ws = {i, j};
            }
        }

        wf::get_core().output_layout->connect_signal("output-added", &output_added);
        output->connect_signal("reserved-workarea", &workarea_changed);
        output->connect_signal("viewport-changed", &viewport_changed);
        font.set_callback(option_changed);
        position.set_callback(option_changed);
        background_color.set_callback(option_changed);
        text_color.set_callback(option_changed);
    }

    wf::signal_connection_t output_added{[this] (wf::signal_data_t *data)
    {
        const auto& wo = static_cast<output_added_signal*>(data)->output;
        auto wsize = output->workspace->get_workspace_grid_size();
        int i = 1, j;

        for (auto& o : wf::get_core().output_layout->get_outputs())
        {
            if (o->handle == wo->handle)
            {
                output_nums[o->handle] = i;
                break;
            }
            i++;
        }

        auto section = wf::get_core().config.get_section(grab_interface->name);
        for (auto option : section->get_registered_options())
        {
            int output_num, ws;
            if (sscanf(option->get_name().c_str(),
                "output_%d_workspace_%d", &output_num, &ws) == 2)
            {
                for (i = 0; i < wsize.width; i++)
                {
                    for (j = 0; j < wsize.height; j++)
                    {
                        if (ws == i + j * wsize.width + 1 &&
                            output_nums[output->handle] == output_num)
                        {
                            workspaces[i][j].name = option->get_value_str();
                            update_texture(workspaces[i][j]);
                            i = wsize.width;
                            break;
                        }
                    }
                }
            }
        }
    }};

    void update_texture(workspace& ws)
    {
        update_texture_position(ws);
        render_workspace_name(ws);
    }

    void update_textures()
    {
        auto wsize = output->workspace->get_workspace_grid_size();
        for (int i = 0; i < wsize.width; i++)
        {
            for (int j = 0; j < wsize.height; j++)
            {
                update_texture(workspaces[i][j]);
            }
        }
        output->render->damage_whole();
    }

    void cairo_recreate(workspace& ws)
    {
        auto og = output->get_relative_geometry();
        auto font_size = og.height * 0.05;
        cairo_t *cr = ws.cr;
        cairo_surface_t *cairo_surface = ws.cairo_surface;

        if (!cr)
        {
            /* Setup dummy context to get initial font size */
            cairo_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 1, 1);
            cr = cairo_create(cairo_surface);
        }

        cairo_select_font_face(cr, std::string(font).c_str(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, font_size);

        char name[128];
        get_workspace_name(ws, name);
        cairo_text_extents(cr, name, &ws.text_extents);

        ws.width = ws.text_extents.width + WIDGET_PADDING * 2;
        ws.height = ws.text_extents.height + WIDGET_PADDING * 2;

        /* Recreate surface based on font size */
        cairo_destroy(cr);
        cairo_surface_destroy(cairo_surface);

        cairo_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, ws.width, ws.height);
        cr = cairo_create(cairo_surface);

        cairo_select_font_face(cr, std::string(font).c_str(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, font_size);

        ws.cr = cr;
        ws.cairo_surface = cairo_surface;
    }

    wf::config::option_base_t::updated_callback_t option_changed = [=] ()
    {
        update_textures();
    };

    void update_texture_position(workspace& ws)
    {
        auto workarea = output->workspace->get_workarea();

        cairo_recreate(ws);

        if ((std::string) position == "top_left")
        {
            ws.x = workarea.x;
            ws.y = workarea.y;
        } else if ((std::string) position == "top_center")
        {
            ws.x = workarea.x + (workarea.width / 2 - ws.width / 2);
            ws.y = workarea.y;
        } else if ((std::string) position == "top_right")
        {
            ws.x = workarea.x + (workarea.width - ws.width);
            ws.y = workarea.y;
        } else if ((std::string) position == "center_left")
        {
            ws.x = workarea.x;
            ws.y = workarea.y + (workarea.height / 2 - ws.height / 2);
        } else if ((std::string) position == "center")
        {
            ws.x = workarea.x + (workarea.width / 2 - ws.width / 2);
            ws.y = workarea.y + (workarea.height / 2 - ws.height / 2);
        } else if ((std::string) position == "center_right")
        {
            ws.x = workarea.x + (workarea.width - ws.width);
            ws.y = workarea.y + (workarea.height / 2 - ws.height / 2);
        } else if ((std::string) position == "bottom_left")
        {
            ws.x = workarea.x;
            ws.y = workarea.y + (workarea.height - ws.height);
        } else if ((std::string) position == "bottom_center")
        {
            ws.x = workarea.x + (workarea.width / 2 - ws.width / 2);
            ws.y = workarea.y + (workarea.height - ws.height);
        } else if ((std::string) position == "bottom_right")
        {
            ws.x = workarea.x + (workarea.width - ws.width);
            ws.y = workarea.y + (workarea.height - ws.height);
        } else
        {
            ws.x = workarea.x;
            ws.y = workarea.y;
        }
    }

    wf::signal_connection_t workarea_changed{[this] (wf::signal_data_t *data)
    {
        update_textures();
    }};

    void cairo_clear(cairo_t *cr)
    {
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_paint(cr);
    }

    /* GLESv2 doesn't support GL_BGRA */
    void cairo_set_source_rgba_swizzle(cairo_t *cr, double r, double g, double b, double a)
    {
        cairo_set_source_rgba(cr, b, g, r, a);
    }

    void get_workspace_name(workspace& ws, char *name)
    {
        int num;
        auto wsize = output->workspace->get_workspace_grid_size();

        num = ws.ws.x + ws.ws.y * wsize.width + 1;

        if (ws.name.empty())
            sprintf(name, "Workspace %d", num);
        else
            sprintf(name, "%s", ws.name.c_str());
    }

    void render_workspace_name(workspace& ws)
    {
        double xc = ws.width / 2;
        double yc = ws.height / 2;
        int x2, y2;
        char name[128];
        double radius = 30;
        cairo_t *cr = ws.cr;

        get_workspace_name(ws, name);

        cairo_clear(cr);

        x2 = ws.width;
        y2 = ws.height;

        cairo_set_source_rgba_swizzle(cr,
            wf::color_t(background_color).r,
            wf::color_t(background_color).g,
            wf::color_t(background_color).b,
            wf::color_t(background_color).a);
        cairo_new_path(cr);
        cairo_arc(cr, radius, y2 - radius, radius, M_PI / 2, M_PI);
        cairo_line_to(cr, 0, radius);
        cairo_arc(cr, radius, radius, radius, M_PI, 3 * M_PI / 2);
        cairo_line_to(cr, x2 - radius, 0);
        cairo_arc(cr, x2 - radius, radius, radius, 3 * M_PI / 2, 2 * M_PI);
        cairo_line_to(cr, x2, y2 - radius);
        cairo_arc(cr, x2 - radius, y2 - radius, radius, 0, M_PI / 2);
        cairo_close_path(cr);
        cairo_fill(cr);

        cairo_set_source_rgba_swizzle(cr,
            wf::color_t(text_color).r,
            wf::color_t(text_color).g,
            wf::color_t(text_color).b,
            wf::color_t(text_color).a);
        cairo_text_extents(cr, name, &ws.text_extents);
        cairo_move_to(cr,
            xc - (ws.text_extents.width / 2 + ws.text_extents.x_bearing),
            yc - (ws.text_extents.height / 2 + ws.text_extents.y_bearing));
        cairo_show_text(cr, name);
        cairo_stroke(cr);

        OpenGL::render_begin();
        ws.texture.allocate(ws.width, ws.height);
        GL_CALL(glBindTexture(GL_TEXTURE_2D, ws.texture.tex));
        GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
            ws.width, ws.height,
            0, GL_RGBA, GL_UNSIGNED_BYTE,
            cairo_image_surface_get_data(ws.cairo_surface)));
        OpenGL::render_end();
    }

    wf::effect_hook_t pre_hook = [=] ()
    {
        output->render->damage_whole();
    };

    wf::signal_connection_t viewport_changed{[this] (wf::signal_data_t *data)
    {
        activate();

        if (!alpha_fade.running())
        {
            if (!timer.is_connected())
            {
                alpha_fade.animate(0.0, 1.0);
            }
        }
        else if (timed_out)
        {
            timed_out = false;
            alpha_fade.flip();
        }

        if (timer.is_connected())
        {
            timer.disconnect();
            timer.set_timeout((int) display_duration, timeout);
        }
    }};

    wf::wl_timer::callback_t timeout = [=] ()
    {
        alpha_fade.animate(1.0, 0.0);
        timer.disconnect();
        timed_out = true;
    };

    wf::signal_connection_t overlay_hook{[this] (wf::signal_data_t *data)
    {
        const auto& ws = static_cast<wf::stream_signal_t*>(data);
        auto& workspace = workspaces[ws->ws.x][ws->ws.y];

        OpenGL::render_begin(ws->fb);
        gl_geometry src_geometry = {
            (float) workspace.x,
            (float) workspace.y,
            (float) workspace.x + workspace.width,
            (float) workspace.y + workspace.height};
        OpenGL::render_transformed_texture(workspace.texture.tex,
            src_geometry, {}, ws->fb.get_orthographic_projection(),
            glm::vec4(1, 1, 1, alpha_fade), TEXTURE_TRANSFORM_INVERT_Y);
        OpenGL::render_end();
    }};

    wf::effect_hook_t post_hook = [=] ()
    {
        if (!alpha_fade.running())
        {
            if (timed_out)
            {
                deactivate();
                timed_out = false;
                output->render->damage_whole();
            }
            else if (!timer.is_connected())
            {
                timer.set_timeout((int) display_duration, timeout);
            }
        }
    };

    void activate()
    {
        if (hook_set)
            return;

        output->render->connect_signal("workspace-stream-post", &overlay_hook);
        output->render->add_effect(&post_hook, wf::OUTPUT_EFFECT_POST);
        output->render->add_effect(&pre_hook, wf::OUTPUT_EFFECT_PRE);
        output->render->set_redraw_always();
        hook_set = true;
    }

    void deactivate()
    {
        if (!hook_set)
            return;

        output->render->set_redraw_always(false);
        output->render->rem_effect(&post_hook);
        output->render->rem_effect(&pre_hook);
        overlay_hook.disconnect();
        hook_set = false;
    }

    void fini() override
    {
        deactivate();
        wf::get_core().output_layout->disconnect_signal(&output_added);
        auto wsize = output->workspace->get_workspace_grid_size();
        for (int i = 0; i < wsize.width; i++)
        {
            for (int j = 0; j < wsize.height; j++)
            {
                workspace& ws = workspaces[i][j];
                cairo_surface_destroy(ws.cairo_surface);
                cairo_destroy(ws.cr);
            }
        }
        output->render->damage_whole();
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_workspace_names_screen);
