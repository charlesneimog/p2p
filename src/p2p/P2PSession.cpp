#include "P2PSession.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <variant>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <spdlog/spdlog.h>

using Json = nlohmann::json;

// ╭─────────────────────────────────────╮
// │               Logging               │
// ╰─────────────────────────────────────╯
static const char *CandidateTypeName(rtc::Candidate::Type type) {
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
static std::string FormatMessage(const char *format, va_list arguments) {
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, arguments);
    return buffer;
}

// ─────────────────────────────────────
static void InitializeRtcLogger() {
    static std::once_flag once;
    std::call_once(once, []() {
        rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel level, std::string message) {
            if (level == rtc::LogLevel::Fatal || level == rtc::LogLevel::Error) {
                spdlog::error("[libdatachannel] {}", message);
            } else {
                spdlog::warn("[libdatachannel] {}", message);
            }
        });
    });
}

// ╭─────────────────────────────────────╮
// │          Certificate Trust          │
// ╰─────────────────────────────────────╯
static bool IsReadableRegularFile(const std::string &path) {
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    std::ifstream file(path);
    return file.good();
}

struct CaBundleResolution {
    std::string m_Path;
    std::string m_Error;
};

#ifdef __APPLE__
static bool WritePemCertificate(FILE *file, const unsigned char *data, size_t size) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (std::fputs("-----BEGIN CERTIFICATE-----\n", file) == EOF) {
        return false;
    }

    size_t column = 0;
    for (size_t offset = 0; offset < size; offset += 3) {
        const size_t remaining = size - offset;
        const unsigned int value =
            static_cast<unsigned int>(data[offset]) << 16 |
            (remaining > 1 ? static_cast<unsigned int>(data[offset + 1]) << 8 : 0) |
            (remaining > 2 ? static_cast<unsigned int>(data[offset + 2]) : 0);
        const char encoded[] = {
            alphabet[(value >> 18) & 0x3f],
            alphabet[(value >> 12) & 0x3f],
            remaining > 1 ? alphabet[(value >> 6) & 0x3f] : '=',
            remaining > 2 ? alphabet[value & 0x3f] : '=',
        };
        for (const char character : encoded) {
            if (std::fputc(character, file) == EOF) {
                return false;
            }
            if (++column == 64) {
                if (std::fputc('\n', file) == EOF) {
                    return false;
                }
                column = 0;
            }
        }
    }
    if (column != 0 && std::fputc('\n', file) == EOF) {
        return false;
    }
    return std::fputs("-----END CERTIFICATE-----\n", file) != EOF;
}

// ─────────────────────────────────────
static CaBundleResolution CreateMacosCaBundle() {
    CFArrayRef anchors = nullptr;
    const OSStatus status = SecTrustCopyAnchorCertificates(&anchors);
    if (status != errSecSuccess || !anchors) {
        return {{},
                "Could not read macOS Keychain trust anchors (OSStatus " + std::to_string(status) +
                    ")"};
    }

    std::string path;
    std::vector<char> path_buffer;
    FILE *file = nullptr;
    int descriptor = -1;
    bool temporary_file_created = false;
    try {
        path = (std::filesystem::temp_directory_path() / "p2p-ca-bundle-XXXXXX").string();
        path_buffer.assign(path.begin(), path.end());
        path_buffer.push_back('\0');
        descriptor = mkstemp(path_buffer.data());
        if (descriptor < 0) {
            CFRelease(anchors);
            return {{},
                    "Could not create temporary macOS CA bundle: " +
                        std::string(std::strerror(errno))};
        }
        temporary_file_created = true;
        path = path_buffer.data();
        fchmod(descriptor, S_IRUSR | S_IWUSR);
        file = fdopen(descriptor, "w");
        if (!file) {
            const std::string error = std::strerror(errno);
            close(descriptor);
            descriptor = -1;
            std::remove(path.c_str());
            CFRelease(anchors);
            return {{}, "Could not open temporary macOS CA bundle: " + error};
        }
        descriptor = -1; // Owned by file after fdopen succeeds.
    } catch (const std::exception &exception) {
        if (descriptor >= 0) {
            close(descriptor);
        }
        if (temporary_file_created) {
            unlink(path_buffer.data());
        }
        CFRelease(anchors);
        return {{},
                "Could not locate the macOS temporary directory: " + std::string(exception.what())};
    }

    size_t certificate_count = 0;
    bool write_succeeded = true;
    const CFIndex anchor_count = CFArrayGetCount(anchors);
    for (CFIndex index = 0; index < anchor_count && write_succeeded; ++index) {
        const void *value = CFArrayGetValueAtIndex(anchors, index);
        if (!value || CFGetTypeID(value) != SecCertificateGetTypeID()) {
            continue;
        }
        const SecCertificateRef certificate =
            reinterpret_cast<SecCertificateRef>(const_cast<void *>(value));
        CFDataRef der = SecCertificateCopyData(certificate);
        if (!der) {
            continue;
        }
        write_succeeded = WritePemCertificate(file, CFDataGetBytePtr(der),
                                              static_cast<size_t>(CFDataGetLength(der)));
        CFRelease(der);
        if (write_succeeded) {
            ++certificate_count;
        }
    }
    CFRelease(anchors);

    if (std::fclose(file) != 0) {
        write_succeeded = false;
    }
    if (!write_succeeded || certificate_count == 0) {
        std::remove(path.c_str());
        return {{}, "Could not export macOS Keychain trust anchors to a temporary CA bundle"};
    }
    return {path, {}};
}

class MacosCaBundle {
public:
    MacosCaBundle() : m_Resolution(CreateMacosCaBundle()) {}
    ~MacosCaBundle() {
        if (!m_Resolution.m_Path.empty()) {
            std::remove(m_Resolution.m_Path.c_str());
        }
    }

    const CaBundleResolution &Resolution() const {
        return m_Resolution;
    }

private:
    CaBundleResolution m_Resolution;
};

static const CaBundleResolution &ResolveMacosCaBundle() {
    static const MacosCaBundle bundle;
    return bundle.Resolution();
}
#endif

static CaBundleResolution ResolveCaBundle() {
    if (const char *environment_path = std::getenv("SSL_CERT_FILE");
        environment_path && environment_path[0]) {
        if (IsReadableRegularFile(environment_path)) {
            return {environment_path, {}};
        }
        return {{},
                std::string("SSL_CERT_FILE does not name a readable CA bundle: '") +
                    environment_path + "'"};
    }

#ifdef __APPLE__
    // Static OpenSSL cannot query Keychain trust itself. Export the current
    // machine's trusted anchors at runtime instead of shipping a stale snapshot.
    return ResolveMacosCaBundle();
#else
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
        if (IsReadableRegularFile(candidate)) {
            return {candidate, {}};
        }
    }

    // Leaving this unset preserves libdatachannel/OpenSSL's default trust lookup.
    return {};
