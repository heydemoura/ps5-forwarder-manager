// Host-only shim of net::get using system libcurl, to exercise the real
// sgdb client code against the live API.
#include "net/http.hpp"
#include <curl/curl.h>
namespace net {
static size_t wr(char*d,size_t s,size_t n,void*u){auto*o=(std::vector<unsigned char>*)u;o->insert(o->end(),(unsigned char*)d,(unsigned char*)d+s*n);return s*n;}
void global_init(){ curl_global_init(CURL_GLOBAL_DEFAULT); }
Response get(const std::string&url,const std::string&bearer,long){
  Response r; CURL*e=curl_easy_init();
  curl_easy_setopt(e,CURLOPT_URL,url.c_str());
  curl_easy_setopt(e,CURLOPT_FOLLOWLOCATION,1L);
  curl_easy_setopt(e,CURLOPT_USERAGENT,"ps5fwdgen/1.0");
  curl_easy_setopt(e,CURLOPT_WRITEFUNCTION,wr);
  curl_easy_setopt(e,CURLOPT_WRITEDATA,&r.body);
  struct curl_slist*h=nullptr; std::string a;
  if(!bearer.empty()){a="Authorization: Bearer "+bearer;h=curl_slist_append(h,a.c_str());curl_easy_setopt(e,CURLOPT_HTTPHEADER,h);}
  CURLcode c=curl_easy_perform(e); r.curl_code=(int)c;
  if(c==CURLE_OK) curl_easy_getinfo(e,CURLINFO_RESPONSE_CODE,&r.status);
  else {r.status=0;r.error=curl_easy_strerror(c);}
  if(h)curl_slist_free_all(h); curl_easy_cleanup(e); return r;
}
Response post_file(const std::string&,const std::string&,const std::string&,const std::vector<unsigned char>&,long){Response r;return r;}
}
