#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>
#include <unordered_map>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "lru.h"

// ---------- The database and its lock ----------
// store_mutex protects ALL of: cache, expires, expiry_heap and the counters below.
LRUCache* cache = nullptr;
std::mutex store_mutex;

// ---------- Thread-safe printing ----------
std::mutex log_mutex;
void log_line(const std::string& msg) {
    std::lock_guard<std::mutex> g(log_mutex);
    std::cout << msg << std::endl;
}

// ---------- Expiry bookkeeping ----------
// steady_clock never jumps backwards (system_clock can, if the system time is changed)
using Clock = std::chrono::steady_clock;

struct ExpiryEntry {
    Clock::time_point when;
    std::string key;
    bool operator>(const ExpiryEntry& o) const { return when > o.when; }
};

// key -> the moment it dies. This is the source of truth.
std::unordered_map<std::string, Clock::time_point> expires;

// Min-heap: the soonest expiry is on top. Entries can become stale; a stale entry is
// detected when popped by comparing it against 'expires' (lazy deletion).
std::priority_queue<ExpiryEntry, std::vector<ExpiryEntry>, std::greater<ExpiryEntry>> expiry_heap;

size_t expired_lazy = 0;     // removed because a command touched an expired key
size_t expired_active = 0;   // removed by the background sweeper

// Call with store_mutex held. If the key has expired, delete it and return true.
bool expire_if_needed(const std::string& key) {
    auto it = expires.find(key);
    if (it == expires.end()) return false;
    if (Clock::now() < it->second) return false;
    cache->erase(key);
    expires.erase(it);
    expired_lazy++;
    return true;
}

// Background thread: every 100 ms, remove keys whose time has come.
void sweeper_loop() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> guard(store_mutex);
        auto now = Clock::now();
        while (!expiry_heap.empty() && expiry_heap.top().when <= now) {
            ExpiryEntry e = expiry_heap.top();
            expiry_heap.pop();
            auto it = expires.find(e.key);
            if (it != expires.end() && it->second == e.when) {
                cache->erase(e.key);
                expires.erase(it);
                expired_active++;
            }
        }
    }
}

// ---------- Persistence (snapshots) ----------
// File format (plain text, one entry per line):
//   MINIREDIS-SNAPSHOT 1
//   <key> <expire_at_epoch_ms or -1> <value>      (least recently used first)
//   END <number of entries>
// Keys never contain spaces and values never contain newlines (the command parser
// guarantees both), so this simple format is unambiguous.
const char* SNAPSHOT_PATH   = "dump.snap";
const char* SNAPSHOT_TMP    = "dump.snap.tmp";
const char* SNAPSHOT_HEADER = "MINIREDIS-SNAPSHOT 1";

std::mutex snapshot_mutex;        // only one save at a time (SAVE, periodic, shutdown)
size_t dirty = 0;                 // counts data changes (protected by store_mutex)
size_t dirty_saved = 0;           // value of 'dirty' captured by the last successful save
size_t saves_done = 0;
long long last_save_epoch_s = 0;

long long epoch_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

struct SnapEntry {
    std::string key, value;
    long long expire_at_ms;       // wall-clock time of death, or -1 for "never"
};

