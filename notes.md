Part1

1. Why accept() returns a new file descriptor

server_fd is the listening socket. Its only job is to wait for new connections, and it stays open that way for the whole life of the server. When a client connects, accept() creates a separate socket just for that conversation and returns its file descriptor (client_fd). read() and write() use client_fd to talk to that one client, while server_fd stays free to accept others. If accept() reused server_fd, the listening socket would be tied up with one client and the server could never accept another. In my run, server_fd was 3 and client_fd was 4, which shows they’re different objects.

2. Why the second client hangs

The server is single-threaded and is stuck on the read() line inside the inner while loop. As long as the first client stays connected, read() blocks waiting for that client’s next message, so the program never gets back to the accept() line at the top of the outer loop. The second client’s connection does get queued by the operating system (that’s what the listen(server_fd, 10) backlog is for), but nobody accepts it or echoes anything. The moment the first client disconnects, read() returns 0, the inner loop ends, the server calls accept(), and the second client is served. The fix, which you’ll build on Day 3, is to handle each client in its own thread so the main loop can keep calling accept().

3. What SO_REUSEADDR does

When a server closes a connection, the OS keeps that port reserved for a short while (the TIME_WAIT state) to make sure stray packets from the old connection don’t confuse a new one. Without SO_REUSEADDR, restarting the server right after stopping it can fail with bind: Address already in use, even though no program is using the port. With SO_REUSEADDR on, the OS lets bind() succeed during that waiting period, so you can restart the server immediately during development. In Step 3 of your experiment, the version without the option failed to restart while the normal one worked.


#############################################################################

Part2

1. Why the server can’t assume one read() equals one command

TCP is a stream of bytes with no message boundaries. It guarantees the bytes arrive in order, but it doesn’t guarantee how they’re grouped into read() calls. My two experiments showed both failure cases:

Experiment 1: I sent SET a 1, GET a and DEL a in a single printf. The server did one read() that contained all three commands. If the code had treated that read as a single command, it would have processed SET a 1\nGET a\nDEL a\n as one broken line.
Experiment 2: I sent SET na, waited 3 seconds, then sent me Tarun\n. The first read() returned only half a command, and the buffer held 6 bytes. Without a buffer, the server would have tried to run SET na and then me Tarun as two separate bad commands.

So the server appends every read() to a buffer, extracts only the complete lines (those ending in \n), and keeps any leftover bytes for the next read. The newline is the boundary I define myself, because TCP doesn’t provide one.

2. Why send_reply loops around write()

write() returns the number of bytes it actually wrote, and that can be fewer than the number I asked for. This is called a partial write. It can happen when the socket’s send buffer is full (for example, with a large value or a slow client) or when the call is interrupted. If I call write() once and ignore the return value, the client could receive a truncated reply with no error. The loop tracks how many bytes were sent and keeps calling write() with the remaining bytes until the whole message is out, or until write() reports an error (return value 0 or less), in which case it gives up. It’s the sending-side version of the buffering problem in question 1: the OS gives no guarantee that a single call moves everything.

3. Why SET uses getline instead of >> for the value

The >> operator stops at the first whitespace, so it reads only one word. For SET city New Delhi, iss >> value would store just New and silently lose Delhi. std::getline(iss, value) reads the entire rest of the line, so New Delhi is stored intact. After iss >> key, the stream is still sitting on the space before the value, so the code then skips leading spaces with find_first_not_of(' ') and takes the substring from there. The same check also catches SET onlykey, because with no value left on the line the code returns a “wrong number of arguments” error. In my test, GET city returned New Delhi, which confirms the whole value was kept.

#############################################################################

Part3:-

1. What happened without the lock, and why

“With the lock removed, I ran 8 clients at once, each sending 5,000 SET commands, and expected 40,000 keys. The server crashed with Segmentation fault (core dumped), so the final DBSIZE query got no reply and the script printed a blank. This happened on every run. The locked version returned exactly 40,000 every time.

The cause is a data race. std::unordered_map is not thread-safe, and eight worker threads were inserting into it at the same time. An insert can trigger a rehash, where the table allocates a bigger bucket array and moves every entry across. If another thread reads or writes the table during that rehash, it follows pointers into memory that was just freed or half-moved, which the OS stops with a segfault. A mutex makes each operation run one thread at a time, so no thread ever sees the table in a half-updated state.”

2. Why workers wait on a condition variable instead of looping on tasks.empty()

“Looping on tasks.empty() is busy waiting. Each idle worker would run the check millions of times per second, using 100% of a CPU core while doing nothing, and with 8 workers that wastes 8 cores and competes with the threads doing real work. A condition variable puts the thread to sleep, using no CPU, and the OS wakes it only when submit() calls notify_one() after adding a task. The wait also releases the mutex while sleeping and re-acquires it on wake-up, so there’s no race between checking the queue and going to sleep. The predicate (!tasks.empty()) is re-checked on wake-up to handle spurious wake-ups.”

3. What limits the pool, and the fix

