#include <m_pd.h>
#include <string.h>
#include <stdlib.h>
#include <cstring>
#include <string>
#include <atomic>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <map>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <opus/opus.h>

#include <boost/lockfree/spsc_queue.hpp>

#include "message_queue.hpp"

using json = nlohmann::json;

// ─────────────────────────────────────
struct QueuedCandidate {
    std::string candidate;
    std::string mid;
};

struct P2PNode {
    // Connection state
    std::string user;
    std::string remote_peer_id;
    std::atomic<bool> is_streaming{false};
    std::vector<QueuedCandidate> pending_remote_candidates;
    bool remote_description_set{false};
    bool is_polite{false};
    bool making_offer{false};
    bool ignore_offer{false};

    std::shared_ptr<ix::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> audio_track;

    // Audio buffers
    boost::lockfree::spsc_queue<float, boost::lockfree::capacity<16384>> send_buffer;
    boost::lockfree::spsc_queue<float, boost::lockfree::capacity<16384>> receive_buffer;

    // Audio codec
    OpusEncoder *opus_enc = nullptr;
    OpusDecoder *opus_dec = nullptr;
    const int sample_rate = 48000;
    const int frame_size = 480;

    // Threading
    std::thread tx_thread;
    std::atomic<bool> thread_running{true};
    int channel_index = -1;

    ~P2PNode() {
        thread_running = false;
        if (tx_thread.joinable()) {
            tx_thread.join();
        }
        if (opus_enc) {
            opus_encoder_destroy(opus_enc);
        }
        if (opus_dec) {
            opus_decoder_destroy(opus_dec);
        }
    }
};

// ─────────────────────────────────────
static t_class *p2p_tilde_class;
struct p2p_tilde {
    t_object x_obj;
    t_sample x_f;
    std::vector<std::unique_ptr<P2PNode>> nodes;
    std::shared_ptr<ix::WebSocket> shared_ws;
    float peer_connected;
    t_clock *report_clock;
    t_outlet *out_signals;
    t_outlet *out_msgs;

    std::unordered_map<std::string, int> peers_channels;

    bool multichannel;
    bool fixchannels;
    int max_channels;
};

// ─────────────────────────────────────
static P2PNode *find_node_by_peer(p2p_tilde *x, const std::string &peer_id) {
    for (auto &node : x->nodes) {
        if (node->remote_peer_id == peer_id) {
            return node.get();
        }
    }
    return nullptr;
}

// ─────────────────────────────────────
static P2PNode *find_free_node(p2p_tilde *x) {
    for (auto &node : x->nodes) {
        if (node->remote_peer_id.empty() && !node->pc) {
            return node.get();
        }
    }
    return nullptr;
}

// ─────────────────────────────────────
static int count_active_nodes(p2p_tilde *x) {
    int count = 0;
    for (auto &node : x->nodes) {
        if (!node->remote_peer_id.empty() && node->pc) {
            count++;
        }
    }
    return count;
}

// ─────────────────────────────────────
inline void safelogpost(p2p_tilde *x, t_loglevel level, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    auto *d = new p2p_tilde_messdata();
    d->type = p2p_tilde_messdata::LOG;
    d->level = level;
    d->msg = buf;

    pd_queue_mess(&pd_maininstance, &x->x_obj.te_g.g_pd, d, p2p_tilde_mess);
}

