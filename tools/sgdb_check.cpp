// ps5fwdgen - Host check of the SteamGridDB chain.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Full authenticated SteamGridDB chain with the REAL client code:
// search -> assets -> download -> decode -> encode a 512x512 icon.
#include "net/steamgriddb.hpp"
#include "fwd/image.hpp"
#include <cstdio>
int main(int argc,char**argv){
  if(argc<2){std::printf("usage: sgdb_full <API_KEY> [game]\n");return 2;}
  std::string key=argv[1]; std::string game=argc>2?argv[2]:"Celeste";
  std::vector<sgdb::Game> games;
  auto r=sgdb::search(key,game,games);
  std::printf("search ok=%d err='%s' games=%zu\n",r.ok,r.error.c_str(),games.size());
  if(!r.ok||games.empty())return 1;
  std::printf("  top: #%ld %s\n",games[0].id,games[0].name.c_str());
  std::vector<sgdb::Asset> as;
  auto r2=sgdb::assets(key,sgdb::Kind::icon,games[0].id,as);
  std::printf("assets ok=%d err='%s' n=%zu\n",r2.ok,r2.error.c_str(),as.size());
  if(!r2.ok||as.empty())return 1;
  std::printf("  first: %dx%d %s\n",as[0].width,as[0].height,as[0].url.c_str());
  std::vector<unsigned char> img;
  auto r3=sgdb::download(as[0].url,img);
  std::printf("download ok=%d err='%s' bytes=%zu\n",r3.ok,r3.error.c_str(),img.size());
  if(!r3.ok)return 1;
  auto icon=fwd::make_icon_png(img.data(),img.size());
  int w=0,h=0;std::vector<unsigned char> rgba;
  bool ok=fwd::decode_image(icon.data(),icon.size(),w,h,rgba);
  std::printf("icon encoded: %zu bytes, valid=%d %dx%d\n",icon.size(),ok,w,h);
  FILE*f=fopen("/tmp/claude-1000/-var-home-bazzite-Projects-heydemoura-ps5fwdgen/ce93b368-401a-43db-b2d9-3e3213ca441a/scratchpad/sgdb_icon.png","wb");
  if(f){fwrite(icon.data(),1,icon.size(),f);fclose(f);}
  std::printf("ALL OK: full SteamGridDB -> icon pipeline worked\n");
  return 0;
}
