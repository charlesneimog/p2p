#include "PdFrontends.hpp"

#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"

#include <m_pd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#ifdef P2P_GEM_VIDEO
#include "Gem/Image.h"
#include "Gem/State.h"
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
    t_clock *attach_clock;
    t_outlet *gem_outlet;
    t_outlet *info_outlet;
    bool registered;
    bool missing_reported;
    bool ambiguity_reported;
#ifdef P2P_GEM_VIDEO
    pixBlock *pixels;
    uint64_t serial;
#endif
};

#ifdef P2P_GEM_VIDEO
static void p2p_r_video_output_info(P2PRVideo *object, int width, int height,
                                    const char *codec, const char *pixel_format) {
    t_atom resolution[2];
    SETFLOAT(resolution, width);
    SETFLOAT(resolution + 1, height);
    outlet_anything(object->info_outlet, gensym("resolution"), 2, resolution);

    t_atom value;
    SETSYMBOL(&value, gensym(codec ? codec : "unknown"));
    outlet_anything(object->info_outlet, gensym("codec"), 1, &value);
    SETSYMBOL(&value, gensym(pixel_format ? pixel_format : "unknown"));
    outlet_anything(object->info_outlet, gensym("pixel_format"), 1, &value);
}
#endif

// ─────────────────────────────────────
static void p2p_r_video_detach(P2PRVideo *object) {
    auto session = std::atomic_load(object->session);
    if (session && object->registered) {
        session->unregisterVideoReceiver();
    }
    object->registered = false;
    std::atomic_store(object->session, std::shared_ptr<P2PSession>());
}

// ─────────────────────────────────────
static void p2p_r_video_poll(P2PRVideo *object) {
    if (!object->session_id->empty() && !object->username->empty()) {
        auto current = std::atomic_load(object->session);
        auto found = P2PSessionRegistry::find(*object->session_id);
        if (found != current) {
            p2p_r_video_detach(object);
            if (found) {
                found->registerVideoReceiver();
                object->registered = true;
                std::atomic_store(object->session, found);
            }
        }
        if (found) {
            object->missing_reported = false;
            const auto resolution = found->resolvePeer(*object->username);
            if (resolution.ambiguous && !object->ambiguity_reported) {
                object->ambiguity_reported = true;
                pd_error(object, "[p2p.r.video] duplicate username is ambiguous: '%s'",
                         object->username->c_str());
            } else if (!resolution.ambiguous) {
                object->ambiguity_reported = false;
            }
        } else if (!object->missing_reported) {
            object->missing_reported = true;
            pd_error(object, "[p2p.r.video] no active [p2p.config] for session '%s'; waiting",
                     object->session_id->c_str());
        }
    }
    clock_delay(object->attach_clock, 100);
}

// ─────────────────────────────────────
static void p2p_r_video_gem_state(P2PRVideo *object, t_symbol *, int argc, t_atom *argv) {
#ifndef P2P_GEM_VIDEO
    outlet_anything(object->gem_outlet, gensym("gem_state"), argc, argv);
#else
    if (argc != 2 || argv[0].a_type != A_POINTER || argv[1].a_type != A_POINTER) {
        pd_error(object, "[p2p.r.video] expected 2 GEM state pointers");
        return;
    }
    auto *state = reinterpret_cast<GemState *>(argv[1].a_w.w_gpointer);
    if (!state) {
        outlet_anything(object->gem_outlet, gensym("gem_state"), argc, argv);
        return;
    }
    auto session = std::atomic_load(object->session);
    auto resolution = session ? session->resolvePeer(*object->username) : P2PPeerResolution{};
    auto peer = resolution.ambiguous ? std::shared_ptr<P2PPeer>() : resolution.peer;
    pixBlock *previous = nullptr;
    bool replaced = false;
    int width = 0;
    int height = 0;
    std::string codec;
    std::string pixel_format;
    if (peer && peer->active) {
        std::lock_guard<std::mutex> lock(peer->video_mutex);
        if (peer->video_serial && peer->rgba_frame && peer->rgba_frame->width > 0) {
            width = peer->rgba_frame->width;
            height = peer->rgba_frame->height;
            codec =
                peer->video_codec && peer->video_codec->name ? peer->video_codec->name : "unknown";
            const char *format_name =
                peer->video_frame
                    ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(peer->video_frame->format))
                    : nullptr;
            pixel_format = format_name ? format_name : "unknown";
            object->pixels->image.xsize = width;
            object->pixels->image.ysize = height;
            object->pixels->image.setFormat(GEM_RGBA);
            unsigned char *destination = object->pixels->image.reallocate();
            if (destination && !peer->rgba_pixels.empty()) {
                memcpy(destination, peer->rgba_pixels.data(), peer->rgba_pixels.size());
                object->pixels->image.upsidedown = true;
                object->pixels->newimage = object->serial != peer->video_serial;
                object->serial = peer->video_serial;
                state->get(GemState::_PIX, previous);
                state->set(GemState::_PIX, object->pixels);
                replaced = true;
            }
        }
    }
    if (replaced) {
        p2p_r_video_output_info(object, width, height, codec.c_str(), pixel_format.c_str());
    }
    outlet_anything(object->gem_outlet, gensym("gem_state"), argc, argv);
    if (replaced) {
        state->set(GemState::_PIX, previous);
    }
#endif
}

// ─────────────────────────────────────
static void *p2p_r_video_new(t_symbol *, int argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PRVideo *>(pd_new(p2p_r_video_class));
    object->session_id = new std::string();
    object->username = new std::string();
    object->session = new std::shared_ptr<P2PSession>();
    object->registered = false;
    object->missing_reported = false;
    object->ambiguity_reported = false;
    object->gem_outlet = outlet_new(&object->object, gensym("gem_state"));
    object->info_outlet = outlet_new(&object->object, &s_anything);
    object->attach_clock = clock_new(object, reinterpret_cast<t_method>(p2p_r_video_poll));
#ifdef P2P_GEM_VIDEO
    object->pixels = new pixBlock();
    object->serial = 0;
#else
    pd_error(object, "[p2p.r.video] video support was not compiled");
#endif
    if (argc < 2 || argv[0].a_type != A_SYMBOL || argv[1].a_type != A_SYMBOL ||
        !atom_getsymbol(argv)->s_name[0] || !atom_getsymbol(argv + 1)->s_name[0]) {
        pd_error(object, "[p2p.r.video] expected session ID and username");
    } else {
        *object->session_id = atom_getsymbol(argv)->s_name;
        *object->username = atom_getsymbol(argv + 1)->s_name;
    }
    clock_delay(object->attach_clock, 0);
    return object;
}

// ─────────────────────────────────────
static void p2p_r_video_free(P2PRVideo *object) {
    clock_unset(object->attach_clock);
    clock_free(object->attach_clock);
    p2p_r_video_detach(object);
#ifdef P2P_GEM_VIDEO
    delete object->pixels;
#endif
    delete object->session;
    delete object->username;
    delete object->session_id;
}

// ─────────────────────────────────────
void p2p_r_video_setup() {
    p2p_r_video_class = class_new(gensym("p2p.r.video"), reinterpret_cast<t_newmethod>(p2p_r_video_new),
                                  reinterpret_cast<t_method>(p2p_r_video_free), sizeof(P2PRVideo),
                                  CLASS_DEFAULT, A_GIMME, 0);
    class_addmethod(p2p_r_video_class, reinterpret_cast<t_method>(p2p_r_video_gem_state),
                    gensym("gem_state"), A_GIMME, 0);
}
