/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
//#include <boost/thread.hpp>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>


#include <stdlib.h>
#include "http-request.h"
#include "http-response.h"
#include "http-headers.h"

using namespace std;

#define RESPONSE_SIZE 100000
#define REQUEST_SIZE 80000
#define CHILDLIMIT 5

static int nChilds = 0;
/*#ifndef BOOST_SYSTEM_NO_DEPRECATED
#define BOOST_STSTEM_NO_DEPRECATED 1
#endif*/

//const int MAXTHREADS = 10;
//boost::thread* threads[MAXTHREADS];
/*struct Data {
  //include cache and mutex pointers
  //i want to include the pid num as well
  int id;
  int socketfd;
  
};*/

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
          cout << "Entered blocking fork!!!\n";
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
  freeaddrinfo(result); // Deallocate dynamic stuff
    return NULL;
  }
  
  freeaddrinfo(result); // Deallocate dynamic stuff
   
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
  //res_buf[total_count] = '\0';
  *res_len = total_count;
  //cout << "Length of response is: " << total_count << " " << RESPONSE_SIZE <<endl;
  //cout << res_buf << endl;
  
  //HttpResponse resp;
    
  //char const * c = resp.ParseResponse(res_buf,total_count);
  //(void) c;
    
  //cout << res_buf << endl;
  
  shutdown(res_socket,SHUT_RDWR);
  close(res_socket);
  
  return res_buf;
}

void parse_request(int sockfd2)
{
  cout << "Entered parse_request! PID: " << getpid()<<endl;
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
    if(total_count == 0)
    {
        shutdown(sockfd2,SHUT_RDWR);
        close(sockfd2);
        free(buf);
        return;
    }
    //buf[total_count] = '\0';
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
          cout << "Sending a 400\n";
          cout << buf;
          cout << ee<<endl;
          char _my400[] = "HTTP/1.1 400 Bad Request\r\n";
          cout << "Length of my message is: "<<strlen(_my400)<<endl;
          cout <<_my400;
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
        cout << "Sending a not supported\n";
        cout << buf;
        cout << ee<<endl;
        char _my501[] = "HTTP/1.1 501 Not Implemented\r\n";
        cout << "Length of my message is: "<<strlen(_my501)<<endl;
        cout <<_my501;
        write(sockfd2,_my501,30);
        
        shutdown(sockfd2,SHUT_RDWR);
        close(sockfd2);
        free(buf);
        return;
      }    
    }
    
    (void) c;  // Avoid compiler warning
    
    req.ModifyHeader("Connection","close");
    free(buf);
    buf = (char *)malloc(sizeof(char)*req.GetTotalLength());
    req.FormatRequest(buf);
    //cout << "Formatted request\n";
    //cout << buf;
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
    if(my_buf == NULL || res_len == 0)
    {
      shutdown(sockfd2,SHUT_RDWR);
      close(sockfd2);
      free(buf);
      return;
    }
    
    //Respond back to client
    write(sockfd2,my_buf,res_len);
    
    //Delete Allocated Buffers
    delete [] tempo;
    free(my_buf);
    free(buf);
    
    //Close sockets
    shutdown(sockfd2,SHUT_RDWR);
    close(sockfd2);
}

/* void routine (Data* dat){
    parse_request(dat->socketfd);
  }*/

int main (int argc, char *argv[])
{
  //typedef std::chrono::duration<int> seconds_type;
  //thread initialization
  /*for(int i = 0; i < MAXTHREADS; i++){
    threads[i] = NULL;
  }
  int numThreads = 0; */
  
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
    /*else{
      if(numThreads == MAXTHREADS){
        for(int i = 0; i < MAXTHREADS; i++){
          threads[i]->join();
          threads[i] = NULL;
          numThreads--;
        }  
        //run clean up routine
      }
      else{
        for(int i = 0; i < MAXTHREADS; i++){
          if(threads[i] == NULL){
            Data* newD = new Data();
            newD->id = numThreads;
            newD->socketfd = sockfd2;
            threads[i] = new boost::thread(routine, newD);
          //some error check on er
            numThreads++;
          }
        }
      }
      //i dont think there has to be an else
      
    }*/
    
    if(CHILDLIMIT != 0)
    {
      pid = blocking_fork();
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
        parse_request(sockfd2);
        cout << "GONNA DIE!" << getpid() << endl;
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
