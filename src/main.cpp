#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>
#include "capture/factory.hpp"
#include "input/factory.hpp"
#include "crypto/gen_certs.hpp"
#include "web/embedded_assets.hpp"
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <codec/tile/tile_encoder.hpp>
#include <codec/tile/webp_encoder.cpp>
#include <codec/tile/jpeg_encoder.cpp>
#include <codec/video/x264_encoder.cpp>
#include <capture/rect_utils.hpp>
#include <atomic>
#include "audio/factory.hpp"
#include "audio/opus_encoder.hpp"

// First byte of every binary WebSocket message; must match web/app.js
enum class MessageType : uint8_t {
    ScreenInfo = 1, // u16 width, u16 height (LE); always precedes a full frame
    Tile = 2,       // u8 codec, u16 x, y, width, height (LE), then encoded image
    Cursor = 3,     // u16 width, height, xhot, yhot (LE), then width*height straight RGBA
    Audio = 4,      // u8 channels, u32 sample_rate, then float samples (interleaved)
    H264Video = 5,  // u8 is_keyframe, u64 timestamp_us (LE), then Annex B access unit
    Ping = 6,       // u32 seq (LE); the client echoes it straight back as a Pong
    StreamMode = 7, // u8 StreamTier; applies to all frame data that follows it
};

// First byte of client -> server messages
enum class ClientMessage : uint8_t {
    MouseMove = 0x10,
    MouseButton = 0x11,
    Scroll = 0x12,
    Key = 0x13,
    Pong = 0x20,            // u32 seq (LE), copied from the Ping
    KeyframeRequest = 0x21, // decoder lost sync and is waiting for an IDR
    H264Unsupported = 0x22, // browser has no usable H.264 decoder; pin to tiles
};

// The two discrete connection tiers. Every viewer starts on Tiles.
enum class StreamTier : uint8_t {
    Tiles = 0, // damage-driven JPEG tiles; tolerant of stalls, no decoder state
    H264 = 1,  // x264 video; much cheaper on bandwidth but needs a steady link
};

// Tier switching policy. The upgrade and downgrade thresholds are deliberately
// far apart, and an upgrade needs a sustained clean window, so the tier does not flap.
namespace tier_policy {
    constexpr auto kPingInterval = std::chrono::milliseconds(100);

    // Upgrade to H.264 only when ALL of these hold for the whole hold window
    constexpr size_t kUpgradeMaxBuffered = 8 * 1024;
    constexpr double kUpgradeMaxJitterMs = 15.0; // under one 60fps frame interval
    constexpr int64_t kUpgradeMaxUnackedUs = 150'000;
    constexpr int kUpgradeMinRttSamples = 20;
    constexpr int64_t kBaseUpgradeHoldUs = 5'000'000;
    // Each downgrade that follows an upgrade quickly doubles the next hold
    constexpr int64_t kMaxUpgradeHoldUs = 40'000'000;
    // Staying on H.264 this long counts as stable and resets the hold
    constexpr int64_t kHoldResetUs = 60'000'000;

    // Downgrade to tiles when ANY of these trips
    constexpr size_t kDowngradeBuffered = 64 * 1024;
    constexpr double kDowngradeJitterMs = 35.0;
    constexpr int64_t kDowngradeUnackedUs = 300'000;
}

// Tile data is skipped for a viewer above this, then resynced with a full frame
constexpr size_t kTileBufferGate = 128 * 1024;
// x264 frames encoded from an unchanged screen after damage stops, so rate
// control can refine the last (often blurry) frame of a burst
constexpr int kH264TailFrames = 12;