#endif
}

// ╭─────────────────────────────────────╮
// │     Session Lifecycle and Events    │
// ╰─────────────────────────────────────╯
std::shared_ptr<P2PSession> P2PSession::Create(const std::string &id, int sample_rate,
                                               MainThreadDispatcher dispatcher) {
    auto session =
        std::shared_ptr<P2PSession>(new P2PSession(id, sample_rate, std::move(dispatcher)));
    session->Initialize();
    return session;
}

// ─────────────────────────────────────
P2PSession::P2PSession(std::string id, int sample_rate, MainThreadDispatcher dispatcher)
    : m_Id(std::move(id)), m_SampleRate(sample_rate),
      m_MainThreadDispatcher(std::move(dispatcher)) {}

void P2PSession::Initialize() {
    InitializeRtcLogger();
    RebuildRealtimePeersLocked();
    if (m_SampleRate != 48000) {
        m_Available = false;
    }
}

// ─────────────────────────────────────
P2PSession::~P2PSession() {
    m_Available = false;
    Disconnect();
}

// ─────────────────────────────────────
const std::string &P2PSession::Id() const {
    return m_Id;
}

// ─────────────────────────────────────
bool P2PSession::Available() const {
    return m_Available;
}

// ─────────────────────────────────────
bool P2PSession::Connected() const {
    return m_WebsocketConnected;
}

// ─────────────────────────────────────
void P2PSession::Deactivate() {
    if (!m_Available.exchange(false)) {
        return;
    }
    Disconnect();
    std::lock_guard<std::mutex> lock(m_ListenersMutex);
    m_Listeners.clear();
}

// ─────────────────────────────────────
uint64_t P2PSession::AddListener(Listener listener) {
    const uint64_t id = m_NextListenerId++;
    std::lock_guard<std::mutex> lock(m_ListenersMutex);
    m_Listeners[id] = std::move(listener);
    return id;
}

// ─────────────────────────────────────
void P2PSession::RemoveListener(uint64_t listener_id) {
    std::lock_guard<std::mutex> lock(m_ListenersMutex);
    m_Listeners.erase(listener_id);
}

// ─────────────────────────────────────
void P2PSession::Emit(P2PEvent event) {
    std::vector<Listener> listeners;
    {
        std::lock_guard<std::mutex> lock(m_ListenersMutex);
        listeners.reserve(m_Listeners.size());
        for (const auto &entry : m_Listeners) {
            listeners.push_back(entry.second);
        }
    }
    m_MainThreadDispatcher([listeners = std::move(listeners), event = std::move(event)]() {
        for (const Listener &listener : listeners) {
            listener(event);
        }
    });
}

// ─────────────────────────────────────
void P2PSession::Log(P2PLogLevel level, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::string message = FormatMessage(format, arguments);
    va_end(arguments);
    Emit({P2PEventType::Log, {}, std::move(message), 0, level});
}

// ─────────────────────────────────────
void P2PSession::Error(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::string message = FormatMessage(format, arguments);
    va_end(arguments);
    Emit({P2PEventType::Error, {}, std::move(message), 0, P2PLogLevel::Error});
}

// ╭─────────────────────────────────────╮
// │        Signalling Connection        │
// ╰─────────────────────────────────────╯
void P2PSession::Connect(const std::string &websocket_url, const std::string &room,
                         const std::string &local_username) {
    if (!m_Available) {
        Error("Session '%s' is unavailable", m_Id.c_str());
        return;
    }
    std::shared_ptr<rtc::WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(m_WebsocketMutex);
        if (!m_Websocket) {
            const CaBundleResolution ca_bundle = ResolveCaBundle();
            if (!ca_bundle.m_Error.empty()) {
                Error("%s", ca_bundle.m_Error.c_str());
                return;
            }
            rtc::WebSocket::Configuration configuration;
            // libdatachannel tries resolved addresses sequentially. Its 30-second
            // default leaves enough time to fall back from a broken IPv6 route to
            // IPv4 and to complete the TLS handshake on slower networks.
            configuration.connectionTimeout = std::chrono::seconds(30);
            if (!ca_bundle.m_Path.empty()) {
                configuration.caCertificatePemFile = ca_bundle.m_Path;
            }
            m_WebsocketCaBundle = ca_bundle.m_Path;
            m_Websocket = std::make_shared<rtc::WebSocket>(configuration);
        }
        websocket = m_Websocket;
        if (websocket->isOpen()) {
            Error("Already Connected");
            return;
        }
        m_Room = room;
        m_LocalUsername = local_username;
    }
    InstallWebSocketCallbacks();
    const std::string url = websocket_url + "/?room=" + room;
    websocket->open(url);
}

// ─────────────────────────────────────
void P2PSession::InstallWebSocketCallbacks() {
    std::shared_ptr<rtc::WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(m_WebsocketMutex);
        websocket = m_Websocket;
    }
    if (!websocket) {
        return;
    }
    std::weak_ptr<P2PSession> weak_session = shared_from_this();
    websocket->onOpen([weak_session]() {
        std::shared_ptr<P2PSession> session = weak_session.lock();
        if (!session || !session->m_Available) {
            return;
        }
        std::shared_ptr<rtc::WebSocket> socket;
        std::string username;
        std::string room;
        {
            std::lock_guard<std::mutex> lock(session->m_WebsocketMutex);
            socket = session->m_Websocket;
            username = session->m_LocalUsername;
            room = session->m_Room;
        }
        if (socket) {
            Json join = {{"type", "join"}, {"name", username}};
            socket->send(join.dump());
        }
        session->m_WebsocketConnected = true;
        session->Log(P2PLogLevel::Normal, "Connected to the room: '%s'", room.c_str());
        session->Emit({P2PEventType::Connected});
        session->EmitConnectionCount();
    });
    websocket->onClosed([weak_session]() {
        std::shared_ptr<P2PSession> session = weak_session.lock();
        if (!session) {
            return;
        }
        const bool was_connected = session->m_WebsocketConnected.exchange(false);
        session->RemoveAllPeers();
        if (was_connected) {
            session->Emit({P2PEventType::Disconnected});
        }
        session->EmitConnectionCount();
    });
    websocket->onError([weak_session](std::string message) {
        if (std::shared_ptr<P2PSession> session = weak_session.lock()) {
            if (message == "TLS connection failed") {
                std::string ca_bundle;
                {
                    std::lock_guard<std::mutex> lock(session->m_WebsocketMutex);
                    ca_bundle = session->m_WebsocketCaBundle;
                }
                if (!ca_bundle.empty()) {
                    session->Error("WebSocket TLS handshake failed while using CA bundle '%s'; "
                                   "see the preceding [libdatachannel] error",
                                   ca_bundle.c_str());
                } else {
                    session->Error("WebSocket TLS handshake failed; see the preceding "
                                   "[libdatachannel] error (no CA bundle was detected; "
                                   "SSL_CERT_FILE can override it)");
                }
            } else {
                session->Error("WebSocket error: %s", message.c_str());
            }
        }
    });
    websocket->onMessage([weak_session](std::variant<rtc::binary, std::string> data) {
        std::shared_ptr<P2PSession> session = weak_session.lock();
        if (!session || !session->m_Available) {
            return;
        }
        std::string payload;
        if (std::holds_alternative<std::string>(data)) {
            payload = std::get<std::string>(std::move(data));
        } else {
            const rtc::binary &binary = std::get<rtc::binary>(data);
            payload.assign(reinterpret_cast<const char *>(binary.data()), binary.size());
        }
        session->OnSignallingMessage(payload);
    });
}

