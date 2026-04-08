#ifndef PROJ2_SERVER_H_
#define PROJ2_SERVER_H_

#include <csignal>
#include <string>
#include <vector>

// Global stop flag set by signal handler
extern volatile sig_atomic_t g_stop;

// Parse and handle one client request datagram
void handle_request(std::string datagram);

#endif  // PROJ2_SERVER_H_
