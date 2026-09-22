#include "Webserv.hpp"

// ============================================================================
// Utils.cpp - Implementation of the small shared helper functions.
// Each function does one simple job used throughout the codebase.
// ============================================================================

namespace Utils {

// Remove leading/trailing whitespace (spaces, tabs, CR, LF).
std::string trim(const std::string &str) {
	size_t start = str.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return "";  // string was all whitespace
	size_t end = str.find_last_not_of(" \t\r\n");
	return str.substr(start, end - start + 1);
}

// Return a lowercase copy of the string (used for case-insensitive compares).
std::string toLower(const std::string &str) {
	std::string result = str;
	for (size_t i = 0; i < result.size(); ++i)
		result[i] = std::tolower(result[i]);
	return result;
}

// Convert an int to a string using a stringstream.
std::string toStr(int n) {
	std::ostringstream oss;
	oss << n;
	return oss.str();
}

// Same but for size_t (unsigned).
std::string toStr(size_t n) {
	std::ostringstream oss;
	oss << n;
	return oss.str();
}

// Convert a string to an int.
int toInt(const std::string &str) {
	std::istringstream iss(str);
	int n;
	iss >> n;
	return n;
}

// Convert a string to a size_t.
size_t toSizeT(const std::string &str) {
	std::istringstream iss(str);
	size_t n;
	iss >> n;
	return n;
}

// Split a string on a delimiter character, skipping empty pieces.
// Example: split("/a/b/", '/') -> ["a", "b"]
std::vector<std::string> split(const std::string &str, char delim) {
	std::vector<std::string> tokens;
	std::istringstream iss(str);
	std::string token;
	while (std::getline(iss, token, delim)) {
		if (!token.empty())
			tokens.push_back(token);
	}
	return tokens;
}

// Return the file extension including the dot, e.g. "test.py" -> ".py".
std::string getExtension(const std::string &path) {
	size_t pos = path.rfind('.');
	if (pos == std::string::npos)
		return "";
	return path.substr(pos);
}

// Map a file extension to its Content-Type (MIME type). This tells the browser
// how to interpret the file (render HTML, apply CSS, show an image, etc.).
std::string getMimeType(const std::string &extension) {
	// Build the table once (static) and reuse it on later calls.
	static std::map<std::string, std::string> mimeTypes;
	if (mimeTypes.empty()) {
		mimeTypes[".html"] = "text/html";
		mimeTypes[".htm"] = "text/html";
		mimeTypes[".css"] = "text/css";
		mimeTypes[".js"] = "application/javascript";
		mimeTypes[".json"] = "application/json";
		mimeTypes[".png"] = "image/png";
		mimeTypes[".jpg"] = "image/jpeg";
		mimeTypes[".jpeg"] = "image/jpeg";
		mimeTypes[".gif"] = "image/gif";
		mimeTypes[".svg"] = "image/svg+xml";
		mimeTypes[".ico"] = "image/x-icon";
		mimeTypes[".txt"] = "text/plain";
		mimeTypes[".pdf"] = "application/pdf";
		mimeTypes[".xml"] = "application/xml";
		mimeTypes[".mp3"] = "audio/mpeg";
		mimeTypes[".mp4"] = "video/mp4";
		mimeTypes[".zip"] = "application/zip";
	}
	std::map<std::string, std::string>::iterator it = mimeTypes.find(extension);
	if (it != mimeTypes.end())
		return it->second;
	// Unknown type: a safe generic default (browser will offer to download).
	return "application/octet-stream";
}

// Map an HTTP status code to its standard reason phrase.
std::string getStatusText(int code) {
	switch (code) {
		case 200: return "OK";
		case 201: return "Created";
		case 204: return "No Content";
		case 301: return "Moved Permanently";
		case 302: return "Found";
		case 303: return "See Other";
		case 307: return "Temporary Redirect";
		case 400: return "Bad Request";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 408: return "Request Timeout";
		case 413: return "Payload Too Large";
		case 414: return "URI Too Long";
		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		case 502: return "Bad Gateway";
		case 504: return "Gateway Timeout";
		case 505: return "HTTP Version Not Supported";
		default: return "Unknown";
	}
}

// Return the current time formatted the way the HTTP "Date" header expects,
// e.g. "Thu, 20 Aug 2026 15:00:00 GMT".
std::string getDate() {
	time_t now = time(0);
	struct tm *gmt = gmtime(&now);
	char buf[128];
	strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", gmt);
	return std::string(buf);
}

// Does a path exist on disk? (stat succeeds for files and directories alike.)
bool fileExists(const std::string &path) {
	struct stat st;
	return (stat(path.c_str(), &st) == 0);
}

// Is the path a directory (folder)?
bool isDirectory(const std::string &path) {
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return false;
	return S_ISDIR(st.st_mode);
}

// Read an entire file into a string (binary-safe). Empty string if it can't open.
std::string readFileContent(const std::string &path) {
	std::ifstream file(path.c_str(), std::ios::binary);
	if (!file.is_open())
		return "";
	std::ostringstream oss;
	oss << file.rdbuf();  // slurp the whole file at once
	return oss.str();
}

// Decode percent-encoding in a URL: "%20" -> space, and '+' -> space.
// Example: "a%20b+c" -> "a b c"
std::string urlDecode(const std::string &str) {
	std::string result;
	for (size_t i = 0; i < str.size(); ++i) {
		if (str[i] == '%' && i + 2 < str.size()) {
			// Take the two hex digits after '%' and turn them into a byte.
			std::string hex = str.substr(i + 1, 2);
			char c = static_cast<char>(strtol(hex.c_str(), NULL, 16));
			result += c;
			i += 2;  // skip the two hex digits we consumed
		} else if (str[i] == '+') {
			result += ' ';
		} else {
			result += str[i];
		}
	}
	return result;
}

// Put a socket into non-blocking mode so reads/writes never freeze the server.
// On macOS the subject only allows the F_SETFL / O_NONBLOCK / FD_CLOEXEC flags.
void setNonBlocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);  // read current flags
	if (flags == -1)
		flags = 0;
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);  // add the non-blocking flag
}

} // namespace Utils
