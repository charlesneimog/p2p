#include <m_pd.h>
#include <string>
#include <cstdarg>
#include <cstdio>

struct p2p_tilde;

// typedef void (*t_messfn)(t_pd *obj, void *data);
struct p2p_tilde_messdata {
    enum P2P_MESS {
        LOG,
        MESSAGE,
    };
    P2P_MESS type;
    std::string msg;
    t_loglevel level;
};

// ─────────────────────────────────────
inline void p2p_tilde_mess(t_pd *obj, void *data) {
    p2p_tilde *x = (p2p_tilde *)obj;
    p2p_tilde_messdata *d = (p2p_tilde_messdata *)data;

    switch (d->type) {
    case p2p_tilde_messdata::LOG: {
        logpost(x, d->level, "[p2p~] %s", d->msg.c_str());
        break;
    }
    case p2p_tilde_messdata::MESSAGE: {
        break;
    }
    }

    delete d;
    return;
}