// ─────────────────────────────────────
void P2PSession::OnSignallingMessage(const std::string &payload) {
    try {
        const Json data = Json::parse(payload);
        spdlog::info("{}", data.dump(4));
        const std::string type = data.contains("type") ? data["type"].get<std::string>() : "";
        if (type == "welcome") {
            Welcome(data);
        } else if (type == "peer-joined") {
            PeerJoined(data);
        } else if (type == "existing-peers") {
            ExistingPeers(data);
        } else if (type == "offer") {
            Offer(data);
        } else if (type == "ice-candidate") {
            IceCandidate(data);
        } else if (type == "answer") {
            Answer(data);
        } else if (type == "peer-left") {
            PeerLeft(data);
        } else {
            Error("%s", payload.c_str());
        }
    } catch (const std::exception &exception) {
        Error("Invalid signalling message: %s", exception.what());
    }
}

// ─────────────────────────────────────
void P2PSession::Disconnect() {
    std::shared_ptr<rtc::WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(m_WebsocketMutex);
        websocket = m_Websocket;
        if (websocket) {
            websocket->resetCallbacks();
        }
        m_Websocket.reset();
        m_WebsocketCaBundle.clear();
        m_LocalPeerId.clear();
    }
    if (websocket) {
        websocket->close();
    }
    RemoveAllPeers();
    const bool was_connected = m_WebsocketConnected.exchange(false);
    if (was_connected) {
        Emit({P2PEventType::Disconnected});
    }
    EmitConnectionCount();
    Log(P2PLogLevel::Normal, "Disconnected");
}

// ╭─────────────────────────────────────╮
// │    Streaming and Receiver Claims    │
// ╰─────────────────────────────────────╯
void P2PSession::SetStreaming(bool enabled) {
    if (m_WantsStream.exchange(enabled) != enabled) {
        Log(P2PLogLevel::Normal, "Stream %s", enabled ? "active" : "paused");
    }
    const auto peers = PeerSnapshot();
    for (const auto &peer : peers) {
        peer->m_IsStreaming = enabled;
    }
}

// ─────────────────────────────────────
bool P2PSession::Streaming() const {
    return m_WantsStream;
}

// ─────────────────────────────────────
void P2PSession::SendMessage(const std::string &text) {
    Json payload = {{"type", "message"}, {"text", text}};
    const std::string serialized = payload.dump(4);
    SendJson(serialized);
}

// ─────────────────────────────────────
void P2PSession::SendJson(const std::string &json_text) {
    const auto peers = PeerSnapshot();
    for (const auto &peer : peers) {
        if (peer->m_DataChannel && peer->m_DataChannel->isOpen()) {
            peer->m_DataChannel->send(json_text);
        }
    }
}

// ─────────────────────────────────────
int P2PSession::ConnectionCount() const {
    int count = 0;
    const auto peers = PeerSnapshot();
    for (const auto &peer : peers) {
        if (peer->m_Active && peer->m_Connected) {
            ++count;
        }
    }
    return count;
}

// ─────────────────────────────────────
void P2PSession::EmitConnectionCount() {
    Emit({P2PEventType::Connections, {}, {}, ConnectionCount()});
}

// ─────────────────────────────────────
void P2PSession::Report() {
    EmitConnectionCount();
}

// ─────────────────────────────────────
bool P2PSession::ClaimController(const void *owner) {
    std::lock_guard<std::mutex> lock(m_ClaimsMutex);
    if (m_ControllerOwner && m_ControllerOwner != owner) {
        return false;
    }
    m_ControllerOwner = owner;
    return true;
}

// ─────────────────────────────────────
void P2PSession::ReleaseController(const void *owner) {
    std::lock_guard<std::mutex> lock(m_ClaimsMutex);
    if (m_ControllerOwner == owner) {
        m_ControllerOwner = nullptr;
    }
}

// ─────────────────────────────────────
bool P2PSession::ClaimAudioSender(const void *owner) {
    std::lock_guard<std::mutex> lock(m_ClaimsMutex);
    if (m_AudioSenderOwner && m_AudioSenderOwner != owner) {
        return false;
    }
    m_AudioSenderOwner = owner;
    return true;
}

// ─────────────────────────────────────
void P2PSession::ReleaseAudioSender(const void *owner) {
    std::lock_guard<std::mutex> lock(m_ClaimsMutex);
    if (m_AudioSenderOwner == owner) {
        m_AudioSenderOwner = nullptr;
    }
}

// ─────────────────────────────────────
bool P2PSession::ClaimAudioReceiver(const std::string &username, const void *owner) {
    std::lock_guard<std::mutex> lock(m_ClaimsMutex);
    auto iterator = m_AudioReceiverOwners.find(username);
    if (iterator != m_AudioReceiverOwners.end() && iterator->second != owner) {
        return false;
    }
    m_AudioReceiverOwners[username] = owner;
    return true;
}

// ─────────────────────────────────────
void P2PSession::ReleaseAudioReceiver(const std::string &username, const void *owner) {
    std::lock_guard<std::mutex> lock(m_ClaimsMutex);
    auto iterator = m_AudioReceiverOwners.find(username);
    if (iterator != m_AudioReceiverOwners.end() && iterator->second == owner) {
        m_AudioReceiverOwners.erase(iterator);
    }
}

// ─────────────────────────────────────
void P2PSession::RegisterVideoReceiver() {
    const int previous = m_VideoReceivers.fetch_add(1);
    if (previous == 0 && !PeerSnapshot().empty() && !m_VideoNegotiated) {
        Error("[p2p.r.video] reconnect the session to enable video");
    }
}

// ─────────────────────────────────────
void P2PSession::UnregisterVideoReceiver() {
    int current = m_VideoReceivers;
    while (current > 0 && !m_VideoReceivers.compare_exchange_weak(current, current - 1)) {
    }
}

