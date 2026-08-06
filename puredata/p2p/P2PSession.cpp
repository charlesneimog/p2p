#include "P2PSession.hpp"

#include "P2PMainThreadDispatch.hpp"

#include <m_pd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <variant>

#include <spdlog/spdlog.h>

using json = nlohmann::json;

// ─────────────────────────────────────
const char *candidateTypeName(rtc::Candidate::Type type) {
    switch (type) {
    case rtc::Candidate::Type::Host:
        return "host";
    case rtc::Candidate::Type::ServerReflexive:
        return "srflx";
    case rtc::Candidate::Type::PeerReflexive:
        return "prflx";
    case rtc::Candidate::Type::Relayed:
        return "relay";
    default:
        return "unknown";
    }
}

// ─────────────────────────────────────
std::string formatMessage(const char *format, va_list arguments) {
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, arguments);
    return buffer;
}

// ─────────────────────────────────────
bool isReadableRegularFile(const std::string &path) {
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    std::ifstream file(path);
    return file.good();
}

// ─────────────────────────────────────
struct CaBundleResolution {
    std::string path;
    std::string error;
};

// ─────────────────────────────────────
CaBundleResolution resolveCaBundle() {
    if (const char *environment_path = std::getenv("SSL_CERT_FILE");
        environment_path && environment_path[0]) {
        if (isReadableRegularFile(environment_path)) {
            return {environment_path, {}};
        }
        return {{},
                std::string("SSL_CERT_FILE does not name a readable CA bundle: '") +
                    environment_path + "'"};
    }

    // OpenSSL built into this external may have a prefix which differs from the
    // host system. Prefer the host's maintained trust bundle when it exists.
    static constexpr const char *candidate_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch
        "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                            // openSUSE
        "/etc/ssl/cert.pem",                                 // Alpine, macOS with OpenSSL
        "/usr/local/share/certs/ca-root-nss.crt",            // FreeBSD
        "/opt/homebrew/etc/openssl@3/cert.pem",              // Apple Silicon Homebrew
        "/usr/local/etc/openssl@3/cert.pem",                 // Intel Homebrew
    };
    for (const char *candidate : candidate_paths) {
        if (isReadableRegularFile(candidate)) {
            return {candidate, {}};
        }
    }

    // Leaving this unset preserves libdatachannel/OpenSSL's default trust lookup.
    return {};
}

// ─────────────────────────────────────
std::shared_ptr<P2PSession> P2PSession::create(const std::string &id) {
    auto session = std::shared_ptr<P2PSession>(new P2PSession(id));
    session->initialize();
    return session;
}

// ─────────────────────────────────────
P2PSession::P2PSession(std::string id)
    : id_(std::move(id)), sample_rate_(static_cast<int>(sys_getsr())) {}

// ─────────────────────────────────────
void P2PSession::initialize() {
    rebuildRealtimePeersLocked();
    if (sample_rate_ != 48000) {
        available_ = false;
    }
}

// ─────────────────────────────────────
P2PSession::~P2PSession() {
    available_ = false;
    disconnect();
}

// ─────────────────────────────────────
const std::string &P2PSession::id() const {
    return id_;
}

// ─────────────────────────────────────
bool P2PSession::available() const {
    return available_;
}

// ─────────────────────────────────────
bool P2PSession::connected() const {
    return websocket_connected_;
}

// ─────────────────────────────────────
void P2PSession::deactivate() {
    if (!available_.exchange(false)) {
        return;
    }
    disconnect();
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    listeners_.clear();
}

// ─────────────────────────────────────
uint64_t P2PSession::addListener(Listener listener) {
    const uint64_t id = next_listener_id_++;
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    listeners_[id] = std::move(listener);
    return id;
}

// ─────────────────────────────────────
void P2PSession::removeListener(uint64_t listener_id) {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    listeners_.erase(listener_id);
}

// ─────────────────────────────────────
void P2PSession::emit(P2PEvent event) {
    std::vector<Listener> listeners;
    {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        listeners.reserve(listeners_.size());
        for (const auto &entry : listeners_) {
            listeners.push_back(entry.second);
        }
    }
    P2PMainThreadDispatch::enqueue([listeners = std::move(listeners), event = std::move(event)]() {
        for (const auto &listener : listeners) {
            listener(event);
        }
    });
}

// ─────────────────────────────────────
void P2PSession::log(int level, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::string message = formatMessage(format, arguments);
    va_end(arguments);
    emit({P2PEventType::Log, {}, std::move(message), 0, level});
}

// ─────────────────────────────────────
void P2PSession::error(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::string message = formatMessage(format, arguments);
    va_end(arguments);
    emit({P2PEventType::Error, {}, std::move(message), 0, PD_ERROR});
}

// ─────────────────────────────────────
void P2PSession::connect(const std::string &websocket_url, const std::string &room,
                         const std::string &local_username) {
    if (!available_) {
        error("Session '%s' is unavailable", id_.c_str());
        return;
    }
    std::shared_ptr<rtc::WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        if (!websocket_) {
            const auto ca_bundle = resolveCaBundle();
            if (!ca_bundle.error.empty()) {
                error("%s", ca_bundle.error.c_str());
                return;
            }
            rtc::WebSocket::Configuration configuration;
            configuration.connectionTimeout = std::chrono::milliseconds(1500);
            if (!ca_bundle.path.empty()) {
                configuration.caCertificatePemFile = ca_bundle.path;
            }
            websocket_ca_bundle_ = ca_bundle.path;
            websocket_ = std::make_shared<rtc::WebSocket>(configuration);
        }
        websocket = websocket_;
        if (websocket->isOpen()) {
            error("Already Connected");
            return;
        }
        room_ = room;
        local_username_ = local_username;
    }
    installWebSocketCallbacks();
    const std::string url = websocket_url + "/?room=" + room;
    websocket->open(url);
}