// ─────────────────────────────────────
static void setup_webrtc_for_node(p2p_tilde *x, P2PNode *node, bool is_caller) {
    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    node->pc = std::make_shared<rtc::PeerConnection>(config);
    node->remote_description_set = false;

    node->pc->onStateChange([x, node](rtc::PeerConnection::State state) {
        safelogpost(x, PD_DEBUG, "[p2p~] PC State for peer %s: %d", node->remote_peer_id.c_str(),
                    state);
        if (state == rtc::PeerConnection::State::Failed) {
            safelogpost(x, PD_ERROR, "[p2p~] Connection failed for peer %s!",
                        node->remote_peer_id.c_str());
        } else if (state == rtc::PeerConnection::State::Connected) {
            safelogpost(x, PD_NORMAL, "[p2p~] WebRTC Connected to peer %s!",
                        node->remote_peer_id.c_str());
        }
    });

    node->pc->onSignalingStateChange([x, node](rtc::PeerConnection::SignalingState state) {
        safelogpost(x, PD_DEBUG, "[p2p~] Signaling State for peer %s: %d",
                    node->remote_peer_id.c_str(), state);
        if (state == rtc::PeerConnection::SignalingState::Stable) {
            node->making_offer = false;
        }
    });

    node->pc->onLocalDescription([x, node](rtc::Description description) {
        if (node->ws && node->ws->getReadyState() == ix::ReadyState::Open) {
            json msg = {
                {"type", description.typeString()},
                {"sdp", {{"type", description.typeString()}, {"sdp", std::string(description)}}},
                {"to", node->remote_peer_id}};
            node->ws->send(msg.dump());
            safelogpost(x, PD_DEBUG, "[p2p~] Sent %s to %s", description.typeString().c_str(),
                        node->remote_peer_id.c_str());
        }
        node->making_offer = false;
    });

    node->pc->onLocalCandidate([x, node](rtc::Candidate candidate) {
        if (node->ws && node->ws->getReadyState() == ix::ReadyState::Open) {
            json msg;
            msg["type"] = "ice-candidate";
            msg["candidate"]["candidate"] = candidate.candidate();
            msg["candidate"]["sdpMid"] = candidate.mid();
            msg["candidate"]["sdpMLineIndex"] = 0;
            msg["to"] = node->remote_peer_id;
            node->ws->send(msg.dump());
            safelogpost(x, PD_DEBUG, "[p2p~] Sent ICE candidate to %s",
                        node->remote_peer_id.c_str());
        }
    });

    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
    audio.addOpusCodec(111);
    audio.setBitrate(64000);
    node->audio_track = node->pc->addTrack(audio);
    if (!node->audio_track) {
        safelogpost(x, PD_ERROR, "[p2p~] Failed to add audio track for peer %s",
                    node->remote_peer_id.c_str());
        return;
    }

    safelogpost(x, PD_NORMAL, "[p2p~] Audio track added for peer %s", node->remote_peer_id.c_str());

    node->audio_track->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
    node->audio_track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

    node->audio_track->onFrame([x, node](rtc::binary data, rtc::FrameInfo) {
        if (!node->opus_dec) {
            return;
        }
        const int MAX_SAMPLES = 5760;
        float pcm[MAX_SAMPLES];
        int samples =
            opus_decode_float(node->opus_dec, reinterpret_cast<const unsigned char *>(data.data()),
                              data.size(), pcm, MAX_SAMPLES, 0);
        if (samples > 0) {
            for (int i = 0; i < samples; i++) {
                node->receive_buffer.push(pcm[i]);
            }
        }
    });

    node->pc->onTrack([x, node](std::shared_ptr<rtc::Track> track) {
        if (track->description().type() == "audio") {
            safelogpost(x, PD_NORMAL, "[p2p~] Remote audio track active for peer %s",
                        node->remote_peer_id.c_str());
        }
    });

    if (is_caller) {
        node->dc = node->pc->createDataChannel("pd_data");
        node->dc->onOpen([x, node]() {
            safelogpost(x, PD_NORMAL, "[p2p~] DataChannel open with peer %s",
                        node->remote_peer_id.c_str());
        });
    } else {
        node->pc->onDataChannel([x, node](std::shared_ptr<rtc::DataChannel> dc) {
            node->dc = dc;
            node->dc->onOpen([x, node]() {
                safelogpost(x, PD_NORMAL, "[p2p~] DataChannel open with peer %s",
                            node->remote_peer_id.c_str());
            });
        });
    }
}

