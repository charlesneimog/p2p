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
#include <random>
#include <cstdarg>
#include <cstdio>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <opus.h>

#include <boost/lockfree/spsc_queue.hpp>

using json = nlohmann::json;

static t_class *p2p_class;
static t_class *p2p_tilde_class;

// ─────────────────────────────────────
struct QueuedCandidate {
    std::string candidate;
    std::string mid;
};

// ─────────────────────────────────────
struct P2PNode {
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

    std::vector<std::function<void()>> pending_negotiations;

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
struct p2p_tilde_messdata {
    enum P2P_MESS {
        LOG,
        MESSAGE,
    };
    P2P_MESS type;
    std::string msg;
    std::string user;
    t_loglevel level;
};

// ─────────────────────────────────────
struct p2p_state {
    std::string local_peer_id;
    std::vector<std::unique_ptr<P2PNode>> nodes;
    std::shared_ptr<ix::WebSocket> shared_ws;
    std::unordered_map<std::string, int> peers_channels;
    std::string origin;
    std::string jsonkey;
};

// ─────────────────────────────────────
struct p2p_tilde {
    t_object x_obj;
    t_sample x_f;
    float peer_connected;

    bool json;
    bool wants_stream;
    bool multichannel;
    bool fixchannels;
    int max_out_channels;
    int max_in_channels;

    t_clock *report_clock;
    t_outlet *out_signals;
    t_outlet *out_msgs;

