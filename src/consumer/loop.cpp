#include "consumer/loop.h"

#include <cerrno>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace pixil::consumer {
namespace {

constexpr int kDataFd = 3;
constexpr int kControlFd = 4;
constexpr int kReplyFd = 5;

struct Frame {
    int kind = 0;  // type byte, or -1 for EOF sentinel
    std::vector<uint8_t> payload;
};

class ReplyWriter {
public:
    explicit ReplyWriter(int fd) : fd_(fd) {}
    void send(char kind, std::string_view payload = {}) {
        std::lock_guard lock(mu_);
        uint8_t hdr[5];
        hdr[0] = static_cast<uint8_t>(kind);
        uint32_t len = static_cast<uint32_t>(payload.size());
        std::memcpy(hdr + 1, &len, 4);
        write_all(hdr, 5);
        if (!payload.empty()) write_all(payload.data(), payload.size());
    }

private:
    void write_all(const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        size_t off = 0;
        while (off < n) {
            ssize_t w = ::write(fd_, c + off, n - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (w == 0) return;
            off += static_cast<size_t>(w);
        }
    }
    int fd_;
    std::mutex mu_;
};

bool read_exact(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    size_t off = 0;
    while (off < n) {
        ssize_t r = ::read(fd, p + off, n - off);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(r);
    }
    return true;
}

class Inbox {
public:
    explicit Inbox(size_t cap) : cap_(cap) {}

    void push(Frame f) {
        std::unique_lock lock(mu_);
        cv_full_.wait(lock, [&] { return q_.size() < cap_ || closed_; });
        if (closed_) return;
        q_.push_back(std::move(f));
        cv_empty_.notify_one();
    }

    // timeout_ms < 0 waits forever; 0 polls; >0 timed wait.
    std::optional<Frame> pop(int timeout_ms) {
        std::unique_lock lock(mu_);
        if (timeout_ms < 0) {
            cv_empty_.wait(lock, [&] { return !q_.empty() || closed_; });
        } else if (timeout_ms == 0) {
            if (q_.empty()) return std::nullopt;
        } else {
            cv_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !q_.empty() || closed_; });
        }
        if (q_.empty()) return std::nullopt;
        Frame f = std::move(q_.front());
        q_.pop_front();
        cv_full_.notify_one();
        return f;
    }

    size_t size() const {
        std::lock_guard lock(mu_);
        return q_.size();
    }
    size_t capacity() const { return cap_; }

    void close() {
        std::lock_guard lock(mu_);
        closed_ = true;
        cv_empty_.notify_all();
        cv_full_.notify_all();
    }

private:
    size_t cap_;
    mutable std::mutex mu_;
    std::condition_variable cv_empty_;
    std::condition_variable cv_full_;
    std::deque<Frame> q_;
    bool closed_ = false;
};

}  // namespace

