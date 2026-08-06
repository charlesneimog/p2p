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

#ifdef P2P_JITTER_VIDEO
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

struct QueuedCandidate {
    std::string candidate;
    std::string mid;
};

class P2PPeer : public std::enable_shared_from_this<P2PPeer> {
public:
    P2PPeer(std::string peer_id, std::string username);
    ~P2PPeer();

    bool initializeEncoder(int sample_rate);
    void startTransmission(int frame_size);
    void shutdown();
    bool popReceived(float &sample);

    const std::string peer_id;
    const std::string username;
    std::atomic<bool> active{true};
    std::atomic<bool> connected{false};
    std::atomic<bool> is_streaming{false};

    // The perfect-negotiation state is intentionally kept verbatim.
    std::vector<QueuedCandidate> pending_remote_candidates;
    bool remote_description_set{false};
    bool is_polite{false};
    bool making_offer{false};
    bool ignore_offer{false};
    bool answering_offer{false};
    bool local_offer_sent{false};
    bool polite_media_offer_sent{false};
    bool stun_warning_reported{false};

    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> audio_track;
    boost::lockfree::spsc_queue<float, boost::lockfree::capacity<16384>> send_buffer;
    boost::lockfree::spsc_queue<float, boost::lockfree::capacity<16384>> receive_buffer;
    uint32_t audio_ssrc{0};
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp_config;
    OpusDecoder *opus_dec_mono{nullptr};
    std::mutex opus_dec_mono_mutex;
    std::vector<std::function<void()>> pending_negotiations;

#ifdef P2P_JITTER_VIDEO
    std::shared_ptr<rtc::Track> video_track;
    const AVCodec *video_codec{nullptr};
    AVCodecContext *video_decoder{nullptr};
    AVFrame *video_frame{nullptr};
    AVFrame *rgba_frame{nullptr};
    SwsContext *video_scaler{nullptr};
    std::vector<unsigned char> rgba_pixels;
    std::mutex video_mutex;
    uint64_t video_serial{0};
    bool video_encoded_logged{false};
    bool video_decoded_logged{false};
    int video_decode_errors{0};
#endif

private:
    int encodeMono(const float *pcm, int samples, unsigned char *output, int capacity);

    OpusEncoder *opus_enc_mono_{nullptr};
    std::thread tx_thread_;
    std::atomic<bool> thread_running_{false};
    std::mutex shutdown_mutex_;
    bool shut_down_{false};
};
