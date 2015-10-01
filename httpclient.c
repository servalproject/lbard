#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <assert.h>
#include <netdb.h>
#include <time.h>
#include <sys/time.h>

#include "serial.h"

int connect_to_port(char *host,int port)
{
  struct hostent *hostent;
  hostent = gethostbyname(host);
  if (!hostent) {
    return -1;
  }

  struct sockaddr_in addr;  
  addr.sin_family = AF_INET;     
  addr.sin_port = htons(port);   
  addr.sin_addr = *((struct in_addr *)hostent->h_addr);
  bzero(&(addr.sin_zero),8);     

  int sock=socket(AF_INET, SOCK_STREAM, 0);
  if (sock==-1) {
    perror("Failed to create a socket.");
    return -1;
  }

  if (connect(sock,(struct sockaddr *)&addr,sizeof(struct sockaddr)) == -1) {
    perror("connect() to port failed");
    close(sock);
    return -1;
  }
  return sock;
}

int num_to_char(int n)
{
  assert(n>=0); assert(n<64);
  if (n<26) return 'A'+(n-0);
  if (n<52) return 'a'+(n-26);
  if (n<62) return '0'+(n-52);
  switch(n) {
  case 62: return '+'; 
  case 63: return '/';
  default: return -1;
  }
}

int base64_append(char *out,int *out_offset,unsigned char *bytes,int count)
{
  int i;
  for(i=0;i<count;i+=3) {
    int n=4;
    unsigned int b[30];
    b[0]=bytes[i];
    if ((i+2)>=count) { b[2]=0; n=3; } else b[2]=bytes[i+2];
    if ((i+1)>=count) { b[1]=0; n=2; } else b[1]=bytes[i+1];
    out[(*out_offset)++] = num_to_char((b[0]&0xfc)>>2);
    out[(*out_offset)++] = num_to_char( ((b[0]&0x03)<<4) | ((b[1]&0xf0)>>4) );
    if (n==2) {
      out[(*out_offset)++] = '=';
      out[(*out_offset)++] = '=';
      return 0;
    }
    out[(*out_offset)++] = num_to_char( ((b[1]&0x0f)<<2) | ((b[2]&0xc0)>>6) );
    if (n==3) {
      out[(*out_offset)++] = '=';
      return 0;
    }
    out[(*out_offset)++] = num_to_char((b[2]&0x3f)>>0);
  }
  return 0;
}

// From os.c in serval-dna
long long gettime_ms()
{
  struct timeval nowtv;
  // If gettimeofday() fails or returns an invalid value, all else is lost!
  if (gettimeofday(&nowtv, NULL) == -1)
    return -1;
  if (nowtv.tv_sec < 0 || nowtv.tv_usec < 0 || nowtv.tv_usec >= 1000000)
    return -1;
  return nowtv.tv_sec * 1000LL + nowtv.tv_usec / 1000;
}


int http_get_simple(char *server_and_port, char *auth_token,
		    char *path, FILE *outfile, int timeout_ms,
		    long long *last_read_time)
{
  // Send simple HTTP request to server, and write result into outfile.

  char server_name[1024];
  int server_port=-1;

  if (sscanf(server_and_port,"%[^:]:%d",server_name,&server_port)!=2) return -1;

  long long timeout_time=gettime_ms()+timeout_ms;
  
  if (strlen(auth_token)>500) return -1;
  if (strlen(path)>500) return -1;
  
  char request[2048];
  char authdigest[1024];
  int zero=0;
  
  bzero(authdigest,1024);
  base64_append(authdigest,&zero,(unsigned char *)auth_token,strlen(auth_token));

  // Build request
  snprintf(request,2048,
	   "GET %s HTTP/1.1\n"
	   "Authorization: Basic %s\n"
	   "Host: %s:%d\n"
	   "Accept: */*\n"
	   "\n",
	   path,
	   authdigest,
	   server_name,server_port);

  int sock=connect_to_port(server_name,server_port);
  if (sock<0) return -1;

  write_all(sock,request,strlen(request));

  // Read reply, streaming output to file after we have skipped the header
  int http_response=-1;
  char line[1024];
  int len=0;
  int empty_count=0;
  int content_length=-1;
  set_nonblock(sock);
  int r;
  while(len<1024) {
    r=read_nonblock(sock,&line[len],1);
    if (r==1) {
      if ((line[len]=='\n')||(line[len]=='\r')) {
	if (len) empty_count=0; else empty_count++;
	line[len+1]=0;
	// if (len) printf("Line of response: %s\n",line);
	if (sscanf(line,"Content-Length: %d",&content_length)==1) {
	  // got content length
	}
	if (sscanf(line,"HTTP/1.0 %d",&http_response)==1) {
	  // got http response
	}
	len=0;
	// Have we found end of headers?
	if (empty_count==3) break;
      } else len++;
    } else usleep(1000);
    if (gettime_ms()>timeout_time) {
      // If still in header, just quit on timeout
      close(sock);
      return -1;
    }
  }

  // Got headers, read body and write to file
  // printf("  reading body...\n");

  int rxlen=0;
  r=0;
  while(r>-1) {
    r=read_nonblock(sock,line,1024);
    if (r>0) {
      // printf("read %d body bytes.\n",r);
      if (last_read_time) *last_read_time=gettime_ms();
      fwrite(line,r,1,outfile);
      rxlen+=r;
      if (content_length>-1) {
	if (rxlen>=content_length) break;
      }
    } else usleep(1000);

    if (gettime_ms()>timeout_time) {
      close(sock);
      return http_response;
    }
    
  }
  
  close(sock);
  
  return http_response;
}

