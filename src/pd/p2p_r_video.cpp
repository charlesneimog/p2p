#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"

#include <m_pd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#ifdef P2P_VIDEO
#include "Gem/Image.h"
#include "Gem/State.h"
extern "C" {
#include <libavutil/pixdesc.h>
}
#endif

struct P2PRVideo {
    t_object m_Object;
    std::string *m_SessionId;
    std::string *m_Username;
    std::shared_ptr<P2PSession> *m_Session;
    t_clock *m_AttachClock;
    t_outlet *m_GemOutlet;
    t_outlet *m_InfoOutlet;
    bool m_Registered;
    bool m_MissingReported;
    bool m_AmbiguityReported;
#ifdef P2P_VIDEO
    pixBlock *m_Pixels;
    uint64_t m_Serial;
#endif

    static t_class *&GetClass() {
        // The host retains this registration for the lifetime of the external.
        static t_class *host_class = nullptr;
        return host_class;
    }
};

#ifdef P2P_VIDEO
static void P2PRVideoOutputInfo(P2PRVideo *object, int width, int height, const char *codec,
                                const char *pixel_format) {
    t_atom resolution[2];
    SETFLOAT(resolution, width);
    SETFLOAT(resolution + 1, height);
    outlet_anything(object->m_InfoOutlet, gensym("resolution"), 2, resolution);

    t_atom value;
    SETSYMBOL(&value, gensym(codec ? codec : "unknown"));
    outlet_anything(object->m_InfoOutlet, gensym("codec"), 1, &value);
    SETSYMBOL(&value, gensym(pixel_format ? pixel_format : "unknown"));
    outlet_anything(object->m_InfoOutlet, gensym("pixel_format"), 1, &value);
}
#endif

// ─────────────────────────────────────
static void P2PRVideoDetach(P2PRVideo *object) {
    std::shared_ptr<P2PSession> session = std::atomic_load(object->m_Session);
    if (session && object->m_Registered) {
        session->UnregisterVideoReceiver();
    }
    object->m_Registered = false;
    std::atomic_store(object->m_Session, std::shared_ptr<P2PSession>());
}

// ─────────────────────────────────────
static void P2PRVideoPoll(P2PRVideo *object) {
    if (!object->m_SessionId->empty() && !object->m_Username->empty()) {
        std::shared_ptr<P2PSession> current = std::atomic_load(object->m_Session);
        std::shared_ptr<P2PSession> found = P2PSessionRegistry::Find(*object->m_SessionId);
        if (found != current) {
            P2PRVideoDetach(object);
            if (found) {
                found->RegisterVideoReceiver();
                object->m_Registered = true;
                std::atomic_store(object->m_Session, found);
            }
        }
        if (found) {
            object->m_MissingReported = false;
            const P2PPeerResolution resolution = found->ResolvePeer(*object->m_Username);
            if (resolution.m_Ambiguous && !object->m_AmbiguityReported) {
                object->m_AmbiguityReported = true;
                pd_error(object, "[p2p.r.video] duplicate username is ambiguous: '%s'",
                         object->m_Username->c_str());
            } else if (!resolution.m_Ambiguous) {
                object->m_AmbiguityReported = false;
            }
        } else if (!object->m_MissingReported) {
            object->m_MissingReported = true;
            pd_error(object, "[p2p.r.video] no active [p2p.config] for session '%s'; waiting",
                     object->m_SessionId->c_str());
        }
    }
    clock_delay(object->m_AttachClock, 100);
}

