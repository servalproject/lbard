/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2015 Serval Project Inc.

This program monitors a local Rhizome database and attempts
to synchronise it over low-bandwidth declarative transports, 
such as bluetooth name or wifi-direct service information
messages.  It is intended to give a high priority to MeshMS
converations among nearby nodes.

The design is fully asynchronous, so a call to the update_my_message()
function from time to time should be all that is required.


This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <curl/curl.h>

struct peer_state {
  unsigned char *last_message;
  time_t last_message_time;
};

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

int load_rhizome_db(char *servald_server)
{
  CURL *curl;
  CURLcode result_code;
  curl=curl_easy_init();
  if (!curl) return -1;
  char url[8192];
  snprintf(url,8192,"http://%s/restful/rhizome/bundlelist.json",
	   servald_server);
  curl_easy_setopt(curl, CURLOPT_URL, "http://example.com");
  FILE *f=fopen("bundle.list","w");
  if (!f) {
    curl_easy_cleanup(curl);
    return -1;
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  result_code=curl_easy_perform(curl);
  fclose(f);
  if(result_code!=CURLE_OK) {
    curl_easy_cleanup(curl);
    return -1;
  }

  curl_easy_cleanup(curl);
  return 0;
}

int update_my_message()
{
  return 0;
}

int main(int argc, char **argv)
{
  while(1) {
    update_my_message();
    // The time it takes for a Bluetooth scan
    sleep(12);
  }
}
