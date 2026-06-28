/**
 * @file frame_source.cpp
 * @see frame_source.h
 */
#include "frame_source.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

FrameSource::FrameSource()
    : m_fd(-1), m_connected(false),
      m_buf_pos(0), m_state(State::ReadHeader), m_frame_size(0)
{
    m_buf.resize(1024 * 1024);  /* 1 MB read buffer */
}

FrameSource::~FrameSource()
{
    disconnect();
}

bool FrameSource::connect(const std::string &host, uint16_t port)
{
    if (m_connected) disconnect();

    m_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_fd < 0) {
        std::fprintf(stderr, "[FrameSource] socket() failed: %s\n", std::strerror(errno));
        return false;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "[FrameSource] inet_pton(%s) failed\n", host.c_str());
        ::close(m_fd);
        m_fd = -1;
        return false;
    }

    if (::connect(m_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::fprintf(stderr, "[FrameSource] connect(%s:%u) failed: %s\n",
                     host.c_str(), port, std::strerror(errno));
        ::close(m_fd);
        m_fd = -1;
        return false;
    }

    /* Set non-blocking */
    int flags = ::fcntl(m_fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);

    m_connected  = true;
    m_state      = State::ReadHeader;
    m_buf_pos    = 0;
    m_frame_size = 0;

    std::fprintf(stderr, "[FrameSource] connected to %s:%u\n", host.c_str(), port);
    return true;
}

void FrameSource::disconnect()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_connected = false;
}

bool FrameSource::isConnected() const
{
    return m_connected;
}

void FrameSource::setFrameCallback(FrameCallback cb)
{
    m_callback = std::move(cb);
}

bool FrameSource::readExact(void *dst, size_t len)
{
    while (m_buf_pos < len) {
        ssize_t n = ::read(m_fd,
                           reinterpret_cast<uint8_t *>(dst) + m_buf_pos,
                           len - m_buf_pos);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;   /* would block — try again later */
            std::fprintf(stderr, "[FrameSource] read error: %s\n", std::strerror(errno));
            disconnect();
            return false;
        }
        if (n == 0) {
            std::fprintf(stderr, "[FrameSource] relay closed connection\n");
            disconnect();
            return false;
        }
        m_buf_pos += static_cast<size_t>(n);
    }
    return true;
}

void FrameSource::poll()
{
    if (!m_connected) return;

    switch (m_state) {

    case State::ReadHeader: {
        uint32_t be_size = 0;
        if (!readExact(&be_size, 4)) return;

        m_frame_size = ntohl(be_size);

        if (m_frame_size == 0 || m_frame_size > 1024 * 1024) {
            std::fprintf(stderr, "[FrameSource] invalid frame size %u\n",
                         m_frame_size);
            /* Try to resync — skip one byte and retry */
            m_buf_pos = 0;
            /* Read and discard one byte */
            uint8_t dummy;
            if (::read(m_fd, &dummy, 1) <= 0) { disconnect(); return; }
            return;
        }

        m_buf_pos = 0;
        m_state   = State::ReadPayload;
        /* Fall through to read the payload */
    }
    /* fallthrough */

    case State::ReadPayload: {
        if (m_frame_size > m_buf.size())
            m_buf.resize(m_frame_size);

        if (!readExact(m_buf.data(), m_frame_size)) return;

        /* Complete frame received */
        if (m_callback) {
            m_callback(std::vector<uint8_t>(m_buf.data(),
                                            m_buf.data() + m_frame_size));
        }

        /* Reset for next frame */
        m_buf_pos = 0;
        m_state   = State::ReadHeader;
        break;
    }
    } /* switch */
}
