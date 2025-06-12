# httpserver
Complete source code for a multithreaded/multiprocess HTTP server written in C++. 
The server is designed to handle basic web requests and serve files from a local directory, with several features:

Key Features
Modes of Operation:
The server supports three modes, selectable by command-line arguments:

-f: Forks a new process for each incoming connection.

-t: Creates a new thread for each request.

-p: Uses a fixed pool of threads to handle connections.

Socket Programming: It uses low-level POSIX socket APIs (socket, bind, listen, accept) to listen for and accept TCP connections.

Request Handling:
Only HTTP GET requests are supported. The server parses the HTTP request, extracts the requested path, and serves the corresponding file or directory listing.

Directory Listings:
If a directory is requested and no index.html exists, the server generates an HTML directory listing, supporting sorting by name, size, or modification time, in ascending or descending order.

Static File Serving:
Files are served from a base directory. The server detects the MIME type based on file extension and sends appropriate HTTP headers.

CGI Support:
Requests to /cgi-bin/ execute scripts located in a special directory, with environment variables set as per CGI standards. The output of the script is sent as the HTTP response.

Basic Authentication:
The server requires HTTP Basic authentication with a hardcoded username and password (kayli:gerka). Unauthorized requests receive a 401 Unauthorized response.

Logging and Stats:

/stats: Returns an HTML page with server uptime, number of requests, and min/max service times.
/logs: Returns a log of recent requests.

Security:
Prevents directory traversal by checking that resolved file paths are within the allowed directories.
Handles zombie processes and graceful shutdown on SIGINT (Ctrl+C).

Thread/Process Safety:
Uses mutexes and atomic variables to protect shared state (e.g., request counters, logs).

Typical Usage
Compile the code and run it as:
./myhttpd -f 8080
./myhttpd -t 8080
./myhttpd -p 8080
