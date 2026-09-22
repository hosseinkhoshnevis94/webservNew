#ifndef CLIENT_HPP
#define CLIENT_HPP

// ============================================================================
// Client.hpp - The state of ONE connected browser/user.
//
// Because a single poll() loop juggles many clients in tiny fragments, we need
// somewhere to remember each client's progress between loop iterations:
//   - the request being parsed
//   - the response being sent (and how many bytes have gone out)
//   - any CGI script running for this client
//   - the last time they did something (for timeouts)
//
// Think of a Client as a "sticky note" the loop reads to remember where it
// left off with this particular user.
// ============================================================================

#include <string>
#include <ctime>

class Request;
class Response;
struct ServerConfig;

class Client {
public:
	Client();
	Client(int fd, const ServerConfig *config, int listenFd);  // the real constructor used per connection
	Client(const Client &other);
	Client &operator=(const Client &other);
	~Client();

	// ---- getters / basic info ----
	int					getFd() const;         // this client's socket
	int					getListenFd() const;   // which listening socket they arrived on
	const ServerConfig	*getConfig() const;    // which server config applies to them
	void				setConfig(const ServerConfig *config);  // switch config after Host header is known
	Request				&getRequest();         // the request being parsed
	Response			&getResponse();        // the response being built
	time_t				getLastActivity() const;
	bool				isResponseReady() const;         // is the answer ready to send?
	const std::string	&getSendBuffer() const;          // the response text
	size_t				getBytesSent() const;            // how much we've already sent

	// ---- state changes ----
	void	updateActivity();                    // reset the "last active" clock
	void	setResponseReady(bool ready);
	void	setSendBuffer(const std::string &buf);
	void	addBytesSent(size_t n);
	bool	hasTimedOut(time_t now, time_t timeout) const;  // idle too long?
	bool	shouldKeepAlive() const;             // reuse the connection after replying?
	void	reset();                             // wipe state to serve another request (keep-alive)

	// ---- CGI state (only used when a script runs for this client) ----
	bool	isCGIRunning() const;
	void	setCGIRunning(bool running);
	int		getCGIPid() const;                   // process id of the script
	void	setCGIPid(int pid);
	int		getCGIFd() const;                    // pipe we read the script's output from
	void	setCGIFd(int fd);
	std::string	&getCGIOutput();                 // accumulated script output
	time_t	getCGIStartTime() const;             // used to detect a hung script
	void	setCGIStartTime(time_t t);

	// ---- CGI write pipe (sending the request body into the script) ----
	int		getCGIWriteFd() const;
	void	setCGIWriteFd(int fd);
	const std::string	&getCGIWriteBuffer() const;
	void	setCGIWriteBuffer(const std::string &buf);
	size_t	getCGIWriteOffset() const;
	void	addCGIWriteOffset(size_t n);
	bool	hasCGIWritePending() const;

private:
	int					_fd;             // client socket
	int					_listenFd;       // listening socket they came from
	const ServerConfig	*_config;        // the applicable server config
	Request				*_request;       // heap-allocated request parser
	Response			*_response;      // heap-allocated response builder
	time_t				_lastActivity;   // for idle timeout
	bool				_responseReady;  // true once we have something to send
	std::string			_sendBuffer;     // the full response text
	size_t				_bytesSent;      // progress through _sendBuffer

	// CGI bookkeeping
	bool				_cgiRunning;
	int					_cgiPid;
	int					_cgiFd;          // read end (script -> us)
	std::string			_cgiOutput;
	time_t				_cgiStartTime;
	int					_cgiWriteFd;     // write end (us -> script)
	std::string			_cgiWriteBuffer;
	size_t				_cgiWriteOffset;
};

#endif
