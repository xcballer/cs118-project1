/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#include <queue>

#include "http-request.h"
#include "http-response.h"
#include "http-headers.h"

using namespace std;

#define RESPONSE_SIZE 10000
#define REQUEST_SIZE 8000
#define CHILDLIMIT 5

static int nChilds = 0;

int get_message(int socket,int size,int maxfd,char* buf);
char const* parse_request(char * data, int size, HttpRequest * req,int socket);
bool check_cached(HttpRequest * req,char* cache);
int initiate_server(HttpRequest *req);
int send_message(int socket,int size,char* data);
//void parse_response(char* data, int size, HttpResponse *res);
void update_cache(HttpRequest* req, HttpResponse* res,char* data, int length);
void serve_client(int socket);

/*Returns true if cache is up-to-date*/
bool compare_times(string expires)
{
  if(expires == "")
    return false;
  time_t curr = time(NULL);
  //struct tm * currentgmt = gmtime(&curr);
  //currentgmt->tm_isdst = 0;
  //time_t currgmt = mktime(currentgmt);
  struct tm exp;
  //exp.tm_isdst = 0;
  char const * c = strptime(expires.c_str(),"%a, %d %b %Y %H:%M:%S GMT",&exp);
  time_t expired;
  if(c != NULL)
  {
    expired = mktime(&exp);
    if(expired > curr)
    {
      return true;
    }
  }
  return false;
}

string get_file_name(char* hostname)
{
    string host = "";
    for(int i = 0; hostname[i] != '\0'; i++){
      if(hostname[i] == '.' || hostname[i] == '/'){
        continue;
      }
      else
        host += hostname[i];
    }
    return host;
}
int cache_write(char* hostname, char* path,  char* res, int len)
{
  string hname = get_file_name(hostname);
  string pname = get_file_name(path);
  string filename = hname + pname + ".txt";
  char* fname = new char[filename.length() + 1];
  strcpy(fname, filename.c_str());
  int fd;
  fd = open(fname, O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);
  if(fd < 0)
    return -1;
  struct flock fl;
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
  fl.l_pid = getpid();
  fcntl(fd, F_SETLKW, &fl);
  size_t length = len;
  write(fd, res, length);
  close(fd);
  fl.l_type = F_UNLCK;
  fcntl(fd, F_SETLK, &fd);
  delete [] fname;
  return 0;
  
}
char* cache_read(HttpRequest* req)
{
  string hostname = req->GetHost();
  string path = req->GetPath();
  int pathlen = path.length()+1;
  int len = hostname.length()+1;
  char* cstr = new char [len];
  char* pstr = new char [pathlen];
  strcpy(cstr, hostname.c_str());
  strcpy(pstr, path.c_str());
  hostname = get_file_name(cstr);
  path = get_file_name(pstr);
  string filename = hostname + path + ".txt";
  char* fname = new char [filename.length() + 1];
  strcpy(fname, filename.c_str());
  int fd;
  fd = open(fname, O_RDONLY);
  if(fd < 0)
    return NULL;
  //lock
  struct flock fl;
  fl.l_type = F_RDLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
  fl.l_pid = getpid();
  fcntl(fd, F_SETLKW  , &fl);
  struct stat filestat;
  if(fstat(fd, &filestat) < 0)
    return NULL;
  size_t length = filestat.st_size;
  char* buf = (char*)malloc((sizeof(char) * length) + 1);
  read(fd, buf, length);
  buf[length] = '\0';
  //release lock
  close(fd);
  fl.l_type = F_UNLCK;
  fcntl(fd, F_SETLK, &fl);
  delete [] cstr;
  delete [] fname;
  delete [] pstr;
  return buf;
}
pid_t blocking_fork()
{
  pid_t pid;
  int stat;

  if (nChilds < CHILDLIMIT)
  {
    nChilds++;
    return fork();
  }
  else
  {
    //printf("reached maximum number of children, reaping zombies...\n");
    while (nChilds != 0)
    {
      pid = waitpid(-1, &stat, WNOHANG);
      if (pid == -1)
      { // Wait error
        return -1;
      }
      else if (pid == 0)
      { 
        if (nChilds == CHILDLIMIT)
        { // No child died, block until one does
          //printf("no child died...\n");
          while (waitpid(-1, &stat, 0) && WIFEXITED(stat))
            /* just wait until an exit */;
          return fork();
        }
        else
        { // At least one child has finished, but no more are available
          break;
        }
      }
      else
      {
        //printf("child %d was reaped\n", pid);
        nChilds--;
      }
    }
    nChilds++;
    return fork();
  }
}

