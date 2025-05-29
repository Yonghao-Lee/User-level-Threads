#include "uthreads.h"
#include <signal.h>
#include <sys/time.h>
#include <setjmp.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <list>
#include <algorithm>
#include <string.h>

// Thread states
enum State { RUNNING, READY, BLOCKED };

// Thread Control Block
struct Thread {
    int tid;                  // Thread ID
    State state;              // Current state
    sigjmp_buf env;           // Thread context
    char stack[STACK_SIZE];   // Thread stack (statically allocated)
    int quantums;             // Number of quantums run
    int sleep_quantums;       // Quantums to sleep
    bool sleeping;            // Is sleeping
    bool exists;              // Thread exists
    bool manually_blocked;    // Blocked via uthread_block (not sleep)
};

// Architecture-specific definitions
#ifdef __x86_64__
/* code for 64 bit Intel arch */
typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

address_t translate_address(address_t addr) {
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}
#else
/* code for 32 bit Intel arch */
typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5

address_t translate_address(address_t addr) {
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}
#endif

// Global variables
static Thread threads[MAX_THREAD_NUM];
static bool thread_exists[MAX_THREAD_NUM] = {false};
static std::list<int> ready_list;
static int running_thread = -1;
static int quantum_usecs;
static int total_quantums = 0;
static struct sigaction sa;
static struct itimerval timer;
static sigset_t mask;
static bool initialized = false;

// Forward declarations
static void scheduler(bool from_timer = false);
static void setup_timer();

// Block signals to prevent interruptions during critical sections
static void block_signals() {
    sigprocmask(SIG_BLOCK, &mask, nullptr);
}

// Unblock signals to allow normal execution
static void unblock_signals() {
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);
}

// Timer handler for quantum expiration
static void timer_handler(int sig) {
    block_signals();

    // Increment total quantum counter
    total_quantums++;

    // Handle sleeping threads
    for (int i = 0; i < MAX_THREAD_NUM; i++) {
        if (thread_exists[i] && threads[i].sleeping) {
            threads[i].sleep_quantums--;

            // If sleep time is over, wake up thread
            if (threads[i].sleep_quantums <= 0) {
                threads[i].sleeping = false;

                // Move to READY state if not blocked for other reasons
                if (threads[i].state == BLOCKED && !threads[i].manually_blocked) {
                    threads[i].state = READY;
                    ready_list.push_back(i);
                }
            }
        }
    }

    // Only preempt if there are other threads ready to run
    if (!ready_list.empty()) {
        if (running_thread != -1 && thread_exists[running_thread] && 
            threads[running_thread].state == RUNNING) {
            threads[running_thread].state = READY;
            ready_list.push_back(running_thread);
        }
        scheduler(true);
    } else {
        // Just increment the running thread's quantum count
        if (running_thread != -1 && thread_exists[running_thread]) {
            threads[running_thread].quantums++;
        }
    }

    unblock_signals();
}

// Reset and start the timer
static void setup_timer() {
    // Configure the timer
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = quantum_usecs;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = quantum_usecs;  // Set interval for repeated firing

    // Start the timer
    if (setitimer(ITIMER_VIRTUAL, &timer, nullptr) < 0) {
        fprintf(stderr, "system error: setitimer failed\n");
        exit(1);
    }
}

// Get next available thread ID
static int get_available_tid() {
    for (int i = 0; i < MAX_THREAD_NUM; i++) {
        if (!thread_exists[i]) {
            return i;
        }
    }
    return -1;
}

// Initialize a new thread
static int setup_thread(int tid, thread_entry_point entry_point) {
    // Initialize thread data
    threads[tid].tid = tid;
    threads[tid].state = READY;
    threads[tid].quantums = 0;
    threads[tid].sleep_quantums = 0;
    threads[tid].sleeping = false;
    threads[tid].exists = true;
    threads[tid].manually_blocked = false;

    // Save initial context
    sigsetjmp(threads[tid].env, 1);

    // Set up stack for non-main threads
    if (tid != 0) {
        // Calculate stack pointer and program counter
        address_t sp = (address_t)&threads[tid].stack + STACK_SIZE - sizeof(address_t);
        address_t pc = (address_t)entry_point;

        // Setup thread context with translated addresses
        (threads[tid].env->__jmpbuf)[JB_SP] = translate_address(sp);
        (threads[tid].env->__jmpbuf)[JB_PC] = translate_address(pc);

        // Clean signal mask
        sigemptyset(&threads[tid].env->__saved_mask);
    }

    thread_exists[tid] = true;
    return tid;
}

// Round-Robin scheduler
static void scheduler(bool from_timer) {
    // No threads to run
    if (ready_list.empty()) {
        if (running_thread == -1 || !thread_exists[running_thread]) {
            fprintf(stderr, "system error: no threads to run\n");
            exit(1);
        }
        // Keep current thread running
        return;
    }

    // Get next thread to run
    int next_thread = ready_list.front();
    ready_list.pop_front();

    // Ensure this thread still exists
    if (!thread_exists[next_thread]) {
        scheduler(from_timer);
        return;
    }

    // Save old running thread and update to new one
    int prev_thread = running_thread;
    running_thread = next_thread;

    // Update state and counters
    threads[next_thread].state = RUNNING;
    threads[next_thread].quantums++; // Increment quantum count for new running thread

    // If there was no previous running thread, just jump to the new one
    if (prev_thread == -1) {
        siglongjmp(threads[running_thread].env, 1);
        return;
    }

    // Context switch
    if (sigsetjmp(threads[prev_thread].env, 1) == 0) {
        siglongjmp(threads[running_thread].env, 1);
    }
}

