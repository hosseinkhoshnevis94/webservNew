#include "Webserv.hpp"

// ============================================================================
// Config.cpp - Turning the config text file into usable C++ data.
//
// The flow is:
//   parse()      -> read the file, strip comments, then tokenize + walk tokens
//   _tokenize()  -> chop the text into small pieces ("server", "{", "8080", ";")
//   _parseServer / _parseLocation / _parseListen -> interpret those tokens
//
// The result is a std::vector<ServerConfig> that the rest of the program uses.
// ============================================================================

// ---- Default values for a location block (used if the config omits them) ----
LocationConfig::LocationConfig()
	: path("/"), root(""), index("index.html"), autoindex(false),
	  redirect(""), redirectCode(0), uploadDir(""), clientMaxBody(1048576) {
	// By default a route allows all three methods.
	methods.push_back("GET");
	methods.push_back("POST");
	methods.push_back("DELETE");
}

// ---- Default values for a server block ----
ServerConfig::ServerConfig()
	: host("0.0.0.0"), port(8080), clientMaxBody(1048576),
	  root("www"), index("index.html") {}

Config::Config() {}

// Convenience constructor: build and parse in one step.
Config::Config(const std::string &filename) {
	parse(filename);
}

Config::Config(const Config &other) : _servers(other._servers), _tokens(other._tokens) {}

Config &Config::operator=(const Config &other) {
	if (this != &other) {
		_servers = other._servers;
		_tokens = other._tokens;
	}
	return *this;
}

Config::~Config() {}

// Hand the parsed servers to whoever needs them (the Server).
const std::vector<ServerConfig> &Config::getServers() const {
	return _servers;
}

// ----------------------------------------------------------------------------
// parse(): the main entry point. Reads the file, tokenizes it, walks the
// tokens building server blocks, then validates the result.
// ----------------------------------------------------------------------------
void Config::parse(const std::string &filename) {
	std::ifstream file(filename.c_str());
	if (!file.is_open())
		throw std::runtime_error("Cannot open config file: " + filename);

	// Read the file line by line, dropping everything after a '#' (comments).
	std::string content;
	std::string line;
	while (std::getline(file, line)) {
		size_t commentPos = line.find('#');
		if (commentPos != std::string::npos)
			line = line.substr(0, commentPos);
		content += line + "\n";
	}
	file.close();

	// Break the whole file into tokens we can step through.
	_tokenize(content);

	// Walk the tokens. At the top level we only expect "server { ... }".
	size_t i = 0;
	while (i < _tokens.size()) {
		if (_tokens[i] == "server") {
			++i;
			if (i < _tokens.size() && _tokens[i] == "{") {
				++i;
				_parseServer(i);
			} else {
				throw std::runtime_error("Config: Expected '{' after 'server'");
			}
		} else {
			throw std::runtime_error("Config: Unexpected token '" + _tokens[i] + "'");
		}
	}

	if (_servers.empty())
		throw std::runtime_error("Config: No server block defined");

	// ---- Validation: reject truly duplicate servers ----
	// Two server blocks may share the same host:port ONLY if they have
	// different server_names (that is how virtual hosting works). If they
	// share host:port AND a server_name (or both have none), that's a mistake.
	for (size_t a = 0; a < _servers.size(); ++a) {
		for (size_t b = a + 1; b < _servers.size(); ++b) {
			if (_servers[a].port == _servers[b].port && _servers[a].host == _servers[b].host) {
				// Same host:port — are the server_names distinct?
				if (_servers[a].serverNames.empty() && _servers[b].serverNames.empty()) {
					throw std::runtime_error("Config: Duplicate server on " +
						_servers[a].host + ":" + Utils::toStr(_servers[a].port));
				}
				for (size_t i = 0; i < _servers[a].serverNames.size(); ++i) {
					for (size_t j = 0; j < _servers[b].serverNames.size(); ++j) {
						if (_servers[a].serverNames[i] == _servers[b].serverNames[j]) {
							throw std::runtime_error("Config: Duplicate server_name '" +
								_servers[a].serverNames[i] + "' on port " +
								Utils::toStr(_servers[a].port));
						}
					}
				}
			}
		}
	}
}

