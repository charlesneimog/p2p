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

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <opus.h>

#include <spdlog/spdlog.h>

#include <boost/lockfree/spsc_queue.hpp>

#include <ext.h>
#include <ext_buffer.h>
#include <ext_obex.h>
#include <z_dsp.h>

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
    bool answering_offer{false};
    bool local_offer_sent{false};
    bool polite_media_offer_sent{false};

    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> audio_track;

    // Audio buffers
    boost::lockfree::spsc_queue<float, boost::lockfree::capacity<16384>> send_buffer;
    boost::lockfree::spsc_queue<float, boost::lockfree::capacity<16384>> receive_buffer;
    uint32_t audio_ssrc = 0;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp_config;

    // Audio codec
    OpusEncoder *opus_enc = nullptr;
    OpusDecoder *opus_dec = nullptr;
    int sample_rate = 48000;
    int frame_size = 480;

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
    std::shared_ptr<rtc::WebSocket> shared_ws;
    std::unordered_map<std::string, int> peers_channels;
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
    int frame_size;

    t_clock *report_clock;
    t_outlet *out_signals;
    t_outlet *out_msgs;

    // json
    t_outlet **out_json_keys;
    char **json_keys;
    int json_keys_count;

    rtc::Description::Direction direction;
    p2p_state *state;
    t_symbol *username;
    t_symbol *room;
    char *jsonkey;
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
static void p2p_setup_webrtc_for_node(p2p_tilde *x, P2PNode *node) {
    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    node->pc = std::make_shared<rtc::PeerConnection>(config);
    node->remote_description_set = false;
    p2p_safelogpost(x, PD_DEBUG, "Pd is polite=%d", node->is_polite);
    node->pc->onLocalDescription([x, node](rtc::Description description) {
        const std::string type = description.typeString();
        p2p_safelogpost(x, PD_DEBUG, "onLocalDescription %s", type.c_str());

        if (type == "offer") {
            const bool post_answer_offer = !node->making_offer && node->remote_description_set;
            if (post_answer_offer) {
                if (!node->is_polite || node->polite_media_offer_sent) {
                    p2p_safelogpost(x, PD_DEBUG, "Suppressing follow-up local offer for peer '%s'",
                                    node->user.c_str());
                    return;
                }
                node->polite_media_offer_sent = true;
                p2p_safelogpost(x, PD_DEBUG, "Sending one polite media update offer for peer '%s'",
                                node->user.c_str());
            }
            node->making_offer = false;
            node->local_offer_sent = true;
        } else if (type == "answer") {
            node->answering_offer = false;
        }

        if (node->ws) {
            json msg = {{"type", type},
                        {"sdp", {{"type", type}, {"sdp", std::string(description)}}},
                        {"to", node->remote_peer_id}};
            node->ws->send(msg.dump());
        } else {
            p2p_safelogpost(x, PD_ERROR, "Error: WebSocket missing onLocalDescription");
        }
    });

    auto install_sendrecv_handler = [node](std::shared_ptr<rtc::Track> track) {
        node->rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(node->audio_ssrc, "audio",
                                                                         109, node->sample_rate);
        auto handler = std::make_shared<rtc::OpusRtpPacketizer>(node->rtp_config);
        handler->addToChain(std::make_shared<rtc::OpusRtpDepacketizer>());
        handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        handler->addToChain(std::make_shared<rtc::RtcpSrReporter>(node->rtp_config));
        track->setMediaHandler(handler);
        node->audio_track = track;
    };

    // Offerer creates the audio m-line.
    if (!node->is_polite) {
        rtc::Description::Audio audio("audio", x->direction);
        audio.addOpusCodec(109);
        audio.addSSRC(node->audio_ssrc, "audio");
        node->audio_track = node->pc->addTrack(audio);
        install_sendrecv_handler(node->audio_track);
        node->audio_track->onFrame([x, node](rtc::binary data, rtc::FrameInfo info) {
            if (!node->opus_dec) {
                p2p_safelogpost(x, PD_ERROR, "Opus decode not initialized");
                return;
            }

            constexpr int MAX_SAMPLES = 5760;
            float pcm[MAX_SAMPLES];

            int samples = opus_decode_float(
                node->opus_dec, reinterpret_cast<const unsigned char *>(data.data()),
                static_cast<opus_int32>(data.size()), pcm, MAX_SAMPLES, 0);

            if (samples > 0) {
                for (int i = 0; i < samples; i++) {
                    node->receive_buffer.push(pcm[i]);
                }
            }

            if (samples < 0) {
                p2p_safelogpost(x, PD_ERROR, "Opus decode failed: %d, bytes=%zu", samples,
                                data.size());
                return;
            }
        });
    }

    node->pc->onTrack([x, node, install_sendrecv_handler](std::shared_ptr<rtc::Track> track) {
        p2p_safelogpost(x, PD_NORMAL, "Remote audio track active for peer %s",
                        node->remote_peer_id.c_str());

        if (node->is_polite) {
            auto desc = track->description();
            desc.addSSRC(node->audio_ssrc, "audio");
            track->setDescription(desc);

            install_sendrecv_handler(track);

        } else {
            auto handler = std::make_shared<rtc::OpusRtpDepacketizer>();
            handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
            track->setMediaHandler(handler);
        }

        track->onFrame([x, node](rtc::binary data, rtc::FrameInfo info) {
            if (!node->opus_dec) {
                p2p_safelogpost(x, PD_ERROR, "Opus decode not initialized");
                return;
            }

            constexpr int MAX_SAMPLES = 5760;
            float pcm[MAX_SAMPLES];

            int samples = opus_decode_float(
                node->opus_dec, reinterpret_cast<const unsigned char *>(data.data()),
                static_cast<opus_int32>(data.size()), pcm, MAX_SAMPLES, 0);

            if (samples > 0) {
                for (int i = 0; i < samples; i++) {
                    node->receive_buffer.push(pcm[i]);
                }
            }

            if (samples < 0) {
                p2p_safelogpost(x, PD_ERROR, "Opus decode failed: %d, bytes=%zu", samples,
                                data.size());
                return;
            }
        });
    });

    if (!node->is_polite) {
        auto dc = node->pc->createDataChannel("data");
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
static void p2p_stream(p2p_tilde *x, t_float f) {
    if (f != x->wants_stream) {
        p2p_safelogpost(x, PD_NORMAL, "Stream %s", f ? "active" : "paused");
    }
    x->wants_stream = (f != 0);
    for (auto &node : x->state->nodes) {
        node->is_streaming = x->wants_stream;
    }
}

// ─────────────────────────────────────
static void p2p_disconnect(p2p_tilde *x) {
    for (auto &node : x->state->nodes) {
        node->is_streaming = false;
        node->remote_description_set = false;
        node->making_offer = false;
        node->ignore_offer = false;
        node->answering_offer = false;
        node->local_offer_sent = false;
        node->polite_media_offer_sent = false;
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
        x->state->shared_ws->close();
    }

    x->peer_connected = 0;
    clock_delay(x->report_clock, 0);
    p2p_safelogpost(x, PD_NORMAL, "Disconnected");
}

// ─────────────────────────────────────
static void p2p_peer_join(p2p_tilde *x, json data) {
    P2PNode *node = p2p_tilde_find_free_node(x);
    std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";

    if (!node) {
        p2p_safelogpost(x, PD_ERROR, "No free nodes available for peer %s", from_peer.c_str());
        return;
    }

    std::string peer_name = data.contains("peer") && data["peer"].contains("name")
                                ? data["peer"]["name"].get<std::string>()
                                : from_peer;
    node->user = peer_name;
    node->remote_peer_id = from_peer;
    node->ws = x->state->shared_ws;

    bool should_be_caller = (x->state->local_peer_id < from_peer);
    node->is_polite = !should_be_caller;
    node->is_streaming = x->wants_stream;
    node->making_offer = should_be_caller;
    p2p_setup_webrtc_for_node(x, node);
    x->peer_connected = p2p_count_active_nodes(x);
    clock_delay(x->report_clock, 0);

    if (should_be_caller) {
        if (!node->local_offer_sent) {
            node->pc->setLocalDescription(); // Generates and sends offer
        }
    } else {
        p2p_safelogpost(x, PD_NORMAL, "Waiting for offer from %s (I am callee)", from_peer.c_str());
    }

    p2p_safelogpost(x, PD_NORMAL, "Peer '%s' joined", node->user.c_str());
}

// ─────────────────────────────────────
static void p2p_offer(p2p_tilde *x, json data) {
    std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    P2PNode *node = p2p_find_node_by_peer(x, from_peer);

    if (!node) {
        node = p2p_tilde_find_free_node(x);
        if (!node) {
            p2p_safelogpost(x, PD_ERROR, "No more available nodes for %s", from_peer.c_str());
            return;
        }
        node->remote_peer_id = from_peer;
        node->ws = x->state->shared_ws;
        node->user = from_peer;
        bool should_be_caller = (x->state->local_peer_id < from_peer);
        node->is_polite = !should_be_caller;
        node->making_offer = should_be_caller;
        p2p_setup_webrtc_for_node(x, node);
    }

    // ─── GLARE FIX: Collision Resolution ───
    bool offer_collision = (node->making_offer || node->pc->signalingState() !=
                                                      rtc::PeerConnection::SignalingState::Stable);
    if (offer_collision && !node->is_polite) {
        p2p_safelogpost(x, PD_DEBUG, "Glare: ignoring offer from %s (impolite)", from_peer.c_str());
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

        node->making_offer = false;
        node->answering_offer = true;

        node->pc->setLocalDescription();
    } catch (const std::exception &e) {
        node->answering_offer = false;
        p2p_safelogpost(x, PD_ERROR, "Failed to set remote description (offer): %s", e.what());
    }
}

// ─────────────────────────────────────
static void p2p_existing_peers(p2p_tilde *x, json data) {
    auto peers = data["peers"];
    for (const auto &peer : peers) {
        std::string peer_id = peer["id"].get<std::string>();
        std::string peer_name = peer["name"].get<std::string>();
        P2PNode *node = p2p_tilde_find_free_node(x);
        if (node) {
            node->user = peer_name;
            node->remote_peer_id = peer_id;
            node->ws = x->state->shared_ws;
            bool should_be_caller = (x->state->local_peer_id < peer_id);
            node->is_polite = !should_be_caller;
            node->making_offer = should_be_caller;
            p2p_setup_webrtc_for_node(x, node);
            if (should_be_caller && !node->local_offer_sent) {
                node->pc->setLocalDescription();
                p2p_safelogpost(x, PD_NORMAL, "Connecting to existing peer '%s' (%s)",
                                peer_name.c_str(), peer_id.substr(0, 6).c_str());
            } else if (should_be_caller) {
                p2p_safelogpost(x, PD_NORMAL, "Connecting to existing peer '%s' (%s)",
                                peer_name.c_str(), peer_id.substr(0, 6).c_str());
            } else {
                p2p_safelogpost(x, PD_NORMAL, "Waiting for offer from existing peer '%s' (%s)",
                                peer_name.c_str(), peer_id.substr(0, 6).c_str());
            }
        } else {
            p2p_safelogpost(x, PD_ERROR, "No more available nodes for %s", peer_name.c_str());
        }
    }
    x->peer_connected = p2p_count_active_nodes(x);
    clock_delay(x->report_clock, 0);
}

// ─────────────────────────────────────
static void p2p_answer(p2p_tilde *x, json data) {
    std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    P2PNode *node = p2p_find_node_by_peer(x, from_peer);

    if (!node || !node->pc) {
        return;
    }

    if (node->pc->signalingState() != rtc::PeerConnection::SignalingState::HaveLocalOffer) {
        p2p_safelogpost(x, PD_DEBUG,
                        "Ignoring unexpected answer from %s; signaling state is not HaveLocalOffer",
                        from_peer.c_str());
        return;
    }

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

        node->making_offer = false;
        node->remote_description_set = true;
        node->ignore_offer = false;

        p2p_flush_pending_candidates(x, node);
    } catch (const std::exception &e) {
        p2p_safelogpost(x, PD_ERROR, "Failed to set remote description (answer): %s", e.what());
    }
}

// ─────────────────────────────────────
static void p2p_icecantidate(p2p_tilde *x, json data) {
    std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    P2PNode *node = p2p_find_node_by_peer(x, from_peer);

    if (!node || !node->pc || node->ignore_offer) {
        return;
    }

    if (!data.contains("candidate") || !data["candidate"].is_object()) {
        return;
    }

    auto c = data["candidate"];

    if (!c.contains("candidate") || !c.contains("sdpMid")) {
        return;
    }

    std::string cand_str = c["candidate"].get<std::string>();
    std::string mid_str = c["sdpMid"].get<std::string>();

    if (cand_str.empty() || mid_str.empty()) {
        return;
    }

    if (!node->remote_description_set ||
        node->pc->signalingState() == rtc::PeerConnection::SignalingState::HaveLocalOffer) {
        node->pending_remote_candidates.push_back({cand_str, mid_str});
        p2p_safelogpost(x, PD_DEBUG, "Queuing ICE candidate from %s, mid=%s", from_peer.c_str(),
                        mid_str.c_str());
        return;
    }

    try {
        rtc::Candidate rtc_cand(cand_str, mid_str);
        node->pc->addRemoteCandidate(rtc_cand);
    } catch (const std::exception &e) {
        p2p_safelogpost(x, PD_ERROR, "Failed to add ICE candidate mid=%s: %s", mid_str.c_str(),
                        e.what());
    }
}

// ─────────────────────────────────────
static void p2p_peerleft(p2p_tilde *x, json data) {
    std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    P2PNode *node = p2p_find_node_by_peer(x, from_peer);
    if (node) {
        p2p_safelogpost(x, PD_NORMAL, "Peer '%s' left", node->user.c_str());
        node->is_streaming = false;
        node->remote_description_set = false;
        node->making_offer = false;
        node->ignore_offer = false;
        node->answering_offer = false;
        node->local_offer_sent = false;
        node->polite_media_offer_sent = false;
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
}

// ─────────────────────────────────────
static void p2p_welcome(p2p_tilde *x, json data) {
    std::string Id = data["id"].get<std::string>();
    std::string miniId = Id.substr(0, 6);
    p2p_safelogpost(x, PD_NORMAL, "Connected ID: %s", miniId.c_str());
    x->state->local_peer_id = data["id"].get<std::string>();
}

// ─────────────────────────────────────
static void p2p_onmessage_callback(p2p_tilde *x) {
    x->state->shared_ws->onMessage([x](std::variant<rtc::binary, std::string> data) {
        std::string payload;
        if (std::holds_alternative<std::string>(data)) {
            payload = std::get<std::string>(data);
        } else {
            const auto &bin = std::get<rtc::binary>(data);
            payload.assign(reinterpret_cast<const char *>(bin.data()), bin.size());
        }

        json json_data = json::parse(payload);

        // p2p_safelogpost(x, PD_NORMAL, "%s", json_data.dump(4).c_str());
        spdlog::info("{}", json_data.dump(4));
        std::string type = json_data.contains("type") ? json_data["type"].get<std::string>() : "";
        if (type == "welcome") {
            p2p_welcome(x, json_data);
        } else if (type == "peer-joined") {
            p2p_peer_join(x, json_data);
        } else if (type == "existing-peers") {
            p2p_existing_peers(x, json_data);
        } else if (type == "offer") {
            p2p_offer(x, json_data);
        } else if (type == "ice-candidate") {
            p2p_icecantidate(x, json_data);
        } else if (type == "answer") {
            p2p_answer(x, json_data);
        } else if (type == "peer-left") {
            p2p_peerleft(x, json_data);
        } else {
            p2p_safelogpost(x, PD_ERROR, "%s", payload.c_str());
        }
    });
}

// ─────────────────────────────────────
static void p2p_connect(p2p_tilde *x, t_symbol *wss, t_symbol *room, t_symbol *user) {
    if (!x->state->shared_ws) {
        rtc::WebSocket::Configuration config;
        config.connectionTimeout = std::chrono::milliseconds(1500);
        x->state->shared_ws = std::make_shared<rtc::WebSocket>(config);
    }

    if (x->state->shared_ws->isOpen()) {
        p2p_safelogpost(x, PD_ERROR, "Already Connected");
        return;
    }

    std::string username = std::string(user->s_name);
    x->username = user;
    x->room = room;
    std::string url = std::string(wss->s_name) + "/?room=" + std::string(room->s_name);
    x->state->shared_ws->open(url);
    x->state->shared_ws->onOpen([x, username, room]() {
        json join = {{"type", "join"}, {"name", username}};
        x->state->shared_ws->send(join.dump());
        p2p_safelogpost(x, PD_NORMAL, "Connected to the room: '%s'", room->s_name);
    });
    p2p_onmessage_callback(x);
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
    if (argc == 0 && (strcmp("json", s->s_name) == 0)) {
        pd_error(x, "[p2p~] Message is empty");
        return;
    }

    t_symbol *json_str;
    if (strcmp("json", s->s_name) == 0) {
        json_str = atom_getsymbol(argv);
    } else {
        json_str = s;
    }

    try {
        json message = json::parse(json_str->s_name);
        if (message.contains(x->jsonkey)) {
            std::string value = message[x->jsonkey];
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
static void p2p_json_symbol(p2p_tilde *x, t_symbol *s) {
    t_atom atoms[1];
    SETSYMBOL(atoms, s);
    p2p_json(x, gensym("json"), 1, atoms);
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

    if (!x->wants_stream) {
        // Clear all output channels first
        for (int ch = 0; ch < num_chans; ch++) {
            memset(out + ch * n, 0, n * sizeof(t_sample));
        }
        return (w + 6);
    }

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
    p2p_tilde *x;

    if (strcmp(s->s_name, "p2p") == 0) {
        x = (p2p_tilde *)pd_new(p2p_class);
        x->json_keys_count = argc;
        x->out_json_keys = (t_outlet **)getbytes(argc * sizeof(*x->out_json_keys));
        x->json_keys = (char **)getbytes(argc * sizeof(*x->json_keys));
        if (!x->out_json_keys || !x->json_keys) {
            if (x->out_json_keys) {
                freebytes(x->out_json_keys, argc * sizeof(*x->out_json_keys));
            }
            if (x->json_keys) {
                freebytes(x->json_keys, argc * sizeof(*x->json_keys));
            }
            x->out_json_keys = NULL;
            x->json_keys = NULL;
            x->json_keys_count = 0;
            pd_error(x, "could not allocate memory for p2p keys");
            return NULL;
        }

        for (int i = 0; i < argc; i++) {
            x->out_json_keys[i] = outlet_new(&x->x_obj, &s_anything);
            if (argv[i].a_type == A_SYMBOL) {
                x->json_keys[i] = strdup(atom_getsymbol(argv + i)->s_name);
            } else {
                x->json_keys[i] = NULL;
            }
        }

        return x;
    }

    x = (p2p_tilde *)pd_new(p2p_tilde_class);
    x->peer_connected = 0;
    x->multichannel = false;
    x->max_out_channels = 8;
    x->fixchannels = false;
    x->json = false;
    x->frame_size = 480;
    x->state = new p2p_state();
    x->direction = rtc::Description::Direction::SendRecv;
    spdlog::set_level(spdlog::level::debug);

    if (sys_getsr() != 48000) {
        pd_error(x, "[p2p~] Expects sampleRate of 48000Hz");
    }

    bool user_had_other_flags = false;
    for (int i = 0; i < argc; i++) {
        if (argv[i].a_type == A_SYMBOL) {
            const char *flag = atom_getsymbol(argv + i)->s_name;
            if (strcmp(flag, "-o") == 0 && i + 1 < argc) {
                x->multichannel = true;
                x->max_out_channels = atom_getfloat(argv + i + 1);
                user_had_other_flags = true;
                i++;
            } else if (strcmp(flag, "-i") == 0 && i + 1 < argc) {
                x->max_in_channels = atom_getfloat(argv + i + 1);
                user_had_other_flags = true;
                i++;
            } else if (strcmp(flag, "-f") == 0) {
                x->fixchannels = true;
                x->multichannel = true;
                user_had_other_flags = true;
            } else if (strcmp(flag, "-b") == 0) {
                user_had_other_flags = true;
                x->frame_size = atom_getfloat(argv + i + 1);
                i++;
            } else if (strcmp(flag, "-sr") == 0) {
                x->direction = rtc::Description::Direction::SendRecv;
                p2p_safelogpost(x, PD_NORMAL, "Send and Receive Audio");
            } else if (strcmp(flag, "-s") == 0) {
                x->direction = rtc::Description::Direction::SendOnly;
                p2p_safelogpost(x, PD_NORMAL, "Only Send Audio");
            } else if (strcmp(flag, "-r") == 0) {
                x->direction = rtc::Description::Direction::RecvOnly;
                p2p_safelogpost(x, PD_NORMAL, "Only Receive Audio");
            } else {
                p2p_safelogpost(x, PD_ERROR, "Unknown flag: %s", argv[i].a_w.w_symbol->s_name);
            }
        }
    }

    x->state->nodes.reserve(x->max_out_channels);
    for (int i = 0; i < x->max_out_channels; i++) {
        auto node = std::make_unique<P2PNode>();
        node->channel_index = i;
        node->frame_size = x->frame_size;

        int err;
        node->opus_enc = opus_encoder_create(node->sample_rate, 1, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK) {
            p2p_safelogpost(x, PD_ERROR, "Opus encoder error for node %d: %d", i, err);
            return nullptr;
        }
        opus_encoder_ctl(node->opus_enc, OPUS_SET_APPLICATION(OPUS_APPLICATION_AUDIO));
        opus_encoder_ctl(node->opus_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

        opus_encoder_ctl(node->opus_enc, OPUS_SET_BITRATE(256000));
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
            constexpr int MAX_OPUS_BYTES = 4000;
            unsigned char opus_payload[MAX_OPUS_BYTES];
            int FRAME_SIZE = node_ptr->frame_size; // 10 ms at 48 kHz
            float pcm_frame[FRAME_SIZE];
            int collected = 0;

            while (node_ptr->thread_running) {
                while (collected < FRAME_SIZE && node_ptr->send_buffer.pop(pcm_frame[collected])) {
                    collected++;
                }

                if (collected < FRAME_SIZE) {
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                    continue;
                }

                collected = 0;

                if (!node_ptr->is_streaming || !node_ptr->audio_track ||
                    !node_ptr->audio_track->isOpen() || !node_ptr->rtp_config ||
                    !node_ptr->opus_enc) {
                    continue;
                }

                int bytes = opus_encode_float(node_ptr->opus_enc, pcm_frame, FRAME_SIZE,
                                              opus_payload, sizeof(opus_payload));

                if (bytes <= 0) {
                    continue;
                }

                node_ptr->audio_track->send(reinterpret_cast<const std::byte *>(opus_payload),
                                            static_cast<size_t>(bytes));
                node_ptr->rtp_config->timestamp += FRAME_SIZE;
            }
        });

        x->state->nodes.push_back(std::move(node));
    }

    if (x->max_out_channels < 1 || x->max_out_channels > 1000) {
        pd_error(x, "[p2p~] Min for output is 1 and max is 1000");
        x->max_out_channels = 8;
    }

    x->out_signals = outlet_new(&x->x_obj, &s_signal);
    x->out_msgs = outlet_new(&x->x_obj, gensym("anything"));
    x->report_clock = clock_new(&x->x_obj, (t_method)p2p_report);
    x->state->peers_channels.reserve(x->max_out_channels);
    return x;
}

// ─────────────────────────────────────
static void p2p_free(p2p_tilde *x) {
    if (!x) {
        return;
    }

    if (x->state) {
        for (auto &node : x->state->nodes) {
            if (!node) {
                continue;
            }
            node->thread_running = false;
            if (node->pc) {
                node->pc->close();
            }
        }
        if (x->state->shared_ws) {
            x->state->shared_ws->close();
        }
        delete x->state;
        x->state = nullptr;
    }

    if (x->report_clock) {
        clock_free(x->report_clock);
        x->report_clock = nullptr;
    }
}

// ─────────────────────────────────────
extern "C" void p2p_tilde_setup(void) {
    post("[p2p~] by Charles K. Neimog %d.%d.%d", 0, 1, 1);

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

    // p2p json
    p2p_class = class_new(gensym("p2p"), (t_newmethod)p2p_new, (t_method)p2p_free,
                          sizeof(p2p_tilde), CLASS_DEFAULT, A_GIMME, 0);
    class_addmethod(p2p_class, (t_method)p2p_json, gensym("json"), A_GIMME, 0);
    class_addanything(p2p_class, (t_method)p2p_json);
}
