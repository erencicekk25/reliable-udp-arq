#pragma once

#include "ProtocolHeader.hpp"

#include <netinet/in.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace rudp {

// Thrown for unrecoverable socket / protocol errors.
class SocketError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Config {
    // Number of unacknowledged packets allowed in flight.
    // 1 = Stop-and-Wait, >1 = Go-Back-N sliding window.
    std::size_t windowSize = 4;

    // Retransmission timeout (RTO) for the oldest unacknowledged packet.
    std::chrono::milliseconds timeout{300};

    // Consecutive timeouts without progress before giving up.
    int maxRetries = 10;

    // Probability [0, 1) of dropping an outgoing packet on purpose.
    // Used to simulate a lossy network on localhost.
    double lossRate = 0.0;

    // Receiver gives up if nothing arrives from the peer for this long.
    std::chrono::milliseconds idleTimeout{30000};
};

struct Stats {
    std::uint64_t packetsSent = 0;
    std::uint64_t packetsReceived = 0;
    std::uint64_t retransmissions = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t duplicateAcks = 0;
    std::uint64_t duplicatePackets = 0;
    std::uint64_t outOfOrderPackets = 0;
    std::uint64_t corruptedPackets = 0;
    std::uint64_t simulatedDrops = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesDelivered = 0;
};

// A connection-oriented, reliable, in-order byte stream on top of UDP.
//
// Typical usage:
//   sender:   connect() -> send()... -> close()
//   receiver: bind() -> accept() -> receive() until it returns 0 -> close()
class ReliableSocket {
public:
    explicit ReliableSocket(Config config = {});
    ~ReliableSocket();

    ReliableSocket(const ReliableSocket&) = delete;
    ReliableSocket& operator=(const ReliableSocket&) = delete;
    ReliableSocket(ReliableSocket&&) = delete;
    ReliableSocket& operator=(ReliableSocket&&) = delete;

    // Binds the local UDP port (receiver side).
    void bind(std::uint16_t port);

    // UDP port this socket is bound to (useful after bind(0)); 0 if unbound.
    std::uint16_t localPort() const;

    // Blocks until a peer performs the SYN handshake (receiver side).
    void accept();

    // Performs the SYN / SYN-ACK handshake with a remote receiver (sender side).
    void connect(const std::string& host, std::uint16_t port);

    // Reliably sends 'len' bytes. Blocks until every byte is acknowledged.
    void send(const std::uint8_t* data, std::size_t len);
    void send(const std::vector<std::uint8_t>& data) { send(data.data(), data.size()); }

    // Appends the next in-order payload to 'out'.
    // Returns the number of bytes appended, or 0 once the peer closed the stream.
    std::size_t receive(std::vector<std::uint8_t>& out);

    // Graceful shutdown: the sender performs the FIN handshake, the receiver
    // lingers briefly to re-acknowledge a retransmitted FIN. Safe to call twice.
    void close();

    const Stats& stats() const { return stats_; }
    const Config& config() const { return config_; }

private:
    enum class State { Idle, Established, PeerClosed, Closed };
    enum class RecvStatus { Ok, Timeout, Ignored };

    struct Packet {
        PacketHeader header{};  // host byte order
        std::vector<std::uint8_t> payload;
    };

    struct Segment {
        std::uint32_t seq;
        std::vector<std::uint8_t> payload;
        int transmissions;
    };

    using Clock = std::chrono::steady_clock;

    void sendPacket(std::uint8_t flags, std::uint32_t seq, std::uint32_t ack,
                    const std::uint8_t* payload, std::size_t len, bool retransmission);
    void sendAck(std::uint8_t extraFlags = 0);
    RecvStatus receivePacket(Packet& packet, std::chrono::milliseconds timeout,
                             sockaddr_in* from = nullptr);
    bool waitReadable(std::chrono::milliseconds timeout);
    void lingerForFinRetransmissions();
    void releaseDescriptor() noexcept;
    void requireState(State expected, const char* operation) const;
    bool shouldSimulateLoss();

    static std::chrono::milliseconds remainingUntil(Clock::time_point deadline);
    static std::string describe(const PacketHeader& header);
    static std::string endpointToString(const sockaddr_in& addr);

    int fd_ = -1;
    Config config_;
    Stats stats_;
    State state_ = State::Idle;
    bool isInitiator_ = false;

    sockaddr_in peer_{};
    bool hasPeer_ = false;

    // Sender-side sliding window state.
    std::uint32_t sendBase_ = 0;  // oldest unacknowledged sequence number
    std::uint32_t nextSeq_ = 0;   // next sequence number to be used

    // Receiver-side state.
    std::uint32_t peerInitialSeq_ = 0;
    std::uint32_t expectedSeq_ = 0;  // next in-order sequence number expected
    std::uint32_t localInitialSeq_ = 0;

    std::mt19937 rng_;
    std::uniform_real_distribution<double> lossDist_{0.0, 1.0};
};

}  // namespace rudp
