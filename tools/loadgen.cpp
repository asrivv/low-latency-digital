#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "lle/common.hpp"
#include "lle/flow.hpp"
#include "lle/histogram.hpp"
#include "lle/ouch.hpp"
#include "lle/soupbin.hpp"
#include "lle/sysinfo.hpp"

using namespace lle;

namespace {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 15000;
    std::uint32_t sessions = 4;
    std::uint64_t rate = 1'000'000;
    double duration = 10.0;
    double warmup = 2.0;
    std::uint64_t seed = 1;
    std::int64_t pace_ns = 40'000;
    std::string out;
};

struct SessionResult {
    Histogram hist;
    std::uint64_t sent = 0;
    std::uint64_t sent_in_window = 0;
    std::uint64_t received = 0;
    std::uint64_t received_in_window = 0;
    std::uint64_t answered_in_window = 0;
    std::uint64_t unanswered = 0;
    std::string error;
};

int connect_to(const Options& o) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(o.host.c_str(), std::to_string(o.port).c_str(), &hints, &res) != 0 || res == nullptr)
        throw std::runtime_error("cannot resolve " + o.host);
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) throw std::runtime_error(std::string("connect: ") + std::strerror(errno));
    int one = 1, sz = 4 * 1024 * 1024;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
#if defined(SO_NOSIGPIPE)
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

void login(int fd) {
    std::byte buf[64];
    std::size_t n = soupbin::encode_login_request(buf, {"LOAD", "", "", 0});
    if (::send(fd, buf, n, 0) != static_cast<ssize_t>(n)) throw std::runtime_error("login send failed");
    std::size_t got = 0;
    while (got < 3 + soupbin::kLoginAcceptedPayload) {
        ssize_t r = ::recv(fd, buf + got, 3 + soupbin::kLoginAcceptedPayload - got, 0);
        if (r <= 0) throw std::runtime_error("login: connection closed");
        got += static_cast<std::size_t>(r);
    }
    if (static_cast<char>(buf[2]) != 'A') throw std::runtime_error("login rejected");
}

// One SoupBinTCP session on an open-loop schedule. Latency runs from an order's scheduled send
// time, not the time it was actually written, to its first response, so a stalled exchange
// cannot hide queueing delay behind a slower send rate.
class SessionDriver {
public:
    SessionDriver(const Options& o, std::uint32_t index, SessionResult& res)
        : opt_(o),
          res_(res),
          flow_(o.seed * 1000 + index, 1, index),
          sbuf_(kSendBuffer),
          rbuf_(kRecvBuffer) {}

    void connect() {
        fd_ = connect_to(opt_);
        login(fd_);
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    }

    void run(std::int64_t start) {
        const std::int64_t w0 = start + static_cast<std::int64_t>(opt_.warmup * 1e9);
        const std::int64_t w1 = w0 + static_cast<std::int64_t>(opt_.duration * 1e9);
        const std::int64_t drain_until = w1 + 2'000'000'000;
        const double per_session = static_cast<double>(opt_.rate) / opt_.sessions;
        const double period = per_session > 0 ? 1e9 / per_session : 0;
        w0_ = w0;
        w1_ = w1;
        std::uint64_t next = 0;
        while (now_ns() < start) cpu_relax();
        for (;;) {
            std::int64_t now = now_ns();
            if (now < w1) {
                if (period > 0) {
                    auto due = static_cast<std::uint64_t>(static_cast<double>(now - start) / period) + 1;
                    while (next < due && slen_ + kMaxFrame <= kSendBuffer) {
                        generate(start + static_cast<std::int64_t>(static_cast<double>(next) * period));
                        ++next;
                    }
                } else {
                    while (slen_ - soff_ < kUnthrottledBacklog && slen_ + kMaxFrame <= kSendBuffer) generate(now);
                }
            }
            std::size_t backlog = slen_ - soff_;
            if (backlog != 0 && (pacer_.ready(now) || backlog >= kFlushBytes)) {
                pacer_.sent(now);
                flush();
            }
            if (!receive()) break;
            if (now >= w1 && soff_ == slen_ && (pending_.empty() || now >= drain_until)) break;
        }
        res_.unanswered = pending_.size();
        std::byte bye[3];
        ::send(fd_, bye, soupbin::encode_empty(bye, 'O'), 0);
        ::close(fd_);
    }

private:
    static constexpr std::size_t kSendBuffer = 4 * 1024 * 1024;
    static constexpr std::size_t kRecvBuffer = 4 * 1024 * 1024;
    static constexpr std::size_t kMaxFrame = 64;
    static constexpr std::size_t kUnthrottledBacklog = 256 * 1024;
    static constexpr std::size_t kFlushBytes = 64 * 1024;

