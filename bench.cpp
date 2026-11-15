// bench.cpp - load generator for MiniRedis
// Usage: ./bench <host> <port> <clients> <ops_per_client> [set_percent=50] [keys_per_client=1000]
//
// Each client keeps ONE request in flight: send a command, wait for the reply,
// record how long that took, repeat. All clients start together (barrier).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct ClientResult {
    std::vector<double> lat_us;      // one entry per completed request
    size_t errors = 0;               // "ERR" replies or broken connections
    bool connect_failed = false;
};

std::atomic<int> ready_count{0};
std::atomic<bool> go{false};

bool send_all(int fd, const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = write(fd, s.data() + sent, s.size() - sent);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Reads one '\n'-terminated reply. 'buf' keeps leftover bytes between calls.
bool read_line(int fd, std::string& buf, std::string& line) {
    while (true) {
        size_t pos = buf.find('\n');
        if (pos != std::string::npos) {
            line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            return true;
        }
        char chunk[4096];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) return false;
        buf.append(chunk, static_cast<size_t>(n));
    }
}

void client_thread(const std::string& host, int port, int id, size_t ops,
                   int set_pct, size_t keys, ClientResult& r) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    bool ok = fd >= 0 &&
              inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1 &&
              connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (!ok) {
        r.connect_failed = true;
        if (fd >= 0) close(fd);
        ready_count++;
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    r.lat_us.reserve(ops);
    std::mt19937 rng(12345 + id);
    const std::string value(32, 'x');
    std::string buf, reply;

    ready_count++;
    while (!go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    for (size_t i = 0; i < ops; i++) {
        std::string key = "k" + std::to_string(id) + "_" + std::to_string(rng() % keys);
        std::string cmd = (static_cast<int>(rng() % 100) < set_pct)
                              ? "SET " + key + " " + value + "\n"
                              : "GET " + key + "\n";

        auto t0 = Clock::now();
        if (!send_all(fd, cmd) || !read_line(fd, buf, reply)) {
            r.errors++;
            break;
        }
        auto t1 = Clock::now();

        r.lat_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        if (reply.compare(0, 3, "ERR") == 0) r.errors++;
    }
    close(fd);
}

int main(int argc, char* argv[]) {
    auto usage = [&]() {
        std::fprintf(stderr,
                     "Usage: %s <host> <port> <clients> <ops_per_client> [set_percent=50] [keys_per_client=1000]\n",
                     argv[0]);
        return 1;
    };
    if (argc < 5) return usage();

    std::string host = argv[1];
    int port = std::atoi(argv[2]);
    int clients = std::atoi(argv[3]);
    size_t ops = std::strtoul(argv[4], nullptr, 10);
    int set_pct = argc > 5 ? std::atoi(argv[5]) : 50;
    size_t keys = argc > 6 ? std::strtoul(argv[6], nullptr, 10) : 1000;
    if (port <= 0 || port > 65535 || clients <= 0 || ops == 0 ||
        set_pct < 0 || set_pct > 100 || keys == 0)
        return usage();

    std::vector<ClientResult> results(clients);
    std::vector<std::thread> threads;
    for (int i = 0; i < clients; i++) {
        threads.emplace_back(client_thread, std::cref(host), port, i, ops, set_pct, keys,
                             std::ref(results[i]));
    }

    // Barrier: wait until every client is connected, then release them together
    while (ready_count.load() < clients) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto start = Clock::now();
    go.store(true);
    for (auto& t : threads) t.join();
    auto end = Clock::now();

    int failed = 0;
    size_t errors = 0;
    std::vector<double> all;
    for (auto& r : results) {
        if (r.connect_failed) failed++;
        errors += r.errors;
        all.insert(all.end(), r.lat_us.begin(), r.lat_us.end());
    }
    if (failed > 0) {
        std::fprintf(stderr, "ERROR: %d of %d clients could not connect to %s:%d (is the server running?)\n",
                     failed, clients, host.c_str(), port);
        return 1;
    }
    if (all.empty()) {
        std::fprintf(stderr, "ERROR: no requests completed\n");
        return 1;
    }

    double secs = std::chrono::duration<double>(end - start).count();
    std::sort(all.begin(), all.end());
    auto pct = [&](double p) {
        size_t idx = static_cast<size_t>(p / 100.0 * all.size());
        return all[std::min(idx, all.size() - 1)];
    };
    double sum = 0;
    for (double v : all) sum += v;

    std::printf("clients=%d  ops/client=%zu  set=%d%%  keys/client=%zu\n", clients, ops, set_pct, keys);
    std::printf("completed %zu requests in %.2f s\n", all.size(), secs);
    std::printf("throughput: %.0f ops/s\n", all.size() / secs);
    std::printf("latency us: avg=%.0f  p50=%.0f  p95=%.0f  p99=%.0f  max=%.0f\n",
                sum / all.size(), pct(50), pct(95), pct(99), all.back());
    std::printf("errors: %zu\n", errors);
    return errors == 0 ? 0 : 2;
}