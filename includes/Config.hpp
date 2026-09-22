#ifndef CONFIG_HPP
#define CONFIG_HPP

// ============================================================================
// Config.hpp - Reading and storing the configuration file.
//
// A web server needs to know WHAT to serve and HOW. That information lives in
// a text config file (like NGINX's). This file defines:
//   - LocationConfig : rules for one URL path (e.g. "/upload")
//   - ServerConfig   : one "server { }" block (one website / one port)
//   - Config         : the parser that reads the file into those structs
// ============================================================================

#include <string>
#include <vector>
#include <map>

// ----------------------------------------------------------------------------
// LocationConfig: the rules for a single route/URL path inside a server.
// Example: a "location /upload { ... }" block becomes one of these.
// ----------------------------------------------------------------------------
struct LocationConfig {
	std::string					path;         // the URL prefix this applies to, e.g. "/upload"
	std::string					root;         // folder on disk where files live
	std::string					index;        // default file to serve for a directory (e.g. index.html)
	std::vector<std::string>	methods;      // which HTTP methods are allowed here (GET/POST/DELETE)
	bool						autoindex;    // if true, show a directory listing when no index file
	std::string					redirect;     // if set, redirect requests to this URL
	int							redirectCode; // the redirect status code, e.g. 301
	std::string					uploadDir;    // where uploaded files are saved
	std::map<std::string, std::string>	cgi;  // maps a file extension -> interpreter, e.g. ".py" -> "/usr/bin/python3"
	size_t						clientMaxBody; // max allowed request body size (bytes) for this route

	LocationConfig();  // constructor sets sensible defaults
};

// ----------------------------------------------------------------------------
// ServerConfig: one "server { }" block = one virtual website.
// ----------------------------------------------------------------------------
struct ServerConfig {
	std::string					host;          // IP to bind to, e.g. "0.0.0.0" (all interfaces)
	int							port;          // TCP port to listen on, e.g. 8080
	std::vector<std::string>	serverNames;   // domain names for virtual hosting (e.g. "example.com")
	std::map<int, std::string>	errorPages;    // custom error pages: 404 -> "www/error/404.html"
	size_t						clientMaxBody; // default max body size for this server
	std::string					root;          // default root folder
	std::string					index;         // default index file
	std::vector<LocationConfig>	locations;     // all the "location { }" blocks in this server

	ServerConfig();  // constructor sets sensible defaults
};

// ----------------------------------------------------------------------------
// Config: reads the config file and produces a list of ServerConfig objects.
// ----------------------------------------------------------------------------
class Config {
public:
	Config();
	Config(const std::string &filename);          // convenience: construct + parse in one step
	Config(const Config &other);
	Config &operator=(const Config &other);
	~Config();

	void	parse(const std::string &filename);    // main entry: read + validate the file
	const std::vector<ServerConfig>	&getServers() const;  // hand the parsed servers to the Server

private:
	std::vector<ServerConfig>	_servers;   // the final parsed result
	std::vector<std::string>	_tokens;    // the file broken into small pieces ("tokens")

	void	_tokenize(const std::string &content);            // split file text into tokens
	void	_parseServer(size_t &i);                          // parse one "server { }" block
	void	_parseLocation(size_t &i, ServerConfig &server);  // parse one "location { }" block
	void	_parseListen(size_t &i, ServerConfig &server);    // parse a "listen host:port;" line
};

#endif
