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

#include "http-request.h"
#include "http-response.h"
#include "http-headers.h"

using namespace std;

#define RESPONSE_SIZE 10000
#define REQUEST_SIZE 8000
#define CHILDLIMIT 5

static int nChilds = 0;

/*Returns true if cache is up-to-date*/
bool compare_times(string expires)
{
  if(expires == "")
    return false;
  time_t curr = time(NULL);
  struct tm exp;
  char const * c = strptime(expires.c_str(),"%a, %d %b %Y %H:%M:%S GMT",&exp);
  if(c != NULL)
  {
    time_t expired = mktime(&exp);
    if(expired > curr)
      return true;
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

char* send_request(char* my_buf, char *host, size_t my_reqLen,int * res_len,int c_socket,
  HttpRequest* hr,char * cached_data)
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
  sprintf(my_port,"%d",hr->GetPort());
  
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
    //TODO
    //freeaddrinfo(result); // Deallocate dynamic stuff only if iterated
    return NULL;
  }
  
  freeaddrinfo(result); // Deallocate dynamic stuff
   
  /*Send Request*/
  cout << "Sending the following\n" << my_buf;
  int acc = 0;
  for(;;)
  {
    int j = send(res_socket,my_buf+acc,my_reqLen-acc,0);
    if(j == -1)
    {
      cout << "send returned -1!!\n";
      close(res_socket);
      return NULL;
    }
    acc += j;
    if(my_reqLen == (size_t) acc) break;
  }
  
  /*Recieve Response*/
  char *res_buf = (char *)malloc(sizeof(char)*RESPONSE_SIZE);
  int res_read = 0;
  int total_count = 0;
  int real_size = RESPONSE_SIZE;
  
  struct timeval tv;
  fd_set readfds;
  int maxfd = (res_socket > c_socket) ? res_socket : c_socket; 
  
  for(;;)
  {
    tv.tv_sec = 5; // Timeout after 5 sec
    tv.tv_usec = 0;
    FD_ZERO(&readfds);
    FD_SET(res_socket,&readfds);
    
    int n = select(maxfd+1, &readfds,NULL,NULL,&tv);
    if(n < 0)
    {
      cout << "select failed!!\n";
      free(res_buf);
      close(res_socket);
      return NULL;
    }
    if(FD_ISSET(res_socket,&readfds))
    {
      if((res_read = recv(res_socket,res_buf+total_count,real_size-total_count,0)) > 0)
      {
        total_count += res_read;
        if(total_count == real_size)
        {
          char * t = (char *) realloc(res_buf,sizeof(char)*(real_size+RESPONSE_SIZE));
          res_buf = t;
          real_size += RESPONSE_SIZE;
          }
      }
      else
        break; //End of request
    }
    else
      break; // Timeout
  }
  
  *res_len = total_count;
  
  /*If we did not receive anything return null*/
  if(total_count == 0)
  {
    free(res_buf);
    res_buf = NULL;
  }
  
  shutdown(res_socket,SHUT_RDWR);
  close(res_socket);
  
  /*Parse Request to see if we got a 304*/
  HttpResponse server_response;
  char const * d = server_response.ParseResponse(res_buf,total_count);
  (void) d; //Avoid compiler warning
  if(server_response.GetStatusCode() == "304")
  {
    //We have the latest version. Return the cached version.
    return cached_data;
  }
  //cout << "Gonna create a cache!!\n";
  cache_write(host,res_buf,total_count);
  return res_buf;
}

