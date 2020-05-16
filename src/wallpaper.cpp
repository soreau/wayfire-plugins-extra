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

#include <thread>
#include <unistd.h>
#include <jerror.h>
#include <jpeglib.h>
#include <curl/curl.h>
#include <sys/eventfd.h>
#include <openssl/err.h>
#include <wayfire/util.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/compositor-surface.hpp>
#include <wayfire/compositor-view.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/workspace-stream.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/plugins/common/simple-texture.hpp>

#include <wayfire/util/log.hpp>

#define RETRY_TIMEOUT 1000

class wayfire_wallpaper_screen;

class wallpaper : public wf::color_rect_view_t
{
  public:
    int sig_fd;
    FILE *image_fp;
    char *image_ptr;
    wlr_box geometry;
    size_t image_size;
    int failed_counter = 0;
    wl_event_source *event_source;
    wayfire_wallpaper_screen *wps;
    bool downloaded = false;
    bool download_failed = false;
    std::unique_ptr<std::thread> thread = nullptr;
    std::unique_ptr<wf::simple_texture_t> from, to, tmp = nullptr;

    wallpaper(wayfire_wallpaper_screen *screen);

    void simple_render(const wf::framebuffer_t& fb, int x, int y,
        const wf::region_t& damage) override;

    virtual ~wallpaper();
};

static void clean_up(wallpaper& wp)
{
    wl_event_source_remove(wp.event_source);
    wp.thread->join();
    wp.thread.reset();
    wp.thread = nullptr;
    fclose(wp.image_fp);
    free(wp.image_ptr);
    close(wp.sig_fd);
}

class wayfire_wallpaper_screen : public wf::plugin_interface_t
{
    wf::wl_timer timer;
    bool hook_set = false;
    bool wallpaper_shutdown = false;
    std::vector<std::vector<nonstd::observer_ptr<wallpaper>>> wallpapers;
    wf::option_wrapper_t<bool> cycle{"wallpaper/cycle"};
    wf::option_wrapper_t<int> cycle_time{"wallpaper/cycle_time"};
    wf::option_wrapper_t<int> fade_duration{"wallpaper/fade_duration"};

