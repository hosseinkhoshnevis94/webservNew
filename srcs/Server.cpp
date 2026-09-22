#include "Webserv.hpp"

// ============================================================================
// Server.cpp - The engine: sockets + the single poll() loop.
//
// This is where connections are accepted, requests are read, responses are
// sent, and CGI pipes are pumped -- all driven by ONE poll() call so the
// server never blocks and handles many clients at once.
// ============================================================================

// Global "keep running" flag. 'volatile' tells the compiler it can change at
// any moment (a signal handler flips it), so don't optimize away checks of it.
static volatile bool g_serverRunning = true;

// Ctrl+C handler: ask the loop to stop after the current iteration.
static void handleSignal(int sig) {
	(void)sig;
	g_serverRunning = false;
}

Server::Server() {}

// Build the server from a parsed Config.
Server::Server(const Config &config) {
	_configs = config.getServers();       // copy the parsed server blocks
	signal(SIGINT, handleSignal);          // Ctrl+C -> graceful stop
	signal(SIGPIPE, SIG_IGN);              // never die from a broken pipe
	_setupSockets();                       // create/bind/listen all sockets
}

Server::Server(const Server &other)
	: _configs(other._configs), _listenFds(other._listenFds),
	  _fdToConfigs(other._fdToConfigs), _pollfds(other._pollfds) {}

Server &Server::operator=(const Server &other) {
	if (this != &other) {
		_configs = other._configs;
		_listenFds = other._listenFds;
		_fdToConfigs = other._fdToConfigs;
		_pollfds = other._pollfds;
	}
	return *this;
}

// Destructor: close every socket and free every client so nothing leaks.
Server::~Server() {
	for (size_t i = 0; i < _listenFds.size(); ++i)
		close(_listenFds[i]);
	std::map<int, Client*>::iterator it;
	for (it = _clients.begin(); it != _clients.end(); ++it) {
		close(it->first);
		delete it->second;
	}
}

// ----------------------------------------------------------------------------
// _setupSockets(): open one listening socket per unique host:port.
// Multiple server blocks that share a host:port (virtual hosts) share a socket.
// ----------------------------------------------------------------------------
void Server::_setupSockets() {
	// Group configs by "host:port" so shared ports use one socket.
	std::map<std::string, std::vector<ServerConfig*> > groups;
	for (size_t i = 0; i < _configs.size(); ++i) {
		std::string key = _configs[i].host + ":" + Utils::toStr(_configs[i].port);
		groups[key].push_back(&_configs[i]);
	}

	std::map<std::string, std::vector<ServerConfig*> >::iterator git;
	for (git = groups.begin(); git != groups.end(); ++git) {
		ServerConfig *first = git->second[0];

		// 1) Create a TCP socket.
		int sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd == -1)
			throw std::runtime_error("Failed to create socket");

		// Allow immediate reuse of the port after a restart (no "address in use").
		int optval = 1;
		setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

		// Make it non-blocking so accept() never freezes the loop.
		Utils::setNonBlocking(sockfd);

		// Fill in the address (which IP + port to listen on).
		struct sockaddr_in addr;
		std::memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(first->port);   // htons = host-to-network byte order
		if (first->host == "0.0.0.0" || first->host.empty())
			addr.sin_addr.s_addr = INADDR_ANY;                 // all interfaces
		else
			addr.sin_addr.s_addr = inet_addr(first->host.c_str());

		// 2) Bind the socket to that address/port.
		if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
			close(sockfd);
			throw std::runtime_error("Failed to bind to " + git->first);
		}

		// 3) Start listening for incoming connections.
		if (listen(sockfd, SOMAXCONN) == -1) {
			close(sockfd);
			throw std::runtime_error("Failed to listen on " + git->first);
		}

		// 4) Remember it and add it to the poll() watch list (watch for POLLIN).
		_listenFds.push_back(sockfd);
		_fdToConfigs[sockfd] = git->second;
		_addPollFd(sockfd, POLLIN);

		std::cout << "Listening on " << first->host << ":" << first->port << std::endl;
	}
}

