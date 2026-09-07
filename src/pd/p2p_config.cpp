#include "P2PSession.hpp"
#include "P2PSessionRegistry.hpp"

#include <m_pd.h>

#include <atomic>
#include <memory>
#include <string>

struct ConfigLifetime {
    std::atomic<bool> m_Active{true};
    t_object *m_Object{nullptr};
    t_outlet *m_Outlet{nullptr};
};

struct P2PConfig {
    t_object m_Object;
    std::string *m_SessionId;
    std::shared_ptr<P2PSession> *m_Session;
    std::shared_ptr<ConfigLifetime> *m_Lifetime;
    uint64_t m_ListenerId;
    bool m_ControlsSession;
    t_outlet *m_Outlet;

    static t_class *&GetClass() {
        // The host retains this registration for the lifetime of the external.
        static t_class *host_class = nullptr;
        return host_class;
    }
};

static t_loglevel P2PConfigLogLevel(P2PLogLevel level) {
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

// ─────────────────────────────────────
static void P2PConfigOutputEvent(const std::shared_ptr<ConfigLifetime> &lifetime,
                                 const P2PEvent &event) {
    if (!lifetime || !lifetime->m_Active || !lifetime->m_Object || !lifetime->m_Outlet) {
        return;
    }
    switch (event.m_Type) {
    case P2PEventType::Log:
        logpost(lifetime->m_Object, P2PConfigLogLevel(event.m_LogLevel), "[p2p.config] %s",
                event.m_Text.c_str());
        break;
    case P2PEventType::Error: {
        pd_error(lifetime->m_Object, "[p2p.config] %s", event.m_Text.c_str());
        t_atom atom;
        SETSYMBOL(&atom, gensym(event.m_Text.c_str()));
        outlet_anything(lifetime->m_Outlet, gensym("error"), 1, &atom);
        break;
    }
    case P2PEventType::Connected:
        outlet_anything(lifetime->m_Outlet, gensym("connected"), 0, nullptr);
        break;
    case P2PEventType::Disconnected:
        outlet_anything(lifetime->m_Outlet, gensym("disconnected"), 0, nullptr);
        break;
    case P2PEventType::Connections: {
        t_atom atom;
        SETFLOAT(&atom, event.m_Count);
        outlet_anything(lifetime->m_Outlet, gensym("connections"), 1, &atom);
        break;
    }
    case P2PEventType::PeerJoined:
    case P2PEventType::PeerLeft: {
        t_atom atoms[2];
        SETSYMBOL(&atoms[0], gensym(event.m_Type == P2PEventType::PeerJoined ? "joined" : "left"));
        SETSYMBOL(&atoms[1], gensym(event.m_Peer.c_str()));
        outlet_anything(lifetime->m_Outlet, gensym("peer"), 2, atoms);
        break;
    }
    case P2PEventType::Message: {
        t_atom atoms[2];
        SETSYMBOL(&atoms[0], gensym(event.m_Peer.c_str()));
        SETSYMBOL(&atoms[1], gensym(event.m_Text.c_str()));
        outlet_anything(lifetime->m_Outlet, gensym("json"), 2, atoms);
        break;
    }
    }
}

// ─────────────────────────────────────
static void *P2PConfigNew(t_symbol *, int argc, t_atom *argv) {
    auto *object = reinterpret_cast<P2PConfig *>(pd_new(P2PConfig::GetClass()));
    object->m_SessionId = new std::string();
    object->m_Session = new std::shared_ptr<P2PSession>();
    object->m_Lifetime = new std::shared_ptr<ConfigLifetime>(std::make_shared<ConfigLifetime>());
    object->m_ListenerId = 0;
    object->m_ControlsSession = false;
    object->m_Outlet = outlet_new(&object->m_Object, &s_anything);
    (*object->m_Lifetime)->m_Object = &object->m_Object;
    (*object->m_Lifetime)->m_Outlet = object->m_Outlet;

    if (argc < 1 || argv[0].a_type != A_SYMBOL || !atom_getsymbol(argv)->s_name[0]) {
        pd_error(object, "[p2p.config] missing session ID");
        return object;
    }
    *object->m_SessionId = atom_getsymbol(argv)->s_name;
    *object->m_Session =
        P2PSessionRegistry::Acquire(*object->m_SessionId, static_cast<int>(sys_getsr()));
    if ((*object->m_Session)->SampleRate() != 48000) {
        pd_error(object, "[p2p.config] requires a sample rate of exactly 48000 Hz");
        P2PSessionRegistry::Release(*object->m_SessionId, *object->m_Session);
        object->m_Session->reset();
        return object;
    }
    object->m_ControlsSession = (*object->m_Session)->ClaimController(object);
    if (!object->m_ControlsSession) {
        pd_error(object, "[p2p.config] another config already controls session '%s'",
                 object->m_SessionId->c_str());
        object->m_Session->reset();
        return object;
    }
    std::weak_ptr<ConfigLifetime> weak_lifetime = *object->m_Lifetime;
    object->m_ListenerId =
        (*object->m_Session)->AddListener([weak_lifetime](const P2PEvent &event) {
            if (auto lifetime = weak_lifetime.lock()) {
                P2PConfigOutputEvent(lifetime, event);
            }
        });
    return object;
}

// ─────────────────────────────────────
static void P2PConfigFree(P2PConfig *object) {
    if (object->m_Lifetime && *object->m_Lifetime) {
        (*object->m_Lifetime)->m_Active = false;
        (*object->m_Lifetime)->m_Object = nullptr;
        (*object->m_Lifetime)->m_Outlet = nullptr;
    }
    if (object->m_Session && *object->m_Session && object->m_ControlsSession) {
        (*object->m_Session)->RemoveListener(object->m_ListenerId);
        (*object->m_Session)->ReleaseController(object);
        (*object->m_Session)->Deactivate();
        P2PSessionRegistry::Release(*object->m_SessionId, *object->m_Session);
    }
    delete object->m_Lifetime;
    delete object->m_Session;
    delete object->m_SessionId;
}

// ─────────────────────────────────────
static void P2PConfigConnect(P2PConfig *object, t_symbol *url, t_symbol *room, t_symbol *username) {
    if (object->m_Session && *object->m_Session && object->m_ControlsSession) {
        (*object->m_Session)->Connect(url->s_name, room->s_name, username->s_name);
    }
}

// ─────────────────────────────────────
static void P2PConfigDisconnect(P2PConfig *object) {
    if (object->m_Session && *object->m_Session && object->m_ControlsSession) {
        (*object->m_Session)->Disconnect();
    }
}

// ─────────────────────────────────────
static void P2PConfigStream(P2PConfig *object, t_floatarg value) {
    if (object->m_Session && *object->m_Session && object->m_ControlsSession) {
        (*object->m_Session)->SetStreaming(value != 0);
    }
}

// ─────────────────────────────────────
static std::string P2PConfigAtomsToText(int argc, t_atom *argv) {
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

// ─────────────────────────────────────
static void P2PConfigMessage(P2PConfig *object, t_symbol *, int argc, t_atom *argv) {
    if (object->m_Session && *object->m_Session && object->m_ControlsSession) {
        (*object->m_Session)->SendTextMessage(P2PConfigAtomsToText(argc, argv));
    }
}

// ─────────────────────────────────────
static void P2PConfigJson(P2PConfig *object, t_symbol *, int argc, t_atom *argv) {
    if (!object->m_Session || !*object->m_Session || !object->m_ControlsSession || argc < 1) {
        if (argc < 1) {
            pd_error(object, "[p2p.config] json message is empty");
        }
        return;
    }
    (*object->m_Session)->SendJson(P2PConfigAtomsToText(argc, argv));
}

// ─────────────────────────────────────
static void P2PConfigReport(P2PConfig *object) {
    if (object->m_Session && *object->m_Session && object->m_ControlsSession) {
        (*object->m_Session)->Report();
    }
}

// ─────────────────────────────────────
extern "C" void setup_p2p0x2econfig() {
    t_class *host_class = class_new(
        gensym("p2p.config"), reinterpret_cast<t_newmethod>(P2PConfigNew),
        reinterpret_cast<t_method>(P2PConfigFree), sizeof(P2PConfig), CLASS_DEFAULT, A_GIMME, 0);
    class_addmethod(host_class, reinterpret_cast<t_method>(P2PConfigConnect), gensym("connect"),
                    A_SYMBOL, A_SYMBOL, A_SYMBOL, 0);
    class_addmethod(host_class, reinterpret_cast<t_method>(P2PConfigDisconnect),
                    gensym("disconnect"), A_NULL, 0);
    class_addmethod(host_class, reinterpret_cast<t_method>(P2PConfigStream), gensym("stream"),
                    A_FLOAT, 0);
    class_addmethod(host_class, reinterpret_cast<t_method>(P2PConfigReport), gensym("report"),
                    A_NULL, 0);
    class_addmethod(host_class, reinterpret_cast<t_method>(P2PConfigMessage), gensym("message"),
                    A_GIMME, 0);
    class_addmethod(host_class, reinterpret_cast<t_method>(P2PConfigJson), gensym("json"), A_GIMME,
                    0);
    P2PConfig::GetClass() = host_class;
}
