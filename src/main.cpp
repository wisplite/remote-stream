#include <iostream>
#include <thread>
#include <chrono>
#include "capture/factory.hpp"
#include "crypto/gen_certs.hpp"
#include "web/embedded_assets.hpp"
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>

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

    server.setOnClientMessageCallback([](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            std::cout << "Client connected: " << state->getRemoteIp() << std::endl;
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
    server.wait();

    /*auto capture = create_tile_source();
    if (!capture || !capture->init(":1")) {
        std::cerr << "Failed to initialize capture source\n";
        return 1;
    }

    std::cout << "Listening for damage events...\n";
    RawFrameDelta delta;

    while (true) {
        if (capture->poll_delta(delta)) {
            std::cout << "Frame update: " << delta.dirty_rects.size() << " dirty rect(s)\n";
            for (const auto& r : delta.dirty_rects) {
                std::cout << "  -> [" << r.x << ", " << r.y << " " << r.width << "x" << r.height << "]\n";
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60Hz poll
    }*/

    return 0;
}