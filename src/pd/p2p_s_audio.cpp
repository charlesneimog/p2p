#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"

#include <m_pd.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// ─────────────────────────────────────
struct P2PSAudio {
    t_object m_Object;
    t_sample m_Signal;
    std::string *m_SessionId;
    std::shared_ptr<P2PSession> *m_Session;
    std::vector<std::shared_ptr<P2PSession>> *m_RetiredSessions;
    std::atomic<P2PSession *> *m_RealtimeSession;
    t_clock *m_AttachClock;
    bool m_Claimed;
    bool m_MissingReported;
    bool m_DuplicateReported;

    static t_class *&GetClass() {
        // The host retains this registration for the lifetime of the external.
        static t_class *host_class = nullptr;
        return host_class;
    }
};

// ─────────────────────────────────────
static void P2PSAudioDetach(P2PSAudio *object) {
    std::shared_ptr<P2PSession> session = std::atomic_load(object->m_Session);
    if (session && object->m_Claimed) {
        session->ReleaseAudioSender(object);
    }
    object->m_Claimed = false;
    object->m_RealtimeSession->store(nullptr, std::memory_order_release);
    if (session) {
        object->m_RetiredSessions->push_back(session);
    }
    std::atomic_store(object->m_Session, std::shared_ptr<P2PSession>());
}

// ─────────────────────────────────────
static void P2PSAudioPoll(P2PSAudio *object) {
    if (!object->m_SessionId->empty()) {
        std::shared_ptr<P2PSession> current = std::atomic_load(object->m_Session);
        std::shared_ptr<P2PSession> found = P2PSessionRegistry::Find(*object->m_SessionId);
        if (found != current) {
            P2PSAudioDetach(object);
            current.reset();
        }
        if (found && !object->m_Claimed) {
            object->m_Claimed = found->ClaimAudioSender(object);
            if (object->m_Claimed) {
                std::atomic_store(object->m_Session, found);
                object->m_RealtimeSession->store(found.get(), std::memory_order_release);
            }
            if (!object->m_Claimed && !object->m_DuplicateReported) {
                object->m_DuplicateReported = true;
                pd_error(object, "[p2p.s.audio~] another sender already exists for this session");
            }
        }
        if (found) {
            object->m_MissingReported = false;
        } else if (!object->m_MissingReported) {
            object->m_MissingReported = true;
            pd_error(object,
                     "[p2p.s.audio~] no active [p2p.config] for session '%s'; "
                     "waiting",
                     object->m_SessionId->c_str());
        }
    }
    clock_delay(object->m_AttachClock, 100);
}

// ─────────────────────────────────────
static t_int *P2PSAudioPerform(t_int *w) {
    auto *object = reinterpret_cast<P2PSAudio *>(w[1]);
    auto *input = reinterpret_cast<t_sample *>(w[2]);
    const int count = static_cast<int>(w[3]);
    const int channels = static_cast<int>(w[4]);
    P2PSession *session = object->m_RealtimeSession->load(std::memory_order_acquire);
    if (session && session->Available()) {
#if PD_FLOATSIZE == 32
        session->PushOutgoingAudio(input, count, channels);
#else
#error "Not Supported"
#endif
    }
    return w + 5;
}

// ─────────────────────────────────────
static void P2PSAudioDsp(P2PSAudio *object, t_signal **signals) {
    const int channels = signals[0]->s_nchans == 2 ? 2 : 1;
    dsp_add(P2PSAudioPerform, 4, object, signals[0]->s_vec, signals[0]->s_n, channels);
}

// ─────────────────────────────────────
static void *P2PSAudioNew(t_symbol *, int argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PSAudio *>(pd_new(P2PSAudio::GetClass()));
    object->m_Signal = 0;
    object->m_SessionId = new std::string();
    object->m_Session = new std::shared_ptr<P2PSession>();
    object->m_RetiredSessions = new std::vector<std::shared_ptr<P2PSession>>();
    object->m_RealtimeSession = new std::atomic<P2PSession *>(nullptr);
    object->m_Claimed = false;
    object->m_MissingReported = false;
    object->m_DuplicateReported = false;
    object->m_AttachClock = clock_new(object, reinterpret_cast<t_method>(P2PSAudioPoll));
    if (argc < 1 || argv[0].a_type != A_SYMBOL || !atom_getsymbol(argv)->s_name[0]) {
        pd_error(object, "[p2p.s.audio~] missing session ID");
    } else {
        *object->m_SessionId = atom_getsymbol(argv)->s_name;
    }
    clock_delay(object->m_AttachClock, 0);
    return object;
}

// ─────────────────────────────────────
static void P2PSAudioFree(P2PSAudio *object) {
    clock_unset(object->m_AttachClock);
    clock_free(object->m_AttachClock);
    P2PSAudioDetach(object);
    delete object->m_RealtimeSession;
    delete object->m_RetiredSessions;
    delete object->m_Session;
    delete object->m_SessionId;
}

// ─────────────────────────────────────
extern "C" void setup_p2p0x2es0x2eaudio_tilde() {
    t_class *host_class =
        class_new(gensym("p2p.s.audio~"), reinterpret_cast<t_newmethod>(P2PSAudioNew),
                  reinterpret_cast<t_method>(P2PSAudioFree), sizeof(P2PSAudio), CLASS_MULTICHANNEL,
                  A_GIMME, 0);

    CLASS_MAINSIGNALIN(host_class, P2PSAudio, m_Signal);
    class_addmethod(host_class, reinterpret_cast<t_method>(P2PSAudioDsp), gensym("dsp"), A_CANT, 0);
    P2PSAudio::GetClass() = host_class;
}
