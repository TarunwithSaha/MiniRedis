#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "lru.h"

// ---------- The database and its lock ----------
LRUCache* cache = nullptr;     // created in main() once we know the capacity
std::mutex store_mutex;
// Note: even GET changes the cache (it reorders the list), so every command
// needs the full lock, not just writes.

// ---------- Thread-safe printing ----------
std::mutex log_mutex;
void log_line(const std::string& msg) {
    std::lock_guard<std::mutex> g(log_mutex);
    std::cout << msg << std::endl;
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
        return "OK";
    }

    if (cmd == "GET") {
        std::string key, extra;
        iss >> key;
        if (key.empty() || (iss >> extra))
            return "ERR wrong number of arguments for 'GET'";
        std::lock_guard<std::mutex> guard(store_mutex);
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
        return cache->erase(key) ? "(integer) 1" : "(integer) 0";
    }

    if (cmd == "DBSIZE") {
        std::lock_guard<std::mutex> guard(store_mutex);
        return "(integer) " + std::to_string(cache->size());
    }

    if (cmd == "INFO") {
        std::lock_guard<std::mutex> guard(store_mutex);
        return "size=" + std::to_string(cache->size()) +
               " capacity=" + std::to_string(cache->capacity()) +
               " evictions=" + std::to_string(cache->evictions());
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
    // Usage: ./server [max_keys]   (default 1000)
    size_t capacity = 1000;
    if (argc > 1) {
        capacity = std::strtoul(argv[1], nullptr, 10);
        if (capacity == 0) {
            std::cerr << "Usage: ./server [max_keys]  (max_keys must be at least 1)\n";
            return 1;
        }
    }
    cache = new LRUCache(capacity);

    const size_t POOL_SIZE = 8;
    ThreadPool pool(POOL_SIZE);

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