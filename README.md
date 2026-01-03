# Network Proxy Server (HTTP/HTTPS)

## Overview

This project implements a **multithreaded HTTP/HTTPS proxy server** in C++ using POSIX sockets.
The proxy listens for client connections, parses HTTP requests, applies filtering rules, forwards allowed traffic to destination servers, and relays responses back to clients.

The project was built with a strong focus on **correctness, robustness, and clarity**, following all required deliverables specified in the assignment. Optional extensions were intentionally avoided to keep the implementation stable and easy to reason about.

---

## Features Implemented

### 1. Core Proxy Functionality

* Listens on a configurable TCP port (default: `8888`)
* Accepts connections from multiple clients
* Forwards HTTP requests to destination servers
* Streams server responses back to clients without buffering entire responses

### 2. Concurrency Model

* Uses a **thread-per-connection** model
* Each client connection is handled independently using `std::thread`
* Supports multiple concurrent clients without blocking

### 3. HTTP Request Handling

* Parses HTTP request line and headers
* Supports:

  * `GET`
  * `HEAD`
  * `POST`
* Correctly handles:

  * Absolute URIs (`http://host/path`)
  * Relative paths (`/path`)
  * `Host` header extraction
  * `Content-Length` and request bodies for POST requests

### 4. HTTPS Support (CONNECT Method)

* Implements HTTPS tunneling using the `CONNECT` method
* Establishes a TCP tunnel between client and destination server
* Forwards encrypted data bidirectionally
* Does **not** inspect or modify TLS traffic

### 5. Domain Blocking / Filtering

* Supports domain blocking using a configuration file:

  ```
  config/blocked_domains.txt
  ```
* Blocks both HTTP and HTTPS (CONNECT) requests
* Returns:

  ```
  HTTP/1.1 403 Forbidden
  ```
* Logs all blocked requests

### 6. Logging and Metrics

All requests are logged to:

```
logs/proxy.log
```

Each log entry includes:

* Timestamp
* Client IP address
* HTTP method
* Requested host
* Action taken (`ALLOWED` / `BLOCKED`)
* HTTP status code
* Bytes transferred

### 7. Error Handling

* Gracefully handles malformed requests
* Returns appropriate HTTP error responses:

  * `400 Bad Request`
  * `403 Forbidden`
  * `502 Bad Gateway`
* Ensures stable server behavior under invalid input

---

## Project Structure

```
.
├── src/
│   └── main.cpp
├── config/
│   ├── blocked_domains.txt
│   └── server.conf
├── logs/
│   └── proxy.log
├── tests/
│   ├── README.md
│   ├── test_*.txt
│   └── sample_proxy.log
├── docs/
│   └── design.md
├── Makefile
└── README.md
```

---

## Build Instructions

### Requirements

* Linux / Ubuntu
* `g++` with C++17 support
* POSIX-compliant environment

### Compile

```bash
make
```

### Run

```bash
./proxy
```

The proxy listens on port `8888` by default.

---

## Usage Examples

### HTTP GET

```bash
curl -x localhost:8888 http://example.com
```

### HTTP POST

```bash
curl -x localhost:8888 -d "a=10&b=20" http://httpbin.org/post
```

### HTTPS (CONNECT)

```bash
curl -x localhost:8888 https://example.com
```

### Blocking Test

Add a domain to `config/blocked_domains.txt`:

```
example.com
```

Then:

```bash
curl -x localhost:8888 http://example.com
```

---

## Testing

All required test cases and expected outputs are documented in the `tests/` directory.

Test coverage includes:

* HTTP GET and HEAD requests
* POST request body forwarding
* HTTPS CONNECT tunneling
* Domain blocking
* Concurrent client handling
* Log verification

Sample logs generated from real executions are included as test artifacts.

---

## Demo Video and Screenshots

A short demo video and screenshots are included in the repository to demonstrate correct behavior and usage of the proxy server.

### Demo Video
**Location:**
```
docs/demo_video.mp4
```
### Screenshots

Screenshots capture:

* Proxy server startup
* Successful GET and POST requests
* 403 Forbidden response for blocked domains
* HTTPS CONNECT tunneling
* Concurrent request handling
* Contents of `proxy.log`

**Location:**

```
docs/screenshots/
```

---

## Design Notes

* **Concurrency:** Thread-per-connection chosen for simplicity and clarity
* **Streaming:** Responses are streamed directly to clients
* **Security:** TLS traffic is tunneled without inspection
* **Limitations:**
  * No caching (optional feature)
  * No chunked transfer decoding
  * No HTTP/2 support

These limitations are documented and acceptable within the project scope.

---

## Conclusion

This project provides a complete and correct implementation of an HTTP/HTTPS proxy server with filtering, logging, concurrency, and robust request handling.

