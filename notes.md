1. Why accept() returns a new file descriptor

server_fd is the listening socket. Its only job is to wait for new connections, and it stays open that way for the whole life of the server. When a client connects, accept() creates a separate socket just for that conversation and returns its file descriptor (client_fd). read() and write() use client_fd to talk to that one client, while server_fd stays free to accept others. If accept() reused server_fd, the listening socket would be tied up with one client and the server could never accept another. In my run, server_fd was 3 and client_fd was 4, which shows they’re different objects.

2. Why the second client hangs

The server is single-threaded and is stuck on the read() line inside the inner while loop. As long as the first client stays connected, read() blocks waiting for that client’s next message, so the program never gets back to the accept() line at the top of the outer loop. The second client’s connection does get queued by the operating system (that’s what the listen(server_fd, 10) backlog is for), but nobody accepts it or echoes anything. The moment the first client disconnects, read() returns 0, the inner loop ends, the server calls accept(), and the second client is served. The fix, which you’ll build on Day 3, is to handle each client in its own thread so the main loop can keep calling accept().

3. What SO_REUSEADDR does

When a server closes a connection, the OS keeps that port reserved for a short while (the TIME_WAIT state) to make sure stray packets from the old connection don’t confuse a new one. Without SO_REUSEADDR, restarting the server right after stopping it can fail with bind: Address already in use, even though no program is using the port. With SO_REUSEADDR on, the OS lets bind() succeed during that waiting period, so you can restart the server immediately during development. In Step 3 of your experiment, the version without the option failed to restart while the normal one worked.