// ─────────────────────────────────────
static void flush_pending_candidates(p2p_tilde *x, P2PNode *node) {
    for (const auto &qc : node->pending_remote_candidates) {
        try {
            rtc::Candidate rtc_cand(qc.candidate, qc.mid);
            node->pc->addRemoteCandidate(rtc_cand);
            safelogpost(x, PD_DEBUG, "[p2p~] Flushed candidate for peer %s",
                        node->remote_peer_id.c_str());
        } catch (const std::exception &e) {
            safelogpost(x, PD_ERROR, "[p2p~] Failed to add queued candidate: %s", e.what());
        }
    }
    node->pending_remote_candidates.clear();
}

// ─────────────────────────────────────
static void p2p_connect(p2p_tilde *x, t_symbol *wss, t_symbol *room, t_symbol *user) {
    if (!x->shared_ws) {
        x->shared_ws = std::make_shared<ix::WebSocket>();
    }

    std::string url = std::string(wss->s_name) + "/?room=" + std::string(room->s_name);
    x->shared_ws->setUrl(url);
    ix::WebSocketHttpHeaders headers;
    headers["Origin"] = "https://charlesneimog.github.io";
    x->shared_ws->setExtraHeaders(headers);
    std::string username = std::string(user->s_name);

    x->shared_ws->setOnMessageCallback([x, username](const ix::WebSocketMessagePtr &msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            json join = {{"type", "join"}, {"name", username}};
            x->shared_ws->send(join.dump());
            safelogpost(x, PD_NORMAL, "[p2p~] Connected to the room");
            return;
        }

        if (msg->type != ix::WebSocketMessageType::Message) {
            return;
        }

        json data = json::parse(msg->str);
        std::string type = data["type"];
        std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
        safelogpost(x, PD_DEBUG, "[p2p~] Received: %s from %s", type.c_str(), from_peer.c_str());

        if (type == "peer-joined") {
            P2PNode *node = find_free_node(x);
            if (!node) {
                safelogpost(x, PD_ERROR, "[p2p~] No free nodes available for peer %s",
                            from_peer.c_str());
                return;
            }

            // FIX Bug 1: store the peer's display name so setchannel lookup works.
            // The signaling server sends the name inside data["peer"]["name"].
            std::string peer_name = data.contains("peer") && data["peer"].contains("name")
                                        ? data["peer"]["name"].get<std::string>()
                                        : from_peer;
            node->user = peer_name;
            node->remote_peer_id = from_peer;
            node->ws = x->shared_ws;
            node->is_polite = false;
            setup_webrtc_for_node(x, node, true);
            x->peer_connected = count_active_nodes(x);
            clock_delay(x->report_clock, 0);
            node->making_offer = true;
            node->pc->setLocalDescription();

        } else if (type == "offer") {
            P2PNode *node = find_node_by_peer(x, from_peer);
            if (!node) {
                // No existing node for this peer — browser initiated first, PD is polite.
                node = find_free_node(x);
                if (!node) {
                    safelogpost(x, PD_ERROR, "[p2p~] No free nodes for offer from %s",
                                from_peer.c_str());
                    return;
                }
                node->remote_peer_id = from_peer;
                node->ws = x->shared_ws;
                // FIX Bug 2: only set is_polite=true when we are truly the answerer
                // (no existing PC). If pc already exists from existing-peers, is_polite
                // stays false and we win the glare collision below.
                node->is_polite = true;
                setup_webrtc_for_node(x, node, false);
            }
            // If node already exists (set up from existing-peers with is_polite=false),
            // do not overwrite is_polite — the impolite-peer glare logic must stay intact.

            if (node->making_offer && !node->is_polite) {
                safelogpost(x, PD_DEBUG, "[p2p~] Glare: ignoring offer from %s (impolite)",
                            from_peer.c_str());
                node->ignore_offer = true;
                return;
            }
            node->making_offer = false;
            node->ignore_offer = false;

            std::string sdp_str;
            if (data["sdp"].is_object()) {
                sdp_str = data["sdp"]["sdp"].get<std::string>();
            } else if (data["sdp"].is_string()) {
                sdp_str = data["sdp"].get<std::string>();
            } else {
                safelogpost(x, PD_ERROR, "[p2p~] Invalid SDP format");
                return;
            }

            try {
                rtc::Description desc(sdp_str, "offer");
                node->pc->setRemoteDescription(std::move(desc));
                node->remote_description_set = true;
                flush_pending_candidates(x, node);
                node->making_offer = true;
                node->pc->setLocalDescription();
            } catch (const std::exception &e) {
                safelogpost(x, PD_ERROR, "[p2p~] Failed to set remote description: %s", e.what());
            }

        } else if (type == "answer") {
            P2PNode *node = find_node_by_peer(x, from_peer);
            if (!node) {
                return;
            }

            if (node->ignore_offer) {
                safelogpost(x, PD_DEBUG, "[p2p~] Ignoring answer from %s", from_peer.c_str());
                node->ignore_offer = false;
                return;
            }

            std::string sdp_str;
            if (data["sdp"].is_object()) {
                sdp_str = data["sdp"]["sdp"].get<std::string>();
            } else if (data["sdp"].is_string()) {
                sdp_str = data["sdp"].get<std::string>();
            } else {
                safelogpost(x, PD_ERROR, "[p2p~] Invalid SDP format");
                return;
            }

            try {
                rtc::Description desc(sdp_str, "answer");
                node->pc->setRemoteDescription(std::move(desc));
                node->remote_description_set = true;
                flush_pending_candidates(x, node);
            } catch (const std::exception &e) {
                safelogpost(x, PD_ERROR, "[p2p~] Failed to set remote description: %s", e.what());
            }

        } else if (type == "ice-candidate") {
            P2PNode *node = find_node_by_peer(x, from_peer);
            if (!node) {
                return;
            }

            auto cand = data["candidate"];
            std::string cand_str = cand["candidate"].get<std::string>();
            std::string mid = cand["sdpMid"].get<std::string>();

            if (!node->remote_description_set) {
                safelogpost(x, PD_DEBUG, "[p2p~] Queuing ICE candidate from %s", from_peer.c_str());
                node->pending_remote_candidates.push_back({cand_str, mid});
            } else {
                try {
                    rtc::Candidate rtc_cand(cand_str, mid);
                    node->pc->addRemoteCandidate(rtc_cand);
                    safelogpost(x, PD_DEBUG, "[p2p~] Added ICE candidate from %s",
                                from_peer.c_str());
                } catch (const std::exception &e) {
                    safelogpost(x, PD_ERROR, "[p2p~] Failed to add candidate: %s", e.what());
                }
            }

        } else if (type == "peer-left") {
            P2PNode *node = find_node_by_peer(x, from_peer);
            if (node) {
                node->is_streaming = false;
                node->remote_description_set = false;
                if (node->dc) {
                    node->dc->close();
                    node->dc.reset();
                }
                if (node->audio_track) {
                    node->audio_track->close();
                    node->audio_track.reset();
                }
                if (node->pc) {
                    node->pc->close();
                    node->pc.reset();
                }
                node->remote_peer_id.clear();
                node->user.clear();
                node->pending_remote_candidates.clear();
            }
            x->peer_connected = count_active_nodes(x);
            clock_delay(x->report_clock, 0);

        } else if (type == "existing-peers") {
            auto peers = data["peers"];
            for (const auto &peer : peers) {
                std::string peer_id = peer["id"].get<std::string>();
                std::string peer_name = peer["name"].get<std::string>();
                P2PNode *node = find_free_node(x);
                if (node) {
                    node->user = peer_name;
                    node->remote_peer_id = peer_id;
                    node->ws = x->shared_ws;
                    node->is_polite = false;
                    setup_webrtc_for_node(x, node, true);
                    node->making_offer = true;
                    node->pc->setLocalDescription();
                    safelogpost(x, PD_NORMAL, "[p2p~] Connecting to existing peer %s (%s)",
                                peer_name.c_str(), peer_id.substr(0, 8).c_str());
                }
            }
            x->peer_connected = count_active_nodes(x);
            clock_delay(x->report_clock, 0);

        } else if (type == "welcome") {
            safelogpost(x, PD_NORMAL, "[p2p~] Connection ID: %s",
                        data["id"].get<std::string>().c_str());
        }
    });

    x->shared_ws->start();
    safelogpost(x, PD_NORMAL, "[p2p~] Connecting...");
}

