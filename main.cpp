#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

// The database: key -> value, all in memory
std::unordered_map<std::string, std::string> store;

// write() may send fewer bytes than asked, so keep going until all are sent
void send_reply(int fd, const std::string& msg) {
    std::string out = msg + "\n";
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = write(fd, out.data() + sent, out.size() - sent);
        if (n <= 0) return;
        sent += n;
    }
}

// Takes one command line, returns the reply text
std::string handle_command(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty()) return "ERR empty command";

    // Make the command case-insensitive: "set" == "SET"
    std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    if (cmd == "SET") {
        std::string key, value;
        iss >> key;
        std::getline(iss, value);                    // the rest of the line is the value
        size_t start = value.find_first_not_of(' ');
        if (key.empty() || start == std::string::npos)
            return "ERR wrong number of arguments for 'SET'";
        value = value.substr(start);                 // drop the leading space
        store[key] = value;
        return "OK";
    }

    if (cmd == "GET") {
        std::string key, extra;
        iss >> key;
        if (key.empty() || (iss >> extra))
            return "ERR wrong number of arguments for 'GET'";
        auto it = store.find(key);
        if (it == store.end()) return "(nil)";
        return it->second;
    }

    if (cmd == "DEL") {
        std::string key, extra;
        iss >> key;
        if (key.empty() || (iss >> extra))
            return "ERR wrong number of arguments for 'DEL'";
        return store.erase(key) ? "(integer) 1" : "(integer) 0";
    }

    return "ERR unknown command '" + cmd + "'";
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(6380);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, 10) < 0) { perror("listen"); return 1; }
    std::cout << "MiniRedis listening on port 6380\n";

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) { perror("accept"); continue; }
        std::cout << "Client connected (fd " << client_fd << ")\n";

        std::string buffer;   // bytes received but not yet processed
        char chunk[1024];
        ssize_t n;

        while ((n = read(client_fd, chunk, sizeof(chunk))) > 0) {
            buffer.append(chunk, n);
            std::cout << "read " << n << " bytes, buffer now holds " << buffer.size() << "\n";

            // Process every complete line (ending in \n) in the buffer
            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();

                std::cout << "Command: [" << line << "]\n";
                send_reply(client_fd, handle_command(line));
            }
            // Anything left in buffer is an incomplete command: wait for more bytes
        }

        std::cout << "Client disconnected\n";
        close(client_fd);
    }
}