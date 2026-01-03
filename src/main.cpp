// src/main.cpp
#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstring>
#include <string>
#include <sstream>
#include <cctype>
#include <strings.h>
#include <vector>
#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <errno.h>
#include <ctime>
#include <mutex>

std::mutex log_mutex;
std::ofstream log_file;

static std::atomic<bool> running(true);
int server_fd_global = -1;

void signal_handler(int) {
    running = false;
    if (server_fd_global != -1) {
        close(server_fd_global);
    }
}

ssize_t send_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, buf + total, len - total, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return (ssize_t)total;
}

std::string recv_headers(int fd) {
    std::string data;
    char buf[1024];

    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        data.append(buf, n);
        if (data.find("\r\n\r\n") != std::string::npos)
            break;
        // safety cap to avoid unbounded growth for malformed input
        if (data.size() > 64 * 1024) break;
    }
    return data;
}

static inline std::string trim_crlf(std::string s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

void send_http_error(int client_fd, int code, const std::string &reason) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << " " << reason << "\r\n"
        << "Content-Length: 0\r\n"
        << "Connection: close\r\n\r\n";
    send_all(client_fd, oss.str().c_str(), oss.str().size());
}

std::unordered_set<std::string> blocked;

void load_blocked(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        // trim spaces
        while (!line.empty() && isspace(line.back())) line.pop_back();
        while (!line.empty() && isspace(line.front())) line.erase(line.begin());
        blocked.insert(line);
    }
}

bool is_blocked(const std::string& host) {
    if (blocked.count(host)) return true;
    // simple suffix match: *.example.com
    for (auto& b : blocked) {
        if (b.size() < host.size() &&
            host.compare(host.size() - b.size(), b.size(), b) == 0 &&
            host[host.size() - b.size() - 1] == '.') {
            return true;
        }
    }
    return false;
}

void log_event(const std::string& client_ip,
               int client_port,
               const std::string& request_line,
               const std::string& host,
               int port,
               const std::string& action,
               int status,
               size_t bytes) {
    std::lock_guard<std::mutex> lock(log_mutex);

    std::time_t now = std::time(nullptr);
    char ts[64];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    log_file << "[" << ts << "] "
         << client_ip << ":" << client_port << " "
         << "\"" << request_line << "\" "
         << host << ":" << port << " "
         << action << " "
         << status << " "
         << bytes << "B\n";


    log_file.flush();
}

void tunnel(int fd1, int fd2) {
    char buf[8192];
    while (true) {
        ssize_t n = recv(fd1, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (send_all(fd2, buf, n) < 0) break;
    }
}

ssize_t recv_all(int fd, char* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, buf + total, len - total, 0);
        if (n <= 0) return n;
        total += n;
    }
    return total;
}