// ─────────────────────────────────────
static void p2p_disconnect(p2p_tilde *x) {
    for (auto &node : x->nodes) {
        node->is_streaming = false;
        node->remote_description_set = false;
        if (node->dc) {
            node->dc->close();
            node->dc.reset();
        }
        if (node->audio_track) {
            node->audio_track->close();
            node->audio_track.reset();
        }
        if (node->pc) {
            node->pc->close();
            node->pc.reset();
        }
        node->remote_peer_id.clear();
        node->user.clear();
        node->pending_remote_candidates.clear();
    }

    if (x->shared_ws) {
        x->shared_ws->stop();
    }

    x->peer_connected = 0;
    clock_delay(x->report_clock, 0);
    safelogpost(x, PD_NORMAL, "[p2p~] Disconnected");
}

// ─────────────────────────────────────
static void p2p_stream(p2p_tilde *x, t_float f) {
    bool wants_stream = (f != 0);
    for (auto &node : x->nodes) {
        node->is_streaming = wants_stream;
    }
    safelogpost(x, PD_NORMAL, "Stream %s", wants_stream ? "active" : "paused");
}

// ─────────────────────────────────────
static void p2p_channel(p2p_tilde *x, t_symbol *user, t_float f) {
    if (f > x->max_channels || f < 1) {
        safelogpost(x, PD_ERROR, "Invalid channel. Valid channels are 1-%d.", x->max_channels);
        return;
    }
    if (!x->fixchannels) {
        safelogpost(x, PD_ERROR, "Create the object with '-f' to use setchannel");
        return;
    }

    x->peers_channels[user->s_name] = (int)f - 1;
    safelogpost(x, PD_NORMAL, "User: %s -> Ch: %d", user->s_name, (int)f);
}

