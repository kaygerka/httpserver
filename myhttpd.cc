#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <string>      // for std::string
#include <fstream>     // for std::ifstream
#include <sstream>     // for std::istringstream
#include <sys/wait.h>  // For waitpid
#include <signal.h>    // For SIGCHLD and signal()
#include <limits.h> // for PATH_MAX
#include <dirent.h>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <ctime>
#include <arpa/inet.h>
#include <mutex>
#include <chrono>
#include <limits>
#include <atomic>





#define BUFFER_SIZE 4096
const char* usage = "Usage: ./myhttpd <port>\n";
const int QueueLength = 10; // do i need this again?
// controls accept
pthread_mutex_t acceptMutex = PTHREAD_MUTEX_INITIALIZER;
volatile int masterSocket = -1; // for ctrl c closing --fd (listens)
#include <atomic>
//determines if server needs to be shut down
std::atomic<bool> shutdownRequested(false);

struct DirEntry {
    std::string name;
    bool isDir;
    off_t size;
    time_t mtime;
};

// For stats better time explanaition
using namespace std::chrono;
std::atomic<int> request_count(0);
double min_service_time = std::numeric_limits<double>::max();
double max_service_time = std::numeric_limits<double>::lowest();
std::string min_service_time_path;
std::string max_service_time_path;
std::mutex stats_mutex;
steady_clock::time_point server_start_time;

// For logs
static std::vector<std::string> logs;
static std::mutex log_mutex;
static const char* log_filename = "myhttpd.log";


// Processes time request
void processHttpRequest( int socket );

// ------- ctrl c
void handle_sigint(int sig) {
  shutdownRequested = true;
  if (masterSocket != -1) {
    close(masterSocket); // This will cause accept() to fail in all threads
    masterSocket = -1;
  }
  // Optionally, exit here or set a flag to let threads exit their loops
  exit(0); // Uncomment if you want immediate exit
}
// ------  404
void send_404(int clientSocket) {
  std::string header = 
    "HTTP/1.1 404 File Not Found\r\n"
    "Server: CS 252 lab5\r\n"
    "Content-type: text/plain\r\n"
    "\r\n"
    "Could not find the specified URL. The server returned an error.\n";
  send(clientSocket, header.c_str(), header.size(), 0);
}
// ------ 200
void send_200(int clientSocket, const std::string& filePath) {
  std::ifstream file(filePath, std::ios::binary);
  if (!file.is_open()) {
    send_404(clientSocket);
    return;
  }
  std::string header = 
    "HTTP/1.1 200 Document follows\r\n"
    "Server: CS 252 lab5\r\n"
    "Content-type: text/html\r\n"
    "\r\n";

  send(clientSocket, header.c_str(), header.size(), 0);
  char fileBuffer[BUFFER_SIZE];
  while (file.read(fileBuffer, sizeof(fileBuffer))) {
    send(clientSocket, fileBuffer, file.gcount(), 0);
  }
  send(clientSocket, fileBuffer, file.gcount(), 0);
  file.close();
}

// zombies ---------
void reapZombies(int sig) {
  while (waitpid(-1, NULL, WNOHANG) > 0);
}

// -p pool of threads ---------
/* worker threads, each one loops, accepts new client connections from master */
void* threadPoolFunction(void* arg) {
  // arg = master socket
  int* masterSocketPtr = (int*)arg;
  // while no ctrl c
  while (!shutdownRequested) {
    // lock mutex so only one thread calls accept
    pthread_mutex_lock(&acceptMutex);
    // invalid
    if (*masterSocketPtr == -1) {
      pthread_mutex_unlock(&acceptMutex);
      break;
    }
    // call accept and unlock to let other threads call accept
    int serfSocket = accept(*masterSocketPtr, NULL, NULL);
    pthread_mutex_unlock(&acceptMutex);

    if (serfSocket < 0) {
      if (shutdownRequested) break;
      if (errno == EBADF || errno == EINVAL) break; // bad fd/ invalid arg
      perror("accept failed");
      continue;
    }

    // PROCESS!
    processHttpRequest(serfSocket);
  }
  printf("Thread exiting\n");
  return NULL; // unreachable?
}

