// pixil — native Pixil producer (Phase 1). Drives the legacy Python consumer per ADR-001.
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "emit/draw_batch.h"
#include "host/playlist.h"
#include "ipc/consumer_link.h"
#include "runtime/machine.h"
#include "runtime/py_random.h"
#include "runtime/value.h"
#include "syntax/script.h"

namespace fs = std::filesystem;
using namespace pixil;

namespace {

constexpr const char* kVersion = "pixil 0.4.0 (Phase 1 / M4 host)";
constexpr const char* kStatusPath = "/dev/shm/pixil_native_status.json";
constexpr const char* kControlPath = "/dev/shm/pixil_native_control.json";

enum class Advance { None, Next, Jump };

Advance g_advance = Advance::None;
std::string g_jump_target;
std::string g_next_name;  // script that starts after the current one; empty if none
std::chrono::steady_clock::time_point g_last_poll{};

std::atomic<bool> g_stop{false};
std::atomic<bool> g_key_next{false};
std::string g_current_script;

void on_signal(int) { g_stop.store(true); }

// Space skips to the next script, matching the legacy terminal handler.
// Active only when stdin is a terminal (./run interactive). ./run start is not.
class SpaceSkip {
public:
    SpaceSkip() {
        if (!isatty(STDIN_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) return;
        termios raw = saved_;
        raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
        active_ = true;
        reader_ = std::thread([this] { read_keys(); });
        std::cerr << "[pixil] space skips to the next script\n";
    }
    ~SpaceSkip() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
        if (active_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    }
    SpaceSkip(const SpaceSkip&) = delete;
    SpaceSkip& operator=(const SpaceSkip&) = delete;

private:
    void read_keys() {
        while (!stop_.load()) {
            pollfd pfd{STDIN_FILENO, POLLIN, 0};
            const int ready = poll(&pfd, 1, 200);
            if (ready <= 0) continue;
            char key = 0;
            if (read(STDIN_FILENO, &key, 1) == 1 && key == ' ') g_key_next.store(true);
        }
    }

    termios saved_{};
    bool active_ = false;
    std::atomic<bool> stop_{false};
    std::thread reader_;
};

void usage(std::ostream& os) {
    os << "usage: pixil [options] [script.pix ...]\n"
          "\n"
          "Native Pixil producer. The LED consumer is the legacy Python rgb_matrix_lib,\n"
          "driven through bridge/pixil_consumer_bridge.py (docs/ADR-001-ipc.md).\n"
          "\n"
          "options:\n"
          "  --hello              run the M2 hello-matrix demo (no .pix language needed)\n"
          "  script or main/*     one script, several scripts, or a wildcard under scripts/\n"
          "  -t, --time mm:ss     play each script for this long (60:00 is one hour)\n"
          "  -d, --debug LEVEL    DEBUG_OFF (default), DEBUG_CONCISE, DEBUG_SUMMARY,\n"
          "                       or DEBUG_VERBOSE\n"
          "  -q, --queue-monitor  once a second: script, elapsed, frames, commands,\n"
          "                       and inbox fill as waiting/5000 and a percent\n"
          "  --seconds N          hello animation length in seconds (default 10)\n"
          "  --max-commands N     stop each script after N consumer commands (includes preamble)\n"
          "  --max-frames N       stop each script after N frames\n"
          "  --seed N             random seed, applied at the start of every script (default 1)\n"
          "  --trace FILE         write every consumer command, one per line\n"
          "  --dry-run            parse and trace only; a wildcard plays one sorted pass\n"
          "  --headless           stub rgbmatrix in the consumer (no GPIO; real RGB_Api)\n"
          "  --record FILE        consumer-side JSONL log of every command it executed\n"
          "  --legacy-root DIR    legacy tree with rgb_matrix_lib/ (default $PIXIL_LEGACY_ROOT\n"
          "                       or <repo>/../git)\n"
          "  --bridge FILE        consumer bridge script (default <repo>/bridge/pixil_consumer_bridge.py)\n"
          "  --python EXE         Python interpreter for the consumer (default python3)\n"
          "  --version            print version and exit\n"
          "  --help               this help\n"
          "\n"
          "A wildcard keeps playing, in a new order each pass, until the process is\n"
          "signaled. next and jump arrive through /dev/shm/pixil_native_control.json.\n"
          "The show launcher is ./run. Hardware runs need root:\n"
          "  sudo build/release/pixil main/* -t 0:30\n";
}

struct Options {
    bool hello = false;
    bool headless = false;
    bool dry_run = false;
    double seconds = 10.0;
    int duration_seconds = -1;
    DebugLevel debug = DebugLevel::Off;
    bool queue_monitor = false;
    int max_commands = -1;
    int max_frames = -1;
    int64_t seed = 1;
    std::string trace;
    std::string record;
    std::string legacy_root;
    std::string bridge;
    std::string python = "python3";
    std::vector<std::string> scripts;
};

fs::path repo_root() {
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return fs::current_path();
    // <repo>/build/<mode>/pixil
    return exe.parent_path().parent_path().parent_path();
}

Value I(int64_t v) { return Value::integer(v); }
Value F(double v) { return Value::real(v); }
Value S(const char* v) { return Value::str(v); }
Value B(bool v) { return Value::boolean(v); }

void hello(ConsumerLink& link, double seconds) {
    using Clock = std::chrono::steady_clock;

    // Same preamble Pixil.py sends: fps(0) per script, clear + sync_queue at startup.
    link.send("fps(0)");
    link.send("clear");
    link.send("sync_queue");
    link.sync();
    std::cout << "[hello] consumer synced; drawing static scene\n";

    link.send("draw_rectangle(0, 0, 64, 64, blue, 60, false)");
    link.send("draw_rectangle(4, 4, 56, 56, navy, 100, true)");
    link.send("draw_circle(32, 32, 20, gold, 100, false)");
    link.send("draw_line(4, 4, 59, 59, red, 100)");
    link.send("draw_line(59, 4, 4, 59, green, 100)");
    link.send("rest(1.5)");
    link.sync();

    // Control plane: a drain must interrupt a long rest and discard the backlog.
    link.send("rest(10)");
    for (int i = 0; i < 200; ++i) link.send("draw_circle(32, 32, 30, red, 100, true)");
    auto d0 = Clock::now();
    uint32_t swallowed = link.drain();
    double drain_ms = std::chrono::duration<double, std::milli>(Clock::now() - d0).count();
    link.script_reset();
    std::cout << "[hello] drain swallowed=" << swallowed << " in " << drain_ms
              << " ms (rest(10) interrupted); script reset ok\n";
    if (swallowed < 200 || drain_ms > 3000) throw LinkError("drain did not discard the backlog promptly");

    std::cout << "[hello] animating " << seconds << "s at ~30 fps via draw_batch\n";
    PyRandom rng;
    rng.seed(1);
    DrawBatch batch;
    const auto frame_dt = std::chrono::microseconds(33333);
    const auto t0 = Clock::now();
    auto next = t0;
    long frames = 0;
    double max_sync_ms = 0.0;
    double bx = 10, by = 20, vx = 1.3, vy = 0.9;
    std::vector<std::pair<int, int>> sparks;

    while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
        if (g_stop.load()) throw LinkInterrupted();
        double t = static_cast<double>(frames) / 30.0;
        bx += vx;
        by += vy;
        if (bx < 6 || bx > 57) vx = -vx;
        if (by < 6 || by > 57) vy = -vy;

        batch.clear();
        batch.append("draw_polygon", {I(32), I(32), I(26), I(6), I(static_cast<int64_t>(frames) % 100), I(35),
                                      F(t * 40.0), B(false)});
        double a = t * 1.7;
        batch.append("draw_line", {I(32), I(32), F(32 + 40 * std::cos(a)), F(32 + 40 * std::sin(a)), S("white"),
                                   I(70)});
        batch.append("draw_circle", {F(bx), F(by), I(5), I((frames * 3) % 100), I(100), B(true)});
        batch.append("draw_ellipse", {I(32), I(32), F(10 + 6 * std::sin(t * 2)), F(10 + 6 * std::cos(t * 2)),
                                      S("magenta"), I(80), B(false), F(t * 25.0)});
        if (sparks.size() > 24) sparks.erase(sparks.begin());
        sparks.emplace_back(static_cast<int>(rng.random() * 64), static_cast<int>(rng.random() * 64));
        for (auto& [sx, sy] : sparks) batch.append("plot", {I(sx), I(sy), S("white"), I(100)});

        link.send("begin_frame(false)");
        link.send(batch.command());
        link.send("end_frame");
        ++frames;

        if (frames % 30 == 0) {
            auto s0 = Clock::now();
            link.sync();
            max_sync_ms = std::max(max_sync_ms, std::chrono::duration<double, std::milli>(Clock::now() - s0).count());
        }
        next += frame_dt;
        std::this_thread::sleep_until(next);
    }
    link.sync();
    double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    std::cout << "[hello] frames=" << frames << " wall=" << wall << "s fps=" << frames / wall
              << " max_sync_wait_ms=" << max_sync_ms << "\n";
    std::cout << "[hello] buffer fingerprint: " << link.test_snapshot() << "\n";