“Each client connection occupies one worker thread for its entire lifetime, because handle_client blocks on read() until the client disconnects. So with 8 workers, at most 8 clients are served at once, and a ninth waits in the queue even if all 8 are idle. In my test with POOL_SIZE set to 2, the third client hung until one of the others disconnected. More threads help only partly, because each thread uses memory for its stack and the OS slows down with thousands of threads. The real fix is an event-driven design: use epoll so one thread (or a few) waits on many sockets at once and only does work on the ones with data ready. That’s how real Redis works. A smaller fix is to give each worker a short read timeout, but epoll is the proper answer.”



#############################################################################
Part 4:-

1. Why a doubly linked list, not a singly linked list or an array

An LRU cache has to move a key to the front on every use and remove the oldest key from the back, both in O(1). A doubly linked list can do this because each node knows both its neighbors. To unlink a node, I set prev->next = next and next->prev = prev, with no searching.

Singly linked list: a node only knows its next neighbor, so to unlink a node in the middle I’d have to walk from the head to find the node before it. That’s O(n), and it would happen on every GET.
Array or vector: moving an element to the front means shifting everything before it by one position, which is also O(n).

I also used two sentinel nodes (head_ and tail_) so that inserting and removing never needs special cases for an empty list or for the first and last element.

2. Why the hash map stores pointers to nodes, and what happens if it stores only keys

The map goes from a key straight to that key’s node in the list, so on a GET I can find the node in O(1) and call move_to_front on it immediately. The two structures work as a pair: the map gives fast lookup, and the list gives fast reordering.

If the map stored only keys (or only whether a key exists), I’d know the key is present but not where its node is, so I’d have to scan the list to find it. That makes every GET, SET update and DEL O(n). With a million keys, a cache that was meant to be fast would be scanning a million nodes per request. The pointer is what keeps all three operations O(1).

3. Why GET now needs the same exclusive lock as SET

In a plain hash map, a GET only reads, so concurrent readers are harmless. In an LRU cache, a GET also writes: it moves the node to the front, which rewrites four pointers (prev and next of the neighbors and of the node itself). If two threads ran GET at the same time, they could interleave those pointer updates and corrupt the list, leaving broken links, a node pointing at itself, or a freed node still reachable. That would cause crashes or lost entries, just like the Part 3 race condition.

So a read-only (shared) lock would be unsafe here, and every command takes the full mutex. The cost is that all commands run one at a time, which limits throughput. Real Redis sidesteps this by executing commands on a single thread, and a multithreaded design could split the keys across several independent caches (sharding), each with its own lock, so threads only block each other when they touch the same shard

Removing move_to_front from the update path made test_update_does_not_grow_and_refreshes fail, because updating a key didn’t refresh its recency, so the wrong key was evicted.


#############################################################################


Part5:-

1. Why both lazy and active expiry

Lazy expiry checks a key’s deadline when a command touches it, so a command never returns a dead key. But it only runs on access, so a key nobody reads again stays in memory. Active expiry (the sweeper every 100 ms) frees that memory on its own, but it runs periodically, so it can’t be the correctness guarantee.

Evidence: with ./server_nosweeper, I set a to expire in 3 seconds and waited 5. DBSIZE still returned 1 and INFO showed size=1 ttl_keys=1 expired_lazy=0 expired_active=0, so the dead key was still in memory. After GET a returned (nil), INFO showed size=0 ttl_keys=0 expired_lazy=1. With the sweeper on, an untouched key expired by itself (expired_active=1), and 1000 keys with the same deadline went from DBSIZE 1000 to 0 with expired_lazy=0 and ttl_keys=0.

2. Why a min-heap, and what stale entries are

Scanning every key’s expiry on every sweep is O(n), and almost all of it is wasted checking keys that aren’t due. A min-heap keeps the soonest deadline on top, so a sweep looks at the top in O(1) and pops only due entries in O(log n) each. The cost depends on how many keys expire, not how many exist.

A heap can’t cheaply delete or update an entry in the middle. When a key is deleted, overwritten or re-EXPIREd, its old heap entry stays behind as a stale entry. The expires map is the source of truth. When the sweeper pops an entry, it acts only if expires still holds the same deadline (it->second == e.when), and discards it otherwise. That’s lazy deletion. The cost is that stale entries use memory until their time passes.

3. Why the LRU needed an eviction callback

The LRU removes keys by itself inside put() when it goes over capacity, but the expiry records live in main.cpp, outside the cache. Without the callback, an evicted key’s record would stay in expires: wasted memory, an inflated ttl_keys, and a bug if the same key name is later set again, because it would inherit a stale deadline. This is the same kind of bug as Part H, where an old deadline killed a newly set value.

Evidence: with ./server 3, after SET a, EXPIRE a 100, then three more keys, INFO showed size=3 evictions=1 ttl_keys=0, and TTL a returned -2. The callback removed a’s expiry record with a itself. The callback runs inside put() while store_mutex is already held, so it must not lock again or the thread would deadlock on itself.


With expires.erase(key) commented out in SET, SET k v1, EXPIRE k 20, SET k v2 left TTL k at 6, and GET k returned (nil): the old deadline deleted the newly written value. With the line restored, TTL k returned -1 and GET k returned v2.

#############################################################################