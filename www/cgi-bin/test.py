#!/usr/bin/env python3

import os
import sys

print("Content-Type: text/html\r")
print("\r")
print("<html><head><title>CGI Test</title></head>")
print("<body>")
print("<h1>CGI Test Page</h1>")
print("<h2>Environment Variables:</h2>")
print("<table border='1'>")
for key, value in sorted(os.environ.items()):
    if key.startswith(("HTTP_", "REQUEST_", "SERVER_", "SCRIPT_", "QUERY_", "CONTENT_", "PATH_", "GATEWAY_", "REDIRECT_")):
        print("<tr><td><b>{}</b></td><td>{}</td></tr>".format(key, value))
print("</table>")

# Handle POST body
if os.environ.get("REQUEST_METHOD") == "POST":
    content_length = int(os.environ.get("CONTENT_LENGTH", 0))
    if content_length > 0:
        body = sys.stdin.read(content_length)
        print("<h2>POST Body:</h2>")
        print("<pre>{}</pre>".format(body))

print("<h2>Query String:</h2>")
print("<p>{}</p>".format(os.environ.get("QUERY_STRING", "(none)")))
print("</body></html>")
