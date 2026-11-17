#include "ipc/consumer_link.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace pixil {

namespace {

constexpr int kDataFd = 3;
constexpr int kControlFd = 4;
constexpr int kReplyFd = 5;
constexpr size_t kHeaderSize = 5;

using Clock = std::chrono::steady_clock;

int ms_left(Clock::time_point deadline) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left < 0 ? 0 : static_cast<int>(left);
}

void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

std::string sys_error(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

// Waits up to timeout_ms for pid to exit; returns true if reaped.
bool wait_exit(pid_t pid, int timeout_ms) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return true;
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace

ConsumerLink::~ConsumerLink() {
    if (running()) {
        try {
            shutdown();
        } catch (...) {
        }
    }
}

std::string ConsumerLink::start(const LinkOptions& opts) {
    if (running()) throw LinkError("consumer bridge already running");
    opts_ = opts;
    reply_buf_.clear();
    replies_.clear();
    error_.clear();
    reply_eof_ = false;
    int data[2], control[2], reply[2];
    if (pipe2(data, O_CLOEXEC) || pipe2(control, O_CLOEXEC) || pipe2(reply, O_CLOEXEC)) {
        throw LinkError(sys_error("pipe2"));
    }

    std::vector<std::string> argv_s = {opts.python, "-u", opts.bridge_script, "--legacy-root", opts.legacy_root};
    if (opts.headless) argv_s.push_back("--headless");
    if (!opts.record_path.empty()) {
        argv_s.push_back("--record");
        argv_s.push_back(opts.record_path);
    }
    std::vector<char*> argv;
    for (auto& s : argv_s) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) throw LinkError(sys_error("fork"));
    if (pid == 0) {
        // Move child ends above 5 first so dup2 onto 3/4/5 cannot clobber them.
        int d = fcntl(data[0], F_DUPFD_CLOEXEC, 10);
        int c = fcntl(control[0], F_DUPFD_CLOEXEC, 10);
        int r = fcntl(reply[1], F_DUPFD_CLOEXEC, 10);
        if (d < 0 || c < 0 || r < 0 || dup2(d, kDataFd) < 0 || dup2(c, kControlFd) < 0 || dup2(r, kReplyFd) < 0) {
            _exit(126);
        }
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_IGN);  // producer orchestrates shutdown; ignored disposition survives exec
        execvp(argv[0], argv.data());
        _exit(127);
    }

    pid_ = pid;
    close(data[0]);
    close(control[0]);
    close(reply[1]);
    data_fd_ = data[1];
    control_fd_ = control[1];
    reply_fd_ = reply[0];
    set_nonblocking(data_fd_);
    set_nonblocking(control_fd_);
    set_nonblocking(reply_fd_);

    return wait_reply('H', opts.ready_timeout_ms, "bridge ready").payload;
}

bool ConsumerLink::child_alive() {
    if (pid_ <= 0) return false;
    int status = 0;
    pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == 0) return true;
    pid_ = -1;
    return false;
}

void ConsumerLink::check_stop() {
    if (opts_.stop_requested && opts_.stop_requested()) throw LinkInterrupted();
}

void ConsumerLink::read_replies(int timeout_ms) {
    if (reply_eof_) {
        if (!error_.empty()) throw LinkError("consumer failed: " + error_);
        throw LinkError("consumer bridge exited (reply pipe closed)");
    }
    pollfd p{reply_fd_, POLLIN, 0};
    int n = poll(&p, 1, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return;
        throw LinkError(sys_error("poll"));
    }
    if (n == 0) return;
    char buf[65536];
    for (;;) {
        ssize_t got = read(reply_fd_, buf, sizeof buf);
        if (got > 0) {
            reply_buf_.append(buf, static_cast<size_t>(got));
            continue;
        }
        if (got == 0) {
            reply_eof_ = true;  // parse what arrived first; the next read reports the exit
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        throw LinkError(sys_error("read"));
    }
    while (reply_buf_.size() >= kHeaderSize) {
        uint32_t len = 0;
        std::memcpy(&len, reply_buf_.data() + 1, 4);  // little-endian host (aarch64/x86)
        if (reply_buf_.size() < kHeaderSize + len) break;
        Reply rep{reply_buf_[0], reply_buf_.substr(kHeaderSize, len)};
        reply_buf_.erase(0, kHeaderSize + len);
        if (rep.kind == 'E') {
            error_ = rep.payload;
            std::cerr << "[pixil] consumer error: " << rep.payload << "\n";
        } else {
            replies_.push_back(std::move(rep));
        }
    }
}

ConsumerLink::Reply ConsumerLink::wait_reply(char kind, int timeout_ms, const char* what) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        for (auto it = replies_.begin(); it != replies_.end(); ++it) {
            if (it->kind == kind) {
                Reply r = std::move(*it);
                replies_.erase(it);
                return r;
            }
        }
        if (!error_.empty()) throw LinkError("consumer failed: " + error_);
        check_stop();
        if (Clock::now() >= deadline) throw LinkError(std::string("timed out waiting for ") + what);
        read_replies(std::min(ms_left(deadline), 50));
    }
}