// ─────────────────────────────────────
void P2PSession::installWebSocketCallbacks() {
    std::shared_ptr<rtc::WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        websocket = websocket_;
    }
    if (!websocket) {
        return;
    }
    std::weak_ptr<P2PSession> weak_session = shared_from_this();
    websocket->onOpen([weak_session]() {
        auto session = weak_session.lock();
        if (!session || !session->available_) {
            return;
        }
        std::shared_ptr<rtc::WebSocket> socket;
        std::string username;
        std::string room;
        {
            std::lock_guard<std::mutex> lock(session->websocket_mutex_);
            socket = session->websocket_;
            username = session->local_username_;
            room = session->room_;
        }
        if (socket) {
            json join = {{"type", "join"}, {"name", username}};
            socket->send(join.dump());
        }
        session->websocket_connected_ = true;
        session->log(PD_NORMAL, "Connected to the room: '%s'", room.c_str());
        session->emit({P2PEventType::Connected});
        session->emitConnectionCount();
    });
    websocket->onClosed([weak_session]() {
        auto session = weak_session.lock();
        if (!session) {
            return;
        }
        const bool was_connected = session->websocket_connected_.exchange(false);
        session->removeAllPeers();
        if (was_connected) {
            session->emit({P2PEventType::Disconnected});
        }
        session->emitConnectionCount();
    });
    websocket->onError([weak_session](std::string message) {
        if (auto session = weak_session.lock()) {
            if (message == "TLS connection failed") {
                std::string ca_bundle;
                {
                    std::lock_guard<std::mutex> lock(session->websocket_mutex_);
                    ca_bundle = session->websocket_ca_bundle_;
                }
                if (!ca_bundle.empty()) {
                    session->error("WebSocket error: TLS connection failed "
                                   "(CA bundle: '%s')",
                                   ca_bundle.c_str());
                } else {
                    session->error("WebSocket error: TLS connection failed; no system CA "
                                   "bundle was detected (set SSL_CERT_FILE)");
                }
            } else {
                session->error("WebSocket error: %s", message.c_str());
            }
        }
    });
    websocket->onMessage([weak_session](std::variant<rtc::binary, std::string> data) {
        auto session = weak_session.lock();
        if (!session || !session->available_) {
            return;
        }
        std::string payload;
        if (std::holds_alternative<std::string>(data)) {
            payload = std::get<std::string>(std::move(data));
        } else {
            const auto &binary = std::get<rtc::binary>(data);
            payload.assign(reinterpret_cast<const char *>(binary.data()), binary.size());
        }
        session->onSignallingMessage(payload);
    });
}

// ─────────────────────────────────────
void P2PSession::onSignallingMessage(const std::string &payload) {
    try {
        const json data = json::parse(payload);
        spdlog::info("{}", data.dump(4));
        const std::string type = data.contains("type") ? data["type"].get<std::string>() : "";
        if (type == "welcome") {
            welcome(data);
        } else if (type == "peer-joined") {
            peerJoined(data);
        } else if (type == "existing-peers") {
            existingPeers(data);
        } else if (type == "offer") {
            offer(data);
        } else if (type == "ice-candidate") {
            iceCandidate(data);
        } else if (type == "answer") {
            answer(data);
        } else if (type == "peer-left") {
            peerLeft(data);
        } else {
            error("%s", payload.c_str());
        }
    } catch (const std::exception &exception) {
        error("Invalid signalling message: %s", exception.what());
    }
}

// ─────────────────────────────────────
void P2PSession::disconnect() {
    std::shared_ptr<rtc::WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        websocket = websocket_;
        if (websocket) {
            websocket->resetCallbacks();
        }
        websocket_.reset();
        websocket_ca_bundle_.clear();
        local_peer_id_.clear();
    }
    if (websocket) {
        websocket->close();
    }
    removeAllPeers();
    const bool was_connected = websocket_connected_.exchange(false);
    if (was_connected) {
        emit({P2PEventType::Disconnected});
    }
    emitConnectionCount();
    log(PD_NORMAL, "Disconnected");
}

// ─────────────────────────────────────
void P2PSession::setStreaming(bool enabled) {
    if (wants_stream_.exchange(enabled) != enabled) {
        log(PD_NORMAL, "Stream %s", enabled ? "active" : "paused");
    }
    const auto peers = peerSnapshot();
    for (const auto &peer : peers) {
        peer->is_streaming = enabled;
    }
}

// ─────────────────────────────────────
bool P2PSession::streaming() const {
    return wants_stream_;
}

// ─────────────────────────────────────
void P2PSession::sendMessage(const std::string &text) {
    json payload = {{"type", "message"}, {"text", text}};
    const std::string serialized = payload.dump(4);
    sendJson(serialized);
}

// ─────────────────────────────────────
void P2PSession::sendJson(const std::string &json_text) {
    const auto peers = peerSnapshot();
    for (const auto &peer : peers) {
        if (peer->dc && peer->dc->isOpen()) {
            peer->dc->send(json_text);
        }
    }
}

// ─────────────────────────────────────
int P2PSession::connectionCount() const {
    int count = 0;
    const auto peers = peerSnapshot();
    for (const auto &peer : peers) {
        if (peer->active && peer->connected) {
            ++count;
        }
    }
    return count;
}

// ─────────────────────────────────────
void P2PSession::emitConnectionCount() {
    emit({P2PEventType::Connections, {}, {}, connectionCount()});
}