static int64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Viewer {
    explicit Viewer(std::string ip) : remote_ip(std::move(ip)) {}
    const std::string remote_ip;

    // Link telemetry and tier decision. Shared by the connection thread (pongs),
    // the control thread (pings and evaluation) and the capture thread (send-time
    // congestion), so guarded by mu.
    std::mutex mu;
    uint32_t next_ping_seq = 0;
    std::deque<std::pair<uint32_t, int64_t>> pending_pings; // seq, sent_us; oldest first
    double last_rtt_ms = -1.0;
    double avg_rtt_ms = 0.0;
    double jitter_ms = 0.0; // RFC 3550-style smoothed |delta RTT|
    int rtt_samples = 0;
    int64_t stable_since_us = 0;
    int64_t upgrade_hold_us = tier_policy::kBaseUpgradeHoldUs;
    int64_t upgraded_at_us = 0;
    bool want_h264 = false;
    bool h264_supported = true;

    std::atomic<bool> keyframe_requested{false};

    // Owned by the capture thread, which is the only place tiers actually switch,
    // so each switch is ordered correctly with the frame data around it
    StreamTier tier = StreamTier::Tiles;
    bool awaiting_keyframe = false;
    bool needs_full_tiles = true; // new viewers start with a full-screen resync

    bool wants_h264() {
        std::lock_guard lock(mu);
        return want_h264;
    }

    void downgrade(int64_t now, const char* reason) {
        std::lock_guard lock(mu);
        downgrade_locked(now, reason);
    }

    void downgrade_locked(int64_t now, const char* reason) {
        if (!want_h264) return;
        want_h264 = false;
        stable_since_us = 0;
        // Bouncing straight back out of H.264 suggests a marginal link; be slower to retry
        if (now - upgraded_at_us < tier_policy::kHoldResetUs) {
            upgrade_hold_us = std::min(upgrade_hold_us * 2, tier_policy::kMaxUpgradeHoldUs);
        } else {
            upgrade_hold_us = tier_policy::kBaseUpgradeHoldUs;
        }
        std::cout << "Viewer " << remote_ip << ": H.264 -> tiles (" << reason
                  << ", jitter " << jitter_ms << "ms, rtt " << avg_rtt_ms << "ms)\n";
    }

    void mark_h264_unsupported(int64_t now) {
        std::lock_guard lock(mu);
        h264_supported = false;
        downgrade_locked(now, "client cannot decode H.264");
    }

    uint32_t record_ping_sent(int64_t now) {
        std::lock_guard lock(mu);
        const uint32_t seq = next_ping_seq++;
        pending_pings.emplace_back(seq, now);
        if (pending_pings.size() > 256) pending_pings.pop_front(); // bound a dead link
        return seq;
    }

    void record_pong(uint32_t seq, int64_t now) {
        std::lock_guard lock(mu);
        while (!pending_pings.empty()) {
            const auto [sent_seq, sent_us] = pending_pings.front();
            if (static_cast<int32_t>(seq - sent_seq) < 0) break; // pong for an older ping
            pending_pings.pop_front();
            if (sent_seq != seq) continue;

            const double rtt_ms = (now - sent_us) / 1000.0;
            if (last_rtt_ms >= 0.0) {
                jitter_ms += (std::abs(rtt_ms - last_rtt_ms) - jitter_ms) / 16.0;
                avg_rtt_ms += (rtt_ms - avg_rtt_ms) / 8.0;
            } else {
                avg_rtt_ms = rtt_ms;
            }
            last_rtt_ms = rtt_ms;
            ++rtt_samples;
            break;
        }
    }

    // Called by the control thread every ping interval
    void evaluate_tier(size_t buffered, int64_t now) {
        using namespace tier_policy;
        std::lock_guard lock(mu);
        const int64_t unacked_us = pending_pings.empty() ? 0 : now - pending_pings.front().second;

        if (want_h264) {
            if (buffered > kDowngradeBuffered) downgrade_locked(now, "send buffer backed up");
            else if (jitter_ms > kDowngradeJitterMs) downgrade_locked(now, "RTT jitter");
            else if (unacked_us > kDowngradeUnackedUs) downgrade_locked(now, "ping unacknowledged");
            return;
        }

        const bool link_clean = h264_supported
            && rtt_samples >= kUpgradeMinRttSamples
            && buffered <= kUpgradeMaxBuffered
            && jitter_ms < kUpgradeMaxJitterMs
            && unacked_us < kUpgradeMaxUnackedUs;
        if (!link_clean) {
            stable_since_us = 0;
            return;
        }
        if (stable_since_us == 0) {
            stable_since_us = now;
        } else if (now - stable_since_us >= upgrade_hold_us) {
            want_h264 = true;
            upgraded_at_us = now;
            std::cout << "Viewer " << remote_ip << ": tiles -> H.264 (stable for "
                      << upgrade_hold_us / 1'000'000 << "s, jitter " << jitter_ms
                      << "ms, rtt " << avg_rtt_ms << "ms)\n";
        }
    }
};

