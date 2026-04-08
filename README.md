# CSCE 311 Project 2 — Local IPC and Resource Management

## Build

```
make
```

Produces `bin/proj2-server`.

## Usage

```
./bin/proj2-server <socket_path> <num_readers> <num_solvers>
```

| Argument | Description |
|---|---|
| `socket_path` | Path for the server's Unix domain datagram socket (e.g. `/tmp/proj2.sock`) |
| `num_readers` | Number of file-reader slots in the `FileReaders` pool |
| `num_solvers` | Number of SHA-solver slots in the `ShaSolvers` pool |

Send `SIGINT` or `SIGTERM` to shut down cleanly.

## Design

### Protocol

The server binds a **SOCK_DGRAM** Unix domain socket and loops on `RecvFrom`. For each incoming datagram the server spawns a detached thread that:

1. Parses the length-prefixed binary framing:
   - `uint32_t` reply endpoint path length + path
   - `uint32_t` file count
   - For each file: `uint32_t` path length + path + `uint32_t` row count
2. Acquires resources, processes files, releases resources, and streams results back.

### Deadlock Prevention

All threads acquire resources in the same fixed order — **solvers before readers** — eliminating circular waits. The number of solvers checked out is `max(row_count_i)` across all files in the request, which is the maximum parallelism any single file can exploit.

### Concurrency

- One detached thread per client request.
- `FileReaders::Checkout` and `ShaSolvers::Checkout` block internally until resources are available (FIFO).
- The main loop uses `SO_RCVTIMEO` to periodically check the `g_stop` flag set by signal handlers.

### Signal Handling

`SIGINT` and `SIGTERM` set a `volatile sig_atomic_t g_stop` flag. The signal handler performs no other operations (async-signal-safe). The main loop exits when the flag is set.