// ----------------------------------------------------------------------------
// _resolveConfig(): pick the right virtual host based on the "Host" header.
// If no server_name matches, fall back to the first config on that socket.
// ----------------------------------------------------------------------------
const ServerConfig *Server::_resolveConfig(int listenFd, const std::string &host) {
	std::map<int, std::vector<ServerConfig*> >::iterator it = _fdToConfigs.find(listenFd);
	if (it == _fdToConfigs.end() || it->second.empty())
		return NULL;

	// The Host header may look like "example.com:8080"; strip the port.
	std::string hostname = host;
	size_t colonPos = hostname.find(':');
	if (colonPos != std::string::npos)
		hostname = hostname.substr(0, colonPos);

	// Look for a config whose server_name matches the requested hostname.
	for (size_t i = 0; i < it->second.size(); ++i) {
		const std::vector<std::string> &names = it->second[i]->serverNames;
		for (size_t j = 0; j < names.size(); ++j) {
			if (names[j] == hostname)
				return it->second[i];
		}
	}

	// No match -> use the first (default) config for this socket.
	return it->second[0];
}

// ----------------------------------------------------------------------------
// run(): THE main loop. One poll() watches every socket and pipe. We only act
// on descriptors poll() reports as ready. This is the core of the project.
// ----------------------------------------------------------------------------
void Server::run() {
	while (g_serverRunning) {
		// Wait up to 1000ms for any watched fd to become ready.
		int ret = poll(&_pollfds[0], _pollfds.size(), 1000);
		if (ret == -1) {
			if (!g_serverRunning)
				break;       // interrupted by our shutdown signal
			continue;        // otherwise just try again
		}

		// Check every watched fd to see what happened (revents).
		for (size_t i = 0; i < _pollfds.size(); ++i) {
			if (_pollfds[i].revents == 0)
				continue;   // nothing happened on this fd

			int fd = _pollfds[i].fd;

			if (_isListenFd(fd)) {
				// A listening socket is ready -> a new client is knocking.
				if (_pollfds[i].revents & POLLIN)
					_acceptConnection(fd);
			} else {
				// Is this the write-end of a CGI pipe (us -> script stdin)?
				int cgiWriteClientFd = _findClientByCGIWriteFd(fd);
				if (cgiWriteClientFd != -1) {
					if (_pollfds[i].revents & (POLLOUT | POLLERR | POLLHUP))
						_handleCGIWrite(fd, cgiWriteClientFd);
				} else {
					// Is this the read-end of a CGI pipe (script stdout -> us)?
					int clientFd = _findClientByCGIFd(fd);
					if (clientFd != -1) {
						if (_pollfds[i].revents & (POLLIN | POLLHUP))
							_handleCGIRead(fd, clientFd);
					} else {
						// Otherwise it's a normal client socket.
						if (_pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
							_removeClient(fd);   // error/hangup -> drop them
						} else {
							if (_pollfds[i].revents & POLLIN)
								_handleRead(fd);     // data to read
							if (_pollfds[i].revents & POLLOUT) {
								// The client may have been removed during read;
								// check it still exists before writing.
								if (_clients.find(fd) != _clients.end())
									_handleWrite(fd);  // ready to send
							}
						}
					}
				}
			}
		}
		// Once per loop, drop idle clients and kill hung CGI scripts.
		_checkTimeouts();
	}
}

// ----------------------------------------------------------------------------
// _acceptConnection(): a listening socket is ready; accept the new client.
// ----------------------------------------------------------------------------
void Server::_acceptConnection(int listenFd) {
	struct sockaddr_in clientAddr;
	socklen_t addrLen = sizeof(clientAddr);
	int clientFd = accept(listenFd, (struct sockaddr*)&clientAddr, &addrLen);
	if (clientFd == -1)
		return;   // nothing to accept right now; that's fine

	// Safety cap on concurrent clients.
	if (_clients.size() >= MAX_CLIENTS) {
		close(clientFd);
		return;
	}

	Utils::setNonBlocking(clientFd);

	// Start with the first config on this socket; we'll refine it once we read
	// the Host header (for virtual hosting).
	std::map<int, std::vector<ServerConfig*> >::iterator it = _fdToConfigs.find(listenFd);
	const ServerConfig *config = it->second[0];
	Client *client = new Client(clientFd, config, listenFd);
	_clients[clientFd] = client;
	_addPollFd(clientFd, POLLIN);   // watch for the request to arrive
}