void parse_request(int sockfd2)
{
    char * buf = (char *)malloc(sizeof(char)*REQUEST_SIZE);
    HttpRequest req;
  
    int num_read = 0;
    int total_count = 0;
    //int real_size = REQUEST_SIZE;
    //fcntl(sockfd2,F_SETFL,O_NONBLOCK);
    while((num_read = read(sockfd2,buf+total_count,REQUEST_SIZE)) == REQUEST_SIZE)
    {
      total_count += num_read;
      char * t = (char *) realloc(buf,sizeof(char)*(total_count+REQUEST_SIZE));
      buf = t;
    }
    /*while((num_read = recv(sockfd2,buf+total_count,real_size-total_count,0)) > 0)
    {
      total_count += num_read;
      if(total_count == real_size)
      {
        char * t = (char *) realloc(buf,sizeof(char)*(real_size+REQUEST_SIZE));
        buf = t;
        real_size += REQUEST_SIZE;
      }
    }*/
    if(num_read > 0)
      total_count += num_read;
    if(total_count == 0)
    {
        shutdown(sockfd2,SHUT_RDWR);
        close(sockfd2);
        free(buf);
        return;
    }
    
    char const * c = NULL;
    
    //Parse the request
    try
    {
      c = req.ParseRequest(buf,total_count);
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
        req.ModifyHeader("Host",req.GetHost());
      }
      for(i = 0; (bad[i] != '\0') && (ee[i] != '\0') && (header_neq == 1);i++)
      {
        if(bad[i] != ee[i])
        {
          /*Send a 400 response*/
          char _my400[] = "HTTP/1.1 400 Bad Request\r\n";
          
          //TODO Use send here
          write(sockfd2,_my400,26);
          
          shutdown(sockfd2,SHUT_RDWR);
          close(sockfd2);
          free(buf);
          return;
        }
      }
      /*Send a not supported response*/
      if((header_neq == 1)) //&& (req_neq == 0))
      {
        char _my501[] = "HTTP/1.1 501 Not Implemented\r\n";
        
        //TODO Use send here
        write(sockfd2,_my501,30);
        
        shutdown(sockfd2,SHUT_RDWR);
        close(sockfd2);
        free(buf);
        return;
      }    
    }
    
    (void) c;  // Avoid compiler warning
    
    /* Add Connection close header*/
    req.ModifyHeader("Connection","close");
    
    char * my_buf = NULL;
    int res_len;
    string temp;
    int size;
    char * tempo;
    
    //If cached, get last modified date
    char * cached_data = cache_read(&req);
    if(cached_data != NULL)
    {
      string last_mod;
      string expires;
      HttpResponse cached_res;
      //May want to add exception handling
      char const * d = cached_res.ParseResponse(cached_data,strlen(cached_data));
      (void) d; // avoid compiler warning
      
      last_mod = cached_res.FindHeader("Last-Modified");
      expires = cached_res.FindHeader("Expires");
      
      /*TODO*/
      //Convert to numbers to compare
      //If Expires is less than todays date, check for update
      //Else use set my_buf to cache (use strlen stuff too) and goto fin:
      bool is_current = compare_times(expires);
      if(is_current)
      {
        my_buf = cached_data;
        res_len = strlen(cached_data);
        goto fin;
      }
      if(last_mod != "")
      {
        //cout << date << endl;
        req.ModifyHeader("If-Modified-Since",last_mod);
      }
    }
    free(buf);
    buf = (char *)malloc(sizeof(char)*req.GetTotalLength());
    req.FormatRequest(buf);
    
    /*Copy host name into a char buffer*/
    temp = req.GetHost();
    size = temp.size();
    tempo = new char [size+1];
    for(int i = 0; i < size; i++)
      tempo[i] = temp[i];
    tempo[size] = '\0';
    
    total_count = req.GetTotalLength();
    
    //Send request to server
    my_buf = send_request(buf, tempo, total_count,&res_len,sockfd2,&req,cached_data);
    delete [] tempo;
    
    if(my_buf == NULL)
    {
      //May want to write to client here
      shutdown(sockfd2,SHUT_RDWR);
      close(sockfd2);
      free(buf);
      return;
    }
    
    //Respond back to client
    fin:
    
    int acc = 0;
    for(;;)
    {
      int j = send(sockfd2,my_buf+acc,res_len-acc,0);
      if(j == -1)
      {
        cout << "send returned -1!!\n";
        close(sockfd2);
        free(buf);
        free(my_buf);
        return;
      }
      acc += j;
      if(acc == res_len) break;
    }
    
    //Delete Allocated Buffers
    free(my_buf);
    free(buf);
    
    //Close sockets
    shutdown(sockfd2,SHUT_RDWR);
    close(sockfd2);
}

int main (int argc, char *argv[])
{
  pid_t pid;
  
  struct sockaddr_in myAddr;
  memset(&myAddr,0,sizeof(myAddr));
  
  int sockfd = socket(AF_INET,SOCK_STREAM,0);
  
  myAddr.sin_family = AF_INET;
  myAddr.sin_port = htons(1100);
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
        parse_request(sockfd2);
        //cout << "GONNA DIE!" << getpid() << endl;
        _exit(0);
      }
    }
    else
      parse_request(sockfd2);
  }
  /*for(int i = 0; i < MAXTHREADS; i++)
    threads[i]->join();*/
  close(sockfd);
  
  return 0;
}