//send directory listing as HTML ---------------------
/* creates and send HTML format listing of directories*/
void send_directory_listing(int serfSocket, const std::string& dirPath, const std::string& urlPath, 
                            const std::string& sortBy, bool descending) {

  // open dir
  DIR* dir = opendir(dirPath.c_str());
  if (!dir) {
    send_404(serfSocket);
    return;
  }
  std::vector<DirEntry> entries;
  // directory entry
  struct dirent* entry;

  // read directory -- and store values
  while ((entry = readdir(dir)) != NULL) {
    DirEntry de;
    de.name = entry->d_name;
    std::string fullEntryPath = dirPath + "/" + de.name;
    struct stat st;
    if (stat(fullEntryPath.c_str(), &st) == 0) {
      de.isDir = S_ISDIR(st.st_mode);
      de.size = st.st_size;
      de.mtime = st.st_mtime;
      entries.push_back(de);
    }
  } // doen reading
  //close dir
  closedir(dir);

  // Sorting
  // sort -- name
  if (sortBy == "name") { 
    if (!descending) // asc
      std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        return a.name < b.name;
      });
    else // des
       std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
         return a.name > b.name;
        });
  // sort -- size
  } else if (sortBy == "size") {
    if (!descending) {
      // ac
      std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir; // dir first -
        return a.size < b.size;
      });
    } else {
      // des
      std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir < b.isDir; // dir last --
        return a.size > b.size;
      });
    }
  // sort -- time
  } else if (sortBy == "mtime") {
    if (!descending) // asc
      std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        return a.mtime < b.mtime;
      });
    else // desc
       std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
         return a.mtime > b.mtime;
       });
  }

    // HTML Output for directory listing
    std::ostringstream html;
    // determines increasing or decreasing
    std::string nextSort = (sortBy == "name" && !descending) ? "name_desc" : "name";
    std::string nextSizeSort = (sortBy == "size" && !descending) ? "size_desc" : "size";
    std::string nextMtimeSort = (sortBy == "mtime" && !descending) ? "mtime_desc" : "mtime";
    html << "<html><head><title>Index of " << dirPath << "</title></head><body>";
    html << "<h1>Index of " << dirPath << "</h1>";
    html << "<table><tr>"
         << "<th></th>"
         << "<th><a href=\"?sort=" << nextSort << "\">Name</a></th>"
         << "<th><a href=\"?sort=" << nextSort << "\">Last modified</a></th>"
         << "<th align=\"right\"><a href=\"?sort=" << nextSizeSort << "\">Size</a></th>"
         << "<th><a href=\"?sort=" << nextSort << "\"> Description</a></th></tr>";
    html << "<tr><td colspan=\"5\" style=\"border-top:1px solid #000;\"></td></tr>";


    // Parent directory link
    if (urlPath != "/") {
        std::string parentUrl = urlPath;
        if (parentUrl.back() == '/') parentUrl.pop_back();
        size_t slash = parentUrl.find_last_of('/');
        parentUrl = (slash == std::string::npos) ? "/" : parentUrl.substr(0, slash+1);
        html << "<tr><td><img src=\"/icons/back.gif\"></td>"
             << "<td><a href=\""<< parentUrl << "\">Parent Directory</a></td>"
             << "<td></td>"
             << "<td align=\"right\">-</td>" // Align the dash to the right
             << "<td></td></tr>";
    }

    // loop thru dir
    for (const auto& de : entries) {
        // skip hidden?
        if (de.name == "." || de.name == "..") continue;
        std::string icon;
        std::string desc;
        if (de.isDir) {
            // folder for directory
            icon = "/icons/folder.gif";
        } else {
            size_t dot = de.name.find_last_of('.');
            if (dot != std::string::npos) {
              std::string ext = de.name.substr(dot + 1);
              // get file type and assign .gif
              if (ext == "gif" || ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "svg") {
                icon = "/icons/image2.gif";
                //desc = "Image";
              } else if (ext == "txt" || ext == "html" || ext == "htm") {
                icon = "/icons/text.gif";
                //desc = "Text";
              } else if (ext == "o" || ext == "bin" || ext == "exe") {
                icon = "/icons/binary.gif";
                //desc = "Binary";
              } else {
                icon = "/icons/sphere1.gif";
                //desc = "";
              }
           } else {
             icon = "/icons/sphere1.gif";
             desc = "";
          }
        }
        // size of file
        std::string sizeStr = de.isDir ? "-" : std::to_string(de.size);
        if (!de.isDir && de.size > 1024) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1fK", de.size/1024.0);
            sizeStr = buf;
        }
        // Format date
        char datebuf[64];
        strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M", localtime(&de.mtime));

        // links
        //format table
        html << "<tr><td><img src=\"" << icon << "\" alt=\"[   ]\"></td><td>";
        html << "<a href=\"" << urlPath;
        if (urlPath.back() != '/') html << "/";
        html << de.name;
        if (de.isDir) html << "/";
        html << "\">" << de.name << (de.isDir ? "/" : "") << "</a></td>";
        html << "<td>" << datebuf << "</td>";
        html << "<td align=\"right\">" << sizeStr << "</td>";
        html << "<td>" << desc << "</td></tr>";
    }
    html << "<tr><td colspan=\"5\" style=\"border-top:1px solid #000;\"></td></tr>";
    html << "</table></body></html>";


    // SEND THE HEADER ITS OK
    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n";
    send(serfSocket, header.c_str(), header.size(), 0);
    send(serfSocket, html.str().c_str(), html.str().size(), 0);
    }



