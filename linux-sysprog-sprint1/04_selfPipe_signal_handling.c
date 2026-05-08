/* code example of the self-pipe trick to avoid async-signal-safety issues
    - The self-pipe trick is a classic Unix pattern used to handle signals safely by converting them into standard file descriptor events.
      This allows us to process signals within our main event loop (poll, select, or epoll) rather than inside the restricted environment of a signal handler.
    - The Concept:
        1- Create a pipe before your main loop starts.
        2- Inside the signal handler, perform a single write() of one byte to the pipe's "write" end. Since write() is async-signal-safe, this is legal.
        3- In our main loop, monitor the pipe's "read" end along with our other sockets or files.
        4- When the loop wakes up because of the pipe, we know a signal occurred and can handle it safely using any function (like printf or malloc).
    - Why this is better than a simple flag?:
        A simple global flag (volatile sig_atomic_t) only works if our program is already running code.
        If our program is sleeping in a poll() or select() call, it might stay asleep until a network event happens, effectively "missing" the signal until much later.
        The self-pipe trick wakes up the loop immediately.
        Note: On modern Linux, we can often replace this entire pattern with a signalfd, which lets the kernel manage the "pipe" for us .. see 05_signalfd_example.c

*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>

int pipe_fds[2];

// ASYNC-SIGNAL-SAFE HANDLER
void signal_handler(int sig) {
    int saved_errno = errno;
    char dummy = 1;
    // write() is safe to call here.
    write(pipe_fds[1], &dummy, 1); 
    errno = saved_errno;
}

int main() {
    // 1. Create the pipe
    if (pipe(pipe_fds) == -1) { perror("pipe"); exit(1); }

    // 2. Set the write end to non-blocking to avoid deadlocks if pipe fills up
    fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);

    // 3. Register handler
    struct sigaction sa = { .sa_handler = signal_handler };
    sigaction(SIGINT, &sa, NULL);

    struct pollfd fds[1];
    fds[0].fd = pipe_fds[0];
    fds[0].events = POLLIN;

    printf("Waiting for SIGINT (Ctrl+C)...\n");

    while (1) {
        int ret = poll(fds, 1, -1);
        if (ret > 0 && (fds[0].revents & POLLIN)) {
            char buf;
            read(pipe_fds[0], &buf, 1); // Consume the signal byte
            
            // NOW IT IS SAFE to use unsafe functions like printf()
            printf("\n[Safe] Caught signal in main loop! Doing heavy work...\n");
        }
    }
    return 0;
}