    link.send("clear");
    link.sync();
}

int parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "pixil: " << name << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            usage(std::cout);
            return 0;
        } else if (a == "--version") {
            std::cout << kVersion << "\n";
            return 0;
        } else if (a == "--hello") {
            o.hello = true;
        } else if (a == "--headless") {
            o.headless = true;
        } else if (a == "--seconds") {
            o.seconds = std::atof(value("--seconds").c_str());
        } else if (a == "-t" || a == "--time") {
            o.duration_seconds = parse_duration(value("-t"));
        } else if (a == "-d" || a == "--debug") {
            o.debug = parse_debug_level(value("-d"));
        } else if (a == "-q" || a == "--queue-monitor") {
            o.queue_monitor = true;
        } else if (a == "--max-commands") {
            o.max_commands = std::atoi(value("--max-commands").c_str());
        } else if (a == "--max-frames") {
            o.max_frames = std::atoi(value("--max-frames").c_str());
        } else if (a == "--seed") {
            o.seed = std::atoll(value("--seed").c_str());
        } else if (a == "--trace") {
            o.trace = value("--trace");
        } else if (a == "--dry-run") {
            o.dry_run = true;
        } else if (a == "--record") {
            o.record = value("--record");
        } else if (a == "--legacy-root") {
            o.legacy_root = value("--legacy-root");
        } else if (a == "--bridge") {
            o.bridge = value("--bridge");
        } else if (a == "--python") {
            o.python = value("--python");
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "pixil: unknown option " << a << "\n";
            usage(std::cerr);
            return 2;
        } else {
            o.scripts.push_back(a);
        }
    }
    return -1;
}