// ─────────────────────────────────────
static void p2p_message(p2p_tilde *x, t_symbol *, int argc, t_atom *argv) {
    std::string text;
    for (int i = 0; i < argc; ++i) {
        if (argv[i].a_type == A_SYMBOL) {
            text += argv[i].a_w.w_symbol->s_name;
        } else if (argv[i].a_type == A_FLOAT) {
            text += std::to_string(argv[i].a_w.w_float);
        }
        if (i != argc - 1) {
            text += " ";
        }
    }
    for (auto &node : x->nodes) {
        if (node->dc && node->dc->isOpen()) {
            node->dc->send(text);
        }
    }
}

// ─────────────────────────────────────
static void p2p_report(p2p_tilde *x) {
    t_atom atoms[1];
    SETFLOAT(atoms, x->peer_connected);
    outlet_anything(x->out_msgs, gensym("peers"), 1, atoms);
    canvas_update_dsp();
}

// ─────────────────────────────────────
static t_int *p2p_tilde_perform(t_int *w) {
    auto *x = (p2p_tilde *)w[1];
    auto *in = (t_sample *)w[2];
    int n = (int)w[3];
    auto *out_base = (t_sample *)w[4];
    int num_chans = (int)w[5];

    if (x->multichannel) {
        for (auto &node : x->nodes) {
            if (node->is_streaming && !node->remote_peer_id.empty() && node->pc) {
                for (int i = 0; i < n; ++i) {
                    node->send_buffer.push(in[i]);
                }
            }
        }

        // Clear all output channels first
        for (int ch = 0; ch < num_chans; ch++) {
            memset(out_base + ch * n, 0, n * sizeof(t_sample));
        }

        if (x->fixchannels) {
            for (auto &node : x->nodes) {
                if (node->remote_peer_id.empty() || !node->pc) {
                    continue;
                }
                auto it = x->peers_channels.find(node->user);
                if (it == x->peers_channels.end()) {
                    float dummy;
                    while (node->receive_buffer.pop(dummy)) {
                    }
                    continue;
                }
                int ch = it->second;
                if (ch < 0 || ch >= num_chans) {
                    continue;
                }
                t_sample *out_ch = out_base + ch * n;
                for (int i = 0; i < n; ++i) {
                    float s = 0.f;
                    node->receive_buffer.pop(s);
                    out_ch[i] = s;
                }
            }
        } else {
            // Dynamic mode: assign channels sequentially in node order
            int ch = 0;
            for (auto &node : x->nodes) {
                if (node->remote_peer_id.empty() || !node->pc) {
                    continue;
                }
                if (ch >= num_chans) {
                    break;
                }
                t_sample *out_ch = out_base + ch * n;
                for (int i = 0; i < n; ++i) {
                    float s = 0.f;
                    node->receive_buffer.pop(s);
                    out_ch[i] = s;
                }
                ch++;
            }
        }

    } else {
        // Mono mix mode
        for (auto &node : x->nodes) {
            if (node->is_streaming && !node->remote_peer_id.empty() && node->pc) {
                for (int i = 0; i < n; ++i) {
                    node->send_buffer.push(in[i]);
                }
            }
        }
        for (int i = 0; i < n; ++i) {
            float mixed = 0.f;
            for (auto &node : x->nodes) {
                float s = 0.f;
                node->receive_buffer.pop(s);
                mixed += s;
            }
            out_base[i] = mixed;
        }
    }

    return (w + 7); // FIX: 6 args passed → must skip 6 + 1 = 7 words
}

