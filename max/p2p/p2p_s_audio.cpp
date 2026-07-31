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

namespace {
t_class *p2p_s_audio_class = nullptr;

struct P2PSAudio {
    t_pxobject object;
    std::string *session_id;
    std::shared_ptr<P2PSession> *session;
    std::vector<std::shared_ptr<P2PSession>> *retired_sessions;
    std::atomic<P2PSession *> *realtime_session;
    t_clock *attach_clock;
    bool claimed;
    bool missing_reported;
    bool duplicate_reported;
};

void senderDetach(P2PSAudio *object) {
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

void senderPoll(P2PSAudio *object) {
    if (!object->session_id->empty()) {
        auto current = std::atomic_load(object->session);
        auto found = P2PSessionRegistry::find(*object->session_id);
        if (found != current) {
            senderDetach(object);
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
                object_error((t_object *)object,
                         "[p2p.s.audio~] another sender already exists for this session");
            }
        }
        if (found) {
            object->missing_reported = false;
        } else if (!object->missing_reported) {
            object->missing_reported = true;
            object_error((t_object *)object, "[p2p.s.audio~] no active [p2p.config] for session '%s'; "
                             "waiting",
                     object->session_id->c_str());
        }
    }
    clock_delay(object->attach_clock, 100);
}

void senderPerform64(P2PSAudio *object, t_object *, double **inputs, long,
                     double **, long, long count, long, void *) {
    auto *session = object->realtime_session->load(std::memory_order_acquire);
    if (session && session->available()) {
        constexpr long block = 2048;
        float converted[block];
        for (long offset = 0; offset < count; offset += block) {
            const long n = std::min(block, count - offset);
            for (long i = 0; i < n; ++i) converted[i] = static_cast<float>(inputs[0][offset + i]);
            session->pushOutgoingAudio(converted, static_cast<int>(n));
        }
    }
}

void senderDsp64(P2PSAudio *object, t_object *dsp64, short *, double, long, long) {
    object_method(dsp64, gensym("dsp_add64"), object, senderPerform64, 0, nullptr);
}

void *senderNew(t_symbol *, long argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PSAudio *>(object_alloc(p2p_s_audio_class));
    dsp_setup(&object->object, 1);
    object->session_id = new std::string();
    object->session = new std::shared_ptr<P2PSession>();
    object->retired_sessions = new std::vector<std::shared_ptr<P2PSession>>();
    object->realtime_session = new std::atomic<P2PSession *>(nullptr);
    object->claimed = false;
    object->missing_reported = false;
    object->duplicate_reported = false;
    object->attach_clock = clock_new(object, reinterpret_cast<method>(senderPoll));
    if (argc < 1 || atom_gettype(argv + 0) != A_SYM ||
        !atom_getsym(argv)->s_name[0]) {
        object_error((t_object *)object, "[p2p.s.audio~] missing session ID");
    } else {
        *object->session_id = atom_getsym(argv)->s_name;
    }
    clock_delay(object->attach_clock, 0);
    return object;
}

void senderFree(P2PSAudio *object) {
    clock_unset(object->attach_clock);
    object_free(object->attach_clock);
    senderDetach(object);
    dsp_free(&object->object);
    delete object->realtime_session;
    delete object->retired_sessions;
    delete object->session;
    delete object->session_id;
}
} // namespace

void p2p_s_audio_setup() {
    p2p_s_audio_class =
        class_new("p2p.s.audio~", reinterpret_cast<method>(senderNew),
                  reinterpret_cast<method>(senderFree), sizeof(P2PSAudio), nullptr,
                  A_GIMME, 0);
    class_addmethod(p2p_s_audio_class, reinterpret_cast<method>(senderDsp64), "dsp64",
                    A_CANT, 0);
    class_dspinit(p2p_s_audio_class);
    class_register(CLASS_BOX, p2p_s_audio_class);
}

extern "C" C74_EXPORT void ext_main(void *) { p2p_s_audio_setup(); }
