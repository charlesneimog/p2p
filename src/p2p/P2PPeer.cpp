#include "P2PPeer.hpp"

#include <chrono>
P2PPeer::P2PPeer(std::string id, std::string name)
    : peer_id(std::move(id)), username(std::move(name)) {}

P2PPeer::~P2PPeer() {
    shutdown();
}

bool P2PPeer::initializeEncoder(int sample_rate) {
    int error_code = OPUS_OK;
    opus_enc_mono_ = opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_AUDIO, &error_code);
    if (error_code != OPUS_OK || !opus_enc_mono_) {
        opus_enc_mono_ = nullptr;
        return false;
    }

    error_code = OPUS_OK;
    opus_enc_stereo_ = opus_encoder_create(sample_rate, 2, OPUS_APPLICATION_AUDIO, &error_code);
    if (error_code != OPUS_OK || !opus_enc_stereo_) {
        opus_enc_stereo_ = nullptr;
        opus_encoder_destroy(opus_enc_mono_);
        opus_enc_mono_ = nullptr;
        return false;
    }

    for (auto *encoder : {opus_enc_mono_, opus_enc_stereo_}) {
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

int P2PPeer::encodeMono(const float *pcm, int samples, unsigned char *output, int capacity) {
    if (!opus_enc_mono_) {
        return OPUS_INVALID_STATE;
    }
    return opus_encode_float(opus_enc_mono_, pcm, samples, output, capacity);
}

int P2PPeer::encodeStereo(const float *pcm, int samples, unsigned char *output, int capacity) {
    if (!opus_enc_stereo_) {
        return OPUS_INVALID_STATE;
    }
    return opus_encode_float(opus_enc_stereo_, pcm, samples, output, capacity);
}

void P2PPeer::startTransmission(int frame_size, int sample_rate) {
    thread_running_ = true;
    std::weak_ptr<P2PPeer> weak_peer = shared_from_this();
    tx_thread_ = std::thread([weak_peer, frame_size, sample_rate]() {
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

        while (auto peer = weak_peer.lock()) {
            if (!peer->thread_running_) {
                break;
            }
            QueuedAudioSample sample{};
            while (collected < frame_size && peer->send_buffer.pop(sample)) {
                if (channels != 0 && sample.channels != channels) {
                    collected = 0;
                }
                channels = sample.channels;
                if (channels == 2) {
                    pcm_frame[static_cast<size_t>(collected) * 2] = sample.left;
                    pcm_frame[static_cast<size_t>(collected) * 2 + 1] = sample.right;
                } else {
                    pcm_frame[collected] = sample.left;
                }
                ++collected;
            }
            if (collected < frame_size) {
                peer.reset();
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            collected = 0;
            const bool streaming = peer->is_streaming.load(std::memory_order_relaxed);
            if ((!streaming && stream_gain <= 0.0F) || !peer->audio_track ||
                !peer->audio_track->isOpen() || !peer->rtp_config) {
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

            const int bytes = channels == 2
                                  ? peer->encodeStereo(pcm_frame.data(), frame_size, opus_payload,
                                                       maximum_opus_bytes)
                                  : peer->encodeMono(pcm_frame.data(), frame_size, opus_payload,
                                                     maximum_opus_bytes);
            if (bytes <= 0) {
                continue;
            }
            peer->audio_track->sendFrame(reinterpret_cast<const std::byte *>(opus_payload),
                                         static_cast<size_t>(bytes),
                                         rtc::FrameInfo(peer->rtp_config->timestamp));
            peer->rtp_config->timestamp += static_cast<uint32_t>(frame_size);
        }
    });
}

bool P2PPeer::popReceived(float &sample) {
    return receive_buffer.pop(sample);
}

void P2PPeer::shutdown() {
    {
        std::lock_guard<std::mutex> lock(shutdown_mutex_);
        if (shut_down_) {
            return;
        }
        shut_down_ = true;
        active = false;
        connected = false;
        is_streaming = false;
        thread_running_ = false;
    }
    if (tx_thread_.joinable() && tx_thread_.get_id() != std::this_thread::get_id()) {
        tx_thread_.join();
    }
    if (opus_enc_mono_) {
        opus_encoder_destroy(opus_enc_mono_);
        opus_enc_mono_ = nullptr;
    }
    if (opus_enc_stereo_) {
        opus_encoder_destroy(opus_enc_stereo_);
        opus_enc_stereo_ = nullptr;
    }
    if (dc) {
        dc->close();
        dc.reset();
    }
    if (audio_track) {
        audio_track->close();
        audio_track.reset();
    }
#ifdef P2P_VIDEO
    if (video_track) {
        video_track->close();
        video_track.reset();
    }
#endif
    if (pc) {
        pc->close();
        pc.reset();
    }
    {
        std::lock_guard<std::mutex> lock(opus_dec_mono_mutex);
        if (opus_dec_mono) {
            opus_decoder_destroy(opus_dec_mono);
            opus_dec_mono = nullptr;
        }
    }
#ifdef P2P_VIDEO
    {
        std::lock_guard<std::mutex> lock(video_mutex);
        sws_freeContext(video_scaler);
        video_scaler = nullptr;
        av_frame_free(&rgba_frame);
        av_frame_free(&video_frame);
        avcodec_free_context(&video_decoder);
        rgba_pixels.clear();
    }
#endif
    pending_remote_candidates.clear();
    pending_negotiations.clear();
    rtp_config.reset();
    ws.reset();
}
