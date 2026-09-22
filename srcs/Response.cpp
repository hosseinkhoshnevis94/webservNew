#include "Webserv.hpp"

// ============================================================================
// Response.cpp - Deciding WHAT to send and formatting it as valid HTTP.
//
// build() is the router: it inspects the request, picks the matching location,
// checks the method, and dispatches to the GET/POST/DELETE handler (or flags a
// CGI run). Helpers assemble the final "HTTP/1.1 ... headers ... body" text.
// ============================================================================

// A fresh response defaults to 200 OK and no CGI.
Response::Response() : _statusCode(200), _needsCGI(false) {}

Response::Response(const Response &other)
	: _statusCode(other._statusCode), _headers(other._headers), _body(other._body),
	  _response(other._response), _needsCGI(other._needsCGI),
	  _cgiPath(other._cgiPath), _cgiExecutable(other._cgiExecutable),
	  _filePath(other._filePath) {}

Response &Response::operator=(const Response &other) {
	if (this != &other) {
		_statusCode = other._statusCode;
		_headers = other._headers;
		_body = other._body;
		_response = other._response;
		_needsCGI = other._needsCGI;
		_cgiPath = other._cgiPath;
		_cgiExecutable = other._cgiExecutable;
		_filePath = other._filePath;
	}
	return *this;
}

Response::~Response() {}

// ----------------------------------------------------------------------------
// build(): the main decision tree that turns a request into a response.
// ----------------------------------------------------------------------------
void Response::build(const Request &req, const ServerConfig &config) {
	// 1) If the parser flagged an error (bad request, too large, etc.), send it.
	if (req.hasError()) {
		setErrorResponse(req.getErrorCode(), config);
		return;
	}

	// 2) Find the location block that best matches this URL path.
	const LocationConfig *loc = _matchLocation(req.getPath(), config);
	if (!loc) {
		setErrorResponse(404, config);   // no route matched
		return;
	}

	// 3) If this location is a redirect, send the redirect and stop.
	if (!loc->redirect.empty()) {
		_statusCode = loc->redirectCode ? loc->redirectCode : 301;
		_headers["Location"] = loc->redirect;
		_body = "";
		_buildResponse();
		return;
	}

	// 4) Method checks.
	const std::string &method = req.getMethod();
	// Not a real HTTP method at all -> 501 Not Implemented.
	if (method != "GET" && method != "POST" && method != "DELETE"
		&& method != "HEAD" && method != "PUT" && method != "PATCH"
		&& method != "OPTIONS" && method != "TRACE" && method != "CONNECT") {
		setErrorResponse(501, config);
		return;
	}

	// A valid method, but not allowed on this route -> 405 with an Allow header
	// that lists which methods ARE permitted (required by the HTTP spec).
	if (!_isMethodAllowed(method, *loc)) {
		_statusCode = 405;
		std::string allow;
		for (size_t i = 0; i < loc->methods.size(); ++i) {
			if (i > 0) allow += ", ";
			allow += loc->methods[i];
		}
		_headers["Allow"] = allow;
		_body = _getErrorPage(405, config);
		_headers["Content-Type"] = "text/html";
		_buildResponse();
		return;
	}

	// 5) Dispatch to the right handler.
	if (method == "GET" || method == "HEAD")
		_handleGET(req, config, *loc);
	else if (method == "POST")
		_handlePOST(req, config, *loc);
	else if (method == "DELETE")
		_handleDELETE(req, config, *loc);
	else
		setErrorResponse(501, config);

	// 6) A HEAD request wants the same headers as GET but NO body, so strip it.
	if (method == "HEAD" && !_needsCGI) {
		size_t headerEnd = _response.find("\r\n\r\n");
		if (headerEnd != std::string::npos)
			_response = _response.substr(0, headerEnd + 4);
	}
}

std::string Response::getResponse() const {
	return _response;
}

bool Response::needsCGI() const { return _needsCGI; }
const std::string &Response::getCGIPath() const { return _cgiPath; }
const std::string &Response::getCGIExecutable() const { return _cgiExecutable; }
const std::string &Response::getFilePath() const { return _filePath; }