  public:
    wf::animation::simple_animation_t fade_animation{fade_duration};
    void init() override
    {
        grab_interface->name = "wallpaper";
        grab_interface->capabilities = 0;

        fade_animation.set(0, 0);

        auto wsize = output->workspace->get_workspace_grid_size();
        auto og = output->get_relative_geometry();
        wallpapers.resize(wsize.width);
        for (int x = 0; x < wsize.width; x++)
        {
            wallpapers[x].resize(wsize.height);
            for (int y = 0; y < wsize.height; y++)
            {
                auto view = std::make_unique<wallpaper>(this);

                view->set_output(output);
                view->set_geometry({x * og.width, y * og.height, og.width, og.height});
                view->role = wf::VIEW_ROLE_UNMANAGED;
                output->workspace->add_view(view, wf::LAYER_BACKGROUND);
                wallpapers[x][y] = {view};
                wf::get_core().add_view(std::move(view));
            }
        }

        cycle.set_callback(cycle_changed);
        output->connect_signal("reserved-workarea", &workarea_changed);
        output->connect_signal("output-configuration-changed", &output_config_changed);
        update_textures();
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

        auto wsize = output->workspace->get_workspace_grid_size();
        auto og = output->get_relative_geometry();
        for (int x = 0; x < wsize.width; x++)
        {
            for (int y = 0; y < wsize.height; y++)
            {
                auto wp = wallpapers[x][y];
                wp->set_geometry({x * og.width, y * og.height, og.width, og.height});
            }
        }
        update_textures();
    }};

    wf::config::option_base_t::updated_callback_t cycle_changed = [=] ()
    {
        if (timer.is_connected())
        {
            timer.disconnect();
        }

        if (cycle)
        {
            update_textures();
        }
    };

    static bool texture_from_jpeg_fp(std::FILE *file, wf::simple_texture_t& texture, GLuint target)
    {
        unsigned long data_size;
        unsigned char *rowptr[1];
        unsigned char *jdata;
        struct jpeg_decompress_struct infot;
        struct jpeg_error_mgr err;

        infot.err = jpeg_std_error(&err);
        jpeg_create_decompress(&infot);

        jpeg_stdio_src(&infot, file);
        jpeg_read_header(&infot, TRUE);
        jpeg_start_decompress(&infot);

        data_size = infot.output_width * infot.output_height * 3;

        jdata = new unsigned char[data_size];
        while (infot.output_scanline < infot.output_height)
        {
            rowptr[0] = (unsigned char *)jdata +  3 * infot.output_width * infot.output_scanline;
            jpeg_read_scanlines(&infot, rowptr, 1);
        }

        jpeg_finish_decompress(&infot);

        texture.width = infot.output_width;
        texture.height = infot.output_height;

        OpenGL::render_begin();
        if (texture.tex == (GLuint)-1)
        {
            GL_CALL(glGenTextures(1, &texture.tex));
        }
        GL_CALL(glBindTexture(GL_TEXTURE_2D, texture.tex));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GL_CALL(glTexImage2D(target, 0, GL_RGB,
                infot.output_width, infot.output_height,
                0, GL_RGB, GL_UNSIGNED_BYTE, jdata));
        OpenGL::render_end();

        delete[] jdata;

        return true;
    }

    static size_t write_cb(void *ptr, size_t size, size_t nmemb, void* userdata)
    {
        wallpaper& wp = *((wallpaper *) userdata);
        FILE *stream = wp.image_fp;

        if (wp.wps->wallpaper_shutdown)
        {
            LOGI("Wallpaper shutting down");
            return 0;
        }
        if (!stream)
        {
            LOGE("No stream");
            return 0;
        }

        size_t written = fwrite(ptr, size, nmemb, stream);

        return written;
    }

    static void curl_download(wallpaper *pwp)
    {
        wallpaper& wp = *pwp;
        CURL *ch;
        CURLM *mh;
        CURLMcode mc;
        int numfds;
        int still_running = 0;
        int repeats = 0;
        int chunks = 0;

        auto og = wp.geometry;
        std::string url = "https://picsum.photos/" +
            std::to_string(og.width) + "/" + std::to_string(og.height) +
            "/?random&t=" + std::to_string(rand() % 10000000);

        curl_global_init(CURL_GLOBAL_DEFAULT);

        ch = curl_easy_init();

        curl_easy_setopt(ch, CURLOPT_URL, url.c_str());
        curl_easy_setopt(ch, CURLOPT_WRITEDATA, &wp);
        curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(ch, CURLOPT_FOLLOWLOCATION, 1);
        curl_easy_setopt(ch, CURLOPT_CONNECTTIMEOUT, 1);
        curl_easy_setopt(ch, CURLOPT_NOSIGNAL, 1);
        curl_easy_setopt(ch, CURLOPT_VERBOSE, 1);

        mh = curl_multi_init();
        curl_multi_add_handle(mh, ch);

        do
        {
            LOGI(pwp, " Calling curl_multi_perform()");
            mc = curl_multi_perform(mh, &still_running);
            if (mc != CURLM_OK)
            {
                LOGE("curl_multi_perform() failed, code: ", mc);
                wp.download_failed = true;
                break;
            }

            LOGI(pwp, " Calling curl_multi_wait()");
            mc = curl_multi_wait(mh, NULL, 0, 100, &numfds);
            if (mc != CURLM_OK)
            {
                LOGE("curl_multi_wait() failed, code: ", mc);
                wp.download_failed = true;
                break;
            }

            if (!numfds)
            {
                repeats++;
                if (repeats > 1)
                {
                    LOGI(pwp, " Sleeping for 100000 usecs");
                    usleep(100000);
                }
            }
            else
            {
                repeats = 0;
            }
            LOGI(pwp, " Looping shutting down = ", wp.wps->wallpaper_shutdown ? "true" : "false");
        }
        while (still_running && (!wp.wps->wallpaper_shutdown || chunks++ < 10));

        LOGI(pwp, " Calling curl_multi_remove_handle()");
        curl_multi_remove_handle(mh, ch);
        LOGI(pwp, " Calling curl_multi_cleanup()");
        curl_multi_cleanup(mh);
        LOGI(pwp, " Calling curl_easy_cleanup()");
        curl_easy_cleanup(ch);
        LOGI(pwp, " Calling curl_global_cleanup()");
        curl_global_cleanup();

        write(wp.sig_fd, "12345678", 8);
    }

    static int download_complete(int fd, uint32_t mask, void *data)
    {
        wallpaper& wp = *((wallpaper *) data);
        wayfire_wallpaper_screen& s = *wp.wps;

        if (mask & WL_EVENT_READABLE)
        {
            char c[8];
            read(fd, c, 8);
        }
        else
        {
            LOGE("Event not readable");
            return 0;
        }

        if (!wp.thread)
        {
            LOGE("Thread end handler called without running thread");
            return 0;
        }

        if (wp.wps->wallpaper_shutdown)
        {
            LOGE("Wallpaper shutdown, cleaning up");
            clean_up(wp);
            return 0;
        }

        fflush(wp.image_fp);

        auto og = s.output->get_relative_geometry();
        if (wp.geometry.width != og.width || wp.geometry.height != og.height)
        {
            wp.download_failed = true;
        }

        if (wp.download_failed || !wp.image_size)
        {
            LOGE("Download failed");

            clean_up(wp);

            if (++wp.failed_counter > 3)
            {
                LOGE("Download failed too many times, waiting ", (int) s.cycle_time / 1000, " seconds");

                if (!s.timer.is_connected())
                {
                    s.timer.set_timeout(RETRY_TIMEOUT, s.cycle_timeout);
                }
            }
	    else
	    {
                LOGI("Retrying download");
                s.update_wallpaper(wp);
            }
            wp.download_failed = false;
            return 0;
        }

        wp.downloaded = true;
        wp.failed_counter = 0;

        if (!wp.tmp)
        {
            wp.tmp = std::make_unique<wf::simple_texture_t> ();
        }

        texture_from_jpeg_fp(wp.image_fp, *wp.tmp, GL_TEXTURE_2D);

        LOGI("Downloaded random image from picsum.photos ",
            wp.tmp->width, "x", wp.tmp->height, ", bytes: ", wp.image_size);

        bool all_done = true;
        auto wsize = s.output->workspace->get_workspace_grid_size();
        for (int x = 0; x < wsize.width; x++)
        {
            for (int y = 0; y < wsize.height; y++)
            {
                if (!s.wallpapers[x][y]->downloaded)
                {
                    all_done = false;
                    x = wsize.width;
                    break;
                }
            }
        }

        if (all_done)
        {
            /* All wallpapers finished downloading for each workspace
             * of the output, so swap the new images in place and start
             * the fade animation */
            for (int x = 0; x < wsize.width; x++)
            {
                for (int y = 0; y < wsize.height; y++)
                {
                    auto& wp = *s.wallpapers[x][y];
                    wp.from = std::move(wp.to);
                    wp.to = std::move(wp.tmp);
                    wp.tmp.reset();
                    wp.downloaded = false;
                    wp.download_failed = false;
                }
            }
            s.fade_animation.animate(0.0, 1.0);
            s.activate();
            if (s.cycle)
            {
                s.timer.set_timeout((int) s.cycle_time, s.cycle_timeout);
            }
        }

        clean_up(wp);

        return 0;
    }

    void update_wallpaper(wallpaper& wp)
    {
        if (wp.thread)
        {
            timer.set_timeout(RETRY_TIMEOUT, cycle_timeout);
            return;
        }
        if ((wp.sig_fd = eventfd(0, EFD_CLOEXEC)) == -1)
        {
            LOGE("eventfd() failed: ", std::strerror(errno));
            timer.disconnect();
            return;
        }
        if ((wp.image_fp = open_memstream(&wp.image_ptr, &wp.image_size)) == 0)
        {
            LOGE("open_memstream() failed: ", std::strerror(errno));
            timer.disconnect();
            close(wp.sig_fd);
            return;
        }
        wp.geometry = output->get_relative_geometry();
        wp.event_source = wl_event_loop_add_fd(wf::get_core().ev_loop, wp.sig_fd,
            WL_EVENT_READABLE, download_complete, &wp);
        wp.thread = std::make_unique<std::thread> (curl_download, &wp);
    }

    void update_texture(wallpaper& wp)
    {
        wp.failed_counter = 0;

        if (!wp.downloaded)
        {
            update_wallpaper(wp);
        }
    }

    void update_textures()
    {
        if (timer.is_connected())
        {
            timer.disconnect();
	}
        auto wsize = output->workspace->get_workspace_grid_size();
        for (int x = 0; x < wsize.width; x++)
        {
            for (int y = 0; y < wsize.height; y++)
            {
                update_texture(*wallpapers[x][y]);
            }
        }
        output->render->damage_whole();
    }

    void damage_wallpapers()
    {
        auto wsize = output->workspace->get_workspace_grid_size();
        for (int x = 0; x < wsize.width; x++)
        {
            for (int y = 0; y < wsize.height; y++)
            {
                wallpapers[x][y]->damage();
            }
        }
    }

    wf::signal_connection_t workarea_changed{[this] (wf::signal_data_t *data)
    {
        update_textures();
    }};

    wf::effect_hook_t pre_hook = [=] ()
    {
        damage_wallpapers();
    };

    wf::wl_timer::callback_t cycle_timeout = [=] ()
    {
        update_textures();
    };

    wf::effect_hook_t post_hook = [=] ()
    {
        if (!fade_animation.running())
        {
            auto wsize = output->workspace->get_workspace_grid_size();
            for (int x = 0; x < wsize.width; x++)
            {
                for (int y = 0; y < wsize.height; y++)
                {
                    wallpaper& wp = *wallpapers[x][y];
                    if (wp.from)
                    {
                        wp.from.reset();
                    }
                }
            }
            deactivate();
            output->render->damage_whole();
        }
    };

    void activate()
    {
        if (hook_set)
        {
            return;
        }

        output->render->add_effect(&post_hook, wf::OUTPUT_EFFECT_POST);
        output->render->add_effect(&pre_hook, wf::OUTPUT_EFFECT_PRE);
        damage_wallpapers();
        hook_set = true;
    }

    void deactivate()
    {
        if (!hook_set)
        {
            return;
        }

        output->render->rem_effect(&post_hook);
        output->render->rem_effect(&pre_hook);
        hook_set = false;
    }

    void fini() override
    {
        deactivate();
        timer.disconnect();
        workarea_changed.disconnect();

        LOGI("fini: wallpaper_shutdown = true");
        wallpaper_shutdown = true;

        auto wsize = output->workspace->get_workspace_grid_size();
        for (int x = 0; x < wsize.width; x++)
        {
            for (int y = 0; y < wsize.height; y++)
            {
                auto& wp = *wallpapers[x][y];

                if (wp.thread)
                {
                    LOGI(&wp, " Cleaning up thread ", x, ",", y);
                    clean_up(wp);
                    LOGI(&wp, " Thread joined      ", x, ",", y);
                }

                if (wp.from)
                {
                    wp.from.reset();
                }
                if (wp.to)
                {
                    wp.to.reset();
                }
                if (wp.tmp)
                {
                    wp.tmp.reset();
                }
                wp.close();
            }
        }
        output->render->damage_whole();
        LOGI("fini: complete");
    }
};

wallpaper::wallpaper(wayfire_wallpaper_screen *screen)
    : wf::color_rect_view_t()
{
    this->wps = screen;
}

void wallpaper::simple_render(const wf::framebuffer_t& fb, int x, int y,
    const wf::region_t& damage)
{
    auto og = fb.geometry;

    OpenGL::render_begin(fb);
    for (auto& box : damage)
    {
        fb.logic_scissor(wlr_box_from_pixman_box(box));
        if (wps->fade_animation.running() && from && from->tex != (uint32_t) -1)
        {
            OpenGL::render_texture(wf::texture_t{from->tex},
            fb, og, glm::vec4(1.0),
            OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
        }
        if (to && to->tex != (uint32_t) -1)
        {
            OpenGL::render_texture(wf::texture_t{to->tex},
            fb, og, glm::vec4(1, 1, 1, wps->fade_animation),
            OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
        }
    }
    OpenGL::render_end();
}

wallpaper::~wallpaper()
{
}

DECLARE_WAYFIRE_PLUGIN(wayfire_wallpaper_screen);