// main ------------------
int main( int argc, char ** argv ) {
  server_start_time = steady_clock::now();

  if (argc < 2 || argc > 3) {
    fprintf(stderr, "Usage: %s [-f|-t|-p] [<port>]\n", argv[0]);
    return 1;
  }

  // default
  std::string mode = "-f"; 
  int port = 6000;
 
  // no mode
  if (argc == 2) {
    port = atoi(argv[1]);
  }

  // with mode
  if (argc == 3) {
    mode = argv[1];
    port = atoi(argv[2]);
  }

  // CHECK ARG VALIDITY
  if (mode != "-f" && mode != "-t" && mode != "-p") {
    fprintf(stderr, "Invalid mode %s. Use -f, -t, or -p.\n", mode.c_str());
    return 1;
  }

  // -f PREVENT ZOMBIE BY REAPING FINISHED CHILDREN IN FORK MODE
  if (mode == "-f") {
        signal(SIGCHLD, reapZombies);
  }

  // Create socket for incoming tcp connections
  masterSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (masterSocket < 0) {
    perror("socket failed");
    return 1;
  }
  printf("Socket created successfully\n");
  signal(SIGINT, handle_sigint);


  struct sockaddr_in serverAddr;
  memset(&serverAddr, 0, sizeof(serverAddr));
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = INADDR_ANY;
  serverAddr.sin_port = htons(port);

  // Bind sokcet
  if (bind(masterSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
    perror("bind failed");
    return 1;
  }
  printf("Socket bound successfully on port %d\n", port);

  // Listen for incoming connections
  if (listen(masterSocket, 5) < 0) {
    perror("listen failed");
    return 1;
  }
  printf("Server is listening on port %d...\n", port);

  // P - POOL OF PROCESS 
  /* create pool size of 5, each thread runs function and waits for connection, main thread waits*/
  if (mode == "-p") {
    const int poolSize = 5;
    pthread_t poolThreads[poolSize];
    // Pass the masterSocket to each thread
    for (int i = 0; i < poolSize; i++) {
      if (pthread_create(&poolThreads[i], NULL, threadPoolFunction, (void*)&masterSocket) != 0) {
        perror("Failed to create pool thread");
        exit(1);
      }
    }
    // Main thread just waits forever
    for (int i = 0; i < poolSize; i++) {
      pthread_join(poolThreads[i], NULL);
    }
    close(masterSocket);
     return 0;
  }


  // Accept connections and process requests
  while (1) {
    // accepts
    int serfSocket = accept(masterSocket, NULL, NULL);
    if (serfSocket < 0) {
      if (shutdownRequested) {
        break;
      }
      perror("accept failed");
      continue;
    }
    // F - PROCESS MODE
    if (mode == "-f") {
      pid_t pid = fork();
      if (pid == 0) {
        // In child, doesnt need master
        close(masterSocket);
        processHttpRequest(serfSocket);
        exit(0); 
      } else if (pid > 0) {
        // In parent dont need serf socket
        close(serfSocket); 
      } else {
        perror("fork failed");
        close(serfSocket);
      }
    
    // T - THERAD PER REQUEST
    /* each creates a new detached thread and handles http requewst*/
    } else if (mode == "-t") {
      pthread_t thread;
      int *newSock = new int;
      *newSock = serfSocket;
      if (pthread_create(&thread, NULL, [](void *socket) -> void* {
        processHttpRequest(*(int *)socket);
        delete (int *)socket;
        return NULL;
      }, newSock) != 0) {
        perror("Failed to create thread");
        close(serfSocket);
      } else {
        // detach, cleans up after self
        pthread_detach(thread);
      }
    } else {
      fprintf(stderr, "Unsupported mode: %s\n", mode.c_str());
      close(serfSocket);
      continue;
    }
 }
 return 0;
}

// processHttpRequest -----------------
void processHttpRequest( int serfSocket ){
  // start clock for each process and increase process
  auto start = std::chrono::steady_clock::now();
  request_count++;
  bool done = false;

  const int MaxRequest = 8192;
  char request[MaxRequest + 1];
  int requestLength = 0;
  int n;

  // read the request/num of bytes  (until you hit \r\n\r\n)
  while ((n = read(serfSocket, request + requestLength, MaxRequest - requestLength)) > 0) {
    requestLength += n;
    request[requestLength] = 0;
    if (strstr(request, "\r\n\r\n")) break;
  }

  // get clinet ip addy
  struct sockaddr_in addr;
  socklen_t addr_size = sizeof(struct sockaddr_in);
  getpeername(serfSocket, (struct sockaddr *)&addr, &addr_size);
  std::string client_ip = inet_ntoa(addr.sin_addr);

  // LOGIN AND PASSWORD
  const char* auth_header = "Authorization: Basic a2F5bGk6Z2Vya2E=";
  if (!strstr(request, auth_header)) {
    const char *unauthResponse =
      "HTTP/1.1 401 Unauthorized\r\n"
      "WWW-Authenticate: Basic realm=\"myhttpd-cs252\"\r\n"
      "Content-Type: text/plain\r\n"
      "\r\n"
      "401 Unauthorized: Please provide valid credentials.\n";
    write(serfSocket, unauthResponse, strlen(unauthResponse));
    shutdown(serfSocket, SHUT_WR);

    close(serfSocket);
    return;
  }

  // method = GET
  // path = /index.html
  // protocol = HTTP/1.1
  char method[MaxRequest], path[MaxRequest], protocol[MaxRequest];
  sscanf(request, "%s %s %s", method, path, protocol);
  printf("Received request: %s %s %s\n", method, path, protocol);

  // remove question mark and stuff after
  char* qmark = strchr(path, '?');
  if (qmark) *qmark = '\0';

  // log
  std::string log_entry = client_ip + " " + path;


  // only handle GET requests
  if (strcmp(method, "GET") != 0) {
    const char *response = "HTTP/1.1 501 Not Implemented\r\nContent-Type: text/plain\r\n\r\nOnly GET supported.\n";
    write(serfSocket, response, strlen(response));
    shutdown(serfSocket, SHUT_WR);
    close(serfSocket);
    return;
  }

  // CGI BIN
  if (strncmp(path, "/cgi-bin/", 9) == 0) {
    std::string scriptName = path + 9;
    std::string queryString = "";

    // remove questionmakr
    char* qmark = strchr(path, '?');
    if (qmark) {
        *qmark = '\0';
        scriptName = path + 9;
        queryString = std::string(qmark + 1);
    }

    // full path and check existas
    std::string scriptPath = "http-root-dir/cgi-bin/" + scriptName;
    char resolvedScript[PATH_MAX];
    if (realpath(scriptPath.c_str(), resolvedScript) == NULL) {
        send_404(serfSocket);
        shutdown(serfSocket, SHUT_WR);
        close(serfSocket);
        return;
    }

    // fork child process
    pid_t pid = fork();
    if (pid == 0) {
        // set env var
        setenv("REQUEST_METHOD", "GET", 1);
        setenv("QUERY_STRING", queryString.c_str(), 1);

        // redirect stdout/err to serfSocket
        dup2(serfSocket, STDOUT_FILENO);
        dup2(serfSocket, STDERR_FILENO);
        close(serfSocket);
        
        // send ok header and call execv to run script/full path
        char* argv[] = {resolvedScript, NULL};
        const char *header = "HTTP/1.1 200 OK\r\n";
        write(STDOUT_FILENO, header, strlen(header));
        execv(resolvedScript, argv);
        perror("execv failed");
        exit(1);
    // wait for child to finish -- parent
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);

        shutdown(serfSocket, SHUT_WR);
        close(serfSocket);
        return;
    } else {
        perror("fork failed");
        send_404(serfSocket);

        shutdown(serfSocket, SHUT_WR);
        close(serfSocket);
        return;
    }
  }
// stats ------
  if (strcmp(path, "/stats") == 0) {
    auto now = std::chrono::steady_clock::now();
    // calc server up time
    double uptime = std::chrono::duration<double>(now - server_start_time).count();
    int num_requests;
    double min_time, max_time;
    std::string min_path, max_path;

    // MUTEX -- THE {} is own lifetime var? destroys when done?
    // calcs teh min path and max path
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        num_requests = request_count;
        min_time = (min_service_time == std::numeric_limits<double>::max()) ? 0.0 : min_service_time;
        max_time = (max_service_time == std::numeric_limits<double>::lowest()) ? 0.0 : max_service_time;
        min_path = min_service_time_path;
        max_path = max_service_time_path;
    }

    // html for logs
    // builds http response with ostringstream
    std::ostringstream html;
    html << "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
    html << "<html><body>";
    html << "<h1>Server Statcs</h1>";
    html << "<p>Student Name: Kayli Gerka</p>";
    html << "<p>Server Uptime: " << uptime << " seconds</p>";
    html << "<p>Number of Requests: " << num_requests << "</p>";
    //html << "<p>Min Service Time: " << min_time << " seconds</p>";
    html << "<p>Min Service Time:  " << min_time << " seconds: " << min_path << "</p>";
    //html << "<p>Max Service Time: " << max_time << " seconds</p>";
    html << "<p>Max Service Time:" << max_time << " seconds: " << max_path << "</p>";
    html << "</body></html>";

    std::string response = html.str();
    write(serfSocket, response.c_str(), response.length());
    shutdown(serfSocket, SHUT_WR);
    close(serfSocket);
    return;
}
// log ---------
if (strcmp(path, "/logs") == 0) {
    std::ostringstream html;
    html << "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
    html << "<html><body><h1>Request Log</h1><pre>";

    // read log line by line
    std::ifstream log_file(log_filename);
    std::string line;
    while (std::getline(log_file, line)) {
        html << line << "\n";
    }
    html << "</pre></body></html>";
    std::string response = html.str();
    write(serfSocket, response.c_str(), response.length());
    shutdown(serfSocket, SHUT_WR);
    close(serfSocket);
    return;
}


