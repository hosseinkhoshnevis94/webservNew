*This project has been created as part of the 42 curriculum.*

## Description

Webserv is a lightweight HTTP/1.1 server written in C++ 98. It handles concurrent connections using non-blocking I/O driven by a single `poll()` loop, serving static files, executing CGI scripts, and processing file uploads. The server is configured via an NGINX-inspired configuration file supporting multiple virtual servers on different ports.

Key features:
- GET, POST, DELETE methods
- Chunked transfer-encoding decoding
- Multipart file upload
- Python CGI execution
- Directory listing (autoindex)
- HTTP redirections
- Custom error pages
- Keep-alive connections
- Client body size limits
- Timeout handling

## Instructions

### Compilation

```bash
make        # build
make clean  # remove object files
make fclean # remove object files and binary
make re     # full rebuild
```

Requires a C++ compiler supporting C++98 (tested with clang++ / g++).

### Execution

```bash
./webserv                    # uses conf/default.conf
./webserv conf/default.conf  # explicit config file
```

The server listens on all ports defined in the configuration file. The default config serves on ports 8080 and 8081.

### Testing

Open a browser to `http://localhost:8080` for the index page with links to test autoindex, CGI, redirects, and file upload.

```bash
# Static file
curl http://localhost:8080/

# CGI
curl http://localhost:8080/cgi-bin/test.py?hello=world

# File upload
curl -F "file=@somefile.txt" http://localhost:8080/upload

# Delete
curl -X DELETE http://localhost:8080/upload/somefile.txt

# Chunked encoding
curl -H "Transfer-Encoding: chunked" -d "hello" http://localhost:8080/cgi-bin/test.py
```

## Resources

- [RFC 2616 – HTTP/1.1](https://datatracker.ietf.org/doc/html/rfc2616)
- [RFC 3875 – CGI/1.1](https://datatracker.ietf.org/doc/html/rfc3875)
- [NGINX configuration documentation](https://nginx.org/en/docs/http/ngx_http_core_module.html)
- [Beej's Guide to Network Programming](https://beej.us/guide/bgnet/)

### AI Usage

AI tools were used to assist with boilerplate generation (canonical form methods, repetitive getter/setter implementations) and to review edge cases in HTTP parsing and CGI environment variable setup. All generated content was reviewed, tested, and adapted to fit the project's architecture.
