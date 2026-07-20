#include "server/http_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace gomoku::server {
namespace {

constexpr std::size_t kMaxResponseHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxCustomResponseHeaders = 100;
constexpr std::size_t kMaxHeaderNameBytes = 128;
constexpr std::size_t kMaxHeaderValueBytes = 8192;

class HttpError final : public std::runtime_error {
public:
    HttpError(int status, std::string message)
        : std::runtime_error(std::move(message)), status_(status) {}

    [[nodiscard]] int status() const noexcept { return status_; }

private:
    int status_;
};

char ascii_lower(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

std::string to_ascii_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower);
    return result;
}

std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool is_token_character(unsigned char ch) noexcept {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9')) {
        return true;
    }
    switch (ch) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~': return true;
        default: return false;
    }
}

bool is_valid_header_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxHeaderNameBytes) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char ch) {
        return is_token_character(static_cast<unsigned char>(ch));
    });
}

bool is_valid_header_value(std::string_view value) noexcept {
    if (value.size() > kMaxHeaderValueBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte == '\t' || (byte >= 0x20 && byte != 0x7f);
    });
}

bool is_safe_route_path(std::string_view path) noexcept {
    if (path.empty() || path.front() != '/' || path.size() > 2048) {
        return false;
    }
    for (char ch : path) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x21 || byte == 0x7f || ch == '\\' || ch == '?' || ch == '#') {
            return false;
        }
    }

    std::size_t segment_start = 1;
    while (segment_start <= path.size()) {
        const std::size_t slash = path.find('/', segment_start);
        const std::size_t segment_end =
            slash == std::string_view::npos ? path.size() : slash;
        const std::string_view segment =
            path.substr(segment_start, segment_end - segment_start);
        if (segment == "." || segment == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        segment_start = slash + 1;
    }
    return true;
}

std::string status_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 417: return "Expectation Failed";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

HttpResponse error_response(int status, std::string code, std::string message) {
    json::Value::Object body;
    body.emplace("error", std::move(code));
    body.emplace("message", std::move(message));
    body.emplace("status", status);
    return HttpResponse::json(status, json::Value(std::move(body)));
}

