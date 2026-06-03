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

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <opus/opus.h>

using json = nlohmann::json;

// ─────────────────────────────────────
// Lock-free SPSC queue for audio samples
template <typename T, size_t Size = 16384> class SPSCQueue {
    T buffer[Size];
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};

  public:
    bool push(const T &item) {
        auto current_tail = tail.load(std::memory_order_relaxed);
        auto next_tail = (current_tail + 1) % Size;
        if (next_tail != head.load(std::memory_order_acquire)) {
            buffer[current_tail] = item;
            tail.store(next_tail, std::memory_order_release);
            return true;
        }
        return false;
    }

    bool pop(T &item) {
        auto current_head = head.load(std::memory_order_relaxed);
        if (current_head == tail.load(std::memory_order_acquire)) {
            return false;
        }
        item = buffer[current_head];
        head.store((current_head + 1) % Size, std::memory_order_release);
        return true;
    }

    void clear() {
        T dummy;
        while (pop(dummy)) {
        }
    }
};

// ─────────────────────────────────────
struct QueuedCandidate {
    std::string candidate;
    std::string mid;
};

struct P2PNode {
    // WebRTC components
    ix::WebSocket ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> audio_track;

    // Audio buffers
    SPSCQueue<float> tx_buffer;
    SPSCQueue<float> rx_buffer;

    // Connection state
    std::string remote_peer_id;
    std::atomic<bool> is_streaming{false};
    std::vector<QueuedCandidate> pending_remote_candidates;
    bool remote_description_set{false};
    bool is_polite{false}; // For glare handling
    bool making_offer{false};
    bool ignore_offer{false};

    // Audio codec
    OpusEncoder *opus_enc = nullptr;
    OpusDecoder *opus_dec = nullptr;
    const int sample_rate = 48000;
    const int frame_size = 480; // 10ms at 48kHz

    // Threading
    std::thread tx_thread;
    std::atomic<bool> thread_running{true};

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
    P2PNode *node;
    float peer_connected;
    t_clock *report_clock;
    t_outlet *out_signals;
    t_outlet *out_msgs;
};

// ─────────────────────────────────────
static void setup_webrtc(p2p_tilde *x, bool is_caller) {
    P2PNode *node = x->node;

    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    node->pc = std::make_shared<rtc::PeerConnection>(config);
    node->remote_description_set = false;

    // PeerConnection state callback
    node->pc->onStateChange([x](rtc::PeerConnection::State state) {
        logpost(x, PD_DEBUG, "[p2p~] PC State: %d", state);
        if (state == rtc::PeerConnection::State::Failed) {
            logpost(x, PD_ERROR, "[p2p~] Connection failed!");
        } else if (state == rtc::PeerConnection::State::Connected) {
            logpost(x, PD_NORMAL, "[p2p~] WebRTC Connected!");
        }
    });

    // Signaling state callback for glare handling
    node->pc->onSignalingStateChange([x, node](rtc::PeerConnection::SignalingState state) {
        logpost(x, PD_DEBUG, "[p2p~] Signaling State: %d", state);
        if (state == rtc::PeerConnection::SignalingState::Stable) {
            node->making_offer = false;
        }
    });

    // Local description callback (sends offer/answer)
    node->pc->onLocalDescription([x, node](rtc::Description description) {
        if (node->ws.getReadyState() == ix::ReadyState::Open) {
            json msg = {
                {"type", description.typeString()},
                {"sdp", {{"type", description.typeString()}, {"sdp", std::string(description)}}},
                {"to", node->remote_peer_id}};
            node->ws.send(msg.dump());
            logpost(x, PD_DEBUG, "[p2p~] Sent %s", description.typeString().c_str());
        }
        node->making_offer = false;
    });

    // Local candidate callback
    node->pc->onLocalCandidate([x, node](rtc::Candidate candidate) {
        if (node->ws.getReadyState() == ix::ReadyState::Open) {
            json msg;
            msg["type"] = "ice-candidate";
            msg["candidate"]["candidate"] = candidate.candidate();
            msg["candidate"]["sdpMid"] = candidate.mid();
            msg["candidate"]["sdpMLineIndex"] = 0;
            msg["to"] = node->remote_peer_id;

            node->ws.send(msg.dump());
            logpost(x, PD_DEBUG, "[p2p~] Sent ICE candidate");
        }
    });

    // FIX Bug 1: Both sides must be SendRecv so audio flows in both directions.
    // RecvOnly on the callee means it never sends, making it a one-way call.
    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
    audio.addOpusCodec(111);
    audio.setBitrate(64000);
    node->audio_track = node->pc->addTrack(audio);
    if (!node->audio_track) {
        logpost(x, PD_ERROR, "[p2p~] Failed to add audio track");
        return;
    }

    logpost(x, PD_NORMAL, "[p2p~] Audio track added");
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
                node->rx_buffer.push(pcm[i]);
            }
        }
    });

    node->pc->onTrack([x, node](std::shared_ptr<rtc::Track> track) {
        if (track->description().type() == "audio") {
            logpost(x, PD_NORMAL, "[p2p~] Remote audio track active");
        }
    });

    // Setup data channel
    if (is_caller) {
        node->dc = node->pc->createDataChannel("pd_data");
        node->dc->onOpen([x]() { logpost(x, PD_NORMAL, "[p2p~] DataChannel open"); });
    } else {
        node->pc->onDataChannel([x, node](std::shared_ptr<rtc::DataChannel> dc) {
            node->dc = dc;
            node->dc->onOpen([x]() { logpost(x, PD_NORMAL, "[p2p~] DataChannel open"); });
        });
    }
}

