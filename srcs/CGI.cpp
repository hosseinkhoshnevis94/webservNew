#include "Webserv.hpp"

// ============================================================================
// CGI.cpp - Running external scripts to build a page dynamically.
//
// The technique:
//   pipe()   -> create two one-way tubes (one for stdin, one for stdout)
//   fork()   -> split into a parent (the server) and a child (the script)
//   dup2()   -> point the child's stdin/stdout at our pipes
//   chdir()  -> run in the script's own folder (so relative paths work)
//   execve() -> replace the child with the interpreter (python3, etc.)
//
// The parent then feeds the request body into the script's stdin and reads its
// stdout as the page -- all driven by the Server's poll() loop.
// ============================================================================

CGI::CGI() : _pipeFd(-1), _writePipeFd(-1), _pid(-1) {}

CGI::CGI(const CGI &other) : _pipeFd(other._pipeFd), _writePipeFd(other._writePipeFd), _pid(other._pid) {}

CGI &CGI::operator=(const CGI &other) {
	if (this != &other) {
		_pipeFd = other._pipeFd;
		_writePipeFd = other._writePipeFd;
		_pid = other._pid;
	}
	return *this;
}

CGI::~CGI() {}

// ----------------------------------------------------------------------------
// execute(): launch the script. Returns 0 on success (pipes/pid stored),
// or -1 if a pipe or fork failed.
// ----------------------------------------------------------------------------
int CGI::execute(const std::string &scriptPath, const std::string &executable,
				const Request &req, const ServerConfig &config) {
	int pipeIn[2];   // pipeIn[0] = read end, pipeIn[1] = write end (body -> script)
	int pipeOut[2];  // pipeOut[0] = read end, pipeOut[1] = write end (script -> us)

	// Create both pipes. If either fails, we can't run the CGI.
	if (pipe(pipeIn) == -1 || pipe(pipeOut) == -1)
		return -1;

	// Split into two processes.
	_pid = fork();
	if (_pid == -1) {
		// fork failed: close everything and report failure.
		close(pipeIn[0]);
		close(pipeIn[1]);
		close(pipeOut[0]);
		close(pipeOut[1]);
		return -1;
	}

	if (_pid == 0) {
		// ---------------- CHILD PROCESS (becomes the script) ----------------
		// We only use one end of each pipe here; close the ends we don't need.
		close(pipeIn[1]);    // child won't write to its own stdin pipe
		close(pipeOut[0]);   // child won't read from its own stdout pipe

		// Redirect the child's stdin/stdout to our pipes:
		//   the script reads its stdin  <- pipeIn[0]  (the request body we send)
		//   the script writes its stdout -> pipeOut[1] (which we then read)
		dup2(pipeIn[0], STDIN_FILENO);
		dup2(pipeOut[1], STDOUT_FILENO);

		close(pipeIn[0]);
		close(pipeOut[1]);

		// Build the CGI environment variables and convert to char** for execve.
		std::map<std::string, std::string> env = _buildEnv(scriptPath, req, config);
		char **envp = _envToCharArray(env);

		// Run in the script's directory so it can open files by relative path.
		std::string dir = scriptPath.substr(0, scriptPath.rfind('/'));
		std::string basename = scriptPath.substr(scriptPath.rfind('/') + 1);
		if (!dir.empty())
			chdir(dir.c_str());

		// Arguments passed to the interpreter: e.g. ["python3", "test.py", NULL].
		char *args[3];
		args[0] = const_cast<char*>(executable.c_str());
		args[1] = const_cast<char*>(basename.c_str());
		args[2] = NULL;

		// Replace this process with the interpreter. On success, this never
		// returns -- the child IS now the script.
		execve(executable.c_str(), args, envp);

		// We only get here if execve failed.
		_freeCharArray(envp);
		std::cerr << "CGI execve failed: " << strerror(errno) << std::endl;
		exit(1);
	}

	// ---------------- PARENT PROCESS (the server) ----------------
	// Close the ends the child owns; keep the ends we use.
	close(pipeIn[0]);    // parent doesn't read the body pipe
	close(pipeOut[1]);   // parent doesn't write the output pipe

	// The write end (us -> script stdin): make non-blocking so poll() drives it.
	Utils::setNonBlocking(pipeIn[1]);
	_writePipeFd = pipeIn[1];

	// The read end (script stdout -> us): also non-blocking for the poll loop.
	Utils::setNonBlocking(pipeOut[0]);
	_pipeFd = pipeOut[0];

	return 0;
}

