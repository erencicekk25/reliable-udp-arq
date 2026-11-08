// sender: reliably streams a file, a message or stdin to a receiver over UDP.

#include "Logger.hpp"
#include "ReliableSocket.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string host;
    std::uint16_t port = 0;
    std::optional<std::string> file;
    std::optional<std::string> message;
    rudp::Config config;
    bool quiet = false;
};

void printUsage(const char* program) {
    std::cerr
        << "Usage: " << program << " <host> <port> [options]\n"
        << "\n"
        << "Data source (default: stdin):\n"
        << "  -f, --file <path>       send the contents of a file\n"
        << "  -m, --message <text>    send a text message\n"
        << "\n"
        << "Protocol options:\n"
        << "  -w, --window <n>        sliding window size, 1 = Stop-and-Wait (default: 4)\n"
        << "  -t, --timeout <ms>      retransmission timeout in ms (default: 300)\n"
        << "  -r, --retries <n>       max consecutive retries before giving up (default: 10)\n"
        << "  -l, --loss <p>          simulated packet loss probability 0..0.99 (default: 0)\n"
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

// Returns std::nullopt when --help was requested.
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
        } else if (arg == "-f" || arg == "--file") {
            opt.file = next();
        } else if (arg == "-m" || arg == "--message") {
            opt.message = next();
        } else if (arg == "-w" || arg == "--window") {
            opt.config.windowSize = parseUnsigned(next(), arg, 1, 1024);
        } else if (arg == "-t" || arg == "--timeout") {
            opt.config.timeout = std::chrono::milliseconds(
                static_cast<std::chrono::milliseconds::rep>(parseUnsigned(next(), arg, 1, 60000)));
        } else if (arg == "-r" || arg == "--retries") {
            opt.config.maxRetries = static_cast<int>(parseUnsigned(next(), arg, 0, 1000));
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

    if (positional.size() != 2) {
        throw std::invalid_argument("expected <host> and <port>");
    }
    if (opt.file && opt.message) {
        throw std::invalid_argument("--file and --message cannot be used together");
    }
    opt.host = positional[0];
    opt.port = static_cast<std::uint16_t>(parseUnsigned(positional[1], "port", 1, 65535));
    return opt;
}

// FNV-1a hash so both sides can print a fingerprint of the stream and compare.
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
    os << "\n=== Sender summary ===\n"
       << "  bytes sent          : " << s.bytesSent << "\n"
       << "  packets sent        : " << s.packetsSent << "\n"
       << "  retransmissions     : " << s.retransmissions << "\n"
       << "  timeouts            : " << s.timeouts << "\n"
       << "  duplicate ACKs      : " << s.duplicateAcks << "\n"
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
        // Pick the input source.
        std::unique_ptr<std::istream> fileStream;
        std::istringstream messageStream;
        std::istream* input = &std::cin;

        if (options->file) {
            fileStream = std::make_unique<std::ifstream>(*options->file, std::ios::binary);
            if (!*fileStream) {
                throw std::runtime_error("cannot open file '" + *options->file + "'");
            }
            input = fileStream.get();
        } else if (options->message) {
            messageStream.str(*options->message);
            input = &messageStream;
        }

        const auto& cfg = options->config;
        std::ostringstream mode;
        mode << "Mode: " << (cfg.windowSize == 1 ? "Stop-and-Wait" : "Go-Back-N")
             << ", window=" << cfg.windowSize << ", timeout=" << cfg.timeout.count() << "ms"
             << ", loss=" << cfg.lossRate * 100.0 << "%";
        rudp::log::write(rudp::log::Level::Info, mode.str());

        rudp::ReliableSocket socket(cfg);
        socket.connect(options->host, options->port);

        const auto start = std::chrono::steady_clock::now();
        Fnv1a hash;
        std::vector<char> chunk(64 * 1024);

        while (*input) {
            input->read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto got = static_cast<std::size_t>(input->gcount());
            if (got == 0) break;

            const auto* bytes = reinterpret_cast<const std::uint8_t*>(chunk.data());
            hash.update(bytes, got);
            socket.send(bytes, got);
        }
        if (input->bad()) {
            throw std::runtime_error("I/O error while reading input");
        }

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
