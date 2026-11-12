#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main() {
    // 1. Create a socket (an endpoint for network communication)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    std::cout << "server_fd = " << server_fd << "\n";

    // 2. Allow the port to be reused right after the server stops.
    //    Compile with -DNO_REUSE to turn this off for the Part C experiment.
#ifndef NO_REUSE
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    std::cout << "SO_REUSEADDR is ON\n";
#else
    std::cout << "SO_REUSEADDR is OFF\n";
#endif

    // 3. Attach the socket to port 6380
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(6380);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    // 4. Start listening for connections
    if (listen(server_fd, 10) < 0) { perror("listen"); return 1; }
    std::cout << "Listening on port 6380\n";

    // 5. Accept clients one at a time, forever
    while (true) {
        std::cout << "Waiting for a client...\n";
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) { perror("accept"); continue; }
        std::cout << "Client connected, client_fd = " << client_fd << "\n";

        // 6. Echo everything this client sends back to it
        char buf[1024];
        ssize_t n;
        while ((n = read(client_fd, buf, sizeof(buf))) > 0) {
            std::cout << "read() returned " << n << " bytes\n";
            write(client_fd, buf, n);
        }

        std::cout << "read() returned " << n << ", client disconnected\n";
        close(client_fd);
    }
}