// ----------------------------------------------------------------------------
// _tokenize(): split the file text into meaningful pieces.
// '{', '}', and ';' become their own tokens; everything else splits on spaces.
// Example: "listen 8080;" -> ["listen", "8080", ";"]
// ----------------------------------------------------------------------------
void Config::_tokenize(const std::string &content) {
	std::string token;
	for (size_t i = 0; i < content.size(); ++i) {
		char c = content[i];
		if (c == '{' || c == '}' || c == ';') {
			// A structural character ends the current token and is itself a token.
			if (!token.empty()) {
				_tokens.push_back(token);
				token.clear();
			}
			_tokens.push_back(std::string(1, c));
		} else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			// Whitespace just ends the current token.
			if (!token.empty()) {
				_tokens.push_back(token);
				token.clear();
			}
		} else {
			// Any other character builds up the current token.
			token += c;
		}
	}
	if (!token.empty())
		_tokens.push_back(token);
}

// ----------------------------------------------------------------------------
// _parseListen(): interpret a "listen ...;" directive.
// Accepts either "listen 8080;" or "listen 127.0.0.1:8080;".
// 'i' is passed by reference so we advance the caller's position.
// ----------------------------------------------------------------------------
void Config::_parseListen(size_t &i, ServerConfig &server) {
	if (i >= _tokens.size())
		throw std::runtime_error("Config: Expected value after 'listen'");

	std::string value = _tokens[i++];
	size_t colonPos = value.rfind(':');
	if (colonPos != std::string::npos) {
		// "host:port" form
		server.host = value.substr(0, colonPos);
		server.port = Utils::toInt(value.substr(colonPos + 1));
	} else {
		// just "port"
		server.port = Utils::toInt(value);
	}

	if (i < _tokens.size() && _tokens[i] == ";")
		++i;  // skip the trailing ';'
}

