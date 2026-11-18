// Native producer side of ADR-001: spawn and drive bridge/pixil_consumer_bridge.py.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pixil {

class LinkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown out of blocking calls when the stop predicate turns true (SIGINT/SIGTERM).
class LinkInterrupted : public std::runtime_error {
public:
    LinkInterrupted() : std::runtime_error("interrupted") {}
};

enum class ConsumerKind {
    Python,  // temporary default: bridge/pixil_consumer_bridge.py → rgb_matrix_lib
    Native,  // Phase 2: pixil-consumer (ADR-002); becomes the only path after cutover
};

struct LinkOptions {
    ConsumerKind consumer = ConsumerKind::Python;
    std::string python = "python3";
    std::string bridge_script;   // bridge/pixil_consumer_bridge.py (python consumer)
    std::string native_binary;   // path to pixil-consumer (native consumer)
    std::string legacy_root;     // legacy tree containing rgb_matrix_lib/ (python only)
    bool headless = false;
    std::string record_path;  // optional consumer-side JSONL recorder
    int ready_timeout_ms = 20000;
    std::function<bool()> stop_requested;  // polled while blocked
};

class ConsumerLink {
public:
    ConsumerLink() = default;
    ~ConsumerLink();
    ConsumerLink(const ConsumerLink&) = delete;
    ConsumerLink& operator=(const ConsumerLink&) = delete;

    // Spawns the bridge and waits for its ready frame. Returns the ready JSON.
    std::string start(const LinkOptions& opts);
    bool running() const { return pid_ > 0; }

    // Queue one consumer command (e.g. "clear", "draw_batch(\"...\")").
    // Blocks under backpressure when the consumer is behind.
    void send(std::string_view command);

    // Ask the bridge how full its command inbox is. The answer is commands
    // waiting, out of the inbox capacity, plus a monotonic present counter
    // (end_frame + immediate swaps; excludes fade pumps). known is false until
    // a sample arrives; presents_known is false for 8-byte (legacy) Q replies.
    struct QueueSample {
        uint32_t depth = 0;
        uint32_t capacity = 0;
        uint32_t presents = 0;
        bool known = false;
        bool presents_known = false;
        // True only when this sample_queue() call received its Q reply.
        // Stale leftovers must not advance the cfps baseline (else 0 / 2× cfps).
        bool fresh = false;
    };
    QueueSample sample_queue();

    // Waits until every previously sent command has been executed.
    void sync(int timeout_ms = 30000);
    // Fast drain: discard everything queued before this call. Returns count swallowed.
    uint32_t drain(int timeout_ms = 5000);
    // Between-script reset (__SCRIPT_RESET__): reset_fps, end open frame, clear, dispose sprites.
    void script_reset(int timeout_ms = 5000);
    // Drawing-buffer fingerprint once prior commands are executed (__test_snapshot__).
    std::string test_snapshot(int timeout_ms = 30000);

    // Finish queued work, then blackout and exit (normal end of run).
    void finish(int timeout_ms = 60000);
    // Immediate blackout, interrupting rest/backlog. Falls back to kill + emergency blackout.
    void shutdown(int timeout_ms = 5000);

private:
    struct Reply {
        char kind;
        std::string payload;
    };

    void write_frame(int fd, char kind, std::string_view payload);
    void write_control(char byte);
    void read_replies(int timeout_ms);  // reads whatever arrives within timeout
    Reply wait_reply(char kind, int timeout_ms, const char* what);
    bool child_alive();
    void check_stop();
    void close_fds();
    void kill_child();
    void emergency_blackout();

    LinkOptions opts_;
    pid_t pid_ = -1;
    int data_fd_ = -1;
    int control_fd_ = -1;
    int reply_fd_ = -1;
    std::string reply_buf_;
    bool reply_eof_ = false;
    std::deque<Reply> replies_;
    std::string error_;
    uint32_t next_fence_ = 1;
    QueueSample queue_;
    uint32_t queue_ack_ = 0;  // incremented on each Q reply (freshness vs baseline)
};

}  // namespace pixil
