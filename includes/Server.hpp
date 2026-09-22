#ifndef SERVER_HPP
#define SERVER_HPP

// ============================================================================
// Server.hpp - The heart of the whole program.
//
// The Server owns:
//   - the listening sockets (one per host:port)
//   - the list of connected clients
//   - the single poll() loop that drives ALL input/output
//
// The golden rule of this project: exactly ONE poll() handles everything, and
// we never read/write a socket unless poll() says it is ready. That keeps the
// server non-blocking (it never freezes waiting on a slow client).
// ============================================================================

#include <string>
#include <vector>
#include <map>
#include <poll.h>

class Config;
class Client;
struct ServerConfig;

class Server {
public:
	Server();
	Server(const Config &config);   // build the server from parsed config
	Server(const Server &other);
	Server &operator=(const Server &other);
	~Server();

	void	run();   // enter the poll() loop and serve forever

private:
	std::vector<ServerConfig>		_configs;      // all server blocks from the config
	std::vector<int>				_listenFds;    // the listening sockets
	std::map<int, std::vector<ServerConfig*> >	_fdToConfigs;  // listen socket -> the configs it serves (for virtual hosts)
	std::map<int, Client*>			_clients;      // client socket -> that client's state
	std::vector<struct pollfd>		_pollfds;      // the exact list poll() watches each loop

	void	_setupSockets();                        // create/bind/listen all sockets
	void	_acceptConnection(int listenFd);        // accept a brand-new client
	void	_handleRead(int clientFd);              // recv() request data from a client
	void	_handleWrite(int clientFd);             // send() response data to a client
	void	_handleCGIRead(int cgiFd, int clientFd);   // read a CGI script's output
	void	_handleCGIWrite(int cgiFd, int clientFd);  // write the request body into a CGI script
	void	_processRequest(Client *client);        // decide the response (or launch CGI)
	void	_removeClient(int fd);                  // close socket + free the client cleanly
	void	_checkTimeouts();                       // drop silent clients / kill hung CGIs
	void	_addPollFd(int fd, short events);       // start watching an fd in poll()
	void	_removePollFd(int fd);                  // stop watching an fd
	int		_findClientByCGIFd(int cgiFd);          // find which client owns a CGI read pipe
	int		_findClientByCGIWriteFd(int cgiFd);     // find which client owns a CGI write pipe
	bool	_isListenFd(int fd);                    // is this fd one of our listening sockets?
	const ServerConfig	*_resolveConfig(int listenFd, const std::string &host); // pick virtual host by Host header
};

#endif
