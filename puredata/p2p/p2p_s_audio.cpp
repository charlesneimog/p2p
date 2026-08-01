#include "PdFrontends.hpp"

#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"

#include <m_pd.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

t_class *p2p_s_audio_class = nullptr;

// ─────────────────────────────────────
struct P2PSAudio {
    t_object object;
    t_sample signal;
    std::string *session_id;
    std::shared_ptr<P2PSession> *session;
    std::vector<std::shared_ptr<P2PSession>> *retired_sessions;
    std::atomic<P2PSession *> *realtime_session;
    t_clock *attach_clock;
    bool claimed;
    bool missing_reported;
    bool duplicate_reported;
};

// ─────────────────────────────────────
static void p2p_s_audio_detach(P2PSAudio *object) {
    auto session = std::atomic_load(object->session);
    if (session && object->claimed) {
        session->releaseAudioSender(object);
    }
    object->claimed = false;
    object->realtime_session->store(nullptr, std::memory_order_release);
    if (session) {
        object->retired_sessions->push_back(session);
    }
    std::atomic_store(object->session, std::shared_ptr<P2PSession>());
}

// ─────────────────────────────────────
static void p2p_s_audio_poll(P2PSAudio *object) {
    if (!object->session_id->empty()) {
        auto current = std::atomic_load(object->session);
        auto found = P2PSessionRegistry::find(*object->session_id);
        if (found != current) {
            p2p_s_audio_detach(object);
            current.reset();
        }
        if (found && !object->claimed) {
            object->claimed = found->claimAudioSender(object);
            if (object->claimed) {
                std::atomic_store(object->session, found);
                object->realtime_session->store(found.get(), std::memory_order_release);
            }
            if (!object->claimed && !object->duplicate_reported) {
                object->duplicate_reported = true;
                pd_error(object, "[p2p.s.audio~] another sender already exists for this session");
            }
        }
        if (found) {
            object->missing_reported = false;
        } else if (!object->missing_reported) {
            object->missing_reported = true;
            pd_error(object,
                     "[p2p.s.audio~] no active [p2p.config] for session '%s'; "
                     "waiting",
                     object->session_id->c_str());
        }
    }
    clock_delay(object->attach_clock, 100);
}

// ─────────────────────────────────────
static t_int *p2p_s_audio_perform(t_int *w) {
    auto *object = reinterpret_cast<P2PSAudio *>(w[1]);
    auto *input = reinterpret_cast<t_sample *>(w[2]);
    const int count = static_cast<int>(w[3]);
    auto *session = object->realtime_session->load(std::memory_order_acquire);
    if (session && session->available()) {
#if PD_FLOATSIZE == 32
        session->pushOutgoingAudio(input, count);
#else
#error "Not Supported"
#endif
    }
    return w + 4;
}

// ─────────────────────────────────────
static void p2p_s_audio_dsp(P2PSAudio *object, t_signal **signals) {
    dsp_add(p2p_s_audio_perform, 3, object, signals[0]->s_vec, signals[0]->s_n);
}

// ─────────────────────────────────────
static void *p2p_s_audio_new(t_symbol *, int argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PSAudio *>(pd_new(p2p_s_audio_class));
    object->signal = 0;
    object->session_id = new std::string();
    object->session = new std::shared_ptr<P2PSession>();
    object->retired_sessions = new std::vector<std::shared_ptr<P2PSession>>();
    object->realtime_session = new std::atomic<P2PSession *>(nullptr);
    object->claimed = false;
    object->missing_reported = false;
    object->duplicate_reported = false;
    object->attach_clock = clock_new(object, reinterpret_cast<t_method>(p2p_s_audio_poll));
    if (argc < 1 || argv[0].a_type != A_SYMBOL || !atom_getsymbol(argv)->s_name[0]) {
        pd_error(object, "[p2p.s.audio~] missing session ID");
    } else {
        *object->session_id = atom_getsymbol(argv)->s_name;
    }
    clock_delay(object->attach_clock, 0);
    return object;
}

// ─────────────────────────────────────
static void p2p_s_audio_free(P2PSAudio *object) {
    clock_unset(object->attach_clock);
    clock_free(object->attach_clock);
    p2p_s_audio_detach(object);
    delete object->realtime_session;
    delete object->retired_sessions;
    delete object->session;
    delete object->session_id;
}

// ─────────────────────────────────────
void p2p_s_audio_setup() {
    p2p_s_audio_class = class_new(
        gensym("p2p.s.audio~"), reinterpret_cast<t_newmethod>(p2p_s_audio_new),
        reinterpret_cast<t_method>(p2p_s_audio_free), sizeof(P2PSAudio), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(p2p_s_audio_class, P2PSAudio, signal);
    class_addmethod(p2p_s_audio_class, reinterpret_cast<t_method>(p2p_s_audio_dsp), gensym("dsp"),
                    A_CANT, 0);
}
