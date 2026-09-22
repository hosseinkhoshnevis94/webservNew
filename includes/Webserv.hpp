#ifndef WEBSERV_HPP
#define WEBSERV_HPP

// ============================================================================
// Webserv.hpp - The "master include" file.
//
// Instead of every .cpp file listing all the system headers it needs, they
// simply #include "Webserv.hpp". This one file pulls in:
//   1. All the C++ standard library headers we use
//   2. All the system (POSIX) headers for sockets, files, processes
//   3. All of our own project headers (Config, Request, Response, etc.)
//
// This keeps the top of every source file clean and guarantees every file
// sees the same set of tools.
// ============================================================================

// ---- C++ standard library ----
#include <iostream>   // std::cout, std::cerr (printing to screen)
#include <string>     // std::string
#include <vector>     // std::vector (dynamic arrays)
#include <map>        // std::map (key -> value dictionary)
#include <set>        // std::set (unique collection)
#include <sstream>    // std::stringstream (build/parse strings)
#include <fstream>    // std::ifstream/ofstream (read/write files)
#include <algorithm>  // std::find, etc.
#include <cstring>    // std::memset, std::strcpy (C++ version of string.h)
#include <cstdlib>    // std::strtol, etc.
#include <cstdio>     // std::remove (delete a file)
#include <cerrno>     // errno (we avoid using it after read/write per the subject)
#include <ctime>      // time(), for timeouts and the Date header

// ---- System / POSIX headers (the networking + OS tools) ----
#include <unistd.h>        // read, write, close, fork, chdir, dup2
#include <fcntl.h>         // fcntl (used to make sockets non-blocking)
#include <sys/socket.h>    // socket, bind, listen, accept, send, recv
#include <sys/types.h>     // basic system data types
#include <sys/stat.h>      // stat (check if a path is a file or directory)
#include <sys/wait.h>      // waitpid (clean up finished CGI child processes)
#include <netinet/in.h>    // sockaddr_in (an IPv4 address + port)
#include <arpa/inet.h>     // htons, inet_addr (byte-order / address helpers)
#include <netdb.h>         // network database helpers
#include <dirent.h>        // opendir, readdir, closedir (list a directory)
#include <signal.h>        // signal (handle Ctrl+C, ignore broken pipes)
#include <poll.h>          // poll (the single I/O multiplexer at the core)

// ---- Project-wide constants ----
#define BUFFER_SIZE 65536   // How many bytes we read/recv at a time (64 KB)
#define MAX_CLIENTS 1024    // Safety cap on simultaneous connections
#define TIMEOUT_SEC 60      // Drop a client / kill a CGI after 60s of no activity

// ---- Our own project headers ----
// Order matters a little: things that others depend on come first.
#include "Utils.hpp"     // small helper functions used everywhere
#include "Config.hpp"    // parses and stores the configuration file
#include "Request.hpp"   // parses an incoming HTTP request
#include "Response.hpp"  // builds the HTTP response to send back
#include "Client.hpp"    // holds the state of one connected client
#include "CGI.hpp"       // runs external scripts (Python/PHP/etc.)
#include "Server.hpp"    // the main engine: sockets + the poll() loop

#endif
