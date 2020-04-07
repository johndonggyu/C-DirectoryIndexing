#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/mman.h>

#define LOG 100
#define ERROR 200
#define MAXLINE 1024

#define ROOT "hw2"
#define SERVERNAME "MyServer"
#define VERSION "1.0"

typedef struct {
	char *ext; // extension
	char *filetype; // mime_type
} extension_map;

typedef struct {
    char filename[512];
    off_t offset;               // for range
    size_t end;
} http_request;

extension_map file_types [ ] = {
	{".css", "text/css"},
	{".js", "application/javascript"},
	{".pdf", "application/pdf"},
	{".mp4", "video/mp4"},
	{".svg", "image/svg+xml"},
	{".xml", "text/xml"},
	{".gif", "image/gif" },
	{".jpg", "image/jpeg"},
	{".jpeg","image/jpeg"},
	{".png", "image/png" },
	{".zip", "image/zip" },
	{".gz",  "image/gz"  },
	{".tar", "image/tar" },
	{".htm", "text/html" },
	{".html","text/html" },
	{0,0} }; //NULL, NULL

char *default_file_type = "text/plain"; // default mime type

char *SERVERADDRESS = "127.0.0.1";
int PORT = 9999;

void webprocess(int c_sock, struct sockaddr_in *c_addr);
void weblog(int type, char s1[ ], char s2[ ], int n);
void requestprocess(int fd, http_request *req);
static const char* getfiletype(char *filename);
void sizetransform(char* buf, struct stat *stat);
void directoryhandler(int out_fd, int dir_fd, char *filename);
void servestatic(int outfd, int infd, http_request *req, size_t total_size);
void clienterror(int fd, int code, char *msg, char *lmsg);

int
main(int argc, char *argv[ ]) {
	struct sockaddr_in s_addr, c_addr;
	int	s_sock, c_sock;
	int	len;

	unsigned short port;

	int	pid;

	if(argc != 2){
		printf("usage: webServer port_number");
		return -1;
	}

	if(fork( ) != 0) 			//background process
		return 0;			// parent return to shell

	weblog(LOG, SERVERADDRESS, "web server start", getpid());

	if((s_sock=socket(AF_INET, SOCK_STREAM, 0))<0){
		weblog(ERROR, "SYSCALL", "web server listen socket open error", s_sock);
	}

	PORT = port=atoi(argv[1]);
	if(port > 60000)
		weblog(ERROR, "PORT", "invalid port number", port);

	int optval = 1;
	/* Eliminates "Address already in use" error from bind. */
	if (setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR,
                (const void *)&optval , sizeof(int)) < 0)
        	return -1;

	// 6 is TCP's protocol number
	// enable this, much faster : 4000 req/s -> 17000 req/s
	if (setsockopt(s_sock, 6, TCP_CORK,
		(const void *)&optval , sizeof(int)) < 0)
		return -1;

	memset(&s_addr, 0, sizeof(s_addr));
	s_addr.sin_family = AF_INET;
	s_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	s_addr.sin_port = htons(port);

	if(bind(s_sock, (struct sockaddr *) &s_addr, sizeof(s_addr)) < 0){
		perror("bind");
		weblog(ERROR, "BIND", "server cannot bind", 0);
	}

	if(listen(s_sock, 10) < 0) // Max_Client = 10
		return -1;

	signal(SIGCHLD, SIG_IGN);		// ignore child death(prevent zombies)
	signal(SIGPIPE, SIG_IGN);		// ignore browser interrupts

	while(1){ // Accept
		len = sizeof(c_addr);
		c_sock = accept(s_sock, (struct sockaddr *) &c_addr, &len);
		if(c_sock < 0){
			perror("accept");			
			exit(-1);
		}		
		if((pid = fork( )) < 0) {
			perror("fork");
			exit(-1);
		} else if(pid == 0) { // child
			//printf("child forked pid = %d\n", getpid());
			close(s_sock);
			webprocess(c_sock, &c_addr);
			close(c_sock);
			return 0;
		} else { // parent
			close(c_sock);
		}
	}
	
	return 0;
}