std::string json_escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out.push_back(c);
    }
    return out;
}

void write_status(const fs::path& script, long long started, long long ends, int frames, int commands) {
    const fs::path tmp = std::string(kStatusPath) + ".tmp";
    std::ofstream out(tmp);
    if (!out) return;
    out << "{\"pid\":" << static_cast<long long>(getpid()) << ",\"script\":\""
        << json_escape(script.filename().string()) << "\",\"path\":\"" << json_escape(script.string())
        << "\",\"started\":" << started << ",\"ends\":";
    if (ends < 0) out << "null";
    else out << ends;
    out << ",\"frames\":" << frames << ",\"commands\":" << commands << ",\"next\":";
    if (g_next_name.empty()) out << "null";
    else out << "\"" << json_escape(g_next_name) << "\"";
    out << "}\n";
    out.close();
    std::error_code ec;
    fs::rename(tmp, kStatusPath, ec);
}

void clear_status() {
    std::error_code ec;
    fs::remove(kStatusPath, ec);
    fs::remove(std::string(kStatusPath) + ".tmp", ec);
}

std::string json_field(const std::string& body, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    std::size_t pos = body.find(token);
    if (pos == std::string::npos) return {};
    pos = body.find(':', pos + token.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) return {};
    if (body[pos] == '"') {
        std::string value;
        for (++pos; pos < body.size() && body[pos] != '"'; ++pos) {
            if (body[pos] == '\\' && pos + 1 < body.size()) value.push_back(body[++pos]);
            else value.push_back(body[pos]);
        }
        return value;
    }
    std::string value;
    while (pos < body.size() && std::isdigit(static_cast<unsigned char>(body[pos]))) value.push_back(body[pos++]);
    return value;
}