int CGI::getPipeFd() const { return _pipeFd; }
int CGI::getWritePipeFd() const { return _writePipeFd; }
int CGI::getPid() const { return _pid; }

// ----------------------------------------------------------------------------
// _buildEnv(): construct the environment variables CGI scripts expect. These
// are how the script learns about the request (method, query, headers, etc.).
// ----------------------------------------------------------------------------
std::map<std::string, std::string> CGI::_buildEnv(const std::string &scriptPath,
	const Request &req, const ServerConfig &config) {
	std::map<std::string, std::string> env;

	// Standard CGI/1.1 variables.
	env["GATEWAY_INTERFACE"] = "CGI/1.1";
	env["SERVER_PROTOCOL"] = "HTTP/1.1";
	env["SERVER_SOFTWARE"] = "webserv/1.0";
	env["SERVER_NAME"] = config.host;
	env["SERVER_PORT"] = Utils::toStr(config.port);
	env["REQUEST_METHOD"] = req.getMethod();   // GET / POST
	env["REQUEST_URI"] = req.getUri();
	env["SCRIPT_NAME"] = req.getPath();
	env["SCRIPT_FILENAME"] = scriptPath;
	env["PATH_INFO"] = req.getPath();
	env["PATH_TRANSLATED"] = scriptPath;
	env["QUERY_STRING"] = req.getQuery();      // the ?... part of the URL
	env["REDIRECT_STATUS"] = "200";            // required by php-cgi

	// Body-related variables (only when there is a body).
	std::string contentType = req.getHeader("Content-Type");
	if (!contentType.empty())
		env["CONTENT_TYPE"] = contentType;
	if (req.getContentLength() > 0)
		env["CONTENT_LENGTH"] = Utils::toStr(req.getContentLength());

	// Every request header is also exposed as HTTP_<NAME> (uppercase, dashes ->
	// underscores). E.g. "User-Agent" becomes "HTTP_USER_AGENT".
	const std::map<std::string, std::string> &headers = req.getHeaders();
	std::map<std::string, std::string>::const_iterator it;
	for (it = headers.begin(); it != headers.end(); ++it) {
		std::string key = "HTTP_" + Utils::toLower(it->first);
		for (size_t i = 0; i < key.size(); ++i) {
			if (key[i] == '-')
				key[i] = '_';
			else
				key[i] = std::toupper(key[i]);
		}
		env[key] = it->second;
	}

	return env;
}

// ----------------------------------------------------------------------------
// _envToCharArray(): execve needs the environment as a NULL-terminated array of
// "KEY=VALUE" C-strings. Build that from our C++ map (allocated with new).
// ----------------------------------------------------------------------------
char **CGI::_envToCharArray(const std::map<std::string, std::string> &env) {
	char **result = new char*[env.size() + 1];   // +1 for the NULL terminator
	size_t i = 0;
	std::map<std::string, std::string>::const_iterator it;
	for (it = env.begin(); it != env.end(); ++it) {
		std::string entry = it->first + "=" + it->second;
		result[i] = new char[entry.size() + 1];
		std::strcpy(result[i], entry.c_str());
		++i;
	}
	result[i] = NULL;   // execve stops reading at NULL
	return result;
}

// Free the array (and each string) that _envToCharArray allocated.
void CGI::_freeCharArray(char **arr) {
	if (!arr) return;
	for (size_t i = 0; arr[i]; ++i)
		delete[] arr[i];
	delete[] arr;
}
