/* This code explains how "signalfd" simplifies the previous code example by removing the need for a custom handler entirely
    - While the self-pipe trick is the portable "old-school" way to handle signals, signalfd is the modern Linux-specific way that is much cleaner.
      With signalfd, we don't need a signal handler function at all. Instead, we tell the kernel to redirect signals into a file descriptor that we can simply read() from or monitor with poll().

    - Why signalfd is simpler?:
        1- No Handlers: We don't have to write a separate void handler(int sig) function.
        2- No Async-Signal-Safety Issues: Since the signal is "delivered" during a normal read() call in our main() function, we can use any function we want (like printf, malloc, or complex logic) without risk of deadlocks.
        3- Detailed Info: The signalfd_siginfo struct gives us more data than a standard handler, such as the PID and UID of the process that sent the signal.

    - Note: We must block the signals using sigprocmask before creating the signalfd.
       If we don't, the kernel will still try to run the default handler (which usually kills our program) before the signal ever reaches our file descriptor.
*/

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <poll.h>

int main() {
    sigset_t mask;
    int sfd;
    struct signalfd_siginfo fdsi;

    // 1. Block the signals we want to handle via signalfd
    // This prevents the default "kill" behavior from happening.
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    // 2. Create the signal file descriptor
    sfd = signalfd(-1, &mask, 0);

    struct pollfd fds[1];
    fds[0].fd = sfd;
    fds[0].events = POLLIN;

    printf("Waiting for SIGINT (Ctrl+C). Try it!\n");

    while (1) {
        poll(fds, 1, -1);

        if (fds[0].revents & POLLIN) {
            // 3. "Read" the signal like it's data from a file
            read(sfd, &fdsi, sizeof(struct signalfd_siginfo));

            if (fdsi.ssi_signo == SIGINT) {
                printf("\n[signalfd] Caught SIGINT synchronously! Safe to use printf.\n");
            }
        }
    }
    return 0;
}
