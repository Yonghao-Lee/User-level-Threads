# User-Level Thread Library

A lightweight user-level thread library implementing Round-Robin scheduling with preemptive multitasking.

## Features

- **Round-Robin Scheduling**: Fair time-slicing between threads
- **Preemptive Multitasking**: Automatic context switching using virtual timer signals
- **Thread Management**: Create, terminate, block, and resume threads
- **Sleep Functionality**: Threads can sleep for a specified number of quantums
- **Signal-Safe Implementation**: Proper signal masking to prevent race conditions

## Implementation Details

### Core Components

- **Thread Control Block (TCB)**: Maintains thread state, stack, context, and scheduling information
- **Virtual Timer**: Uses `SIGVTALRM` for quantum-based preemption
- **Context Switching**: Leverages `sigsetjmp`/`siglongjmp` for saving and restoring thread contexts
- **Signal Masking**: Prevents timer interrupts during critical sections

### Thread States

- **RUNNING**: Currently executing thread
- **READY**: Ready to run, waiting in the ready queue
- **BLOCKED**: Blocked by explicit block call or sleeping

### Architecture Support

- x86 (32-bit) and x86_64 (64-bit) architectures
- Stack pointer and program counter manipulation for thread initialization

## API Reference

- `uthread_init(quantum_usecs)`: Initialize the library with quantum duration
- `uthread_spawn(entry_point)`: Create a new thread
- `uthread_terminate(tid)`: Terminate a thread
- `uthread_block(tid)`: Block a thread
- `uthread_resume(tid)`: Resume a blocked thread
- `uthread_sleep(num_quantums)`: Put calling thread to sleep
- `uthread_get_tid()`: Get current thread ID
- `uthread_get_total_quantums()`: Get total quantum count
- `uthread_get_quantums(tid)`: Get quantum count for specific thread

## Building

```bash
make
```

This creates `libuthreads.a` static library.

## Usage Example

```cpp
#include "uthreads.h"

void worker_thread() {
    for (int i = 0; i < 10; i++) {
        // Do work...
        uthread_sleep(2);  // Sleep for 2 quantums
    }
    uthread_terminate(uthread_get_tid());
}

int main() {
    uthread_init(50000);  // 50ms quantum
    
    int tid1 = uthread_spawn(worker_thread);
    int tid2 = uthread_spawn(worker_thread);
    
    // Main thread continues...
    
    return 0;
}
```

## Technical Notes

- Main thread (tid 0) cannot be blocked or put to sleep
- Maximum of 100 concurrent threads
- Each thread gets 4KB stack space
- Quantum counting starts at 1 after initialization
