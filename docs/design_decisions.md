# Design Decisions, Trade-Offs & Data Structures — ThreadForge

## 1. Priority Queue vs. Ring Buffer / FIFO Queue

### Context
ThreadForge requires scheduled task prioritization (e.g. Critical, High, Normal, Low) while guaranteeing that tasks within the same priority class maintain deterministic First-In-First-Out (FIFO) ordering.

### Alternatives Considered
1. **Multiple FIFO Queues (Multi-Level Feedback / Priority Buckets)**:
   - *Design*: 4 distinct `std::queue<Task>` for each enum value.
   - *Pros*: O(1) push and pop within each bucket.
   - *Cons*: Rigidly fixed priority levels; harder to extend to arbitrary integer priorities; introduces starvation if high-priority queues are continuously filled unless complex aging algorithms are added.
2. **`std::priority_queue` with Monotonic Sequence Counter (Selected)**:
   - *Design*: Standard max-heap with custom `TaskComparator`.
   - *Pros*: Supports arbitrary priorities and guarantees strict weak ordering; simple, mathematically verifiable FIFO tie-breaking using atomic sequence numbers.
   - *Trade-off*: Insertion and extraction cost is O(log n) vs O(1). Under micro-benchmarking with microscopic tasks (~3 µs), queue lock contention and tree rebalancing introduce overhead, but for realistic systems tasks (> 100 µs), safety and deterministic ordering far outweigh log-n cost.

---

## 2. Priority Comparator Semantics Walkthrough

`std::priority_queue<T, Container, Compare>` operates as a **max-heap** where the top element is the one for which `Compare(top, other)` is `false`.

```cpp
struct TaskComparator {
    bool operator()(const Task& lhs, const Task& rhs) const noexcept {
        if (lhs.priority() != rhs.priority()) {
            return lhs.priority() < rhs.priority();
        }
        return lhs.sequence_number() > rhs.sequence_number();
    }
};
```

### Truth Table:
| Scenario | Condition | Comparator Output | Meaning |
| :--- | :--- | :--- | :--- |
| **Different Priorities** | `lhs.prio < rhs.prio` | `true` | `rhs` has higher priority; `rhs` comes first |
| **Different Priorities** | `lhs.prio > rhs.prio` | `false` | `lhs` has higher priority; `lhs` comes first |
| **Equal Priorities** | `lhs.seq > rhs.seq` | `true` | `lhs` submitted later; `rhs` was earlier (FIFO), so `rhs` comes first |
| **Equal Priorities** | `lhs.seq < rhs.seq` | `false` | `lhs` was submitted earlier (FIFO), so `lhs` comes first |

---

## 3. Explicit Cancellation vs. Conflating with Failure

### Design Question:
When the scheduler undergoes immediate shutdown (`shutdown(drain = false)`), what happens to unstarted tasks?

### Alternatives Considered:
1. **Treat Cancellation as `Failed`**:
   - Marking unstarted tasks as `Failed` without setting an exception leaves `get_task_exception()` returning `nullptr` despite `status == Failed`.
   - Leaves futures vulnerable to `std::future_error (broken_promise)`.
   - Corrupts failure metrics by conflating runtime task exceptions with unstarted cancellations.
2. **Dedicated `TaskStatus::Cancelled` & `TaskCancelledException` (Selected)**:
   - Clearly separates cancellation from runtime exceptions.
   - Provides explicit `TaskCancelledException` via `future.get()`.
   - Updates invariant to:
     $$\text{accepted} = \text{pending} + \text{running} + \text{completed} + \text{failed} + \text{cancelled}$$
   - Allows clean auditing of cancellations vs execution failures.

---

## 4. History Retention and Memory Management

### Trade-Off
Retaining task records allows callers to query historical statuses and timings long after completion, but creates an unbounded memory leak if millions of tasks are submitted over time.

### Selected Policy:
1. **Bounded Size with Auto-Pruning**: A configurable `max_history_records` parameter (default 10,000). If internal records exceed `2 * max_history_records`, an internal prune is triggered.
2. **Active Task Inviolability**: Pruning **only removes terminal tasks** (`Completed`, `Failed`, or `Cancelled`). `Pending` or `Running` tasks are never removed.
3. **Explicit Cleanup APIs**:
   - `scheduler.clear_history()`: Safely wipes all terminal task records.
   - `scheduler.prune_history(retain_count)`: Sorts terminal tasks by submission sequence and trims the oldest entries.
