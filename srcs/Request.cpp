#include "Webserv.hpp"

// ============================================================================
// Request.cpp - Parsing an incoming HTTP request as a state machine.
//
// Data can arrive in fragments over a non-blocking socket, so we can't assume
// the whole request is available at once. feed() appends new bytes and advances
// through the stages: request line -> headers -> body (fixed or chunked).
// ============================================================================

// Fresh request: start at the request line, default 1 MB body cap, no error.
Request::Request()
	: _state(PARSING_REQUEST_LINE), _contentLength(0), _chunked(false),
	  _maxBodySize(1048576), _errorCode(0) {}

Request::Request(const Request &other)
	: _state(other._state), _raw(other._raw), _method(other._method),
	  _uri(other._uri), _path(other._path), _query(other._query),
	  _version(other._version), _headers(other._headers), _body(other._body),
	  _contentLength(other._contentLength), _chunked(other._chunked),
	  _maxBodySize(other._maxBodySize), _errorCode(other._errorCode) {}

Request &Request::operator=(const Request &other) {
	if (this != &other) {
		_state = other._state;
		_raw = other._raw;
		_method = other._method;
		_uri = other._uri;
		_path = other._path;
		_query = other._query;
		_version = other._version;
		_headers = other._headers;
		_body = other._body;
		_contentLength = other._contentLength;
		_chunked = other._chunked;
		_maxBodySize = other._maxBodySize;
		_errorCode = other._errorCode;
	}
	return *this;
}

Request::~Request() {}

// ----------------------------------------------------------------------------
// feed(): called every time new bytes arrive. Append them and parse as far as
// possible. Each 'if' tries to advance one stage; when a stage can't finish
// (needs more data) it simply returns and we resume on the next feed().
// ----------------------------------------------------------------------------
void Request::feed(const std::string &data) {
	_raw += data;

	if (_state == PARSING_REQUEST_LINE)
		_parseRequestLine();
	if (_state == PARSING_HEADERS)
		_parseHeaders();
	if (_state == PARSING_BODY)
		_parseBody();
	if (_state == PARSING_CHUNKED)
		_parseChunked();
}

bool Request::isComplete() const {
	return _state == PARSE_COMPLETE;
}

bool Request::hasError() const {
	return _state == PARSE_ERROR;
}

int Request::getErrorCode() const {
	return _errorCode;
}

// ---- Accessors for the parsed pieces ----
const std::string &Request::getMethod() const { return _method; }
const std::string &Request::getUri() const { return _uri; }
const std::string &Request::getPath() const { return _path; }
const std::string &Request::getQuery() const { return _query; }
const std::string &Request::getVersion() const { return _version; }
const std::string &Request::getBody() const { return _body; }
const std::map<std::string, std::string> &Request::getHeaders() const { return _headers; }

// Look up a header case-insensitively ("Host", "host", "HOST" are the same).
std::string Request::getHeader(const std::string &key) const {
	std::string lowerKey = Utils::toLower(key);
	std::map<std::string, std::string>::const_iterator it;
	for (it = _headers.begin(); it != _headers.end(); ++it) {
		if (Utils::toLower(it->first) == lowerKey)
			return it->second;
	}
	return "";
}

size_t Request::getContentLength() const { return _contentLength; }
bool Request::isChunked() const { return _chunked; }

// Set the maximum allowed body size (from client_max_body_size in the config).
void Request::setMaxBodySize(size_t size) { _maxBodySize = size; }

// ----------------------------------------------------------------------------
// _parseRequestLine(): parse the first line, e.g. "GET /path HTTP/1.1".
// ----------------------------------------------------------------------------
void Request::_parseRequestLine() {
	// The line ends at the first CRLF. If it's not here yet, wait for more data.
	size_t pos = _raw.find("\r\n");
	if (pos == std::string::npos)
		return;

	std::string line = _raw.substr(0, pos);
	_raw = _raw.substr(pos + 2);   // consume the line + CRLF

	// Split into the three parts.
	std::istringstream iss(line);
	iss >> _method >> _uri >> _version;

	// Validate: all three parts must be present.
	if (_method.empty() || _uri.empty() || _version.empty()) {
		_state = PARSE_ERROR;
		_errorCode = 400;   // Bad Request
		return;
	}

	// We only support HTTP/1.0 and HTTP/1.1.
	if (_version != "HTTP/1.1" && _version != "HTTP/1.0") {
		_state = PARSE_ERROR;
		_errorCode = 505;   // HTTP Version Not Supported
		return;
	}

	// Reject absurdly long URIs.
	if (_uri.size() > 8192) {
		_state = PARSE_ERROR;
		_errorCode = 414;   // URI Too Long
		return;
	}

	_parseUri();               // split query, decode, normalize
	_state = PARSING_HEADERS;  // move on to headers
}

