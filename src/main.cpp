#include <iostream>
#include <thread>
#include <chrono>
#include "capture/factory.hpp"
#include "crypto/gen_certs.hpp"
#include "web/embedded_assets.hpp"
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <codec/tile/tile_encoder.hpp>
#include <codec/tile/webp_encoder.cpp>
#include <codec/tile/jpeg_encoder.cpp>
#include <capture/rect_utils.hpp>
#include <atomic>

// First byte of every binary WebSocket message; must match web/app.js
enum class MessageType : uint8_t {
    ScreenInfo = 1, // u16 width, u16 height (LE); always precedes a full frame
    Tile = 2,       // u8 codec, u16 x, y, width, height (LE), then encoded image
};

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

    // Set by connection threads, consumed by the capture thread
    std::atomic<bool> full_frame_requested{false};

    server.setOnClientMessageCallback([&full_frame_requested](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            std::cout << "Client connected: " << state->getRemoteIp() << std::endl;
            full_frame_requested = true;
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            std::cout << "Client disconnected: " << state->getRemoteIp() << std::endl;
        } else if (msg->type == ix::WebSocketMessageType::Message) {
            std::cout << "Client sent message: " << msg->str << std::endl;
        }
    });

    auto res = server.listen();
    if (!res.first) {
        std::cerr << "Failed to listen on port 7200: " << res.second << std::endl;
        return 1;
    }

    server.start();

    // start tile damage capture
    std::jthread capture_thread([&](std::stop_token stoken) {
        auto capture = create_tile_source();
        if (!capture || !capture->init(":1")) {
            std::cerr << "Failed to initialize capture source\n";
            return 1;
        }

        std::cout << "Listening for damage events...\n";
        auto encoder = std::make_unique<JpegTileEncoder>(75);
        RawFrameDelta delta;

        auto write_u16_le = [](std::vector<uint8_t>& buf, uint16_t val) {
            buf.push_back(static_cast<uint8_t>(val & 0xFF));
            buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        };
        auto broadcast = [&](const std::vector<uint8_t>& packet) {
            for (const auto& client : server.getClients()) {
                if (client->getReadyState() == ix::ReadyState::Open) {
                    client->sendBinary(packet);
                }
            }
        };

        while (!stoken.stop_requested()) {
            if (full_frame_requested.exchange(false)) {
                capture->request_full_frame();
            }

            if (capture->poll_delta(delta)) {
                if (delta.full_frame) {
                    std::vector<uint8_t> packet;
                    packet.push_back(static_cast<uint8_t>(MessageType::ScreenInfo));
                    write_u16_le(packet, static_cast<uint16_t>(delta.width));
                    write_u16_le(packet, static_cast<uint16_t>(delta.height));
                    broadcast(packet);
                    if (delta.size_changed) {
                        std::cout << "Screen size: " << delta.width << "x" << delta.height << "\n";
                    }
                }

                RectUtils::merge_dirty_rects(delta.dirty_rects);
                for (const auto& r : delta.dirty_rects) {
                    EncodedTile tile;
                    if (encoder->encode(delta, r, tile)) {
                        std::vector<uint8_t> packet;
                        packet.reserve(10 + tile.payload.size());
                        packet.push_back(static_cast<uint8_t>(MessageType::Tile));
                        packet.push_back(static_cast<uint8_t>(tile.codec));
                        write_u16_le(packet, static_cast<uint16_t>(tile.bounds.x));
                        write_u16_le(packet, static_cast<uint16_t>(tile.bounds.y));
                        write_u16_le(packet, static_cast<uint16_t>(tile.bounds.width));
                        write_u16_le(packet, static_cast<uint16_t>(tile.bounds.height));
                        packet.insert(packet.end(), tile.payload.begin(), tile.payload.end());
                        broadcast(packet);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60Hz poll
        }
        return 0;
    });

    server.wait();

    return 0;
}