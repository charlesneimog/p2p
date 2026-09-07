#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"

#include <m_pd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct P2PRAudio {
    t_object m_Object;
    std::string *m_SessionId;
    std::string *m_Username;
    std::shared_ptr<P2PSession> *m_Session;
    std::shared_ptr<P2PPeer> *m_Peer;
    std::vector<std::shared_ptr<P2PPeer>> *m_RetiredPeers;
    std::atomic<P2PPeer *> *m_RealtimePeer;
    t_clock *m_AttachClock;
    t_outlet *m_SignalOutlet;
    bool m_Claimed;
    bool m_MissingReported;
    bool m_DuplicateConsumerReported;
    bool m_AmbiguityReported;

    static t_class *&GetClass() {
        // The host retains this registration for the lifetime of the external.
        static t_class *host_class = nullptr;
        return host_class;
    }
};

static void P2PRAudioStorePeer(P2PRAudio *object, std::shared_ptr<P2PPeer> peer) {
    std::shared_ptr<P2PPeer> old_peer = std::atomic_load(object->m_Peer);
    if (old_peer == peer) {
        return;
    }
    object->m_RealtimePeer->store(nullptr, std::memory_order_release);
    if (old_peer) {
        object->m_RetiredPeers->push_back(std::move(old_peer));
    }
    std::atomic_store(object->m_Peer, std::move(peer));
    std::shared_ptr<P2PPeer> current = std::atomic_load(object->m_Peer);
    object->m_RealtimePeer->store(current.get(), std::memory_order_release);
}

// ─────────────────────────────────────
static void P2PRAudioDetach(P2PRAudio *object) {
    std::shared_ptr<P2PSession> session = std::atomic_load(object->m_Session);
    if (session && object->m_Claimed) {
        session->ReleaseAudioReceiver(*object->m_Username, object);
    }
    object->m_Claimed = false;
    P2PRAudioStorePeer(object, {});
    std::atomic_store(object->m_Session, std::shared_ptr<P2PSession>());
}

// ─────────────────────────────────────
static void P2PRAudioPoll(P2PRAudio *object) {
    if (!object->m_SessionId->empty() && !object->m_Username->empty()) {
        std::shared_ptr<P2PSession> current = std::atomic_load(object->m_Session);
        std::shared_ptr<P2PSession> found = P2PSessionRegistry::Find(*object->m_SessionId);
        if (found != current) {
            P2PRAudioDetach(object);
            if (found) {
                std::atomic_store(object->m_Session, found);
            }
        }
        if (found && !object->m_Claimed) {
            object->m_Claimed = found->ClaimAudioReceiver(*object->m_Username, object);
            if (!object->m_Claimed && !object->m_DuplicateConsumerReported) {
                object->m_DuplicateConsumerReported = true;
                pd_error(object, "[p2p.r.audio~] another receiver already consumes this user");
            }
        }
        if (!found) {
            P2PRAudioStorePeer(object, {});
            if (!object->m_MissingReported) {
                object->m_MissingReported = true;
                pd_error(object,
                         "[p2p.r.audio~] no active [p2p.config] for session '%s'; "
                         "waiting",
                         object->m_SessionId->c_str());
            }
        } else {
            object->m_MissingReported = false;
            const P2PPeerResolution resolution =
                object->m_Claimed ? found->ResolvePeer(*object->m_Username) : P2PPeerResolution{};
            if (resolution.m_Ambiguous) {
                P2PRAudioStorePeer(object, {});
                if (!object->m_AmbiguityReported) {
                    object->m_AmbiguityReported = true;
                    pd_error(object, "[p2p.r.audio~] duplicate username is ambiguous: '%s'",
                             object->m_Username->c_str());
                }
            } else {
                object->m_AmbiguityReported = false;
                P2PRAudioStorePeer(object, resolution.m_Peer);
            }
        }
    }
    clock_delay(object->m_AttachClock, 50);
}

// ─────────────────────────────────────
static t_int *P2PRAudioPerform(t_int *words) {
    auto *object = reinterpret_cast<P2PRAudio *>(words[1]);
    auto *output = reinterpret_cast<t_sample *>(words[2]);
    const int count = static_cast<int>(words[3]);
    P2PPeer *peer = object->m_RealtimePeer->load(std::memory_order_acquire);
    if (!peer || !peer->m_Active || !peer->m_Connected) {
        memset(output, 0, static_cast<size_t>(count) * sizeof(t_sample));
        return words + 4;
    }
    for (int index = 0; index < count; ++index) {
        float sample = 0;
        peer->PopReceived(sample);
        output[index] = sample;
    }
    return words + 4;
}

// ─────────────────────────────────────
static void P2PRAudioDsp(P2PRAudio *object, t_signal **signals) {
    dsp_add(P2PRAudioPerform, 3, object, signals[0]->s_vec, signals[0]->s_n);
}

// ─────────────────────────────────────
static void *P2PRAudioNew(t_symbol *, int argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PRAudio *>(pd_new(P2PRAudio::GetClass()));
    object->m_SessionId = new std::string();
    object->m_Username = new std::string();
    object->m_Session = new std::shared_ptr<P2PSession>();
    object->m_Peer = new std::shared_ptr<P2PPeer>();
    object->m_RetiredPeers = new std::vector<std::shared_ptr<P2PPeer>>();
    object->m_RealtimePeer = new std::atomic<P2PPeer *>(nullptr);
    object->m_Claimed = false;
    object->m_MissingReported = false;
    object->m_DuplicateConsumerReported = false;
    object->m_AmbiguityReported = false;
    object->m_SignalOutlet = outlet_new(&object->m_Object, &s_signal);
    object->m_AttachClock = clock_new(object, reinterpret_cast<t_method>(P2PRAudioPoll));
    if (argc < 2 || argv[0].a_type != A_SYMBOL || argv[1].a_type != A_SYMBOL ||
        !atom_getsymbol(argv)->s_name[0] || !atom_getsymbol(argv + 1)->s_name[0]) {
        pd_error(object, "[p2p.r.audio~] expected session ID and username");
    } else {
        *object->m_SessionId = atom_getsymbol(argv)->s_name;
        *object->m_Username = atom_getsymbol(argv + 1)->s_name;
    }
    clock_delay(object->m_AttachClock, 0);
    return object;
}

// ─────────────────────────────────────
static void P2PRAudioFree(P2PRAudio *object) {
    clock_unset(object->m_AttachClock);
    clock_free(object->m_AttachClock);
    P2PRAudioDetach(object);
    delete object->m_RealtimePeer;
    delete object->m_RetiredPeers;
    delete object->m_Peer;
    delete object->m_Session;
    delete object->m_Username;
    delete object->m_SessionId;
}

// ─────────────────────────────────────
extern "C" void setup_p2p0x2er0x2eaudio_tilde() {
    t_class *host_class = class_new(
        gensym("p2p.r.audio~"), reinterpret_cast<t_newmethod>(P2PRAudioNew),
        reinterpret_cast<t_method>(P2PRAudioFree), sizeof(P2PRAudio), CLASS_DEFAULT, A_GIMME, 0);
    class_addmethod(host_class, reinterpret_cast<t_method>(P2PRAudioDsp), gensym("dsp"), A_CANT, 0);
    P2PRAudio::GetClass() = host_class;
}
