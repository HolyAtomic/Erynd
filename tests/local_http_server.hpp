#pragma once

#include <chrono>
#include <algorithm>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace test_support {

class LocalHttpServer final {
public:
    LocalHttpServer(int status, std::string body, std::vector<std::pair<std::string, std::string>> headers = {},
        bool hold_response = false, std::vector<int> interim_statuses = {}, std::size_t expected_requests = 1) : status_{ status }, body_{ std::move(body) },
        headers_{ std::move(headers) }, interim_statuses_{ std::move(interim_statuses) },
        hold_response_{ hold_response }, expected_requests_{ expected_requests } {
        if (expected_requests_ == 0) throw std::invalid_argument("expected request count must be positive");
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
        winsock_started_ = true;
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == invalid_socket) throw std::runtime_error("socket creation failed");
        int reuse = 1;
        (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
            reinterpret_cast<const char*>(&reuse), sizeof(reuse)
#else
            &reuse, sizeof(reuse)
#endif
        );
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        const int backlog = static_cast<int>(std::min(expected_requests_, static_cast<std::size_t>(INT_MAX)));
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, backlog) != 0) {
            close_socket(listener_);
            throw std::runtime_error("loopback bind/listen failed");
        }
#ifdef _WIN32
        int address_length = sizeof(address);
#else
        socklen_t address_length = sizeof(address);
#endif
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
            close_socket(listener_);
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread{ [this] { serve(); } };
    }

    ~LocalHttpServer() {
        bool needs_accept_wakeup{};
        {
            std::lock_guard lock{ mutex_ };
            released_ = true;
            needs_accept_wakeup = requests_.size() < expected_requests_;
        }
        condition_.notify_all();
        if (listener_ != invalid_socket && needs_accept_wakeup) {
            const Socket wake = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (wake != invalid_socket) {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = htons(port_);
                (void)::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address));
                close_socket(wake);
            }
        }
        if (listener_ != invalid_socket) shutdown_socket(listener_);
        if (worker_.joinable()) worker_.join();
        if (listener_ != invalid_socket) close_socket(std::exchange(listener_, invalid_socket));
        for (const auto client : clients_) close_socket(client);
#ifdef _WIN32
        if (winsock_started_) WSACleanup();
#endif
    }

    LocalHttpServer(const LocalHttpServer&) = delete;
    LocalHttpServer& operator=(const LocalHttpServer&) = delete;

    [[nodiscard]] std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    [[nodiscard]] bool wait_for_request(std::size_t count = 1,
        std::chrono::milliseconds timeout = std::chrono::seconds{ 5 }) {
        std::unique_lock lock{ mutex_ };
        return condition_.wait_for(lock, timeout, [this, count] { return requests_.size() >= count; });
    }

    [[nodiscard]] std::string request() const {
        std::lock_guard lock{ mutex_ };
        return requests_.empty() ? std::string{} : requests_.front();
    }

    [[nodiscard]] std::vector<std::string> requests() const {
        std::lock_guard lock{ mutex_ };
        return requests_;
    }

private:
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket invalid_socket = INVALID_SOCKET;
    static void close_socket(Socket socket) noexcept { if (socket != invalid_socket) closesocket(socket); }
    static void shutdown_socket(Socket socket) noexcept { if (socket != invalid_socket) (void)::shutdown(socket, SD_BOTH); }
    static int socket_send(Socket socket, const char* data, int length) noexcept { return ::send(socket, data, length, 0); }
    static int socket_recv(Socket socket, char* data, int length) noexcept { return ::recv(socket, data, length, 0); }
#else
    using Socket = int;
    static constexpr Socket invalid_socket = -1;
    static void close_socket(Socket socket) noexcept { if (socket != invalid_socket) ::close(socket); }
    static void shutdown_socket(Socket socket) noexcept { if (socket != invalid_socket) (void)::shutdown(socket, SHUT_RDWR); }
    static int socket_send(Socket socket, const char* data, int length) noexcept { return static_cast<int>(::send(socket, data, static_cast<std::size_t>(length), 0)); }
    static int socket_recv(Socket socket, char* data, int length) noexcept { return static_cast<int>(::recv(socket, data, static_cast<std::size_t>(length), 0)); }