// ----------------------------------------------------------------------------
// _parseUri(): split off the query string, URL-decode, and normalize the path
// to prevent directory traversal (e.g. "/../../etc/passwd").
// ----------------------------------------------------------------------------
void Request::_parseUri() {
	// Separate "/path" from "?query".
	size_t queryPos = _uri.find('?');
	if (queryPos != std::string::npos) {
		_path = Utils::urlDecode(_uri.substr(0, queryPos));
		_query = _uri.substr(queryPos + 1);
	} else {
		_path = Utils::urlDecode(_uri);
	}

	// Remember if the path ended with '/' so we can restore it after cleaning.
	bool hasTrailingSlash = (_path.size() > 1 && _path[_path.size() - 1] == '/');

	// Walk the path segments, applying "." (stay) and ".." (go up) safely.
	std::vector<std::string> parts = Utils::split(_path, '/');
	std::vector<std::string> stack;
	for (size_t i = 0; i < parts.size(); ++i) {
		if (parts[i] == "..") {
			if (!stack.empty()) stack.pop_back();   // go up one level (can't escape root)
		} else if (parts[i] != "." && !parts[i].empty()) {
			stack.push_back(parts[i]);
		}
	}
	// Rebuild the cleaned path.
	_path = "/";
	for (size_t i = 0; i < stack.size(); ++i) {
		_path += stack[i];
		if (i + 1 < stack.size())
			_path += "/";
	}
	// Restore a trailing slash if the original had one (matters for directories).
	if (hasTrailingSlash && _path.size() > 1)
		_path += "/";
}

// ----------------------------------------------------------------------------
// _parseHeaders(): read "Name: value" lines until the blank line that ends
// the header block, then decide how the body will arrive.
// ----------------------------------------------------------------------------
void Request::_parseHeaders() {
	while (true) {
		size_t pos = _raw.find("\r\n");
		if (pos == std::string::npos)
			return;   // no complete line yet; wait for more data

		if (pos == 0) {
			// A blank line marks the end of the headers.
			_raw = _raw.substr(2);

			// How is the body delivered?
			std::string transferEncoding = getHeader("Transfer-Encoding");
			if (Utils::toLower(transferEncoding).find("chunked") != std::string::npos) {
				// Chunked: size-prefixed pieces.
				_chunked = true;
				_state = PARSING_CHUNKED;
			} else {
				std::string cl = getHeader("Content-Length");
				if (!cl.empty()) {
					// Fixed length body.
					_contentLength = Utils::toSizeT(cl);
					// Enforce the configured maximum body size.
					if (_maxBodySize > 0 && _contentLength > _maxBodySize) {
						_state = PARSE_ERROR;
						_errorCode = 413;   // Payload Too Large
						return;
					}
					_state = PARSING_BODY;
				} else {
					// No body at all (typical GET) -> we're done.
					_state = PARSE_COMPLETE;
				}
			}
			return;
		}

		// Otherwise this is a "Name: value" header line.
		std::string line = _raw.substr(0, pos);
		_raw = _raw.substr(pos + 2);

		size_t colonPos = line.find(':');
		if (colonPos != std::string::npos) {
			std::string key = Utils::trim(line.substr(0, colonPos));
			std::string value = Utils::trim(line.substr(colonPos + 1));
			_headers[key] = value;
		}
	}
}

// ----------------------------------------------------------------------------
// _parseBody(): read a fixed-length body. Wait until we have Content-Length
// bytes, then take exactly that many as the body.
// ----------------------------------------------------------------------------
void Request::_parseBody() {
	if (_raw.size() >= _contentLength) {
		_body = _raw.substr(0, _contentLength);
		_raw = _raw.substr(_contentLength);
		_state = PARSE_COMPLETE;
	}
}

// ----------------------------------------------------------------------------
// _parseChunked(): decode a chunked body. Each chunk is "<hexsize>\r\n<data>\r\n";
// a zero-size chunk marks the end. The result is the reassembled body.
// Example: "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n" -> "hello world"
// ----------------------------------------------------------------------------
void Request::_parseChunked() {
	while (true) {
		// Find the end of the size line.
		size_t pos = _raw.find("\r\n");
		if (pos == std::string::npos)
			return;   // need more data

		// The size is written in hexadecimal.
		std::string sizeLine = _raw.substr(0, pos);
		size_t chunkSize = strtol(sizeLine.c_str(), NULL, 16);

		if (chunkSize == 0) {
			// Zero-size chunk = end of body.
			_raw = _raw.substr(pos + 2);
			if (_raw.size() >= 2)
				_raw = _raw.substr(2);   // skip the final CRLF
			_contentLength = _body.size();
			_state = PARSE_COMPLETE;
			return;
		}

		// Do we have the full chunk yet (size line + data + trailing CRLF)?
		if (_raw.size() < pos + 2 + chunkSize + 2)
			return;   // wait for more data

		// Append this chunk's data to the body.
		_body += _raw.substr(pos + 2, chunkSize);
		_raw = _raw.substr(pos + 2 + chunkSize + 2);

		// Enforce the max body size even for chunked uploads.
		if (_maxBodySize > 0 && _body.size() > _maxBodySize) {
			_state = PARSE_ERROR;
			_errorCode = 413;   // Payload Too Large
			return;
		}
	}
}