// Maps the WebSocket handed to the message callback to its Viewer. The capture and
// control threads iterate server.getClients() and look viewers up here.
class ViewerRegistry {
public:
    void add(const ix::WebSocket* ws, std::shared_ptr<Viewer> viewer) {
        std::lock_guard lock(mu_);
        viewers_[ws] = std::move(viewer);
    }
    void remove(const ix::WebSocket* ws) {
        std::lock_guard lock(mu_);
        viewers_.erase(ws);
    }
    std::shared_ptr<Viewer> find(const ix::WebSocket* ws) {
        std::lock_guard lock(mu_);
        auto it = viewers_.find(ws);
        return it == viewers_.end() ? nullptr : it->second;
    }
private:
    std::mutex mu_;
    std::unordered_map<const ix::WebSocket*, std::shared_ptr<Viewer>> viewers_;
};

static void write_u16_le(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

static void write_u32_le(std::vector<uint8_t>& buf, uint32_t val) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((val >> (8 * i)) & 0xFF));
}

static void write_u64_le(std::vector<uint8_t>& buf, uint64_t val) {
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((val >> (8 * i)) & 0xFF));
}

int main() {
    ix::initNetSystem();

    ix::HttpServer server(7200, "0.0.0.0");

    auto tls_certs = ensure_tls_certs();
    ix::SocketTLSOptions tlsOptions;
    tlsOptions.tls = true;
    tlsOptions.certFile = tls_certs.cert_path.string();
    tlsOptions.keyFile = tls_certs.key_path.string();
    tlsOptions.caFile = "NONE";
    server.setTLSOptions(tlsOptions);

    server.setOnConnectionCallback([](ix::HttpRequestPtr request, std::shared_ptr<ix::ConnectionState>) -> ix::HttpResponsePtr {
        if (auto asset = find_embedded_asset(request->uri)) {
            return std::make_shared<ix::HttpResponse>(
                200, "OK", ix::HttpErrorCode::Ok,
                ix::WebSocketHttpHeaders{{"Content-Type", std::string(asset->mime_type)}},
                std::string(asset->content)
            );
        }
        return std::make_shared<ix::HttpResponse>(
            404, "Not Found", ix::HttpErrorCode::Ok,
            ix::WebSocketHttpHeaders{{"Content-Type", "text/plain; charset=utf-8"}},
            "Not Found"
        );
    });

    ViewerRegistry viewers;
    // Set by connection threads, consumed by the capture thread
    std::atomic<bool> cursor_refresh_requested{false};

    auto input_injector = create_input_injector();
    if (!input_injector || !input_injector->init(":1")) {
        std::cerr << "Failed to initialize input injector\n";
        return 1;
    }

    server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            std::cout << "Client connected: " << state->getRemoteIp() << std::endl;
            // The capture thread sends the new viewer a full tile frame on its own,
            // so other viewers don't get a redundant full-screen refresh
            viewers.add(&ws, std::make_shared<Viewer>(state->getRemoteIp()));
            cursor_refresh_requested = true;
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            std::cout << "Client disconnected: " << state->getRemoteIp() << std::endl;
            viewers.remove(&ws);
        } else if (msg->type == ix::WebSocketMessageType::Message) {
            if (!msg->binary || msg->str.empty()) return;

            const auto* view = reinterpret_cast<const uint8_t*>(msg->str.data());
            const size_t len = msg->str.size();
            const auto type = static_cast<ClientMessage>(view[0]);
            if (type == ClientMessage::Pong && len >= 5) {
                const int64_t received_us = now_us();
                const uint32_t seq = view[1] | (view[2] << 8) | (view[3] << 16) | (static_cast<uint32_t>(view[4]) << 24);
                if (auto viewer = viewers.find(&ws)) viewer->record_pong(seq, received_us);
            } else if (type == ClientMessage::KeyframeRequest) {
                if (auto viewer = viewers.find(&ws)) viewer->keyframe_requested = true;
            } else if (type == ClientMessage::H264Unsupported) {
                if (auto viewer = viewers.find(&ws)) viewer->mark_h264_unsupported(now_us());
            } else if (type == ClientMessage::MouseMove && len >= 5) {
                const uint16_t x = view[1] | (view[2] << 8);
                const uint16_t y = view[3] | (view[4] << 8);
                input_injector->move_mouse(x, y);
            } else if (type == ClientMessage::MouseButton && len >= 3) {
                const uint8_t button = view[1];
                const bool pressed = view[2] != 0;
                input_injector->mouse_button(static_cast<MouseButton>(button), pressed);
            } else if (type == ClientMessage::Scroll && len >= 3) {
                const int8_t delta_x = view[1];
                const int8_t delta_y = view[2];
                input_injector->scroll(delta_x, delta_y);
            } else if (type == ClientMessage::Key && len >= 6) {
                const uint32_t key = view[1] | (view[2] << 8) | (view[3] << 16) | (view[4] << 24);
                const bool pressed = view[5] != 0;
                input_injector->key_event(key, pressed);
            }
        }
    });

    auto res = server.listen();
    if (!res.first) {
        std::cerr << "Failed to listen on port 7200: " << res.second << std::endl;
        return 1;
    }

    server.start();

    // Pings every viewer and decides its tier. Kept off the capture thread so that
    // encode time never delays pings or skews the RTT measurements.
    std::jthread control_thread([&](std::stop_token stoken) {
        while (!stoken.stop_requested()) {
            for (const auto& client : server.getClients()) {
                if (client->getReadyState() != ix::ReadyState::Open) continue;
                auto viewer = viewers.find(client.get());
                if (!viewer) continue;

                const int64_t now = now_us();
                viewer->evaluate_tier(client->bufferedAmount(), now);

                std::vector<uint8_t> packet;
                packet.reserve(5);
                packet.push_back(static_cast<uint8_t>(MessageType::Ping));
                write_u32_le(packet, viewer->record_ping_sent(now));
                // Never gated: a ping stuck behind a full send buffer is exactly the signal we want
                client->sendBinary(packet);
            }
            std::this_thread::sleep_for(tier_policy::kPingInterval);
        }
    });

    // start tile damage capture
    std::jthread capture_thread([&](std::stop_token stoken) {
        auto capture = create_tile_source();
        if (!capture || !capture->init(":1")) {
            std::cerr << "Failed to initialize capture source\n";
            return 1;
        }

        // Optional: without it the client keeps showing its own local cursor
        auto cursor = create_cursor_source();
        if (cursor && !cursor->init(":1")) {
            std::cerr << "Cursor capture unavailable; clients will use their local cursor\n";
            cursor.reset();
        }
        CursorImage cursor_img;

        std::cout << "Listening for damage events...\n";
        auto tile_encoder = std::make_unique<JpegTileEncoder>(75);
        X264Encoder video_encoder;
        // Reused across polls: bgra_buffer keeps pointing at the last captured frame,
        // which lets us resync or re-encode without new damage
        RawFrameDelta delta;
        bool idr_pending = false;
        int h264_tail_frames = 0;
        const int64_t epoch_us = now_us();

        struct Target {
            std::shared_ptr<ix::WebSocket> ws;
            std::shared_ptr<Viewer> viewer;
            bool skip_tile_deltas = false; // just resynced from the current buffer
        };
        std::vector<Target> targets;

        auto screen_info_packet = [](int width, int height) {
            std::vector<uint8_t> packet;
            packet.push_back(static_cast<uint8_t>(MessageType::ScreenInfo));
            write_u16_le(packet, static_cast<uint16_t>(width));
            write_u16_le(packet, static_cast<uint16_t>(height));
            return packet;
        };
        auto tile_packet = [](const EncodedTile& tile) {
            std::vector<uint8_t> packet;
            packet.reserve(10 + tile.payload.size());
            packet.push_back(static_cast<uint8_t>(MessageType::Tile));
            packet.push_back(static_cast<uint8_t>(tile.codec));
            write_u16_le(packet, static_cast<uint16_t>(tile.bounds.x));
            write_u16_le(packet, static_cast<uint16_t>(tile.bounds.y));
            write_u16_le(packet, static_cast<uint16_t>(tile.bounds.width));
            write_u16_le(packet, static_cast<uint16_t>(tile.bounds.height));
            packet.insert(packet.end(), tile.payload.begin(), tile.payload.end());
            return packet;
        };
        auto send_stream_mode = [](ix::WebSocket& ws, StreamTier tier) {
            std::vector<uint8_t> packet{static_cast<uint8_t>(MessageType::StreamMode), static_cast<uint8_t>(tier)};
            ws.sendBinary(packet);
        };

        while (!stoken.stop_requested()) {
            // Snapshot the open, registered viewers for this tick
            targets.clear();
            for (const auto& client : server.getClients()) {
                if (client->getReadyState() != ix::ReadyState::Open) continue;
                if (auto viewer = viewers.find(client.get())) targets.push_back({client, std::move(viewer)});
            }

            // Apply tier switches decided by the control thread
            for (auto& t : targets) {
                Viewer& v = *t.viewer;
                const bool want_h264 = v.wants_h264();
                if (want_h264 && v.tier == StreamTier::Tiles) {
                    v.tier = StreamTier::H264;
                    v.awaiting_keyframe = true;
                    v.needs_full_tiles = false;
                    idr_pending = true;
                    send_stream_mode(*t.ws, StreamTier::H264);
                } else if (!want_h264 && v.tier == StreamTier::H264) {
                    v.tier = StreamTier::Tiles;
                    v.awaiting_keyframe = false;
                    v.needs_full_tiles = true; // the tile canvas must not inherit video state
                    send_stream_mode(*t.ws, StreamTier::Tiles);
                }
                if (v.keyframe_requested.exchange(false) && v.tier == StreamTier::H264) {
                    v.awaiting_keyframe = true;
                    idr_pending = true;
                }
            }

            if (cursor_refresh_requested.exchange(false) && cursor) {
                // New clients need the current shape, not just the next change
                cursor->request_full_update();
            }
            if (cursor && cursor->poll_cursor(cursor_img)) {
                std::vector<uint8_t> packet;
                packet.reserve(9 + cursor_img.rgba.size());
                packet.push_back(static_cast<uint8_t>(MessageType::Cursor));
                write_u16_le(packet, static_cast<uint16_t>(cursor_img.width));
                write_u16_le(packet, static_cast<uint16_t>(cursor_img.height));
                write_u16_le(packet, static_cast<uint16_t>(cursor_img.xhot));
                write_u16_le(packet, static_cast<uint16_t>(cursor_img.yhot));
                packet.insert(packet.end(), cursor_img.rgba.begin(), cursor_img.rgba.end());
                for (auto& t : targets) {
                    if (t.ws->bufferedAmount() < kTileBufferGate) t.ws->sendBinary(packet);
                }
            }

            const bool fresh = capture->poll_delta(delta);
            const bool have_frame = delta.bgra_buffer != nullptr;

            if (fresh && delta.full_frame) {
                // Small and essential, so never gated
                const auto packet = screen_info_packet(delta.width, delta.height);
                for (auto& t : targets) {
                    t.ws->sendBinary(packet);
                    // This delta's dirty rects already cover the whole screen
                    if (t.viewer->tier == StreamTier::Tiles) t.viewer->needs_full_tiles = false;
                }
                if (delta.size_changed) {
                    std::cout << "Screen size: " << delta.width << "x" << delta.height << "\n";
                }
            }

            // ---- Tile tier: each rect is encoded once and fanned out ----
            const bool any_tiles = std::any_of(targets.begin(), targets.end(),
                [](const Target& t) { return t.viewer->tier == StreamTier::Tiles; });
            if (any_tiles && have_frame) {
                // Full-screen resync for new viewers, viewers leaving H.264, and
                // viewers that missed tile data while congested. Encoded at most once.
                std::vector<uint8_t> full_packet;
                for (auto& t : targets) {
                    Viewer& v = *t.viewer;
                    if (v.tier != StreamTier::Tiles || !v.needs_full_tiles) continue;
                    if (t.ws->bufferedAmount() >= kTileBufferGate) continue; // retry once drained
                    if (full_packet.empty()) {
                        EncodedTile tile;
                        if (!tile_encoder->encode(delta, {0, 0, delta.width, delta.height}, tile)) break;
                        full_packet = tile_packet(tile);
                    }
                    t.ws->sendBinary(screen_info_packet(delta.width, delta.height));
                    t.ws->sendBinary(full_packet);
                    v.needs_full_tiles = false;
                    t.skip_tile_deltas = true;
                }

                if (fresh) {
                    RectUtils::merge_dirty_rects(delta.dirty_rects);
                    for (const auto& r : delta.dirty_rects) {
                        EncodedTile tile;
                        if (!tile_encoder->encode(delta, r, tile)) continue;
                        const auto packet = tile_packet(tile);
                        for (auto& t : targets) {
                            Viewer& v = *t.viewer;
                            if (v.tier != StreamTier::Tiles || v.needs_full_tiles || t.skip_tile_deltas) continue;
                            if (t.ws->bufferedAmount() < kTileBufferGate) {
                                t.ws->sendBinary(packet);
                            } else {
                                // Pause frame data so audio keeps flowing, and resync later
                                // instead of leaving this region stale forever
                                v.needs_full_tiles = true;
                            }
                        }
                    }
                }
            }

            // ---- H.264 tier: one encode per tick, shared by every H.264 viewer ----
            const bool any_h264 = std::any_of(targets.begin(), targets.end(),
                [](const Target& t) { return t.viewer->tier == StreamTier::H264; });
            if (!any_h264) {
                // Nobody is watching video: don't encode at all
                idr_pending = false;
                h264_tail_frames = 0;
            } else if (have_frame && (fresh || idr_pending || h264_tail_frames > 0)) {
                h264_tail_frames = fresh ? kH264TailFrames : std::max(0, h264_tail_frames - 1);

                if (!video_encoder.matches(delta.width, delta.height)) {
                    // Also re-runs after a resize; a fresh encoder always starts with an IDR
                    if (!video_encoder.init(delta.width, delta.height)) {
                        std::cerr << "Failed to initialize x264 encoder for "
                                  << delta.width << "x" << delta.height << "\n";
                        for (auto& t : targets) {
                            if (t.viewer->tier == StreamTier::H264) t.viewer->downgrade(now_us(), "encoder unavailable");
                        }
                    }
                }
                if (video_encoder.is_open()) {
                    if (idr_pending) video_encoder.request_keyframe();

                    EncodedVideoFrame frame;
                    if (video_encoder.encode_bgra(delta.bgra_buffer, delta.stride, now_us() - epoch_us, frame)) {
                        if (frame.is_keyframe) idr_pending = false;

                        std::vector<uint8_t> packet;
                        packet.reserve(10 + frame.annexb_data.size());
                        packet.push_back(static_cast<uint8_t>(MessageType::H264Video));
                        packet.push_back(frame.is_keyframe ? 1 : 0);
                        write_u64_le(packet, static_cast<uint64_t>(frame.timestamp_us));
                        packet.insert(packet.end(), frame.annexb_data.begin(), frame.annexb_data.end());

                        for (auto& t : targets) {
                            Viewer& v = *t.viewer;
                            if (v.tier != StreamTier::H264) continue;
                            // Deltas are useless until the decoder has a keyframe to reference
                            if (v.awaiting_keyframe && !frame.is_keyframe) continue;
                            if (t.ws->bufferedAmount() > tier_policy::kDowngradeBuffered) {
                                // Dropping a frame would break the reference chain anyway;
                                // fall back to tiles immediately rather than wait for the control tick
                                v.downgrade(now_us(), "send buffer backed up");
                                v.awaiting_keyframe = true;
                                continue;
                            }
                            t.ws->sendBinary(packet);
                            v.awaiting_keyframe = false;
                        }
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60Hz poll
        }
        return 0;
    });

    // start audio capture
    std::jthread audio_thread([&](std::stop_token stoken) {
        auto audio = create_audio_source();
        auto encoder = std::make_unique<OpusAudioEncoder>();
        if (!audio || !audio->init(AudioConfig{48000, 2, 960})) {
            std::cerr << "Failed to initialize audio source\n";
            return 1;
        }
        if (!encoder || !encoder->init(48000, 2, 128000)) {
            std::cerr << "Failed to initialize audio encoder\n";
            return 1;
        }

        std::vector<float> pcm_interleaved;
        std::vector<uint8_t> opus_packet;
        while (!stoken.stop_requested()) {
            if (server.getClients().empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            if (audio->read_chunk(pcm_interleaved)) {
                std::vector<uint8_t> packet;
                if (encoder->encode(pcm_interleaved, opus_packet)) {
                    const size_t payload_bytes = opus_packet.size();
                    packet.reserve(6 + payload_bytes);
                    packet.push_back(static_cast<uint8_t>(MessageType::Audio));
                    packet.push_back(static_cast<uint8_t>(audio->config().channels));
                    write_u32_le(packet, audio->config().sample_rate);
                    const auto* raw_bytes = reinterpret_cast<const uint8_t*>(opus_packet.data());
                    packet.insert(packet.end(), raw_bytes, raw_bytes + payload_bytes);
                    for (const auto& client : server.getClients()) {
                        if (client->getReadyState() == ix::ReadyState::Open) {
                            client->sendBinary(packet);
                        }
                    }
                }
            }
        }
        return 0;
    });

    server.wait();

    return 0;
}
