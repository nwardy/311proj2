// proj2_server.cc
// Usage: ./proj2-server <socket_path> <num_readers> <num_solvers>

#include "proj2_server.h"

#include "proj2/lib/domain_socket.h"
#include "proj2/lib/file_reader.h"
#include "proj2/lib/sha_solver.h"
#include "proj2/lib/thread_log.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>

volatile sig_atomic_t g_stop = 0;

static void signal_handler(int) {
    g_stop = 1;
}

// Parse a uint32_t from buf at offset; advances offset by 4. Returns false on
// underflow.
static bool read_u32(const char* buf, size_t total,
                     size_t& offset, uint32_t& out) {
    if (offset + 4 > total) return false;
    std::memcpy(&out, buf + offset, 4);
    offset += 4;
    return true;
}

// Copy len bytes from buf into out; advances offset. Returns false on
// underflow.
static bool read_bytes(const char* buf, size_t total,
                       size_t& offset, std::string& out, uint32_t len) {
    if (offset + len > total) return false;
    out.assign(buf + offset, len);
    offset += len;
    return true;
}

void handle_request(std::string datagram) {
    const char* buf = datagram.data();
    size_t total = datagram.size();
    size_t offset = 0;

    // --- Parse binary framing ---
    uint32_t reply_len = 0;
    if (!read_u32(buf, total, offset, reply_len)) {
        ThreadErr("handle_request: failed to parse reply endpoint length\n");
        return;
    }
    std::string reply_path;
    if (!read_bytes(buf, total, offset, reply_path, reply_len)) {
        ThreadErr("handle_request: failed to parse reply endpoint path\n");
        return;
    }

    uint32_t file_count = 0;
    if (!read_u32(buf, total, offset, file_count)) {
        ThreadErr("handle_request: failed to parse file count\n");
        return;
    }

    std::vector<std::string> file_paths(file_count);
    std::vector<uint32_t> row_counts(file_count);

    for (uint32_t i = 0; i < file_count; i++) {
        uint32_t path_len = 0;
        if (!read_u32(buf, total, offset, path_len)) {
            ThreadErr("handle_request: failed to parse path length for file %u\n", i);
            return;
        }
        if (!read_bytes(buf, total, offset, file_paths[i], path_len)) {
            ThreadErr("handle_request: failed to parse path for file %u\n", i);
            return;
        }
        if (!read_u32(buf, total, offset, row_counts[i])) {
            ThreadErr("handle_request: failed to parse row count for file %u\n", i);
            return;
        }
    }

    if (file_count == 0) return;

    // --- Acquire resources (solvers FIRST to prevent deadlock) ---
    // Always checkout solvers before readers to establish a consistent lock
    // ordering and prevent circular waits.
    uint32_t max_rows = *std::max_element(row_counts.begin(), row_counts.end());

    proj2::SolverHandle solver = proj2::ShaSolvers::Checkout(max_rows);
    proj2::ReaderHandle reader = proj2::FileReaders::Checkout(file_count, &solver);

    // --- Process files ---
    std::vector<std::vector<proj2::ReaderHandle::HashType>> hashes;
    reader.Process(file_paths, row_counts, &hashes);

    // --- Release resources (readers FIRST, then solvers) ---
    proj2::FileReaders::Checkin(std::move(reader));
    proj2::ShaSolvers::Checkin(std::move(solver));

    // --- Connect to client reply stream socket and write all hashes ---
    proj2::UnixDomainStreamClient reply_sock(reply_path);
    try {
        reply_sock.Init();
    } catch (const std::exception& e) {
        ThreadErr("handle_request: failed to connect to %s: %s\n",
                  reply_path.c_str(), e.what());
        return;
    }

    // Write hashes in file order; each hash is exactly 64 ASCII bytes
    for (const auto& file_hashes : hashes) {
        for (const auto& hash : file_hashes) {
            reply_sock.Write(hash.data(), 64);
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        ThreadErr("Usage: %s <socket_path> <num_readers> <num_solvers>\n", argv[0]);
        return 1;
    }

    const std::string socket_path = argv[1];
    const uint32_t num_readers = static_cast<uint32_t>(std::stoul(argv[2]));
    const uint32_t num_solvers = static_cast<uint32_t>(std::stoul(argv[3]));

    // Install signal handlers (only set flag — no unsafe ops in handler)
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Initialize resource pools
    proj2::ShaSolvers::Init(num_solvers);
    proj2::FileReaders::Init(num_readers);

    // Bind server datagram socket
    proj2::UnixDomainDatagramEndpoint server(socket_path);
    server.Init();

    // Set 1-second receive timeout so the loop can check g_stop between calls
    struct timeval tv{1, 0};
    (void)setsockopt(server.socket_fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ThreadLog("Server started: socket=%s readers=%u solvers=%u\n",
              socket_path.c_str(), num_readers, num_solvers);

    while (!g_stop) {
        std::string peer_path;
        std::string datagram;
        try {
            datagram = server.RecvFrom(&peer_path, 65536);
        } catch (const std::exception&) {
            // Timeout or interrupted syscall — just loop back
            continue;
        }
        if (datagram.empty()) continue;

        // Dispatch a thread per request; detach so we don't need to join
        std::thread(handle_request, std::move(datagram)).detach();
    }

    ThreadLog("Server shutting down.\n");
    StopLog();
    return 0;
}