// ─────────────────────────────────────
bool P2PSession::VideoNegotiated() const {
    return m_VideoNegotiated;
}

// ╭─────────────────────────────────────╮
// │           Peer Management           │
// ╰─────────────────────────────────────╯
std::vector<std::shared_ptr<P2PPeer>> P2PSession::PeerSnapshot() const {
    std::vector<std::shared_ptr<P2PPeer>> peers;
    std::lock_guard<std::mutex> lock(m_PeersMutex);
    peers.reserve(m_PeersById.size());
    for (const auto &entry : m_PeersById) {
        peers.push_back(entry.second);
    }
    return peers;
}

// ─────────────────────────────────────
void P2PSession::PushOutgoingAudio(const float *samples, int count, int channels) {
    const auto *peers = m_RealtimePeers.load(std::memory_order_acquire);
    if (!peers) {
        return;
    }
    channels = channels == 2 ? 2 : 1;
    for (const auto &peer : *peers) {
        if (!peer->m_Active || !peer->m_Connected) {
            continue;
        }
        for (int index = 0; index < count; ++index) {
            const float left = samples[index];
            const float right = channels == 2 ? samples[count + index] : left;
            peer->m_SendBuffer.push({left, right, channels});
        }
    }
}

P2PPeerResolution P2PSession::ResolvePeer(const std::string &username) const {
    std::lock_guard<std::mutex> lock(m_PeersMutex);
    auto iterator = m_PeersByName.find(username);
    if (iterator == m_PeersByName.end()) {
        return {};
    }
    P2PPeerResolution result;
    int matches = 0;
    for (const auto &weak_peer : iterator->second) {
        std::shared_ptr<P2PPeer> peer = weak_peer.lock();
        if (peer && peer->m_Active) {
            result.m_Peer = std::move(peer);
            ++matches;
        }
    }
    if (matches != 1) {
        result.m_Peer.reset();
        result.m_Ambiguous = matches > 1;
    }
    return result;
}

// ─────────────────────────────────────
int P2PSession::FrameSize() const {
    return m_FrameSize;
}

// ─────────────────────────────────────
int P2PSession::SampleRate() const {
    return m_SampleRate;
}

// ╭─────────────────────────────────────╮
// │         Signalling Messages         │
// ╰─────────────────────────────────────╯
void P2PSession::Welcome(const Json &data) {
    const std::string id = data["id"].get<std::string>();
    {
        std::lock_guard<std::mutex> lock(m_WebsocketMutex);
        m_LocalPeerId = id;
    }
    Log(P2PLogLevel::Normal, "Connected ID: %s", id.substr(0, 6).c_str());
}

// ─────────────────────────────────────
std::shared_ptr<P2PPeer> P2PSession::AddPeer(const std::string &peer_id,
                                             const std::string &username) {
    std::shared_ptr<P2PPeer> peer;
    {
        std::lock_guard<std::mutex> lock(m_PeersMutex);
        auto existing = m_PeersById.find(peer_id);
        if (existing != m_PeersById.end()) {
            return existing->second;
        }
        if (m_PeersById.size() >= 8) {
            Error("No free nodes available for peer %s", peer_id.c_str());
            return {};
        }
        peer = std::make_shared<P2PPeer>(peer_id, username);
        if (!peer->InitializeEncoder(m_SampleRate)) {
            Error("Opus encoder error for peer '%s'", username.c_str());
            return {};
        }
        m_PeersById[peer_id] = peer;
        m_PeersByName[username].push_back(peer);
        RebuildRealtimePeersLocked();
    }
    {
        std::lock_guard<std::mutex> lock(m_WebsocketMutex);
        peer->m_Websocket = m_Websocket;
        const bool should_be_caller = m_LocalPeerId < peer_id;
        peer->m_IsPolite = !should_be_caller;
        peer->m_MakingOffer = should_be_caller;
    }
    peer->m_IsStreaming = m_WantsStream.load();
    peer->StartTransmission(m_FrameSize, m_SampleRate);
    if (!SetupWebRtc(peer)) {
        RemovePeer(peer_id, false);
        return {};
    }
    Emit({P2PEventType::PeerJoined, username});
    return peer;
}

// ─────────────────────────────────────
std::shared_ptr<P2PPeer> P2PSession::FindPeerById(const std::string &peer_id) const {
    std::lock_guard<std::mutex> lock(m_PeersMutex);
    auto iterator = m_PeersById.find(peer_id);
    return iterator == m_PeersById.end() ? std::shared_ptr<P2PPeer>() : iterator->second;
}

// ─────────────────────────────────────
void P2PSession::RemovePeer(const std::string &peer_id, bool notify) {
    std::shared_ptr<P2PPeer> peer;
    {
        std::lock_guard<std::mutex> lock(m_PeersMutex);
        auto iterator = m_PeersById.find(peer_id);
        if (iterator == m_PeersById.end()) {
            return;
        }
        peer = iterator->second;
        m_RetiredPeers.push_back(peer);
        m_PeersById.erase(iterator);
        auto names = m_PeersByName.find(peer->m_Username);
        if (names != m_PeersByName.end()) {
            auto &entries = names->second;
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [&peer](const std::weak_ptr<P2PPeer> &candidate) {
                                             std::shared_ptr<P2PPeer> locked = candidate.lock();
                                             return !locked || locked == peer;
                                         }),
                          entries.end());
            if (entries.empty()) {
                m_PeersByName.erase(names);
            }
        }
        RebuildRealtimePeersLocked();
    }
    peer->Shutdown();
    if (notify) {
        Emit({P2PEventType::PeerLeft, peer->m_Username});
    }
    EmitConnectionCount();
}

// ─────────────────────────────────────
void P2PSession::RemoveAllPeers() {
    std::vector<std::shared_ptr<P2PPeer>> peers;
    {
        std::lock_guard<std::mutex> lock(m_PeersMutex);
        peers.reserve(m_PeersById.size());
        for (auto &entry : m_PeersById) {
            peers.push_back(entry.second);
            m_RetiredPeers.push_back(std::move(entry.second));
        }
        m_PeersById.clear();
        m_PeersByName.clear();
        RebuildRealtimePeersLocked();
    }
    for (const auto &peer : peers) {
        peer->Shutdown();
    }
}

// ─────────────────────────────────────
void P2PSession::RebuildRealtimePeersLocked() {
    auto peers = std::make_shared<std::vector<std::shared_ptr<P2PPeer>>>();
    peers->reserve(m_PeersById.size());
    for (const auto &entry : m_PeersById) {
        peers->push_back(entry.second);
    }
    auto immutable = std::static_pointer_cast<const std::vector<std::shared_ptr<P2PPeer>>>(peers);
    m_RetainedPeerSnapshots.push_back(immutable);
    m_RealtimePeers.store(immutable.get(), std::memory_order_release);
}