bool parse_content_length(std::string_view text, std::size_t& result) noexcept {
    if (text.empty() || text.front() == '+' || text.front() == '-') {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    result = static_cast<std::size_t>(parsed);
    return true;
}

bool is_application_json(std::string_view content_type) {
    const std::size_t semicolon = content_type.find(';');
    content_type = trim_ascii(content_type.substr(0, semicolon));
    return to_ascii_lower(content_type) == "application/json";
}

std::string guess_content_type(const std::filesystem::path& path) {
    const std::string extension = to_ascii_lower(path.extension().string());
    if (extension == ".html" || extension == ".htm") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".js" || extension == ".mjs") {
        return "text/javascript; charset=utf-8";
    }
    if (extension == ".json" || extension == ".map") {
        return "application/json; charset=utf-8";
    }
    if (extension == ".txt") {
        return "text/plain; charset=utf-8";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".gif") {
        return "image/gif";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    if (extension == ".ico") {
        return "image/x-icon";
    }
    if (extension == ".wasm") {
        return "application/wasm";
    }
    return "application/octet-stream";
}

void set_close_on_exec(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

void configure_client_socket(int fd,
                             std::chrono::milliseconds timeout) noexcept {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        timeout - seconds);
    timeval value{};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(seconds.count());
    value.tv_usec = static_cast<decltype(value.tv_usec)>(microseconds.count());
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
    const int enabled = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    set_close_on_exec(fd);
}

bool send_all(int fd, std::string_view data) noexcept {
    while (!data.empty()) {
#ifdef MSG_NOSIGNAL
        const ssize_t sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
#else
        const ssize_t sent = ::send(fd, data.data(), data.size(), 0);
#endif
        if (sent > 0) {
            data.remove_prefix(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool client_disconnected(int fd) noexcept {
    char byte = 0;
    const ssize_t received = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (received == 0) {
        return true;
    }
    if (received > 0) {
        return false;
    }
    return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
}

std::pair<std::string, std::uint16_t> numeric_peer_name(
    const sockaddr* address, socklen_t length) {
    std::array<char, NI_MAXHOST> host{};
    std::array<char, NI_MAXSERV> service{};
    const int result = ::getnameinfo(
        address, length, host.data(), static_cast<socklen_t>(host.size()),
        service.data(), static_cast<socklen_t>(service.size()),
        NI_NUMERICHOST | NI_NUMERICSERV);
    if (result != 0) {
        return {"unknown", 0};
    }
    std::uint16_t port = 0;
    unsigned parsed_port = 0;
    const std::string_view port_text(service.data());
    const auto [end, error] = std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), parsed_port);
    if (error == std::errc{} && end == port_text.data() + port_text.size() &&
        parsed_port <= std::numeric_limits<std::uint16_t>::max()) {
        port = static_cast<std::uint16_t>(parsed_port);
    }
    return {std::string(host.data()), port};
}

bool is_reserved_response_header(std::string_view lower_name) noexcept {
    return lower_name == "content-length" || lower_name == "content-type" ||
           lower_name == "connection" || lower_name == "server" ||
           lower_name == "access-control-allow-origin" ||
           lower_name == "x-content-type-options";
}

}  // namespace

thread_local const HttpServer* HttpServer::current_worker_server_ = nullptr;

const std::string* HttpRequest::header(std::string_view name) const noexcept {
    for (const auto& [stored_name, value] : headers) {
        if (stored_name.size() != name.size()) {
            continue;
        }
        bool equal = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (stored_name[i] != ascii_lower(name[i])) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return &value;
        }
    }
    return nullptr;
}

HttpResponse HttpResponse::json(int status, const json::Value& value,
                                const json::SerializeOptions& options) {
    HttpResponse response;
    response.status = status;
    response.content_type = "application/json; charset=utf-8";
    response.body = json::stringify(value, options);
    return response;
}

HttpResponse HttpResponse::text(int status, std::string body,
                                std::string content_type) {
    HttpResponse response;
    response.status = status;
    response.content_type = std::move(content_type);
    response.body = std::move(body);
    return response;
}

HttpResponse HttpResponse::no_content(int status) {
    HttpResponse response;
    response.status = status;
    response.content_type.clear();
    response.body.clear();
    return response;
}

HttpServer::HttpServer(HttpServerOptions options)
    : options_(std::move(options)), bound_port_(options_.port) {
    if (options_.listen_backlog <= 0 || options_.max_connections == 0 ||
        options_.max_request_header_bytes == 0 ||
        options_.max_request_body_bytes == 0 ||
        options_.max_static_file_bytes == 0 ||
        options_.max_response_body_bytes == 0 ||
        options_.io_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("HTTP server limits and timeout must be positive");
    }
    if (!is_valid_header_value(options_.cors_allow_origin)) {
        throw std::invalid_argument("invalid CORS allow-origin value");
    }
    if (options_.bind_address.find('\0') != std::string::npos) {
        throw std::invalid_argument("bind address contains a null byte");
    }
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::add_post_route(std::string path, PostHandler handler) {
    if (!is_safe_route_path(path)) {
        throw std::invalid_argument("POST route must be a safe absolute URL path");
    }
    if (!handler) {
        throw std::invalid_argument("POST route handler must not be empty");
    }
    std::lock_guard lock(routes_mutex_);
    const auto [unused, inserted] =
        post_routes_.emplace(std::move(path), std::move(handler));
    (void)unused;
    if (!inserted) {
        throw std::invalid_argument("POST route is already registered");
    }
}

void HttpServer::add_static_file(std::string url_path,
                                 std::filesystem::path file_path,
                                 std::string content_type) {
    if (!is_safe_route_path(url_path)) {
        throw std::invalid_argument(
            "static resource route must be a safe absolute URL path");
    }
    if (url_path == "/api/health") {
        throw std::invalid_argument("/api/health is reserved by the server");
    }
    if (file_path.empty()) {
        throw std::invalid_argument("static resource file path must not be empty");
    }
    if (content_type.empty()) {
        content_type = guess_content_type(file_path);
    }
    if (!is_valid_header_value(content_type)) {
        throw std::invalid_argument("invalid static resource content type");
    }

    StaticFile entry{std::move(file_path), std::move(content_type)};
    std::lock_guard lock(routes_mutex_);
    const auto [unused, inserted] =
        static_files_.emplace(std::move(url_path), std::move(entry));
    (void)unused;
    if (!inserted) {
        throw std::invalid_argument("static resource route is already registered");
    }
}

int HttpServer::open_listener() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* addresses = nullptr;
    const std::string port_text = std::to_string(options_.port);
    const char* node = options_.bind_address.empty()
                           ? nullptr
                           : options_.bind_address.c_str();
    const int lookup =
        ::getaddrinfo(node, port_text.c_str(), &hints, &addresses);
    if (lookup != 0) {
        throw std::runtime_error("failed to resolve bind address: " +
                                 std::string(::gai_strerror(lookup)));
    }

    int listener = -1;
    int last_error = EADDRNOTAVAIL;
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        listener =
            ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (listener < 0) {
            last_error = errno;
            continue;
        }
        set_close_on_exec(listener);
        const int enabled = 1;
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                           sizeof(enabled));
        if (::bind(listener, address->ai_addr, address->ai_addrlen) == 0 &&
            ::listen(listener, options_.listen_backlog) == 0) {
            break;
        }
        last_error = errno;
        ::close(listener);
        listener = -1;
    }
    ::freeaddrinfo(addresses);

    if (listener < 0) {
        throw std::system_error(last_error, std::generic_category(),
                                "failed to bind HTTP listener");
    }

    sockaddr_storage local_address{};
    socklen_t local_length = sizeof(local_address);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&local_address),
                      &local_length) == 0) {
        const auto [unused_address, local_port] = numeric_peer_name(
            reinterpret_cast<const sockaddr*>(&local_address), local_length);
        (void)unused_address;
        bound_port_.store(local_port, std::memory_order_release);
    } else {
        bound_port_.store(options_.port, std::memory_order_release);
    }
    return listener;
}