// ----------------------------------------------------------------------------
// _parseServer(): read one "server { ... }" block, filling a ServerConfig.
// ----------------------------------------------------------------------------
void Config::_parseServer(size_t &i) {
	ServerConfig server;

	// Read directives until we hit the closing '}'.
	while (i < _tokens.size() && _tokens[i] != "}") {
		std::string directive = _tokens[i++];

		if (directive == "listen") {
			_parseListen(i, server);
		} else if (directive == "server_name") {
			// Collect one or more names until ';'.
			while (i < _tokens.size() && _tokens[i] != ";") {
				server.serverNames.push_back(_tokens[i++]);
			}
			if (i < _tokens.size()) ++i; // skip ;
		} else if (directive == "root") {
			if (i < _tokens.size()) server.root = _tokens[i++];
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else if (directive == "index") {
			if (i < _tokens.size()) server.index = _tokens[i++];
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else if (directive == "client_max_body_size") {
			// Parse a size that may end in K or M (kilobytes / megabytes).
			if (i < _tokens.size()) {
				std::string val = _tokens[i++];
				size_t multiplier = 1;
				if (val[val.size() - 1] == 'M' || val[val.size() - 1] == 'm') {
					multiplier = 1048576;               // 1 MB
					val = val.substr(0, val.size() - 1);
				} else if (val[val.size() - 1] == 'K' || val[val.size() - 1] == 'k') {
					multiplier = 1024;                  // 1 KB
					val = val.substr(0, val.size() - 1);
				}
				server.clientMaxBody = Utils::toSizeT(val) * multiplier;
			}
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else if (directive == "error_page") {
			// "error_page 404 www/error/404.html;"
			int code = 0;
			std::string path;
			if (i < _tokens.size()) code = Utils::toInt(_tokens[i++]);
			if (i < _tokens.size()) path = _tokens[i++];
			server.errorPages[code] = path;
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else if (directive == "location") {
			// The path can come before the '{', e.g. "location /upload {".
			std::string locPath = "/";
			if (i < _tokens.size() && _tokens[i] != "{")
				locPath = _tokens[i++];
			if (i < _tokens.size() && _tokens[i] == "{") {
				++i;
				_parseLocation(i, server);
				server.locations.back().path = locPath;  // record which path it belongs to
			}
		} else {
			// Unknown directive: skip safely until the next ';'.
			while (i < _tokens.size() && _tokens[i] != ";")
				++i;
			if (i < _tokens.size()) ++i;
		}
	}
	if (i < _tokens.size()) ++i; // skip the closing '}'

	// If the server has no locations, add a default "/" one so it still works.
	if (server.locations.empty()) {
		LocationConfig defaultLoc;
		defaultLoc.root = server.root;
		defaultLoc.index = server.index;
		defaultLoc.clientMaxBody = server.clientMaxBody;
		server.locations.push_back(defaultLoc);
	}

	_servers.push_back(server);
}

// ----------------------------------------------------------------------------
// _parseLocation(): read one "location { ... }" block, filling a LocationConfig.
// ----------------------------------------------------------------------------
void Config::_parseLocation(size_t &i, ServerConfig &server) {
	LocationConfig loc;
	loc.path = "/";
	loc.clientMaxBody = server.clientMaxBody;  // inherit the server's limit by default
	loc.methods.clear();                       // we'll fill methods from the block (or default later)
	loc.index = server.index;                  // inherit the server's index by default

	while (i < _tokens.size() && _tokens[i] != "}") {
		std::string directive = _tokens[i++];

		if (directive == "root") {
			if (i < _tokens.size()) loc.root = _tokens[i++];
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else if (directive == "index") {
			// Allow an explicit empty index ("" or '') to mean "no index file".
			if (i < _tokens.size()) {
				std::string val = _tokens[i++];
				loc.index = (val == "\"\"" || val == "''") ? "" : val;
			}
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else if (directive == "methods" || directive == "allow_methods" || directive == "limit_except") {
			// A list of allowed methods until ';'.
			loc.methods.clear();
			while (i < _tokens.size() && _tokens[i] != ";") {
				loc.methods.push_back(_tokens[i++]);
			}
			if (i < _tokens.size()) ++i;
		} else if (directive == "autoindex") {
			// "autoindex on;" enables directory listings.
			if (i < _tokens.size()) {
				loc.autoindex = (_tokens[i] == "on");
				++i;
			}
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else if (directive == "return") {
			// "return 301 http://...;" sets up a redirect.
			if (i < _tokens.size()) {
				loc.redirectCode = Utils::toInt(_tokens[i++]);
			}
			if (i < _tokens.size() && _tokens[i] != ";") {
				loc.redirect = _tokens[i++];
			}
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else if (directive == "upload_dir" || directive == "upload_store") {
			if (i < _tokens.size()) loc.uploadDir = _tokens[i++];
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else if (directive == "cgi") {
			// "cgi .py /usr/bin/python3;" maps an extension to an interpreter.
			std::string ext, path;
			if (i < _tokens.size()) ext = _tokens[i++];
			if (i < _tokens.size()) path = _tokens[i++];
			loc.cgi[ext] = path;
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else if (directive == "client_max_body_size") {
			// Same size parsing as the server-level directive.
			if (i < _tokens.size()) {
				std::string val = _tokens[i++];
				size_t multiplier = 1;
				if (val[val.size() - 1] == 'M' || val[val.size() - 1] == 'm') {
					multiplier = 1048576;
					val = val.substr(0, val.size() - 1);
				} else if (val[val.size() - 1] == 'K' || val[val.size() - 1] == 'k') {
					multiplier = 1024;
					val = val.substr(0, val.size() - 1);
				}
				loc.clientMaxBody = Utils::toSizeT(val) * multiplier;
			}
			if (i < _tokens.size() && _tokens[i] == ";") ++i;
		} else {
			// Unknown directive: skip to the next ';'.
			while (i < _tokens.size() && _tokens[i] != ";")
				++i;
			if (i < _tokens.size()) ++i;
		}
	}
	if (i < _tokens.size()) ++i; // skip the closing '}'

	// If no methods were listed, default to allowing all three.
	if (loc.methods.empty()) {
		loc.methods.push_back("GET");
		loc.methods.push_back("POST");
		loc.methods.push_back("DELETE");
	}

	// If the location didn't set its own root, inherit the server's.
	if (loc.root.empty())
		loc.root = server.root;

	server.locations.push_back(loc);
}