// ─────────────────────────────────────
void P2PSession::report() {
    emitConnectionCount();
}

// ─────────────────────────────────────
bool P2PSession::claimController(const void *owner) {
    std::lock_guard<std::mutex> lock(claims_mutex_);
    if (controller_owner_ && controller_owner_ != owner) {
        return false;
    }
    controller_owner_ = owner;
    return true;
}

// ─────────────────────────────────────
void P2PSession::releaseController(const void *owner) {
    std::lock_guard<std::mutex> lock(claims_mutex_);
    if (controller_owner_ == owner) {
        controller_owner_ = nullptr;
    }
}

// ─────────────────────────────────────
bool P2PSession::claimAudioSender(const void *owner) {
    std::lock_guard<std::mutex> lock(claims_mutex_);
    if (audio_sender_owner_ && audio_sender_owner_ != owner) {
        return false;
    }
    audio_sender_owner_ = owner;
    return true;
}

// ─────────────────────────────────────
void P2PSession::releaseAudioSender(const void *owner) {
    std::lock_guard<std::mutex> lock(claims_mutex_);
    if (audio_sender_owner_ == owner) {
        audio_sender_owner_ = nullptr;
    }
}

// ─────────────────────────────────────
bool P2PSession::claimAudioReceiver(const std::string &username, const void *owner) {
    std::lock_guard<std::mutex> lock(claims_mutex_);
    auto iterator = audio_receiver_owners_.find(username);
    if (iterator != audio_receiver_owners_.end() && iterator->second != owner) {
        return false;
    }
    audio_receiver_owners_[username] = owner;
    return true;
}

// ─────────────────────────────────────
void P2PSession::releaseAudioReceiver(const std::string &username, const void *owner) {
    std::lock_guard<std::mutex> lock(claims_mutex_);
    auto iterator = audio_receiver_owners_.find(username);
    if (iterator != audio_receiver_owners_.end() && iterator->second == owner) {
        audio_receiver_owners_.erase(iterator);
    }
}

// ─────────────────────────────────────
void P2PSession::registerVideoReceiver() {
    const int previous = video_receivers_.fetch_add(1);
    if (previous == 0 && !peerSnapshot().empty() && !video_negotiated_) {
        error("[p2p.r.video] reconnect the session to enable video");
    }
}

// ─────────────────────────────────────
void P2PSession::unregisterVideoReceiver() {
    int current = video_receivers_;
    while (current > 0 && !video_receivers_.compare_exchange_weak(current, current - 1)) {
    }
}

// ─────────────────────────────────────
bool P2PSession::videoNegotiated() const {
    return video_negotiated_;
}

// ─────────────────────────────────────
std::vector<std::shared_ptr<P2PPeer>> P2PSession::peerSnapshot() const {
    std::vector<std::shared_ptr<P2PPeer>> peers;
    std::lock_guard<std::mutex> lock(peers_mutex_);
    peers.reserve(peers_by_id_.size());
    for (const auto &entry : peers_by_id_) {
        peers.push_back(entry.second);
    }
    return peers;
}

// ─────────────────────────────────────
void P2PSession::pushOutgoingAudio(const float *samples, int count) {
    const auto *peers = realtime_peers_.load(std::memory_order_acquire);
    if (!peers || !wants_stream_) {
        return;
    }
    for (const auto &peer : *peers) {
        if (!peer->active || !peer->connected || !peer->is_streaming) {
            continue;
        }
        for (int index = 0; index < count; ++index) {
            peer->send_buffer.push(samples[index]);
        }
    }
}

// ─────────────────────────────────────
P2PPeerResolution P2PSession::resolvePeer(const std::string &username) const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto iterator = peers_by_name_.find(username);
    if (iterator == peers_by_name_.end()) {
        return {};
    }
    P2PPeerResolution result;
    int matches = 0;
    for (const auto &weak_peer : iterator->second) {
        auto peer = weak_peer.lock();
        if (peer && peer->active) {
            result.peer = std::move(peer);
            ++matches;
        }
    }
    if (matches != 1) {
        result.peer.reset();
        result.ambiguous = matches > 1;
    }
    return result;
}

// ─────────────────────────────────────
int P2PSession::frameSize() const {
    return frame_size_;
}

// ─────────────────────────────────────
int P2PSession::sampleRate() const {
    return sample_rate_;
}

// ─────────────────────────────────────
void P2PSession::welcome(const json &data) {
    const std::string id = data["id"].get<std::string>();
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        local_peer_id_ = id;
    }
    log(PD_NORMAL, "Connected ID: %s", id.substr(0, 6).c_str());
}

// ─────────────────────────────────────
std::shared_ptr<P2PPeer> P2PSession::addPeer(const std::string &peer_id,
                                             const std::string &username) {
    std::shared_ptr<P2PPeer> peer;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto existing = peers_by_id_.find(peer_id);
        if (existing != peers_by_id_.end()) {
            return existing->second;
        }
        if (peers_by_id_.size() >= 8) {
            error("No free nodes available for peer %s", peer_id.c_str());
            return {};
        }
        peer = std::make_shared<P2PPeer>(peer_id, username);
        if (!peer->initializeEncoder(sample_rate_)) {
            error("Opus encoder error for peer '%s'", username.c_str());
            return {};
        }
        peers_by_id_[peer_id] = peer;
        peers_by_name_[username].push_back(peer);
        rebuildRealtimePeersLocked();
    }
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        peer->ws = websocket_;
        const bool should_be_caller = local_peer_id_ < peer_id;
        peer->is_polite = !should_be_caller;
        peer->making_offer = should_be_caller;
    }
    peer->is_streaming = wants_stream_.load();
    peer->startTransmission(frame_size_);
    if (!setupWebRtc(peer)) {
        removePeer(peer_id, false);
        return {};
    }
    emit({P2PEventType::PeerJoined, username});
    return peer;
}