void HttpServer::start() {
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        throw std::logic_error("HTTP server is already running");
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    {
        std::unique_lock workers_lock(workers_mutex_);
        workers_finished_.wait(workers_lock, [this] {
            return active_connections_.load(std::memory_order_acquire) == 0;
        });
    }

    const int listener = open_listener();
    listen_fd_.store(listener, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    try {
        accept_thread_ = std::thread(&HttpServer::accept_loop, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        listen_fd_.store(-1, std::memory_order_release);
        ::close(listener);
        throw;
    }
}

void HttpServer::serve_forever() {
    start();
    wait();
}

void HttpServer::wait() {
    std::thread listener_thread;
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (accept_thread_.joinable()) {
            listener_thread = std::move(accept_thread_);
        }
    }
    if (listener_thread.joinable()) {
        listener_thread.join();
    }
    if (current_worker_server_ != this) {
        std::unique_lock lock(workers_mutex_);
        workers_finished_.wait(lock, [this] {
            return active_connections_.load(std::memory_order_acquire) == 0;
        });
    }
}

void HttpServer::stop() noexcept {
    running_.store(false, std::memory_order_release);
    const int listener = listen_fd_.exchange(-1, std::memory_order_acq_rel);
    if (listener >= 0) {
        (void)::shutdown(listener, SHUT_RDWR);
        (void)::close(listener);
    }

    std::thread listener_thread;
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (accept_thread_.joinable() &&
            accept_thread_.get_id() != std::this_thread::get_id()) {
            listener_thread = std::move(accept_thread_);
        }
    }
    if (listener_thread.joinable()) {
        listener_thread.join();
    }

    if (current_worker_server_ != this) {
        std::unique_lock lock(workers_mutex_);
        workers_finished_.wait(lock, [this] {
            return active_connections_.load(std::memory_order_acquire) == 0;
        });
    }
}

bool HttpServer::is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::uint16_t HttpServer::bound_port() const noexcept {
    return bound_port_.load(std::memory_order_acquire);
}

const HttpServerOptions& HttpServer::options() const noexcept { return options_; }

void HttpServer::accept_loop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        const int listener = listen_fd_.load(std::memory_order_acquire);
        if (listener < 0) {
            break;
        }

        sockaddr_storage remote{};
        socklen_t remote_length = sizeof(remote);
        const int client = ::accept(listener, reinterpret_cast<sockaddr*>(&remote),
                                    &remote_length);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (!running_.load(std::memory_order_acquire) || errno == EBADF ||
                errno == EINVAL) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        configure_client_socket(client, options_.io_timeout);
        if (!running_.load(std::memory_order_acquire)) {
            ::close(client);
            break;
        }

        std::size_t active = active_connections_.load(std::memory_order_relaxed);
        bool acquired = false;
        while (active < options_.max_connections) {
            if (active_connections_.compare_exchange_weak(
                    active, active + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                acquired = true;
                break;
            }
        }
        if (!acquired) {
            send_response(client,
                          error_response(503, "server_busy",
                                         "too many concurrent connections"));
            ::close(client);
            continue;
        }

        auto [remote_address, remote_port] = numeric_peer_name(
            reinterpret_cast<const sockaddr*>(&remote), remote_length);
        try {
            std::thread([this, client,
                         remote_address = std::move(remote_address),
                         remote_port]() mutable {
                current_worker_server_ = this;
                handle_client(client, std::move(remote_address), remote_port);
                ::close(client);
                current_worker_server_ = nullptr;
                connection_finished();
            }).detach();
        } catch (...) {
            active_connections_.fetch_sub(1, std::memory_order_acq_rel);
            workers_finished_.notify_all();
            send_response(client,
                          error_response(503, "server_busy",
                                         "unable to create a worker thread"));
            ::close(client);
        }
    }

    running_.store(false, std::memory_order_release);
    const int listener = listen_fd_.exchange(-1, std::memory_order_acq_rel);
    if (listener >= 0) {
        ::close(listener);
    }
}