// Returns "" on success, otherwise an error message.
std::string save_snapshot() {
    std::lock_guard<std::mutex> save_guard(snapshot_mutex);

    // Step 1: copy the data while holding the store lock (fast, memory only)
    std::vector<SnapEntry> entries;
    size_t version;
    {
        std::lock_guard<std::mutex> guard(store_mutex);
        version = dirty;
        Clock::time_point now = Clock::now();
        long long now_ms = epoch_ms_now();
        entries.reserve(cache->size());
        cache->for_each_lru_to_mru([&](const std::string& k, const std::string& v) {
            long long expire_at = -1;
            auto it = expires.find(k);
            if (it != expires.end()) {
                long long remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          it->second - now).count();
                if (remaining <= 0) return;           // already dead: do not save it
                expire_at = now_ms + remaining;       // convert to an absolute wall-clock time
            }
            entries.push_back({k, v, expire_at});
        });
    }   // store lock released here: clients are not blocked by the slow disk work below

    // Step 2: write to a temp file
    FILE* f = std::fopen(SNAPSHOT_TMP, "wb");
    if (!f) return std::string("cannot open temp file: ") + std::strerror(errno);

    bool ok = std::fputs(SNAPSHOT_HEADER, f) >= 0 && std::fputc('\n', f) != EOF;
    for (size_t i = 0; ok && i < entries.size(); i++) {
        const SnapEntry& e = entries[i];
        std::string line = e.key + " " + std::to_string(e.expire_at_ms) + " " + e.value + "\n";
        ok = std::fwrite(line.data(), 1, line.size(), f) == line.size();
    }
    if (ok) {
        std::string trailer = "END " + std::to_string(entries.size()) + "\n";
        ok = std::fwrite(trailer.data(), 1, trailer.size(), f) == trailer.size();
    }
    if (ok) ok = (std::fflush(f) == 0) && (fsync(fileno(f)) == 0);   // force it onto the disk
    int saved_errno = errno;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        std::remove(SNAPSHOT_TMP);
        return std::string("write failed: ") + std::strerror(saved_errno);
    }

    // Step 3: atomically replace the old snapshot
    if (std::rename(SNAPSHOT_TMP, SNAPSHOT_PATH) != 0) {
        std::string err = std::string("rename failed: ") + std::strerror(errno);
        std::remove(SNAPSHOT_TMP);
        return err;
    }
    int dfd = open(".", O_RDONLY);        // make the rename itself durable
    if (dfd >= 0) { fsync(dfd); close(dfd); }

    // Step 4: record that this version of the data is safely on disk
    {
        std::lock_guard<std::mutex> guard(store_mutex);
        dirty_saved = version;
        saves_done++;
        last_save_epoch_s = epoch_ms_now() / 1000;
    }
    return "";
}

// Runs once at startup, before any other thread exists. Returns "" on success.
std::string load_snapshot(size_t& loaded, size_t& skipped) {
    loaded = skipped = 0;
    std::ifstream in(SNAPSHOT_PATH, std::ios::binary);
    if (!in) return "cannot open file";

    std::string line;
    if (!std::getline(in, line) || line != SNAPSHOT_HEADER)
        return "missing or unknown header";

    long long now_ms = epoch_ms_now();
    bool seen_end = false;
    size_t line_no = 1;

    while (std::getline(in, line)) {
        line_no++;
        if (seen_end) return "unexpected data after the END line";

        size_t p1 = line.find(' ');
        size_t p2 = (p1 == std::string::npos) ? std::string::npos : line.find(' ', p1 + 1);

        if (p2 == std::string::npos) {
            // An entry line always has two spaces. The trailer has exactly one.
            if (line.compare(0, 4, "END ") != 0) return "malformed line " + std::to_string(line_no);
            char* endp = nullptr;
            unsigned long long n = std::strtoull(line.c_str() + 4, &endp, 10);
            if (*endp != '\0' || n != loaded + skipped)
                return "entry count does not match the END line";
            seen_end = true;
            continue;
        }

        std::string key = line.substr(0, p1);
        std::string ttl = line.substr(p1 + 1, p2 - p1 - 1);
        std::string value = line.substr(p2 + 1);
        char* endp = nullptr;
        long long expire_at = std::strtoll(ttl.c_str(), &endp, 10);
        if (key.empty() || ttl.empty() || *endp != '\0' || value.empty())
            return "malformed line " + std::to_string(line_no);

        if (expire_at != -1) {
            long long remaining = expire_at - now_ms;
            if (remaining <= 0) { skipped++; continue; }      // died while the server was down
            cache->put(key, value);
            auto when = Clock::now() + std::chrono::milliseconds(remaining);
            expires[key] = when;
            expiry_heap.push({when, key});
        } else {
            cache->put(key, value);
            expires.erase(key);
        }
        loaded++;
    }
    if (!seen_end) return "missing END line (file truncated?)";
    return "";
}

// Background thread: every N seconds, save if anything changed since the last save.
void snapshot_loop(long interval_s) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_s));
        bool need;
        {
            std::lock_guard<std::mutex> guard(store_mutex);
            need = (dirty != dirty_saved);
        }
        if (need) {
            std::string err = save_snapshot();
            log_line(err.empty() ? "Periodic snapshot saved" : "Periodic snapshot FAILED: " + err);
        }
    }
}

