# HTTP Proxy Server – Design Document

## Architecture
Client → Proxy Server → Origin Server  
The proxy listens on port 8888, parses HTTP requests, applies filtering rules,
forwards allowed requests, and relays responses back to clients.

## Components
- Listener: Accepts incoming TCP connections
- Request Parser: Extracts method, target, headers, body
- Filter Engine: Blocks requests using blocked_domains.txt
- Forwarder: Sends requests to destination server
- Logger: Writes structured logs to logs/proxy.log
- Tunnel Handler: Handles HTTPS via CONNECT

## Concurrency Model
Thread-per-connection using std::thread.
Each client connection is handled independently for simplicity.

## Data Flow
1. Client connects to proxy
2. Proxy reads request headers/body
3. Host extracted and checked against blocklist
4. If allowed, proxy connects to server
5. Request forwarded
6. Response streamed back to client
7. Event logged

## Error Handling
- 400 for malformed requests
- 403 for blocked domains
- 502 for DNS/connect failures
- Graceful socket closure on errors

## Limitations
- No HTTP/2 support
- No TLS interception
- No caching enabled

## Security Considerations
- No request modification
- No TLS inspection
- Basic input size limits applied