void HttpServer::connection_finished() noexcept {
    active_connections_.fetch_sub(1, std::memory_order_acq_rel);
    workers_finished_.notify_all();
}

std::optional<HttpRequest> HttpServer::read_request(
    int client_fd, std::string remote_address, std::uint16_t remote_port) {
    std::string buffer;
    buffer.reserve(std::min<std::size_t>(options_.max_request_header_bytes, 8192));
    constexpr std::string_view delimiter = "\r\n\r\n";
    std::size_t header_end = std::string::npos;
    std::array<char, 4096> chunk{};

    while (header_end == std::string::npos) {
        const ssize_t received = ::recv(client_fd, chunk.data(), chunk.size(), 0);
        if (received == 0) {
            if (buffer.empty()) {
                return std::nullopt;
            }
            throw HttpError(400, "connection closed before request headers completed");
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                throw HttpError(408, "timed out while reading request headers");
            }
            throw HttpError(400, "failed to read request headers");
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(received));
        header_end = buffer.find(delimiter);
        if (header_end != std::string::npos) {
            if (header_end + delimiter.size() >
                options_.max_request_header_bytes) {
                throw HttpError(431, "request headers exceed configured limit");
            }
        } else if (buffer.size() > options_.max_request_header_bytes) {
            throw HttpError(431, "request headers exceed configured limit");
        }
    }

    const std::string_view header_block(buffer.data(), header_end);
    const std::size_t request_line_end = header_block.find("\r\n");
    const std::string_view request_line =
        request_line_end == std::string_view::npos
            ? header_block
            : header_block.substr(0, request_line_end);
    const std::size_t first_space = request_line.find(' ');
    const std::size_t second_space =
        first_space == std::string_view::npos
            ? std::string_view::npos
            : request_line.find(' ', first_space + 1);
    if (first_space == std::string_view::npos ||
        second_space == std::string_view::npos || first_space == 0 ||
        second_space == first_space + 1 ||
        request_line.find(' ', second_space + 1) != std::string_view::npos) {
        throw HttpError(400, "malformed HTTP request line");
    }

    HttpRequest request;
    request.method = std::string(request_line.substr(0, first_space));
    request.target = std::string(request_line.substr(
        first_space + 1, second_space - first_space - 1));
    request.http_version = std::string(request_line.substr(second_space + 1));
    request.remote_address = std::move(remote_address);
    request.remote_port = remote_port;

    if (request.method.size() > 16 ||
        !std::all_of(request.method.begin(), request.method.end(), [](char ch) {
            return is_token_character(static_cast<unsigned char>(ch));
        })) {
        throw HttpError(400, "invalid HTTP method");
    }
    if (request.http_version != "HTTP/1.1" &&
        request.http_version != "HTTP/1.0") {
        throw HttpError(400, "unsupported HTTP version");
    }
    if (request.target.empty() || request.target.front() != '/' ||
        request.target.find('#') != std::string::npos) {
        throw HttpError(400, "only origin-form request targets are supported");
    }
    const std::size_t query_separator = request.target.find('?');
    request.path = request.target.substr(0, query_separator);
    if (query_separator != std::string::npos) {
        request.query = request.target.substr(query_separator + 1);
    }
    if (!is_safe_route_path(request.path)) {
        throw HttpError(400, "invalid request path");
    }

    std::size_t cursor = request_line_end == std::string_view::npos
                             ? header_block.size()
                             : request_line_end + 2;
    while (cursor < header_block.size()) {
        const std::size_t line_end = header_block.find("\r\n", cursor);
        const std::string_view line =
            line_end == std::string_view::npos
                ? header_block.substr(cursor)
                : header_block.substr(cursor, line_end - cursor);
        if (line.empty() || line.front() == ' ' || line.front() == '\t') {
            throw HttpError(400, "invalid folded or empty header line");
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            throw HttpError(400, "malformed HTTP header");
        }
        const std::string_view raw_name = line.substr(0, colon);
        const std::string_view raw_value = trim_ascii(line.substr(colon + 1));
        if (!is_valid_header_name(raw_name) || !is_valid_header_value(raw_value)) {
            throw HttpError(400, "invalid HTTP header");
        }
        std::string name = to_ascii_lower(raw_name);
        const auto [unused, inserted] =
            request.headers.emplace(std::move(name), std::string(raw_value));
        (void)unused;
        if (!inserted) {
            throw HttpError(400, "duplicate HTTP header is not supported");
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        cursor = line_end + 2;
    }

    if (request.http_version == "HTTP/1.1" &&
        request.headers.find("host") == request.headers.end()) {
        throw HttpError(400, "HTTP/1.1 request requires a Host header");
    }
    if (request.headers.find("transfer-encoding") != request.headers.end()) {
        throw HttpError(501, "Transfer-Encoding is not supported");
    }
    if (request.headers.find("expect") != request.headers.end()) {
        throw HttpError(417, "Expect header is not supported");
    }

    std::size_t content_length = 0;
    const auto length_header = request.headers.find("content-length");
    if (length_header != request.headers.end() &&
        !parse_content_length(length_header->second, content_length)) {
        throw HttpError(400, "invalid Content-Length header");
    }
    if (request.method == "POST" && length_header == request.headers.end()) {
        throw HttpError(411, "POST requests require Content-Length");
    }
    if (content_length > options_.max_request_body_bytes) {
        throw HttpError(413, "request body exceeds configured limit");
    }

    const std::size_t body_start = header_end + delimiter.size();
    const std::size_t buffered_body = buffer.size() - body_start;
    request.body.reserve(content_length);
    request.body.append(buffer.data() + body_start,
                        std::min(buffered_body, content_length));
    while (request.body.size() < content_length) {
        const std::size_t remaining = content_length - request.body.size();
        const ssize_t received =
            ::recv(client_fd, chunk.data(), std::min(chunk.size(), remaining), 0);
        if (received == 0) {
            throw HttpError(400, "connection closed before request body completed");
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                throw HttpError(408, "timed out while reading request body");
            }
            throw HttpError(400, "failed to read request body");
        }
        request.body.append(chunk.data(), static_cast<std::size_t>(received));
    }
    return request;
}