// ─────────────────────────────────────
std::shared_ptr<P2PPeer> P2PSession::findPeerById(const std::string &peer_id) const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto iterator = peers_by_id_.find(peer_id);
    return iterator == peers_by_id_.end() ? std::shared_ptr<P2PPeer>() : iterator->second;
}

// ─────────────────────────────────────
void P2PSession::removePeer(const std::string &peer_id, bool notify) {
    std::shared_ptr<P2PPeer> peer;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto iterator = peers_by_id_.find(peer_id);
        if (iterator == peers_by_id_.end()) {
            return;
        }
        peer = iterator->second;
        retired_peers_.push_back(peer);
        peers_by_id_.erase(iterator);
        auto names = peers_by_name_.find(peer->username);
        if (names != peers_by_name_.end()) {
            auto &entries = names->second;
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [&peer](const std::weak_ptr<P2PPeer> &candidate) {
                                             auto locked = candidate.lock();
                                             return !locked || locked == peer;
                                         }),
                          entries.end());
            if (entries.empty()) {
                peers_by_name_.erase(names);
            }
        }
        rebuildRealtimePeersLocked();
    }
    peer->shutdown();
    if (notify) {
        emit({P2PEventType::PeerLeft, peer->username});
    }
    emitConnectionCount();
}

// ─────────────────────────────────────
void P2PSession::removeAllPeers() {
    std::vector<std::shared_ptr<P2PPeer>> peers;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peers.reserve(peers_by_id_.size());
        for (auto &entry : peers_by_id_) {
            peers.push_back(entry.second);
            retired_peers_.push_back(std::move(entry.second));
        }
        peers_by_id_.clear();
        peers_by_name_.clear();
        rebuildRealtimePeersLocked();
    }
    for (const auto &peer : peers) {
        peer->shutdown();
    }
}

// ─────────────────────────────────────
void P2PSession::rebuildRealtimePeersLocked() {
    auto peers = std::make_shared<std::vector<std::shared_ptr<P2PPeer>>>();
    peers->reserve(peers_by_id_.size());
    for (const auto &entry : peers_by_id_) {
        peers->push_back(entry.second);
    }
    auto immutable = std::static_pointer_cast<const std::vector<std::shared_ptr<P2PPeer>>>(peers);
    retained_peer_snapshots_.push_back(immutable);
    realtime_peers_.store(immutable.get(), std::memory_order_release);
}

// ─────────────────────────────────────
void P2PSession::peerJoined(const json &data) {
    const std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    const std::string peer_name = data.contains("peer") && data["peer"].contains("name")
                                      ? data["peer"]["name"].get<std::string>()
                                      : from_peer;
    auto peer = addPeer(from_peer, peer_name);
    if (!peer) {
        return;
    }
    if (!peer->is_polite) {
        if (!peer->local_offer_sent) {
            peer->pc->setLocalDescription();
        }
    } else {
        log(PD_NORMAL, "Waiting for offer from %s (I am callee)", from_peer.c_str());
    }
    log(PD_NORMAL, "Peer '%s' joined", peer->username.c_str());
}

// ─────────────────────────────────────
void P2PSession::existingPeers(const json &data) {
    for (const auto &description : data["peers"]) {
        const std::string peer_id = description["id"].get<std::string>();
        const std::string peer_name = description["name"].get<std::string>();
        auto peer = addPeer(peer_id, peer_name);
        if (!peer) {
            continue;
        }
        if (!peer->is_polite && !peer->local_offer_sent) {
            peer->pc->setLocalDescription();
            log(PD_NORMAL, "Connecting to existing peer '%s' (%s)", peer_name.c_str(),
                peer_id.substr(0, 6).c_str());
        } else if (!peer->is_polite) {
            log(PD_NORMAL, "Connecting to existing peer '%s' (%s)", peer_name.c_str(),
                peer_id.substr(0, 6).c_str());
        } else {
            log(PD_NORMAL, "Waiting for offer from existing peer '%s' (%s)", peer_name.c_str(),
                peer_id.substr(0, 6).c_str());
        }
    }
    emitConnectionCount();
}

// ─────────────────────────────────────
void P2PSession::offer(const json &data) {
    const std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    auto peer = findPeerById(from_peer);
    if (!peer) {
        peer = addPeer(from_peer, from_peer);
        if (!peer) {
            return;
        }
    }
    const bool offer_collision =
        peer->making_offer ||
        peer->pc->signalingState() != rtc::PeerConnection::SignalingState::Stable;
    if (offer_collision && !peer->is_polite) {
        log(PD_DEBUG, "Glare: ignoring offer from %s (impolite)", from_peer.c_str());
        peer->ignore_offer = true;
        return;
    }
    peer->ignore_offer = false;

    std::string sdp;
    if (data["sdp"].is_object()) {
        sdp = data["sdp"]["sdp"].get<std::string>();
    } else if (data["sdp"].is_string()) {
        sdp = data["sdp"].get<std::string>();
    } else {
        error("Invalid SDP format");
        return;
    }
    try {
        rtc::Description description(sdp, "offer");
        configureVideoMedia(description);
        peer->pc->setRemoteDescription(std::move(description));
        peer->remote_description_set = true;
        flushPendingCandidates(peer);
        peer->making_offer = false;
        peer->answering_offer = true;
        peer->pc->setLocalDescription();
    } catch (const std::exception &exception) {
        peer->answering_offer = false;
        error("Failed to set remote description (offer): %s", exception.what());
    }
}