    void generate(std::int64_t sched) {
        FlowMsg f = flow_.next();
        std::byte msg[64];
        std::size_t n = f.encode(msg);
        if (soff_ == slen_) soff_ = slen_ = 0;
        slen_ += soupbin::encode_frame(sbuf_.data() + slen_, 'U', msg, n);
        const Token& key = f.kind == FlowMsg::Kind::Enter    ? f.enter.token
                           : f.kind == FlowMsg::Kind::Cancel ? f.cancel.token
                                                             : f.replace.replacement;
        pending_[seq_of(key)] = sched;
        ++res_.sent;
        if (sched >= w0_ && sched < w1_) ++res_.sent_in_window;
    }

    void flush() {
        if (soff_ == slen_) return;
        ssize_t n = ::send(fd_, sbuf_.data() + soff_, slen_ - soff_, 0);
        if (n > 0) {
            soff_ += static_cast<std::size_t>(n);
            if (soff_ == slen_) soff_ = slen_ = 0;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            throw std::runtime_error(std::string("send: ") + std::strerror(errno));
        }
        // Keep room for new frames by compacting once the sent prefix dominates.
        if (soff_ > kSendBuffer / 2) {
            std::memmove(sbuf_.data(), sbuf_.data() + soff_, slen_ - soff_);
            slen_ -= soff_;
            soff_ = 0;
        }
    }

    bool receive() {
        ssize_t n = ::recv(fd_, rbuf_.data() + rlen_, kRecvBuffer - rlen_, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
            throw std::runtime_error(std::string("recv: ") + std::strerror(errno));
        }
        rlen_ += static_cast<std::size_t>(n);
        std::int64_t t = now_ns();
        bool open = true;
        auto r = soupbin::parse(rbuf_.data(), rlen_, [&](char type, const std::byte* p, std::size_t len) {
            if (type == 'Z') open = false;
            if (type == 'S' && len != 0) on_ouch(p, t);
            return true;
        });
        if (r.error) throw std::runtime_error("malformed SoupBin stream");
        rlen_ -= r.consumed;
        if (rlen_ != 0) std::memmove(rbuf_.data(), rbuf_.data() + r.consumed, rlen_);
        return open;
    }

    void on_ouch(const std::byte* p, std::int64_t t) {
        ++res_.received;
        if (t >= w0_ && t < w1_) ++res_.received_in_window;
        char type = static_cast<char>(p[0]);
        Token key;
        switch (type) {
            case 'A': key = ouch::decode_accepted(p).token; break;
            case 'J': key = ouch::decode_rejected(p).token; break;
            case 'C': key = ouch::decode_canceled(p).token; break;
            case 'U': key = ouch::decode_replaced(p).token; break;
            default: flow_.on_ouch(p); return;
        }
        auto it = pending_.find(seq_of(key));
        if (it != pending_.end()) {
            std::int64_t sched = it->second;
            if (sched >= w0_ && sched < w1_) {
                res_.hist.record(static_cast<std::uint64_t>(t - sched));
                ++res_.answered_in_window;
            }
            pending_.erase(it);
        }
        flow_.on_ouch(p);
    }

    static std::uint64_t seq_of(const Token& t) {
        std::uint32_t client;
        std::uint64_t seq;
        FlowModel::parse_token(t, client, seq);
        return seq;
    }

