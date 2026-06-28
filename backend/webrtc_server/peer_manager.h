/**
 * @file peer_manager.h
 * WebRTC PeerConnection wrapper using libdatachannel.
 *
 * Manages a single peer connection with a browser:
 *   - Accepts SDP offer, produces SDP answer
 *   - Handles ICE candidate exchange
 *   - Sends JPEG frames over a "video" data channel
 */
#ifndef WEBRTC_PEER_MANAGER_H
#define WEBRTC_PEER_MANAGER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/* Forward declarations for libdatachannel types */
namespace rtc {
class PeerConnection;
class DataChannel;
class Candidate;
struct Description;
}  // namespace rtc

class PeerManager {
public:
    /* Callbacks for signalling — the owner must deliver these to the browser
     * via the web_server signalling proxy. */
    using AnswerCallback   = std::function<void(const std::string &sdp)>;
    using IceCallback      = std::function<void(const std::string &candidate,
                                                const std::string &mid)>;

    PeerManager();
    ~PeerManager();

    /**
     * Initialize the peer connection (creates the RTCPeerConnection with
     * STUN configuration and "video" data channel).
     */
    bool initialize();

    /**
     * Process an SDP offer from the browser.
     * Calls the answer callback when the local answer is ready.
     */
    void onOffer(const std::string &sdp);

    /**
     * Add a remote ICE candidate received from the browser.
     */
    void onIceCandidate(const std::string &candidate,
                        const std::string &mid);

    /**
     * Send a JPEG frame over the data channel.
     * Non-blocking; returns false if the channel is not open or the buffer
     * is full.
     */
    bool sendFrame(const std::vector<uint8_t> &jpeg);

    /** Returns true if the data channel is open and ready to send. */
    bool isChannelOpen() const;

    /** Close the peer connection. */
    void close();

    /** Set callbacks for signalling output. */
    void setAnswerCallback(AnswerCallback cb);
    void setIceCallback(IceCallback cb);

private:
    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::DataChannel>    m_dc;
    bool m_dcOpen;

    AnswerCallback m_answerCb;
    IceCallback    m_iceCb;

    /* Pending candidates to add after setRemoteDescription completes */
    std::vector<std::pair<std::string, std::string>> m_pendingCandidates;

    void onLocalDescription(const rtc::Description &desc);
    void onLocalCandidate(const rtc::Candidate &cand);
};

#endif /* WEBRTC_PEER_MANAGER_H */