// ----------------------------------------------------------------------------
// _handleRead(): poll said this client has data; receive and parse it.
// ----------------------------------------------------------------------------
void Server::_handleRead(int clientFd) {
	std::map<int, Client*>::iterator it = _clients.find(clientFd);
	if (it == _clients.end())
		return;

	Client *client = it->second;
	char buffer[BUFFER_SIZE];
	ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer), 0);

	// IMPORTANT (subject rule): check both 0 and <0, and never inspect errno.
	if (bytesRead == 0) {
		_removeClient(clientFd);   // client closed the connection
		return;
	}
	if (bytesRead < 0) {
		_removeClient(clientFd);   // read error -> drop the client
		return;
	}

	client->updateActivity();
	// Feed the bytes to the request parser (it handles partial data).
	client->getRequest().feed(std::string(buffer, bytesRead));

	// Once the request is fully parsed (or errored), build the response.
	if (client->getRequest().isComplete() || client->getRequest().hasError()) {
		// Choose the correct virtual host based on the Host header.
		std::string host = client->getRequest().getHeader("Host");
		if (!host.empty()) {
			const ServerConfig *resolved = _resolveConfig(client->getListenFd(), host);
			if (resolved && resolved != client->getConfig())
				client->setConfig(resolved);
		}
		_processRequest(client);
	}
}

// ----------------------------------------------------------------------------
// _processRequest(): decide the response. If it's static, it's ready to send.
// If it needs a CGI script, launch it and wire its pipes into poll().
// ----------------------------------------------------------------------------
void Server::_processRequest(Client *client) {
	client->getResponse().build(client->getRequest(), *client->getConfig());

	if (client->getResponse().needsCGI()) {
		// Launch the script.
		CGI cgi;
		int result = cgi.execute(
			client->getResponse().getCGIPath(),
			client->getResponse().getCGIExecutable(),
			client->getRequest(),
			*client->getConfig()
		);

		if (result == -1) {
			// Could not start the script -> 500 error.
			client->getResponse().setErrorResponse(500, *client->getConfig());
			client->setSendBuffer(client->getResponse().getResponse());
			client->setResponseReady(true);
		} else {
			// Remember the running script and watch its output pipe.
			client->setCGIRunning(true);
			client->setCGIPid(cgi.getPid());
			client->setCGIFd(cgi.getPipeFd());
			client->setCGIStartTime(time(NULL));
			_addPollFd(cgi.getPipeFd(), POLLIN);

			// If there's a request body, we need to write it into the script.
			const std::string &body = client->getRequest().getBody();
			if (!body.empty()) {
				client->setCGIWriteFd(cgi.getWritePipeFd());
				client->setCGIWriteBuffer(body);
				_addPollFd(cgi.getWritePipeFd(), POLLOUT);
			} else {
				// No body -> close the write pipe so the script sees EOF.
				close(cgi.getWritePipeFd());
				client->setCGIWriteFd(-1);
			}
		}
	} else {
		// Static response is ready immediately.
		client->setSendBuffer(client->getResponse().getResponse());
		client->setResponseReady(true);
	}

	// If we have something to send now, switch this client to write mode.
	if (client->isResponseReady()) {
		for (size_t i = 0; i < _pollfds.size(); ++i) {
			if (_pollfds[i].fd == client->getFd()) {
				_pollfds[i].events = POLLOUT;
				break;
			}
		}
	}
}

// ----------------------------------------------------------------------------
// _handleCGIRead(): collect a script's output. On EOF, the script is done.
// ----------------------------------------------------------------------------
void Server::_handleCGIRead(int cgiFd, int clientFd) {
	std::map<int, Client*>::iterator it = _clients.find(clientFd);
	if (it == _clients.end())
		return;

	Client *client = it->second;
	char buffer[BUFFER_SIZE];
	ssize_t bytesRead = read(cgiFd, buffer, sizeof(buffer));

	if (bytesRead > 0) {
		// Accumulate output and wait for more.
		client->getCGIOutput().append(buffer, bytesRead);
		return;
	}

	if (bytesRead < 0) {
		// Pipe not ready yet; poll will notify us again.
		return;
	}

	// bytesRead == 0 -> EOF -> the script finished writing.
	_removePollFd(cgiFd);
	close(cgiFd);

	// Reap the child process so it doesn't become a zombie.
	int status;
	waitpid(client->getCGIPid(), &status, 0);

	client->setCGIRunning(false);
	client->setCGIFd(-1);

	// Turn the script's output into a proper HTTP response.
	client->getResponse().setCGIResponse(client->getCGIOutput());
	client->setSendBuffer(client->getResponse().getResponse());
	client->setResponseReady(true);

	// Switch the client to write mode.
	for (size_t i = 0; i < _pollfds.size(); ++i) {
		if (_pollfds[i].fd == client->getFd()) {
			_pollfds[i].events = POLLOUT;
			break;
		}
	}
}

