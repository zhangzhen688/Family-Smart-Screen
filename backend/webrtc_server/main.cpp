/**
 * @file main.cpp
 * WebRTC remote video server.
 *
 * Connects to the camera frame relay (TCP :8081), manages WebRTC peer
 * connections from browsers, and sends MJPEG frames over a data channel.
 *
 * Signalling is relayed through the web_server (HTTP ↔ TCP :8082
 * newline-delimited JSON).
 */
#include "frame_source.h"
#include "peer_manager.h"
#include "signaling_server.h"
#include "webrtc_protocol.h"

#include <cstdio>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <unistd.h>
#include <chrono>
#include <thread>

static volatile int g_running = 1;

static void sigHandler(int)
{
    g_running = 0;
}

int main()
{
    std::fprintf(stderr, "============================================\n");
    std::fprintf(stderr, "Smart Screen — WebRTC Remote Video Server\n");
    std::fprintf(stderr, "Relay:    127.0.0.1:%d\n", WEBRTC_RELAY_PORT);
    std::fprintf(stderr, "Signalling: :%d\n", WEBRTC_SIGNALING_PORT);
    std::fprintf(stderr, "============================================\n");

    ::signal(SIGINT, sigHandler);
    ::signal(SIGTERM, sigHandler);

    /* ── Components ────────────────────────────────────────────────── */
    FrameSource     frameSrc;
    PeerManager     peer;
    SignalingServer sigSrv;

    if (!sigSrv.start(WEBRTC_SIGNALING_PORT)) {
        std::fprintf(stderr, "[main] Failed to start signalling server\n");
        return 1;
    }

    /* Track the current web_server client fd (single-client model) */
    int currentClientFd = -1;

    /* Wire: signalling → peer */
    sigSrv.setOfferHandler([&peer, &currentClientFd](const std::string &sdp,
                                                       int clientFd) {
        currentClientFd = clientFd;
        if (!peer.isChannelOpen()) {
            peer.initialize();
        }
        peer.onOffer(sdp);
    });

    sigSrv.setIceHandler([&peer, &currentClientFd](const std::string &candidate,
                                                     const std::string &mid,
                                                     int clientFd) {
        currentClientFd = clientFd;
        peer.onIceCandidate(candidate, mid);
    });

    /* Wire: peer → signalling */
    peer.setAnswerCallback([&sigSrv, &currentClientFd](const std::string &sdp) {
        sigSrv.sendAnswer(currentClientFd, sdp);
    });

    peer.setIceCallback([&sigSrv, &currentClientFd](const std::string &,
                                                      const std::string &) {
        sigSrv.sendOk(currentClientFd);
    });

    /* ── Connect to camera relay (with retry) ──────────────────────── */
    std::fprintf(stderr, "[main] Connecting to camera relay...\n");
    while (g_running && !frameSrc.isConnected()) {
        if (frameSrc.connect("127.0.0.1", WEBRTC_RELAY_PORT))
            break;
        std::fprintf(stderr, "[main] Retrying relay connection in 1s...\n");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (!frameSrc.isConnected()) {
        std::fprintf(stderr, "[main] WARNING: Could not connect to relay — "
                     "is camera_server running?\n");
    }

    /* Forward frames from relay to WebRTC data channel */
    frameSrc.setFrameCallback([&peer](const std::vector<uint8_t> &jpeg) {
        if (peer.isChannelOpen()) {
            peer.sendFrame(jpeg);
        }
    });

    /* ── Main loop ────────────────────────────────────────────────── */
    std::fprintf(stderr, "[main] Entering main loop\n");

    while (g_running) {
        sigSrv.poll();
        frameSrc.poll();

        /* Reconnect to relay if disconnected */
        if (!frameSrc.isConnected()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            frameSrc.connect("127.0.0.1", WEBRTC_RELAY_PORT);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    /* ── Cleanup ──────────────────────────────────────────────────── */
    std::fprintf(stderr, "[main] Shutting down...\n");
    peer.close();
    frameSrc.disconnect();
    sigSrv.stop();

    std::fprintf(stderr, "[main] WebRTC server stopped.\n");
    return 0;
}