// ---------- Thread pool ----------
class ThreadPool {
public:
    explicit ThreadPool(size_t n) {
        for (size_t i = 0; i < n; i++) {
            workers.emplace_back([this] { worker_loop(); });
        }
    }

    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(m);
            tasks.push(std::move(task));
        }
        cv.notify_one();
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [this] { return !tasks.empty(); });
                task = std::move(tasks.front());
                tasks.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex m;
    std::condition_variable cv;
};

// ---------- Networking helpers ----------
void send_reply(int fd, const std::string& msg) {
    std::string out = msg + "\n";
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = write(fd, out.data() + sent, out.size() - sent);
        if (n <= 0) return;
        sent += n;
    }
}

// ---------- Command handling ----------
std::string handle_command(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty()) return "ERR empty command";

    std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    if (cmd == "SET") {
        std::string key, value;
        iss >> key;
        std::getline(iss, value);
        size_t start = value.find_first_not_of(' ');
        if (key.empty() || start == std::string::npos)
            return "ERR wrong number of arguments for 'SET'";
        value = value.substr(start);
        std::lock_guard<std::mutex> guard(store_mutex);
        cache->put(key, value);
        expires.erase(key);          // like Redis: a fresh SET clears any old expiry
        dirty++;
        return "OK";
    }

    if (cmd == "GET") {
        std::string key, extra;
        iss >> key;
        if (key.empty() || (iss >> extra))
            return "ERR wrong number of arguments for 'GET'";
        std::lock_guard<std::mutex> guard(store_mutex);
        if (expire_if_needed(key)) return "(nil)";
        std::string value;
        if (!cache->get(key, value)) return "(nil)";
        return value;
    }

    if (cmd == "DEL") {
        std::string key, extra;
        iss >> key;
        if (key.empty() || (iss >> extra))
            return "ERR wrong number of arguments for 'DEL'";
        std::lock_guard<std::mutex> guard(store_mutex);
        expire_if_needed(key);
        bool existed = cache->erase(key);
        expires.erase(key);
        if (existed) dirty++;
        return existed ? "(integer) 1" : "(integer) 0";
    }

    if (cmd == "EXPIRE") {
        std::string key, secs_str, extra;
        iss >> key >> secs_str;
        if (key.empty() || secs_str.empty() || (iss >> extra))
            return "ERR wrong number of arguments for 'EXPIRE'";
        char* end = nullptr;
        long secs = std::strtol(secs_str.c_str(), &end, 10);
        if (*end != '\0' || secs <= 0 || secs > 31536000)
            return "ERR seconds must be an integer between 1 and 31536000";
        std::lock_guard<std::mutex> guard(store_mutex);
        if (expire_if_needed(key) || !cache->contains(key)) return "(integer) 0";
        auto when = Clock::now() + std::chrono::seconds(secs);
        expires[key] = when;
        expiry_heap.push({when, key});
        dirty++;
        return "(integer) 1";
    }

    if (cmd == "TTL") {
        std::string key, extra;
        iss >> key;
        if (key.empty() || (iss >> extra))
            return "ERR wrong number of arguments for 'TTL'";
        std::lock_guard<std::mutex> guard(store_mutex);
        if (expire_if_needed(key) || !cache->contains(key)) return "(integer) -2";
        auto it = expires.find(key);
        if (it == expires.end()) return "(integer) -1";
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           it->second - Clock::now()).count();
        if (ms < 0) ms = 0;
        return "(integer) " + std::to_string((ms + 999) / 1000);
    }

    if (cmd == "SAVE") {
        std::string err = save_snapshot();       // takes its own locks
        return err.empty() ? "OK" : "ERR snapshot failed: " + err;
    }

    if (cmd == "LASTSAVE") {
        std::lock_guard<std::mutex> guard(store_mutex);
        return "(integer) " + std::to_string(last_save_epoch_s);
    }

    if (cmd == "DBSIZE") {
        std::lock_guard<std::mutex> guard(store_mutex);
        return "(integer) " + std::to_string(cache->size());
    }

    if (cmd == "INFO") {
        std::lock_guard<std::mutex> guard(store_mutex);
        return "size=" + std::to_string(cache->size()) +
               " capacity=" + std::to_string(cache->capacity()) +
               " evictions=" + std::to_string(cache->evictions()) +
               " ttl_keys=" + std::to_string(expires.size()) +
               " expired_lazy=" + std::to_string(expired_lazy) +
               " expired_active=" + std::to_string(expired_active) +
               " saves=" + std::to_string(saves_done) +
               " unsaved_changes=" + std::to_string(dirty - dirty_saved);
    }

    return "ERR unknown command '" + cmd + "'";
}

