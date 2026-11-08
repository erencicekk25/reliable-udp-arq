// receiver: accepts one sender, reassembles the in-order byte stream and writes
// it to a file or stdout.

#include "Logger.hpp"
#include "ReliableSocket.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::uint16_t port = 0;
    std::optional<std::string> output;
    rudp::Config config;
    bool quiet = false;
};

void printUsage(const char* program) {
    std::cerr
        << "Usage: " << program << " <port> [options]\n"
        << "\n"
        << "  -o, --output <path>     write received data to a file (default: stdout)\n"
        << "  -t, --timeout <ms>      linger / retransmission timeout in ms (default: 300)\n"
        << "  -i, --idle <ms>         give up if the sender is silent this long (default: 30000)\n"
        << "  -l, --loss <p>          simulated loss probability for outgoing ACKs (default: 0)\n"
        << "  -q, --quiet             only print errors and the final summary\n"
        << "  -h, --help              show this help\n";
}

unsigned long parseUnsigned(const std::string& text, const std::string& name, unsigned long min,
                            unsigned long max) {
    try {
        std::size_t pos = 0;
        if (!text.empty() && text[0] == '-') throw std::invalid_argument("negative");
        const unsigned long value = std::stoul(text, &pos);
        if (pos != text.size() || value < min || value > max) throw std::out_of_range("range");
        return value;
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid value for " + name + ": '" + text + "' (expected " +
                                    std::to_string(min) + ".." + std::to_string(max) + ")");
    }
}

double parseProbability(const std::string& text, const std::string& name) {
    try {
        std::size_t pos = 0;
        const double value = std::stod(text, &pos);
        if (pos != text.size() || value < 0.0 || value >= 1.0) throw std::out_of_range("range");
        return value;
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid value for " + name + ": '" + text +
                                    "' (expected a number in [0, 1))");
    }
}

std::optional<Options> parseArgs(int argc, char** argv) {
    Options opt;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + arg);
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            return std::nullopt;
        } else if (arg == "-o" || arg == "--output") {
            opt.output = next();
        } else if (arg == "-t" || arg == "--timeout") {
            opt.config.timeout = std::chrono::milliseconds(
                static_cast<std::chrono::milliseconds::rep>(parseUnsigned(next(), arg, 1, 60000)));
        } else if (arg == "-i" || arg == "--idle") {
            opt.config.idleTimeout = std::chrono::milliseconds(
                static_cast<std::chrono::milliseconds::rep>(parseUnsigned(next(), arg, 100, 3600000)));
        } else if (arg == "-l" || arg == "--loss") {
            opt.config.lossRate = parseProbability(next(), arg);
        } else if (arg == "-q" || arg == "--quiet") {
            opt.quiet = true;
        } else if (!arg.empty() && arg[0] == '-') {
            throw std::invalid_argument("unknown option: " + arg);
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() != 1) {
        throw std::invalid_argument("expected <port>");
    }
    opt.port = static_cast<std::uint16_t>(parseUnsigned(positional[0], "port", 1, 65535));
    return opt;
}

struct Fnv1a {
    std::uint64_t value = 1469598103934665603ull;
    void update(const std::uint8_t* data, std::size_t len) {
        for (std::size_t i = 0; i < len; ++i) {
            value ^= data[i];
            value *= 1099511628211ull;
        }
    }
};

void printSummary(const rudp::Stats& s, std::uint64_t hash, double seconds) {
    std::ostringstream os;
    os << "\n=== Receiver summary ===\n"
       << "  bytes delivered     : " << s.bytesDelivered << "\n"
       << "  packets received    : " << s.packetsReceived << "\n"
       << "  duplicate packets   : " << s.duplicatePackets << "\n"
       << "  out-of-order packets: " << s.outOfOrderPackets << "\n"
       << "  corrupted packets   : " << s.corruptedPackets << "\n"
       << "  ACKs sent           : " << s.packetsSent << "\n"
       << "  simulated drops     : " << s.simulatedDrops << "\n"
       << "  elapsed             : " << std::fixed << std::setprecision(3) << seconds << " s\n"
       << "  stream fingerprint  : 0x" << std::hex << std::setw(16) << std::setfill('0') << hash << "\n";
    std::cerr << os.str();
}

}  // namespace

int main(int argc, char** argv) {
    std::optional<Options> options;
    try {
        options = parseArgs(argc, argv);
    } catch (const std::invalid_argument& e) {
        std::cerr << "error: " << e.what() << "\n\n";
        printUsage(argv[0]);
        return 2;
    }
    if (!options) {
        printUsage(argv[0]);
        return 0;
    }

    rudp::log::setEnabled(!options->quiet);

    try {
        std::ofstream file;
        std::ostream* output = &std::cout;
        if (options->output) {
            file.open(*options->output, std::ios::binary | std::ios::trunc);
            if (!file) {
                throw std::runtime_error("cannot open output file '" + *options->output + "'");
            }
            output = &file;
        }

        rudp::ReliableSocket socket(options->config);
        socket.bind(options->port);
        socket.accept();

        const auto start = std::chrono::steady_clock::now();
        Fnv1a hash;
        std::vector<std::uint8_t> buffer;

        while (true) {
            buffer.clear();
            const std::size_t got = socket.receive(buffer);
            if (got == 0) break;  // sender finished (FIN)

            hash.update(buffer.data(), buffer.size());
            output->write(reinterpret_cast<const char*>(buffer.data()),
                          static_cast<std::streamsize>(buffer.size()));
            if (!*output) {
                throw std::runtime_error("failed to write received data");
            }
        }
        output->flush();

        socket.close();

        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        printSummary(socket.stats(), hash.value, seconds);
        return 0;
    } catch (const rudp::SocketError& e) {
        rudp::log::write(rudp::log::Level::Error, std::string("network error: ") + e.what());
    } catch (const std::exception& e) {
        rudp::log::write(rudp::log::Level::Error, e.what());
    }
    return 1;
}
