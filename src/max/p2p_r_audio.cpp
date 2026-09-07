#include "MaxFrontends.hpp"

#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"

#include <ext.h>
#include <ext_obex.h>
#include <z_dsp.h>

#include <atomic>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct P2PRAudio {
    t_pxobject m_Object;
    std::string *m_SessionId;
    std::string *m_Username;
    std::shared_ptr<P2PSession> *m_Session;
    std::shared_ptr<P2PPeer> *m_Peer;
    std::vector<std::shared_ptr<P2PPeer>> *m_RetiredPeers;
    std::atomic<P2PPeer *> *m_RealtimePeer;
    t_clock *m_AttachClock;
    void *m_SignalOutlet;
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
                object_error((t_object *)object,
                             "[p2p.r.audio~] another receiver already consumes this user");
            }
        }
        if (!found) {
            P2PRAudioStorePeer(object, {});
            if (!object->m_MissingReported) {
                object->m_MissingReported = true;
                object_error((t_object *)object,
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
                    object_error((t_object *)object,
                                 "[p2p.r.audio~] duplicate username is ambiguous: '%s'",
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
static void P2PRAudioPerform64(P2PRAudio *object, t_object *, double **, long, double **outputs,
                               long, long count, long, void *) {
    double *output = outputs[0];
    P2PPeer *peer = object->m_RealtimePeer->load(std::memory_order_acquire);
    if (!peer || !peer->m_Active || !peer->m_Connected) {
        std::fill(output, output + count, 0.0);
        return;
    }
    for (int index = 0; index < count; ++index) {
        float sample = 0;
        peer->PopReceived(sample);
        output[index] = sample;
    }
}

// ─────────────────────────────────────
static void P2PRAudioDsp64(P2PRAudio *object, t_object *dsp64, short *, double, long, long) {
    object_method(dsp64, gensym("dsp_add64"), object, P2PRAudioPerform64, 0, nullptr);
}

// ─────────────────────────────────────
static void *P2PRAudioNew(t_symbol *, long argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PRAudio *>(object_alloc(P2PRAudio::GetClass()));
    dsp_setup(&object->m_Object, 0);
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
    object->m_SignalOutlet = outlet_new((t_object *)object, "signal");
    object->m_AttachClock = clock_new(object, reinterpret_cast<method>(P2PRAudioPoll));
    if (argc < 2 || atom_gettype(argv + 0) != A_SYM || atom_gettype(argv + 1) != A_SYM ||
        !atom_getsym(argv)->s_name[0] || !atom_getsym(argv + 1)->s_name[0]) {
        object_error((t_object *)object, "[p2p.r.audio~] expected session ID and username");
    } else {
        *object->m_SessionId = atom_getsym(argv)->s_name;
        *object->m_Username = atom_getsym(argv + 1)->s_name;
    }
    clock_delay(object->m_AttachClock, 0);
    return object;
}

// ─────────────────────────────────────
static void P2PRAudioFree(P2PRAudio *object) {
    clock_unset(object->m_AttachClock);
    object_free(object->m_AttachClock);
    P2PRAudioDetach(object);
    dsp_free(&object->m_Object);
    delete object->m_RealtimePeer;
    delete object->m_RetiredPeers;
    delete object->m_Peer;
    delete object->m_Session;
    delete object->m_Username;
    delete object->m_SessionId;
}

// ─────────────────────────────────────
void P2PRAudioSetup() {
    t_class *host_class =
        class_new("p2p.r.audio~", reinterpret_cast<method>(P2PRAudioNew),
                  reinterpret_cast<method>(P2PRAudioFree), sizeof(P2PRAudio), nullptr, A_GIMME, 0);
    class_addmethod(host_class, reinterpret_cast<method>(P2PRAudioDsp64), "dsp64", A_CANT, 0);
    class_dspinit(host_class);
    class_register(CLASS_BOX, host_class);
    P2PRAudio::GetClass() = host_class;
}

// ─────────────────────────────────────
extern "C" C74_EXPORT void ext_main(void *) {
    P2PRAudioSetup();
}
