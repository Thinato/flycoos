#include "server.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <print>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr size_t kMaxLine = 1 << 20;  // a request longer than this drops the connection
constexpr int kPollMs = 100;          // how often the thread checks for shutdown
constexpr auto kIdle = std::chrono::seconds(5);  // a client with no complete line for this long is dropped (one slot)

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE on the socket instead
#endif

}  // namespace

bool CommandServer::start(int port) {
    listen_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from other machines
    if (listen_ < 0 || bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) || listen(listen_, 4)) {
        std::println(stderr, "command server: cannot listen on 127.0.0.1:{}: {}", port, std::strerror(errno));
        if (listen_ >= 0) close(listen_);
        listen_ = -1;
        return false;
    }
    std::println("command server listening on 127.0.0.1:{}", port);
    thread_ = std::thread([this] { loop(); });
    return true;
}

CommandServer::~CommandServer() {
    {
        std::lock_guard lock(mutex_);  // so a thread about to wait can't miss it
        stop_ = true;
    }
    answered_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (listen_ >= 0) close(listen_);
}

std::optional<std::string> CommandServer::take() {
    if (!pending_) return std::nullopt;  // the per-frame cost when idle: one atomic load
    std::lock_guard lock(mutex_);
    pending_ = false;
    return std::exchange(request_, std::nullopt);
}

void CommandServer::reply(std::string response) {
    {
        std::lock_guard lock(mutex_);
        response_ = std::move(response);
    }
    answered_.notify_all();
}

void CommandServer::loop() {
    while (!stop_) {
        pollfd p{listen_, POLLIN, 0};
        if (poll(&p, 1, kPollMs) <= 0) continue;
        int client = accept(listen_, nullptr, nullptr);
        if (client < 0) continue;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);  // a vanished client mustn't kill the game
#endif
        serve(client);
        close(client);
    }
}

// Reads request lines, hands each to the main thread and writes back its answer, until the client
// disconnects or the server stops.
void CommandServer::serve(int client) {
    std::string buffer;
    char chunk[4096];
    auto deadline = std::chrono::steady_clock::now() + kIdle;
    while (!stop_) {
        size_t end = buffer.find('\n');
        if (end == std::string::npos) {
            if (buffer.size() > kMaxLine || std::chrono::steady_clock::now() > deadline) return;
            pollfd p{client, POLLIN, 0};
            if (poll(&p, 1, kPollMs) <= 0) continue;
            ssize_t n = recv(client, chunk, sizeof chunk, 0);
            if (n <= 0) return;
            buffer.append(chunk, size_t(n));
            continue;
        }
        std::string line = buffer.substr(0, end);
        buffer.erase(0, end + 1);
        size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        // Only JSON objects: a web page's cross-origin POST starts with an HTTP request line, so the
        // connection closes there and its body never runs.
        if (line[first] != '{') return;

        std::unique_lock lock(mutex_);
        request_ = std::move(line);
        response_.reset();
        pending_ = true;
        answered_.wait(lock, [&] { return response_ || stop_; });
        if (stop_) return;
        std::string out = std::move(*response_) + "\n";
        response_.reset();
        lock.unlock();
        for (size_t sent = 0; sent < out.size();) {
            ssize_t n = send(client, out.data() + sent, out.size() - sent, kSendFlags);
            if (n <= 0) return;
            sent += size_t(n);
        }
        deadline = std::chrono::steady_clock::now() + kIdle;
    }
}
