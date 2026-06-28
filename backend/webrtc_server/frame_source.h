/**
 * @file frame_source.h
 * TCP client that reads MJPEG frames from the camera relay server.
 *
 * Protocol: 4-byte big-endian frame_size, followed by frame_size JPEG bytes.
 * Calls a user-provided callback on each complete frame.
 */
#ifndef WEBRTC_FRAME_SOURCE_H
#define WEBRTC_FRAME_SOURCE_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class FrameSource {
public:
    using FrameCallback = std::function<void(const std::vector<uint8_t> &jpeg)>;

    FrameSource();
    ~FrameSource();

    /**
     * Connect to relay at @p host:@p port.
     * Returns true on success.
     */
    bool connect(const std::string &host, uint16_t port);

    /** Disconnect and clean up. */
    void disconnect();

    /**
     * Process incoming data (non-blocking).
     * Call this periodically (e.g. in a main loop).
     * Calls the frame callback when a complete frame arrives.
     */
    void poll();

    /** Set the callback invoked for each complete JPEG frame. */
    void setFrameCallback(FrameCallback cb);

    /** Returns true if connected to the relay. */
    bool isConnected() const;

private:
    int          m_fd;
    bool         m_connected;

    /* Read buffer and state machine */
    std::vector<uint8_t> m_buf;
    size_t               m_buf_pos;
    enum class State {
        ReadHeader,   // waiting for 4-byte size
        ReadPayload   // waiting for frame_size bytes
    };
    State        m_state;
    uint32_t     m_frame_size;   // expected payload size (host byte order)

    FrameCallback m_callback;

    bool readExact(void *dst, size_t len);
};

#endif /* WEBRTC_FRAME_SOURCE_H */