// ─────────────────────────────────────
void P2PSession::answer(const json &data) {
    const std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    auto peer = findPeerById(from_peer);
    if (!peer || !peer->pc) {
        return;
    }
    if (peer->pc->signalingState() != rtc::PeerConnection::SignalingState::HaveLocalOffer) {
        log(PD_DEBUG, "Ignoring unexpected answer from %s; signaling state is not HaveLocalOffer",
            from_peer.c_str());
        return;
    }
    std::string sdp;
    if (data["sdp"].is_object()) {
        sdp = data["sdp"]["sdp"].get<std::string>();
    } else if (data["sdp"].is_string()) {
        sdp = data["sdp"].get<std::string>();
    } else {
        return;
    }
    try {
        rtc::Description description(sdp, "answer");
        peer->pc->setRemoteDescription(std::move(description));
        peer->making_offer = false;
        peer->remote_description_set = true;
        peer->ignore_offer = false;
        flushPendingCandidates(peer);
    } catch (const std::exception &exception) {
        error("Failed to set remote description (answer): %s", exception.what());
    }
}

// ─────────────────────────────────────
void P2PSession::iceCandidate(const json &data) {
    const std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    auto peer = findPeerById(from_peer);
    if (!peer || !peer->pc || peer->ignore_offer || !data.contains("candidate") ||
        !data["candidate"].is_object()) {
        return;
    }
    const auto &candidate = data["candidate"];
    if (!candidate.contains("candidate") || !candidate.contains("sdpMid")) {
        return;
    }
    const std::string candidate_text = candidate["candidate"].get<std::string>();
    const std::string mid = candidate["sdpMid"].get<std::string>();
    if (candidate_text.empty() || mid.empty()) {
        return;
    }
    if (!peer->remote_description_set ||
        peer->pc->signalingState() == rtc::PeerConnection::SignalingState::HaveLocalOffer) {
        peer->pending_remote_candidates.push_back({candidate_text, mid});
        log(PD_DEBUG, "Queuing ICE candidate from %s, mid=%s", from_peer.c_str(), mid.c_str());
        return;
    }
    try {
        peer->pc->addRemoteCandidate(rtc::Candidate(candidate_text, mid));
    } catch (const std::exception &exception) {
        error("Failed to add ICE candidate mid=%s: %s", mid.c_str(), exception.what());
    }
}

// ─────────────────────────────────────
void P2PSession::peerLeft(const json &data) {
    const std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    auto peer = findPeerById(from_peer);
    if (peer) {
        log(PD_NORMAL, "Peer '%s' left", peer->username.c_str());
        removePeer(from_peer, true);
    } else {
        emitConnectionCount();
    }
}

// ─────────────────────────────────────
void P2PSession::flushPendingCandidates(const std::shared_ptr<P2PPeer> &peer) {
    for (const auto &candidate : peer->pending_remote_candidates) {
        try {
            peer->pc->addRemoteCandidate(rtc::Candidate(candidate.candidate, candidate.mid));
            log(PD_DEBUG, "Flushed candidate for peer %s", peer->peer_id.c_str());
        } catch (const std::exception &exception) {
            error("Failed to add queued candidate: %s", exception.what());
        }
    }
    peer->pending_remote_candidates.clear();
}

// ─────────────────────────────────────
void P2PSession::resetPeerConnection(const std::shared_ptr<P2PPeer> &peer) {
    peer->is_streaming = false;
    peer->remote_description_set = false;
    peer->making_offer = false;
    peer->ignore_offer = false;
    peer->answering_offer = false;
    peer->local_offer_sent = false;
    peer->polite_media_offer_sent = false;
    peer->stun_warning_reported = false;
    peer->shutdown();
}

// ─────────────────────────────────────
bool P2PSession::createPeerDecoder(const std::shared_ptr<P2PPeer> &peer) {
    std::lock_guard<std::mutex> lock(peer->opus_dec_mono_mutex);
    if (peer->opus_dec_mono) {
        opus_decoder_destroy(peer->opus_dec_mono);
    }
    int error_code = OPUS_OK;
    peer->opus_dec_mono = opus_decoder_create(sample_rate_, 1, &error_code);
    if (error_code != OPUS_OK) {
        peer->opus_dec_mono = nullptr;
        error("Opus decoder error for peer '%s': %d", peer->username.c_str(), error_code);
        return false;
    }
    return true;
}

// ─────────────────────────────────────
void P2PSession::decodeAudio(const std::shared_ptr<P2PPeer> &peer, const rtc::binary &data) {
    constexpr int maximum_samples = 5760;
    float pcm[maximum_samples];
    int samples;
    {
        std::lock_guard<std::mutex> lock(peer->opus_dec_mono_mutex);
        if (!peer->opus_dec_mono) {
            error("Opus decode not initialized");
            return;
        }
        samples = opus_decode_float(peer->opus_dec_mono,
                                    reinterpret_cast<const unsigned char *>(data.data()),
                                    static_cast<opus_int32>(data.size()), pcm, maximum_samples, 0);
    }
    if (samples > 0) {
        for (int index = 0; index < samples; ++index) {
            peer->receive_buffer.push(pcm[index]);
        }
    } else if (samples < 0) {
        error("Opus decode failed: %d, bytes=%zu", samples, data.size());
    }
}

