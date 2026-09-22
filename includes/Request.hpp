#ifndef REQUEST_HPP
#define REQUEST_HPP

// ============================================================================
// Request.hpp - Understanding what the browser asked for.
//
// An HTTP request is just formatted text, e.g.:
//     GET /index.html HTTP/1.1
//     Host: localhost
//     <blank line>
//     <optional body>
//
// This class parses that text into structured data (method, path, headers,
// body). Because data arrives in pieces over a non-blocking socket, it works
// as a STATE MACHINE: each time new bytes arrive we call feed() and it
// continues parsing from where it stopped.
// ============================================================================

#include <string>
#include <map>

class Request {
public:
	Request();
	Request(const Request &other);
	Request &operator=(const Request &other);
	~Request();

	// The stages the parser moves through as data arrives.
	enum ParseState {
		PARSING_REQUEST_LINE,  // reading the first line: "GET /path HTTP/1.1"
		PARSING_HEADERS,       // reading "Header: value" lines
		PARSING_BODY,          // reading a fixed-length body (Content-Length)
		PARSING_CHUNKED,       // reading a chunked-encoded body
		PARSE_COMPLETE,        // fully parsed and ready
		PARSE_ERROR            // the request was malformed
	};

	void		feed(const std::string &data);  // add newly received bytes and keep parsing
	bool		isComplete() const;             // has the whole request arrived?
	bool		hasError() const;               // was there a parse error?
	int			getErrorCode() const;           // which HTTP error (e.g. 400, 413)

	// ---- accessors for the parsed pieces ----
	const std::string					&getMethod() const;   // GET / POST / DELETE ...
	const std::string					&getUri() const;      // raw URI incl. query
	const std::string					&getPath() const;     // cleaned path (no query)
	const std::string					&getQuery() const;    // the part after '?'
	const std::string					&getVersion() const;  // HTTP/1.0 or HTTP/1.1
	const std::string					&getBody() const;     // the request body
	const std::map<std::string, std::string>	&getHeaders() const;      // all headers
	std::string						getHeader(const std::string &key) const;  // one header (case-insensitive)
	size_t							getContentLength() const;
	bool							isChunked() const;
	void							setMaxBodySize(size_t size);  // enforce client_max_body_size

private:
	ParseState						_state;         // current parser stage
	std::string						_raw;           // unparsed bytes received so far
	std::string						_method;        // parsed method
	std::string						_uri;           // parsed raw URI
	std::string						_path;          // parsed + normalized path
	std::string						_query;         // parsed query string
	std::string						_version;       // parsed HTTP version
	std::map<std::string, std::string>	_headers;   // parsed headers
	std::string						_body;          // parsed body
	size_t							_contentLength; // expected body length
	bool							_chunked;       // is the body chunked?
	size_t							_maxBodySize;   // limit for the body
	int								_errorCode;     // set when _state == PARSE_ERROR

	void	_parseRequestLine();  // parse the first line
	void	_parseHeaders();      // parse the header block
	void	_parseBody();         // parse a Content-Length body
	void	_parseChunked();      // parse a chunked body
	void	_parseUri();          // split query off + normalize + url-decode the path
};

#endif
