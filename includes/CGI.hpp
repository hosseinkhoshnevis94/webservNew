#ifndef CGI_HPP
#define CGI_HPP

// ============================================================================
// CGI.hpp - Running external scripts (CGI = Common Gateway Interface).
//
// Sometimes a page is not a static file but the OUTPUT of a program (Python,
// PHP, etc.). CGI is the standard way a web server runs such a program:
//   - fork() a child process
//   - connect pipes to the child's stdin/stdout
//   - execve() the interpreter on the script
//   - feed the request body to its stdin, read its stdout as the page
//
// This class sets that up. The actual reading/writing of the pipes is driven
// by the Server's poll() loop so the server never blocks on a slow script.
// ============================================================================

#include <string>
#include <map>

class Request;
struct ServerConfig;

class CGI {
public:
	CGI();
	CGI(const CGI &other);
	CGI &operator=(const CGI &other);
	~CGI();

	// Launch the script. Returns 0 on success, -1 on failure.
	int		execute(const std::string &scriptPath, const std::string &executable,
					const Request &req, const ServerConfig &config);
	int		getPipeFd() const;       // read end: script's stdout -> us
	int		getWritePipeFd() const;  // write end: us -> script's stdin
	int		getPid() const;          // the child process id (to wait on / kill)

private:
	int		_pipeFd;       // read end
	int		_writePipeFd;  // write end
	int		_pid;          // child pid

	// Build the CGI environment variables (REQUEST_METHOD, QUERY_STRING, etc.)
	std::map<std::string, std::string>	_buildEnv(const std::string &scriptPath,
												const Request &req, const ServerConfig &config);
	// Convert the C++ env map into the char** array execve() requires.
	char	**_envToCharArray(const std::map<std::string, std::string> &env);
	void	_freeCharArray(char **arr);  // free what _envToCharArray allocated
};

#endif
