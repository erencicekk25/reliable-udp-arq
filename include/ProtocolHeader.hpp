#pragma once

#include <arpa/inet.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace rudp {

// Packet type flags. Several flags may be combined (e.g. SYN | ACK).
enum Flag : std::uint8_t {
    FLAG_SYN  = 1u << 0,  // connection request
    FLAG_ACK  = 1u << 1,  // 'ack' field is valid (cumulative: next expected seq)
    FLAG_DATA = 1u << 2,  // packet carries payload
    FLAG_FIN  = 1u << 3,  // sender has no more data
};

// Wire format of every packet. All multi-byte fields are in network byte order
// on the wire; use toNetwork()/toHost() to convert.
#pragma pack(push, 1)
struct PacketHeader {
    std::uint32_t seq;       // sequence number of this packet
    std::uint32_t ack;       // cumulative ACK: next sequence number expected
    std::uint8_t  flags;     // combination of Flag values
    std::uint8_t  reserved;  // must be zero (keeps the layout even-sized)
    std::uint16_t length;    // payload length in bytes
    std::uint16_t checksum;  // Internet checksum over header + payload
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 14, "PacketHeader must be tightly packed (14 bytes)");

// Payload size kept well below a typical 1500-byte Ethernet MTU.
constexpr std::size_t kMaxPayload = 1024;
constexpr std::size_t kHeaderSize = sizeof(PacketHeader);
constexpr std::size_t kMaxPacketSize = kHeaderSize + kMaxPayload;

inline PacketHeader toNetwork(PacketHeader h) {
    h.seq      = htonl(h.seq);
    h.ack      = htonl(h.ack);
    h.length   = htons(h.length);
    h.checksum = htons(h.checksum);
    return h;
}

inline PacketHeader toHost(PacketHeader h) {
    h.seq      = ntohl(h.seq);
    h.ack      = ntohl(h.ack);
    h.length   = ntohs(h.length);
    h.checksum = ntohs(h.checksum);
    return h;
}

// RFC 1071 Internet checksum (16-bit one's complement sum).
inline std::uint16_t internetChecksum(const std::uint8_t* data, std::size_t len) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < len; i += 2) {
        sum += static_cast<std::uint32_t>((data[i] << 8) | data[i + 1]);
    }
    if (len % 2 != 0) {
        sum += static_cast<std::uint32_t>(data[len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(~sum & 0xFFFFu);
}

// Serial number arithmetic (RFC 1982 style): true if a comes before b,
// correctly handling 32-bit wrap-around.
inline bool seqLess(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) < 0;
}

inline std::string flagsToString(std::uint8_t flags) {
    std::string out;
    auto append = [&out](const char* name) {
        if (!out.empty()) out += '|';
        out += name;
    };
    if (flags & FLAG_SYN)  append("SYN");
    if (flags & FLAG_ACK)  append("ACK");
    if (flags & FLAG_DATA) append("DATA");
    if (flags & FLAG_FIN)  append("FIN");
    return out.empty() ? "NONE" : out;
}

}  // namespace rudp
