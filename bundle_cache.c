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
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <dirent.h>
#include <assert.h>

#include "lbard.h"

char *bid_of_cached_bundle=NULL;
long long cached_version=0;
int cached_manifest_len=0;
unsigned char *cached_manifest=NULL;
int cached_body_len=0;
unsigned char *cached_body=NULL;

int prime_bundle_cache(int bundle_number,char *prefix,
		       char *servald_server, char *credential)
{
  if (bundle_number<0) return -1;
  
  if ((!bid_of_cached_bundle)
      ||strcasecmp(bundles[bundle_number].bid,bid_of_cached_bundle)) {
    // Cache is invalid - release
    if (bid_of_cached_bundle) {
      free(bid_of_cached_bundle); bid_of_cached_bundle=NULL;
      free(cached_manifest);
      free(cached_body);
    }

    // Load bundle into cache
    char url[8192];
    char filename[1024];
    CURL *curl;
    CURLcode result_code;
    curl=curl_easy_init();
    if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERPWD, credential);  
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000);

    snprintf(url,8192,"http://%s/restful/rhizome/%s.rhm",
	     servald_server,bundles[bundle_number].bid);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    snprintf(filename,1024,"%smanifest",prefix);
    unlink(filename);
    FILE *f=fopen(filename,"w");
    if (!f) {
      curl_easy_cleanup(curl);
      fprintf(stderr,"could not open output file.\n");
      return -1;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    result_code=curl_easy_perform(curl);
    fclose(f);
    if(result_code!=CURLE_OK) {
      curl_easy_cleanup(curl);
      fprintf(stderr,"curl request failed. URL:%s\n",url);
      fprintf(stderr,"libcurl: %s\n",curl_easy_strerror(result_code));
      return -1;
    }

    f=fopen(filename,"r");
    cached_manifest=malloc(8192);
    cached_manifest_len=fread(cached_manifest,1,8192,f);
    cached_manifest=realloc(cached_manifest,cached_manifest_len);    
    fclose(f);
    fprintf(stderr,"  manifest is %d bytes long.\n",cached_manifest_len);
    
    
    snprintf(url,8192,"http://%s/restful/rhizome/%s/raw.bin",
	     servald_server,bundles[bundle_number].bid);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    snprintf(filename,1024,"%sraw",prefix);
    unlink(filename);
    f=fopen(filename,"w");
    if (!f) {
      curl_easy_cleanup(curl);
      fprintf(stderr,"could not open output file.\n");
      return -1;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    result_code=curl_easy_perform(curl);
    fclose(f);
    if(result_code!=CURLE_OK) {
      curl_easy_cleanup(curl);
      fprintf(stderr,"curl request failed. URL:%s\n",url);
      fprintf(stderr,"libcurl: %s\n",curl_easy_strerror(result_code));
      return -1;
    }

    // XXX - This transport only allows bundles upto 1MB!
    f=fopen(filename,"r");
    cached_body=malloc(1024*1024);
    cached_body_len=fread(cached_body,1,1024*1024,f);
    cached_body=realloc(cached_body,cached_body_len);    
    fclose(f);
    fprintf(stderr,"  body is %d bytes long.\n",cached_body_len);

    bid_of_cached_bundle=strdup(bundles[bundle_number].bid);

    cached_version=bundles[bundle_number].version;
    
    fprintf(stderr,"Cached manifest and body for %s\n",
	    bundles[bundle_number].bid);
  }
  
  return 0;
}
