// SPDX-License-Identifier: Apache-2.0
// The browser, preview, export and mount share Clip's codec dispatch.
#include "Develop.h"
#include "ClipSummary.h"
#include "Crc32c.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
namespace fs=std::filesystem;
static std::vector<uint8_t> read(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void put(std::vector<uint8_t>& p,uint64_t v,int n){for(int i=0;i<n;++i)p.push_back(uint8_t(v>>(8*i)));}
static void chunk(std::vector<uint8_t>& file,const char* tag,const std::vector<uint8_t>& p){file.insert(file.end(),tag,tag+4);put(file,0,4);put(file,p.size(),8);file.insert(file.end(),p.begin(),p.end());put(file,craw::detail::crc32c(p.data(),p.size()),4);}
int main(){
    const fs::path vectors=CRAW_VECTORS_DIR,path=fs::temp_directory_path()/"craw_tui_compact_test.craw";
    try {
        std::vector<uint8_t> file{'C','R','A','W',0,0,2,0,0,0,0,0,0,0,0,0};
        const std::string json=R"({"sensor":{"width":128,"height":96,"bit_depth":10,"cfa":"GBRG","white_level":1023,"black_level":[64,64,64,64]}})";
        chunk(file,"META",{json.begin(),json.end()});
        std::vector<uint8_t> fram;put(fram,0,8);put(fram,0,8);put(fram,2,4);fram.insert(fram.end(),{'{','}'});
        const auto payload=read(vectors/"compact_scene10.payload");fram.insert(fram.end(),payload.begin(),payload.end());chunk(file,"FRAM",fram);
        {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(file.data()),file.size());}
        craw::tools::Clip clip(path.string());const auto raw=clip.frameRaw(0);const auto expected=read(vectors/"compact_scene10.raw");
        if(raw.pixels.size()*2!=expected.size())throw std::runtime_error("geometry mismatch");
        for(size_t i=0;i<raw.pixels.size();++i)if(raw.pixels[i]!=(expected[2*i]|expected[2*i+1]<<8))throw std::runtime_error("RAW mismatch");
        const auto summary=craw::tui::ClipSummary::load(path.string());if(!summary.error.empty()||summary.frames!=1)throw std::runtime_error("browser failed");
        std::vector<uint16_t> rgb;uint32_t w,h;craw::tui::DevelopSettings settings;
        craw::tui::develop(raw,clip.info(),settings,rgb,w,h,1);
        if(w!=128||h!=96||rgb.size()!=128*96*3)throw std::runtime_error("preview failed");
        // Same DNG generation path used by craw_mount and craw2dng.
        if(clip.frameDng(0,4096,1).size()<=4096)throw std::runtime_error("DNG export failed");
        fs::remove(path);return 0;
    }catch(const std::exception& e){fs::remove(path);std::fprintf(stderr,"%s\n",e.what());return 1;}
}