// OUTSIDE CGI AND STATS/LOG
  // Map "/" to "/index.html"
  if (strcmp(path, "/") == 0) {
    strcpy(path, "/index.html");
  }
  // Build the full path
  std::string baseDir = "http-root-dir";
  std::string finalPath;

  if (strncmp(path, "/icons/", 7) == 0) {
    finalPath = baseDir + std::string(path); // skip htdocs
  } else {
    finalPath = baseDir + "/htdocs" + std::string(path);
  }

  //ABSOLUTE PATH FOUND
  char resolvedPath[PATH_MAX];
  if (realpath(finalPath.c_str(), resolvedPath) == NULL) {
    // Could not resolve path (file does not exist or other error)
    send_404(serfSocket);
    shutdown(serfSocket, SHUT_WR);
    close(serfSocket);
    return;
  }

  // directory traversal protection
  // checks if resolved path is in directory and if not dont traverse
  std::string allowedDir;
  if (strncmp(path, "/icons/", 7) == 0) {
    allowedDir = baseDir + "/icons";
  } else {
    allowedDir = baseDir + "/htdocs";
  }
  char resolvedAllowedDir[PATH_MAX];
  if (realpath(allowedDir.c_str(), resolvedAllowedDir) == NULL) {
    // Should not happen, but handle error
    send_404(serfSocket);

    shutdown(serfSocket, SHUT_WR);
    close(serfSocket);
    return;
  }

  // Check if the resolved path is inside the allowed directory
  if (strncmp(resolvedPath, resolvedAllowedDir, strlen(resolvedAllowedDir)) != 0) {
    // Directory traversal attempt!
    send_404(serfSocket); // Or send a 403 Forbidden if you prefer

    shutdown(serfSocket, SHUT_WR);
    close(serfSocket);
    return;
  } // DONE WITH PROTECT

  // stat the resolved path
  struct stat path_stat;
  if (stat(resolvedPath, &path_stat) == 0) {  // stat succeeded
    if (S_ISDIR(path_stat.st_mode)) {
      // find index.html in the directory
      std::string indexPath = std::string(resolvedPath) + "/index.html";
      struct stat index_stat;
      if (stat(indexPath.c_str(), &index_stat) == 0) {
        send_200(serfSocket, indexPath);
        shutdown(serfSocket, SHUT_WR);
        close(serfSocket);
        return;
      }
      // no index.html, create directory listing
      // Parse sort parameter from the URL
      std::string sortBy = "name";
      bool descending = false;
      char* sortParam = strstr(request, "sort=");
      if (sortParam) {
        sortParam += 5;
    if (strncmp(sortParam, "size_desc", 9) == 0) { sortBy = "size"; descending = true; }
    else if (strncmp(sortParam, "size", 4) == 0) { sortBy = "size"; descending = false; }
    else if (strncmp(sortParam, "mtime", 5) == 0) { sortBy = "mtime"; descending = false; }
    else if (strncmp(sortParam, "mtime_desc", 10) == 0) { sortBy = "mtime"; descending = true; }
    else if (strncmp(sortParam, "name_desc", 9) == 0) { sortBy = "name"; descending = true; }
    else if (strncmp(sortParam, "name", 4) == 0) { sortBy = "name"; descending = false; }
      } 
      send_directory_listing(serfSocket, resolvedPath, path, sortBy, descending);
      shutdown(serfSocket, SHUT_WR);
      close(serfSocket);
      return;
    }
  } else {
    // stat failed -- probably file not found
    send_404(serfSocket);

    shutdown(serfSocket, SHUT_WR);
    close(serfSocket);
    return;
  }

  // Try to open the file
  int file = open(finalPath.c_str(), O_RDONLY);
  if (file < 0) {
    // File not found
    send_404(serfSocket);

    shutdown(serfSocket, SHUT_WR);
    close(serfSocket);
    return;
  }

  // Detect MIME type
  std::string mimeType = "text/html";
  std::string fname(resolvedPath);
  size_t dot = fname.find_last_of('.');
  if (dot != std::string::npos) {
    std::string ext = fname.substr(dot + 1);
    if (ext == "gif") mimeType = "image/gif";
    else if (ext == "png") mimeType = "image/png";
    else if (ext == "jpg" || ext == "jpeg") mimeType = "image/jpeg";
    else if (ext == "svg") mimeType = "image/svg+xml";
    else if (ext == "xbm") mimeType = "image/x-xbitmap";
    else if (ext == "html" || ext == "htm") mimeType = "text/html";
    else if (ext == "txt") mimeType = "text/plain";
  }
  // File exists — send 200 OK and the file contents
  std::string okHeader =
  "HTTP/1.1 200 Document follows\r\n"
  "Server: CS 252 lab5\r\n"
  "Content-type: " + mimeType + "\r\n"
  "\r\n";

  write(serfSocket, okHeader.c_str(), okHeader.length());

  // Send the file contents
  char buffer[BUFFER_SIZE];
  while ((n = read(file, buffer, BUFFER_SIZE)) > 0) {
    write(serfSocket, buffer, n);
  // end of process stop clock and track time
  }

    auto end = std::chrono::steady_clock::now();
    double service_time = std::chrono::duration<double>(end - start).count();
{
    std::lock_guard<std::mutex> lock(stats_mutex);
    if (service_time < min_service_time) {
        min_service_time = service_time;
        min_service_time_path = path;  // path is the request path (e.g., "/index.html")
    }
    if (service_time > max_service_time) {
        max_service_time = service_time;
        max_service_time_path = path;
    }
}

  close(file);
  shutdown(serfSocket, SHUT_WR);
  close(serfSocket);
}
