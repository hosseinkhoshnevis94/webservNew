#include "Webserv.hpp"

// ============================================================================
// main.cpp - Where the program starts.
//
// This file is intentionally tiny. Its only job is to wire the big pieces
// together: read the config, build the server, and start the loop. All the
// real work happens inside the classes.
// ============================================================================

// A global flag other code can check to know if we should keep running.
static bool g_running = true;

// Called when the user presses Ctrl+C (SIGINT). Instead of dying instantly,
// we flip the flag so the program can shut down cleanly.
void signalHandler(int sig) {
	(void)sig;  // we don't care which signal; silence "unused parameter"
	g_running = false;
	std::cout << "\nShutting down server..." << std::endl;
}

int main(int argc, char **argv) {
	// Default config path if the user doesn't provide one.
	std::string configFile = "conf/default.conf";

	// Usage: ./webserv [config file]. More than one argument is an error.
	if (argc > 2) {
		std::cerr << "Usage: ./webserv [configuration file]" << std::endl;
		return 1;
	}
	if (argc == 2)
		configFile = argv[1];  // use the file the user passed

	// Ctrl+C -> graceful shutdown (not an instant kill).
	signal(SIGINT, signalHandler);
	// If a client disconnects mid-send, the OS would send SIGPIPE and kill us.
	// We ignore it so the server never crashes; send() just returns an error
	// we handle normally.
	signal(SIGPIPE, SIG_IGN);

	try {
		Config config(configFile);  // 1. read + validate the config file
		Server server(config);      // 2. create sockets from the config
		(void)g_running;            // (the Server keeps its own running flag)
		server.run();               // 3. run the poll() loop until shutdown
	} catch (const std::exception &e) {
		// Any setup failure (bad config, can't bind a port, etc.) lands here.
		// We print a friendly message and exit instead of crashing.
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}