// ─────────────────────────────────────
void P2PSession::updateConnectionState(const std::shared_ptr<P2PPeer> &peer,
                                       rtc::PeerConnection::State state) {
    const bool now_connected = state == rtc::PeerConnection::State::Connected;
    const bool was_connected = peer->connected.exchange(now_connected);
    if (was_connected != now_connected || state == rtc::PeerConnection::State::Failed ||
        state == rtc::PeerConnection::State::Disconnected ||
        state == rtc::PeerConnection::State::Closed) {
        emitConnectionCount();
    }
}

// ─────────────────────────────────────
void P2PSession::warnIfNotStunPair(const std::shared_ptr<P2PPeer> &peer) {
    if (peer->stun_warning_reported || !peer->pc) {
        return;
    }
    rtc::Candidate local;
    rtc::Candidate remote;
    if (!peer->pc->getSelectedCandidatePair(&local, &remote)) {
        return;
    }
    peer->stun_warning_reported = true;
    const bool uses_stun = local.type() == rtc::Candidate::Type::ServerReflexive ||
                           remote.type() == rtc::Candidate::Type::ServerReflexive;
    if (!uses_stun) {
        error("Warning: selected ICE pair for peer '%s' is not STUN/srflx "
              "(local=%s, remote=%s)",
              peer->username.c_str(), candidateTypeName(local.type()),
              candidateTypeName(remote.type()));
    }
}

// ─────────────────────────────────────
void P2PSession::configureVideoMedia(rtc::Description &description) {
    for (int index = 0; index < description.mediaCount(); ++index) {
        auto media = description.media(index);
        if (!std::holds_alternative<rtc::Description::Media *>(media)) {
            continue;
        }
        auto *remote_media = std::get<rtc::Description::Media *>(media);
        if (!remote_media || remote_media->type() != "video") {
            continue;
        }
#ifdef P2P_GEM_VIDEO
        if (video_receivers_ <= 0) {
            remote_media->setDirection(rtc::Description::Direction::Inactive);
            remote_media->markRemoved();
            continue;
        }
        const auto offered_payloads = remote_media->payloadTypes();
        std::vector<int> h264_payloads;
        for (int payload : offered_payloads) {
            const auto *map = remote_media->rtpMap(payload);
            if (!map) {
                continue;
            }
            log(PD_NORMAL, "Offered video codec: PT %d %s", payload, map->format.c_str());
            if (strcasecmp(map->format.c_str(), "H264") == 0) {
                h264_payloads.push_back(payload);
            }
        }
        for (int payload : offered_payloads) {
            if (!remote_media->hasPayloadType(payload)) {
                continue;
            }
            const auto *map = remote_media->rtpMap(payload);
            if (!map) {
                continue;
            }
            bool keep = strcasecmp(map->format.c_str(), "H264") == 0;
            if (!keep && strcasecmp(map->format.c_str(), "RTX") == 0) {
                for (int h264_payload : h264_payloads) {
                    const std::string apt = "apt=" + std::to_string(h264_payload);
                    if (std::find(map->fmtps.begin(), map->fmtps.end(), apt) != map->fmtps.end()) {
                        keep = true;
                        break;
                    }
                }
            }
            if (!keep) {
                remote_media->removeRtpMap(payload);
            }
        }
        if (!h264_payloads.empty()) {
            remote_media->setDirection(rtc::Description::Direction::SendOnly);
            video_negotiated_ = true;
            log(PD_NORMAL, "Accepting remote H264 video");
        } else {
            remote_media->setDirection(rtc::Description::Direction::Inactive);
            remote_media->markRemoved();
            error("Peer did not offer H264; video disabled");
        }
#else
        remote_media->setDirection(rtc::Description::Direction::Inactive);
        remote_media->markRemoved();
#endif
    }
}