// ─────────────────────────────────────
static void p2p_dsp(p2p_tilde *x, t_signal **sp) {
    int num_active = count_active_nodes(x);

    int num_outputs;
    if (x->multichannel) {
        num_outputs = x->fixchannels ? x->max_channels : (num_active > 0 ? num_active : 1);
    } else {
        num_outputs = 1;
    }
    signal_setmultiout(&sp[1], num_outputs);

    // FIX Bug 3: pass 6 args so perform can safely read w[1]..w[6]
    dsp_add(p2p_tilde_perform, 6,
            x,            // w[1]
            sp[0]->s_vec, // w[2]  input
            sp[0]->s_n,   // w[3]  block size
            sp[1]->s_vec, // w[4]  output base
            num_outputs,  // w[5]  channel count
            0             // w[6]  padding (unused)
    );
}

// ─────────────────────────────────────
static void *p2p_new(t_symbol *s, int argc, t_atom *argv) {
    p2p_tilde *x = (p2p_tilde *)pd_new(p2p_tilde_class);
    x->peer_connected = 0;
    x->multichannel = false;
    x->max_channels = 8;
    x->fixchannels = false;

    for (int i = 0; i < argc; i++) {
        if (argv[i].a_type == A_SYMBOL) {
            if (strcmp(argv[i].a_w.w_symbol->s_name, "-o") == 0) {
                x->multichannel = true;
            } else if (strcmp(argv[i].a_w.w_symbol->s_name, "-f") == 0) {
                x->fixchannels = true;
                x->multichannel = true; // -f implies multichannel
            } else {
                safelogpost(x, PD_NORMAL, "[p2p~] Unknown flag: %s", argv[i].a_w.w_symbol->s_name);
            }
        } else if (argv[i].a_type == A_FLOAT) {
            x->max_channels = (int)argv[i].a_w.w_float;
        }
    }

    x->nodes.reserve(x->max_channels);
    for (int i = 0; i < x->max_channels; i++) {
        auto node = std::make_unique<P2PNode>();
        node->channel_index = i;

        int err;
        node->opus_enc = opus_encoder_create(node->sample_rate, 1, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK) {
            safelogpost(x, PD_ERROR, "[p2p~] Opus encoder error for node %d: %d", i, err);
            return nullptr;
        }
        opus_encoder_ctl(node->opus_enc, OPUS_SET_BITRATE(OPUS_BITRATE_MAX));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_COMPLEXITY(10));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_PACKET_LOSS_PERC(0));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_INBAND_FEC(0));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_DTX(0));

        node->opus_dec = opus_decoder_create(node->sample_rate, 1, &err);
        if (err != OPUS_OK) {
            safelogpost(x, PD_ERROR, "[p2p~] Opus decoder error for node %d: %d", i, err);
        }

        node->tx_thread = std::thread([node_ptr = node.get()]() {
            const int FRAME_SIZE = 480;
            float pcm_frame[FRAME_SIZE];
            int collected = 0;
            unsigned char opus_payload[4000];
            uint16_t seq = 0;
            uint32_t rtp_timestamp = 0;
            const uint32_t ssrc = 12345;
            const uint8_t payload_type = 111;
            const uint32_t timestamp_increment = FRAME_SIZE;

            while (node_ptr->thread_running) {
                while (collected < FRAME_SIZE && node_ptr->send_buffer.pop(pcm_frame[collected])) {
                    collected++;
                }

                if (collected == FRAME_SIZE) {
                    if (node_ptr->is_streaming && node_ptr->audio_track &&
                        node_ptr->audio_track->isOpen()) {
                        int bytes = opus_encode_float(node_ptr->opus_enc, pcm_frame, FRAME_SIZE,
                                                      opus_payload, sizeof(opus_payload));
                        if (bytes > 0) {
                            rtc::binary rtp_packet(12 + bytes);
                            auto *p = reinterpret_cast<uint8_t *>(rtp_packet.data());
                            p[0] = 0x80;
                            p[1] = payload_type & 0x7F;
                            p[2] = (seq >> 8) & 0xFF;
                            p[3] = seq & 0xFF;
                            p[4] = (rtp_timestamp >> 24) & 0xFF;
                            p[5] = (rtp_timestamp >> 16) & 0xFF;
                            p[6] = (rtp_timestamp >> 8) & 0xFF;
                            p[7] = rtp_timestamp & 0xFF;
                            p[8] = (ssrc >> 24) & 0xFF;
                            p[9] = (ssrc >> 16) & 0xFF;
                            p[10] = (ssrc >> 8) & 0xFF;
                            p[11] = ssrc & 0xFF;
                            std::memcpy(p + 12, opus_payload, bytes);
                            seq++;
                            rtp_timestamp += timestamp_increment;
                            node_ptr->audio_track->send(rtp_packet);
                        }
                    }
                    collected = 0;
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            }
        });

        x->nodes.push_back(std::move(node));
    }

    x->out_signals = outlet_new(&x->x_obj, &s_signal);
    x->out_msgs = outlet_new(&x->x_obj, gensym("anything"));
    x->report_clock = clock_new(&x->x_obj, (t_method)p2p_report);
    x->peers_channels.reserve(x->max_channels);

    safelogpost(x, PD_NORMAL, "[p2p~] Created with %d channels, multichannel: %s, fixchannels: %s",
                x->max_channels, x->multichannel ? "on" : "off", x->fixchannels ? "on" : "off");

    return x;
}

// ─────────────────────────────────────
static void p2p_free(p2p_tilde *x) {
    for (auto &node : x->nodes) {
        node->thread_running = false;
        if (node->pc) {
            node->pc->close();
        }
    }
    if (x->shared_ws) {
        x->shared_ws->stop();
    }
    if (x->report_clock) {
        clock_free(x->report_clock);
    }
}

// ─────────────────────────────────────
extern "C" void p2p_tilde_setup(void) {
    ix::initNetSystem();

    p2p_tilde_class = class_new(gensym("p2p~"), (t_newmethod)p2p_new, (t_method)p2p_free,
                                sizeof(p2p_tilde), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(p2p_tilde_class, p2p_tilde, x_f);
    class_addmethod(p2p_tilde_class, (t_method)p2p_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_stream, gensym("stream"), A_FLOAT, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_connect, gensym("connect"), A_SYMBOL, A_SYMBOL,
                    A_SYMBOL, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_disconnect, gensym("disconnect"), A_NULL, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_message, gensym("message"), A_GIMME, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_channel, gensym("setchannel"), A_SYMBOL, A_FLOAT,
                    0);
}
