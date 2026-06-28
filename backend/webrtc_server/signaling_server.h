/**
 * @file signaling_server.h
 * Minimal TCP server for WebRTC signalling relay.
 *
 * Listens on a TCP port and accepts newline-delimited JSON messages
 * from the web_server. Each message is:
 *
 *   {"type":"offer","sdp":"v=0..."}\n
 *   {"type":"ice","candidate":"...","sdpMid":"0","sdpMLineIndex":0}\n
 *
 * Responses are sent back as single-line JSON:
 *
 *   {"type":"answer","sdp":"v=0..."}\n
 *   {"type":"ok"}\n
 */
#ifndef WEBRTC_SIGNALING_SERVER_H
#define WEBRTC_SIGNALING_SERVER_H

#include <cstdint>
#include <functional>
#include <string>

class SignalingServer {
public:
    /** Called when an SDP offer arrives from the browser (via web_server). */
    using OfferHandler = std::function<void(const std::string &sdp,
                                            int clientFd)>;
    /** Called when an ICE candidate arrives. */
    using IceHandler   = std::function<void(const std::string &candidate,
                                            const std::string &mid,
                                            int clientFd)>;

    SignalingServer();
    ~SignalingServer();

    /**
     * Start listening on @p port.
     * Returns true on success.
     */
    bool start(uint16_t port);

    /** Stop the server and close all connections. */
    void stop();

    /**
     * Poll for new connections and messages (non-blocking).
     * Call this periodically from the main loop.
     */
    void poll();

    /** Send an SDP answer back to a specific client. */
    bool sendAnswer(int clientFd, const std::string &sdp);

    /** Send an acknowledgment back to a specific client. */
    bool sendOk(int clientFd);

    /** Returns true if the server is running. */
    bool isRunning() const;

    /** Set handlers. */
    void setOfferHandler(OfferHandler h);
    void setIceHandler(IceHandler h);

private:
    int  m_listenFd;
    int  m_clientFd;
    bool m_running;

    /* Read buffer for the current client */
    char   m_buf[8192];
    size_t m_bufLen;

    OfferHandler m_offerHandler;
    IceHandler   m_iceHandler;

    void processLine(const std::string &line);
    void disconnectClient();
};

#endif /* WEBRTC_SIGNALING_SERVER_H */
