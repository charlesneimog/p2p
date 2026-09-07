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

struct P2PRVideo {
    t_object m_Object;
    std::string *m_SessionId;
    std::string *m_Username;
    std::shared_ptr<P2PSession> *m_Session;
    t_clock *m_PollClock;
    void *m_MatrixOutlet;
    void *m_InfoOutlet;
    void *m_Matrix;
    t_symbol *m_MatrixName;
    uint64_t m_Serial;
    bool m_Registered;
    bool m_MissingReported;
    bool m_AmbiguityReported;

    static t_class *&GetClass() {
        // The host retains this registration for the lifetime of the external.
        static t_class *host_class = nullptr;
        return host_class;
    }
};

#ifdef P2P_VIDEO
static void P2PRVideoOutputInfo(P2PRVideo *x, long width, long height, const char *codec,
                                const char *pixel_format) {
    t_atom resolution[2];
    atom_setlong(resolution, width);
    atom_setlong(resolution + 1, height);
    outlet_anything(x->m_InfoOutlet, gensym("resolution"), 2, resolution);

    t_atom value;
    atom_setsym(&value, gensym(codec ? codec : "unknown"));
    outlet_anything(x->m_InfoOutlet, gensym("codec"), 1, &value);
    atom_setsym(&value, gensym(pixel_format ? pixel_format : "unknown"));
    outlet_anything(x->m_InfoOutlet, gensym("pixel_format"), 1, &value);
}
#endif

static void P2PRVideoDetach(P2PRVideo *x) {
    std::shared_ptr<P2PSession> session = std::atomic_load(x->m_Session);
    if (session && x->m_Registered) {
        session->UnregisterVideoReceiver();
    }
    x->m_Registered = false;
    std::atomic_store(x->m_Session, std::shared_ptr<P2PSession>());
}

// ─────────────────────────────────────
static void P2PRVideoAttach(P2PRVideo *x) {
    if (x->m_SessionId->empty() || x->m_Registered) {
        return;
    }
    // Acquire rather than merely find: this lets the video object be created
    // before p2p.config. The later config object acquires the same session and
    // its connect message therefore negotiates video from the first offer.
    std::shared_ptr<P2PSession> session =
        P2PSessionRegistry::Acquire(*x->m_SessionId, static_cast<int>(sys_getsr()));
    session->RegisterVideoReceiver();
    x->m_Registered = true;
    std::atomic_store(x->m_Session, std::move(session));
}

// ─────────────────────────────────────
static void P2PRVideoPoll(P2PRVideo *x) {
    if (!x->m_SessionId->empty() && !x->m_Username->empty()) {
        std::shared_ptr<P2PSession> current = std::atomic_load(x->m_Session);
        std::shared_ptr<P2PSession> found = P2PSessionRegistry::Find(*x->m_SessionId);
        if (found != current) {
            P2PRVideoDetach(x);
            if (found) {
                found->RegisterVideoReceiver();
                x->m_Registered = true;
                std::atomic_store(x->m_Session, found);
            }
        }
        if (found) {
            x->m_MissingReported = false;
            P2PPeerResolution resolution = found->ResolvePeer(*x->m_Username);
            if (resolution.m_Ambiguous && !x->m_AmbiguityReported) {
                x->m_AmbiguityReported = true;
                object_error((t_object *)x, "duplicate username is ambiguous: '%s'",
                             x->m_Username->c_str());
            } else if (!resolution.m_Ambiguous) {
                x->m_AmbiguityReported = false;
            }
        } else if (!x->m_MissingReported) {
            x->m_MissingReported = true;
            object_error((t_object *)x, "no active [p2p.config] for session '%s'; waiting",
                         x->m_SessionId->c_str());
        }
    }
    clock_delay(x->m_PollClock, 100);
}