// ----------------------------------------------------------------------------
// setCGIResponse(): turn a CGI script's raw output into a proper HTTP response.
// The script prints its own headers, a blank line, then the body.
// ----------------------------------------------------------------------------
void Response::setCGIResponse(const std::string &cgiOutput) {
	// A script that produced nothing at all is treated as a failure -> 500.
	if (cgiOutput.empty()) {
		_statusCode = 500;
		_body = "<html><head><title>500 Internal Server Error</title></head>\n"
				"<body><center><h1>500 Internal Server Error</h1></center>\n"
				"<hr><center>webserv/1.0</center></body></html>\n";
		_headers["Content-Type"] = "text/html";
		_buildResponse();
		return;
	}

	// Split the script output into its header block and body.
	// (Support both \r\n\r\n and \n\n separators.)
	size_t headerEnd = cgiOutput.find("\r\n\r\n");
	if (headerEnd == std::string::npos)
		headerEnd = cgiOutput.find("\n\n");

	std::string headers;
	if (headerEnd != std::string::npos) {
		headers = cgiOutput.substr(0, headerEnd);
		size_t bodyStart = headerEnd + (cgiOutput[headerEnd + 1] == '\n' ? 2 : 4);
		_body = cgiOutput.substr(bodyStart);
	} else {
		// No header block found: treat everything as the body.
		_body = cgiOutput;
	}

	_statusCode = 200;

	// Parse the script's headers. A "Status:" header lets the script set the code.
	std::istringstream iss(headers);
	std::string line;
	while (std::getline(iss, line)) {
		if (!line.empty() && line[line.size() - 1] == '\r')
			line = line.substr(0, line.size() - 1);   // strip trailing CR
		size_t colonPos = line.find(':');
		if (colonPos != std::string::npos) {
			std::string key = Utils::trim(line.substr(0, colonPos));
			std::string value = Utils::trim(line.substr(colonPos + 1));
			if (Utils::toLower(key) == "status") {
				_statusCode = Utils::toInt(value);
			} else {
				_headers[key] = value;
			}
		}
	}

	// Make sure there's always a Content-Type.
	if (_headers.find("Content-Type") == _headers.end())
		_headers["Content-Type"] = "text/html";

	_buildResponse();
}

// Build an error response (using a custom page if the config provides one).
void Response::setErrorResponse(int code, const ServerConfig &config) {
	_statusCode = code;
	_body = _getErrorPage(code, config);
	_headers["Content-Type"] = "text/html";
	_buildResponse();
}

// ----------------------------------------------------------------------------
// _matchLocation(): pick the location whose path is the longest prefix of the
// request path. So "/upload/img" beats "/upload" beats "/".
// ----------------------------------------------------------------------------
const LocationConfig *Response::_matchLocation(const std::string &path, const ServerConfig &config) {
	const LocationConfig *best = NULL;
	size_t bestLen = 0;

	for (size_t i = 0; i < config.locations.size(); ++i) {
		const std::string &locPath = config.locations[i].path;
		if (path.find(locPath) == 0) {          // path starts with locPath
			if (locPath.size() > bestLen) {     // keep the longest match
				bestLen = locPath.size();
				best = &config.locations[i];
			}
		}
	}
	return best;
}

// Is this method allowed on this location? HEAD is treated like GET.
bool Response::_isMethodAllowed(const std::string &method, const LocationConfig &loc) {
	std::string checkMethod = method;
	if (method == "HEAD")
		checkMethod = "GET";
	for (size_t i = 0; i < loc.methods.size(); ++i) {
		if (loc.methods[i] == checkMethod)
			return true;
	}
	return false;
}

// Does this file's extension map to a CGI interpreter in this location?
bool Response::_isCGI(const std::string &path, const LocationConfig &loc) {
	std::string ext = Utils::getExtension(path);
	return loc.cgi.find(ext) != loc.cgi.end();
}