// ─────────────────────────────────────
static void flush_pending_candidates(p2p_tilde *x) {
    P2PNode *node = x->node;

    for (const auto &qc : node->pending_remote_candidates) {
        try {
            rtc::Candidate rtc_cand(qc.candidate, qc.mid);
            node->pc->addRemoteCandidate(rtc_cand);
            logpost(x, PD_DEBUG, "[p2p~] Flushed candidate");
        } catch (const std::exception &e) {
            logpost(x, PD_ERROR, "[p2p~] Failed to add queued candidate: %s", e.what());
        }
    }
    node->pending_remote_candidates.clear();
}

// ─────────────────────────────────────
static void p2p_connect(p2p_tilde *x, t_symbol *wss, t_symbol *room, t_symbol *user) {
    P2PNode *node = x->node;

    // Reset state
    node->remote_description_set = false;
    node->pending_remote_candidates.clear();
    node->remote_peer_id.clear();
    node->is_polite = false;
    node->making_offer = false;
    node->ignore_offer = false;

    // Setup WebSocket
    node->ws.stop();
    std::string url = std::string(wss->s_name) + "/?room=" + std::string(room->s_name);
    node->ws.setUrl(url);

    ix::WebSocketHttpHeaders headers;
    headers["Origin"] = "https://charlesneimog.github.io";
    node->ws.setExtraHeaders(headers);

    std::string username = std::string(user->s_name);

    node->ws.setOnMessageCallback([x, username](const ix::WebSocketMessagePtr &msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            json join = {{"type", "join"}, {"name", username}};
            x->node->ws.send(join.dump());
            logpost(x, PD_NORMAL, "[p2p~] Connected to the room");
            return;
        }

        if (msg->type != ix::WebSocketMessageType::Message) {
            return;
        }

        json data = json::parse(msg->str);
        std::string type = data["type"];
        logpost(x, PD_DEBUG, "[p2p~] Received: %s", type.c_str());

        if (type == "peer-joined") {
            x->node->remote_peer_id = data["from"];
            x->node->is_polite = false;
            setup_webrtc(x, true);
            x->peer_connected = x->peer_connected + 1;
            clock_delay(x->report_clock, 0);
            x->node->making_offer = true;
            x->node->pc->setLocalDescription();
        } else if (type == "offer") {
            x->node->remote_peer_id = data["from"];
            x->node->is_polite = true;
            if (!x->node->pc) {
                setup_webrtc(x, false);
            }

            if (x->node->making_offer && !x->node->is_polite) {
                logpost(x, PD_DEBUG, "[p2p~] Glare detected, ignoring offer (impolite peer wins)");
                x->node->ignore_offer = true;
                return;
            }
            x->node->making_offer = false;
            x->node->ignore_offer = false;
            std::string sdp_str;
            if (data["sdp"].is_object()) {
                sdp_str = data["sdp"]["sdp"].get<std::string>();
            } else if (data["sdp"].is_string()) {
                sdp_str = data["sdp"].get<std::string>();
            } else {
                logpost(x, PD_ERROR, "[p2p~] Invalid SDP format");
                return;
            }
            std::string sdp_type = data["sdp"]["type"].get<std::string>();

            try {
                rtc::Description desc(sdp_str, sdp_type);
                x->node->pc->setRemoteDescription(std::move(desc));
                x->node->remote_description_set = true;

                // Flush queued candidates
                flush_pending_candidates(x);

                // Create and send answer
                x->node->making_offer = true;
                x->node->pc->setLocalDescription();
            } catch (const std::exception &e) {
                logpost(x, PD_ERROR, "[p2p~] Failed to set remote description: %s", e.what());
            }

        } else if (type == "answer") {
            // If we're the impolite peer and ignored the offer, also ignore the answer
            if (x->node->ignore_offer) {
                logpost(x, PD_DEBUG, "[p2p~] Ignoring answer (impolite peer, glare resolved)");
                x->node->ignore_offer = false;
                return;
            }

            std::string sdp_str;
            if (data["sdp"].is_object()) {
                sdp_str = data["sdp"]["sdp"].get<std::string>();
            } else if (data["sdp"].is_string()) {
                sdp_str = data["sdp"].get<std::string>();
            } else {
                logpost(x, PD_ERROR, "[p2p~] Invalid SDP format");
                return;
            }

            try {
                rtc::Description desc(sdp_str, "answer");
                x->node->pc->setRemoteDescription(std::move(desc));
                x->node->remote_description_set = true;

                // Flush queued candidates
                flush_pending_candidates(x);
            } catch (const std::exception &e) {
                logpost(x, PD_ERROR, "[p2p~] Failed to set remote description: %s", e.what());
            }

        } else if (type == "ice-candidate") {
            auto cand = data["candidate"];
            std::string cand_str = cand["candidate"].get<std::string>();
            std::string mid = cand["sdpMid"].get<std::string>();

            if (!x->node->remote_description_set) {
                logpost(x, PD_DEBUG, "[p2p~] Queuing ICE candidate");
                x->node->pending_remote_candidates.push_back({cand_str, mid});
            } else {
                try {
                    rtc::Candidate rtc_cand(cand_str, mid);
                    x->node->pc->addRemoteCandidate(rtc_cand);
                    logpost(x, PD_DEBUG, "[p2p~] Added ICE candidate");
                } catch (const std::exception &e) {
                    logpost(x, PD_ERROR, "[p2p~] Failed to add candidate: %s", e.what());
                }
            }

        } else if (type == "peer-left") {
            x->peer_connected = x->peer_connected - 1;
            clock_delay(x->report_clock, 0);
        } else if (type == "existing-peers") {
            x->peer_connected = (float)data["peers"].size();
            clock_delay(x->report_clock, 0);
        } else if (type == "welcome") {
            logpost(x, PD_NORMAL, "[p2p~] Connection ID: %s",
                    data["id"].get<std::string>().c_str());
        }
    });

    node->ws.start();
    logpost(x, PD_NORMAL, "[p2p~] Connecting...");
}