int run_consumer(MatrixApi& api, const ConsumerOptions& opts) {
    ReplyWriter replies(kReplyFd);
    Inbox inbox(kInboxCapacity);
    std::atomic<bool> drain_requested{false};
    std::atomic<bool> force_shutdown{false};
    uint32_t swallowed = 0;

    api.set_drain_checker([&] { return drain_requested.load(); });
    api.set_shutdown_checker([&] { return force_shutdown.load(); });

    std::ofstream record;
    if (!opts.record_path.empty()) {
        record.open(opts.record_path);
        if (!record) {
            std::cerr << "[pixil-consumer] cannot open --record " << opts.record_path << "\n";
            return 2;
        }
    }
    auto log = [&](const char* kind, const std::string& v = {}) {
        if (!record) return;
        record << "{\"k\":\"" << kind << "\"";
        if (!v.empty()) record << ",\"v\":\"" << v << "\"";
        record << "}\n";
    };

    std::thread data_reader([&] {
        for (;;) {
            uint8_t hdr[5];
            if (!read_exact(kDataFd, hdr, 5)) break;
            uint32_t len = 0;
            std::memcpy(&len, hdr + 1, 4);
            Frame f;
            f.kind = hdr[0];
            f.payload.resize(len);
            if (len && !read_exact(kDataFd, f.payload.data(), len)) break;
            inbox.push(std::move(f));
        }
        inbox.push(Frame{-1, {}});
    });

    std::thread control_reader([&] {
        for (;;) {
            uint8_t b = 0;
            ssize_t r = ::read(kControlFd, &b, 1);
            if (r == 0) return;
            if (r < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (b == 'D') drain_requested.store(true);
            else if (b == 'X') force_shutdown.store(true);
            else if (b == 'Q') {
                uint32_t depth = static_cast<uint32_t>(inbox.size());
                uint32_t cap = static_cast<uint32_t>(inbox.capacity());
                uint32_t presents = api.present_count();
                char payload[12];
                std::memcpy(payload, &depth, 4);
                std::memcpy(payload + 4, &cap, 4);
                std::memcpy(payload + 8, &presents, 4);
                replies.send('Q', std::string_view(payload, 12));
            }
        }
    });
    data_reader.detach();
    control_reader.detach();

    {
        std::string ready = std::string("{\"pid\":") + std::to_string(getpid()) +
                            ",\"impl\":\"native\",\"headless\":" + (opts.headless ? "true" : "false") +
                            ",\"width\":" + std::to_string(api.width()) +
                            ",\"height\":" + std::to_string(api.height()) + "}";
        replies.send('H', ready);
    }

    auto blackout_and_exit = [&](int code) {
        try {
            api.blackout();
        } catch (const std::exception& e) {
            std::cerr << "[pixil-consumer] blackout error: " << e.what() << "\n";
        }
        log("blackout");
        return code;
    };

    for (;;) {
        if (force_shutdown.load()) {
            replies.send('X');
            return blackout_and_exit(0);
        }
        auto item = inbox.pop(10);
        // Python ThreadedBurnoutManager runs on its own thread. With fps pacing
        // the producer keeps the inbox non-empty, so idle-only pumps never ran
        // and Instant trails (Sine_Wave_Color_Flow, burnout=300) never expired.
        if (!drain_requested.load()) {
            try {
                if (!api.abort_requested()) api.pump_fade_while_busy();
            } catch (...) {
            }
        }
        if (!item) continue;
        Frame f = std::move(*item);
        if (f.kind < 0) {
            std::cerr << "[pixil-consumer] producer closed data pipe; blacking out\n";
            return blackout_and_exit(0);
        }
        if (force_shutdown.load() || f.kind == 'X') {
            replies.send('X');
            return blackout_and_exit(0);
        }
        if (f.kind == 'R') {
            api.reset_fps();
            if (api.frame_mode()) {
                try {
                    api.end_frame();
                } catch (...) {
                }
            }
            api.clear();
            api.dispose_all_sprites();
            api.dispose_all_layers();
            log("script_reset");
            replies.send('R');
            continue;
        }
        if (f.kind == 'K') {
            drain_requested.store(false);
            char payload[4];
            std::memcpy(payload, &swallowed, 4);
            log("drain");
            replies.send('K', std::string_view(payload, 4));
            swallowed = 0;
            continue;
        }
        if (f.kind == 'F') {
            replies.send('F', std::string_view(reinterpret_cast<const char*>(f.payload.data()), f.payload.size()));
            continue;
        }
        if (f.kind == 'T') {
            std::string fp;
            try {
                fp = api.fingerprint();
            } catch (const std::exception& e) {
                fp = std::string("error:") + e.what();
            }
            log("snapshot", fp);
            replies.send('T', fp);
            continue;
        }
        if (f.kind != 'C') {
            replies.send('E', "unknown frame type");
            return blackout_and_exit(2);
        }
        if (drain_requested.load()) {
            ++swallowed;
            continue;
        }
        std::string command(f.payload.begin(), f.payload.end());
        log("cmd", command);
        try {
            api.execute_command(command);
        } catch (const std::exception& e) {
            std::string msg = std::string("command failed: ") + command.substr(0, 200) + "\n" + e.what();
            std::cerr << "[pixil-consumer] " << msg << "\n";
            replies.send('E', msg);
            return blackout_and_exit(3);
        }
    }
}

}  // namespace pixil::consumer
