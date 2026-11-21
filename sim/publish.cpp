// starlifter_publish -- record a coordinator's event feed in an Overby Ledger,
// and check a stamped feed against one.
//
// Publish: reads completed-work events (one OTF-1 event per line, as written by
// `starlifter_fleet_sim --feed`), appends each to a ledger file, and writes the
// same events stamped with their ledger coordinate.
//
//   starlifter_publish --feed feed.otf1.jsonl --ledger ledger.otp \
//                      --out stamped.otf1.jsonl [--instance NAME]
//
// The ledger file is created if it does not exist, and then --instance names
// the ledger and is required: an entry that does not say which ledger it
// belongs to is exactly the confusion the field exists to prevent. If the file
// exists it is loaded, verified, and appended to; --instance, if given, must
// match. Running again over the same feed records nothing twice.
//
// It is all or nothing. If any event is refused, neither the ledger file nor
// the output is written, and the message says which event and why.
//
// Verify: checks stamped events against a ledger file, as anyone holding a copy
// of the ledger could.
//
//   starlifter_publish --verify stamped.otf1.jsonl --ledger ledger.otp
//
// Exit status: 0 on success, 1 if verification found a problem, 2 for a usage
// or file error.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "starlifter/ledger_feed.hpp"

namespace {

// The ledger's own name for the schema this tool publishes. Every entry carries
// it, hashed, so a later change to what an entry means cannot be read back
// into old ones.
constexpr const char* kSchemaVersion = "OTF-1/1.0";

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

// Binary, so a line ending is exactly what was written and not what the
// platform thinks it ought to be.
bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

// Write to a neighbour and rename over the target, so an interrupted run cannot
// leave a ledger that is half of one.
bool writeFileAtomically(const std::string& path, const std::string& text) {
    const std::string temporary = path + ".tmp";
    if (!writeFile(temporary, text)) return false;
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") != std::string::npos) out.push_back(std::move(line));
        start = end + 1;
    }
    return out;
}

int usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s --feed FEED.jsonl --ledger LEDGER.otp --out STAMPED.jsonl [--instance NAME]\n"
                 "       %s --verify STAMPED.jsonl --ledger LEDGER.otp\n",
                 program, program);
    return 2;
}

// Load a ledger file and require it to be intact. A ledger that fails
// verification is not appended to and does not vouch for anything.
bool loadLedger(const std::string& path, otp::Ledger& ledger) {
    std::string text;
    if (!readFile(path, text)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    if (!otp::Ledger::from_canonical(text, &ledger)) {
        std::fprintf(stderr, "%s is not a ledger file\n", path.c_str());
        return false;
    }
    const otp::Integrity integrity = ledger.verify();
    if (!integrity.intact) {
        std::fprintf(stderr, "%s fails verification at entry %llu: %s\n", path.c_str(),
                     static_cast<unsigned long long>(integrity.first_bad_seq),
                     integrity.reason.c_str());
        return false;
    }
    return true;
}

int publish(const std::string& feedPath, const std::string& ledgerPath, const std::string& outPath,
            const std::string& instance) {
    std::string feedText;
    if (!readFile(feedPath, feedText)) {
        std::fprintf(stderr, "cannot read %s\n", feedPath.c_str());
        return 2;
    }
    const std::vector<std::string> events = lines(feedText);
    if (events.empty()) {
        std::fprintf(stderr, "%s holds no events\n", feedPath.c_str());
        return 2;
    }

    otp::Ledger ledger;
    if (std::filesystem::exists(ledgerPath)) {
        if (!loadLedger(ledgerPath, ledger)) return 2;
        if (!instance.empty() && instance != ledger.instance()) {
            std::fprintf(stderr, "%s is the ledger \"%s\", not \"%s\"\n", ledgerPath.c_str(),
                         ledger.instance().c_str(), instance.c_str());
            return 2;
        }
    } else {
        if (instance.empty()) {
            std::fprintf(stderr,
                         "%s does not exist, so a new ledger is being created and --instance "
                         "must name it\n",
                         ledgerPath.c_str());
            return 2;
        }
        ledger.set_instance(instance, kSchemaVersion);
    }

    const std::size_t before = ledger.size();
    otp::Bridge bridge(ledger);
    const starlifter::FeedPublication result = starlifter::publishFeed(bridge, events);
    if (!result.ok) {
        std::fprintf(stderr, "event %zu of %s was refused: %s\nnothing was written\n",
                     result.failedEvent, feedPath.c_str(), result.error.c_str());
        return 2;
    }

    std::string stamped;
    for (const std::string& line : result.stamped) {
        stamped += line;
        stamped += '\n';
    }
    if (!writeFileAtomically(ledgerPath, ledger.to_canonical()) || !writeFile(outPath, stamped)) {
        std::fprintf(stderr, "could not write %s or %s\n", ledgerPath.c_str(), outPath.c_str());
        return 2;
    }

    std::printf("ledger \"%s\": %zu event(s) recorded, %zu already there; now %zu entries (was %zu)\n",
                ledger.instance().c_str(), result.published, result.alreadyPublished, ledger.size(),
                before);
    std::printf("stamped events: %s (%zu)\n", outPath.c_str(), result.stamped.size());
    return 0;
}

int verify(const std::string& stampedPath, const std::string& ledgerPath) {
    otp::Ledger ledger;
    if (!loadLedger(ledgerPath, ledger)) return 1;

    std::string text;
    if (!readFile(stampedPath, text)) {
        std::fprintf(stderr, "cannot read %s\n", stampedPath.c_str());
        return 2;
    }
    const std::vector<std::string> stamped = lines(text);
    if (stamped.empty()) {
        std::fprintf(stderr, "%s holds no events\n", stampedPath.c_str());
        return 2;
    }

    const otp::Bridge bridge(ledger);
    const starlifter::FeedVerification result = starlifter::verifyFeed(bridge, stamped);
    for (const std::string& problem : result.problems) std::printf("FAIL %s\n", problem.c_str());
    if (!result.ok) {
        std::printf("%zu of %zu event(s) did not verify against ledger \"%s\"\n",
                    result.problems.size(), stamped.size(), ledger.instance().c_str());
        return 1;
    }
    std::printf("all %zu event(s) verify against ledger \"%s\" (%zu entries, intact)\n",
                result.verified, ledger.instance().c_str(), ledger.size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string feed, ledger, out, instance, verifyPath;
    for (int i = 1; i < argc; ++i) {
        const auto value = [&](std::string& into) {
            if (i + 1 >= argc) return false;
            into = argv[++i];
            return true;
        };
        if (std::strcmp(argv[i], "--feed") == 0) {
            if (!value(feed)) return usage(argv[0]);
        } else if (std::strcmp(argv[i], "--ledger") == 0) {
            if (!value(ledger)) return usage(argv[0]);
        } else if (std::strcmp(argv[i], "--out") == 0) {
            if (!value(out)) return usage(argv[0]);
        } else if (std::strcmp(argv[i], "--instance") == 0) {
            if (!value(instance)) return usage(argv[0]);
        } else if (std::strcmp(argv[i], "--verify") == 0) {
            if (!value(verifyPath)) return usage(argv[0]);
        } else {
            return usage(argv[0]);
        }
    }

    if (ledger.empty()) return usage(argv[0]);
    if (!verifyPath.empty()) {
        if (!feed.empty() || !out.empty()) return usage(argv[0]);
        return verify(verifyPath, ledger);
    }
    if (feed.empty() || out.empty()) return usage(argv[0]);
    return publish(feed, ledger, out, instance);
}
