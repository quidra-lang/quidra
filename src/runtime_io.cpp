#include "runtime_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace {
bool valid_text(const std::string& value) {
    return quidra_runtime_text_valid_bytes(
        value.data(), static_cast<unsigned long long>(value.size()));
}
char* copy_text(const std::string& value) {
    return quidra_runtime_copy_text_bytes(
        value.data(), static_cast<unsigned long long>(value.size()));
}
}

extern "C" char* quidra_file_read_raw(const char* path) {
    if (!path) return nullptr;
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) return nullptr;
    if (!valid_text(data)) return nullptr;
    return copy_text(data);
}

extern "C" void* quidra_file_read_bin_raw(const char* path) {
    if (!path) return nullptr;
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    std::vector<unsigned char> data(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) return nullptr;
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<long long>::max() / 8) ||
        data.size() > std::numeric_limits<std::size_t>::max() - 8) return nullptr;
    auto* result=static_cast<unsigned char*>(
        quidra_managed_alloc(static_cast<unsigned long long>(8+data.size())));
    const auto bit_count=static_cast<long long>(data.size() * 8);
    std::memcpy(result,&bit_count,sizeof(bit_count));
    if(!data.empty()) std::memcpy(result+8,data.data(),data.size());
    return result;
}

extern "C" bool quidra_file_write_raw(const char* path,const char* text) {
    if(!path||!text) return false;
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out) return false;
    out.write(text,static_cast<std::streamsize>(std::strlen(text)));
    out.close();
    return static_cast<bool>(out);
}

extern "C" bool quidra_file_write_bin_raw(const char* path,const void* bin_raw) {
    if(!path||!bin_raw) return false;
    long long bit_count=0;
    std::memcpy(&bit_count,bin_raw,sizeof(bit_count));
    if(bit_count<0 || bit_count%8!=0) return false;
    const auto byte_count=static_cast<unsigned long long>(bit_count/8);
    const auto size=static_cast<std::size_t>(byte_count);
    if(static_cast<unsigned long long>(size)!=byte_count ||
       size>static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) return false;
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out) return false;
    if(size) out.write(static_cast<const char*>(bin_raw)+8,static_cast<std::streamsize>(size));
    out.close();
    return static_cast<bool>(out);
}

extern "C" int quidra_file_exists_raw(const char* path) {
    if(!path) return -1;
    std::error_code error;
    const bool result=std::filesystem::exists(path,error);
    return error?-1:(result?1:0);
}
extern "C" int quidra_file_is_directory_raw(const char* path) {
    if(!path) return -1;
    std::error_code error;
    const bool exists=std::filesystem::exists(path,error);
    if(error) return -1;
    if(!exists) return 0;
    const bool result=std::filesystem::is_directory(path,error);
    return error?-1:(result?1:0);
}
extern "C" bool quidra_file_remove_raw(const char* path) {
    if(!path) return false;
    std::error_code error;
    return std::filesystem::remove(path,error)&&!error;
}
extern "C" bool quidra_file_copy_raw(const char* source,const char* destination) {
    if(!source||!destination) return false;
    std::error_code error;
    return std::filesystem::copy_file(
        source,destination,std::filesystem::copy_options::overwrite_existing,error)&&!error;
}
extern "C" bool quidra_file_move_raw(const char* source,const char* destination) {
    if(!source||!destination) return false;
    std::error_code error;
    std::filesystem::rename(source,destination,error);
    return !error;
}
extern "C" bool quidra_file_mkdir_raw(const char* path) {
    if(!path) return false;
    std::error_code error;
    return std::filesystem::create_directory(path,error)&&!error;
}

extern "C" void* quidra_file_list_raw(const char* path,bool recursive) {
    if(!path) return nullptr;
    std::error_code error;
    std::vector<std::string> entries;
    auto append=[&](const std::filesystem::path& entry){
        try{
            const auto encoded=entry.u8string();
            std::string text(reinterpret_cast<const char*>(encoded.data()),encoded.size());
            if(!valid_text(text)) return false;
            entries.push_back(std::move(text));
            return true;
        }catch(const std::system_error&){return false;}
    };
    if(recursive){
        std::filesystem::recursive_directory_iterator current(path,error);
        if(error) return nullptr;
        const std::filesystem::recursive_directory_iterator end;
        while(current!=end){
            if(!append(current->path())) return nullptr;
            current.increment(error);
            if(error) return nullptr;
        }
    }else{
        std::filesystem::directory_iterator current(path,error);
        if(error) return nullptr;
        const std::filesystem::directory_iterator end;
        while(current!=end){
            if(!append(current->path())) return nullptr;
            current.increment(error);
            if(error) return nullptr;
        }
    }
    std::sort(entries.begin(),entries.end());
    if(entries.size()>(std::numeric_limits<std::size_t>::max()-8)/sizeof(char*)) return nullptr;
    const auto bytes=8+entries.size()*sizeof(char*);
    auto* result=static_cast<unsigned char*>(
        quidra_managed_alloc(static_cast<unsigned long long>(bytes)));
    const auto count=static_cast<long long>(entries.size());
    std::memcpy(result,&count,sizeof(count));
    for(std::size_t i=0;i<entries.size();++i){
        auto* item=copy_text(entries[i]);
        std::memcpy(result+8+i*sizeof(char*),&item,sizeof(item));
    }
    return result;
}

extern "C" char* quidra_environment_get(const char* name) {
    if(!name) return nullptr;
#ifdef _WIN32
    char* value=nullptr;
    std::size_t size=0;
    if(_dupenv_s(&value,&size,name)!=0||!value) return nullptr;
    const std::string text(value,size?size-1:0);
    std::free(value);
#else
    const char* value=std::getenv(name);
    if(!value) return nullptr;
    const std::string text(value);
#endif
    if(!valid_text(text)) {
        quidra_runtime_text_error(
            "environment value must be valid UTF-8 text without NUL");
    }
    return copy_text(text);
}

extern "C" bool quidra_environment_has(const char* name) {
    if(!name) return false;
#ifdef _WIN32
    char* value=nullptr;
    std::size_t size=0;
    const auto error=_dupenv_s(&value,&size,name);
    const bool present=error==0&&value!=nullptr;
    std::free(value);
    return present;
#else
    return std::getenv(name)!=nullptr;
#endif
}
