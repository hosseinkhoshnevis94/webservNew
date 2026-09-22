#!/usr/bin/env python3
import os
print("Content-Type: text/html\r")
print("\r")
print("<h1>Working directory test</h1>")
print("<p>CWD: {}</p>".format(os.getcwd()))
# Open a file by RELATIVE path - only works if we chdir'd correctly
try:
    with open("data.txt") as f:
        print("<p>Relative file read OK: {}</p>".format(f.read().strip()))
except Exception as e:
    print("<p>FAILED to read relative file: {}</p>".format(e))