HttpResponse HttpServer::dispatch(const HttpRequest& request) {
    if (request.method == "OPTIONS") {
        HttpResponse response = HttpResponse::no_content();
        response.headers.emplace_back("Allow", "GET, POST, OPTIONS");
        if (options_.enable_cors) {
            response.headers.emplace_back("Access-Control-Allow-Methods",
                                          "GET, POST, OPTIONS");
            response.headers.emplace_back("Access-Control-Allow-Headers",
                                          "Content-Type");
            response.headers.emplace_back("Access-Control-Max-Age", "600");
        }
        return response;
    }

    if (request.method == "GET") {
        if (request.path == "/api/health") {
            json::Value::Object body;
            body.emplace("status", "ok");
            return HttpResponse::json(200, json::Value(std::move(body)));
        }

        std::optional<StaticFile> file;
        {
            std::lock_guard lock(routes_mutex_);
            const auto iterator = static_files_.find(request.path);
            if (iterator != static_files_.end()) {
                file = iterator->second;
            }
        }
        if (!file) {
            return error_response(404, "not_found", "resource not found");
        }
        return read_static_file(*file);
    }

    if (request.method == "POST") {
        PostHandler handler;
        {
            std::lock_guard lock(routes_mutex_);
            const auto iterator = post_routes_.find(request.path);
            if (iterator != post_routes_.end()) {
                handler = iterator->second;
            }
        }
        if (!handler) {
            return error_response(404, "not_found", "API route not found");
        }

        const std::string* content_type = request.header("content-type");
        if (content_type == nullptr || !is_application_json(*content_type)) {
            return error_response(415, "unsupported_media_type",
                                  "Content-Type must be application/json");
        }

        json::Value body;
        try {
            body = json::parse(request.body, options_.json_limits);
        } catch (const json::ParseError& error) {
            return error_response(400, "invalid_json", error.what());
        }

        try {
            return handler(request, body);
        } catch (const std::exception&) {
            return error_response(500, "handler_error",
                                  "request handler failed");
        } catch (...) {
            return error_response(500, "handler_error",
                                  "request handler failed");
        }
    }

    HttpResponse response =
        error_response(405, "method_not_allowed", "HTTP method is not allowed");
    response.headers.emplace_back("Allow", "GET, POST, OPTIONS");
    return response;
}