// ─────────────────────────────────────
bool P2PSession::setupWebRtc(const std::shared_ptr<P2PPeer> &peer) {
    rtc::Configuration configuration;
    configuration.iceServers.emplace_back("stun:stun.l.google.com:19302");
    peer->pc = std::make_shared<rtc::PeerConnection>(configuration);
    if (!createPeerDecoder(peer)) {
        peer->pc->close();
        peer->pc.reset();
        return false;
    }
    peer->remote_description_set = false;
    peer->stun_warning_reported = false;
    log(PD_DEBUG, "Pd is polite=%d", peer->is_polite);

    std::weak_ptr<P2PSession> weak_session = shared_from_this();
    std::weak_ptr<P2PPeer> weak_peer = peer;
    peer->pc->onStateChange([weak_session, weak_peer](rtc::PeerConnection::State state) {
        auto session = weak_session.lock();
        auto locked_peer = weak_peer.lock();
        if (session && locked_peer && locked_peer->active) {
            session->updateConnectionState(locked_peer, state);
        }
    });
    peer->pc->onIceStateChange([weak_session, weak_peer](rtc::PeerConnection::IceState state) {
        auto session = weak_session.lock();
        auto locked_peer = weak_peer.lock();
        if (!session || !locked_peer || !locked_peer->active) {
            return;
        }
        if (state == rtc::PeerConnection::IceState::Connected ||
            state == rtc::PeerConnection::IceState::Completed) {
            session->warnIfNotStunPair(locked_peer);
        }
    });
    peer->pc->onLocalCandidate([weak_session, weak_peer](rtc::Candidate candidate) {
        auto session = weak_session.lock();
        auto locked_peer = weak_peer.lock();
        if (!session || !locked_peer || !locked_peer->active || !locked_peer->ws ||
            locked_peer->peer_id.empty()) {
            return;
        }
        json message = {
            {"type", "ice-candidate"},
            {"to", locked_peer->peer_id},
            {"candidate", {{"candidate", std::string(candidate)}, {"sdpMid", candidate.mid()}}}};
        locked_peer->ws->send(message.dump());
    });
    peer->pc->onLocalDescription([weak_session, weak_peer](rtc::Description description) {
        auto session = weak_session.lock();
        auto locked_peer = weak_peer.lock();
        if (!session || !locked_peer || !locked_peer->active) {
            return;
        }
        const std::string type = description.typeString();
        session->log(PD_DEBUG, "onLocalDescription %s", type.c_str());
        if (type == "offer") {
            const bool post_answer_offer =
                !locked_peer->making_offer && locked_peer->remote_description_set;
            if (post_answer_offer) {
                if (!locked_peer->is_polite || locked_peer->polite_media_offer_sent) {
                    session->log(PD_DEBUG, "Suppressing follow-up local offer for peer '%s'",
                                 locked_peer->username.c_str());
                    return;
                }
                locked_peer->polite_media_offer_sent = true;
                session->log(PD_DEBUG, "Sending one polite media update offer for peer '%s'",
                             locked_peer->username.c_str());
            }
            locked_peer->making_offer = false;
            locked_peer->local_offer_sent = true;
        } else if (type == "answer") {
            locked_peer->answering_offer = false;
        }
        if (locked_peer->ws) {
            json message = {{"type", type},
                            {"sdp", {{"type", type}, {"sdp", std::string(description)}}},
                            {"to", locked_peer->peer_id}};
            locked_peer->ws->send(message.dump());
        } else {
            session->error("Error: WebSocket missing onLocalDescription");
        }
    });

    auto install_sendrecv_handler = [sample_rate =
                                         sample_rate_](const std::shared_ptr<P2PPeer> &target,
                                                       const std::shared_ptr<rtc::Track> &track) {
        target->rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
            target->audio_ssrc, "audio", 109, sample_rate);
        auto handler = std::make_shared<rtc::OpusRtpPacketizer>(target->rtp_config);
        handler->addToChain(std::make_shared<rtc::OpusRtpDepacketizer>());
        handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        handler->addToChain(std::make_shared<rtc::RtcpSrReporter>(target->rtp_config));
        track->setMediaHandler(handler);
        target->audio_track = track;
    };

    if (!peer->is_polite) {
        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(109);
        audio.addSSRC(peer->audio_ssrc, "audio");
        peer->audio_track = peer->pc->addTrack(audio);
        install_sendrecv_handler(peer, peer->audio_track);
#ifdef P2P_GEM_VIDEO
        if (video_receivers_ > 0) {
            rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly);
            video.addH264Codec(102);
            peer->video_track = peer->pc->addTrack(video);
            video_negotiated_ = true;
            if (initializeVideoDecoder(peer)) {
                auto handler = std::make_shared<rtc::H264RtpDepacketizer>();
                handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
                peer->video_track->setMediaHandler(handler);
                peer->video_track->onFrame(
                    [weak_session, weak_peer](rtc::binary data, rtc::FrameInfo) {
                        auto session = weak_session.lock();
                        auto locked_peer = weak_peer.lock();
                        if (session && locked_peer && locked_peer->active) {
                            session->decodeVideoFrame(locked_peer, data);
                        }
                    });
                peer->video_track->onOpen([weak_session, weak_peer]() {
                    auto session = weak_session.lock();
                    auto locked_peer = weak_peer.lock();
                    if (session && locked_peer && locked_peer->active) {
                        session->log(PD_NORMAL, "Remote H264 video track open for peer %s",
                                     locked_peer->peer_id.c_str());
                    }
                });
            }
        }