#endif

    static std::size_t content_length(std::string_view headers) noexcept {
        std::size_t position{};
        while (position < headers.size()) {
            const auto end = headers.find("\r\n", position);
            if (end == std::string_view::npos || end == position) break;
            const auto line = headers.substr(position, end - position);
            const auto colon = line.find(':');
            if (colon != std::string_view::npos && line.substr(0, colon).size() == 14) {
                constexpr std::string_view expected = "content-length";
                bool equal = true;
                for (std::size_t i = 0; i < expected.size(); ++i) {
                    auto ch = line[i];
                    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
                    if (ch != expected[i]) equal = false;
                }
                if (equal) {
                    auto value = line.substr(colon + 1);
                    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
                    std::size_t result{};
                    for (const char digit : value) {
                        if (digit < '0' || digit > '9') return 0;
                        result = result * 10 + static_cast<std::size_t>(digit - '0');
                    }
                    return result;
                }
            }
            position = end + 2;
        }
        return 0;
    }

    void serve() noexcept {
        std::vector<Socket> connected;
        std::vector<std::string> captured_requests;
        for (std::size_t request_index = 0; request_index < expected_requests_; ++request_index) {
            const Socket client = ::accept(listener_, nullptr, nullptr);
            if (client == invalid_socket) {
                for (const auto socket : connected) close_socket(socket);
                return;
            }
            connected.push_back(client);
            std::string request;
            char buffer[4096];
            std::size_t expected_total{};
            while (true) {
                const int received = socket_recv(client, buffer, static_cast<int>(sizeof(buffer)));
                if (received <= 0) {
                    for (const auto socket : connected) close_socket(socket);
                    close_socket(client);
                    return;
                }
                request.append(buffer, static_cast<std::size_t>(received));
                const auto separator = request.find("\r\n\r\n");
                if (separator != std::string::npos) {
                    expected_total = separator + 4 + content_length(std::string_view{ request }.substr(0, separator));
                    if (request.size() >= expected_total) break;
                }
            }
            captured_requests.push_back(std::move(request));
        }
        {
            std::lock_guard lock{ mutex_ };
            requests_ = std::move(captured_requests);
            clients_ = connected;
        }
        condition_.notify_all();
        if (hold_response_) {
            std::unique_lock lock{ mutex_ };
            condition_.wait(lock, [this] { return released_; });
            return;
        }
        std::string response;
        for (const int interim_status : interim_statuses_) {
            response += "HTTP/1.1 " + std::to_string(interim_status) + " Interim\r\nLink: </early-hint>\r\n\r\n";
        }
        response += "HTTP/1.1 " + std::to_string(status_) + " Test\r\n";
        bool has_length{};
        for (const auto& [name, value] : headers_) {
            response += name + ": " + value + "\r\n";
            if (name == "Content-Length" || name == "content-length") has_length = true;
        }
        if (!has_length) response += "Content-Length: " + std::to_string(body_.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += body_;
        for (const auto client : connected) {
            std::size_t sent{};
            while (sent < response.size()) {
                const auto remaining = response.size() - sent;
                const int chunk = static_cast<int>(std::min<std::size_t>(remaining, static_cast<std::size_t>(INT_MAX)));
                const int count = socket_send(client, response.data() + sent, chunk);
                if (count <= 0) break;
                sent += static_cast<std::size_t>(count);
            }
        }
    }

    int status_;
    std::string body_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::vector<int> interim_statuses_;
    bool hold_response_{};
    std::size_t expected_requests_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool released_{};
    std::vector<std::string> requests_;
    std::vector<Socket> clients_;
    Socket listener_{ invalid_socket };
    std::uint16_t port_{};
    std::jthread worker_;
#ifdef _WIN32
    bool winsock_started_{};
#endif
};

} // namespace test_support
