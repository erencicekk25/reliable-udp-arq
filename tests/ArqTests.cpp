// Tests for reliable-udp-arq: header encoding, checksum, sequence arithmetic
// and complete sender/receiver transfers over loopback, with and without
// simulated packet loss, for both Stop-and-Wait and Go-Back-N.

#include "Logger.hpp"
#include "ProtocolHeader.hpp"
#include "ReliableSocket.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << ": " #condition "\n"; \
        }                                                                                  \
    } while (false)

#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        ++g_checks;                                                                   \
        const auto& actualValue = (actual);                                           \
        const auto& expectedValue = (expected);                                       \
        if (!(actualValue == expectedValue)) {                                        \
            ++g_failures;                                                             \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << ": " #actual   \
                      << " == " #expected "\n    actual:   " << actualValue           \
                      << "\n    expected: " << expectedValue << "\n";                 \
        }                                                                             \
    } while (false)

struct TestCase {
    const char* name;
    void (*body)();
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*body)()) { registry().push_back({name, body}); }
};

#define TEST(name)                                  \
    void name();                                    \
    const Registrar registrar_##name(#name, &name); \
    void name()

std::vector<std::uint8_t> makePayload(std::size_t size) {
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 131 + 17) % 251);
    }
    return data;
}

struct TransferResult {
    std::vector<std::uint8_t> received;
    rudp::Stats senderStats;
    rudp::Stats receiverStats;
    std::string error;
};

// Runs a full handshake + transfer + teardown between two sockets.
TransferResult transfer(const std::vector<std::uint8_t>& payload, rudp::Config config) {
    TransferResult result;
    rudp::ReliableSocket receiver(config);
    receiver.bind(0);
    const std::uint16_t port = receiver.localPort();

    std::string receiverError;
    std::thread receiverThread([&] {
        try {
            receiver.accept();
            std::vector<std::uint8_t> chunk;
            for (;;) {
                chunk.clear();
                if (receiver.receive(chunk) == 0) {
                    break;
                }
                result.received.insert(result.received.end(), chunk.begin(), chunk.end());
            }
            receiver.close();
        } catch (const std::exception& error) {
            receiverError = error.what();
        }
    });

    try {
        rudp::ReliableSocket sender(config);
        sender.connect("127.0.0.1", port);
        sender.send(payload);
        sender.close();
        result.senderStats = sender.stats();
    } catch (const std::exception& error) {
        result.error = std::string("sender: ") + error.what();
    }

    receiverThread.join();
    if (!receiverError.empty()) {
        result.error += " receiver: " + receiverError;
    }
    result.receiverStats = receiver.stats();
    return result;
}

rudp::Config fastConfig(std::size_t window, double lossRate) {
    rudp::Config config;
    config.windowSize = window;
    config.timeout = 40ms;
    config.maxRetries = 30;
    config.lossRate = lossRate;
    config.idleTimeout = 20s;
    return config;
}

// ---------------------------------------------------------------------------
// Protocol header helpers
// ---------------------------------------------------------------------------

TEST(headerIsTightlyPackedAndRoundTrips) {
    CHECK_EQ(rudp::kHeaderSize, std::size_t{14});

    rudp::PacketHeader header{};
    header.seq = 0x01020304U;
    header.ack = 0xA0B0C0D0U;
    header.flags = rudp::FLAG_DATA | rudp::FLAG_ACK;
    header.length = 1024;
    header.checksum = 0xBEEF;

    const rudp::PacketHeader wire = rudp::toNetwork(header);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&wire);
    CHECK_EQ(int{bytes[0]}, 0x01);  // Big-endian on the wire.
    CHECK_EQ(int{bytes[3]}, 0x04);

    const rudp::PacketHeader back = rudp::toHost(wire);
    CHECK_EQ(back.seq, header.seq);
    CHECK_EQ(back.ack, header.ack);
    CHECK_EQ(int{back.flags}, int{header.flags});
    CHECK_EQ(back.length, header.length);
    CHECK_EQ(back.checksum, header.checksum);
}

TEST(internetChecksumMatchesRfc1071Example) {
    // Worked example from RFC 1071, section 3.
    const std::uint8_t data[] = {0x00, 0x01, 0xF2, 0x03, 0xF4, 0xF5, 0xF6, 0xF7};
    CHECK_EQ(rudp::internetChecksum(data, sizeof(data)), std::uint16_t{0x220D});
}