// ─────────────────────────────────────
void P2PSession::PeerJoined(const Json &data) {
    const std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    const std::string peer_name = data.contains("peer") && data["peer"].contains("name")
                                      ? data["peer"]["name"].get<std::string>()
                                      : from_peer;
    std::shared_ptr<P2PPeer> peer = AddPeer(from_peer, peer_name);
    if (!peer) {
        return;
    }
    if (!peer->m_IsPolite) {
        if (!peer->m_LocalOfferSent) {
            peer->m_PeerConnection->setLocalDescription();
        }
    } else {
        Log(P2PLogLevel::Normal, "Waiting for offer from %s (I am callee)", from_peer.c_str());
    }
    Log(P2PLogLevel::Normal, "Peer '%s' joined", peer->m_Username.c_str());
}

// ─────────────────────────────────────
void P2PSession::ExistingPeers(const Json &data) {
    for (const auto &description : data["peers"]) {
        const std::string peer_id = description["id"].get<std::string>();
        const std::string peer_name = description["name"].get<std::string>();
        std::shared_ptr<P2PPeer> peer = AddPeer(peer_id, peer_name);
        if (!peer) {
            continue;
        }
        if (!peer->m_IsPolite && !peer->m_LocalOfferSent) {
            peer->m_PeerConnection->setLocalDescription();
            Log(P2PLogLevel::Normal, "Connecting to existing peer '%s' (%s)", peer_name.c_str(),
                peer_id.substr(0, 6).c_str());
        } else if (!peer->m_IsPolite) {
            Log(P2PLogLevel::Normal, "Connecting to existing peer '%s' (%s)", peer_name.c_str(),
                peer_id.substr(0, 6).c_str());
        } else {
            Log(P2PLogLevel::Normal, "Waiting for offer from existing peer '%s' (%s)",
                peer_name.c_str(), peer_id.substr(0, 6).c_str());
        }
    }
    EmitConnectionCount();
}

// ─────────────────────────────────────
void P2PSession::Offer(const Json &data) {
    const std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    std::shared_ptr<P2PPeer> peer = FindPeerById(from_peer);
    if (!peer) {
        peer = AddPeer(from_peer, from_peer);
        if (!peer) {
            return;
        }
    }
    const bool offer_collision =
        peer->m_MakingOffer ||
        peer->m_PeerConnection->signalingState() != rtc::PeerConnection::SignalingState::Stable;
    if (offer_collision && !peer->m_IsPolite) {
        Log(P2PLogLevel::Debug, "Glare: ignoring offer from %s (impolite)", from_peer.c_str());
        peer->m_IgnoreOffer = true;
        return;
    }
    peer->m_IgnoreOffer = false;

    std::string sdp;
    if (data["sdp"].is_object()) {
        sdp = data["sdp"]["sdp"].get<std::string>();
    } else if (data["sdp"].is_string()) {
        sdp = data["sdp"].get<std::string>();
    } else {
        Error("Invalid SDP format");
        return;
    }
    try {
        rtc::Description description(sdp, "offer");
        ConfigureVideoMedia(description);
        peer->m_PeerConnection->setRemoteDescription(std::move(description));
        peer->m_RemoteDescriptionSet = true;
        FlushPendingCandidates(peer);
        peer->m_MakingOffer = false;
        peer->m_AnsweringOffer = true;
        peer->m_PeerConnection->setLocalDescription();
    } catch (const std::exception &exception) {
        peer->m_AnsweringOffer = false;
        Error("Failed to set remote description (offer): %s", exception.what());
    }
}