HttpResponse HttpServer::read_static_file(const StaticFile& file) const {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(file.path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        return error_response(404, "not_found", "resource not found");
    }
    const std::uintmax_t size = std::filesystem::file_size(file.path, error);
    if (error) {
        return error_response(404, "not_found", "resource not found");
    }
    if (size > options_.max_static_file_bytes ||
        size > std::numeric_limits<std::size_t>::max()) {
        return error_response(500, "resource_too_large",
                              "static resource exceeds server limit");
    }

    std::ifstream input(file.path, std::ios::binary);
    if (!input) {
        return error_response(404, "not_found", "resource not found");
    }
    std::string body(static_cast<std::size_t>(size), '\0');
    if (!body.empty()) {
        input.read(body.data(), static_cast<std::streamsize>(body.size()));
        if (!input || static_cast<std::size_t>(input.gcount()) != body.size()) {
            return error_response(500, "resource_read_failed",
                                  "failed to read static resource");
        }
    }
    return HttpResponse::text(200, std::move(body), file.content_type);
}

void HttpServer::send_response(int client_fd, HttpResponse response) const noexcept {
    try {
        if (response.status < 100 || response.status > 599 ||
            response.body.size() > options_.max_response_body_bytes) {
            response = error_response(500, "invalid_response",
                                      "handler produced an invalid response");
        }
        if (!is_valid_header_value(response.content_type)) {
            response = error_response(500, "invalid_response",
                                      "handler produced an invalid response");
        }
        if (response.status == 204 || response.status == 304 ||
            (response.status >= 100 && response.status < 200)) {
            response.body.clear();
            response.content_type.clear();
        }

        std::string head;
        head.reserve(512);
        head.append("HTTP/1.1 ");
        head.append(std::to_string(response.status));
        head.push_back(' ');
        head.append(status_reason(response.status));
        head.append("\r\nServer: gomoku-analyzer\r\nConnection: close\r\n");
        head.append("X-Content-Type-Options: nosniff\r\n");
        head.append("Content-Length: ");
        head.append(std::to_string(response.body.size()));
        head.append("\r\n");
        if (!response.content_type.empty()) {
            head.append("Content-Type: ");
            head.append(response.content_type);
            head.append("\r\n");
        }
        if (options_.enable_cors) {
            head.append("Access-Control-Allow-Origin: ");
            head.append(options_.cors_allow_origin);
            head.append("\r\n");
        }

        std::size_t emitted_headers = 0;
        for (const auto& [name, value] : response.headers) {
            if (emitted_headers >= kMaxCustomResponseHeaders ||
                !is_valid_header_name(name) || !is_valid_header_value(value)) {
                continue;
            }
            const std::string lower_name = to_ascii_lower(name);
            if (is_reserved_response_header(lower_name)) {
                continue;
            }
            const std::size_t additional = name.size() + value.size() + 4;
            if (head.size() + additional + 2 > kMaxResponseHeaderBytes) {
                break;
            }
            head.append(name);
            head.append(": ");
            head.append(value);
            head.append("\r\n");
            ++emitted_headers;
        }
        head.append("\r\n");

        if (!send_all(client_fd, head)) {
            return;
        }
        (void)send_all(client_fd, response.body);
    } catch (...) {
        // A failed allocation or serialization leaves the connection closed.
    }
}

void HttpServer::handle_client(int client_fd, std::string remote_address,
                               std::uint16_t remote_port) noexcept {
    try {
        std::optional<HttpRequest> request = read_request(
            client_fd, std::move(remote_address), remote_port);
        if (!request) {
            return;
        }
        request->client_disconnected = [client_fd]() noexcept {
            return client_disconnected(client_fd);
        };
        send_response(client_fd, dispatch(*request));
    } catch (const HttpError& error) {
        send_response(client_fd,
                      error_response(error.status(), "bad_request", error.what()));
    } catch (const std::exception&) {
        send_response(client_fd,
                      error_response(500, "internal_error",
                                     "internal server error"));
    } catch (...) {
        send_response(client_fd,
                      error_response(500, "internal_error",
                                     "internal server error"));
    }
}

}  // namespace gomoku::server