// ----------------------------------------------------------------------------
// _handleWrite(): poll said the client socket can accept data; send some.
// Large responses take several calls, so we track how much we've sent.
// ----------------------------------------------------------------------------
void Server::_handleWrite(int clientFd) {
	std::map<int, Client*>::iterator it = _clients.find(clientFd);
	if (it == _clients.end())
		return;

	Client *client = it->second;
	if (!client->isResponseReady())
		return;

	const std::string &buf = client->getSendBuffer();
	size_t remaining = buf.size() - client->getBytesSent();
	if (remaining == 0) {
		// Nothing left to send. Reuse the connection (keep-alive) or close it.
		if (client->shouldKeepAlive()) {
			client->reset();
			for (size_t i = 0; i < _pollfds.size(); ++i) {
				if (_pollfds[i].fd == client->getFd()) {
					_pollfds[i].events = POLLIN;  // wait for the next request
					break;
				}
			}
		} else {
			_removeClient(clientFd);
		}
		return;
	}

	// Send the next chunk.
	ssize_t sent = send(clientFd, buf.c_str() + client->getBytesSent(), remaining, 0);
	// Same subject rule: check both 0 and <0, never look at errno.
	if (sent == 0) {
		_removeClient(clientFd);
		return;
	}
	if (sent < 0) {
		_removeClient(clientFd);
		return;
	}

	client->addBytesSent(sent);
	client->updateActivity();

	// Did we finish sending everything?
	if (client->getBytesSent() >= buf.size()) {
		if (client->shouldKeepAlive()) {
			client->reset();
			for (size_t i = 0; i < _pollfds.size(); ++i) {
				if (_pollfds[i].fd == client->getFd()) {
					_pollfds[i].events = POLLIN;
					break;
				}
			}
		} else {
			_removeClient(clientFd);
		}
	}
}

// ----------------------------------------------------------------------------
// _removeClient(): fully clean up a client -- kill any CGI, close fds, free it.
// ----------------------------------------------------------------------------
void Server::_removeClient(int fd) {
	std::map<int, Client*>::iterator it = _clients.find(fd);
	if (it != _clients.end()) {
		Client *client = it->second;
		// If a CGI script is still running for this client, kill and reap it.
		if (client->isCGIRunning()) {
			kill(client->getCGIPid(), SIGKILL);
			waitpid(client->getCGIPid(), NULL, 0);
			if (client->getCGIFd() != -1) {
				_removePollFd(client->getCGIFd());
				close(client->getCGIFd());
			}
		}
		// Close the CGI write pipe if it's still open.
		if (client->getCGIWriteFd() != -1) {
			_removePollFd(client->getCGIWriteFd());
			close(client->getCGIWriteFd());
		}
		delete client;        // free the Client (and its Request/Response)
		_clients.erase(it);   // remove from the map
	}
	_removePollFd(fd);        // stop watching this socket
	close(fd);                // close the socket
}