    p2p_state *state;
};

// ─────────────────────────────────────
static void p2p_tilde_mess(t_pd *obj, void *data) {
    p2p_tilde *x = (p2p_tilde *)obj;
    p2p_tilde_messdata *d = (p2p_tilde_messdata *)data;

    switch (d->type) {
    case p2p_tilde_messdata::LOG: {
        logpost(x, d->level, "[p2p~] %s", d->msg.c_str());
        break;
    }
    case p2p_tilde_messdata::MESSAGE: {
        t_atom o[2];
        SETSYMBOL(&o[0], gensym(d->user.c_str()));
        SETSYMBOL(&o[1], gensym(d->msg.c_str()));
        outlet_anything(x->out_msgs, gensym("json"), 2, o);
        break;
    }
    }

    delete d;
    return;
}

// ─────────────────────────────────────
static P2PNode *p2p_find_node_by_peer(p2p_tilde *x, const std::string &peer_id) {
    for (auto &node : x->state->nodes) {
        if (node->remote_peer_id == peer_id) {
            return node.get();
        }
    }
    return nullptr;
}

// ─────────────────────────────────────
static P2PNode *p2p_tilde_find_free_node(p2p_tilde *x) {
    for (auto &node : x->state->nodes) {
        if (node->remote_peer_id.empty() && !node->pc) {
            return node.get();
        }
    }
    return nullptr;
}

// ─────────────────────────────────────
static int p2p_count_active_nodes(p2p_tilde *x) {
    int count = 0;
    for (auto &node : x->state->nodes) {
        if (!node->remote_peer_id.empty() && node->pc) {
            count++;
        }
    }
    return count;
}

// ─────────────────────────────────────
inline void p2p_safelogpost(p2p_tilde *x, t_loglevel level, const char *fmt, ...) {
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
void p2p_request_stream_change(P2PNode *node, std::function<void()> action) {
    if (node->pc->signalingState() == rtc::PeerConnection::SignalingState::Stable) {
        action();
    } else {
        node->pending_negotiations.push_back(action);
    }
}

// ─────────────────────────────────────
static void p2p_setup_webrtc_for_node(p2p_tilde *x, P2PNode *node, bool is_caller) {
    rtc::Configuration config;
    config.enableIceTcp = true;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    config.disableAutoNegotiation = false; // We will manage when tracks are added.
    node->pc = std::make_shared<rtc::PeerConnection>(config);
    node->remote_description_set = false;

    node->pc->onSignalingStateChange([x, node](rtc::PeerConnection::SignalingState state) {
        p2p_safelogpost(x, PD_DEBUG, "Signaling State for peer %s: %d",
                        node->remote_peer_id.c_str(), state);

        if (state == rtc::PeerConnection::SignalingState::Stable) {
            node->making_offer = false;
            if (!node->pending_negotiations.empty()) {
                p2p_safelogpost(x, PD_DEBUG, "Flushing pending negotiations for %s",
                                node->remote_peer_id.c_str());
                auto action = node->pending_negotiations.front();
                node->pending_negotiations.erase(node->pending_negotiations.begin());
                action();
            }
        }
    });

    node->pc->onLocalDescription([x, node](rtc::Description description) {
        if (node->ws && node->ws->getReadyState() == ix::ReadyState::Open) {
            json msg = {
                {"type", description.typeString()},
                {"sdp", {{"type", description.typeString()}, {"sdp", std::string(description)}}},
                {"to", node->remote_peer_id}};
            node->ws->send(msg.dump());
        }
    });

    node->pc->onLocalCandidate([x, node](rtc::Candidate candidate) {
        if (node->pc->signalingState() == rtc::PeerConnection::SignalingState::HaveLocalOffer ||
            node->pc->signalingState() == rtc::PeerConnection::SignalingState::Stable) {

            if (node->ws && node->ws->getReadyState() == ix::ReadyState::Open) {
                json msg;
                msg["type"] = "ice-candidate";
                msg["candidate"]["candidate"] = candidate.candidate();
                msg["candidate"]["sdpMid"] = candidate.mid();
                msg["candidate"]["sdpMLineIndex"] = 0;
                msg["to"] = node->remote_peer_id;
                node->ws->send(msg.dump());
            }
        }
    });

    p2p_request_stream_change(node, [x, node]() {
        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(111);
        audio.setBitrate(64000);
        node->audio_track = node->pc->addTrack(audio);
        if (!node->audio_track) {
            p2p_safelogpost(x, PD_ERROR, "Failed to add audio track for peer %s",
                            node->remote_peer_id.c_str());
            return;
        }
        node->audio_track->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
        node->audio_track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
        node->audio_track->onFrame([x, node](rtc::binary data, rtc::FrameInfo) {
            if (!node->opus_dec) {
                return;
            }
            const int MAX_SAMPLES = 5760;
            float pcm[MAX_SAMPLES];
            int samples = opus_decode_float(node->opus_dec,
                                            reinterpret_cast<const unsigned char *>(data.data()),
                                            data.size(), pcm, MAX_SAMPLES, 0);
            if (samples > 0) {
                for (int i = 0; i < samples; i++) {
                    node->receive_buffer.push(pcm[i]);
                }
            }
        });

        node->pc->onTrack([x, node](std::shared_ptr<rtc::Track> track) {
            if (track->description().type() == "audio") {
                p2p_safelogpost(x, PD_NORMAL, "Remote audio track active for peer %s",
                                node->remote_peer_id.c_str());
            }
        });
    });

    // ─────────────────────────────────────
    if (is_caller) {
        std::shared_ptr<rtc::DataChannel> dc = node->pc->createDataChannel("data");
        node->dc = dc;
        dc->onOpen([x, node]() {
            p2p_safelogpost(x, PD_DEBUG, "DataChannel open with peer '%s'", node->user.c_str());
        });

        dc->onMessage([x, node](std::variant<rtc::binary, std::string> data) {
            std::string payload;
            if (std::holds_alternative<std::string>(data)) {
                payload = std::get<std::string>(data);
            } else {
                const auto &bin = std::get<rtc::binary>(data);
                payload.assign(reinterpret_cast<const char *>(bin.data()), bin.size());
            }
            auto *d = new p2p_tilde_messdata();
            d->type = p2p_tilde_messdata::MESSAGE;
            d->msg = payload;
            d->user = node->user;
            pd_queue_mess(&pd_maininstance, &x->x_obj.te_g.g_pd, d, p2p_tilde_mess);
        });
    } else {
        node->pc->onDataChannel([x, node](std::shared_ptr<rtc::DataChannel> dc) {
            node->dc = dc;
            dc->onOpen([x, node]() {
                p2p_safelogpost(x, PD_DEBUG, "DataChannel open with peer '%s'", node->user.c_str());
            });

            dc->onMessage([x, node](std::variant<rtc::binary, std::string> data) {
                std::string payload;
                if (std::holds_alternative<std::string>(data)) {
                    payload = std::get<std::string>(data);
                } else {
                    const auto &bin = std::get<rtc::binary>(data);
                    payload.assign(reinterpret_cast<const char *>(bin.data()), bin.size());
                }
                auto *d = new p2p_tilde_messdata();
                d->type = p2p_tilde_messdata::MESSAGE;
                d->msg = payload;
                d->user = node->user;
                pd_queue_mess(&pd_maininstance, &x->x_obj.te_g.g_pd, d, p2p_tilde_mess);
            });
        });
    }
}

// ─────────────────────────────────────
static void p2p_flush_pending_candidates(p2p_tilde *x, P2PNode *node) {
    for (const auto &qc : node->pending_remote_candidates) {
        try {
            rtc::Candidate rtc_cand(qc.candidate, qc.mid);
            node->pc->addRemoteCandidate(rtc_cand);
            p2p_safelogpost(x, PD_DEBUG, "Flushed candidate for peer %s",
                            node->remote_peer_id.c_str());
        } catch (const std::exception &e) {
            p2p_safelogpost(x, PD_ERROR, "Failed to add queued candidate: %s", e.what());
        }
    }
    node->pending_remote_candidates.clear();
}

// ─────────────────────────────────────
static void p2p_origin(p2p_tilde *x, t_symbol *s) {
    x->state->origin = s->s_name;
}

// ─────────────────────────────────────
static void p2p_stream(p2p_tilde *x, t_float f) {
    x->wants_stream = (f != 0);
    for (auto &node : x->state->nodes) {
        node->is_streaming = x->wants_stream;
    }
    p2p_safelogpost(x, PD_NORMAL, "Stream %s", x->wants_stream ? "active" : "paused");
}

// ─────────────────────────────────────
static void p2p_disconnect(p2p_tilde *x) {
    for (auto &node : x->state->nodes) {
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

    if (x->state->shared_ws) {
        x->state->shared_ws->stop();
    }

    x->peer_connected = 0;
    clock_delay(x->report_clock, 0);
    p2p_safelogpost(x, PD_NORMAL, "Disconnected");
}

// ─────────────────────────────────────
static void p2p_connect(p2p_tilde *x, t_symbol *wss, t_symbol *room, t_symbol *user) {
    if (!x->state->shared_ws) {
        x->state->shared_ws = std::make_shared<ix::WebSocket>();
    } else {
        p2p_disconnect(x);
    }

    std::string url = std::string(wss->s_name) + "/?room=" + std::string(room->s_name);
    x->state->shared_ws->setUrl(url);
    ix::WebSocketHttpHeaders headers;
    headers["Origin"] = x->state->origin;
    x->state->shared_ws->setExtraHeaders(headers);
    std::string username = std::string(user->s_name);

    x->state->shared_ws->setOnMessageCallback([x, username,
                                               room](const ix::WebSocketMessagePtr &msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            json join = {{"type", "join"}, {"name", username}};
            x->state->shared_ws->send(join.dump());
            p2p_safelogpost(x, PD_NORMAL, "Connected to the room: '%s'", room->s_name);
            return;
        }

        if (msg->type != ix::WebSocketMessageType::Message) {
            return;
        }

        json data;
        try {
            data = json::parse(msg->str);
        } catch (...) {
            return;
        }

        std::string type = data.contains("type") ? data["type"].get<std::string>() : "";
        std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";

        if (type == "peer-joined") {
            P2PNode *node = p2p_tilde_find_free_node(x);
            if (!node) {
                p2p_safelogpost(x, PD_ERROR, "No free nodes available for peer %s",
                                from_peer.c_str());
                return;
            }

            std::string peer_name = data.contains("peer") && data["peer"].contains("name")
                                        ? data["peer"]["name"].get<std::string>()
                                        : from_peer;
            node->user = peer_name;
            node->remote_peer_id = from_peer;
            node->ws = x->state->shared_ws;

            // ─── GLARE FIX: Deterministic Role Assignment ───
            bool should_be_caller = (x->state->local_peer_id < from_peer);
            node->is_polite = !should_be_caller;

            p2p_setup_webrtc_for_node(x, node, should_be_caller);
            x->peer_connected = p2p_count_active_nodes(x);
            clock_delay(x->report_clock, 0);

            if (should_be_caller) {
                node->making_offer = true;
                node->pc->setLocalDescription(); // Generates and sends offer
            } else {
                p2p_safelogpost(x, PD_NORMAL, "Waiting for offer from %s (I am callee)",
                                from_peer.c_str());
            }

        } else if (type == "offer") {
            P2PNode *node = p2p_find_node_by_peer(x, from_peer);
            if (!node) {
                node = p2p_tilde_find_free_node(x);
                if (!node) {
                    p2p_safelogpost(x, PD_ERROR, "No free nodes for offer from %s",
                                    from_peer.c_str());
                    return;
                }
                node->remote_peer_id = from_peer;
                node->ws = x->state->shared_ws;

                bool should_be_caller = (x->state->local_peer_id < from_peer);
                node->is_polite = !should_be_caller;
                p2p_setup_webrtc_for_node(x, node, should_be_caller);
            }

            // ─── GLARE FIX: Collision Resolution ───
            bool offer_collision =
                (node->making_offer ||
                 node->pc->signalingState() != rtc::PeerConnection::SignalingState::Stable);
            if (offer_collision && !node->is_polite) {
                p2p_safelogpost(x, PD_DEBUG, "Glare: ignoring offer from %s (impolite)",
                                from_peer.c_str());
                node->ignore_offer = true;
                return;
            }
            node->ignore_offer = false;

            std::string sdp_str;
            if (data["sdp"].is_object()) {
                sdp_str = data["sdp"]["sdp"].get<std::string>();
            } else if (data["sdp"].is_string()) {
                sdp_str = data["sdp"].get<std::string>();
            } else {
                p2p_safelogpost(x, PD_ERROR, "Invalid SDP format");
                return;
            }

            try {
                rtc::Description desc(sdp_str, "offer");
                node->pc->setRemoteDescription(std::move(desc));
                node->remote_description_set = true;
                p2p_flush_pending_candidates(x, node);

                // We are acting as Callee. Setting local description automatically generates the
                // Answer.
                node->making_offer = false;
                node->pc->setLocalDescription();
            } catch (const std::exception &e) {
                p2p_safelogpost(x, PD_ERROR, "Failed to set remote description (offer): %s",
                                e.what());
            }

        } else if (type == "answer") {
            P2PNode *node = p2p_find_node_by_peer(x, from_peer);
            if (!node) {
                return;
            }

            // CRITICAL FIX: Release the glare lock so future incoming offers are not falsely
            // ignored
            node->making_offer = false;

            std::string sdp_str;
            if (data["sdp"].is_object()) {
                sdp_str = data["sdp"]["sdp"].get<std::string>();
            } else if (data["sdp"].is_string()) {
                sdp_str = data["sdp"].get<std::string>();
            } else {
                return;
            }

            try {
                rtc::Description desc(sdp_str, "answer");
                node->pc->setRemoteDescription(std::move(desc));
                node->remote_description_set = true;
                p2p_flush_pending_candidates(x, node);
            } catch (const std::exception &e) {
                p2p_safelogpost(x, PD_ERROR, "Failed to set remote description (answer): %s",
                                e.what());
            }
            try {
                rtc::Description desc(sdp_str, "answer");
                node->pc->setRemoteDescription(std::move(desc));
                node->remote_description_set = true;
                p2p_flush_pending_candidates(x, node);
            } catch (const std::exception &e) {
                p2p_safelogpost(x, PD_ERROR, "Failed to set remote description (answer): %s",
                                e.what());
            }

        } else if (type == "ice-candidate") {
            P2PNode *node = p2p_find_node_by_peer(x, from_peer);
            if (!node) {
                return;
            }

            if (node->ignore_offer) {
                return; // Do not process candidates belonging to an ignored offer
            }

            std::string cand_str;
            std::string mid_str;
            if (data["candidate"].is_object()) {
                cand_str = data["candidate"]["candidate"].get<std::string>();
                mid_str = data["candidate"]["sdpMid"].get<std::string>();
            } else {
                return;
            }

            if (cand_str.empty()) {
                return;
            }

            if (!node->remote_description_set) {
                QueuedCandidate qc;
                qc.candidate = cand_str;
                qc.mid = mid_str;
                node->pending_remote_candidates.push_back(qc);
                p2p_safelogpost(x, PD_DEBUG, "Queuing ICE candidate from %s", from_peer.c_str());
            } else {
                try {
                    rtc::Candidate rtc_cand(cand_str, mid_str);
                    node->pc->addRemoteCandidate(rtc_cand);
                } catch (const std::exception &e) {
                    p2p_safelogpost(x, PD_ERROR, "Failed to add ICE candidate: %s", e.what());
                }
            }

        } else if (type == "peer-left") {
            P2PNode *node = p2p_find_node_by_peer(x, from_peer);
            if (node) {
                p2p_safelogpost(x, PD_NORMAL, "Peer '%s' left", node->user.c_str());
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
            x->peer_connected = p2p_count_active_nodes(x);
            clock_delay(x->report_clock, 0);

        } else if (type == "existing-peers") {
            auto peers = data["peers"];
            for (const auto &peer : peers) {
                std::string peer_id = peer["id"].get<std::string>();
                std::string peer_name = peer["name"].get<std::string>();
                P2PNode *node = p2p_tilde_find_free_node(x);
                if (node) {
                    node->user = peer_name;
                    node->remote_peer_id = peer_id;
                    node->ws = x->state->shared_ws;

                    // ─── GLARE FIX: Deterministic Role Assignment ───
                    bool should_be_caller = (x->state->local_peer_id < peer_id);
                    node->is_polite = !should_be_caller;

                    p2p_setup_webrtc_for_node(x, node, should_be_caller);

                    if (should_be_caller) {
                        node->making_offer = true;
                        node->pc->setLocalDescription();
                        p2p_safelogpost(x, PD_NORMAL, "Connecting to existing peer '%s' (%s)",
                                        peer_name.c_str(), peer_id.substr(0, 6).c_str());
                    } else {
                        p2p_safelogpost(x, PD_NORMAL,
                                        "Waiting for offer from existing peer '%s' (%s)",
                                        peer_name.c_str(), peer_id.substr(0, 6).c_str());
                    }
                }
            }
            x->peer_connected = p2p_count_active_nodes(x);
            clock_delay(x->report_clock, 0);

        } else if (type == "welcome") {
            p2p_safelogpost(x, PD_NORMAL, "Connection ID: %s",
                            data["id"].get<std::string>().substr(0, 6).c_str());
            x->state->local_peer_id =
                data["id"]
                    .get<std::string>(); // CRITICAL: Extracts our ID to execute the logic above
        }
    });

    x->state->shared_ws->start();
    p2p_safelogpost(x, PD_NORMAL, "Connecting...");

    if (x->wants_stream) {
        p2p_stream(x, 1);
    }
}

// ─────────────────────────────────────
static void p2p_channel(p2p_tilde *x, t_symbol *user, t_float f) {
    if (f > x->max_out_channels || f < 1) {
        p2p_safelogpost(x, PD_ERROR, "Invalid channel. Valid channels are 1-%d.",
                        x->max_out_channels);
        return;
    }
    if (!x->fixchannels) {
        p2p_safelogpost(x, PD_ERROR, "Create the object with '-f' to use setchannel");
        return;
    }

    x->state->peers_channels[user->s_name] = (int)f - 1;
    p2p_safelogpost(x, PD_NORMAL, "User: %s -> Ch: %d", user->s_name, (int)f);
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

    json payload;
    payload["type"] = "message";
    payload["text"] = text;

    std::string str = payload.dump(4);
    for (auto &node : x->state->nodes) {
        if (node->dc && node->dc->isOpen()) {
            node->dc->send(str);
        }
    }
}

// ─────────────────────────────────────
static void p2p_json(p2p_tilde *x, t_symbol *s, int argc, t_atom *argv) {
    if (argc == 0) {
        pd_error(x, "[p2p~] Message is empty");
        return;
    }

    try {
        post("mesage");
        t_symbol *json_str = atom_getsymbol(argv);
        json message = json::parse(json_str->s_name);
        if (message.contains(x->state->jsonkey)) {
            std::string value = message[x->state->jsonkey];
            std::vector<t_atom> atoms;
            char *buf = strdup(value.c_str());
            char *tok = strtok(buf, " ");
            while (tok) {
                t_atom a;
                char *endptr;
                double f = strtod(tok, &endptr);
                if (*endptr == '\0') {
                    SETFLOAT(&a, f);
                } else {
                    SETSYMBOL(&a, gensym(tok));
                }
                atoms.push_back(a);
                tok = strtok(nullptr, " ");
            }
            outlet_list(x->out_msgs, &s_list, static_cast<int>(atoms.size()), atoms.data());
            free(buf);
        }

    } catch (const json::parse_error &e) {
        p2p_safelogpost(x, PD_ERROR, "Invalid JSON: %s", e.what());
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
static t_int *p2p_perform(t_int *w) {
    auto *x = (p2p_tilde *)w[1];
    auto *in = (t_sample *)w[2];
    auto *out = (t_sample *)w[3];
    int n = (int)w[4];
    int num_chans = (int)w[5];

    if (x->multichannel) {
        for (auto &node : x->state->nodes) {
            if (node->is_streaming && !node->remote_peer_id.empty() && node->pc) {
                for (int i = 0; i < n; ++i) {
                    node->send_buffer.push(in[i]);
                }
            }
        }

        // Clear all output channels first
        for (int ch = 0; ch < num_chans; ch++) {
            memset(out + ch * n, 0, n * sizeof(t_sample));
        }

        if (x->fixchannels) {
            for (auto &node : x->state->nodes) {
                if (node->remote_peer_id.empty() || !node->pc) {
                    continue;
                }
                auto it = x->state->peers_channels.find(node->user);
                if (it == x->state->peers_channels.end()) {
                    float dummy;
                    while (node->receive_buffer.pop(dummy)) {
                    }
                    continue;
                }
                int ch = it->second;
                if (ch < 0 || ch >= num_chans) {
                    continue;
                }
                t_sample *out_ch = out + ch * n;
                for (int i = 0; i < n; ++i) {
                    float s = 0.f;
                    node->receive_buffer.pop(s);
                    out_ch[i] = s;
                }
            }
        } else {
            int ch = 0;
            for (auto &node : x->state->nodes) {
                if (node->remote_peer_id.empty() || !node->pc) {
                    continue;
                }
                if (ch >= num_chans) {
                    break;
                }
                t_sample *out_ch = out + ch * n;
                for (int i = 0; i < n; ++i) {
                    float s = 0.f;
                    node->receive_buffer.pop(s);
                    out_ch[i] = s;
                }
                ch++;
            }
        }

    } else {
        for (auto &node : x->state->nodes) {
            if (node->is_streaming && !node->remote_peer_id.empty() && node->pc) {
                for (int i = 0; i < n; ++i) {
                    node->send_buffer.push(in[i]);
                }
            }
        }
        for (int i = 0; i < n; ++i) {
            float mixed = 0.f;
            for (auto &node : x->state->nodes) {
                float s = 0.f;
                node->receive_buffer.pop(s);
                mixed += s;
            }
            out[i] = mixed;
        }
    }

    return (w + 6);
}

// ─────────────────────────────────────
static void p2p_dsp(p2p_tilde *x, t_signal **sp) {
    if (x->json) {
        return;
    }

    int num_active = p2p_count_active_nodes(x);
    int num_outputs;
    if (x->multichannel) {
        num_outputs = x->fixchannels ? x->max_out_channels : (num_active > 0 ? num_active : 1);
    } else {
        num_outputs = 1;
    }
    signal_setmultiout(&sp[1], num_outputs);
    dsp_add(p2p_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n, num_outputs);
}

// ─────────────────────────────────────
static void *p2p_new(t_symbol *s, int argc, t_atom *argv) {
    p2p_tilde *x = (p2p_tilde *)pd_new(p2p_tilde_class);
    x->peer_connected = 0;
    x->multichannel = false;
    x->max_out_channels = 8;
    x->fixchannels = false;
    x->json = false;
    x->state = new p2p_state();

    bool user_had_other_flags = false;
    for (int i = 0; i < argc; i++) {
        if (argv[i].a_type == A_SYMBOL) {
            const char *flag = atom_getsymbol(argv + i)->s_name;
            if (strcmp(flag, "-o") == 0 && i + 1 < argc) {
                x->multichannel = true;
                x->max_out_channels = atom_getfloat(argv + i + 1);
                user_had_other_flags = true;
            } else if (strcmp(flag, "-i") == 0 && i + 1 < argc) {
                x->max_in_channels = atom_getfloat(argv + i + 1);
                user_had_other_flags = true;
            } else if (strcmp(flag, "-f") == 0) {
                x->fixchannels = true;
                x->multichannel = true;
                user_had_other_flags = true;
            } else if (strcmp(flag, "-json") == 0) {
                if (i + 1 >= argc) {
                    pd_error(x, "[p2p~] -json requires a key for processing");
                    return nullptr;
                }

                if (user_had_other_flags) {
                    pd_error(x, "[p2p~] Ignoring other flags");
                }
                x->json = true;
                x->out_msgs = outlet_new(&x->x_obj, gensym("anything"));
                x->state->jsonkey = atom_getsymbol(argv + i + 1)->s_name;
                return x;

            } else {
                p2p_safelogpost(x, PD_ERROR, "Unknown flag: %s", argv[i].a_w.w_symbol->s_name);
            }
        }
    }

    x->state->nodes.reserve(x->max_out_channels);
    for (int i = 0; i < x->max_out_channels; i++) {
        auto node = std::make_unique<P2PNode>();
        node->channel_index = i;

        int err;
        node->opus_enc = opus_encoder_create(node->sample_rate, 1, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK) {
            p2p_safelogpost(x, PD_ERROR, "Opus encoder error for node %d: %d", i, err);
            return nullptr;
        }
        opus_encoder_ctl(node->opus_enc, OPUS_SET_APPLICATION(OPUS_APPLICATION_AUDIO));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

        opus_encoder_ctl(node->opus_enc, OPUS_SET_BITRATE(192000)); // or 256000 stereo
        opus_encoder_ctl(node->opus_enc, OPUS_SET_VBR(1));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_VBR_CONSTRAINT(1));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_COMPLEXITY(10));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_INBAND_FEC(0));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_DTX(0));

        node->opus_dec = opus_decoder_create(node->sample_rate, 1, &err);
        if (err != OPUS_OK) {
            p2p_safelogpost(x, PD_ERROR, "Opus decoder error for node %d: %d", i, err);
        }

        node->tx_thread = std::thread([node_ptr = node.get()]() {
            const int FRAME_SIZE = 480;
            float pcm_frame[FRAME_SIZE];
            int collected = 0;
            unsigned char opus_payload[4000];
            uint16_t seq = 0;
            uint32_t rtp_timestamp = 0;
            std::random_device rd;
            uint32_t ssrc = rd();
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

        x->state->nodes.push_back(std::move(node));
    }
    if (x->max_out_channels < 1 || x->max_out_channels > 1000) {
        pd_error(x, "[p2p~] Min for output is 1 and max is 1000");
        x->max_out_channels = 8;
    }

    post("[p2p~] Max output channels: %d", x->max_out_channels);
    x->out_signals = outlet_new(&x->x_obj, &s_signal);
    x->out_msgs = outlet_new(&x->x_obj, gensym("anything"));
    x->report_clock = clock_new(&x->x_obj, (t_method)p2p_report);
    x->state->peers_channels.reserve(x->max_out_channels);
    return x;
}

// ─────────────────────────────────────
static void p2p_free(p2p_tilde *x) {
    for (auto &node : x->state->nodes) {
        node->thread_running = false;
        if (node->pc) {
            node->pc->close();
        }
    }
    if (x->state->shared_ws) {
        x->state->shared_ws->stop();
    }
    if (x->report_clock) {
        clock_free(x->report_clock);
    }
    delete x->state;
}

// ─────────────────────────────────────
extern "C" void p2p_tilde_setup(void) {
    post("[p2p~] by Charles K. Neimog %d.%d.%d", 0, 1, 0);
    ix::initNetSystem();

    p2p_tilde_class = class_new(gensym("p2p~"), (t_newmethod)p2p_new, (t_method)p2p_free,
                                sizeof(p2p_tilde), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(p2p_tilde_class, p2p_tilde, x_f);
    class_addmethod(p2p_tilde_class, (t_method)p2p_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_stream, gensym("stream"), A_FLOAT, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_connect, gensym("connect"), A_SYMBOL, A_SYMBOL,
                    A_SYMBOL, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_disconnect, gensym("disconnect"), A_NULL, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_origin, gensym("origin"), A_SYMBOL, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_message, gensym("message"), A_GIMME, 0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_channel, gensym("setchannel"), A_SYMBOL, A_FLOAT,
                    0);
    class_addmethod(p2p_tilde_class, (t_method)p2p_json, gensym("json"), A_GIMME, 0);
}