int http_post_bundle(char *server_and_port, char *auth_token,
		     char *path,
		     unsigned char *manifest_data, int manifest_length,
		     unsigned char *body_data, int body_length,
		    int timeout_ms)
{

  char server_name[1024];
  int server_port=-1;

  // Limit bundle size to 5MB via this transport, to limit memory consumption.
  if (body_length>(5*1024*1024)) return -1;
  
  if (sscanf(server_and_port,"%[^:]:%d",server_name,&server_port)!=2) return -1;

  long long timeout_time=gettime_ms()+timeout_ms;
  
  if (strlen(auth_token)>500) return -1;
  if (strlen(path)>500) return -1;
  
  char request[8192+body_length];
  char authdigest[1024];
  int zero=0;

  bzero(authdigest,1024);
  base64_append(authdigest,&zero,(unsigned char *)auth_token,strlen(auth_token));

  // Generate random content dividor token
  unsigned long long unique;
  unique=random(); unique=unique<<32; unique|=random();
  
  char manifest_header[1024];
  snprintf(manifest_header,1024,
	   "Content-Disposition: form-data; name=\"manifest\"\r\n"
	   "Content-Length: %d\r\n"
	   "Content-Type: rhizome/manifest\r\n"
	   "\r\n", manifest_length);
  char body_header[1024];
  snprintf(body_header,1024,
	   "Content-Disposition: form-data; name=\"payload\"\r\n"
	   "Content-Length: %d\r\n"
	   "Content-Type: binary/data\r\n"
	   "\r\n",
	   body_length);

  char boundary_string[1024];
  snprintf(boundary_string,1024,"------------------------%016llx",unique);
  int boundary_len=strlen(boundary_string);

  // Calculate content length
  int content_length=0
    +2+boundary_len+2
    +strlen(manifest_header)
    +manifest_length+2
    +2+boundary_len+2
    +strlen(body_header)
    +body_length+2
    +2+boundary_len+2
    +2;   // not sure where we have missed this last 2, but it is needed to reconcile
  
  // Build request
  int total_len = snprintf(request,8192,
			   "POST %s HTTP/1.1\r\n"
			   "Authorization: Basic %s\r\n"
			   "Host: %s:%d\r\n"
			   "Content-Length: %d\r\n"
			   "Accept: */*\r\n"
			   "Content-Type: multipart/form-data; boundary=%s\r\n"
			   "\r\n"
			   "--%s\r\n"
			   "%s",
			   path,
			   authdigest,
			   server_name,server_port,
			   content_length,
			   boundary_string,
			   boundary_string,
			   manifest_header);
  bcopy(manifest_data,&request[total_len],manifest_length);

  int subtotal_len=total_len;
  total_len=total_len+manifest_length;
  total_len+=snprintf(&request[total_len],8192+body_length-total_len,  
			   "\r\n"
			   "--%s\r\n"
			   "%s",
			   boundary_string,
			   body_header);
  bcopy(body_data,&request[total_len],body_length);
  total_len=total_len+body_length;
  total_len+=snprintf(&request[total_len],8192+body_length-total_len,
	   "\r\n"
	   "--%s--\r\n",
	   boundary_string);

  fprintf(stderr,"  content_length was calculated at %d bytes, total_len=%d\n",
	  content_length,total_len);
  int present_len=2+boundary_len+2+strlen(manifest_header);
  fprintf(stderr,
	  "    subtotal_len=%d, difference+present=%d (should match content_length)\n",
	  subtotal_len,total_len-subtotal_len+present_len);

  int sock=connect_to_port(server_name,server_port);
  if (sock<0) return -1;

  // Write request
  write_all(sock,request,total_len);

  // Read reply, streaming output to file after we have skipped the header
  int http_response=-1;
  char line[1024];
  int len=0;
  int empty_count=0;
  set_nonblock(sock);
  int r;
  while(len<1024) {
    r=read_nonblock(sock,&line[len],1);
    if (r==1) {
      if ((line[len]=='\n')||(line[len]=='\r')) {
	if (len) empty_count=0; else empty_count++;
	line[len+1]=0;
	// if (len) printf("Line of response: %s\n",line);
	if (sscanf(line,"HTTP/1.0 %d",&http_response)==1) {
	  // got http response
	  fprintf(stderr,"  HTTP response from Rhizome for new bundle is: %d\n",http_response);
	}
	len=0;
	// Have we found end of headers?
	if (empty_count==3) break;
      } else len++;
    } else usleep(1000);
    if (gettime_ms()>timeout_time) {
      // If still in header, just quit on timeout
      close(sock);
      return -1;
    }
  }
  return http_response;
  
}
