# Testing Strategy & Verification Guide — ThreadForge

## 1. Automated Test Architecture

ThreadForge uses GoogleTest integrated with CMake and CTest. Tests are organized into six distinct suites:

| Test Suite | File | Focus Areas |
| :--- | :--- | :--- |
| **`test_task_scheduler`** | `tests/test_task_scheduler.cpp` | Construction, rejection of zero workers, submission, status transitions, wait_for_task, unknown ID handling, statistics invariant. |
| **`test_priority_queue`** | `tests/test_priority_queue.cpp` | Deterministic priority ordering, FIFO tie-breaking, multi-level priorities, non-preemptive execution behavior. |
| **`test_concurrency`** | `tests/test_concurrency.cpp` | Multi-producer concurrent submissions, barrier-synchronized overlapping worker execution, concurrent status/stats inspection. |
| **`test_exception_handling`**| `tests/test_exception_handling.cpp` | Standard exception propagation, non-standard exception safety, worker resilience, failure accounting. |
| **`test_shutdown`** | `tests/test_shutdown.cpp` | Graceful drain, immediate cancellation of unstarted tasks, idempotency, concurrent shutdown, self-join deadlock rejection. |
| **`test_task_results`** | `tests/test_task_results.cpp` | Primitive, aggregate, and container typed results, single-shot get() contract enforcement, history pruning and clear. |

---

## 2. Deterministic Synchronization in Tests

To avoid flaky timing-dependent tests, ThreadForge avoids arbitrary sleeps for ordering assertions:

1. **Single Worker Queue Verification**: Tests verifying queue ordering set `num_workers = 1`. A blocker task holds the worker while test tasks are enqueued. Once all test tasks are queued, the blocker is released. This guarantees that task extraction order depends 100% on comparator priority and sequence numbers.
2. **`TestBarrier` Utility**: A lightweight C++17 reusable barrier using `std::mutex` and `std::condition_variable` forces N worker threads to simultaneously reach a rendezvous point, mathematically proving concurrent overlapping execution.
3. **Running-State Spin Gates**: Where tests verify non-preemption or non-draining shutdown, an atomic `running_task_started` boolean is set inside the task's first line of execution. The test thread waits on this atomic flag, guaranteeing the worker is actively running the task before asserting subsequent behavior.

---

## 3. Running Tests Locally

### Debug Build
```bash
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

### Release Build
```bash
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

### Running a Specific Test with GTest Filter
```bash
./build/debug/test_shutdown --gtest_filter=ShutdownTest.GracefulDrainExecutesAllQueuedTasks
```
