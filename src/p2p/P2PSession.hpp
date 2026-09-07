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

enum class P2PLogLevel {
    Normal,
    Debug,
    Error,
};

struct P2PEvent {
    P2PEventType m_Type;
    std::string m_Peer;
    std::string m_Text;
    int m_Count{0};
    P2PLogLevel m_LogLevel{P2PLogLevel::Normal};
};

struct P2PPeerResolution {
    std::shared_ptr<P2PPeer> m_Peer;
    bool m_Ambiguous{false};
};

class P2PSession : public std::enable_shared_from_this<P2PSession> {
public:
    using Listener = std::function<void(const P2PEvent &)>;
    using MainThreadDispatcher = std::function<void(std::function<void()>)>;

    static std::shared_ptr<P2PSession> Create(const std::string &id, int sample_rate,
                                              MainThreadDispatcher dispatcher);
    ~P2PSession();

    const std::string &Id() const;
    bool Available() const;
    bool Connected() const;
    void Deactivate();

    uint64_t AddListener(Listener listener);
    void RemoveListener(uint64_t listener_id);
    void Connect(const std::string &websocket_url, const std::string &room,
                 const std::string &local_username);
    void Disconnect();
    void SetStreaming(bool enabled);
    bool Streaming() const;
    void SendMessage(const std::string &text);
    void SendJson(const std::string &json_text);
    int ConnectionCount() const;
    void Report();

    bool ClaimController(const void *owner);
    void ReleaseController(const void *owner);
    bool ClaimAudioSender(const void *owner);
    void ReleaseAudioSender(const void *owner);
    bool ClaimAudioReceiver(const std::string &username, const void *owner);
    void ReleaseAudioReceiver(const std::string &username, const void *owner);
    void RegisterVideoReceiver();
    void UnregisterVideoReceiver();
    bool VideoNegotiated() const;

    std::vector<std::shared_ptr<P2PPeer>> PeerSnapshot() const;
    void PushOutgoingAudio(const float *samples, int count, int channels = 1);
    P2PPeerResolution ResolvePeer(const std::string &username) const;
    int FrameSize() const;
    int SampleRate() const;

private:
    P2PSession(std::string id, int sample_rate, MainThreadDispatcher dispatcher);
    void Initialize();
    void Emit(P2PEvent event);
    void Log(P2PLogLevel level, const char *format, ...);
    void Error(const char *format, ...);

    void InstallWebSocketCallbacks();
    void OnSignallingMessage(const std::string &payload);
    void Welcome(const nlohmann::json &data);
    void PeerJoined(const nlohmann::json &data);
    void ExistingPeers(const nlohmann::json &data);
    void Offer(const nlohmann::json &data);
    void Answer(const nlohmann::json &data);
    void IceCandidate(const nlohmann::json &data);
    void PeerLeft(const nlohmann::json &data);

    std::shared_ptr<P2PPeer> AddPeer(const std::string &peer_id, const std::string &username);
    std::shared_ptr<P2PPeer> FindPeerById(const std::string &peer_id) const;
    void RemovePeer(const std::string &peer_id, bool notify);
    void RemoveAllPeers();
    bool SetupWebRtc(const std::shared_ptr<P2PPeer> &peer);
    void ResetPeerConnection(const std::shared_ptr<P2PPeer> &peer);
    void FlushPendingCandidates(const std::shared_ptr<P2PPeer> &peer);
    void ConfigureVideoMedia(rtc::Description &description);
    void DecodeAudio(const std::shared_ptr<P2PPeer> &peer, const rtc::binary &data);
    void UpdateConnectionState(const std::shared_ptr<P2PPeer> &peer,
                               rtc::PeerConnection::State state);
    void EmitConnectionCount();
    void WarnIfNotStunPair(const std::shared_ptr<P2PPeer> &peer);
    bool CreatePeerDecoder(const std::shared_ptr<P2PPeer> &peer);
    void RebuildRealtimePeersLocked();

#ifdef P2P_VIDEO
    bool InitializeVideoDecoder(const std::shared_ptr<P2PPeer> &peer);
    void DecodeVideoFrame(const std::shared_ptr<P2PPeer> &peer, const rtc::binary &data);
#endif

    const std::string m_Id;
    const int m_SampleRate;
    const MainThreadDispatcher m_MainThreadDispatcher;
    const int m_FrameSize{120}; // Minimum Opus frame: 2.5 ms at 48 kHz.
    std::atomic<bool> m_Available{true};
    std::atomic<bool> m_WebsocketConnected{false};
    std::atomic<bool> m_WantsStream{false};
    std::atomic<int> m_VideoReceivers{0};
    std::atomic<bool> m_VideoNegotiated{false};

    mutable std::mutex m_WebsocketMutex;
    std::shared_ptr<rtc::WebSocket> m_Websocket;
    std::string m_WebsocketCaBundle;
    std::string m_LocalPeerId;
    std::string m_Room;
    std::string m_LocalUsername;

    mutable std::mutex m_PeersMutex;
    std::unordered_map<std::string, std::shared_ptr<P2PPeer>> m_PeersById;
    std::unordered_map<std::string, std::vector<std::weak_ptr<P2PPeer>>> m_PeersByName;
    std::atomic<const std::vector<std::shared_ptr<P2PPeer>> *> m_RealtimePeers{nullptr};
    std::vector<std::shared_ptr<const std::vector<std::shared_ptr<P2PPeer>>>>
        m_RetainedPeerSnapshots;
    std::vector<std::shared_ptr<P2PPeer>> m_RetiredPeers;

    mutable std::mutex m_ListenersMutex;
    std::unordered_map<uint64_t, Listener> m_Listeners;
    std::atomic<uint64_t> m_NextListenerId{1};

    mutable std::mutex m_ClaimsMutex;
    const void *m_ControllerOwner{nullptr};
    const void *m_AudioSenderOwner{nullptr};
    std::unordered_map<std::string, const void *> m_AudioReceiverOwners;
};