    const Options& opt_;
    SessionResult& res_;
    FlowModel flow_;
    int fd_ = -1;
    std::vector<std::byte> sbuf_;
    std::size_t soff_ = 0, slen_ = 0;
    std::vector<std::byte> rbuf_;
    std::size_t rlen_ = 0;
    std::unordered_map<std::uint64_t, std::int64_t> pending_;
    std::int64_t w0_ = 0, w1_ = 0;
    SendPacer pacer_{opt_.pace_ns, 4};
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + k);
        std::string v = argv[++i];
        if (k == "--host") o.host = v;
        else if (k == "--port") o.port = static_cast<std::uint16_t>(std::stoul(v));
        else if (k == "--sessions") o.sessions = static_cast<std::uint32_t>(std::stoul(v));
        else if (k == "--rate") o.rate = std::stoull(v);
        else if (k == "--duration") o.duration = std::stod(v);
        else if (k == "--warmup") o.warmup = std::stod(v);
        else if (k == "--seed") o.seed = std::stoull(v);
        else if (k == "--pace-us") o.pace_ns = std::stoll(v) * 1000;
        else if (k == "--out") o.out = v;
        else throw std::runtime_error("unknown flag " + k);
    }
    if (o.sessions == 0 || o.sessions > 64) throw std::runtime_error("--sessions must be 1..64");
    return o;
}

}  // namespace

int main(int argc, char** argv) try {
    Options opt = parse_args(argc, argv);
    std::vector<std::unique_ptr<SessionResult>> results;
    std::vector<std::unique_ptr<SessionDriver>> drivers;
    for (std::uint32_t i = 0; i < opt.sessions; ++i) {
        results.push_back(std::make_unique<SessionResult>());
        drivers.push_back(std::make_unique<SessionDriver>(opt, i, *results.back()));
        drivers.back()->connect();
    }
    const std::int64_t start = now_ns() + 200'000'000;
    std::vector<std::thread> threads;
    for (std::uint32_t i = 0; i < opt.sessions; ++i) {
        threads.emplace_back([&, i] {
            make_thread_hot(-1);
            try {
                drivers[i]->run(start);
            } catch (const std::exception& e) {
                results[i]->error = e.what();
            }
        });
    }
    for (auto& t : threads) t.join();

    Histogram hist;
    SessionResult total;
    for (auto& r : results) {
        if (!r->error.empty()) throw std::runtime_error(r->error);
        hist.merge(r->hist);
        total.sent += r->sent;
        total.sent_in_window += r->sent_in_window;
        total.received += r->received;
        total.received_in_window += r->received_in_window;
        total.answered_in_window += r->answered_in_window;
        total.unanswered += r->unanswered;
    }
    char buf[2048];
    std::snprintf(buf, sizeof buf,
                  "{\"bench\": \"e2e\", \"host\": \"%s\", \"timer_tick_ns\": %.2f, \"sessions\": %u, "
                  "\"target_rate\": %llu, \"warmup_s\": %.1f, \"duration_s\": %.1f, "
                  "\"achieved_send_rate\": %.0f, \"achieved_response_rate\": %.0f, \"ouch_received_rate\": %.0f, "
                  "\"messages_sent\": %llu, \"messages_received\": %llu, \"unanswered\": %llu, \"latency\": {%s}}",
                  host_description().c_str(), timer_tick_ns(), opt.sessions,
                  static_cast<unsigned long long>(opt.rate), opt.warmup, opt.duration,
                  static_cast<double>(total.sent_in_window) / opt.duration,
                  static_cast<double>(total.answered_in_window) / opt.duration,
                  static_cast<double>(total.received_in_window) / opt.duration,
                  static_cast<unsigned long long>(total.sent), static_cast<unsigned long long>(total.received),
                  static_cast<unsigned long long>(total.unanswered), hist.json_fields().c_str());
    std::puts(buf);
    if (!opt.out.empty()) {
        std::FILE* f = std::fopen(opt.out.c_str(), "w");
        if (f == nullptr) throw std::runtime_error("cannot write " + opt.out);
        std::fprintf(f, "%s\n", buf);
        std::fclose(f);
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "loadgen: %s\n", e.what());
    return 1;
}
