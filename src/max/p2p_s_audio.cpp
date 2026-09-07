#include "MaxFrontends.hpp"

#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"

#include <ext.h>
#include <ext_obex.h>
#include <z_dsp.h>

#include <atomic>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

struct P2PSAudio {
    t_pxobject m_Object;
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
                object_error((t_object *)object,
                             "[p2p.s.audio~] another sender already exists for this session");
            }
        }
        if (found) {
            object->m_MissingReported = false;
        } else if (!object->m_MissingReported) {
            object->m_MissingReported = true;
            object_error((t_object *)object,
                         "[p2p.s.audio~] no active [p2p.config] for session '%s'; "
                         "waiting",
                         object->m_SessionId->c_str());
        }
    }
    clock_delay(object->m_AttachClock, 100);
}

// ─────────────────────────────────────
static void P2PSAudioPerform64(P2PSAudio *object, t_object *, double **inputs, long, double **,
                               long, long count, long, void *) {
    P2PSession *session = object->m_RealtimeSession->load(std::memory_order_acquire);
    if (session && session->Available()) {
        constexpr long block = 2048;
        float converted[block];
        for (long offset = 0; offset < count; offset += block) {
            const long n = std::min(block, count - offset);
            for (long i = 0; i < n; ++i) {
                converted[i] = static_cast<float>(inputs[0][offset + i]);
            }
            session->PushOutgoingAudio(converted, static_cast<int>(n));
        }
    }
}

// ─────────────────────────────────────
static void P2PSAudioDsp64(P2PSAudio *object, t_object *dsp64, short *, double, long, long) {
    object_method(dsp64, gensym("dsp_add64"), object, P2PSAudioPerform64, 0, nullptr);
}

// ─────────────────────────────────────
static void *P2PSAudioNew(t_symbol *, long argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PSAudio *>(object_alloc(P2PSAudio::GetClass()));
    dsp_setup(&object->m_Object, 1);
    object->m_SessionId = new std::string();
    object->m_Session = new std::shared_ptr<P2PSession>();
    object->m_RetiredSessions = new std::vector<std::shared_ptr<P2PSession>>();
    object->m_RealtimeSession = new std::atomic<P2PSession *>(nullptr);
    object->m_Claimed = false;
    object->m_MissingReported = false;
    object->m_DuplicateReported = false;
    object->m_AttachClock = clock_new(object, reinterpret_cast<method>(P2PSAudioPoll));
    if (argc < 1 || atom_gettype(argv + 0) != A_SYM || !atom_getsym(argv)->s_name[0]) {
        object_error((t_object *)object, "[p2p.s.audio~] missing session ID");
    } else {
        *object->m_SessionId = atom_getsym(argv)->s_name;
    }
    clock_delay(object->m_AttachClock, 0);
    return object;
}

// ─────────────────────────────────────
static void P2PSAudioFree(P2PSAudio *object) {
    clock_unset(object->m_AttachClock);
    object_free(object->m_AttachClock);
    P2PSAudioDetach(object);
    dsp_free(&object->m_Object);
    delete object->m_RealtimeSession;
    delete object->m_RetiredSessions;
    delete object->m_Session;
    delete object->m_SessionId;
}

// ─────────────────────────────────────
void P2PSAudioSetup() {
    t_class *host_class =
        class_new("p2p.s.audio~", reinterpret_cast<method>(P2PSAudioNew),
                  reinterpret_cast<method>(P2PSAudioFree), sizeof(P2PSAudio), nullptr, A_GIMME, 0);
    class_addmethod(host_class, reinterpret_cast<method>(P2PSAudioDsp64), "dsp64", A_CANT, 0);
    class_dspinit(host_class);
    class_register(CLASS_BOX, host_class);
    P2PSAudio::GetClass() = host_class;
}

// ─────────────────────────────────────
extern "C" C74_EXPORT void ext_main(void *) {
    P2PSAudioSetup();
}