void poll_control() {
    if (g_advance != Advance::None) return;
    const auto now = std::chrono::steady_clock::now();
    if (g_last_poll.time_since_epoch().count() != 0 && now - g_last_poll < std::chrono::milliseconds(200)) return;
    g_last_poll = now;
    std::ifstream in(kControlPath);
    if (!in) return;
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::string pid = json_field(body, "pid");
    if (pid != std::to_string(static_cast<long long>(getpid()))) return;
    std::error_code ec;
    fs::remove(kControlPath, ec);
    const std::string action = json_field(body, "action");
    if (action == "next") g_advance = Advance::Next;
    else if (action == "jump") {
        g_jump_target = json_field(body, "target");
        if (!g_jump_target.empty()) g_advance = Advance::Jump;
    }
}

std::string duration_text(int seconds) {
    return std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
}

void shuffle_scripts(std::vector<fs::path>& paths) {
    std::random_device device;
    std::mt19937 rng(device());
    std::shuffle(paths.begin(), paths.end(), rng);
}

// Runs one script. Returns Next or Jump when a control request ended it.
Advance run_script(const Options& opt, const fs::path& repo, const fs::path& script_path, ConsumerLink& link,
                   std::ofstream& trace) {
    using Clock = std::chrono::steady_clock;
    g_advance = Advance::None;
    g_current_script = script_path.filename().string();
    const auto wall_start = Clock::now();
    const long long started = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    const long long ends = opt.duration_seconds >= 0 ? started + opt.duration_seconds : -1;
    if (static_cast<int>(opt.debug) >= static_cast<int>(DebugLevel::Concise)) {
        std::cout << "[pixil] playing " << script_path.filename().string();
        if (opt.duration_seconds >= 0) std::cout << " for " << duration_text(opt.duration_seconds);
        std::cout << "\n";
    }
    write_status(script_path, started, ends, 0, 0);

    SymbolTable symbols;
    Script script = parse_script_file(script_path.string());
    resolve_script(script, symbols);
    Machine machine(symbols);
    machine.env.rng.seed(opt.seed);
    seed_numpy_random(opt.seed);

    int emitted = 0;
    auto last_note = wall_start;
    auto note = [&](std::string_view cmd) {
        const auto now = Clock::now();
        if (now - last_note < std::chrono::seconds(1)) return;
        last_note = now;
        write_status(script_path, started, ends, machine.frames, emitted);
        if (!opt.queue_monitor) return;
        const double elapsed = std::chrono::duration<double>(now - wall_start).count();
        std::cout << "[pixil] " << script_path.filename().string() << " elapsed=" << elapsed
                  << "s frames=" << machine.frames << " commands=" << emitted;
        if (!opt.dry_run) {
            const ConsumerLink::QueueSample q = link.sample_queue();
            if (q.known && q.capacity > 0) {
                const unsigned pct = static_cast<unsigned>(q.depth * 100ull / q.capacity);
                std::cout << " inbox=" << q.depth << "/" << q.capacity << " " << pct << "%";
            } else {
                std::cout << " inbox=?";
            }
        }
        if (!cmd.empty() && static_cast<int>(opt.debug) >= static_cast<int>(DebugLevel::Summary)) {
            std::cout << " last=" << cmd.substr(0, 40);
        }
        std::cout << "\n";
    };
    auto emit_cmd = [&](std::string_view cmd) {
        if (opt.max_commands >= 0 && emitted >= opt.max_commands) return;
        if (trace.is_open()) trace << cmd << '\n';
        if (static_cast<int>(opt.debug) >= static_cast<int>(DebugLevel::Verbose)) {
            std::cout << cmd << "\n";
        } else if (static_cast<int>(opt.debug) >= static_cast<int>(DebugLevel::Summary) &&
                   (cmd.starts_with("fps(") || cmd.starts_with("throttle("))) {
            std::cout << "[pixil] " << cmd << "\n";
        }
        if (!opt.dry_run) link.send(cmd);
        ++emitted;
        note(cmd);
    };

    const auto deadline = opt.duration_seconds >= 0 ? wall_start + std::chrono::seconds(opt.duration_seconds)
                                                    : Clock::time_point::max();
    RunOptions run;
    run.lib_dir = (repo / "scripts" / "lib").string();
    run.max_frames = opt.max_frames;
    run.stop_requested = [&] {
        if (g_stop.load()) return true;
        if (g_key_next.exchange(false) && g_advance == Advance::None) {
            g_advance = Advance::Next;
            std::cerr << "[pixil] space: next up "
                      << (g_next_name.empty() ? std::string("(end of playlist)") : g_next_name) << "\n";
        }
        if (Clock::now() >= deadline) return true;
        if (opt.max_commands >= 0 && emitted >= opt.max_commands) return true;
        poll_control();
        return g_advance != Advance::None;
    };
    run.emit = emit_cmd;
    // Same preamble Pixil.py sends before a script body.
    emit_cmd("clear");
    emit_cmd("sync_queue");
    emit_cmd("fps(0)");
    bool failed = false;
    try {
        machine.run(script, run);
    } catch (const PixilError& err) {
        failed = true;
        std::cerr << "[pixil] " << script_path.filename().string() << ": " << err.what() << "\n";
    }
    const bool early = failed || machine.stopped || g_stop.load() || g_advance != Advance::None ||
                       Clock::now() >= deadline;
    if (!opt.dry_run) {
        // drain discards the old script's backlog but leaves its sprites on the
        // panel. clear does not remove sprites; script_reset does.
        if (early) link.drain();
        else link.sync();
        if (!g_stop.load()) link.script_reset();
    }
    std::cout << "[pixil] " << script_path.string() << " frames=" << machine.frames << " commands=" << emitted
              << "\n";
    const Advance why = g_advance;
    g_advance = Advance::None;
    return why;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (int rc = parse_args(argc, argv, opt); rc >= 0) return rc;

    if (opt.scripts.empty() && !opt.hello) {
        usage(std::cerr);
        return 2;
    }
    if (!opt.scripts.empty() && opt.hello) {
        std::cerr << "pixil: pass either --hello or a script, not both\n";
        return 2;
    }

    fs::path repo = repo_root();
    if (opt.legacy_root.empty()) {
        const char* env = std::getenv("PIXIL_LEGACY_ROOT");
        opt.legacy_root = env && *env ? env : (repo / ".." / "git").string();
    }
    if (opt.bridge.empty()) opt.bridge = (repo / "bridge" / "pixil_consumer_bridge.py").string();
    std::error_code ec;
    opt.legacy_root = fs::weakly_canonical(opt.legacy_root, ec).string();
    if (!opt.dry_run && !fs::exists(fs::path(opt.legacy_root) / "rgb_matrix_lib")) {
        std::cerr << "pixil: no rgb_matrix_lib/ under legacy root " << opt.legacy_root << "\n";
        return 2;
    }
    if (!opt.dry_run && !opt.headless && geteuid() != 0) {
        std::cerr << "pixil: warning: not root; the matrix library usually needs sudo for GPIO\n";
    }

    std::signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    SpaceSkip space_skip;

    LinkOptions lo;
    lo.python = opt.python;
    lo.bridge_script = opt.bridge;
    lo.legacy_root = opt.legacy_root;
    lo.headless = opt.headless;
    lo.record_path = opt.record;
    lo.stop_requested = [] { return g_stop.load(); };

    ConsumerLink link;
    try {
        struct StatusGuard {
            ~StatusGuard() { clear_status(); }
        } status_guard;

        std::ofstream trace;
        if (!opt.trace.empty()) {
            trace.open(opt.trace);
            if (!trace) throw std::runtime_error("cannot write " + opt.trace);
        }
        if (!opt.dry_run) {
            std::string ready = link.start(lo);
            std::cout << "[pixil] consumer ready: " << ready << "\n";
        }
        if (opt.hello) {
            if (opt.dry_run) throw std::runtime_error("--dry-run does not apply to --hello");
            hello(link, opt.seconds);
        } else {
            const Playlist playlist = collect_scripts(opt.scripts, repo / "scripts");
            const bool repeat = playlist.wildcard && !opt.dry_run;
            if (static_cast<int>(opt.debug) >= static_cast<int>(DebugLevel::Concise)) {
                std::cout << "[pixil] " << playlist.scripts.size() << (playlist.wildcard ? " matching" : "")
                          << " script" << (playlist.scripts.size() == 1 ? "" : "s") << "\n";
            }
            // The queue always holds the scripts still to play. For a wildcard it
            // is refilled with a fresh shuffle before its last entry starts, so
            // the next script is known while the current one is running.
            std::deque<fs::path> upcoming;
            auto refill = [&] {
                std::vector<fs::path> order = playlist.scripts;
                if (repeat) shuffle_scripts(order);
                upcoming.assign(order.begin(), order.end());
            };
            refill();
            while (!upcoming.empty() && !g_stop.load()) {
                const fs::path current = upcoming.front();
                upcoming.pop_front();
                if (upcoming.empty() && repeat) refill();
                g_next_name = upcoming.empty() ? std::string() : upcoming.front().filename().string();
                Advance why = Advance::None;
                try {
                    why = run_script(opt, repo, current, link, trace);
                } catch (const PixilError& err) {
                    std::cerr << "[pixil] " << current.filename().string() << ": " << err.what() << "\n";
                    if (!opt.dry_run) link.drain();
                    continue;
                }
                if (g_stop.load()) break;
                if (why == Advance::Jump) {
                    try {
                        const Playlist target = collect_scripts({g_jump_target}, repo / "scripts");
                        if (target.scripts.size() == 1) run_script(opt, repo, target.scripts[0], link, trace);
                        else std::cerr << "[pixil] jump target is not one script: " << g_jump_target << "\n";
                    } catch (const PixilError& err) {
                        std::cerr << "[pixil] " << err.what() << "\n";
                    }
                }
            }
            g_next_name.clear();
        }
        if (trace.is_open()) trace.flush();
        if (!opt.dry_run) {
            if (g_stop.load()) link.shutdown();
            else link.finish();
            std::cout << "[pixil] done; panel blacked out by consumer\n";
        }
        return 0;
    } catch (const LinkInterrupted&) {
        clear_status();
        std::cerr << "[pixil] interrupted; blacking out\n";
        if (!opt.dry_run) link.shutdown();
        return 130;
    } catch (const std::exception& e) {
        clear_status();
        std::cerr << "[pixil] error: " << g_current_script << ": " << e.what() << "\n";
        if (!opt.dry_run) link.shutdown();
        return 1;
    }
}