// ─────────────────────────────────────
static void P2PRVideoGemState(P2PRVideo *object, t_symbol *, int argc, t_atom *argv) {
#ifndef P2P_VIDEO
    outlet_anything(object->m_GemOutlet, gensym("gem_state"), argc, argv);
#else
    if (argc != 2 || argv[0].a_type != A_POINTER || argv[1].a_type != A_POINTER) {
        pd_error(object, "[p2p.r.video] expected 2 GEM state pointers");
        return;
    }
    auto *state = reinterpret_cast<GemState *>(argv[1].a_w.w_gpointer);
    if (!state) {
        outlet_anything(object->m_GemOutlet, gensym("gem_state"), argc, argv);
        return;
    }
    std::shared_ptr<P2PSession> session = std::atomic_load(object->m_Session);
    P2PPeerResolution resolution =
        session ? session->ResolvePeer(*object->m_Username) : P2PPeerResolution{};
    std::shared_ptr<P2PPeer> peer =
        resolution.m_Ambiguous ? std::shared_ptr<P2PPeer>() : resolution.m_Peer;
    pixBlock *previous = nullptr;
    bool replaced = false;
    int width = 0;
    int height = 0;
    std::string codec;
    std::string pixel_format;
    if (peer && peer->m_Active) {
        std::lock_guard<std::mutex> lock(peer->m_VideoMutex);
        if (peer->m_VideoSerial && peer->m_RgbaFrame && peer->m_RgbaFrame->width > 0) {
            width = peer->m_RgbaFrame->width;
            height = peer->m_RgbaFrame->height;
            codec = peer->m_VideoCodec && peer->m_VideoCodec->name ? peer->m_VideoCodec->name
                                                                   : "unknown";
            const char *format_name =
                peer->m_VideoFrame
                    ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(peer->m_VideoFrame->format))
                    : nullptr;
            pixel_format = format_name ? format_name : "unknown";
            object->m_Pixels->image.xsize = width;
            object->m_Pixels->image.ysize = height;
            object->m_Pixels->image.setFormat(GEM_RGBA);
            unsigned char *destination = object->m_Pixels->image.reallocate();
            if (destination && !peer->m_RgbaPixels.empty()) {
                memcpy(destination, peer->m_RgbaPixels.data(), peer->m_RgbaPixels.size());
                object->m_Pixels->image.upsidedown = true;
                object->m_Pixels->newimage = object->m_Serial != peer->m_VideoSerial;
                object->m_Serial = peer->m_VideoSerial;
                state->get(GemState::_PIX, previous);
                state->set(GemState::_PIX, object->m_Pixels);
                replaced = true;
            }
        }
    }
    if (replaced) {
        P2PRVideoOutputInfo(object, width, height, codec.c_str(), pixel_format.c_str());
    }
    outlet_anything(object->m_GemOutlet, gensym("gem_state"), argc, argv);
    if (replaced) {
        state->set(GemState::_PIX, previous);
    }
#endif
}

// ─────────────────────────────────────
static void *P2PRVideoNew(t_symbol *, int argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PRVideo *>(pd_new(P2PRVideo::GetClass()));
    object->m_SessionId = new std::string();
    object->m_Username = new std::string();
    object->m_Session = new std::shared_ptr<P2PSession>();
    object->m_Registered = false;
    object->m_MissingReported = false;
    object->m_AmbiguityReported = false;
    object->m_GemOutlet = outlet_new(&object->m_Object, gensym("gem_state"));
    object->m_InfoOutlet = outlet_new(&object->m_Object, &s_anything);
    object->m_AttachClock = clock_new(object, reinterpret_cast<t_method>(P2PRVideoPoll));
#ifdef P2P_VIDEO
    object->m_Pixels = new pixBlock();
    object->m_Serial = 0;
#else
    pd_error(object, "[p2p.r.video] video support was not compiled");
#endif
    if (argc < 2 || argv[0].a_type != A_SYMBOL || argv[1].a_type != A_SYMBOL ||
        !atom_getsymbol(argv)->s_name[0] || !atom_getsymbol(argv + 1)->s_name[0]) {
        pd_error(object, "[p2p.r.video] expected session ID and username");
    } else {
        *object->m_SessionId = atom_getsymbol(argv)->s_name;
        *object->m_Username = atom_getsymbol(argv + 1)->s_name;
    }
    clock_delay(object->m_AttachClock, 0);
    return object;
}

// ─────────────────────────────────────
static void P2PRVideoFree(P2PRVideo *object) {
    clock_unset(object->m_AttachClock);
    clock_free(object->m_AttachClock);
    P2PRVideoDetach(object);
#ifdef P2P_VIDEO
    delete object->m_Pixels;
#endif
    delete object->m_Session;
    delete object->m_Username;
    delete object->m_SessionId;
}

// ─────────────────────────────────────
extern "C" void setup_p2p0x2er0x2evideo() {
    t_class *host_class = class_new(
        gensym("p2p.r.video"), reinterpret_cast<t_newmethod>(P2PRVideoNew),
        reinterpret_cast<t_method>(P2PRVideoFree), sizeof(P2PRVideo), CLASS_DEFAULT, A_GIMME, 0);
    class_addmethod(host_class, reinterpret_cast<t_method>(P2PRVideoGemState), gensym("gem_state"),
                    A_GIMME, 0);
    P2PRVideo::GetClass() = host_class;
}
