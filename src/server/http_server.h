#pragma once

#include "server/json.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace gomoku::server {

using HttpHeaders = std::map<std::string, std::string, std::less<>>;

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string http_version;
    HttpHeaders headers;
    std::string body;
    std::string remote_address;
    std::uint16_t remote_port = 0;
    // Returns true after the peer closes the connection. Handlers may poll it
    // from long-running work to stop computation that can no longer be returned.
    std::function<bool()> client_disconnected;

    [[nodiscard]] const std::string* header(std::string_view name) const noexcept;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    [[nodiscard]] static HttpResponse json(int status, const json::Value& value,
                                           const json::SerializeOptions& options = {});
    [[nodiscard]] static HttpResponse text(int status, std::string body,
                                           std::string content_type =
                                               "text/plain; charset=utf-8");
    [[nodiscard]] static HttpResponse no_content(int status = 204);
};

struct HttpServerOptions {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 8080;
    int listen_backlog = 64;
    std::size_t max_connections = 128;
    std::size_t max_request_header_bytes = 16 * 1024;
    std::size_t max_request_body_bytes = 1024 * 1024;
    std::size_t max_static_file_bytes = 16 * 1024 * 1024;
    std::size_t max_response_body_bytes = 16 * 1024 * 1024;
    std::chrono::milliseconds io_timeout{10'000};
    json::ParseLimits json_limits{};
    bool enable_cors = true;
    std::string cors_allow_origin = "*";
};

class HttpServer final {
public:
    using PostHandler =
        std::function<HttpResponse(const HttpRequest&, const json::Value&)>;

    explicit HttpServer(HttpServerOptions options = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    // Routes are exact matches. Register them before start whenever possible.
    void add_post_route(std::string path, PostHandler handler);
    void add_static_file(std::string url_path, std::filesystem::path file_path,
                         std::string content_type = {});

    // start() is non-blocking; wait() blocks until the listener stops.
    void start();
    void serve_forever();
    void wait();
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[nodiscard]] const HttpServerOptions& options() const noexcept;

private:
    struct StaticFile {
        std::filesystem::path path;
        std::string content_type;
    };

    [[nodiscard]] int open_listener();
    void accept_loop() noexcept;
    void handle_client(int client_fd, std::string remote_address,
                       std::uint16_t remote_port) noexcept;
    [[nodiscard]] std::optional<HttpRequest> read_request(
        int client_fd, std::string remote_address, std::uint16_t remote_port);
    [[nodiscard]] HttpResponse dispatch(const HttpRequest& request);
    [[nodiscard]] HttpResponse read_static_file(const StaticFile& file) const;
    void send_response(int client_fd, HttpResponse response) const noexcept;
    void connection_finished() noexcept;

    HttpServerOptions options_;

    mutable std::mutex routes_mutex_;
    std::map<std::string, PostHandler, std::less<>> post_routes_;
    std::map<std::string, StaticFile, std::less<>> static_files_;

    mutable std::mutex lifecycle_mutex_;
    std::thread accept_thread_;
    std::atomic<int> listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> bound_port_{0};

    std::atomic<std::size_t> active_connections_{0};
    mutable std::mutex workers_mutex_;
    std::condition_variable workers_finished_;

    static thread_local const HttpServer* current_worker_server_;
};

}  // namespace gomoku::server