// ----------------------------------------------------------------------------
// _handleGET(): serve a file, a directory listing, or hand off to CGI.
// ----------------------------------------------------------------------------
void Response::_handleGET(const Request &req, const ServerConfig &config, const LocationConfig &loc) {
	// Turn the URL path into a real path on disk:
	//   strip the location prefix, then join with the location's root folder.
	std::string reqPath = req.getPath();
	std::string relativePath = reqPath.substr(loc.path.size());
	if (!relativePath.empty() && relativePath[0] == '/')
		relativePath = relativePath.substr(1);

	std::string fullPath = loc.root;
	if (!fullPath.empty() && fullPath[fullPath.size() - 1] != '/')
		fullPath += "/";
	fullPath += relativePath;

	// If the target is a directory:
	if (Utils::isDirectory(fullPath)) {
		// Browsers expect directory URLs to end with '/'. If not, redirect so
		// that relative links inside the page resolve correctly.
		if (reqPath[reqPath.size() - 1] != '/') {
			_statusCode = 301;
			_headers["Location"] = reqPath + "/";
			_body = "";
			_buildResponse();
			return;
		}

		if (!fullPath.empty() && fullPath[fullPath.size() - 1] != '/')
			fullPath += "/";

		// Prefer the configured index file (e.g. index.html) if it exists.
		if (!loc.index.empty()) {
			std::string indexPath = fullPath + loc.index;
			if (Utils::fileExists(indexPath)) {
				fullPath = indexPath;               // serve the index file
			} else if (loc.autoindex) {
				// No index file, but directory listing is enabled.
				_statusCode = 200;
				_body = _generateAutoindex(fullPath, req.getPath());
				_headers["Content-Type"] = "text/html";
				_buildResponse();
				return;
			} else {
				setErrorResponse(403, config);       // listing disabled -> forbidden
				return;
			}
		} else if (loc.autoindex) {
			// No index configured, but listing is on.
			_statusCode = 200;
			_body = _generateAutoindex(fullPath, req.getPath());
			_headers["Content-Type"] = "text/html";
			_buildResponse();
			return;
		} else {
			setErrorResponse(403, config);
			return;
		}
	}

	// Not a directory: the file must exist.
	if (!Utils::fileExists(fullPath)) {
		setErrorResponse(404, config);
		return;
	}

	// If the file is a CGI script, flag it so the Server runs it.
	if (_isCGI(fullPath, loc)) {
		std::string ext = Utils::getExtension(fullPath);
		_needsCGI = true;
		_cgiPath = fullPath;
		_cgiExecutable = loc.cgi.find(ext)->second;
		_filePath = fullPath;
		return;
	}

	// Otherwise, serve the static file with the right Content-Type.
	_body = Utils::readFileContent(fullPath);
	_statusCode = 200;
	std::string ext = Utils::getExtension(fullPath);
	_headers["Content-Type"] = Utils::getMimeType(ext);
	_buildResponse();
}

