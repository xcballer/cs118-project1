/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>


#include <stdlib.h>
#include "http-request.h"
#include "http-response.h"
#include "http-headers.h"

using namespace std;

#define RESPONSE_SIZE 100000
#define REQUEST_SIZE 80000

const int NTHREADS = 10;
pthread_t threads[NTHREADS];

struct data {
  //include cache and mutex pointers
  int socketfd;
  
};
void routine (void* dat){
  pthread_exit(NULL);
}

char* send_request(char* my_buf, char *req, size_t my_reqLen,int * res_len)
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
  
  //cout << req << endl;
  getaddrinfo(req, "80", &hints, &result);
  //if s < 0 error
  
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
    //Error. Do something here.
    cout << "NULL!\n";
    return NULL;
  }
   
  /*Send Request*/
  write(res_socket,my_buf,my_reqLen);
  
  /*Recieve Response and send response*/
  //char * res_buf = new char[RESPONSE_SIZE];
  char *res_buf = (char *)malloc(sizeof(char)*RESPONSE_SIZE);
  int res_read = 0;
  int total_count = 0;
  int real_size = RESPONSE_SIZE;
  /*while((res_read = read(res_socket,res_buf+total_count,RESPONSE_SIZE)) == RESPONSE_SIZE)
  {
    cout << "response is big!" << endl;
    total_count += res_read;
    char * t = (char *)realloc(res_buf,sizeof(char)*(total_count+RESPONSE_SIZE));
    res_buf = t;
  }*/
  
  while((res_read = read(res_socket,res_buf+total_count,real_size-total_count)) > 0)
  {
    total_count += res_read;
    if(total_count == real_size)
    {
      char * t = (char *) realloc(res_buf,sizeof(char)*(real_size+RESPONSE_SIZE));
      res_buf = t;
      real_size += RESPONSE_SIZE;
    }
  }
  
  //total_count += res_read;
  res_buf[total_count] = '\0';
  *res_len = total_count;
  //cout << "Length of response is: " << total_count << " " << RESPONSE_SIZE <<endl;
  //cout << res_buf << endl;
  
  HttpResponse resp;
    
  char const * c = resp.ParseResponse(res_buf,total_count);
  (void) c;
    
  //cout << res_buf << endl;
  
  shutdown(res_socket,SHUT_RDWR);
  close(res_socket);
  
  return res_buf;
}

void parse_request(int sockfd2)
{
  //cout << "entered parse_request\n";
    char * buf = (char *)malloc(sizeof(char)*REQUEST_SIZE);
    HttpRequest req;
  
    int num_read = 0;
    int total_count = 0;
    //int real_size = REQUEST_SIZE;
    while((num_read = read(sockfd2,buf+total_count,REQUEST_SIZE)) == REQUEST_SIZE)
    {
      total_count += num_read;
      char * t = (char *) realloc(buf,sizeof(char)*(total_count+REQUEST_SIZE));
      buf = t;
    }
  
    total_count += num_read;
    buf[total_count] = '\0';
    //cout << total_count<< " "<< real_size;
    char const * c = NULL;
    try
    {
      c = req.ParseRequest(buf,total_count);
    }
    catch(ParseException e)
    {
      char const * ee = e.what();
      char bad[] = "Request is not GET";
      int i = 0;
      for(; (bad[i] != '\0') && (ee[i] != '\0');i++)
      {
        if(bad[i] != ee[i])
        {
          /*Send a 404 response*/
          cout << "404\n";
          return;
        }
      }
      /*Send a not supported response*/
      cout << "Not supported\n";
      return;
    }
    
    (void) c;  // Avoid compiler warning
    
    req.ModifyHeader("Connection","close");
    free(buf);
    buf = (char *)malloc(sizeof(char)*req.GetTotalLength());
    req.FormatRequest(buf);

    char * my_buf = '\0';
    
    string temp = req.GetHost();
    int size = temp.size();
    char * tempo = new char [size+1];
    for(int i = 0; i < size; i++)
      tempo[i] = temp[i];
    tempo[size] = '\0';
    
    //send_response(my_buf, tempo, my_reqLen);
    //cout << total_count<< endl;
    //cout << buf << endl;
    
    int res_len;
    total_count = req.GetTotalLength();
    my_buf = send_request(buf, tempo, total_count,&res_len);
    if(my_buf == NULL)
      return;
    
    //Respond back to client
    write(sockfd2,my_buf,res_len);
    
    //Delete Allocated Buffers
    
    free(my_buf);
    free(buf);
    
    //Close sockets
    shutdown(sockfd2,SHUT_RDWR);
    close(sockfd2);
}

int main (int argc, char *argv[])
{
  // command line parsing
  
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
    
    // THREAD ON THIS FUNCTION?
    parse_request(sockfd2);
  }
  
  close(sockfd);
  
  return 0;
}