/*
 * External interface implementation
 */

int uthread_init(int quantum_usecs_param) {
    if (quantum_usecs_param <= 0) {
        fprintf(stderr, "thread library error: non-positive quantum_usecs\n");
        return -1;
    }

    if (initialized) {
        fprintf(stderr, "thread library error: already initialized\n");
        return -1;
    }

    // Initialize library
    initialized = true;
    quantum_usecs = quantum_usecs_param;
    total_quantums = 1;

    // Initialize thread arrays
    memset(thread_exists, 0, sizeof(thread_exists));

    // Setup signal set
    sigemptyset(&mask);
    sigaddset(&mask, SIGVTALRM);

    // Setup signal handler
    sa.sa_handler = &timer_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGVTALRM, &sa, nullptr) < 0) {
        fprintf(stderr, "system error: sigaction failed\n");
        exit(1);
    }

    // Initialize main thread (tid 0)
    setup_thread(0, nullptr);
    threads[0].state = RUNNING;
    threads[0].quantums = 1;  // Main thread starts with 1 quantum
    running_thread = 0;

    // Save context for main thread
    sigsetjmp(threads[0].env, 1);

    // Start timer
    setup_timer();

    return 0;
}

int uthread_spawn(thread_entry_point entry_point) {
    if (!initialized) {
        fprintf(stderr, "thread library error: library not initialized\n");
        return -1;
    }

    if (entry_point == nullptr) {
        fprintf(stderr, "thread library error: null entry_point\n");
        return -1;
    }

    block_signals();

    // Find available thread ID
    int tid = get_available_tid();
    if (tid == -1) {
        unblock_signals();
        fprintf(stderr, "thread library error: no available thread slots\n");
        return -1;
    }

    // Setup new thread
    setup_thread(tid, entry_point);

    // Add to ready list
    ready_list.push_back(tid);

    unblock_signals();
    return tid;
}

int uthread_terminate(int tid) {
    if (!initialized) {
        fprintf(stderr, "thread library error: library not initialized\n");
        return -1;
    }

    if (tid < 0 || tid >= MAX_THREAD_NUM || !thread_exists[tid]) {
        fprintf(stderr, "thread library error: thread does not exist\n");
        return -1;
    }

    block_signals();

    // Handle main thread termination
    if (tid == 0) {
        exit(0);
    }

    // Remove from ready list if present
    ready_list.remove(tid);

    // Mark as non-existent
    thread_exists[tid] = false;

    // If terminating self, schedule next thread
    if (tid == running_thread) {
        running_thread = -1;
        scheduler();
    }

    unblock_signals();
    return 0;
}

int uthread_block(int tid) {
    if (!initialized) {
        fprintf(stderr, "thread library error: library not initialized\n");
        return -1;
    }

    if (tid < 0 || tid >= MAX_THREAD_NUM || !thread_exists[tid]) {
        fprintf(stderr, "thread library error: thread does not exist\n");
        return -1;
    }

    if (tid == 0) {
        fprintf(stderr, "thread library error: cannot block main thread\n");
        return -1;
    }

    block_signals();

    // Mark as manually blocked (not just sleeping)
    threads[tid].manually_blocked = true;

    // If already blocked, do nothing
    if (threads[tid].state == BLOCKED) {
        unblock_signals();
        return 0;
    }

    // Remove from ready list if ready
    if (threads[tid].state == READY) {
        ready_list.remove(tid);
    }

    // Mark as blocked
    threads[tid].state = BLOCKED;

    // If blocking self, schedule next thread
    if (tid == running_thread) {
        running_thread = -1;
        scheduler();
    }

    unblock_signals();
    return 0;
}

int uthread_resume(int tid) {
    if (!initialized) {
        fprintf(stderr, "thread library error: library not initialized\n");
        return -1;
    }

    if (tid < 0 || tid >= MAX_THREAD_NUM || !thread_exists[tid]) {
        fprintf(stderr, "thread library error: thread does not exist\n");
        return -1;
    }

    block_signals();

    // Clear manual block flag
    threads[tid].manually_blocked = false;

    // Only resume if thread is blocked and not sleeping
    if (threads[tid].state == BLOCKED && !threads[tid].sleeping) {
        threads[tid].state = READY;
        ready_list.push_back(tid);
    }

    unblock_signals();
    return 0;
}

int uthread_sleep(int num_quantums) {
    if (!initialized) {
        fprintf(stderr, "thread library error: library not initialized\n");
        return -1;
    }

    if (num_quantums <= 0) {
        fprintf(stderr, "thread library error: non-positive sleep quantums\n");
        return -1;
    }

    if (running_thread == 0) {
        fprintf(stderr, "thread library error: main thread cannot sleep\n");
        return -1;
    }

    block_signals();

    // Set sleep parameters
    threads[running_thread].sleeping = true;
    threads[running_thread].sleep_quantums = num_quantums;

    // Block thread
    threads[running_thread].state = BLOCKED;
    int prev_thread = running_thread;
    running_thread = -1;

    // Context switch
    if (sigsetjmp(threads[prev_thread].env, 1) == 0) {
        scheduler();
    }

    unblock_signals();
    return 0;
}

int uthread_get_tid() {
    return running_thread;
}

int uthread_get_total_quantums() {
    return total_quantums;
}

int uthread_get_quantums(int tid) {
    if (!initialized) {
        fprintf(stderr, "thread library error: library not initialized\n");
        return -1;
    }

    if (tid < 0 || tid >= MAX_THREAD_NUM || !thread_exists[tid]) {
        fprintf(stderr, "thread library error: thread does not exist\n");
        return -1;
    }

    return threads[tid].quantums;
}