// ----------------------------------------------------------------------------
// _handlePOST(): run CGI, save an upload, or accept the POST with 200.
// ----------------------------------------------------------------------------
void Response::_handlePOST(const Request &req, const ServerConfig &config, const LocationConfig &loc) {
	// Same URL-to-disk-path logic as GET.
	std::string reqPath = req.getPath();
	std::string relativePath = reqPath.substr(loc.path.size());
	if (!relativePath.empty() && relativePath[0] == '/')
		relativePath = relativePath.substr(1);

	std::string fullPath = loc.root;
	if (!fullPath.empty() && fullPath[fullPath.size() - 1] != '/')
		fullPath += "/";
	fullPath += relativePath;

	// If the target is a CGI script, hand off (its stdin gets the body).
	if (_isCGI(fullPath, loc)) {
		std::string ext = Utils::getExtension(fullPath);
		_needsCGI = true;
		_cgiPath = fullPath;
		_cgiExecutable = loc.cgi.find(ext)->second;
		_filePath = fullPath;
		return;
	}

	// If this location accepts uploads, save the incoming file(s).
	if (!loc.uploadDir.empty()) {
		std::string contentType = req.getHeader("Content-Type");
		std::string body = req.getBody();

		std::string uploadPath = loc.uploadDir;
		if (uploadPath[uploadPath.size() - 1] != '/')
			uploadPath += "/";

		// --- Case A: multipart/form-data (how browsers upload files) ---
		if (contentType.find("multipart/form-data") != std::string::npos) {
			// The Content-Type header tells us the boundary marker between parts.
			size_t boundaryPos = contentType.find("boundary=");
			if (boundaryPos == std::string::npos) {
				setErrorResponse(400, config);
				return;
			}
			std::string boundary = "--" + contentType.substr(boundaryPos + 9);

			// Walk each part between boundaries.
			size_t pos = body.find(boundary);
			while (pos != std::string::npos) {
				size_t nextPos = body.find(boundary, pos + boundary.size());
				if (nextPos == std::string::npos)
					break;

				std::string part = body.substr(pos + boundary.size(), nextPos - pos - boundary.size());

				// Extract the filename from this part's headers.
				size_t filenamePos = part.find("filename=\"");
				if (filenamePos != std::string::npos) {
					filenamePos += 10;   // skip past 'filename="'
					size_t filenameEnd = part.find("\"", filenamePos);
					std::string filename = part.substr(filenamePos, filenameEnd - filenamePos);

					if (!filename.empty()) {
						// The file content begins after the part's blank line.
						size_t contentStart = part.find("\r\n\r\n");
						if (contentStart != std::string::npos) {
							contentStart += 4;
							std::string fileContent = part.substr(contentStart);
							// Trim the trailing CRLF that precedes the next boundary.
							if (fileContent.size() >= 2 && fileContent.substr(fileContent.size() - 2) == "\r\n")
								fileContent = fileContent.substr(0, fileContent.size() - 2);

							// Write the file to disk.
							std::string filePath = uploadPath + filename;
							std::ofstream ofs(filePath.c_str(), std::ios::binary);
							if (ofs.is_open()) {
								ofs.write(fileContent.c_str(), fileContent.size());
								ofs.close();
							} else {
								setErrorResponse(500, config);
								return;
							}
						}
					}
				}
				pos = nextPos;
			}

			_statusCode = 201;   // Created
			_body = "<html><body><h1>File Uploaded Successfully</h1></body></html>";
			_headers["Content-Type"] = "text/html";
			_buildResponse();
			return;
		}

		// --- Case B: a raw body upload (e.g. curl --data) ---
		// Save it under a generated filename.
		std::string filename = "upload_" + Utils::toStr(static_cast<int>(time(NULL)));
		std::string filePath = uploadPath + filename;
		std::ofstream ofs(filePath.c_str(), std::ios::binary);
		if (ofs.is_open()) {
			ofs.write(body.c_str(), body.size());
			ofs.close();
			_statusCode = 201;
			_body = "<html><body><h1>File Uploaded Successfully</h1></body></html>";
			_headers["Content-Type"] = "text/html";
			_buildResponse();
		} else {
			setErrorResponse(500, config);
		}
		return;
	}

	// No CGI and no upload configured: just accept the POST with a simple 200.
	_statusCode = 200;
	_body = "<html><body><h1>OK</h1></body></html>";
	_headers["Content-Type"] = "text/html";
	_buildResponse();
}

// ----------------------------------------------------------------------------
// _handleDELETE(): remove a file from disk.
// ----------------------------------------------------------------------------
void Response::_handleDELETE(const Request &req, const ServerConfig &config, const LocationConfig &loc) {
	// Same URL-to-disk-path logic.
	std::string reqPath = req.getPath();
	std::string relativePath = reqPath.substr(loc.path.size());
	if (!relativePath.empty() && relativePath[0] == '/')
		relativePath = relativePath.substr(1);

	std::string fullPath = loc.root;
	if (!fullPath.empty() && fullPath[fullPath.size() - 1] != '/')
		fullPath += "/";
	fullPath += relativePath;

	if (!Utils::fileExists(fullPath)) {
		setErrorResponse(404, config);   // nothing to delete
		return;
	}

	if (Utils::isDirectory(fullPath)) {
		setErrorResponse(403, config);   // we don't delete directories
		return;
	}

	// std::remove deletes the file; non-zero means it failed.
	if (std::remove(fullPath.c_str()) != 0) {
		setErrorResponse(500, config);
		return;
	}

	_statusCode = 200;
	_body = "<html><body><h1>File Deleted</h1></body></html>";
	_headers["Content-Type"] = "text/html";
	_buildResponse();
}