// ─────────────────────────────────────
static void p2p_disconnect(p2p_tilde *x) {
    if (!x->node) {
        return;
    }

    P2PNode *node = x->node;
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

    node->ws.stop();
    node->remote_peer_id.clear();
    node->tx_buffer.clear();
    node->rx_buffer.clear();
    node->pending_remote_candidates.clear();

    x->peer_connected = 0;
    clock_delay(x->report_clock, 0);

    logpost(x, PD_NORMAL, "[p2p~] Disconnected");
}

// ─────────────────────────────────────
static void p2p_stream(p2p_tilde *x, t_float f) {
    bool wants_stream = (f != 0);
    if (x->node->is_streaming == wants_stream) {
        return;
    }

    x->node->is_streaming = wants_stream;

    if (!wants_stream) {
        x->node->tx_buffer.clear();
        logpost(x, PD_NORMAL, "[p2p~] Stream paused");
    } else {
        logpost(x, PD_NORMAL, "[p2p~] Stream active");
    }
}

// ─────────────────────────────────────
static void p2p_message(p2p_tilde *x, t_symbol *, int argc, t_atom *argv) {
    if (!x->node->dc || !x->node->dc->isOpen()) {
        return;
    }

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

    x->node->dc->send(text);
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
    auto *out = (t_sample *)w[3];
    int n = (int)w[4];

    P2PNode *node = x->node;

    // Send audio to network
    if (node->is_streaming) {
        for (int i = 0; i < n; ++i) {
            node->tx_buffer.push(in[i]);
        }
    }

    // Receive audio from network
    for (int i = 0; i < n; ++i) {
        float sample = 0.f;
        if (node->rx_buffer.pop(sample)) {
            out[i] = sample;
        } else {
            out[i] = 0.f;
        }
    }

    return (w + 5);
}

