#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <opus.h>
#include <rtc/rtc.hpp>

#include "P2PPeer.hpp"

enum class P2PEventType {
    Log,
    Error,
    Connected,
    Disconnected,
    Connections,
    PeerJoined,
    PeerLeft,
    Message,
};

struct P2PEvent {
    P2PEventType type;
    std::string peer;
    std::string text;
    int count{0};
    int log_level{0};
};

struct P2PPeerResolution {
    std::shared_ptr<P2PPeer> peer;
    bool ambiguous{false};
};

class P2PSession : public std::enable_shared_from_this<P2PSession> {
public:
    using Listener = std::function<void(const P2PEvent &)>;

    static std::shared_ptr<P2PSession> create(const std::string &id);
    ~P2PSession();

    const std::string &id() const;
    bool available() const;
    bool connected() const;
    void deactivate();

    uint64_t addListener(Listener listener);
    void removeListener(uint64_t listener_id);
    void connect(const std::string &websocket_url, const std::string &room,
                 const std::string &local_username);
    void disconnect();
    void setStreaming(bool enabled);
    bool streaming() const;
    void sendMessage(const std::string &text);
    void sendJson(const std::string &json_text);
    int connectionCount() const;
    void report();

    bool claimController(const void *owner);
    void releaseController(const void *owner);
    bool claimAudioSender(const void *owner);
    void releaseAudioSender(const void *owner);
    bool claimAudioReceiver(const std::string &username, const void *owner);
    void releaseAudioReceiver(const std::string &username, const void *owner);
    void registerVideoReceiver();
    void unregisterVideoReceiver();
    bool videoNegotiated() const;

    std::vector<std::shared_ptr<P2PPeer>> peerSnapshot() const;
    void pushOutgoingAudio(const float *samples, int count);
    P2PPeerResolution resolvePeer(const std::string &username) const;
    int encodeMono(const float *pcm, int samples, unsigned char *output, int capacity);
    int frameSize() const;
    int sampleRate() const;

private:
    explicit P2PSession(std::string id);
    void initialize();
    void emit(P2PEvent event);
    void log(int level, const char *format, ...);
    void error(const char *format, ...);

    void installWebSocketCallbacks();
    void onSignallingMessage(const std::string &payload);
    void welcome(const nlohmann::json &data);
    void peerJoined(const nlohmann::json &data);
    void existingPeers(const nlohmann::json &data);
    void offer(const nlohmann::json &data);
    void answer(const nlohmann::json &data);
    void iceCandidate(const nlohmann::json &data);
    void peerLeft(const nlohmann::json &data);

    std::shared_ptr<P2PPeer> addPeer(const std::string &peer_id, const std::string &username);
    std::shared_ptr<P2PPeer> findPeerById(const std::string &peer_id) const;
    void removePeer(const std::string &peer_id, bool notify);
    void removeAllPeers();
    bool setupWebRtc(const std::shared_ptr<P2PPeer> &peer);
    void resetPeerConnection(const std::shared_ptr<P2PPeer> &peer);
    void flushPendingCandidates(const std::shared_ptr<P2PPeer> &peer);
    void configureVideoMedia(rtc::Description &description);
    void decodeAudio(const std::shared_ptr<P2PPeer> &peer, const rtc::binary &data);
    void updateConnectionState(const std::shared_ptr<P2PPeer> &peer,
                               rtc::PeerConnection::State state);
    void emitConnectionCount();
    void warnIfNotStunPair(const std::shared_ptr<P2PPeer> &peer);
    bool createPeerDecoder(const std::shared_ptr<P2PPeer> &peer);
    void rebuildRealtimePeersLocked();

#ifdef P2P_JITTER_VIDEO
    bool initializeVideoDecoder(const std::shared_ptr<P2PPeer> &peer);
    void decodeVideoFrame(const std::shared_ptr<P2PPeer> &peer, const rtc::binary &data);
#endif

    const std::string id_;
    const int sample_rate_;
    const int frame_size_{480};
    std::atomic<bool> available_{true};
    std::atomic<bool> websocket_connected_{false};
    std::atomic<bool> wants_stream_{false};
    std::atomic<int> video_receivers_{0};
    std::atomic<bool> video_negotiated_{false};

    mutable std::mutex websocket_mutex_;
    std::shared_ptr<rtc::WebSocket> websocket_;
    std::string websocket_ca_bundle_;
    std::string local_peer_id_;
    std::string room_;
    std::string local_username_;

    mutable std::mutex peers_mutex_;
    std::unordered_map<std::string, std::shared_ptr<P2PPeer>> peers_by_id_;
    std::unordered_map<std::string, std::vector<std::weak_ptr<P2PPeer>>> peers_by_name_;
    std::atomic<const std::vector<std::shared_ptr<P2PPeer>> *> realtime_peers_{nullptr};
    std::vector<std::shared_ptr<const std::vector<std::shared_ptr<P2PPeer>>>>
        retained_peer_snapshots_;
    std::vector<std::shared_ptr<P2PPeer>> retired_peers_;

    mutable std::mutex listeners_mutex_;
    std::unordered_map<uint64_t, Listener> listeners_;
    std::atomic<uint64_t> next_listener_id_{1};

    mutable std::mutex claims_mutex_;
    const void *controller_owner_{nullptr};
    const void *audio_sender_owner_{nullptr};
    std::unordered_map<std::string, const void *> audio_receiver_owners_;

    OpusEncoder *opus_encoder_mono_{nullptr};
    OpusEncoder *opus_encoder_stereo_{nullptr};
    std::mutex opus_encoder_mutex_;
};
