#include "MaxFrontends.hpp"
#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"

#include <ext.h>
#include <ext_obex.h>
#include <jit.common.h>
#include <z_dsp.h>

#include <atomic>
#include <memory>
#include <string>

#ifdef P2P_VIDEO
extern "C" {
#include <libavutil/pixdesc.h>
}
#endif

static t_class *p2p_r_video_class = nullptr;

struct P2PRVideo {
    t_object object;
    std::string *session_id;
    std::string *username;
    std::shared_ptr<P2PSession> *session;
    t_clock *poll_clock;
    void *matrix_outlet;
    void *info_outlet;
    void *matrix;
    t_symbol *matrix_name;
    uint64_t serial;
    bool registered;
    bool missing_reported;
    bool ambiguity_reported;
};

#ifdef P2P_VIDEO
static void p2p_r_video_output_info(P2PRVideo *x, long width, long height,
                                    const char *codec, const char *pixel_format) {
    t_atom resolution[2];
    atom_setlong(resolution, width);
    atom_setlong(resolution + 1, height);
    outlet_anything(x->info_outlet, gensym("resolution"), 2, resolution);

    t_atom value;
    atom_setsym(&value, gensym(codec ? codec : "unknown"));
    outlet_anything(x->info_outlet, gensym("codec"), 1, &value);
    atom_setsym(&value, gensym(pixel_format ? pixel_format : "unknown"));
    outlet_anything(x->info_outlet, gensym("pixel_format"), 1, &value);
}
#endif

static void p2p_r_video_detach(P2PRVideo *x) {
    auto session = std::atomic_load(x->session);
    if (session && x->registered) {
        session->unregisterVideoReceiver();
    }
    x->registered = false;
    std::atomic_store(x->session, std::shared_ptr<P2PSession>());
}

static void p2p_r_video_attach(P2PRVideo *x) {
    if (x->session_id->empty() || x->registered) {
        return;
    }
    // Acquire rather than merely find: this lets the video object be created
    // before p2p.config. The later config object acquires the same session and
    // its connect message therefore negotiates video from the first offer.
    auto session = P2PSessionRegistry::acquire(*x->session_id, static_cast<int>(sys_getsr()));
    session->registerVideoReceiver();
    x->registered = true;
    std::atomic_store(x->session, std::move(session));
}

static void p2p_r_video_poll(P2PRVideo *x) {
    if (!x->session_id->empty() && !x->username->empty()) {
        auto current = std::atomic_load(x->session);
        auto found = P2PSessionRegistry::find(*x->session_id);
        if (found != current) {
            p2p_r_video_detach(x);
            if (found) {
                found->registerVideoReceiver();
                x->registered = true;
                std::atomic_store(x->session, found);
            }
        }
        if (found) {
            x->missing_reported = false;
            auto resolution = found->resolvePeer(*x->username);
            if (resolution.ambiguous && !x->ambiguity_reported) {
                x->ambiguity_reported = true;
                object_error((t_object *)x, "duplicate username is ambiguous: '%s'",
                             x->username->c_str());
            } else if (!resolution.ambiguous) {
                x->ambiguity_reported = false;
            }
        } else if (!x->missing_reported) {
            x->missing_reported = true;
            object_error((t_object *)x, "no active [p2p.config] for session '%s'; waiting",
                         x->session_id->c_str());
        }
    }
    clock_delay(x->poll_clock, 100);
}

