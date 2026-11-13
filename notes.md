
Day1

1. Why accept() returns a new file descriptor

server_fd is the listening socket. Its only job is to wait for new connections, and it stays open that way for the whole life of the server. When a client connects, accept() creates a separate socket just for that conversation and returns its file descriptor (client_fd). read() and write() use client_fd to talk to that one client, while server_fd stays free to accept others. If accept() reused server_fd, the listening socket would be tied up with one client and the server could never accept another. In my run, server_fd was 3 and client_fd was 4, which shows they’re different objects.

2. Why the second client hangs

The server is single-threaded and is stuck on the read() line inside the inner while loop. As long as the first client stays connected, read() blocks waiting for that client’s next message, so the program never gets back to the accept() line at the top of the outer loop. The second client’s connection does get queued by the operating system (that’s what the listen(server_fd, 10) backlog is for), but nobody accepts it or echoes anything. The moment the first client disconnects, read() returns 0, the inner loop ends, the server calls accept(), and the second client is served. The fix, which you’ll build on Day 3, is to handle each client in its own thread so the main loop can keep calling accept().

3. What SO_REUSEADDR does

When a server closes a connection, the OS keeps that port reserved for a short while (the TIME_WAIT state) to make sure stray packets from the old connection don’t confuse a new one. Without SO_REUSEADDR, restarting the server right after stopping it can fail with bind: Address already in use, even though no program is using the port. With SO_REUSEADDR on, the OS lets bind() succeed during that waiting period, so you can restart the server immediately during development. In Step 3 of your experiment, the version without the option failed to restart while the normal one worked.

Day2

1. Why the server can’t assume one read() equals one command

TCP is a stream of bytes with no message boundaries. It guarantees the bytes arrive in order, but it doesn’t guarantee how they’re grouped into read() calls. My two experiments showed both failure cases:

Experiment 1: I sent SET a 1, GET a and DEL a in a single printf. The server did one read() that contained all three commands. If the code had treated that read as a single command, it would have processed SET a 1\nGET a\nDEL a\n as one broken line.
Experiment 2: I sent SET na, waited 3 seconds, then sent me Tarun\n. The first read() returned only half a command, and the buffer held 6 bytes. Without a buffer, the server would have tried to run SET na and then me Tarun as two separate bad commands.

So the server appends every read() to a buffer, extracts only the complete lines (those ending in \n), and keeps any leftover bytes for the next read. The newline is the boundary I define myself, because TCP doesn’t provide one.

2. Why send_reply loops around write()

write() returns the number of bytes it actually wrote, and that can be fewer than the number I asked for. This is called a partial write. It can happen when the socket’s send buffer is full (for example, with a large value or a slow client) or when the call is interrupted. If I call write() once and ignore the return value, the client could receive a truncated reply with no error. The loop tracks how many bytes were sent and keeps calling write() with the remaining bytes until the whole message is out, or until write() reports an error (return value 0 or less), in which case it gives up. It’s the sending-side version of the buffering problem in question 1: the OS gives no guarantee that a single call moves everything.

3. Why SET uses getline instead of >> for the value

The >> operator stops at the first whitespace, so it reads only one word. For SET city New Delhi, iss >> value would store just New and silently lose Delhi. std::getline(iss, value) reads the entire rest of the line, so New Delhi is stored intact. After iss >> key, the stream is still sitting on the space before the value, so the code then skips leading spaces with find_first_not_of(' ') and takes the substring from there. The same check also catches SET onlykey, because with no value left on the line the code returns a “wrong number of arguments” error. In my test, GET city returned New Delhi, which confirms the whole value was kept.