// handle_client: main per-connection routine
void handle_client(int client_fd) {


    size_t bytes_to_client = 0;

    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getpeername(client_fd, (sockaddr*)&addr, &len);
    std::string client_ip = inet_ntoa(addr.sin_addr);
    int client_port = ntohs(addr.sin_port);

   
    //Receive request headers from client
   
    std::string req = recv_headers(client_fd);

    // Debug: print raw headers we received
    std::cerr << "\n===== RECEIVED REQUEST HEADERS =====\n";
    if (req.empty()) std::cerr << "(no headers)\n";
    else std::cerr << req << "\n";
    std::cerr << "====================================\n";

    if (req.empty()) {
        std::cerr << "ERROR: Empty request received - closing connection\n";
        close(client_fd);
        return;
    }

    // Parse request line
    std::istringstream iss(req);
    std::string method, target, version;
    iss >> method >> target >> version;

    std::string request_line = method + " " + target + " " + version;


    // debug: parsed request line
    std::cerr << "PARSED REQUEST LINE: METHOD=" << method
              << " TARGET=" << target << " VERSION=" << version << "\n";

    // parse headers to get Host and to reconstruct later
    std::string line;
    std::string host;
    int port = 80;
    std::vector<std::string> headers;

    // detect body presence and content length
    bool has_body = false;
    size_t content_length = 0;

    // consume remainder of the first line in iss (getline will start from next line)
    std::getline(iss, line);

 
    // STEP 2: Parse headers and extract Host and Content-Length
    while (std::getline(iss, line)) {
        if (line == "\r" || line == "") break;
        std::string h = trim_crlf(line);

        // capture Host header
        if (h.size() >= 5 && strncasecmp(h.c_str(), "Host:", 5) == 0) {
            std::string v = h.substr(5);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            auto pos = v.find(':');
            if (pos != std::string::npos) {
                host = v.substr(0, pos);
                port = std::stoi(v.substr(pos + 1));
            } else {
                host = v;
            }
        }
        // capture Content-Length
        else if (h.size() >= 15 && strncasecmp(h.c_str(), "Content-Length:", 15) == 0) {
            std::string v = h.substr(15);
            while (!v.empty() && isspace((unsigned char)v.front())) v.erase(v.begin());
            try {
                content_length = std::stoul(v);
                has_body = true;
            } catch (...) {
                content_length = 0;
                has_body = false;
            }
            headers.push_back(h);
        }
        else {
            headers.push_back(h);
        }
    }

    // Debug: header parsing summary
    std::cerr << "HEADER PARSE: HOST=" << host
              << " HAS_BODY=" << (has_body ? "1" : "0")
              << " CONTENT_LENGTH=" << content_length << "\n";

    // Handle CONNECT (HTTPS tunnel)
    if (method == "CONNECT") {
        // target is "host:port"
        auto pos = target.find(':');
        if (pos == std::string::npos) {
            send_http_error(client_fd, 400, "Bad Request");
            close(client_fd);
            return;
        }

        std::string connect_host = target.substr(0, pos);
        int connect_port = std::stoi(target.substr(pos + 1));

        // BLOCKING applies to CONNECT too
        if (is_blocked(connect_host)) {
            const char* resp =
                "HTTP/1.1 403 Forbidden\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            send_all(client_fd, resp, strlen(resp));
            log_event(client_ip,
                client_port,
                request_line,
                host,
                port,
                "BLOCKED",
                403,
                0);

            std::cerr << "CONNECT blocked for host=" << connect_host << "\n";
            close(client_fd);
            return;
        }

        // DNS resolve
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        std::string port_str = std::to_string(connect_port);

        if (getaddrinfo(connect_host.c_str(), port_str.c_str(), &hints, &res) != 0) {
            send_http_error(client_fd, 502, "Bad Gateway");
            close(client_fd);
            return;
        }

        int server_fd = -1;
        for (auto p = res; p; p = p->ai_next) {
            server_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (server_fd < 0) continue;
            if (connect(server_fd, p->ai_addr, p->ai_addrlen) == 0) break;
            close(server_fd);
            server_fd = -1;
        }
        freeaddrinfo(res);

        if (server_fd < 0) {
            send_http_error(client_fd, 502, "Bad Gateway");
            close(client_fd);
            return;
        }

        // Tell client tunnel is ready
        const char* ok =
            "HTTP/1.1 200 Connection Established\r\n"
            "Proxy-Agent: SimpleProxy\r\n\r\n";
        if (send_all(client_fd, ok, strlen(ok)) < 0) {
            close(server_fd);
            close(client_fd);
            return;
        }

        std::cerr << "CONNECT tunnel established for " << connect_host << ":" << connect_port << "\n";

        // Bidirectional tunnel
        std::thread t1(tunnel, client_fd, server_fd);
        std::thread t2(tunnel, server_fd, client_fd);
        t1.join();
        t2.join();

        log_event(client_ip,
          client_port,
          request_line,
          host,
          port,
          "ALLOWED",
          200,
          bytes_to_client);


        close(server_fd);
        close(client_fd);
        return;
    }

    // STEP 3: Read request body 
    // Split headers and body from initial buffer
    std::string body;
    size_t header_end = req.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        send_http_error(client_fd, 400, "Bad Request");
        close(client_fd);
        return;
    }

    size_t body_start = header_end + 4;

    // If recv_headers already read part of the body
    if (has_body && content_length > 0) {
        if (req.size() > body_start) {
            body = req.substr(body_start);
        }

        // Read remaining body bytes if incomplete
        while (body.size() < content_length) {
            char buf[4096];
            ssize_t n = recv(client_fd, buf,
                            std::min(sizeof(buf), content_length - body.size()),
                            0);
            if (n <= 0) {
                close(client_fd);
                return;
            }
            body.append(buf, n);
        }
    }

    std::cerr << "[DEBUG] BODY SIZE COLLECTED = " << body.size() << "\n";


    // If Host header missing, extract from absolute URI
    if (host.empty()) {
        auto p1 = target.find("://");
        if (p1 != std::string::npos) {
            auto start = p1 + 3;
            auto slash = target.find('/', start);
            std::string hostport = (slash == std::string::npos) ? target.substr(start) : target.substr(start, slash - start);
            auto pos = hostport.find(':');
            if (pos != std::string::npos) {
                host = hostport.substr(0, pos);
                port = std::stoi(hostport.substr(pos + 1));
            } else {
                host = hostport;
            }
        }
    }

    if (host.empty()) {
        send_http_error(client_fd, 400, "Bad Request");
        close(client_fd);
        return;
    }

    std::cout << "HOST SEEN: [" << host << "]\n";

    if (is_blocked(host)) {
        std::cout << "HOST IS BLOCKED\n";
        const char* resp =
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send_all(client_fd, resp, strlen(resp));
        log_event(client_ip,
          client_port,
          request_line,
          host,
          port,
          "BLOCKED",
          403,
          0);
        std::cerr << "REQUEST BLOCKED for host=" << host << "\n";
        close(client_fd);
        return;
    }

    // Resolve host
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);
    int gai = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0 || res == nullptr) {
        send_http_error(client_fd, 502, "Bad Gateway");
        close(client_fd);
        if (res) freeaddrinfo(res);
        return;
    }

    int server_fd = -1;
    struct addrinfo *rp;
    for (rp = res; rp != nullptr; rp = rp->ai_next) {
        server_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (server_fd < 0) continue;
        if (connect(server_fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(server_fd);
        server_fd = -1;
    }

    freeaddrinfo(res);

    if (server_fd < 0) {
        send_http_error(client_fd, 502, "Bad Gateway");
        close(client_fd);
        return;
    }

    // Build a request for origin server:
    // convert absolute URI to path if needed
    std::string path = "/";
    auto posproto = target.find("://");
    if (posproto != std::string::npos) {
        auto start = posproto + 3;
        auto slash = target.find('/', start);
        if (slash != std::string::npos) path = target.substr(slash);
        else path = "/";
    } else {
        // target already a path
        path = target;
    }

    std::ostringstream out;
    out << method << " " << path << " " << version << "\r\n";

    // Re-add Host header (with port if non-standard)
    if (port == 80) {
        out << "Host: " << host << "\r\n";
    } else {
        out << "Host: " << host << ":" << port << "\r\n";
    }

    // Add other headers, skipping connection/proxy-connection
    // Keep Content-Length if provided (we parsed it above)
    for (auto &h : headers) {
        if (h.empty()) continue;
        std::string low = h;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c){ return std::tolower(c); });
        if (low.find("connection:") == 0) continue;
        if (low.find("proxy-connection:") == 0) continue;
        out << h << "\r\n";
    }

    // Force close
    out << "Connection: close\r\n\r\n";
    std::string forward_req = out.str();

    // Debug: show what we will send to server (headers only)
    std::cerr << "=== FORWARDING TO SERVER (headers) ===\n" << forward_req << "=== END HEADERS ===\n";

    // Send headers
    std::cerr << "SENDING REQUEST HEADERS TO SERVER\n";
    if (send_all(server_fd, forward_req.c_str(), forward_req.size()) < 0) {
        std::cerr << "ERROR: Failed to send headers to server\n";
        close(server_fd);
        close(client_fd);
        return;
    }
    std::cerr << "SENT REQUEST HEADERS TO SERVER\n";

    // Send body if present (POST/PUT)
    if (has_body && content_length > 0) {
        std::cerr << "SENDING BODY TO SERVER (" << body.size() << " bytes)\n";
        if (send_all(server_fd, body.data(), body.size()) < 0) {
            std::cerr << "ERROR: Failed to send body to server\n";
            close(server_fd);
            close(client_fd);
            return;
        }
        std::cerr << "SENT BODY TO SERVER\n";
    }

    // STEP 4: Relay response from server to client (streaming)
    std::cerr << "STARTING RESPONSE RELAY FROM SERVER\n";

    char buffer[8192];
    while (true) {
        ssize_t n = recv(server_fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv from server failed");
            break;
        } else if (n == 0) {
            std::cerr << "[DEBUG] Server closed connection\n";
            break;
        } else {
            std::cerr << "[DEBUG] Forwarding " << n << " bytes from server to client\n";
            if (send_all(client_fd, buffer, (size_t)n) < 0) {
                perror("send to client failed");
                break;
            }
            bytes_to_client += n;
        }
    }

    std::cerr << "RESPONSE RELAY FINISHED\n";

    log_event(client_ip,
          client_port,
          request_line,
          host,
          port,
          "ALLOWED",
          200,
          bytes_to_client);
    std::cerr << "REQUEST COMPLETE: " << method << " " << host << "\n";

    close(server_fd);
    close(client_fd);
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    server_fd_global = server_fd;
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen");
        return 1;
    }

    std::cout << "Proxy listening on port 8888\n";
    load_blocked("./config/blocked_domains.txt");
    std::cout << "Blocked entries loaded: " << blocked.size() << "\n";

    log_file.open("logs/proxy.log", std::ios::app);

    while (running) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &len);
        if (client_fd < 0) {
            if (!running) break;
            perror("accept");
            continue;
        }

        std::thread(handle_client, client_fd).detach();
    }

    if (server_fd_global != -1) close(server_fd_global);
    std::cout << "Proxy shut down cleanly\n";
    log_file.close();

    return 0;
}