void
webprocess(int c_sock, struct sockaddr_in *c_addr)
{
	int	code = 200;		// status code (default: 200, success)
	int	fd;			// file discriptor

	struct stat sbuf;
	http_request req;

	// request packet parsing
	requestprocess(c_sock, &req); // get req

	fd = open(req.filename, O_RDONLY, 0); // open request file
	// printf("fd = %d\n", fd); // fd != 0
	if(fd <= 0){
		code = 404;
		char *msg = "File not found";
		clienterror(c_sock, code, "Not found", msg);
	} else {
		fstat(fd, &sbuf); // find file stat
		if(S_ISREG(sbuf.st_mode)){ // if reg file
			if(req.end == 0){
				req.end = sbuf.st_size; // file size
			}
			if(req.offset > 0){
				code = 206; // partial content (download)
			}
			// read file and send to client
			servestatic(c_sock, fd, &req, sbuf.st_size);
		} else if(S_ISDIR(sbuf.st_mode)){ // if directory
			code = 200; // OK
			directoryhandler(c_sock, fd, req.filename);
		} else { // else files
			code = 400;
			char *msg = "Unknown Error";
			clienterror(c_sock, code, "Error", msg);
		}
		close(fd);
	}
	weblog(LOG, inet_ntoa(c_addr->sin_addr), req.filename, code);
}

// if request parameter is directory
void
directoryhandler(int out_fd, int dir_fd, char *filename){
	char buf[MAXLINE], m_time[32], size[16];
	struct stat statbuf;
	sprintf(buf, "HTTP/1.1 200 OK\r\n%s%s%s%s%s",
	    "Content-Type: text/html\r\n\r\n",
	    "<html><head><style>",
	    "td {padding: 1.5px 6px; text-align: center;}",
	    ".name {text-align: left;}",
	    "</style><title>Unix Project</title></head><body>\n");

	DIR *d = fdopendir(dir_fd); // return dirent
	write(out_fd, buf, strlen(buf));

	// index of directory
	sprintf(buf, "<h1>Index of /%s</h1>", filename[0] == '.' ? ROOT:filename);
	write(out_fd, buf, strlen(buf));

	// name, last modified size(dir)
	sprintf(buf, "<table><tr><th><font color='blue'>Name</font></th><th><font color='blue'>Last Modified</font></th><th><font color='blue'>Size</font></th></tr><tr><th colspan='3'><hr></th></tr>\n");
	write(out_fd, buf, strlen(buf));

	// goto parent directory
	if(strcmp(filename, ".")){
		sprintf(buf, 
		"<tr><td><a href='javascript:history.back()'>Parent Directory</a></td></tr>\n");
		write(out_fd, buf, strlen(buf));	
	}	

	struct dirent *dp; //ino,off,reclen,name
	int ffd;
	while ((dp = readdir(d)) != NULL){ // read dir stat
		if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")){
		    continue; // block . or ..
		}
		//openat: open file at dir_fd
		if ((ffd = openat(dir_fd, dp->d_name, O_RDONLY)) == -1){
		    perror(dp->d_name);
		    continue;
		}
		fstat(ffd, &statbuf); // read file stat
		strftime(m_time, sizeof(m_time), // time transform
			 "%Y-%m-%d %H:%M", localtime(&statbuf.st_mtime));
		sizetransform(size, &statbuf); //[DIR] or size(KMG)
		// common
		if(S_ISREG(statbuf.st_mode) || S_ISDIR(statbuf.st_mode)){
			char *d = S_ISDIR(statbuf.st_mode) ? "/" : "";
			// dir name/, reg name
			sprintf(buf, 
			"<tr><td class='name'><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n",
			dp->d_name, d, dp->d_name, d, m_time, size);
			write(out_fd, buf, strlen(buf));
		}
		close(ffd);
	}
	//send to client the footer
	sprintf(buf, "<tr><th colspan='3'><hr></th></tr>"
	"<tr><th colspan='3'>%s/%s Server at %s Port %d</th></tr>"
	"</table></body></html>", SERVERNAME, VERSION, SERVERADDRESS, PORT);
	write(out_fd, buf, strlen(buf));
	closedir(d);
}

// parse request
void
requestprocess(int fd, http_request *req){ // fd : c_sock
	char buf[MAXLINE], method[MAXLINE], uri[MAXLINE];
	int c; // c and ch is for read request line
	char ch;
	req->offset = 0;
	req->end = 0;

	// read request line
	char *bufp = buf;
	for(int i = 1; i < MAXLINE; i++){
		if(c = read(fd, &ch, 1) == 1){
			*bufp++ = ch;
			if(ch == '\n')
				break;
		} else break;
	}
	*bufp = 0; //'\0'
	sscanf(buf, "%s %s", method, uri); // version is not required

	// 206 Partial Content : Request: Ranges, Response: Content-Range
	while(buf[0] != '\n' && buf[1] != '\n') { /* \n || \r\n */
		// read request line
		char *bufp = buf;
		for(int i = 1; i < MAXLINE; i++){
			if(c = read(fd, &ch, 1) == 1){
				*bufp++ = ch;
				if(ch == '\n')
					break;
			} else break;
		}
		*bufp = 0; //'\0'
		// if Range exists, 
		if(buf[0] == 'R' && buf[1] == 'a' && buf[2] == 'n'){
		    sscanf(buf, "Range: bytes=%lu-%lu", 
			&req->offset, (long unsigned int*)&req->end);
		    // Range: [start, end]
		    if( req->end != 0) req->end ++;
		}
	}

	char* filename = uri;
	if(uri[0] == '/'){
		filename = uri + 1; // remove slash
		int length = strlen(filename);
		if(length == 0){ //localhost:9999/ connected
			filename = "."; //cur dir
		} else {
			for(int i=0; i<length; i++){
				if(filename[i] == '?'){ //delete parameter
					filename[i] = '\0';
					break;
				}
			}
		}
	}
	//printf("filename = %s\n", filename); //.
	strcpy(req->filename, filename);
}