// ─────────────────────────────────────
static void P2PRVideoOutputMatrix(P2PRVideo *x) {
#ifndef P2P_VIDEO
    object_error((t_object *)x, "video support was not compiled");
#else
    if (!x->m_Matrix) {
        return;
    }
    std::shared_ptr<P2PSession> session = std::atomic_load(x->m_Session);
    P2PPeerResolution resolution =
        session ? session->ResolvePeer(*x->m_Username) : P2PPeerResolution{};
    std::shared_ptr<P2PPeer> peer =
        resolution.m_Ambiguous ? std::shared_ptr<P2PPeer>() : resolution.m_Peer;
    if (!peer || !peer->m_Active) {
        return;
    }

    long width = 0;
    long height = 0;
    std::string codec;
    std::string pixel_format;
    {
        std::lock_guard<std::mutex> lock(peer->m_VideoMutex);
        if (!peer->m_VideoSerial || !peer->m_RgbaFrame || peer->m_RgbaFrame->width <= 0 ||
            peer->m_RgbaPixels.empty()) {
            return;
        }

        width = peer->m_RgbaFrame->width;
        height = peer->m_RgbaFrame->height;
        codec =
            peer->m_VideoCodec && peer->m_VideoCodec->name ? peer->m_VideoCodec->name : "unknown";
        const char *format_name =
            peer->m_VideoFrame
                ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(peer->m_VideoFrame->format))
                : nullptr;
        pixel_format = format_name ? format_name : "unknown";

        t_jit_matrix_info info{};
        info.type = _jit_sym_char;
        info.planecount = 4;
        info.dimcount = 2;
        info.dim[0] = width;
        info.dim[1] = height;
        jit_object_method(x->m_Matrix, _jit_sym_setinfo, &info);

        char *data = nullptr;
        jit_object_method(x->m_Matrix, _jit_sym_getdata, &data);
        jit_object_method(x->m_Matrix, _jit_sym_getinfo, &info);
        if (!data) {
            return;
        }

        const size_t source_width = static_cast<size_t>(width);
        const size_t source_row_bytes = source_width * 4;
        for (long row = 0; row < info.dim[1]; ++row) {
            const unsigned char *source =
                peer->m_RgbaPixels.data() + static_cast<size_t>(row) * source_row_bytes;
            auto *destination = reinterpret_cast<unsigned char *>(data + static_cast<size_t>(row) *
                                                                             info.dimstride[1]);
            for (size_t column = 0; column < source_width; ++column) {
                const unsigned char *rgba = source + column * 4;
                unsigned char *argb = destination + column * info.dimstride[0];
                // FFmpeg produces RGBA, but a four-plane Jitter char matrix uses
                // ARGB plane order.
                argb[0] = rgba[3];
                argb[1] = rgba[0];
                argb[2] = rgba[1];
                argb[3] = rgba[2];
            }
        }
        x->m_Serial = peer->m_VideoSerial;
    }

    P2PRVideoOutputInfo(x, width, height, codec.c_str(), pixel_format.c_str());
    t_atom name;
    atom_setsym(&name, x->m_MatrixName);
    outlet_anything(x->m_MatrixOutlet, gensym("jit_matrix"), 1, &name);
#endif
}

// ─────────────────────────────────────
static void *P2PRVideoNew(t_symbol *, long argc, t_atom *argv) {
    auto *x = reinterpret_cast<P2PRVideo *>(object_alloc(P2PRVideo::GetClass()));
    x->m_SessionId = new std::string();
    x->m_Username = new std::string();
    x->m_Session = new std::shared_ptr<P2PSession>();
    x->m_Registered = x->m_MissingReported = x->m_AmbiguityReported = false;
    x->m_Serial = 0;
    // Max creates outlets from right to left.
    x->m_InfoOutlet = outlet_new((t_object *)x, nullptr);
    x->m_MatrixOutlet = outlet_new((t_object *)x, "jit_matrix");
    x->m_MatrixName = jit_symbol_unique();
    t_jit_matrix_info info{};
    jit_matrix_info_default(&info);
    info.type = _jit_sym_char;
    info.planecount = 4;
    info.dimcount = 2;
    info.dim[0] = 1;
    info.dim[1] = 1;
    x->m_Matrix = jit_object_new(_jit_sym_jit_matrix, &info);
    if (x->m_Matrix) {
        x->m_Matrix = jit_object_register(x->m_Matrix, x->m_MatrixName);
    } else {
        object_error((t_object *)x, "could not create Jitter output matrix");
    }
    x->m_PollClock = clock_new(x, reinterpret_cast<method>(P2PRVideoPoll));
    if (argc < 2 || atom_gettype(argv) != A_SYM || atom_gettype(argv + 1) != A_SYM) {
        object_error((t_object *)x, "expected session ID and username");
    } else {
        *x->m_SessionId = atom_getsym(argv)->s_name;
        *x->m_Username = atom_getsym(argv + 1)->s_name;
        P2PRVideoAttach(x);
    }
    clock_delay(x->m_PollClock, 0);
    return x;
}

// ─────────────────────────────────────
static void P2PRVideoFree(P2PRVideo *x) {
    clock_unset(x->m_PollClock);
    object_free(x->m_PollClock);
    P2PRVideoDetach(x);
    if (x->m_Matrix) {
        jit_object_free(x->m_Matrix);
    }
    delete x->m_Session;
    delete x->m_Username;
    delete x->m_SessionId;
}

// ─────────────────────────────────────
void P2PRVideoSetup() {
    t_class *host_class =
        class_new("p2p.r.video", reinterpret_cast<method>(P2PRVideoNew),
                  reinterpret_cast<method>(P2PRVideoFree), sizeof(P2PRVideo), nullptr, A_GIMME, 0);
    class_addmethod(host_class, reinterpret_cast<method>(P2PRVideoOutputMatrix), "bang", 0);
    class_register(CLASS_BOX, host_class);
    P2PRVideo::GetClass() = host_class;
}

// ─────────────────────────────────────
extern "C" C74_EXPORT void ext_main(void *) {
    P2PRVideoSetup();
}
