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

static t_class *p2p_r_audio_class = nullptr;

struct P2PRAudio {
    t_pxobject object;
    std::string *session_id;
    std::string *username;
    std::shared_ptr<P2PSession> *session;
    std::shared_ptr<P2PPeer> *peer;
    std::vector<std::shared_ptr<P2PPeer>> *retired_peers;
    std::atomic<P2PPeer *> *realtime_peer;
    t_clock *attach_clock;
    void *signal_outlet;
    bool claimed;
    bool missing_reported;
    bool duplicate_consumer_reported;
    bool ambiguity_reported;
};

static void p2p_r_audio_store_peer(P2PRAudio *object, std::shared_ptr<P2PPeer> peer) {
    auto old_peer = std::atomic_load(object->peer);
    if (old_peer == peer) {
        return;
    }
    object->realtime_peer->store(nullptr, std::memory_order_release);
    if (old_peer) {
        object->retired_peers->push_back(std::move(old_peer));
    }
    std::atomic_store(object->peer, std::move(peer));
    auto current = std::atomic_load(object->peer);
    object->realtime_peer->store(current.get(), std::memory_order_release);
}

static void p2p_r_audio_detach(P2PRAudio *object) {
    auto session = std::atomic_load(object->session);
    if (session && object->claimed) {
        session->releaseAudioReceiver(*object->username, object);
    }
    object->claimed = false;
    p2p_r_audio_store_peer(object, {});
    std::atomic_store(object->session, std::shared_ptr<P2PSession>());
}

static void p2p_r_audio_poll(P2PRAudio *object) {
    if (!object->session_id->empty() && !object->username->empty()) {
        auto current = std::atomic_load(object->session);
        auto found = P2PSessionRegistry::find(*object->session_id);
        if (found != current) {
            p2p_r_audio_detach(object);
            if (found) {
                std::atomic_store(object->session, found);
            }
        }
        if (found && !object->claimed) {
            object->claimed = found->claimAudioReceiver(*object->username, object);
            if (!object->claimed && !object->duplicate_consumer_reported) {
                object->duplicate_consumer_reported = true;
                object_error((t_object *)object,
                         "[p2p.r.audio~] another receiver already consumes this user");
            }
        }
        if (!found) {
            p2p_r_audio_store_peer(object, {});
            if (!object->missing_reported) {
                object->missing_reported = true;
                object_error((t_object *)object, "[p2p.r.audio~] no active [p2p.config] for session '%s'; "
                                 "waiting",
                         object->session_id->c_str());
            }
        } else {
            object->missing_reported = false;
            const auto resolution =
                object->claimed ? found->resolvePeer(*object->username) : P2PPeerResolution{};
            if (resolution.ambiguous) {
                p2p_r_audio_store_peer(object, {});
                if (!object->ambiguity_reported) {
                    object->ambiguity_reported = true;
                    object_error((t_object *)object, "[p2p.r.audio~] duplicate username is ambiguous: '%s'",
                             object->username->c_str());
                }
            } else {
                object->ambiguity_reported = false;
                p2p_r_audio_store_peer(object, resolution.peer);
            }
        }
    }
    clock_delay(object->attach_clock, 50);
}

static void p2p_r_audio_perform64(P2PRAudio *object, t_object *, double **, long,
                                  double **outputs, long, long count, long, void *) {
    auto *output = outputs[0];
    auto *peer = object->realtime_peer->load(std::memory_order_acquire);
    if (!peer || !peer->active || !peer->connected) {
        std::fill(output, output + count, 0.0);
        return;
    }
    for (int index = 0; index < count; ++index) {
        float sample = 0;
        peer->popReceived(sample);
        output[index] = sample;
    }
}

static void p2p_r_audio_dsp64(P2PRAudio *object, t_object *dsp64, short *, double, long, long) {
    object_method(dsp64, gensym("dsp_add64"), object, p2p_r_audio_perform64, 0, nullptr);
}

static void *p2p_r_audio_new(t_symbol *, long argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PRAudio *>(object_alloc(p2p_r_audio_class));
    dsp_setup(&object->object, 0);
    object->session_id = new std::string();
    object->username = new std::string();
    object->session = new std::shared_ptr<P2PSession>();
    object->peer = new std::shared_ptr<P2PPeer>();
    object->retired_peers = new std::vector<std::shared_ptr<P2PPeer>>();
    object->realtime_peer = new std::atomic<P2PPeer *>(nullptr);
    object->claimed = false;
    object->missing_reported = false;
    object->duplicate_consumer_reported = false;
    object->ambiguity_reported = false;
    object->signal_outlet = outlet_new((t_object *)object, "signal");
    object->attach_clock = clock_new(object, reinterpret_cast<method>(p2p_r_audio_poll));
    if (argc < 2 || atom_gettype(argv + 0) != A_SYM || atom_gettype(argv + 1) != A_SYM ||
        !atom_getsym(argv)->s_name[0] || !atom_getsym(argv + 1)->s_name[0]) {
        object_error((t_object *)object, "[p2p.r.audio~] expected session ID and username");
    } else {
        *object->session_id = atom_getsym(argv)->s_name;
        *object->username = atom_getsym(argv + 1)->s_name;
    }
    clock_delay(object->attach_clock, 0);
    return object;
}

static void p2p_r_audio_free(P2PRAudio *object) {
    clock_unset(object->attach_clock);
    object_free(object->attach_clock);
    p2p_r_audio_detach(object);
    dsp_free(&object->object);
    delete object->realtime_peer;
    delete object->retired_peers;
    delete object->peer;
    delete object->session;
    delete object->username;
    delete object->session_id;
}

void p2p_r_audio_setup() {
    p2p_r_audio_class =
        class_new("p2p.r.audio~", reinterpret_cast<method>(p2p_r_audio_new),
                  reinterpret_cast<method>(p2p_r_audio_free), sizeof(P2PRAudio), nullptr,
                  A_GIMME, 0);
    class_addmethod(p2p_r_audio_class, reinterpret_cast<method>(p2p_r_audio_dsp64), "dsp64",
                    A_CANT, 0);
    class_dspinit(p2p_r_audio_class);
    class_register(CLASS_BOX, p2p_r_audio_class);
}

extern "C" C74_EXPORT void ext_main(void *) { p2p_r_audio_setup(); }
