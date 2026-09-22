#ifndef RESPONSE_HPP
#define RESPONSE_HPP

// ============================================================================
// Response.hpp - Building the answer we send back.
//
// Once we understand the request, this class decides WHAT to send and formats
// it as a valid HTTP response. It handles:
//   - serving static files
//   - directory listings (autoindex) and default index files
//   - redirects
//   - file uploads (POST) and deletes (DELETE)
//   - error pages (custom or default)
//   - flagging when a CGI script must run instead
// ============================================================================

#include <string>
#include <map>

class Request;
struct ServerConfig;
struct LocationConfig;

class Response {
public:
	Response();
	Response(const Response &other);
	Response &operator=(const Response &other);
	~Response();

	void		build(const Request &req, const ServerConfig &config);  // main decision routine
	std::string	getResponse() const;         // the finished HTTP text to send
	bool		needsCGI() const;            // does this request need a script run?
	const std::string	&getCGIPath() const;        // path to the CGI script
	const std::string	&getCGIExecutable() const;  // interpreter (e.g. python3)
	const std::string	&getFilePath() const;

	void		setCGIResponse(const std::string &cgiOutput);        // turn script output into HTTP
	void		setErrorResponse(int code, const ServerConfig &config);  // build an error page response

private:
	int								_statusCode;    // e.g. 200, 404
	std::map<std::string, std::string>	_headers;   // response headers
	std::string						_body;          // response body
	std::string						_response;      // the fully-assembled raw HTTP text
	bool							_needsCGI;      // set true if a script must run
	std::string						_cgiPath;       // script file path
	std::string						_cgiExecutable; // interpreter path
	std::string						_filePath;      // resolved file on disk

	// Find the best-matching location block for a URL path (longest prefix wins).
	const LocationConfig	*_matchLocation(const std::string &path, const ServerConfig &config);
	// Per-method handlers:
	void					_handleGET(const Request &req, const ServerConfig &config, const LocationConfig &loc);
	void					_handlePOST(const Request &req, const ServerConfig &config, const LocationConfig &loc);
	void					_handleDELETE(const Request &req, const ServerConfig &config, const LocationConfig &loc);
	void					_buildResponse();  // assemble status line + headers + body into _response
	std::string				_generateAutoindex(const std::string &path, const std::string &uri);  // directory listing HTML
	std::string				_getErrorPage(int code, const ServerConfig &config);  // custom or default error page
	bool					_isMethodAllowed(const std::string &method, const LocationConfig &loc);
	bool					_isCGI(const std::string &path, const LocationConfig &loc);  // does the extension map to a CGI?
};

#endif