// ---------- One client's whole conversation (runs on a worker thread) ----------
void handle_client(int client_fd) {
    log_line("Client connected (fd " + std::to_string(client_fd) + ")");

    std::string buffer;
    char chunk[4096];
    ssize_t n;

    while ((n = read(client_fd, chunk, sizeof(chunk))) > 0) {
        buffer.append(chunk, n);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            send_reply(client_fd, handle_command(line));
        }
    }

    log_line("Client disconnected (fd " + std::to_string(client_fd) + ")");
    close(client_fd);
}

int main(int argc, char* argv[]) {
    // Block SIGINT/SIGTERM in every thread. A dedicated thread (below) waits for them
    // and does a final save. This must happen BEFORE any other thread is created,
    // because new threads inherit the signal mask.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    // Usage: ./server [max_keys] [snapshot_interval_seconds]
    //        defaults: 1000 keys, snapshot every 30 s (0 = no periodic snapshots)
    size_t capacity = 1000;
    long snapshot_interval = 30;
    if (argc > 1) {
        capacity = std::strtoul(argv[1], nullptr, 10);
        if (capacity == 0) {
            std::cerr << "Usage: ./server [max_keys] [snapshot_interval_seconds]\n";
            return 1;
        }
    }
    if (argc > 2) {
        char* endp = nullptr;
        snapshot_interval = std::strtol(argv[2], &endp, 10);
        if (*endp != '\0' || snapshot_interval < 0) {
            std::cerr << "Usage: ./server [max_keys] [snapshot_interval_seconds]\n";
            return 1;
        }
    }
    cache = new LRUCache(capacity);

    // When the LRU evicts a key, forget its expiry too. This runs inside put(),
    // which already holds store_mutex, so it must NOT lock again.
    cache->set_evict_callback([](const std::string& k) { expires.erase(k); });

    // Load the snapshot (if any) before accepting clients. A damaged file stops the
    // server instead of silently starting empty and overwriting it at the next save.
    if (access(SNAPSHOT_PATH, F_OK) != 0) {
        log_line("No snapshot found, starting empty");
    } else {
        size_t loaded = 0, skipped = 0;
        std::string err = load_snapshot(loaded, skipped);
        if (!err.empty()) {
            std::cerr << "ERROR: cannot load " << SNAPSHOT_PATH << ": " << err << "\n"
                      << "Fix or delete the file, then start again.\n";
            return 1;
        }
        log_line("Loaded " + std::to_string(loaded) + " keys from " + SNAPSHOT_PATH +
                 " (" + std::to_string(skipped) + " expired while down and skipped, cache holds " +
                 std::to_string(cache->size()) + ")");
    }

    const size_t POOL_SIZE = 8;
    ThreadPool pool(POOL_SIZE);

#ifndef NO_SWEEPER
    std::thread(sweeper_loop).detach();
    log_line("Active expiry sweeper: ON");
#else
    log_line("Active expiry sweeper: OFF (lazy expiry only)");
#endif

    if (snapshot_interval > 0) {
        std::thread(snapshot_loop, snapshot_interval).detach();
        log_line("Periodic snapshots: every " + std::to_string(snapshot_interval) + " s (only when data changed)");
    } else {
        log_line("Periodic snapshots: OFF (use SAVE, or stop the server with Ctrl+C)");
    }

    // Shutdown thread: wait for Ctrl+C / kill, save, then exit
    std::thread([sigs]() {
        int sig = 0;
        sigwait(&sigs, &sig);
        log_line("Signal " + std::to_string(sig) + " received, saving snapshot before exit...");
        std::string err = save_snapshot();
        log_line(err.empty() ? "Snapshot saved. Bye." : "Snapshot FAILED: " + err);
        _exit(err.empty() ? 0 : 1);
    }).detach();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(6380);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, 128) < 0) { perror("listen"); return 1; }

    log_line("MiniRedis on port 6380, " + std::to_string(POOL_SIZE) +
             " workers, max " + std::to_string(capacity) + " keys");

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) { perror("accept"); continue; }
        pool.submit([client_fd] { handle_client(client_fd); });
    }
}