#include "MaxFrontends.hpp"

#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"
#include "P2PMainThreadDispatch.hpp"

#include <ext.h>
#include <ext_obex.h>
#include <z_dsp.h>

#include <atomic>
#include <memory>
#include <string>

static t_class *p2p_config_class = nullptr;

struct ConfigLifetime {
    std::atomic<bool> active{true};
    t_object *object{nullptr};
    void *outlet{nullptr};
};

struct P2PConfig {
    t_object object;
    std::string *session_id;
    std::shared_ptr<P2PSession> *session;
    std::shared_ptr<ConfigLifetime> *lifetime;
    uint64_t listener_id;
    bool controls_session;
    void *outlet;
};

static void p2p_config_output_event(const std::shared_ptr<ConfigLifetime> &lifetime,
                                    const P2PEvent &event) {
    if (!lifetime || !lifetime->active || !lifetime->object || !lifetime->outlet) {
        return;
    }
    switch (event.type) {
    case P2PEventType::Log:
        object_post(lifetime->object,
                "[p2p.config] %s", event.text.c_str());
        break;
    case P2PEventType::Error: {
        object_error((t_object *)lifetime->object, "[p2p.config] %s", event.text.c_str());
        t_atom atom;
        atom_setsym(&atom, gensym(event.text.c_str()));
        outlet_anything(lifetime->outlet, gensym("error"), 1, &atom);
        break;
    }
    case P2PEventType::Connected:
        outlet_anything(lifetime->outlet, gensym("connected"), 0, nullptr);
        break;
    case P2PEventType::Disconnected:
        outlet_anything(lifetime->outlet, gensym("disconnected"), 0, nullptr);
        break;
    case P2PEventType::Connections: {
        t_atom atom;
        atom_setfloat(&atom, event.count);
        outlet_anything(lifetime->outlet, gensym("connections"), 1, &atom);
        break;
    }
    case P2PEventType::PeerJoined:
    case P2PEventType::PeerLeft: {
        t_atom atoms[2];
        atom_setsym(&atoms[0],
                    gensym(event.type == P2PEventType::PeerJoined ? "joined" : "left"));
        atom_setsym(&atoms[1], gensym(event.peer.c_str()));
        outlet_anything(lifetime->outlet, gensym("peer"), 2, atoms);
        break;
    }
    case P2PEventType::Message: {
        t_atom atoms[2];
        atom_setsym(&atoms[0], gensym(event.peer.c_str()));
        atom_setsym(&atoms[1], gensym(event.text.c_str()));
        outlet_anything(lifetime->outlet, gensym("json"), 2, atoms);
        break;
    }
    }
}

static void *p2p_config_new(t_symbol *, long argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PConfig *>(object_alloc(p2p_config_class));
    object->session_id = new std::string();
    object->session = new std::shared_ptr<P2PSession>();
    object->lifetime = new std::shared_ptr<ConfigLifetime>(std::make_shared<ConfigLifetime>());
    object->listener_id = 0;
    object->controls_session = false;
    object->outlet = outlet_new((t_object *)object, nullptr);
    (*object->lifetime)->object = &object->object;
    (*object->lifetime)->outlet = object->outlet;

    if (argc < 1 || atom_gettype(argv + 0) != A_SYM ||
        !atom_getsym(argv)->s_name[0]) {
        object_error((t_object *)object, "[p2p.config] missing session ID");
        return object;
    }
    *object->session_id = atom_getsym(argv)->s_name;
    *object->session = P2PSessionRegistry::acquire(
        *object->session_id, static_cast<int>(sys_getsr()));
    if ((*object->session)->sampleRate() != 48000) {
        object_error((t_object *)object,
                     "[p2p.config] requires a sample rate of exactly 48000 Hz");
        P2PSessionRegistry::release(*object->session_id, *object->session);
        object->session->reset();
        return object;
    }
    object->controls_session = (*object->session)->claimController(object);
    if (!object->controls_session) {
        object_error((t_object *)object, "[p2p.config] another config already controls session '%s'",
                 object->session_id->c_str());
        object->session->reset();
        return object;
    }
    std::weak_ptr<ConfigLifetime> weak_lifetime = *object->lifetime;
    object->listener_id =
        (*object->session)
            ->addListener([weak_lifetime](const P2PEvent &event) {
                if (auto lifetime = weak_lifetime.lock()) {
                    p2p_config_output_event(lifetime, event);
                }
            });
    return object;
}

