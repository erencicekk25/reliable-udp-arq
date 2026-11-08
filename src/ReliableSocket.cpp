#include "ReliableSocket.hpp"

#include "Logger.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <sstream>

namespace rudp {

namespace {

std::string errnoMessage(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

bool sameEndpoint(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ReliableSocket::ReliableSocket(Config config)
    : config_(config), rng_(std::random_device{}()) {
    if (config_.windowSize == 0) {
        throw std::invalid_argument("windowSize must be at least 1");
    }
    if (config_.timeout.count() <= 0) {
        throw std::invalid_argument("timeout must be positive");
    }
    if (config_.maxRetries < 0) {
        throw std::invalid_argument("maxRetries must not be negative");
    }
    if (config_.lossRate < 0.0 || config_.lossRate >= 1.0) {
        throw std::invalid_argument("lossRate must be in the range [0, 1)");
    }

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        throw SocketError(errnoMessage("socket() failed"));
    }

    // Random initial sequence number, like TCP's ISN.
    localInitialSeq_ = static_cast<std::uint32_t>(rng_());
}

ReliableSocket::~ReliableSocket() { releaseDescriptor(); }

void ReliableSocket::releaseDescriptor() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// Connection setup
// ---------------------------------------------------------------------------

void ReliableSocket::bind(std::uint16_t port) {
    requireState(State::Idle, "bind");

    int reuse = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        throw SocketError(errnoMessage("setsockopt(SO_REUSEADDR) failed"));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw SocketError(errnoMessage("bind() to port " + std::to_string(port) + " failed"));
    }
    log::write(log::Level::Info, "Bound to UDP port " + std::to_string(port));
}

void ReliableSocket::accept() {
    requireState(State::Idle, "accept");
    log::write(log::Level::Info, "Waiting for a SYN from a sender...");

    while (true) {
        Packet packet;
        sockaddr_in from{};
        // Wait in slices so the loop stays responsive; accept() itself has no deadline.
        if (receivePacket(packet, std::chrono::milliseconds(1000), &from) != RecvStatus::Ok) {
            continue;
        }
        if (packet.header.flags != FLAG_SYN) {
            log::write(log::Level::Warn, "Ignoring " + describe(packet.header) +
                                             " from " + endpointToString(from) +
                                             " (no connection yet)");
            continue;
        }

        peer_ = from;
        hasPeer_ = true;
        isInitiator_ = false;
        peerInitialSeq_ = packet.header.seq;
        expectedSeq_ = peerInitialSeq_ + 1;
        state_ = State::Established;

        sendPacket(FLAG_SYN | FLAG_ACK, localInitialSeq_, expectedSeq_, nullptr, 0, false);
        log::write(log::Level::Info, "Connection established with " + endpointToString(peer_) +
                                         " (peer ISN=" + std::to_string(peerInitialSeq_) + ")");
        return;
    }
}

void ReliableSocket::connect(const std::string& host, std::uint16_t port) {
    requireState(State::Idle, "connect");

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result); rc != 0) {
        throw SocketError("Cannot resolve '" + host + "': " + ::gai_strerror(rc));
    }
    std::memcpy(&peer_, result->ai_addr, sizeof(peer_));
    ::freeaddrinfo(result);
    hasPeer_ = true;
    isInitiator_ = true;

    log::write(log::Level::Info, "Connecting to " + endpointToString(peer_) +
                                     " (ISN=" + std::to_string(localInitialSeq_) + ")");

    for (int attempt = 0; attempt <= config_.maxRetries; ++attempt) {
        sendPacket(FLAG_SYN, localInitialSeq_, 0, nullptr, 0, attempt > 0);

        const auto deadline = Clock::now() + config_.timeout;
        while (Clock::now() < deadline) {
            Packet packet;
            if (receivePacket(packet, remainingUntil(deadline)) != RecvStatus::Ok) {
                continue;
            }
            const bool isSynAck = packet.header.flags == (FLAG_SYN | FLAG_ACK);
            if (isSynAck && packet.header.ack == localInitialSeq_ + 1) {
                sendBase_ = nextSeq_ = localInitialSeq_ + 1;
                state_ = State::Established;
                log::write(log::Level::Info, "Connection established with " +
                                                 endpointToString(peer_));
                return;
            }
            log::write(log::Level::Warn, "Unexpected " + describe(packet.header) +
                                             " during handshake, ignoring");
        }

        ++stats_.timeouts;
        log::write(log::Level::Warn, "Handshake timeout (attempt " + std::to_string(attempt + 1) +
                                         "/" + std::to_string(config_.maxRetries + 1) + ")");
    }

