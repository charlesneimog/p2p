#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"

#include <m_pd.h>

#include <atomic>
#include <memory>
#include <string>

static t_class *p2p_config_class = nullptr;

struct ConfigLifetime {
    std::atomic<bool> active{true};
    t_object *object{nullptr};
    t_outlet *outlet{nullptr};
};

struct P2PConfig {
    t_object object;
    std::string *session_id;
    std::shared_ptr<P2PSession> *session;
    std::shared_ptr<ConfigLifetime> *lifetime;
    uint64_t listener_id;
    bool controls_session;
    t_outlet *outlet;
};

static t_loglevel p2p_config_log_level(P2PLogLevel level) {
    switch (level) {
    case P2PLogLevel::Debug:
        return PD_DEBUG;
    case P2PLogLevel::Error:
        return PD_ERROR;
    case P2PLogLevel::Normal:
    default:
        return PD_NORMAL;
    }
}

static void p2p_config_output_event(const std::shared_ptr<ConfigLifetime> &lifetime,
                                    const P2PEvent &event) {
    if (!lifetime || !lifetime->active || !lifetime->object || !lifetime->outlet) {
        return;
    }
    switch (event.type) {
    case P2PEventType::Log:
        logpost(lifetime->object, p2p_config_log_level(event.log_level), "[p2p.config] %s",
                event.text.c_str());
        break;
    case P2PEventType::Error: {
        pd_error(lifetime->object, "[p2p.config] %s", event.text.c_str());
        t_atom atom;
        SETSYMBOL(&atom, gensym(event.text.c_str()));
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
        SETFLOAT(&atom, event.count);
        outlet_anything(lifetime->outlet, gensym("connections"), 1, &atom);
        break;
    }
    case P2PEventType::PeerJoined:
    case P2PEventType::PeerLeft: {
        t_atom atoms[2];
        SETSYMBOL(&atoms[0], gensym(event.type == P2PEventType::PeerJoined ? "joined" : "left"));
        SETSYMBOL(&atoms[1], gensym(event.peer.c_str()));
        outlet_anything(lifetime->outlet, gensym("peer"), 2, atoms);
        break;
    }
    case P2PEventType::Message: {
        t_atom atoms[2];
        SETSYMBOL(&atoms[0], gensym(event.peer.c_str()));
        SETSYMBOL(&atoms[1], gensym(event.text.c_str()));
        outlet_anything(lifetime->outlet, gensym("json"), 2, atoms);
        break;
    }
    }
}

static void *p2p_config_new(t_symbol *, int argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PConfig *>(pd_new(p2p_config_class));
    object->session_id = new std::string();
    object->session = new std::shared_ptr<P2PSession>();
    object->lifetime = new std::shared_ptr<ConfigLifetime>(std::make_shared<ConfigLifetime>());
    object->listener_id = 0;
    object->controls_session = false;
    object->outlet = outlet_new(&object->object, &s_anything);
    (*object->lifetime)->object = &object->object;
    (*object->lifetime)->outlet = object->outlet;

    if (argc < 1 || argv[0].a_type != A_SYMBOL || !atom_getsymbol(argv)->s_name[0]) {
        pd_error(object, "[p2p.config] missing session ID");
        return object;
    }
    *object->session_id = atom_getsymbol(argv)->s_name;
    *object->session =
        P2PSessionRegistry::acquire(*object->session_id, static_cast<int>(sys_getsr()));
    if ((*object->session)->sampleRate() != 48000) {
        pd_error(object, "[p2p.config] requires a sample rate of exactly 48000 Hz");
        P2PSessionRegistry::release(*object->session_id, *object->session);
        object->session->reset();
        return object;
    }
    object->controls_session = (*object->session)->claimController(object);
    if (!object->controls_session) {
        pd_error(object, "[p2p.config] another config already controls session '%s'",
                 object->session_id->c_str());
        object->session->reset();
        return object;
    }
    std::weak_ptr<ConfigLifetime> weak_lifetime = *object->lifetime;
    object->listener_id = (*object->session)->addListener([weak_lifetime](const P2PEvent &event) {
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

static void p2p_config_stream(P2PConfig *object, t_floatarg value) {
    if (object->session && *object->session && object->controls_session) {
        (*object->session)->setStreaming(value != 0);
    }
}

static std::string p2p_config_atoms_to_text(int argc, t_atom *argv) {
    std::string text;
    for (int index = 0; index < argc; ++index) {
        if (index) {
            text += ' ';
        }
        if (argv[index].a_type == A_SYMBOL) {
            text += atom_getsymbol(argv + index)->s_name;
        } else if (argv[index].a_type == A_FLOAT) {
            text += std::to_string(atom_getfloat(argv + index));
        }
    }
    return text;
}

static void p2p_config_message(P2PConfig *object, t_symbol *, int argc, t_atom *argv) {
    if (object->session && *object->session && object->controls_session) {
        (*object->session)->sendMessage(p2p_config_atoms_to_text(argc, argv));
    }
}

static void p2p_config_json(P2PConfig *object, t_symbol *, int argc, t_atom *argv) {
    if (!object->session || !*object->session || !object->controls_session || argc < 1) {
        if (argc < 1) {
            pd_error(object, "[p2p.config] json message is empty");
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

extern "C" void setup_p2p0x2econfig() {
    p2p_config_class = class_new(
        gensym("p2p.config"), reinterpret_cast<t_newmethod>(p2p_config_new),
        reinterpret_cast<t_method>(p2p_config_free), sizeof(P2PConfig), CLASS_DEFAULT, A_GIMME, 0);
    class_addmethod(p2p_config_class, reinterpret_cast<t_method>(p2p_config_connect),
                    gensym("connect"), A_SYMBOL, A_SYMBOL, A_SYMBOL, 0);
    class_addmethod(p2p_config_class, reinterpret_cast<t_method>(p2p_config_disconnect),
                    gensym("disconnect"), A_NULL, 0);
    class_addmethod(p2p_config_class, reinterpret_cast<t_method>(p2p_config_stream),
                    gensym("stream"), A_FLOAT, 0);
    class_addmethod(p2p_config_class, reinterpret_cast<t_method>(p2p_config_report),
                    gensym("report"), A_NULL, 0);
    class_addmethod(p2p_config_class, reinterpret_cast<t_method>(p2p_config_message),
                    gensym("message"), A_GIMME, 0);
    class_addmethod(p2p_config_class, reinterpret_cast<t_method>(p2p_config_json), gensym("json"),
                    A_GIMME, 0);
}