// ─────────────────────────────────────
void P2PSession::Answer(const Json &data) {
    const std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    std::shared_ptr<P2PPeer> peer = FindPeerById(from_peer);
    if (!peer || !peer->m_PeerConnection) {
        return;
    }
    if (peer->m_PeerConnection->signalingState() !=
        rtc::PeerConnection::SignalingState::HaveLocalOffer) {
        Log(P2PLogLevel::Debug,
            "Ignoring unexpected answer from %s; signaling state is not HaveLocalOffer",
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
        peer->m_PeerConnection->setRemoteDescription(std::move(description));
        peer->m_MakingOffer = false;
        peer->m_RemoteDescriptionSet = true;
        peer->m_IgnoreOffer = false;
        FlushPendingCandidates(peer);
    } catch (const std::exception &exception) {
        Error("Failed to set remote description (answer): %s", exception.what());
    }
}

// ─────────────────────────────────────
void P2PSession::IceCandidate(const Json &data) {
    const std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    std::shared_ptr<P2PPeer> peer = FindPeerById(from_peer);
    if (!peer || !peer->m_PeerConnection || peer->m_IgnoreOffer || !data.contains("candidate") ||
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
    if (!peer->m_RemoteDescriptionSet || peer->m_PeerConnection->signalingState() ==
                                             rtc::PeerConnection::SignalingState::HaveLocalOffer) {
        peer->m_PendingRemoteCandidates.push_back({candidate_text, mid});
        Log(P2PLogLevel::Debug, "Queuing ICE candidate from %s, mid=%s", from_peer.c_str(),
            mid.c_str());
        return;
    }
    try {
        peer->m_PeerConnection->addRemoteCandidate(rtc::Candidate(candidate_text, mid));
    } catch (const std::exception &exception) {
        Error("Failed to add ICE candidate mid=%s: %s", mid.c_str(), exception.what());
    }
}

// ─────────────────────────────────────
void P2PSession::PeerLeft(const Json &data) {
    const std::string from_peer = data.contains("from") ? data["from"].get<std::string>() : "";
    std::shared_ptr<P2PPeer> peer = FindPeerById(from_peer);
    if (peer) {
        Log(P2PLogLevel::Normal, "Peer '%s' left", peer->m_Username.c_str());
        RemovePeer(from_peer, true);
    } else {
        EmitConnectionCount();
    }
}

// ─────────────────────────────────────
void P2PSession::FlushPendingCandidates(const std::shared_ptr<P2PPeer> &peer) {
    for (const auto &candidate : peer->m_PendingRemoteCandidates) {
        try {
            peer->m_PeerConnection->addRemoteCandidate(
                rtc::Candidate(candidate.m_Candidate, candidate.m_Mid));
            Log(P2PLogLevel::Debug, "Flushed candidate for peer %s", peer->m_PeerId.c_str());
        } catch (const std::exception &exception) {
            Error("Failed to add queued candidate: %s", exception.what());
        }
    }
    peer->m_PendingRemoteCandidates.clear();
}

// ─────────────────────────────────────
void P2PSession::ResetPeerConnection(const std::shared_ptr<P2PPeer> &peer) {
    peer->m_IsStreaming = false;
    peer->m_RemoteDescriptionSet = false;
    peer->m_MakingOffer = false;
    peer->m_IgnoreOffer = false;
    peer->m_AnsweringOffer = false;
    peer->m_LocalOfferSent = false;
    peer->m_PoliteMediaOfferSent = false;
    peer->m_StunWarningReported = false;
    peer->Shutdown();
}

// ─────────────────────────────────────
bool P2PSession::CreatePeerDecoder(const std::shared_ptr<P2PPeer> &peer) {
    std::lock_guard<std::mutex> lock(peer->m_OpusDecMonoMutex);
    if (peer->m_OpusDecMono) {
        opus_decoder_destroy(peer->m_OpusDecMono);
    }
    int error_code = OPUS_OK;
    peer->m_OpusDecMono = opus_decoder_create(m_SampleRate, 1, &error_code);
    if (error_code != OPUS_OK) {
        peer->m_OpusDecMono = nullptr;
        Error("Opus decoder error for peer '%s': %d", peer->m_Username.c_str(), error_code);
        return false;
    }
    return true;
}

// ╭─────────────────────────────────────╮
// │            Audio Decoding           │
// ╰─────────────────────────────────────╯
void P2PSession::DecodeAudio(const std::shared_ptr<P2PPeer> &peer, const rtc::binary &data) {
    constexpr int maximum_samples = 5760;
    float pcm[maximum_samples];
    int samples;
    {
        std::lock_guard<std::mutex> lock(peer->m_OpusDecMonoMutex);
        if (!peer->m_OpusDecMono) {
            Error("Opus decode not initialized");
            return;
        }
        samples = opus_decode_float(peer->m_OpusDecMono,
                                    reinterpret_cast<const unsigned char *>(data.data()),
                                    static_cast<opus_int32>(data.size()), pcm, maximum_samples, 0);
    }
    if (samples > 0) {
        for (int index = 0; index < samples; ++index) {
            peer->m_ReceiveBuffer.push(pcm[index]);
        }
    } else if (samples < 0) {
        Error("Opus decode failed: %d, bytes=%zu", samples, data.size());
    }
}

// ─────────────────────────────────────
void P2PSession::UpdateConnectionState(const std::shared_ptr<P2PPeer> &peer,
                                       rtc::PeerConnection::State state) {
    const bool now_connected = state == rtc::PeerConnection::State::Connected;
    const bool was_connected = peer->m_Connected.exchange(now_connected);
    if (was_connected != now_connected || state == rtc::PeerConnection::State::Failed ||
        state == rtc::PeerConnection::State::Disconnected ||
        state == rtc::PeerConnection::State::Closed) {
        EmitConnectionCount();
    }
}

// ─────────────────────────────────────
void P2PSession::WarnIfNotStunPair(const std::shared_ptr<P2PPeer> &peer) {
    if (peer->m_StunWarningReported || !peer->m_PeerConnection) {
        return;
    }
    rtc::Candidate local;
    rtc::Candidate remote;
    if (!peer->m_PeerConnection->getSelectedCandidatePair(&local, &remote)) {
        return;
    }
    peer->m_StunWarningReported = true;
    const bool uses_stun = local.type() == rtc::Candidate::Type::ServerReflexive ||
                           remote.type() == rtc::Candidate::Type::ServerReflexive;
    if (!uses_stun) {
        Error("Warning: selected ICE pair for peer '%s' is not STUN/srflx "
              "(local=%s, remote=%s)",
              peer->m_Username.c_str(), CandidateTypeName(local.type()),
              CandidateTypeName(remote.type()));
    }
}

// ╭─────────────────────────────────────╮
// │          Media Negotiation          │
// ╰─────────────────────────────────────╯
void P2PSession::ConfigureVideoMedia(rtc::Description &description) {
    for (int index = 0; index < description.mediaCount(); ++index) {
        auto media = description.media(index);
        if (!std::holds_alternative<rtc::Description::Media *>(media)) {
            continue;
        }
        auto *remote_media = std::get<rtc::Description::Media *>(media);
        if (!remote_media || remote_media->type() != "video") {
            continue;
        }
#ifdef P2P_VIDEO
        if (m_VideoReceivers <= 0) {
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
            Log(P2PLogLevel::Normal, "Offered video codec: PT %d %s", payload, map->format.c_str());
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
            m_VideoNegotiated = true;
            Log(P2PLogLevel::Normal, "Accepting remote H264 video");
        } else {
            remote_media->setDirection(rtc::Description::Direction::Inactive);
            remote_media->markRemoved();
            Error("Peer did not offer H264; video disabled");
        }
#else
        remote_media->setDirection(rtc::Description::Direction::Inactive);
        remote_media->markRemoved();
#endif
    }
}

// ─────────────────────────────────────
bool P2PSession::SetupWebRtc(const std::shared_ptr<P2PPeer> &peer) {
    rtc::Configuration configuration;
    configuration.iceServers.emplace_back("stun:stun.l.google.com:19302");
    peer->m_PeerConnection = std::make_shared<rtc::PeerConnection>(configuration);
    if (!CreatePeerDecoder(peer)) {
        peer->m_PeerConnection->close();
        peer->m_PeerConnection.reset();
        return false;
    }
    peer->m_RemoteDescriptionSet = false;
    peer->m_StunWarningReported = false;
    Log(P2PLogLevel::Debug, "Local peer is polite=%d", peer->m_IsPolite);

    std::weak_ptr<P2PSession> weak_session = shared_from_this();
    std::weak_ptr<P2PPeer> weak_peer = peer;
    peer->m_PeerConnection->onStateChange(
        [weak_session, weak_peer](rtc::PeerConnection::State state) {
            std::shared_ptr<P2PSession> session = weak_session.lock();
            std::shared_ptr<P2PPeer> locked_peer = weak_peer.lock();
            if (session && locked_peer && locked_peer->m_Active) {
                session->UpdateConnectionState(locked_peer, state);
            }
        });
    peer->m_PeerConnection->onIceStateChange(
        [weak_session, weak_peer](rtc::PeerConnection::IceState state) {
            std::shared_ptr<P2PSession> session = weak_session.lock();
            std::shared_ptr<P2PPeer> locked_peer = weak_peer.lock();
            if (!session || !locked_peer || !locked_peer->m_Active) {
                return;
            }
            if (state == rtc::PeerConnection::IceState::Connected ||
                state == rtc::PeerConnection::IceState::Completed) {
                // session->warnIfNotStunPair(locked_peer);
            }
        });
    peer->m_PeerConnection->onLocalCandidate([weak_session, weak_peer](rtc::Candidate candidate) {
        std::shared_ptr<P2PSession> session = weak_session.lock();
        std::shared_ptr<P2PPeer> locked_peer = weak_peer.lock();
        if (!session || !locked_peer || !locked_peer->m_Active || !locked_peer->m_Websocket ||
            locked_peer->m_PeerId.empty()) {
            return;
        }
        Json message = {
            {"type", "ice-candidate"},
            {"to", locked_peer->m_PeerId},
            {"candidate", {{"candidate", std::string(candidate)}, {"sdpMid", candidate.mid()}}}};
        locked_peer->m_Websocket->send(message.dump());
    });
    peer->m_PeerConnection->onLocalDescription(
        [weak_session, weak_peer](rtc::Description description) {
            std::shared_ptr<P2PSession> session = weak_session.lock();
            std::shared_ptr<P2PPeer> locked_peer = weak_peer.lock();
            if (!session || !locked_peer || !locked_peer->m_Active) {
                return;
            }
            const std::string type = description.typeString();
            session->Log(P2PLogLevel::Debug, "onLocalDescription %s", type.c_str());
            if (type == "offer") {
                const bool post_answer_offer =
                    !locked_peer->m_MakingOffer && locked_peer->m_RemoteDescriptionSet;
                if (post_answer_offer) {
                    if (!locked_peer->m_IsPolite || locked_peer->m_PoliteMediaOfferSent) {
                        session->Log(P2PLogLevel::Debug,
                                     "Suppressing follow-up local offer for peer '%s'",
                                     locked_peer->m_Username.c_str());
                        return;
                    }
                    locked_peer->m_PoliteMediaOfferSent = true;
                    session->Log(P2PLogLevel::Debug,
                                 "Sending one polite media update offer for peer '%s'",
                                 locked_peer->m_Username.c_str());
                }
                locked_peer->m_MakingOffer = false;
                locked_peer->m_LocalOfferSent = true;
            } else if (type == "answer") {
                locked_peer->m_AnsweringOffer = false;
            }
            if (locked_peer->m_Websocket) {
                Json message = {{"type", type},
                                {"sdp", {{"type", type}, {"sdp", std::string(description)}}},
                                {"to", locked_peer->m_PeerId}};
                locked_peer->m_Websocket->send(message.dump());
            } else {
                session->Error("Error: WebSocket missing onLocalDescription");
            }
        });

    auto install_sendrecv_handler = [sample_rate =
                                         m_SampleRate](const std::shared_ptr<P2PPeer> &target,
                                                       const std::shared_ptr<rtc::Track> &track) {
        target->m_RtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            target->m_AudioSsrc, "audio", 109, sample_rate);
        auto handler = std::make_shared<rtc::OpusRtpPacketizer>(target->m_RtpConfig);
        handler->addToChain(std::make_shared<rtc::OpusRtpDepacketizer>());
        handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        handler->addToChain(std::make_shared<rtc::RtcpSrReporter>(target->m_RtpConfig));
        track->setMediaHandler(handler);
        target->m_AudioTrack = track;
    };

    if (!peer->m_IsPolite) {
        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(109);
        audio.addSSRC(peer->m_AudioSsrc, "audio");
        peer->m_AudioTrack = peer->m_PeerConnection->addTrack(audio);
        install_sendrecv_handler(peer, peer->m_AudioTrack);
#ifdef P2P_VIDEO
        if (m_VideoReceivers > 0) {
            rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly);
            video.addH264Codec(102);
            peer->m_VideoTrack = peer->m_PeerConnection->addTrack(video);
            m_VideoNegotiated = true;
            if (InitializeVideoDecoder(peer)) {
                auto handler = std::make_shared<rtc::H264RtpDepacketizer>();
                handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
                peer->m_VideoTrack->setMediaHandler(handler);
                peer->m_VideoTrack->onFrame(
                    [weak_session, weak_peer](rtc::binary data, rtc::FrameInfo) {
                        std::shared_ptr<P2PSession> session = weak_session.lock();
                        std::shared_ptr<P2PPeer> locked_peer = weak_peer.lock();
                        if (session && locked_peer && locked_peer->m_Active) {
                            session->DecodeVideoFrame(locked_peer, data);
                        }
                    });
                peer->m_VideoTrack->onOpen([weak_session, weak_peer]() {
                    std::shared_ptr<P2PSession> session = weak_session.lock();
                    std::shared_ptr<P2PPeer> locked_peer = weak_peer.lock();
                    if (session && locked_peer && locked_peer->m_Active) {
                        session->Log(P2PLogLevel::Normal,
                                     "Remote H264 video track open for peer %s",
                                     locked_peer->m_PeerId.c_str());
                    }
                });
            }
        }
#endif
        peer->m_AudioTrack->onFrame([weak_session, weak_peer](rtc::binary data, rtc::FrameInfo) {
            std::shared_ptr<P2PSession> session = weak_session.lock();
            std::shared_ptr<P2PPeer> locked_peer = weak_peer.lock();
            if (session && locked_peer && locked_peer->m_Active) {
                session->DecodeAudio(locked_peer, data);
            }
        });
    }

    peer->m_PeerConnection->onTrack([weak_session, weak_peer,
                                     install_sendrecv_handler](std::shared_ptr<rtc::Track> track) {
        std::shared_ptr<P2PSession> session = weak_session.lock();
        std::shared_ptr<P2PPeer> locked_peer = weak_peer.lock();
        if (!session || !locked_peer || !locked_peer->m_Active) {
            track->close();
            return;
        }
        auto description = track->description();
        if (description.type() == "video") {
#ifdef P2P_VIDEO
            if (session->m_VideoReceivers <= 0) {
                session->Log(P2PLogLevel::Normal,
                             "Video track rejected; create [p2p.r.video] before connecting");
                track->close();
                return;
            }
            if (!session->InitializeVideoDecoder(locked_peer)) {
                session->Error("Could not initialize the H264 video decoder");
                track->close();
                return;
            }
            session->m_VideoNegotiated = true;
            locked_peer->m_VideoTrack = track;
            auto handler = std::make_shared<rtc::H264RtpDepacketizer>();
            handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
            track->setMediaHandler(handler);
            track->onFrame([weak_session, weak_peer](rtc::binary data, rtc::FrameInfo) {
                std::shared_ptr<P2PSession> frame_session = weak_session.lock();
                std::shared_ptr<P2PPeer> frame_peer = weak_peer.lock();
                if (frame_session && frame_peer && frame_peer->m_Active) {
                    frame_session->DecodeVideoFrame(frame_peer, data);
                }
            });
            track->onOpen([weak_session, weak_peer]() {
                std::shared_ptr<P2PSession> open_session = weak_session.lock();
                std::shared_ptr<P2PPeer> open_peer = weak_peer.lock();
                if (open_session && open_peer && open_peer->m_Active) {
                    open_session->Log(P2PLogLevel::Normal, "Remote H264 video active for peer %s",
                                      open_peer->m_PeerId.c_str());
                }
            });
#else
            session->Log(P2PLogLevel::Normal, "Video track rejected; video is not implemented yet");
            track->close();
#endif
            return;
        }
        if (description.type() != "audio") {
            return;
        }
        track->onOpen([weak_session, weak_peer]() {
            std::shared_ptr<P2PSession> open_session = weak_session.lock();
            std::shared_ptr<P2PPeer> open_peer = weak_peer.lock();
            if (open_session && open_peer && open_peer->m_Active) {
                open_session->Log(P2PLogLevel::Normal, "Remote audio track active for peer %s",
                                  open_peer->m_PeerId.c_str());
            }
        });
        if (locked_peer->m_IsPolite) {
            description.addSSRC(locked_peer->m_AudioSsrc, "audio");
            track->setDescription(description);
            install_sendrecv_handler(locked_peer, track);
        } else {
            auto handler = std::make_shared<rtc::OpusRtpDepacketizer>();
            handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
            track->setMediaHandler(handler);
        }
        track->onFrame([weak_session, weak_peer](rtc::binary data, rtc::FrameInfo) {
            std::shared_ptr<P2PSession> frame_session = weak_session.lock();
            std::shared_ptr<P2PPeer> frame_peer = weak_peer.lock();
            if (frame_session && frame_peer && frame_peer->m_Active) {
                frame_session->DecodeAudio(frame_peer, data);
            }
        });
    });

    auto install_data_channel = [weak_session,
                                 weak_peer](const std::shared_ptr<rtc::DataChannel> &channel) {
        std::shared_ptr<P2PSession> session = weak_session.lock();
        std::shared_ptr<P2PPeer> locked_peer = weak_peer.lock();
        if (!session || !locked_peer || !locked_peer->m_Active) {
            channel->close();
            return;
        }
        locked_peer->m_DataChannel = channel;
        channel->onOpen([weak_session, weak_peer]() {
            std::shared_ptr<P2PSession> open_session = weak_session.lock();
            std::shared_ptr<P2PPeer> open_peer = weak_peer.lock();
            if (open_session && open_peer && open_peer->m_Active) {
                open_session->Log(P2PLogLevel::Debug, "DataChannel open with peer '%s'",
                                  open_peer->m_Username.c_str());
            }
        });
        channel->onMessage([weak_session, weak_peer](std::variant<rtc::binary, std::string> data) {
            std::shared_ptr<P2PSession> message_session = weak_session.lock();
            std::shared_ptr<P2PPeer> message_peer = weak_peer.lock();
            if (!message_session || !message_peer || !message_peer->m_Active) {
                return;
            }
            std::string payload;
            if (std::holds_alternative<std::string>(data)) {
                payload = std::get<std::string>(std::move(data));
            } else {
                const rtc::binary &binary = std::get<rtc::binary>(data);
                payload.assign(reinterpret_cast<const char *>(binary.data()), binary.size());
            }
            message_session->Emit(
                {P2PEventType::Message, message_peer->m_Username, std::move(payload)});
        });
    };
    if (!peer->m_IsPolite) {
        install_data_channel(peer->m_PeerConnection->createDataChannel("data"));
    } else {
        peer->m_PeerConnection->onDataChannel(install_data_channel);
    }
    return true;
}

