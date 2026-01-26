#ifndef NETWORK_H
#define NETWORK_H

#include <cstdint>
#include <string>
#include <vector>

// Cross-platform networking abstraction
// POSIX: socket(), bind(), listen(), accept(), select()/poll()
// Windows: Winsock2 (WSASocket(), bind(), listen(), accept(), WSAEventSelect())
// Reference: RFC 793 (TCP), POSIX.1-2008 sockets

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
typedef int socket_t;
#endif

namespace network {

#ifdef _WIN32
static const socket_t INVALID_SOCKET_VALUE = INVALID_SOCKET;
static const int SOCKET_ERROR_VALUE = SOCKET_ERROR;
#else
static const socket_t INVALID_SOCKET_VALUE = -1;
static const int SOCKET_ERROR_VALUE = -1;
#endif

// Initialize networking (Windows: WSAStartup, POSIX: no-op)
bool init();

// Cleanup networking (Windows: WSACleanup, POSIX: no-op)
void cleanup();

// Create TCP socket
socket_t create_tcp_socket();

// Create UDP socket
socket_t create_udp_socket();

// Bind socket to address and port
bool bind_socket(socket_t sock, const std::string& host, uint16_t port);

// Listen on socket
bool listen_socket(socket_t sock, int backlog = 128);

// Accept connection
socket_t accept_connection(socket_t sock, std::string& client_ip, uint16_t& client_port);

// Connect to remote host
bool connect_socket(socket_t sock, const std::string& host, uint16_t port);

// Set socket to non-blocking mode
bool set_nonblocking(socket_t sock);

// Set socket options (SO_REUSEADDR, etc.)
bool set_socket_option(socket_t sock, int level, int optname, int value);

// Close socket
void close_socket(socket_t sock);

// Send data (returns bytes sent, -1 on error)
ssize_t send_data(socket_t sock, const void* data, size_t len);

// Receive data (returns bytes received, -1 on error, 0 on connection closed)
ssize_t recv_data(socket_t sock, void* buffer, size_t len);

// Poll sockets for events (POSIX: poll(), Windows: WSAEventSelect equivalent)
// Returns number of ready sockets, -1 on error
int poll_sockets(socket_t* sockets, int count, int timeout_ms);

// Resolve hostname to IP address (getaddrinfo)
bool resolve_hostname(const std::string& hostname, std::string& ip);

// Get last error message
std::string get_last_error();

// Convert IP address string to sockaddr_in
bool ip_to_sockaddr(const std::string& ip, uint16_t port, struct sockaddr_in& addr);

} // namespace network

#endif // NETWORK_H
