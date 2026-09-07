#include "P2PPeer.hpp"

#include <chrono>
P2PPeer::P2PPeer(std::string id, std::string name)
    : m_PeerId(std::move(id)), m_Username(std::move(name)) {}

P2PPeer::~P2PPeer() {
    Shutdown();
}

// ─────────────────────────────────────
bool P2PPeer::InitializeEncoder(int sample_rate) {
    int error_code = OPUS_OK;
    m_OpusEncMono = opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_AUDIO, &error_code);
    if (error_code != OPUS_OK || !m_OpusEncMono) {
        m_OpusEncMono = nullptr;
        return false;
    }

    error_code = OPUS_OK;
    m_OpusEncStereo = opus_encoder_create(sample_rate, 2, OPUS_APPLICATION_AUDIO, &error_code);
    if (error_code != OPUS_OK || !m_OpusEncStereo) {
        m_OpusEncStereo = nullptr;
        opus_encoder_destroy(m_OpusEncMono);
        m_OpusEncMono = nullptr;
        return false;
    }

    for (OpusEncoder *encoder : {m_OpusEncMono, m_OpusEncStereo}) {
        opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE_MAX));
        opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
        opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(1));
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));
        opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0));
        opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
    }
    return true;
}

// ─────────────────────────────────────
int P2PPeer::EncodeMono(const float *pcm, int samples, unsigned char *output, int capacity) {
    if (!m_OpusEncMono) {
        return OPUS_INVALID_STATE;
    }
    return opus_encode_float(m_OpusEncMono, pcm, samples, output, capacity);
}

// ─────────────────────────────────────
int P2PPeer::EncodeStereo(const float *pcm, int samples, unsigned char *output, int capacity) {
    if (!m_OpusEncStereo) {
        return OPUS_INVALID_STATE;
    }
    return opus_encode_float(m_OpusEncStereo, pcm, samples, output, capacity);
}

// ─────────────────────────────────────
void P2PPeer::StartTransmission(int frame_size, int sample_rate) {
    m_ThreadRunning = true;
    std::weak_ptr<P2PPeer> weak_peer = shared_from_this();
    m_TxThread = std::thread([weak_peer, frame_size, sample_rate]() {
        constexpr int maximum_opus_bytes = 4000;
        constexpr int stream_fade_milliseconds = 50;
        const int ramp_samples =
            sample_rate > 0 ? sample_rate * stream_fade_milliseconds / 1000 : 1;
        const float ramp_step = 1.0F / static_cast<float>(ramp_samples > 0 ? ramp_samples : 1);
        unsigned char opus_payload[maximum_opus_bytes];
        std::vector<float> pcm_frame(static_cast<size_t>(frame_size) * 2);
        int collected = 0;
        int channels = 0;
        float stream_gain = 0.0F;

        while (std::shared_ptr<P2PPeer> peer = weak_peer.lock()) {
            if (!peer->m_ThreadRunning) {
                break;
            }
            QueuedAudioSample sample{};
            while (collected < frame_size && peer->m_SendBuffer.pop(sample)) {
                if (channels != 0 && sample.m_Channels != channels) {
                    collected = 0;
                }
                channels = sample.m_Channels;
                if (channels == 2) {
                    pcm_frame[static_cast<size_t>(collected) * 2] = sample.m_Left;
                    pcm_frame[static_cast<size_t>(collected) * 2 + 1] = sample.m_Right;
                } else {
                    pcm_frame[collected] = sample.m_Left;
                }
                ++collected;
            }
            if (collected < frame_size) {
                peer.reset();
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            collected = 0;
            const bool streaming = peer->m_IsStreaming.load(std::memory_order_relaxed);
            if ((!streaming && stream_gain <= 0.0F) || !peer->m_AudioTrack ||
                !peer->m_AudioTrack->isOpen() || !peer->m_RtpConfig) {
                continue;
            }

            for (int index = 0; index < frame_size; ++index) {
                if (channels == 2) {
                    pcm_frame[static_cast<size_t>(index) * 2] *= stream_gain;
                    pcm_frame[static_cast<size_t>(index) * 2 + 1] *= stream_gain;
                } else {
                    pcm_frame[index] *= stream_gain;
                }

                if (streaming) {
                    stream_gain += ramp_step;
                    if (stream_gain > 1.0F) {
                        stream_gain = 1.0F;
                    }
                } else {
                    stream_gain -= ramp_step;
                    if (stream_gain < 0.0F) {
                        stream_gain = 0.0F;
                    }
                }
            }

            const int bytes = channels == 2 ? peer->EncodeStereo(pcm_frame.data(), frame_size,
                                                                 opus_payload, maximum_opus_bytes)
                                            : peer->EncodeMono(pcm_frame.data(), frame_size,
                                                               opus_payload, maximum_opus_bytes);
            if (bytes <= 0) {
                continue;
            }
            peer->m_AudioTrack->sendFrame(reinterpret_cast<const std::byte *>(opus_payload),
                                          static_cast<size_t>(bytes),
                                          rtc::FrameInfo(peer->m_RtpConfig->timestamp));
            peer->m_RtpConfig->timestamp += static_cast<uint32_t>(frame_size);
        }
    });
}

// ─────────────────────────────────────
bool P2PPeer::PopReceived(float &sample) {
    return m_ReceiveBuffer.pop(sample);
}

// ─────────────────────────────────────
void P2PPeer::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_ShutdownMutex);
        if (m_ShutDown) {
            return;
        }
        m_ShutDown = true;
        m_Active = false;
        m_Connected = false;
        m_IsStreaming = false;
        m_ThreadRunning = false;
    }
    if (m_TxThread.joinable() && m_TxThread.get_id() != std::this_thread::get_id()) {
        m_TxThread.join();
    }
    if (m_OpusEncMono) {
        opus_encoder_destroy(m_OpusEncMono);
        m_OpusEncMono = nullptr;
    }
    if (m_OpusEncStereo) {
        opus_encoder_destroy(m_OpusEncStereo);
        m_OpusEncStereo = nullptr;
    }
    if (m_DataChannel) {
        m_DataChannel->close();
        m_DataChannel.reset();
    }
    if (m_AudioTrack) {
        m_AudioTrack->close();
        m_AudioTrack.reset();
    }
#ifdef P2P_VIDEO
    if (m_VideoTrack) {
        m_VideoTrack->close();
        m_VideoTrack.reset();
    }
#endif
    if (m_PeerConnection) {
        m_PeerConnection->close();
        m_PeerConnection.reset();
    }
    {
        std::lock_guard<std::mutex> lock(m_OpusDecMonoMutex);
        if (m_OpusDecMono) {
            opus_decoder_destroy(m_OpusDecMono);
            m_OpusDecMono = nullptr;
        }
    }
#ifdef P2P_VIDEO
    {
        std::lock_guard<std::mutex> lock(m_VideoMutex);
        sws_freeContext(m_VideoScaler);
        m_VideoScaler = nullptr;
        av_frame_free(&m_RgbaFrame);
        av_frame_free(&m_VideoFrame);
        avcodec_free_context(&m_VideoDecoder);
        m_RgbaPixels.clear();
    }
#endif
    m_PendingRemoteCandidates.clear();
    m_PendingNegotiations.clear();
    m_RtpConfig.reset();
    m_Websocket.reset();
}