#endif
        peer->audio_track->onFrame([weak_session, weak_peer](rtc::binary data, rtc::FrameInfo) {
            auto session = weak_session.lock();
            auto locked_peer = weak_peer.lock();
            if (session && locked_peer && locked_peer->active) {
                session->decodeAudio(locked_peer, data);
            }
        });
    }

    peer->pc->onTrack(
        [weak_session, weak_peer, install_sendrecv_handler](std::shared_ptr<rtc::Track> track) {
            auto session = weak_session.lock();
            auto locked_peer = weak_peer.lock();
            if (!session || !locked_peer || !locked_peer->active) {
                track->close();
                return;
            }
            auto description = track->description();
            if (description.type() == "video") {
#ifdef P2P_GEM_VIDEO
                if (session->video_receivers_ <= 0) {
                    session->log(PD_NORMAL,
                                 "Video track rejected; create [p2p.r.video] before connecting");
                    track->close();
                    return;
                }
                if (!session->initializeVideoDecoder(locked_peer)) {
                    session->error("Could not initialize the H264 video decoder");
                    track->close();
                    return;
                }
                session->video_negotiated_ = true;
                locked_peer->video_track = track;
                auto handler = std::make_shared<rtc::H264RtpDepacketizer>();
                handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
                track->setMediaHandler(handler);
                track->onFrame([weak_session, weak_peer](rtc::binary data, rtc::FrameInfo) {
                    auto frame_session = weak_session.lock();
                    auto frame_peer = weak_peer.lock();
                    if (frame_session && frame_peer && frame_peer->active) {
                        frame_session->decodeVideoFrame(frame_peer, data);
                    }
                });
                track->onOpen([weak_session, weak_peer]() {
                    auto open_session = weak_session.lock();
                    auto open_peer = weak_peer.lock();
                    if (open_session && open_peer && open_peer->active) {
                        open_session->log(PD_NORMAL, "Remote H264 video active for peer %s",
                                          open_peer->peer_id.c_str());
                    }
                });
#else
                session->log(PD_NORMAL, "Video track rejected; video is not implemented yet");
                track->close();
#endif
                return;
            }
            if (description.type() != "audio") {
                return;
            }
            track->onOpen([weak_session, weak_peer]() {
                auto open_session = weak_session.lock();
                auto open_peer = weak_peer.lock();
                if (open_session && open_peer && open_peer->active) {
                    open_session->log(PD_NORMAL, "Remote audio track active for peer %s",
                                      open_peer->peer_id.c_str());
                }
            });
            if (locked_peer->is_polite) {
                description.addSSRC(locked_peer->audio_ssrc, "audio");
                track->setDescription(description);
                install_sendrecv_handler(locked_peer, track);
            } else {
                auto handler = std::make_shared<rtc::OpusRtpDepacketizer>();
                handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
                track->setMediaHandler(handler);
            }
            track->onFrame([weak_session, weak_peer](rtc::binary data, rtc::FrameInfo) {
                auto frame_session = weak_session.lock();
                auto frame_peer = weak_peer.lock();
                if (frame_session && frame_peer && frame_peer->active) {
                    frame_session->decodeAudio(frame_peer, data);
                }
            });
        });

    auto install_data_channel = [weak_session,
                                 weak_peer](const std::shared_ptr<rtc::DataChannel> &channel) {
        auto session = weak_session.lock();
        auto locked_peer = weak_peer.lock();
        if (!session || !locked_peer || !locked_peer->active) {
            channel->close();
            return;
        }
        locked_peer->dc = channel;
        channel->onOpen([weak_session, weak_peer]() {
            auto open_session = weak_session.lock();
            auto open_peer = weak_peer.lock();
            if (open_session && open_peer && open_peer->active) {
                open_session->log(PD_DEBUG, "DataChannel open with peer '%s'",
                                  open_peer->username.c_str());
            }
        });
        channel->onMessage([weak_session, weak_peer](std::variant<rtc::binary, std::string> data) {
            auto message_session = weak_session.lock();
            auto message_peer = weak_peer.lock();
            if (!message_session || !message_peer || !message_peer->active) {
                return;
            }
            std::string payload;
            if (std::holds_alternative<std::string>(data)) {
                payload = std::get<std::string>(std::move(data));
            } else {
                const auto &binary = std::get<rtc::binary>(data);
                payload.assign(reinterpret_cast<const char *>(binary.data()), binary.size());
            }
            message_session->emit(
                {P2PEventType::Message, message_peer->username, std::move(payload)});
        });
    };
    if (!peer->is_polite) {
        install_data_channel(peer->pc->createDataChannel("data"));
    } else {
        peer->pc->onDataChannel(install_data_channel);
    }
    return true;
}

// ─────────────────────────────────────
#ifdef P2P_GEM_VIDEO
bool P2PSession::initializeVideoDecoder(const std::shared_ptr<P2PPeer> &peer) {
    std::lock_guard<std::mutex> lock(peer->video_mutex);
    if (peer->video_decoder) {
        return true;
    }
    peer->video_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    peer->video_decoder = peer->video_codec ? avcodec_alloc_context3(peer->video_codec) : nullptr;
    peer->video_frame = av_frame_alloc();
    peer->rgba_frame = av_frame_alloc();
    return peer->video_decoder && peer->video_frame && peer->rgba_frame &&
           avcodec_open2(peer->video_decoder, peer->video_codec, nullptr) >= 0;
}

// ─────────────────────────────────────
void P2PSession::decodeVideoFrame(const std::shared_ptr<P2PPeer> &peer, const rtc::binary &data) {
    std::lock_guard<std::mutex> lock(peer->video_mutex);
    if (!peer->video_decoder) {
        return;
    }
    if (data.empty()) {
        if (peer->video_decode_errors++ < 3) {
            error("Ignoring empty H264 access unit");
        }
        return;
    }
    if (!peer->video_encoded_logged) {
        peer->video_encoded_logged = true;
        log(PD_NORMAL, "Receiving encoded H264 frames from peer %s", peer->peer_id.c_str());
    }
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        return;
    }
    if (data.size() > static_cast<size_t>(INT_MAX) ||
        av_new_packet(packet, static_cast<int>(data.size())) < 0) {
        av_packet_free(&packet);
        return;
    }
    memcpy(packet->data, data.data(), data.size());
    const int result = avcodec_send_packet(peer->video_decoder, packet);
    av_packet_free(&packet);
    if (result < 0) {
        if (peer->video_decode_errors++ < 3) {
            char message[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(result, message, sizeof(message));
            error("H264 access unit (%zu bytes) rejected: %s", data.size(), message);
        }
        if (result == AVERROR_EOF) {
            avcodec_flush_buffers(peer->video_decoder);
        }
        return;
    }
    while (avcodec_receive_frame(peer->video_decoder, peer->video_frame) == 0) {
        const int width = peer->video_frame->width;
        const int height = peer->video_frame->height;
        peer->rgba_pixels.resize(static_cast<size_t>(width) * height * 4);
        peer->video_scaler =
            sws_getCachedContext(peer->video_scaler, width, height,
                                 static_cast<AVPixelFormat>(peer->video_frame->format), width,
                                 height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!peer->video_scaler) {
            return;
        }
        uint8_t *destination[] = {peer->rgba_pixels.data()};
        int strides[] = {width * 4};
        sws_scale(peer->video_scaler, peer->video_frame->data, peer->video_frame->linesize, 0,
                  height, destination, strides);
        peer->rgba_frame->width = width;
        peer->rgba_frame->height = height;
        ++peer->video_serial;
        if (!peer->video_decoded_logged) {
            peer->video_decoded_logged = true;
            log(PD_NORMAL, "Decoded GEM video frame: %dx%d", width, height);
        }
    }
}
#endif
