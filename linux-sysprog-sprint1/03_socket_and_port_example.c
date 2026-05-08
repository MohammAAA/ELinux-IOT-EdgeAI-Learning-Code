/*
- This example demonstrates the (socket -> bind -> listen) sequence
  and then uses poll() to monitor both that socket and a timerfd simultaneously.
- Key Highlights of the Code:
    Binding: The struct sockaddr_in uses a Designated Initializer (the "." syntax) to set the port to 8080 and bind to all interfaces (INADDR_ANY).
    The "Everything is a File" approach:
     Notice that server_fd (network) and timer_fd (kernel timer) are treated exactly the same way in the pollfd array.
    Efficiency: The poll() call puts the process to sleep. The CPU does zero work until either a person connects to the socket or the 2-second timer finishes.
    Reading the Timer: When the timer triggers, we must read() from it. This clears the "ready" state so that poll() doesn't immediately trigger again.
    
- How to test this:
    Compile and Run: gcc server.c -o server && ./server
    Watch the Timer: You will see "[Event] Timer expired!" every 2 seconds.
    Trigger the Socket: Open another terminal and run curl localhost:8080.
    You will see the socket event trigger immediately.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/timerfd.h>
#include <poll.h>
#include <stdint.h>

int main() {
    // --- 1. SOCKET -> BIND -> LISTEN ---
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8080),
        .sin_addr.s_addr = INADDR_ANY
    };

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);
    printf("Server listening on port 8080...\n");

    // --- 2. TIMERFD SETUP ---
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec timspec = {
        .it_interval = {5, 0}, // Repeat every 5 seconds
        .it_value = {3, 0}     // Start in 3 seconds
    };
    timerfd_settime(timer_fd, 0, &timspec, NULL);

    // --- 3. POLL() LOOP ---
    struct pollfd fds[2];
    fds[0].fd = server_fd;
    fds[0].events = POLLIN;  // Monitor for new connections
    fds[1].fd = timer_fd;
    fds[1].events = POLLIN;  // Monitor for timer ticks

    while (1) {
        printf("Waiting for events...\n");
        poll(fds, 2, -1); // Block until something happens

        if (fds[0].revents & POLLIN) {
            printf("[Event] New connection request on socket!\n");
            int client_fd = accept(server_fd, NULL, NULL);
            close(client_fd); // Just close for this demo
        }

        if (fds[1].revents & POLLIN) {
            uint64_t ticks;
            read(timer_fd, &ticks, sizeof(ticks)); // "Consume" the timer event
            printf("[Event] Timer expired! (Ticks: %llu)\n", (unsigned long long)ticks);
        }
    }

    return 0;
}