// get file type
static const char*
getfiletype(char *filename){
	char* dot = strrchr(filename, '.');
	if(dot){ // strrchr : find last character('.') return pointer of this
		//printf("dot = %s\n", dot);
		extension_map *map = file_types;
		while(map->ext){
			if(strcmp(map->ext, dot) == 0){
				//printf("map->ext = %s\n", map->ext);
				return map->filetype;
			}
			map++;
		}
	}
	return default_file_type;
}

// file size formatting
void
sizetransform(char* buf, struct stat *stat){
	if(S_ISDIR(stat->st_mode)){
		sprintf(buf, "%s", "[DIRECTORY]"); // print in [DIR]
	} else {
		off_t size = stat->st_size;
		if(size < 1024){
			sprintf(buf, "%luB", size);
		} else if(size < 1024 * 1024){
			sprintf(buf, "%.1fKB", (double)size / 1024);
		} else if(size < 1024 * 1024 * 1024){
			sprintf(buf, "%.1fMB", (double)size / 1024 / 1024);
		} else {
			sprintf(buf, "%.1fGB", (double)size / 1024 / 1024 / 1024);
		}
	}
}

// serve-static
void
servestatic(int outfd, int infd, http_request *req, size_t total_size){
	char buf[256];
	if(req->offset > 0){
		sprintf(buf, "HTTP/1.1 206 Partial\r\n");
		// append string with sprintf : buf + strlen(buf)
		sprintf(buf + strlen(buf), "Content-Range: bytes %lu-%lu/%lu\r\n",
			(long unsigned int)req->offset, 
			(long unsigned int)req->end, (long unsigned int)total_size);
	} else {
		sprintf(buf, "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n");
	}
	sprintf(buf + strlen(buf), "Cache-Control: no-cache\r\n");
	
	// Content-Length
	sprintf(buf + strlen(buf), "Content-Length: %lu\r\n", 
		(long unsigned int)(req->end - req->offset));
	
	// Content-Type
	sprintf(buf + strlen(buf), "Content-Type: %s\r\n\r\n", getfiletype(req->filename));

	write(outfd, buf, strlen(buf));
	// memory mapping to write body of document(file)
	int filesize = req->end - req->offset;
	char* p = mmap(0, filesize, PROT_READ, MAP_PRIVATE, infd, 0);
	write(outfd, p, filesize);
	munmap(p, filesize);
}	

// send error to client
void
clienterror(int fd, int code, char *msg, char *lmsg){
	char buf[MAXLINE];
	sprintf(buf, "HTTP/1.1 %d %s\r\n", code, msg);
	sprintf(buf + strlen(buf), 
	"Content-Length: %lu\r\n\r\n", (long unsigned int)strlen(lmsg));
	sprintf(buf + strlen(buf), "%s", lmsg);
	write(fd, buf, strlen(buf));
}
	
void
weblog(int type, char s1[ ], char s2[ ], int n)
{
	int	log_fd;
	char	buf[BUFSIZ], m_time[32];
	time_t	t;

	time(&t);
	strftime(m_time, sizeof(m_time), // time transform
			 "%Y-%m-%d %H:%M:%S", localtime(&t));

	if(type == LOG) {
		sprintf(buf, "[%s] STATUS %s %s %d\n", m_time, s1, s2, n);
	} else if(type == ERROR) {
		sprintf(buf, "[%s] ERROR %s %s %d\n", m_time, s1, s2, n);
	}

	if((log_fd = open("web.log", O_CREAT|O_WRONLY|O_APPEND, 0644)) >= 0) {
		write(log_fd, buf, strlen(buf));
		close(log_fd);
	}

	if(type == ERROR) exit(-1);

}
