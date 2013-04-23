/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

using namespace std;

int main (int argc, char *argv[])
{
  // command line parsing
  
  struct sockaddr_in myAddr;
  memset(&myAddr,0,sizeof(myAddr));
  
  int sockfd = socket(AF_INET,SOCK_STREAM,0);
  
  myAddr.sin_family = AF_INET;
  myAddr.sin_port = htons(1100);
  myAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  
  bind(sockfd,(struct sockaddr *) &myAddr, sizeof(myAddr));
  
  listen(sockfd,1);
  
  int sockfd2 = accept(sockfd,NULL,NULL);
  
  char * buf = new char[100];
  
  int num_read = read(sockfd2,buf,100);
  
  if(num_read < 100)
  {
    buf[num_read] = '\0';
  }
  else{
    buf[99] = '\0';
  }
  
  for(int i = 0; buf[i] != '\0'; i++){
    cout << buf[i];
  }
  
  cout << endl << "Read "<<num_read<<" bytes\n";
  
  delete [] buf;
  
  close(sockfd2);
  close(sockfd);
  
  return 0;
}