    throw SocketError("Connection to " + endpointToString(peer_) + " failed: no SYN-ACK after " +
                      std::to_string(config_.maxRetries + 1) + " attempts");
}

// ---------------------------------------------------------------------------
// Sender: Go-Back-N sliding window (window size 1 == Stop-and-Wait)
// ---------------------------------------------------------------------------

void ReliableSocket::send(const std::uint8_t* data, std::size_t len) {
    requireState(State::Established, "send");
    if (!isInitiator_) {
        throw SocketError("send() is only supported on the connecting (sender) side");
    }
    if (data == nullptr && len > 0) {
        throw std::invalid_argument("send(): null data with non-zero length");
    }

    std::deque<Segment> window;  // packets sent but not yet acknowledged
    std::size_t offset = 0;
    int consecutiveTimeouts = 0;
    auto timerStart = Clock::now();

    while (offset < len || !window.empty()) {
        // 1) Fill the window with new packets.
        while (window.size() < config_.windowSize && offset < len) {
            const std::size_t chunk = std::min(kMaxPayload, len - offset);
            Segment segment{nextSeq_, std::vector<std::uint8_t>(data + offset, data + offset + chunk), 1};

            if (window.empty()) {
                timerStart = Clock::now();  // timer always tracks the oldest packet
            }
            sendPacket(FLAG_DATA, segment.seq, 0, segment.payload.data(), segment.payload.size(), false);
            stats_.bytesSent += chunk;

            window.push_back(std::move(segment));
            ++nextSeq_;
            offset += chunk;
        }

        // 2) Retransmission timer expired? Go-Back-N: resend the whole window.
        const auto deadline = timerStart + config_.timeout;
        if (Clock::now() >= deadline) {
            ++stats_.timeouts;
            ++consecutiveTimeouts;
            if (consecutiveTimeouts > config_.maxRetries) {
                throw SocketError("Giving up: no ACK for seq=" + std::to_string(sendBase_) + " after " +
                                  std::to_string(config_.maxRetries) + " retransmissions");
            }

            log::write(log::Level::Warn,
                       "Timeout waiting for ACK of seq=" + std::to_string(sendBase_) + " -> resending " +
                           std::to_string(window.size()) + " packet(s) (retry " +
                           std::to_string(consecutiveTimeouts) + "/" +
                           std::to_string(config_.maxRetries) + ")");

            for (auto& segment : window) {
                ++segment.transmissions;
                sendPacket(FLAG_DATA, segment.seq, 0, segment.payload.data(), segment.payload.size(), true);
            }
            timerStart = Clock::now();
            continue;
        }

        // 3) Wait for an ACK until the timer expires.
        Packet packet;
        if (receivePacket(packet, remainingUntil(deadline)) != RecvStatus::Ok) {
            continue;
        }

        const auto flags = packet.header.flags;
        if ((flags & FLAG_SYN) != 0) {
            // Late duplicate SYN-ACK from the handshake; harmless.
            log::write(log::Level::Duplicate, "Ignoring late " + describe(packet.header));
            continue;
        }
        if ((flags & FLAG_ACK) == 0) {
            log::write(log::Level::Warn, "Ignoring unexpected " + describe(packet.header));
            continue;
        }

        const std::uint32_t ackNo = packet.header.ack;
        const bool advancesWindow = seqLess(sendBase_, ackNo) && !seqLess(nextSeq_, ackNo);
        if (!advancesWindow) {
            ++stats_.duplicateAcks;
            log::write(log::Level::Duplicate, "Duplicate ACK ack=" + std::to_string(ackNo) +
                                                  " (window base=" + std::to_string(sendBase_) + ")");
            continue;
        }

        // Cumulative ACK: everything below ackNo has arrived.
        std::size_t released = 0;
        while (!window.empty() && seqLess(window.front().seq, ackNo)) {
            window.pop_front();
            ++released;
        }
        sendBase_ = ackNo;
        consecutiveTimeouts = 0;
        timerStart = Clock::now();

        log::write(log::Level::Recv, "ACK ack=" + std::to_string(ackNo) + " -> " +
                                         std::to_string(released) + " packet(s) confirmed, " +
                                         std::to_string(window.size()) + " still in flight");
    }
}

