/**
 * @file peer_manager.cpp
 * @see peer_manager.h
 */
#include "peer_manager.h"

#include <rtc/rtc.hpp>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <stdexcept>

PeerManager::PeerManager()
    : m_dcOpen(false)
{
}

PeerManager::~PeerManager()
{
    close();
}

bool PeerManager::initialize()
{
    try {
        rtc::Configuration config;
        /* Use Google's public STUN server for NAT traversal.
         * On pure LAN this is optional — host candidates suffice. */
        config.iceServers.emplace_back("stun:stun.l.google.com:19302");
        /* Disable auto-negotiation — we handle SDP exchange manually
         * via the signalling channel. */
        config.disableAutoNegotiation = true;

        m_pc = std::make_shared<rtc::PeerConnection>(std::move(config));

        /* ── Callbacks ─────────────────────────────────────────────── */
        m_pc->onLocalDescription([this](rtc::Description desc) {
            this->onLocalDescription(desc);
        });
        m_pc->onLocalCandidate([this](rtc::Candidate cand) {
            this->onLocalCandidate(cand);
        });
        m_pc->onStateChange([](rtc::PeerConnection::State state) {
            std::fprintf(stderr, "[PeerManager] PC state: %d\n",
                         static_cast<int>(state));
        });
        m_pc->onIceStateChange([](rtc::PeerConnection::IceState state) {
            std::fprintf(stderr, "[PeerManager] ICE state: %d\n",
                         static_cast<int>(state));
        });
        m_pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState gs) {
            std::fprintf(stderr, "[PeerManager] Gathering state: %d\n",
                         static_cast<int>(gs));
        });

        /* Create the "video" data channel — reliable ordered delivery */
        rtc::DataChannelInit dcInit;
        dcInit.reliability.unordered = false;  /* ordered delivery for JPEG frames */
        dcInit.protocol = "mjpeg";

        m_dc = m_pc->createDataChannel("video", std::move(dcInit));

        m_dc->onOpen([this]() {
            std::fprintf(stderr, "[PeerManager] DataChannel opened\n");
            m_dcOpen = true;
        });
        m_dc->onClosed([this]() {
            std::fprintf(stderr, "[PeerManager] DataChannel closed\n");
            m_dcOpen = false;
        });
        m_dc->onError([](std::string err) {
            std::fprintf(stderr, "[PeerManager] DataChannel error: %s\n",
                         err.c_str());
        });

        std::fprintf(stderr, "[PeerManager] initialized\n");
        return true;

    } catch (const std::exception &e) {
        std::fprintf(stderr, "[PeerManager] init failed: %s\n", e.what());
        return false;
    }
}

void PeerManager::onOffer(const std::string &sdp)
{
    if (!m_pc) {
        std::fprintf(stderr, "[PeerManager] onOffer: not initialized\n");
        return;
    }

    try {
        rtc::Description remote(sdp, "offer");
        m_pc->setRemoteDescription(std::move(remote));

        /* Process any ICE candidates that arrived before the remote
         * description was set. */
        for (auto &[cand, mid] : m_pendingCandidates) {
            m_pc->addRemoteCandidate(rtc::Candidate(cand, mid));
        }
        m_pendingCandidates.clear();

    } catch (const std::exception &e) {
        std::fprintf(stderr, "[PeerManager] onOffer error: %s\n", e.what());
    }
}

void PeerManager::onIceCandidate(const std::string &candidate,
                                  const std::string &mid)
{
    if (!m_pc) return;

    try {
        /* If the remote description hasn't been set yet, queue the candidate */
        auto state = m_pc->signalingState();
        if (state == rtc::PeerConnection::SignalingState::Stable ||
            state == rtc::PeerConnection::SignalingState::HaveLocalOffer) {
            /* Not ready yet — queue */
            m_pendingCandidates.emplace_back(candidate, mid);
            return;
        }

        m_pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[PeerManager] onIceCandidate error: %s\n",
                     e.what());
    }
}

bool PeerManager::sendFrame(const std::vector<uint8_t> &jpeg)
{
    if (!m_dc || !m_dcOpen) return false;

    try {
        /* Non-blocking: returns false if the buffer is full */
        return m_dc->send(reinterpret_cast<const std::byte *>(jpeg.data()),
                           jpeg.size());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[PeerManager] sendFrame error: %s\n", e.what());
        return false;
    }
}

bool PeerManager::isChannelOpen() const
{
    return m_dcOpen;
}

void PeerManager::close()
{
    if (m_dc) {
        m_dc->close();
        m_dc.reset();
    }
    if (m_pc) {
        m_pc->close();
        m_pc.reset();
    }
    m_dcOpen = false;
    m_pendingCandidates.clear();
}

void PeerManager::setAnswerCallback(AnswerCallback cb)
{
    m_answerCb = std::move(cb);
}

void PeerManager::setIceCallback(IceCallback cb)
{
    m_iceCb = std::move(cb);
}

void PeerManager::onLocalDescription(const rtc::Description &desc)
{
    /* Only forward answers (not offers) */
    if (desc.type() == rtc::Description::Type::Answer && m_answerCb) {
        m_answerCb(std::string(desc));
    }
}

void PeerManager::onLocalCandidate(const rtc::Candidate &cand)
{
    if (m_iceCb) {
        m_iceCb(std::string(cand), cand.mid());
    }
}
