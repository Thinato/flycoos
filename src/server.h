#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Localhost command server: newline-delimited JSON over TCP on 127.0.0.1, one client at a time.
// The thread only does socket I/O. Each request line waits in a one-slot mailbox until the main
// thread takes it (take) at a fixed point of the frame and answers it (reply); then the next line
// is read. A line that isn't a JSON object (e.g. HTTP) or 5 s without a complete line closes the
// connection. No authentication: anything on this machine can edit the world while the game runs.
class CommandServer {
public:
    CommandServer() = default;
    CommandServer(const CommandServer&) = delete;
    CommandServer& operator=(const CommandServer&) = delete;
    ~CommandServer();  // stops the thread within ~100 ms

    bool start(int port);  // false (with a message) if the port can't be bound
    std::optional<std::string> take();  // main thread: the waiting request, if any
    void reply(std::string response);   // main thread: answers the request take() returned

private:
    void loop();
    void serve(int client);

    int listen_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false}, pending_{false};  // pending_: a request waits; lets take() skip the lock
    std::mutex mutex_;
    std::condition_variable answered_;
    std::optional<std::string> request_, response_;
};