// ─────────────────────────────────────
static void p2p_dsp(p2p_tilde *x, t_signal **sp) {
    if (x->peer_connected == 0) {
        signal_setmultiout(&sp[1], 1);
    } else {
        signal_setmultiout(&sp[1], x->peer_connected);
    }

    dsp_add(p2p_tilde_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *p2p_new(t_symbol *s, int argc, t_atom *argv) {
    (void)s;
    (void)argc;
    (void)argv;

    p2p_tilde *x = (p2p_tilde *)pd_new(p2p_tilde_class);
    x->node = new P2PNode();
    x->peer_connected = 0;

    // Initialize Opus encoder
    int err;
    x->node->opus_enc = opus_encoder_create(x->node->sample_rate, 1, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK) {
        logpost(x, PD_ERROR, "[p2p~] Opus encoder error: %d", err);
        delete x->node;
        return nullptr;
    }

    opus_encoder_ctl(x->node->opus_enc, OPUS_SET_BITRATE(OPUS_BITRATE_MAX));
    opus_encoder_ctl(x->node->opus_enc, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(x->node->opus_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(x->node->opus_enc, OPUS_SET_PACKET_LOSS_PERC(0));
    opus_encoder_ctl(x->node->opus_enc, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(x->node->opus_enc, OPUS_SET_DTX(0));

    // Initialize Opus decoder
    x->node->opus_dec = opus_decoder_create(x->node->sample_rate, 1, &err);
    if (err != OPUS_OK) {
        logpost(x, PD_ERROR, "[p2p~] Opus decoder error: %d", err);
    }

    // Create outlets and clock
    x->out_signals = outlet_new(&x->x_obj, &s_signal);
    x->out_msgs = outlet_new(&x->x_obj, gensym("anything"));
    x->report_clock = clock_new(&x->x_obj, (t_method)p2p_report);

    x->node->tx_thread = std::thread([node = x->node]() {
        const int FRAME_SIZE = 960;
        float pcm_frame[FRAME_SIZE];
        int collected = 0;
        unsigned char opus_payload[4000];
        uint16_t seq = 0;
        uint32_t rtp_timestamp = 0;
        const uint32_t ssrc = 12345;
        const uint8_t payload_type = 111;
        const uint32_t timestamp_increment = FRAME_SIZE;

        while (node->thread_running) {
            while (collected < FRAME_SIZE && node->tx_buffer.pop(pcm_frame[collected])) {
                collected++;
            }

            if (collected == FRAME_SIZE) {
                if (node->is_streaming && node->audio_track && node->audio_track->isOpen()) {
                    int bytes = opus_encode_float(node->opus_enc, pcm_frame, FRAME_SIZE,
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
                        node->audio_track->send(rtp_packet);
                    }
                }
                collected = 0;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
    });

    return x;
}

// ─────────────────────────────────────
static void p2p_free(p2p_tilde *x) {
    if (x->node) {
        x->node->thread_running = false;
        x->node->ws.stop();
        if (x->node->pc) {
            x->node->pc->close();
        }
        delete x->node;
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
}
