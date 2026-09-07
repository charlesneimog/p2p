#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/lockfree/spsc_queue.hpp>
#include <opus.h>
#include <rtc/rtc.hpp>

#ifdef P2P_VIDEO
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

struct QueuedCandidate {
    std::string m_Candidate;
    std::string m_Mid;
};

struct QueuedAudioSample {
    float m_Left;
    float m_Right;
    int m_Channels;
};

class P2PPeer : public std::enable_shared_from_this<P2PPeer> {
public:
    P2PPeer(std::string peer_id, std::string username);
    ~P2PPeer();

    bool InitializeEncoder(int sample_rate);
    void StartTransmission(int frame_size, int sample_rate);
    void Shutdown();
    bool PopReceived(float &sample);

    const std::string m_PeerId;
    const std::string m_Username;
    std::atomic<bool> m_Active{true};
    std::atomic<bool> m_Connected{false};
    std::atomic<bool> m_IsStreaming{false};

    // Perfect-negotiation state belongs to this peer connection.
    std::vector<QueuedCandidate> m_PendingRemoteCandidates;
    bool m_RemoteDescriptionSet{false};
    bool m_IsPolite{false};
    bool m_MakingOffer{false};
    bool m_IgnoreOffer{false};
    bool m_AnsweringOffer{false};
    bool m_LocalOfferSent{false};
    bool m_PoliteMediaOfferSent{false};
    bool m_StunWarningReported{false};

    std::shared_ptr<rtc::WebSocket> m_Websocket;
    std::shared_ptr<rtc::PeerConnection> m_PeerConnection;
    std::shared_ptr<rtc::DataChannel> m_DataChannel;
    std::shared_ptr<rtc::Track> m_AudioTrack;
    boost::lockfree::spsc_queue<QueuedAudioSample, boost::lockfree::capacity<16384>> m_SendBuffer;
    boost::lockfree::spsc_queue<float, boost::lockfree::capacity<16384>> m_ReceiveBuffer;
    uint32_t m_AudioSsrc{0};
    std::shared_ptr<rtc::RtpPacketizationConfig> m_RtpConfig;
    OpusDecoder *m_OpusDecMono{nullptr};
    std::mutex m_OpusDecMonoMutex;
    std::vector<std::function<void()>> m_PendingNegotiations;

#ifdef P2P_VIDEO
    std::shared_ptr<rtc::Track> m_VideoTrack;
    const AVCodec *m_VideoCodec{nullptr};
    AVCodecContext *m_VideoDecoder{nullptr};
    AVFrame *m_VideoFrame{nullptr};
    AVFrame *m_RgbaFrame{nullptr};
    SwsContext *m_VideoScaler{nullptr};
    std::vector<unsigned char> m_RgbaPixels;
    std::mutex m_VideoMutex;
    uint64_t m_VideoSerial{0};
    bool m_VideoEncodedLogged{false};
    bool m_VideoDecodedLogged{false};
    int m_VideoDecodeErrors{0};
#endif

private:
    int EncodeMono(const float *pcm, int samples, unsigned char *output, int capacity);
    int EncodeStereo(const float *pcm, int samples, unsigned char *output, int capacity);

    OpusEncoder *m_OpusEncMono{nullptr};
    OpusEncoder *m_OpusEncStereo{nullptr};
    std::thread m_TxThread;
    std::atomic<bool> m_ThreadRunning{false};
    std::mutex m_ShutdownMutex;
    bool m_ShutDown{false};
};