void update_cache(HttpRequest* req, HttpResponse* res,char * data, int length)
{
  HttpResponse initial_r;
  char const * d = NULL;
  try
  {
    d = initial_r.ParseResponse(data,strlen(data));
  }
  catch(ParseException e)
  {
    cout << "Caught an exception in update_cache!\n";
    char const * ee = e.what();
    cout << ee;
    return;
  }
  //char const * d = initial_r.ParseResponse(data,strlen(data));
  (void) d; // Avoid compiler warning
  int init_len = initial_r.GetTotalLength();//Initial length of header stuff
  int data_len = strlen(data) - init_len; // Length of data (not including header stuff)
  
  string expires = res->FindHeader("Expires");
  string lms = res->FindHeader("Last-Modified");
  initial_r.ModifyHeader("Expires",expires);
  initial_r.ModifyHeader("Last-Modified",lms);
  
  int my_header_len = initial_r.GetTotalLength();
  char * result = new char [my_header_len+data_len+1]; // 1 for Null byte
  initial_r.FormatResponse(result);
  
  char * temp = strncat(result,data+init_len,data_len);
  (void) temp; // Avoid compiler warning
  
  string path = req->GetPath();
  string h = req->GetHost();
  int hsize = h.size();
  int size = path.size();
  char * host = new char [hsize+1];
  char * pathc = new char [size+1];
  for(int i = 0; i < hsize; i++)
    host[i] = h[i];
  host[hsize] = '\0';        
  for(int i = 0; i < size; i++)
    pathc[i] = path[i];
  pathc[size] = '\0';

  cache_write(host,pathc,result,my_header_len+data_len);
  cout << result;
  delete [] host;
  delete [] pathc;
  delete [] result;
}

int send_message(int socket, int size, char* data)
{
  int acc = 0;
  if((data == NULL) || (size < 0))
    return -1;
  if(size == 0)
    return 0;
  
  for(;;)
  {
    int j = send(socket,data+acc,size-acc,0);
    if(j == -1)
      return j;
    acc += j;
    if(acc == size) break;
  }
  return acc;
}

int initiate_server(HttpRequest* req)
{
  /*Set up socket to send request*/
    
  struct addrinfo hints;
  struct addrinfo *result,*rp;
  int res_socket;
  
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0;
  hints.ai_protocol = 0;
  
  /*Convert number to string*/
  char my_port[10];
  sprintf(my_port,"%d",req->GetPort());
  
  /*Get host name*/
  string h = req->GetHost();
  int hsize = h.size();
  char * host = new char [hsize+1];
  for(int i = 0; i < hsize; i++)
    host[i] = h[i];
  host[hsize] = '\0'; 
  
  //Get IP of host
  getaddrinfo(host, my_port, &hints, &result);
  
  //Look for an IP that works
  for(rp = result; rp != NULL;rp = rp->ai_next)
  {
    
    res_socket = socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
    
    if(res_socket == -1)
      continue;
    
    if(connect(res_socket, rp->ai_addr, rp->ai_addrlen) != -1)
      break;     // Success
      
    close(res_socket);
  }

  if(rp == NULL)
  {
    //Error. Do something here. Couldn't get IP
    //freeaddrinfo(result); // Deallocate dynamic stuff only if iterated
    delete [] host;
    return -1;
  }
  
  /*Got an IP. Return socket*/
  freeaddrinfo(result); // Deallocate dynamic stuff
  delete [] host;
  
  return res_socket;
}

bool check_cached(HttpRequest * req,char* cache)
{
  if(cache != NULL)
  {
      string last_mod;
      string expires;
      HttpResponse cached_res;
      //May want to add exception handling
      char const * d = NULL;
      try
      {
        d = cached_res.ParseResponse(cache,strlen(cache));
      }
      catch(ParseException e)
      {
        cout << "Caught an exception in check_cached!\n";
        char const * ee = e.what();
        cout << ee;
        return false;
      }
      //char const * d = cached_res.ParseResponse(cache,strlen(cache));
      (void) d; // avoid compiler warning
      
      last_mod = cached_res.FindHeader("Last-Modified");
      expires = cached_res.FindHeader("Expires");
      
      //Convert to numbers to compare
      //If Expires is less than todays date, check for update
      bool is_current = compare_times(expires);
      if(is_current)
      {
        return true;
      }
      if(last_mod != "")
      {
        req->ModifyHeader("If-Modified-Since",last_mod);
      }
      return false;
  }
  else
    return false;
}

