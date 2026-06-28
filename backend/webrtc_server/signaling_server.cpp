/**
 * @file signaling_server.cpp
 * @see signaling_server.h
 *
 * Uses nlohmann/json for parsing (already bundled in the project at
 * backend/voice_server/xiaozhi/json.hpp).
 */
#include "signaling_server.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Bundled single-header JSON library */
#include "json.hpp"
using json = nlohmann::json;

SignalingServer::SignalingServer()
    : m_listenFd(-1), m_clientFd(-1), m_running(false), m_bufLen(0)
{
}

SignalingServer::~SignalingServer()
{
    stop();
}

bool SignalingServer::start(uint16_t port)
{
    m_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd < 0) {
        std::fprintf(stderr, "[Signaling] socket() failed: %s\n",
                     std::strerror(errno));
        return false;
    }

    int opt = 1;
    ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (::bind(m_listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::fprintf(stderr, "[Signaling] bind(:%u) failed: %s\n",
                     port, std::strerror(errno));
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    if (::listen(m_listenFd, 2) < 0) {
        std::fprintf(stderr, "[Signaling] listen() failed: %s\n",
                     std::strerror(errno));
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    /* Set non-blocking */
    int flags = ::fcntl(m_listenFd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(m_listenFd, F_SETFL, flags | O_NONBLOCK);

    m_running = true;
    std::fprintf(stderr, "[Signaling] listening on port %u\n", port);
    return true;
}

void SignalingServer::stop()
{
    m_running = false;
    if (m_clientFd >= 0) {
        ::close(m_clientFd);
        m_clientFd = -1;
    }
    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
    }
    m_bufLen = 0;
}

bool SignalingServer::isRunning() const
{
    return m_running;
}

void SignalingServer::setOfferHandler(OfferHandler h)
{
    m_offerHandler = std::move(h);
}

void SignalingServer::setIceHandler(IceHandler h)
{
    m_iceHandler = std::move(h);
}

void SignalingServer::poll()
{
    if (!m_running) return;

    fd_set rfds;
    FD_ZERO(&rfds);

    int maxFd = m_listenFd;
    FD_SET(m_listenFd, &rfds);

    if (m_clientFd >= 0) {
        FD_SET(m_clientFd, &rfds);
        if (m_clientFd > maxFd) maxFd = m_clientFd;
    }

    struct timeval tv = {0, 10000};  /* 10 ms */
    int n = ::select(maxFd + 1, &rfds, NULL, NULL, &tv);
    if (n < 0) {
        if (errno == EINTR) return;
        std::fprintf(stderr, "[Signaling] select error: %s\n",
                     std::strerror(errno));
        return;
    }
    if (n == 0) return;

    /* Accept new connection (replace existing — one web_server at a time) */
    if (FD_ISSET(m_listenFd, &rfds)) {
        int newFd = ::accept(m_listenFd, NULL, NULL);
        if (newFd >= 0) {
            /* Set non-blocking */
            int flags = ::fcntl(newFd, F_GETFL, 0);
            if (flags >= 0) ::fcntl(newFd, F_SETFL, flags | O_NONBLOCK);

            /* Close previous client if any */
            if (m_clientFd >= 0) ::close(m_clientFd);
            m_clientFd = newFd;
            m_bufLen   = 0;
            std::fprintf(stderr, "[Signaling] web_server connected\n");
        }
        n--;
    }

    /* Read from client */
    if (n > 0 && m_clientFd >= 0 && FD_ISSET(m_clientFd, &rfds)) {
        ssize_t avail = sizeof(m_buf) - m_bufLen - 1;
        if (avail > 0) {
            ssize_t rc = ::read(m_clientFd, m_buf + m_bufLen,
                                static_cast<size_t>(avail));
            if (rc > 0) {
                m_bufLen += static_cast<size_t>(rc);
                m_buf[m_bufLen] = '\0';

                /* Process complete lines */
                char *lineStart = m_buf;
                char *nl;
                while ((nl = std::strchr(lineStart, '\n')) != nullptr) {
                    *nl = '\0';
                    if (nl > lineStart && *(nl - 1) == '\r')
                        *(nl - 1) = '\0';

                    processLine(lineStart);

                    lineStart = nl + 1;
                }

                /* Compact remaining partial line */
                size_t remaining = m_bufLen - static_cast<size_t>(lineStart - m_buf);
                if (remaining > 0 && lineStart != m_buf)
                    std::memmove(m_buf, lineStart, remaining);
                m_bufLen = remaining;
            } else if (rc == 0) {
                disconnectClient();
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                disconnectClient();
            }
        }
    }
}

void SignalingServer::processLine(const std::string &line)
{
    if (line.empty()) return;

    try {
        auto j = json::parse(line);
        std::string type = j.value("type", "");

        if (type == "offer" && m_offerHandler) {
            std::string sdp = j.value("sdp", "");
            m_offerHandler(sdp, m_clientFd);
        } else if (type == "ice" && m_iceHandler) {
            std::string candidate = j.value("candidate", "");
            std::string mid      = j.value("sdpMid", "0");
            m_iceHandler(candidate, mid, m_clientFd);
        } else {
            std::fprintf(stderr, "[Signaling] unknown message type: %s\n",
                         type.c_str());
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[Signaling] JSON parse error: %s\n", e.what());
    }
}

bool SignalingServer::sendAnswer(int clientFd, const std::string &sdp)
{
    if (clientFd < 0) return false;

    /* Escape SDP for JSON — the SDP string is a JSON string value,
     * so we need to escape quotes, backslashes, and control characters. */
    json j;
    j["type"] = "answer";
    j["sdp"]  = sdp;

    std::string msg = j.dump() + "\n";
    ssize_t sent = ::send(clientFd, msg.data(), msg.size(), MSG_NOSIGNAL);
    return (sent == static_cast<ssize_t>(msg.size()));
}

bool SignalingServer::sendOk(int clientFd)
{
    if (clientFd < 0) return false;

    const char *msg = "{\"type\":\"ok\"}\n";
    size_t len = std::strlen(msg);
    ssize_t sent = ::send(clientFd, msg, len, MSG_NOSIGNAL);
    return (sent == static_cast<ssize_t>(len));
}

void SignalingServer::disconnectClient()
{
    if (m_clientFd >= 0) {
        std::fprintf(stderr, "[Signaling] web_server disconnected\n");
        ::close(m_clientFd);
        m_clientFd = -1;
    }
    m_bufLen = 0;
}
