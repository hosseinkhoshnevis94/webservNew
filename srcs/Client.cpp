#include "Webserv.hpp"

// ============================================================================
// Client.cpp - The per-connection state object.
//
// One Client exists per connected browser. It remembers the request being
// parsed, the response being sent, and any CGI running -- so the single loop
// can pick up where it left off each time this client becomes ready.
//
// Because it owns heap memory (_request, _response via new), it carefully
// implements the copy constructor, assignment operator, and destructor
// (the "Rule of Three") to avoid double-frees and leaks.
// ============================================================================

// Default constructor: an empty, unused client.
Client::Client()
	: _fd(-1), _listenFd(-1), _config(NULL), _request(NULL), _response(NULL),
	  _lastActivity(time(NULL)), _responseReady(false), _bytesSent(0),
	  _cgiRunning(false), _cgiPid(-1), _cgiFd(-1), _cgiStartTime(0),
	  _cgiWriteFd(-1), _cgiWriteOffset(0) {}

// The real constructor used when we accept() a new connection.
Client::Client(int fd, const ServerConfig *config, int listenFd)
	: _fd(fd), _listenFd(listenFd), _config(config), _lastActivity(time(NULL)),
	  _responseReady(false), _bytesSent(0),
	  _cgiRunning(false), _cgiPid(-1), _cgiFd(-1), _cgiStartTime(0),
	  _cgiWriteFd(-1), _cgiWriteOffset(0) {
	_request = new Request();
	_response = new Response();
	// Tell the parser how big a body this server allows (client_max_body_size).
	_request->setMaxBodySize(config->clientMaxBody);
}

// Copy constructor: deep-copy the request/response so two clients never share
// (and later double-free) the same objects.
Client::Client(const Client &other)
	: _fd(other._fd), _listenFd(other._listenFd), _config(other._config),
	  _lastActivity(other._lastActivity), _responseReady(other._responseReady),
	  _sendBuffer(other._sendBuffer), _bytesSent(other._bytesSent),
	  _cgiRunning(other._cgiRunning), _cgiPid(other._cgiPid),
	  _cgiFd(other._cgiFd), _cgiOutput(other._cgiOutput),
	  _cgiStartTime(other._cgiStartTime),
	  _cgiWriteFd(other._cgiWriteFd), _cgiWriteBuffer(other._cgiWriteBuffer),
	  _cgiWriteOffset(other._cgiWriteOffset) {
	_request = other._request ? new Request(*other._request) : NULL;
	_response = other._response ? new Response(*other._response) : NULL;
}

// Assignment operator: free our old objects, then deep-copy the other's.
Client &Client::operator=(const Client &other) {
	if (this != &other) {
		delete _request;
		delete _response;
		_fd = other._fd;
		_listenFd = other._listenFd;
		_config = other._config;
		_lastActivity = other._lastActivity;
		_responseReady = other._responseReady;
		_sendBuffer = other._sendBuffer;
		_bytesSent = other._bytesSent;
		_cgiRunning = other._cgiRunning;
		_cgiPid = other._cgiPid;
		_cgiFd = other._cgiFd;
		_cgiOutput = other._cgiOutput;
		_cgiStartTime = other._cgiStartTime;
		_cgiWriteFd = other._cgiWriteFd;
		_cgiWriteBuffer = other._cgiWriteBuffer;
		_cgiWriteOffset = other._cgiWriteOffset;
		_request = other._request ? new Request(*other._request) : NULL;
		_response = other._response ? new Response(*other._response) : NULL;
	}
	return *this;
}

// Destructor: free the heap-allocated request/response.
Client::~Client() {
	delete _request;
	delete _response;
}

// ---- Simple getters ----
int Client::getFd() const { return _fd; }
int Client::getListenFd() const { return _listenFd; }
const ServerConfig *Client::getConfig() const { return _config; }

// Switch to a different server config (used once the Host header picks a
// virtual host). Also update the body-size limit to match the new config.
void Client::setConfig(const ServerConfig *config) {
	_config = config;
	if (_request && config)
		_request->setMaxBodySize(config->clientMaxBody);
}

Request &Client::getRequest() { return *_request; }
Response &Client::getResponse() { return *_response; }
time_t Client::getLastActivity() const { return _lastActivity; }
bool Client::isResponseReady() const { return _responseReady; }
const std::string &Client::getSendBuffer() const { return _sendBuffer; }
size_t Client::getBytesSent() const { return _bytesSent; }

// ---- State changes ----
void Client::updateActivity() { _lastActivity = time(NULL); }  // reset idle clock
void Client::setResponseReady(bool ready) { _responseReady = ready; }
// Setting a new send buffer resets the sent counter to 0.
void Client::setSendBuffer(const std::string &buf) { _sendBuffer = buf; _bytesSent = 0; }
void Client::addBytesSent(size_t n) { _bytesSent += n; }

// Has this client been idle longer than the allowed timeout?
bool Client::hasTimedOut(time_t now, time_t timeout) const {
	return (now - _lastActivity) > timeout;
}

// Decide whether to keep the connection open after responding.
// HTTP/1.1: keep open unless the client said "Connection: close".
// HTTP/1.0: close unless the client said "Connection: keep-alive".
bool Client::shouldKeepAlive() const {
	if (!_request)
		return false;
	std::string connection = _request->getHeader("Connection");
	if (_request->getVersion() == "HTTP/1.1")
		return Utils::toLower(connection) != "close";
	return Utils::toLower(connection) == "keep-alive";
}

// Reset everything so the SAME connection can serve another request
// (this is what makes keep-alive work).
void Client::reset() {
	delete _request;
	delete _response;
	_request = new Request();
	_response = new Response();
	if (_config)
		_request->setMaxBodySize(_config->clientMaxBody);
	_responseReady = false;
	_sendBuffer.clear();
	_bytesSent = 0;
	_cgiRunning = false;
	_cgiPid = -1;
	_cgiFd = -1;
	_cgiOutput.clear();
	_cgiWriteFd = -1;
	_cgiWriteBuffer.clear();
	_cgiWriteOffset = 0;
	_lastActivity = time(NULL);
}

// ---- CGI state accessors (used by the Server while a script runs) ----
bool Client::isCGIRunning() const { return _cgiRunning; }
void Client::setCGIRunning(bool running) { _cgiRunning = running; }
int Client::getCGIPid() const { return _cgiPid; }
void Client::setCGIPid(int pid) { _cgiPid = pid; }
int Client::getCGIFd() const { return _cgiFd; }
void Client::setCGIFd(int fd) { _cgiFd = fd; }
std::string &Client::getCGIOutput() { return _cgiOutput; }
time_t Client::getCGIStartTime() const { return _cgiStartTime; }
void Client::setCGIStartTime(time_t t) { _cgiStartTime = t; }

// ---- CGI write-pipe accessors (sending the request body into the script) ----
int Client::getCGIWriteFd() const { return _cgiWriteFd; }
void Client::setCGIWriteFd(int fd) { _cgiWriteFd = fd; }
const std::string &Client::getCGIWriteBuffer() const { return _cgiWriteBuffer; }
// Setting a new write buffer resets the write offset to 0.
void Client::setCGIWriteBuffer(const std::string &buf) { _cgiWriteBuffer = buf; _cgiWriteOffset = 0; }
size_t Client::getCGIWriteOffset() const { return _cgiWriteOffset; }
void Client::addCGIWriteOffset(size_t n) { _cgiWriteOffset += n; }
// Is there still body left to write into the script?
bool Client::hasCGIWritePending() const { return _cgiWriteFd != -1 && _cgiWriteOffset < _cgiWriteBuffer.size(); }