char const * parse_request(char* data, int size, HttpRequest* req, int socket)
{
  char const * c = NULL;
  try
  {
    c = req->ParseRequest(data,size);
  }
  catch(ParseException e)
  {
    char const * ee = e.what();
    char bad[] = "Request is not GET";
    char bad2[] = "Header line does end with \\r\\n";
    int i = 0;
    int header_neq = 0;
    //int req_neq = 0;
    for(; (bad2[i] != '\0') && (ee[i] != '\0');i++)
    {
      if(bad2[i] != ee[i])
      {
        header_neq = 1;
        break;
      }
    }
    if(header_neq  == 0)
    {
      req->ModifyHeader("Host",req->GetHost());
    }
    for(i = 0; (bad[i] != '\0') && (ee[i] != '\0') && (header_neq == 1);i++)
    {
      if(bad[i] != ee[i])
      {
        /*Send a 400 response*/
        char _my400[] = "HTTP/1.1 400 Bad Request\r\n";//Two more characters?
          
        //TODO Use send here
        send_message(socket,26,_my400);
        
        return NULL;
      }
    }
    /*Send a not supported response*/
    if((header_neq == 1)) //&& (req_neq == 0))
    {
      char _my501[] = "HTTP/1.1 501 Not Implemented\r\n";
        
      //TODO Use send here
      send_message(socket,30,_my501);
      
      return NULL;
    }    
  }
  return c;
}

int get_message(int socket, int size, int maxfd, char* buf)
{
  if((buf == NULL) || (size == 0))
    return -1;
    
  int num_read = 0;
  int total_count = 0;
  int real_size = size;
  
  struct timeval tv;
  fd_set readfds;
  
  for(;;)
  {
    tv.tv_sec = 5; // Timeout after 5 sec
    tv.tv_usec = 0;
    FD_ZERO(&readfds);
    FD_SET(socket,&readfds);
    int n = select(maxfd+1, &readfds,NULL,NULL,&tv);
    if(n < 0)
      return -1;
    if(FD_ISSET(socket,&readfds))
    {
      if((num_read = recv(socket,buf+total_count,real_size-total_count,0)) > 0)
      {
        total_count += num_read;
        if(total_count == real_size)
        {
          char * t = (char *) realloc(buf,sizeof(char)*(real_size+size));
          buf = t;
          real_size += size;
        }
      }
      else
        break; //End of request
    }
    else
      break; // Timeout
  }
  return total_count;
}

