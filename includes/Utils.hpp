#ifndef UTILS_HPP
#define UTILS_HPP

// ============================================================================
// Utils.hpp - A toolbox of small helper functions used across the project.
//
// None of these are complicated; they exist so we don't repeat the same little
// bits of code (string conversions, file checks, MIME types, etc.) everywhere.
// They live in a "namespace Utils" so we call them like Utils::trim(...).
// ============================================================================

#include <string>
#include <sstream>
#include <vector>

namespace Utils {
	std::string	trim(const std::string &str);                 // strip surrounding whitespace
	std::string	toLower(const std::string &str);              // lowercase a string
	std::string	toStr(int n);                                 // int   -> string
	std::string	toStr(size_t n);                              // size_t -> string
	int			toInt(const std::string &str);                // string -> int
	size_t		toSizeT(const std::string &str);              // string -> size_t
	std::vector<std::string> split(const std::string &str, char delim);  // split on a character
	std::string	getExtension(const std::string &path);        // "a.py" -> ".py"
	std::string	getMimeType(const std::string &extension);    // ".css" -> "text/css"
	std::string	getStatusText(int code);                      // 404 -> "Not Found"
	std::string	getDate();                                    // current time in HTTP format
	bool		fileExists(const std::string &path);          // does the path exist?
	bool		isDirectory(const std::string &path);         // is it a folder?
	std::string	readFileContent(const std::string &path);     // read a whole file into a string
	std::string	urlDecode(const std::string &str);            // "%20" -> space, "+" -> space
	void		setNonBlocking(int fd);                       // make a socket non-blocking
}

#endif