void ConsumerLink::write_frame(int fd, char kind, std::string_view payload) {
    if (!running()) throw LinkError("consumer bridge is not running");
    std::string frame(kHeaderSize, '\0');
    frame[0] = kind;
    auto len = static_cast<uint32_t>(payload.size());
    std::memcpy(frame.data() + 1, &len, 4);
    frame.append(payload);

    size_t off = 0;
    while (off < frame.size()) {
        ssize_t n = write(fd, frame.data() + off, frame.size() - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Backpressure: consumer is behind. Keep reading replies so errors surface.
            check_stop();
            pollfd p[2] = {{fd, POLLOUT, 0}, {reply_fd_, POLLIN, 0}};
            if (poll(p, 2, 100) < 0 && errno != EINTR) throw LinkError(sys_error("poll"));
            if (p[1].revents) read_replies(0);
            if (!error_.empty()) throw LinkError("consumer failed: " + error_);
            continue;
        }
        if (n < 0 && errno == EPIPE) throw LinkError("consumer bridge exited (data pipe closed)");
        throw LinkError(sys_error("write"));
    }
}

void ConsumerLink::write_control(char byte) {
    if (control_fd_ < 0) return;
    ssize_t n;
    do {
        n = write(control_fd_, &byte, 1);
    } while (n < 0 && errno == EINTR);
}

void ConsumerLink::send(std::string_view command) {
    if (!error_.empty()) throw LinkError("consumer failed: " + error_);
    write_frame(data_fd_, 'C', command);
}

void ConsumerLink::sync(int timeout_ms) {
    uint32_t id = next_fence_++;
    write_frame(data_fd_, 'F', std::string_view(reinterpret_cast<const char*>(&id), 4));
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        Reply r = wait_reply('F', ms_left(deadline), "sync fence");
        uint32_t got = 0;
        if (r.payload.size() == 4) std::memcpy(&got, r.payload.data(), 4);
        if (got == id) return;
    }
}

uint32_t ConsumerLink::drain(int timeout_ms) {
    write_control('D');
    write_frame(data_fd_, 'K', {});
    Reply r = wait_reply('K', timeout_ms, "drain fence");
    uint32_t swallowed = 0;
    if (r.payload.size() == 4) std::memcpy(&swallowed, r.payload.data(), 4);
    return swallowed;
}

void ConsumerLink::script_reset(int timeout_ms) {
    write_frame(data_fd_, 'R', {});
    wait_reply('R', timeout_ms, "script reset");
}

std::string ConsumerLink::test_snapshot(int timeout_ms) {
    write_frame(data_fd_, 'T', {});
    return wait_reply('T', timeout_ms, "test snapshot").payload;
}

void ConsumerLink::finish(int timeout_ms) {
    if (!running()) return;
    try {
        write_frame(data_fd_, 'X', {});
        wait_reply('X', timeout_ms, "consumer finish");
        close_fds();
        if (!wait_exit(pid_, 3000)) kill_child();
        pid_ = -1;
    } catch (const LinkInterrupted&) {
        shutdown();
        throw;
    } catch (const LinkError& e) {
        std::cerr << "[pixil] graceful finish failed (" << e.what() << "); forcing shutdown\n";
        shutdown();
    }
}

void ConsumerLink::shutdown(int timeout_ms) {
    if (pid_ <= 0) {
        close_fds();
        return;
    }
    bool blacked_out = false;
    write_control('X');
    auto saved_stop = std::move(opts_.stop_requested);
    opts_.stop_requested = nullptr;  // shutdown itself must not be interrupted
    try {
        if (child_alive()) {
            wait_reply('X', timeout_ms, "consumer shutdown");
            blacked_out = true;
        }
    } catch (const LinkError&) {
    }
    opts_.stop_requested = std::move(saved_stop);
    close_fds();
    if (pid_ > 0 && !wait_exit(pid_, 3000)) kill_child();
    pid_ = -1;
    if (!blacked_out) emergency_blackout();
}

void ConsumerLink::close_fds() {
    for (int* fd : {&data_fd_, &control_fd_, &reply_fd_}) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
    }
}

void ConsumerLink::kill_child() {
    if (pid_ <= 0) return;
    kill(pid_, SIGTERM);
    if (!wait_exit(pid_, 1000)) {
        kill(pid_, SIGKILL);
        wait_exit(pid_, 1000);
    }
    pid_ = -1;
}

void ConsumerLink::emergency_blackout() {
    if (opts_.headless) return;
    std::cerr << "[pixil] running emergency blackout\n";
    std::string code =
        "import os, sys; r = sys.argv[1]; sys.path.insert(0, r); os.chdir(r); "
        "from rgb_matrix_lib.emergency_blackout import run_emergency_blackout; run_emergency_blackout()";
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        execlp(opts_.python.c_str(), opts_.python.c_str(), "-c", code.c_str(), opts_.legacy_root.c_str(),
               static_cast<char*>(nullptr));
        _exit(127);
    }
    if (!wait_exit(pid, 4000)) {
        kill(pid, SIGKILL);
        wait_exit(pid, 500);
    }
}

}  // namespace pixil