// ----------------------------------------------------------------------------
// _buildResponse(): assemble the final raw HTTP text: status line + headers +
// blank line + body. Adds Content-Length and Connection if not already set.
// ----------------------------------------------------------------------------
void Response::_buildResponse() {
	std::ostringstream oss;
	// Status line, e.g. "HTTP/1.1 200 OK".
	oss << "HTTP/1.1 " << _statusCode << " " << Utils::getStatusText(_statusCode) << "\r\n";
	oss << "Date: " << Utils::getDate() << "\r\n";
	oss << "Server: webserv/1.0\r\n";

	// The browser needs to know the body length.
	if (_headers.find("Content-Length") == _headers.end())
		_headers["Content-Length"] = Utils::toStr(_body.size());

	// Default to keep-alive so browsers can reuse the connection.
	if (_headers.find("Connection") == _headers.end())
		_headers["Connection"] = "keep-alive";

	// Write out all headers.
	std::map<std::string, std::string>::iterator it;
	for (it = _headers.begin(); it != _headers.end(); ++it) {
		oss << it->first << ": " << it->second << "\r\n";
	}
	// The mandatory blank line between headers and body, then the body.
	oss << "\r\n";
	oss << _body;

	_response = oss.str();
}

// ----------------------------------------------------------------------------
// _generateAutoindex(): build a simple HTML page listing a directory's files
// (used when autoindex is on and there's no index file).
// ----------------------------------------------------------------------------
std::string Response::_generateAutoindex(const std::string &path, const std::string &uri) {
	std::ostringstream html;
	html << "<html><head><title>Index of " << uri << "</title></head>\n";
	html << "<body><h1>Index of " << uri << "</h1><hr>\n";
	html << "<pre>\n";

	// Open the directory and iterate over its entries.
	DIR *dir = opendir(path.c_str());
	if (dir) {
		struct dirent *entry;
		while ((entry = readdir(dir)) != NULL) {
			std::string name = entry->d_name;
			if (name == ".")
				continue;   // skip the "current directory" entry

			// Build the link href for this entry.
			std::string link = uri;
			if (link[link.size() - 1] != '/')
				link += "/";
			link += name;

			// Append a '/' to directory names for clarity.
			std::string fullEntryPath = path + name;
			struct stat st;
			stat(fullEntryPath.c_str(), &st);
			if (S_ISDIR(st.st_mode))
				name += "/";

			html << "<a href=\"" << link << "\">" << name << "</a>\n";
		}
		closedir(dir);
	}

	html << "</pre><hr></body></html>\n";
	return html.str();
}

// ----------------------------------------------------------------------------
// _getErrorPage(): return the body for an error code -- a custom page from the
// config if available, otherwise a generated default page.
// ----------------------------------------------------------------------------
std::string Response::_getErrorPage(int code, const ServerConfig &config) {
	// Use a custom error page if the config specifies one and it loads.
	std::map<int, std::string>::const_iterator it = config.errorPages.find(code);
	if (it != config.errorPages.end()) {
		std::string content = Utils::readFileContent(it->second);
		if (!content.empty())
			return content;
	}

	// Otherwise build a plain default page.
	std::ostringstream html;
	html << "<html><head><title>" << code << " " << Utils::getStatusText(code) << "</title></head>\n";
	html << "<body><center><h1>" << code << " " << Utils::getStatusText(code) << "</h1></center>\n";
	html << "<hr><center>webserv/1.0</center></body></html>\n";
	return html.str();
}