void serve_client(int client_socket)
{
  int maxchild = CHILDLIMIT/2;
  int numchilds = 0;
  pid_t pid;
  char * buf = NULL;
  HttpRequest* req;
  bool is_done = false;
  queue<HttpRequest*> q;
  int c_len;
  char * cached_data = NULL;
  bool is_good;
  int special_counter = 0;
  
  start:
  
  while(!is_done)
  {
    buf = (char *)malloc(sizeof(char)*REQUEST_SIZE);
    req = new HttpRequest;
  
    /* Read from Client*/
    
    c_len = get_message(client_socket,REQUEST_SIZE,client_socket,buf);
    if(c_len == 0)
    {
      /*No data*/
      //close(client_socket);
      free(buf);
      free(req);
      goto fin;
    }
  
    /*Parse the request*/
    char const * c = parse_request(buf,c_len,req,client_socket);
  
    if(c == NULL)
    {
      //close(client_socket);
      free(buf);
      free(req);
      continue;
    }
  
    /*Check for Connection:close header*/
    string closer = req->FindHeader("Connection");
    /*if(closer != "close")
      req->ModifyHeader("Connection","close");
    else
      is_done = true;*/
    if(closer == "close")
      is_done = true;
  
    /*Check cache*/
    cached_data = cache_read(req);
  
    is_good = check_cached(req,cached_data);
  
    free(buf);
    buf = (char*)malloc(sizeof(char)*req->GetTotalLength());
    req->FormatRequest(buf);
    c_len = req->GetTotalLength();
    
    q.push(req);
    
    if(!is_done)
    {
      //Fork a child to get data
      if(numchilds == maxchild)
      {
        waitpid(-1, NULL, 0);
        numchilds--;
      }
      pid = fork();
      if(pid == 0)
      {
        //Child
        close(client_socket);
        break;
      }
      if(pid > 0)
      {
        numchilds++;
        if(cached_data != NULL)
          free(cached_data);
        free(buf);
      }
      
    }
  }
  
  if(!is_good)
  {
      char *res_buf = (char *)malloc(sizeof(char)*RESPONSE_SIZE);
      
      int server_socket = initiate_server(req);
      /*TODO Check for negative return*/
      
      int maxfd;
      if(pid == 0)
        maxfd = server_socket;
      else
        maxfd = (server_socket > client_socket) ? server_socket : client_socket;
      
      int val = send_message(server_socket,c_len,buf);
      (void) val; //Avoid compiler warning
      /*TODO Check for negative return*/
      //free(buf);
      
      /*Get response from server*/
      int s_len = get_message(server_socket,RESPONSE_SIZE,maxfd,res_buf);
      /*TODO Check for negative return*/
      
      close(server_socket);
      
      /*Parse Request to see if we got a 304*/
      HttpResponse server_response;
      char const * d = NULL;
      try
      {
        d = server_response.ParseResponse(res_buf,s_len);
      }
      catch(ParseException e)
      {
        cout << "Caught an exception in serve_client!\n";
        char const * ee = e.what();
        cout << ee<<endl;
        cout << res_buf<< endl;
        cout << "QUE?\n";
        cout << buf;
        free(res_buf);
        free(cached_data);
        close(client_socket);
        return;
      }
      free(buf);
      (void) d; //Avoid compiler warning
      if((server_response.GetStatusCode() == "304") && (cached_data != NULL))
      {
        //We have the latest version. Return the cached version and update cache
        update_cache(req,&server_response,cached_data,s_len);
        //int value = send_message(client_socket,strlen(cached_data),cached_data);
        /*TODO Check for negative return*/        
      }
      else
      {
        //Rewrite the cache. Return response to client
        string path = req->GetPath();
        string h = req->GetHost();
        int hsize = h.size();
        int size = path.size();
        char * host = new char [hsize+1];
        char * pathc = new char [size+1];
        for(int i = 0; i < hsize; i++)
          host[i] = h[i];
        host[hsize] = '\0';        
        for(int i = 0; i < size; i++)
          pathc[i] = path[i];
        pathc[size] = '\0';
        
        cache_write(host,pathc,res_buf,s_len);
        delete [] pathc;
        delete [] host;
      }
      free(res_buf);
      if(cached_data != NULL)
        free(cached_data);
  }
  else
  {
    if(cached_data != NULL)
      free(cached_data);
    free(buf);
  }
  
  if(pid == 0)
    _exit(0);
  /*At this point we are ready to send response back to client.
    The response should be in the cache whether it was updated or not.
    If the cache doesn't exist, there must have been an error.*/
    
  //Cache was modified. Gotta get get it again.
  fin:
  /*Wait for children to finish!*/
  while(numchilds != 0)
  {
    wait(NULL);
    numchilds--;
  }
  while(q.size() != 0)
  {
    HttpRequest *req1 = q.front();
    q.pop();
    cached_data = cache_read(req1);
    if(cached_data != NULL)
    {
      int r = send_message(client_socket,strlen(cached_data),cached_data);
      (void) r; // Avoid compiler warning
      /*TODO: Check for negative return*/
      free(cached_data);
    }
    free(req1);
  }
  
  if(!is_done && (special_counter != 2)) //Client isn't DONE! Unless we've been here before 
  {
    special_counter++;
    goto start;
  }
  //All done with this client
  close(client_socket);
}

int main (int argc, char *argv[])
{
  pid_t pid;
  
  struct sockaddr_in myAddr;
  memset(&myAddr,0,sizeof(myAddr));
  
  int sockfd = socket(AF_INET,SOCK_STREAM,0);
  
  myAddr.sin_family = AF_INET;
  myAddr.sin_port = htons(14805);
  //myAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  inet_pton(AF_INET,"127.0.0.1",&myAddr.sin_addr);
  
  bind(sockfd,(struct sockaddr *) &myAddr, sizeof(myAddr));
  
  listen(sockfd,10);
  
  for(;;)
  {
    int sockfd2 = accept(sockfd,NULL,NULL);
    
    if(sockfd2 < 0)
    {
      cout << "accept() failed\n";
      close(sockfd);
      return 1;
    }
    
    if(CHILDLIMIT != 0)
    {
      pid = blocking_fork();
      //cout << nChilds << " children left out of 5\n";
      if(pid == -1)
      {
        cout << "Error: fork()ing error\n";
        close(sockfd);
        return 1;
      }
      else if(pid != 0)
      {
        //Parent
        close(sockfd2);
        continue;
      }
      else
      {
        //Child
        close(sockfd);
        serve_client(sockfd2);
        //cout << "GONNA DIE!" << getpid() << endl;
        _exit(0);
      }
    }
    else
      serve_client(sockfd2);
  }
  /*for(int i = 0; i < MAXTHREADS; i++)
    threads[i]->join();*/
  close(sockfd);
  
  return 0;
}