static void p2p_r_video_output_matrix(P2PRVideo *x) {
#ifndef P2P_VIDEO
    object_error((t_object *)x, "video support was not compiled");
#else
    if (!x->matrix) {
        return;
    }
    auto session = std::atomic_load(x->session);
    auto resolution = session ? session->resolvePeer(*x->username) : P2PPeerResolution{};
    auto peer = resolution.ambiguous ? std::shared_ptr<P2PPeer>() : resolution.peer;
    if (!peer || !peer->active) {
        return;
    }

    long width = 0;
    long height = 0;
    std::string codec;
    std::string pixel_format;
    {
        std::lock_guard<std::mutex> lock(peer->video_mutex);
        if (!peer->video_serial || !peer->rgba_frame || peer->rgba_frame->width <= 0 ||
            peer->rgba_pixels.empty()) {
            return;
        }

        width = peer->rgba_frame->width;
        height = peer->rgba_frame->height;
        codec = peer->video_codec && peer->video_codec->name ? peer->video_codec->name : "unknown";
        const char *format_name =
            peer->video_frame
                ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(peer->video_frame->format))
                : nullptr;
        pixel_format = format_name ? format_name : "unknown";

        t_jit_matrix_info info{};
        info.type = _jit_sym_char;
        info.planecount = 4;
        info.dimcount = 2;
        info.dim[0] = width;
        info.dim[1] = height;
        jit_object_method(x->matrix, _jit_sym_setinfo, &info);

        char *data = nullptr;
        jit_object_method(x->matrix, _jit_sym_getdata, &data);
        jit_object_method(x->matrix, _jit_sym_getinfo, &info);
        if (!data) {
            return;
        }

        const size_t source_width = static_cast<size_t>(width);
        const size_t source_row_bytes = source_width * 4;
        for (long row = 0; row < info.dim[1]; ++row) {
            const auto *source =
                peer->rgba_pixels.data() + static_cast<size_t>(row) * source_row_bytes;
            auto *destination = reinterpret_cast<unsigned char *>(
                data + static_cast<size_t>(row) * info.dimstride[1]);
            for (size_t column = 0; column < source_width; ++column) {
                const auto *rgba = source + column * 4;
                auto *argb = destination + column * info.dimstride[0];
                // FFmpeg produces RGBA, but a four-plane Jitter char matrix uses
                // ARGB plane order.
                argb[0] = rgba[3];
                argb[1] = rgba[0];
                argb[2] = rgba[1];
                argb[3] = rgba[2];
            }
        }
        x->serial = peer->video_serial;
    }

    p2p_r_video_output_info(x, width, height, codec.c_str(), pixel_format.c_str());
    t_atom name;
    atom_setsym(&name, x->matrix_name);
    outlet_anything(x->matrix_outlet, gensym("jit_matrix"), 1, &name);
#endif
}

static void *p2p_r_video_new(t_symbol *, long argc, t_atom *argv) {
    auto *x = reinterpret_cast<P2PRVideo *>(object_alloc(p2p_r_video_class));
    x->session_id = new std::string();
    x->username = new std::string();
    x->session = new std::shared_ptr<P2PSession>();
    x->registered = x->missing_reported = x->ambiguity_reported = false;
    x->serial = 0;
    // Max creates outlets from right to left.
    x->info_outlet = outlet_new((t_object *)x, nullptr);
    x->matrix_outlet = outlet_new((t_object *)x, "jit_matrix");
    x->matrix_name = jit_symbol_unique();
    t_jit_matrix_info info{};
    jit_matrix_info_default(&info);
    info.type = _jit_sym_char;
    info.planecount = 4;
    info.dimcount = 2;
    info.dim[0] = 1;
    info.dim[1] = 1;
    x->matrix = jit_object_new(_jit_sym_jit_matrix, &info);
    if (x->matrix) {
        x->matrix = jit_object_register(x->matrix, x->matrix_name);
    } else {
        object_error((t_object *)x, "could not create Jitter output matrix");
    }
    x->poll_clock = clock_new(x, reinterpret_cast<method>(p2p_r_video_poll));
    if (argc < 2 || atom_gettype(argv) != A_SYM || atom_gettype(argv + 1) != A_SYM) {
        object_error((t_object *)x, "expected session ID and username");
    } else {
        *x->session_id = atom_getsym(argv)->s_name;
        *x->username = atom_getsym(argv + 1)->s_name;
        p2p_r_video_attach(x);
    }
    clock_delay(x->poll_clock, 0);
    return x;
}

static void p2p_r_video_free(P2PRVideo *x) {
    clock_unset(x->poll_clock);
    object_free(x->poll_clock);
    p2p_r_video_detach(x);
    if (x->matrix) {
        jit_object_free(x->matrix);
    }
    delete x->session;
    delete x->username;
    delete x->session_id;
}

void p2p_r_video_setup() {
    p2p_r_video_class =
        class_new("p2p.r.video", reinterpret_cast<method>(p2p_r_video_new),
                  reinterpret_cast<method>(p2p_r_video_free), sizeof(P2PRVideo), nullptr, A_GIMME, 0);
    class_addmethod(p2p_r_video_class, reinterpret_cast<method>(p2p_r_video_output_matrix), "bang", 0);
    class_register(CLASS_BOX, p2p_r_video_class);
}

extern "C" C74_EXPORT void ext_main(void *) {
    p2p_r_video_setup();
}