// ---------------------------------------------------------------------------
// Receiver: in-order delivery with cumulative ACKs
// ---------------------------------------------------------------------------

std::size_t ReliableSocket::receive(std::vector<std::uint8_t>& out) {
    if (state_ == State::PeerClosed) {
        return 0;
    }
    requireState(State::Established, "receive");
    if (isInitiator_) {
        throw SocketError("receive() is only supported on the accepting (receiver) side");
    }

    const auto idleDeadline = Clock::now() + config_.idleTimeout;

    while (true) {
        if (Clock::now() >= idleDeadline) {
            throw SocketError("No packet from peer for " + std::to_string(config_.idleTimeout.count()) +
                              " ms, assuming the connection is dead");
        }

        Packet packet;
        if (receivePacket(packet, remainingUntil(idleDeadline)) != RecvStatus::Ok) {
            continue;
        }

        const auto& h = packet.header;

        // Our SYN-ACK was lost and the sender retried the handshake.
        if (h.flags == FLAG_SYN) {
            if (h.seq == peerInitialSeq_) {
                log::write(log::Level::Duplicate, "Retransmitted SYN, resending SYN|ACK");
                sendPacket(FLAG_SYN | FLAG_ACK, localInitialSeq_, peerInitialSeq_ + 1, nullptr, 0, true);
            } else {
                log::write(log::Level::Warn, "SYN from a different session, ignoring");
            }
            continue;
        }

        if ((h.flags & FLAG_DATA) != 0) {
            if (h.seq == expectedSeq_) {
                out.insert(out.end(), packet.payload.begin(), packet.payload.end());
                ++expectedSeq_;
                stats_.bytesDelivered += packet.payload.size();
                sendAck();
                return packet.payload.size();
            }
            if (seqLess(h.seq, expectedSeq_)) {
                ++stats_.duplicatePackets;
                log::write(log::Level::Duplicate, "Duplicate DATA seq=" + std::to_string(h.seq) +
                                                      " discarded (already delivered), re-ACKing");
            } else {
                ++stats_.outOfOrderPackets;
                log::write(log::Level::Warn, "Out-of-order DATA seq=" + std::to_string(h.seq) +
                                                 " discarded (expected " +
                                                 std::to_string(expectedSeq_) + ")");
            }
            sendAck();  // repeat the cumulative ACK so the sender knows where we are
            continue;
        }

        if ((h.flags & FLAG_FIN) != 0) {
            if (h.seq == expectedSeq_) {
                ++expectedSeq_;
                state_ = State::PeerClosed;
                log::write(log::Level::Info, "FIN received, stream complete");
                sendAck(FLAG_FIN);
                return 0;
            }
            if (seqLess(h.seq, expectedSeq_)) {
                ++stats_.duplicatePackets;
                log::write(log::Level::Duplicate, "Duplicate FIN, resending FIN|ACK");
                sendAck(FLAG_FIN);
            } else {
                ++stats_.outOfOrderPackets;
                log::write(log::Level::Warn, "FIN arrived before all data (seq=" + std::to_string(h.seq) +
                                                 ", expected " + std::to_string(expectedSeq_) + ")");
                sendAck();
            }
            continue;
        }

        log::write(log::Level::Warn, "Ignoring unexpected " + describe(h));
    }
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void ReliableSocket::close() {
    if (state_ == State::Closed) {
        return;
    }

    if (state_ == State::Established && isInitiator_) {
        const std::uint32_t finSeq = nextSeq_;
        bool acknowledged = false;

        for (int attempt = 0; attempt <= config_.maxRetries && !acknowledged; ++attempt) {
            sendPacket(FLAG_FIN, finSeq, 0, nullptr, 0, attempt > 0);

            const auto deadline = Clock::now() + config_.timeout;
            while (Clock::now() < deadline) {
                Packet packet;
                if (receivePacket(packet, remainingUntil(deadline)) != RecvStatus::Ok) {
                    continue;
                }
                const auto& h = packet.header;
                if ((h.flags & FLAG_FIN) && (h.flags & FLAG_ACK) && h.ack == finSeq + 1) {
                    acknowledged = true;
                    break;
                }
                // Anything else here is a leftover duplicate ACK of earlier data.
                log::write(log::Level::Duplicate, "Ignoring " + describe(h) + " while closing");
            }

            if (!acknowledged) {
                ++stats_.timeouts;
                log::write(log::Level::Warn, "No FIN|ACK yet (attempt " + std::to_string(attempt + 1) +
                                                 "/" + std::to_string(config_.maxRetries + 1) + ")");
            }
        }

        ++nextSeq_;
        if (acknowledged) {
            log::write(log::Level::Info, "Connection closed gracefully");
        } else {
            log::write(log::Level::Warn, "Peer never acknowledged FIN, closing anyway");
        }
    } else if (state_ == State::PeerClosed) {
        lingerForFinRetransmissions();
        log::write(log::Level::Info, "Connection closed");
    }

    state_ = State::Closed;
    releaseDescriptor();
}

void ReliableSocket::lingerForFinRetransmissions() {
    // Like TCP's TIME_WAIT: if our FIN|ACK got lost the sender will resend its FIN,
    // so stay around for a few timeout periods and answer it.
    const auto deadline = Clock::now() + config_.timeout * 3;
    log::write(log::Level::Info, "Lingering " + std::to_string((config_.timeout * 3).count()) +
                                     " ms for possible FIN retransmissions");

    while (Clock::now() < deadline) {
        Packet packet;
        if (receivePacket(packet, remainingUntil(deadline)) != RecvStatus::Ok) {
            continue;
        }
        if ((packet.header.flags & FLAG_FIN) != 0) {
            ++stats_.duplicatePackets;
            log::write(log::Level::Duplicate, "Duplicate FIN while lingering, resending FIN|ACK");
            sendAck(FLAG_FIN);
        }
    }
}

// ---------------------------------------------------------------------------
// Low-level packet I/O
// ---------------------------------------------------------------------------

void ReliableSocket::sendPacket(std::uint8_t flags, std::uint32_t seq, std::uint32_t ack,
                                const std::uint8_t* payload, std::size_t len, bool retransmission) {
    if (len > kMaxPayload) {
        throw std::invalid_argument("payload exceeds kMaxPayload");
    }

    PacketHeader header{};
    header.seq = seq;
    header.ack = ack;
    header.flags = flags;
    header.reserved = 0;
    header.length = static_cast<std::uint16_t>(len);
    header.checksum = 0;

    std::vector<std::uint8_t> buffer(kHeaderSize + len);
    const PacketHeader wire = toNetwork(header);
    std::memcpy(buffer.data(), &wire, kHeaderSize);
    if (len > 0) {
        std::memcpy(buffer.data() + kHeaderSize, payload, len);
    }

    // Checksum is computed with the checksum field set to zero, then patched in.
    const std::uint16_t checksum = htons(internetChecksum(buffer.data(), buffer.size()));
    std::memcpy(buffer.data() + offsetof(PacketHeader, checksum), &checksum, sizeof(checksum));

    const std::string what = describe(header);

    if (shouldSimulateLoss()) {
        ++stats_.simulatedDrops;
        log::write(log::Level::Loss, "Simulated network loss: " + what + " was dropped");
        return;
    }

    while (true) {
        const ssize_t sent = ::sendto(fd_, buffer.data(), buffer.size(), 0,
                                      reinterpret_cast<const sockaddr*>(&peer_), sizeof(peer_));
        if (sent >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        throw SocketError(errnoMessage("sendto() failed for " + what));
    }

    ++stats_.packetsSent;
    if (retransmission) {
        ++stats_.retransmissions;
        log::write(log::Level::Retransmit, what);
    } else {
        log::write(log::Level::Send, what);
    }
}

void ReliableSocket::sendAck(std::uint8_t extraFlags) {
    sendPacket(static_cast<std::uint8_t>(FLAG_ACK | extraFlags), 0, expectedSeq_, nullptr, 0, false);
}

bool ReliableSocket::waitReadable(std::chrono::milliseconds timeout) {
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    const int ms = static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, timeout.count()));
    const int rc = ::poll(&pfd, 1, ms);
    if (rc < 0) {
        if (errno == EINTR) {
            return false;  // caller recomputes its deadline and retries
        }
        throw SocketError(errnoMessage("poll() failed"));
    }
    if (rc > 0 && (pfd.revents & (POLLERR | POLLNVAL)) != 0 && (pfd.revents & POLLIN) == 0) {
        throw SocketError("poll() reported an error condition on the socket");
    }
    return rc > 0;
}