// ----------------------------------------------------------------------------
// _checkTimeouts(): drop clients idle too long; kill CGI scripts that hang.
// This is what keeps the server healthy and prevents "hanging connections".
// ----------------------------------------------------------------------------
void Server::_checkTimeouts() {
	time_t now = time(NULL);
	std::vector<int> toRemove;     // idle clients to drop
	std::vector<int> cgiTimeout;   // clients whose CGI ran too long

	// First pass: find who has timed out (don't modify the map while iterating).
	std::map<int, Client*>::iterator it;
	for (it = _clients.begin(); it != _clients.end(); ++it) {
		if (it->second->isCGIRunning()) {
			if ((now - it->second->getCGIStartTime()) > TIMEOUT_SEC) {
				cgiTimeout.push_back(it->first);
			}
		} else if (it->second->hasTimedOut(now, TIMEOUT_SEC)) {
			toRemove.push_back(it->first);
		}
	}

	// Handle hung CGIs: kill the script and respond with 504 Gateway Timeout.
	for (size_t i = 0; i < cgiTimeout.size(); ++i) {
		Client *client = _clients[cgiTimeout[i]];
		kill(client->getCGIPid(), SIGKILL);
		waitpid(client->getCGIPid(), NULL, 0);
		if (client->getCGIFd() != -1) {
			_removePollFd(client->getCGIFd());
			close(client->getCGIFd());
		}
		if (client->getCGIWriteFd() != -1) {
			_removePollFd(client->getCGIWriteFd());
			close(client->getCGIWriteFd());
			client->setCGIWriteFd(-1);
		}
		client->setCGIRunning(false);
		client->setCGIFd(-1);
		client->getResponse().setErrorResponse(504, *client->getConfig());
		client->setSendBuffer(client->getResponse().getResponse());
		client->setResponseReady(true);
		for (size_t j = 0; j < _pollfds.size(); ++j) {
			if (_pollfds[j].fd == client->getFd()) {
				_pollfds[j].events = POLLOUT;   // switch to sending the 504
				break;
			}
		}
	}

	// Drop the plain idle clients.
	for (size_t i = 0; i < toRemove.size(); ++i)
		_removeClient(toRemove[i]);
}

// Add an fd to the poll() watch list with the events we care about.
void Server::_addPollFd(int fd, short events) {
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;
	_pollfds.push_back(pfd);
}

// Remove an fd from the poll() watch list.
void Server::_removePollFd(int fd) {
	for (size_t i = 0; i < _pollfds.size(); ++i) {
		if (_pollfds[i].fd == fd) {
			_pollfds.erase(_pollfds.begin() + i);
			return;
		}
	}
}

// Given a CGI read-pipe fd, find which client it belongs to (-1 if none).
int Server::_findClientByCGIFd(int cgiFd) {
	std::map<int, Client*>::iterator it;
	for (it = _clients.begin(); it != _clients.end(); ++it) {
		if (it->second->isCGIRunning() && it->second->getCGIFd() == cgiFd)
			return it->first;
	}
	return -1;
}

// Given a CGI write-pipe fd, find which client it belongs to (-1 if none).
int Server::_findClientByCGIWriteFd(int fd) {
	std::map<int, Client*>::iterator it;
	for (it = _clients.begin(); it != _clients.end(); ++it) {
		if (it->second->getCGIWriteFd() == fd)
			return it->first;
	}
	return -1;
}

// ----------------------------------------------------------------------------
// _handleCGIWrite(): feed the request body into the script's stdin, a chunk
// at a time. When done (or on error), close the write pipe so the script sees
// EOF and can start producing output.
// ----------------------------------------------------------------------------
void Server::_handleCGIWrite(int cgiFd, int clientFd) {
	std::map<int, Client*>::iterator it = _clients.find(clientFd);
	if (it == _clients.end())
		return;

	Client *client = it->second;
	const std::string &buf = client->getCGIWriteBuffer();
	size_t remaining = buf.size() - client->getCGIWriteOffset();

	if (remaining == 0) {
		// All body bytes written -> close the pipe (signals EOF to the script).
		_removePollFd(cgiFd);
		close(cgiFd);
		client->setCGIWriteFd(-1);
		return;
	}

	ssize_t written = write(cgiFd, buf.c_str() + client->getCGIWriteOffset(), remaining);
	if (written <= 0) {
		// Pipe closed or error -> stop writing.
		_removePollFd(cgiFd);
		close(cgiFd);
		client->setCGIWriteFd(-1);
		return;
	}

	client->addCGIWriteOffset(written);
	if (client->getCGIWriteOffset() >= buf.size()) {
		// Finished writing the whole body.
		_removePollFd(cgiFd);
		close(cgiFd);
		client->setCGIWriteFd(-1);
	}
}

// Is this fd one of our listening sockets?
bool Server::_isListenFd(int fd) {
	for (size_t i = 0; i < _listenFds.size(); ++i) {
		if (_listenFds[i] == fd)
			return true;
	}
	return false;
}
