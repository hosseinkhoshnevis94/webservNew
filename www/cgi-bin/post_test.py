#!/usr/bin/env python3

import os
import sys

method = os.environ.get("REQUEST_METHOD", "GET")
content_length = int(os.environ.get("CONTENT_LENGTH", 0))
query_string = os.environ.get("QUERY_STRING", "")

body = ""
if content_length > 0:
    body = sys.stdin.read(content_length)

print("Content-Type: text/html\r")
print("Status: 200 OK\r")
print("\r")
print("<html><head><title>CGI POST Test</title></head>")
print("<body>")
print("<h1>CGI POST Test</h1>")
print("<p>Method: {}</p>".format(method))
print("<p>Content-Length: {}</p>".format(content_length))
print("<p>Query String: {}</p>".format(query_string))
if body:
    print("<h2>Body:</h2>")
    print("<pre>{}</pre>".format(body))
print("</body></html>")