ReliableSocket::RecvStatus ReliableSocket::receivePacket(Packet& packet, std::chrono::milliseconds timeout,
                                                         sockaddr_in* from) {
    if (!waitReadable(timeout)) {
        return RecvStatus::Timeout;
    }

    std::uint8_t buffer[kMaxPacketSize + 1];  // +1 lets us detect oversized datagrams
    sockaddr_in source{};
    socklen_t sourceLen = sizeof(source);

    const ssize_t n = ::recvfrom(fd_, buffer, sizeof(buffer), 0,
                                 reinterpret_cast<sockaddr*>(&source), &sourceLen);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return RecvStatus::Ignored;
        }
        if (errno == ECONNREFUSED) {
            // ICMP port unreachable from an earlier datagram; the peer may not be up yet.
            log::write(log::Level::Warn, "Peer port unreachable (is the receiver running?)");
            return RecvStatus::Ignored;
        }
        throw SocketError(errnoMessage("recvfrom() failed"));
    }

    const auto size = static_cast<std::size_t>(n);

    if (hasPeer_ && !sameEndpoint(source, peer_)) {
        log::write(log::Level::Warn, "Ignoring datagram from unknown endpoint " + endpointToString(source));
        return RecvStatus::Ignored;
    }
    if (size < kHeaderSize || size > kMaxPacketSize) {
        ++stats_.corruptedPackets;
        log::write(log::Level::Warn, "Malformed datagram of " + std::to_string(size) + " bytes dropped");
        return RecvStatus::Ignored;
    }

    if (internetChecksum(buffer, size) != 0) {
        ++stats_.corruptedPackets;
        log::write(log::Level::Warn, "Checksum mismatch, corrupted packet dropped");
        return RecvStatus::Ignored;
    }

    PacketHeader wire{};
    std::memcpy(&wire, buffer, kHeaderSize);
    packet.header = toHost(wire);

    if (packet.header.length != size - kHeaderSize) {
        ++stats_.corruptedPackets;
        log::write(log::Level::Warn, "Length field (" + std::to_string(packet.header.length) +
                                         ") does not match datagram size, dropped");
        return RecvStatus::Ignored;
    }

    packet.payload.assign(buffer + kHeaderSize, buffer + size);
    ++stats_.packetsReceived;
    if (from != nullptr) {
        *from = source;
    }

    // ACK-only packets are logged by the sender logic, which knows if they are new or duplicate.
    if ((packet.header.flags & (FLAG_DATA | FLAG_SYN | FLAG_FIN)) != 0) {
        log::write(log::Level::Recv, describe(packet.header));
    }
    return RecvStatus::Ok;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool ReliableSocket::shouldSimulateLoss() {
    return config_.lossRate > 0.0 && lossDist_(rng_) < config_.lossRate;
}

void ReliableSocket::requireState(State expected, const char* operation) const {
    if (state_ != expected) {
        throw SocketError(std::string(operation) + "() called in an invalid connection state");
    }
}

std::chrono::milliseconds ReliableSocket::remainingUntil(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    // Round up so we never busy-loop with a zero timeout just before the deadline.
    return std::max(left + std::chrono::milliseconds(1), std::chrono::milliseconds(0));
}

std::string ReliableSocket::describe(const PacketHeader& header) {
    std::ostringstream os;
    os << flagsToString(header.flags);
    if (header.flags & (FLAG_DATA | FLAG_SYN | FLAG_FIN)) {
        os << " seq=" << header.seq;
    }
    if (header.flags & FLAG_ACK) {
        os << " ack=" << header.ack;
    }
    if (header.length > 0) {
        os << " len=" << header.length;
    }
    return os.str();
}

std::string ReliableSocket::endpointToString(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip)) == nullptr) {
        return "<invalid address>";
    }
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

}  // namespace rudp