TEST(checksumOfDataPlusChecksumIsZero) {
    std::vector<std::uint8_t> data = makePayload(100);
    const std::uint16_t checksum = rudp::internetChecksum(data.data(), data.size());
    data.push_back(static_cast<std::uint8_t>(checksum >> 8));
    data.push_back(static_cast<std::uint8_t>(checksum & 0xFF));
    CHECK_EQ(rudp::internetChecksum(data.data(), data.size()), std::uint16_t{0});
}

TEST(checksumDetectsSingleBitFlip) {
    std::vector<std::uint8_t> data = makePayload(64);
    const std::uint16_t original = rudp::internetChecksum(data.data(), data.size());
    data[10] ^= 0x01;
    CHECK(rudp::internetChecksum(data.data(), data.size()) != original);

    // Odd lengths pad the final byte with zero.
    const std::uint8_t odd[] = {0x12};
    CHECK_EQ(rudp::internetChecksum(odd, 1), static_cast<std::uint16_t>(~0x1200 & 0xFFFF));
}

TEST(sequenceComparisonHandlesWrapAround) {
    CHECK(rudp::seqLess(1, 2));
    CHECK(!rudp::seqLess(2, 1));
    CHECK(!rudp::seqLess(5, 5));
    CHECK(rudp::seqLess(0xFFFFFFFFU, 0));
    CHECK(!rudp::seqLess(0, 0xFFFFFFFFU));
}

TEST(flagsAreDescribedReadably) {
    CHECK_EQ(rudp::flagsToString(0), std::string("NONE"));
    CHECK_EQ(rudp::flagsToString(rudp::FLAG_SYN | rudp::FLAG_ACK), std::string("SYN|ACK"));
}

TEST(configIsValidated) {
    rudp::Config config;
    config.windowSize = 0;
    bool threw = false;
    try {
        rudp::ReliableSocket socket(config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    config = rudp::Config{};
    config.lossRate = 1.0;
    threw = false;
    try {
        rudp::ReliableSocket socket(config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// End-to-end transfers
// ---------------------------------------------------------------------------

TEST(stopAndWaitTransfersOnCleanLink) {
    const auto payload = makePayload(20 * 1024 + 123);  // Not a multiple of the MSS.
    const TransferResult result = transfer(payload, fastConfig(1, 0.0));
    CHECK_EQ(result.error, std::string());
    CHECK(result.received == payload);
    CHECK_EQ(result.senderStats.retransmissions, std::uint64_t{0});
    CHECK_EQ(result.receiverStats.bytesDelivered, std::uint64_t{payload.size()});
}

TEST(goBackNTransfersOnCleanLink) {
    const auto payload = makePayload(64 * 1024);
    const TransferResult result = transfer(payload, fastConfig(8, 0.0));
    CHECK_EQ(result.error, std::string());
    CHECK(result.received == payload);
    CHECK_EQ(result.senderStats.retransmissions, std::uint64_t{0});
}

TEST(stopAndWaitRecoversFromLoss) {
    const auto payload = makePayload(16 * 1024);
    const TransferResult result = transfer(payload, fastConfig(1, 0.2));
    CHECK_EQ(result.error, std::string());
    CHECK(result.received == payload);
    CHECK(result.senderStats.simulatedDrops + result.receiverStats.simulatedDrops > 0);
    CHECK(result.senderStats.retransmissions > 0);
}

TEST(goBackNRecoversFromLoss) {
    const auto payload = makePayload(48 * 1024);
    const TransferResult result = transfer(payload, fastConfig(8, 0.2));
    CHECK_EQ(result.error, std::string());
    CHECK(result.received == payload);
    CHECK(result.senderStats.retransmissions > 0);
    // Go-Back-N resends whole windows, so the receiver sees out-of-order or
    // duplicate segments and must discard them without corrupting the stream.
    CHECK(result.receiverStats.outOfOrderPackets + result.receiverStats.duplicatePackets > 0);
}

TEST(emptyTransferCompletesCleanly) {
    const TransferResult result = transfer({}, fastConfig(4, 0.0));
    CHECK_EQ(result.error, std::string());
    CHECK(result.received.empty());
}

}  // namespace

int main() {
    rudp::log::setEnabled(false);
    for (const auto& test : registry()) {
        const int failuresBefore = g_failures;
        test.body();
        std::cout << (g_failures == failuresBefore ? "[ PASS ] " : "[ FAIL ] ") << test.name << '\n';
    }
    std::cout << '\n'
              << registry().size() << " tests, " << g_checks << " checks, " << g_failures
              << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}