static void p2p_config_free(P2PConfig *object) {
    if (object->lifetime && *object->lifetime) {
        (*object->lifetime)->active = false;
        (*object->lifetime)->object = nullptr;
        (*object->lifetime)->outlet = nullptr;
    }
    if (object->session && *object->session && object->controls_session) {
        (*object->session)->removeListener(object->listener_id);
        (*object->session)->releaseController(object);
        (*object->session)->deactivate();
        P2PSessionRegistry::release(*object->session_id, *object->session);
    }
    delete object->lifetime;
    delete object->session;
    delete object->session_id;
}

static void p2p_config_connect(P2PConfig *object, t_symbol *url, t_symbol *room,
                               t_symbol *username) {
    if (object->session && *object->session && object->controls_session) {
        (*object->session)->connect(url->s_name, room->s_name, username->s_name);
    }
}

static void p2p_config_disconnect(P2PConfig *object) {
    if (object->session && *object->session && object->controls_session) {
        (*object->session)->disconnect();
    }
}

static void p2p_config_stream(P2PConfig *object, double value) {
    if (object->session && *object->session && object->controls_session) {
        (*object->session)->setStreaming(value != 0);
    }
}

static std::string p2p_config_atoms_to_text(long argc, t_atom *argv) {
    std::string text;
    for (long index = 0; index < argc; ++index) {
        if (index) {
            text += ' ';
        }
        if (atom_gettype(argv + index) == A_SYM) {
            text += atom_getsym(argv + index)->s_name;
        } else if (atom_gettype(argv + index) == A_FLOAT) {
            text += std::to_string(atom_getfloat(argv + index));
        }
    }
    return text;
}

static void p2p_config_message(P2PConfig *object, t_symbol *, long argc, t_atom *argv) {
    if (object->session && *object->session && object->controls_session) {
        (*object->session)->sendMessage(p2p_config_atoms_to_text(argc, argv));
    }
}

static void p2p_config_json(P2PConfig *object, t_symbol *, long argc, t_atom *argv) {
    if (!object->session || !*object->session || !object->controls_session || argc < 1) {
        if (argc < 1) {
            object_error((t_object *)object, "[p2p.config] json message is empty");
        }
        return;
    }
    (*object->session)->sendJson(p2p_config_atoms_to_text(argc, argv));
}

static void p2p_config_report(P2PConfig *object) {
    if (object->session && *object->session && object->controls_session) {
        (*object->session)->report();
    }
}

void p2p_config_setup() {
    p2p_config_class =
        class_new("p2p.config", reinterpret_cast<method>(p2p_config_new),
                  reinterpret_cast<method>(p2p_config_free), sizeof(P2PConfig), nullptr,
                  A_GIMME, 0);
    class_addmethod(p2p_config_class, reinterpret_cast<method>(p2p_config_connect),
                    "connect", A_SYM, A_SYM, A_SYM, 0);
    class_addmethod(p2p_config_class, reinterpret_cast<method>(p2p_config_disconnect),
                    "disconnect", 0);
    class_addmethod(p2p_config_class, reinterpret_cast<method>(p2p_config_stream),
                    "stream", A_FLOAT, 0);
    class_addmethod(p2p_config_class, reinterpret_cast<method>(p2p_config_report),
                    "report", 0);
    class_addmethod(p2p_config_class, reinterpret_cast<method>(p2p_config_message),
                    "message", A_GIMME, 0);
    class_addmethod(p2p_config_class, reinterpret_cast<method>(p2p_config_json), "json",
                    A_GIMME, 0);
    class_register(CLASS_BOX, p2p_config_class);
}

extern "C" C74_EXPORT void ext_main(void *) {
    P2PMainThreadDispatch::initialize();
    p2p_config_setup();
}
