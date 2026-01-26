#include "network.h"
#include <cstring>
#include <cerrno>
#include <stdexcept>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
static bool winsock_initialized = false;
#endif

namespace network {

bool init() {
#ifdef _WIN32
    if (winsock_initialized) return true;
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) return false;
    winsock_initialized = true;
    return true;
#else
    return true;
#endif
}

void cleanup() {
#ifdef _WIN32
    if (winsock_initialized) {
        WSACleanup();
        winsock_initialized = false;
    }
#else
    // No cleanup needed on POSIX
#endif
}

socket_t create_tcp_socket() {
    socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    if (sock == INVALID_SOCKET) return INVALID_SOCKET_VALUE;
#else
    if (sock < 0) return INVALID_SOCKET_VALUE;
#endif
    return sock;
}

socket_t create_udp_socket() {
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (sock == INVALID_SOCKET) return INVALID_SOCKET_VALUE;
#else
    if (sock < 0) return INVALID_SOCKET_VALUE;
#endif
    return sock;
}

bool bind_socket(socket_t sock, const std::string& host, uint16_t port) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (host == "0.0.0.0" || host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            return false;
        }
    }
    
    int result = bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
    return result != SOCKET_ERROR;
#else
    return result == 0;
#endif
}

bool listen_socket(socket_t sock, int backlog) {
    int result = listen(sock, backlog);
#ifdef _WIN32
    return result != SOCKET_ERROR;
#else
    return result == 0;
#endif
}

socket_t accept_connection(socket_t sock, std::string& client_ip, uint16_t& client_port) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    socket_t client_sock = accept(sock, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
#ifdef _WIN32
    if (client_sock == INVALID_SOCKET) return INVALID_SOCKET_VALUE;
#else
    if (client_sock < 0) return INVALID_SOCKET_VALUE;
#endif
    
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    client_ip = ip_str;
    client_port = ntohs(client_addr.sin_port);
    
    return client_sock;
}

bool connect_socket(socket_t sock, const std::string& host, uint16_t port) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return false;
    }
    
    int result = connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
    return result != SOCKET_ERROR;
#else
    return result == 0;
#endif
}

bool set_nonblocking(socket_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool set_socket_option(socket_t sock, int level, int optname, int value) {
    int result = setsockopt(sock, level, optname, reinterpret_cast<const char*>(&value), sizeof(value));
#ifdef _WIN32
    return result != SOCKET_ERROR;
#else
    return result == 0;
#endif
}

void close_socket(socket_t sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

ssize_t send_data(socket_t sock, const void* data, size_t len) {
#ifdef _WIN32
    int result = send(sock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) return 0; // Would block
        return -1;
    }
    return result;
#else
    ssize_t result = send(sock, data, len, 0);
    if (result < 0 && errno == EAGAIN) return 0; // Would block
    return result;
#endif
}

ssize_t recv_data(socket_t sock, void* buffer, size_t len, int flags) {
#ifdef _WIN32
    int result = recv(sock, reinterpret_cast<char*>(buffer), static_cast<int>(len), flags);
    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) return 0; // Would block
        if (error == WSAECONNRESET) return 0; // Connection closed
        return -1;
    }
    if (result == 0) return 0; // Connection closed
    return result;
#else
    ssize_t result = recv(sock, buffer, len, flags);
    if (result < 0 && errno == EAGAIN) return 0; // Would block
    if (result == 0) return 0; // Connection closed
    return result;
#endif
}

int poll_sockets(socket_t* sockets, int count, int timeout_ms) {
#ifdef _WIN32
    // Windows: Use select() for simplicity (WSAEventSelect is more complex)
    fd_set readfds;
    FD_ZERO(&readfds);
    socket_t max_fd = 0;
    for (int i = 0; i < count; ++i) {
        FD_SET(sockets[i], &readfds);
        if (sockets[i] > max_fd) max_fd = sockets[i];
    }
    
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    
    int result = select(static_cast<int>(max_fd + 1), &readfds, nullptr, nullptr, &timeout);
    if (result == SOCKET_ERROR) return -1;
    
    int ready_count = 0;
    for (int i = 0; i < count; ++i) {
        if (FD_ISSET(sockets[i], &readfds)) {
            ready_count++;
        }
    }
    return ready_count;
#else
    std::vector<struct pollfd> fds(count);
    for (int i = 0; i < count; ++i) {
        fds[i].fd = sockets[i];
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    
    int result = poll(fds.data(), count, timeout_ms);
    if (result < 0) return -1;
    
    int ready_count = 0;
    for (int i = 0; i < count; ++i) {
        if (fds[i].revents & POLLIN) {
            ready_count++;
        }
    }
    return ready_count;
#endif
}

bool resolve_hostname(const std::string& hostname, std::string& ip) {
    struct addrinfo hints, *result = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    int err = getaddrinfo(hostname.c_str(), nullptr, &hints, &result);
    if (err != 0) {
        return false;
    }
    
    bool found = false;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET) {
            struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(rp->ai_addr);
            char ip_str[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN) != nullptr) {
                ip = ip_str;
                found = true;
                break;
            }
        }
    }
    
    freeaddrinfo(result);
    return found;
}

std::string get_last_error() {
#ifdef _WIN32
    int error = WSAGetLastError();
    char buffer[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, error, 0, buffer, sizeof(buffer), nullptr);
    return std::string(buffer);
#else
    return std::string(strerror(errno));
#endif
}

bool ip_to_sockaddr(const std::string& ip, uint16_t port, struct sockaddr_in& addr) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) == 1;
}

bool get_peer_address(socket_t sock, std::string& ip, uint16_t& port) {
    struct sockaddr_in peer_addr;
    socklen_t addr_len = sizeof(peer_addr);
    
#ifdef _WIN32
    int result = getpeername(sock, reinterpret_cast<struct sockaddr*>(&peer_addr), &addr_len);
    if (result == SOCKET_ERROR) {
        return false;
    }
#else
    int result = getpeername(sock, reinterpret_cast<struct sockaddr*>(&peer_addr), &addr_len);
    if (result != 0) {
        return false;
    }
#endif
    
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    ip = ip_str;
    port = ntohs(peer_addr.sin_port);
    
    return true;
}

} // namespace network