#ifdef P2P_VIDEO
// ╭─────────────────────────────────────╮
// │            Video Decoding           │
// ╰─────────────────────────────────────╯
bool P2PSession::InitializeVideoDecoder(const std::shared_ptr<P2PPeer> &peer) {
    std::lock_guard<std::mutex> lock(peer->m_VideoMutex);
    if (peer->m_VideoDecoder) {
        return true;
    }
    peer->m_VideoCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    peer->m_VideoDecoder =
        peer->m_VideoCodec ? avcodec_alloc_context3(peer->m_VideoCodec) : nullptr;
    peer->m_VideoFrame = av_frame_alloc();
    peer->m_RgbaFrame = av_frame_alloc();
    return peer->m_VideoDecoder && peer->m_VideoFrame && peer->m_RgbaFrame &&
           avcodec_open2(peer->m_VideoDecoder, peer->m_VideoCodec, nullptr) >= 0;
}

// ─────────────────────────────────────
void P2PSession::DecodeVideoFrame(const std::shared_ptr<P2PPeer> &peer, const rtc::binary &data) {
    std::lock_guard<std::mutex> lock(peer->m_VideoMutex);
    if (!peer->m_VideoDecoder) {
        return;
    }
    if (data.empty()) {
        if (peer->m_VideoDecodeErrors++ < 3) {
            Error("Ignoring empty H264 access unit");
        }
        return;
    }
    if (!peer->m_VideoEncodedLogged) {
        peer->m_VideoEncodedLogged = true;
        Log(P2PLogLevel::Normal, "Receiving encoded H264 frames from peer %s",
            peer->m_PeerId.c_str());
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
    const int result = avcodec_send_packet(peer->m_VideoDecoder, packet);
    av_packet_free(&packet);
    if (result < 0) {
        if (peer->m_VideoDecodeErrors++ < 3) {
            char message[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(result, message, sizeof(message));
            Error("H264 access unit (%zu bytes) rejected: %s", data.size(), message);
        }
        if (result == AVERROR_EOF) {
            avcodec_flush_buffers(peer->m_VideoDecoder);
        }
        return;
    }
    while (avcodec_receive_frame(peer->m_VideoDecoder, peer->m_VideoFrame) == 0) {
        const int width = peer->m_VideoFrame->width;
        const int height = peer->m_VideoFrame->height;
        peer->m_RgbaPixels.resize(static_cast<size_t>(width) * height * 4);
        peer->m_VideoScaler =
            sws_getCachedContext(peer->m_VideoScaler, width, height,
                                 static_cast<AVPixelFormat>(peer->m_VideoFrame->format), width,
                                 height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!peer->m_VideoScaler) {
            return;
        }
        uint8_t *destination[] = {peer->m_RgbaPixels.data()};
        int strides[] = {width * 4};
        sws_scale(peer->m_VideoScaler, peer->m_VideoFrame->data, peer->m_VideoFrame->linesize, 0,
                  height, destination, strides);
        peer->m_RgbaFrame->width = width;
        peer->m_RgbaFrame->height = height;
        ++peer->m_VideoSerial;
        if (!peer->m_VideoDecodedLogged) {
            peer->m_VideoDecodedLogged = true;
            Log(P2PLogLevel::Normal, "Decoded video frame: %dx%d", width, height);
        }
    }
}
#endif
