#include "P2PMainThreadDispatch.hpp"
#include "PdFrontends.hpp"

#include <m_pd.h>

// ─────────────────────────────────────
extern "C" void p2p_setup() {
    post("[p2p] by Charles K. Neimog %d.%d.%d", 0, 2, 0);
    P2PMainThreadDispatch::initialize();
    p2p_config_setup();
    p2p_s_audio_setup();
    p2p_r_audio_setup();
    p2p_r_video_setup();
}
