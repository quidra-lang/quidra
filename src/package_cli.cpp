#include "package_cli.hpp"

#include "quidra/compiler.hpp"
#include "quidra/import_path.hpp"
#include "quidra/frontend.hpp"
#include "quidra/package_lock.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace quidra::cli {
namespace {

bool valid_package_name(std::string_view name) {
    return is_importable_package_name(name);
}

fs::path package_root() {
#ifdef _WIN32
    const auto home=import_environment_value("USERPROFILE");
#else
    const auto home=import_environment_value("HOME");
#endif
    if(!home||home->empty())
        throw std::runtime_error("cannot determine user home for the Quidra package store");
    return fs::path(*home)/".quidra"/"packages";
}

void require_package_source(const fs::path& source) {
    std::error_code error;
    if(!fs::is_directory(source,error)||error)
        throw std::runtime_error("package source must be a directory: "+source.string());
    const auto main=source/"main.qui";
    if(!fs::is_regular_file(main,error)||error)
        throw std::runtime_error("package source must contain main.qui");
}

void copy_package_tree(const fs::path& source,const fs::path& destination) {
    fs::create_directories(destination);
    for(fs::recursive_directory_iterator iterator(source),end;iterator!=end;++iterator) {
        const auto status=iterator->symlink_status();
        if(fs::is_symlink(status))
            throw std::runtime_error("package install rejects symbolic links: "+iterator->path().string());
        const auto relative=fs::relative(iterator->path(),source);
        const auto target=destination/relative;
        if(fs::is_directory(status)) {
            fs::create_directories(target);
        } else if(fs::is_regular_file(status)) {
            fs::create_directories(target.parent_path());
            fs::copy_file(iterator->path(),target,fs::copy_options::overwrite_existing);
        } else {
            throw std::runtime_error("package contains unsupported filesystem entry: "+iterator->path().string());
        }
    }
}

void install_package(const fs::path& source,std::string name,bool force) {
    if(!valid_package_name(name))
        throw std::runtime_error(
            "package name must be an importable Quidra identifier and must not be a standard namespace");
    const auto absolute=fs::absolute(source).lexically_normal();
    require_package_source(absolute);

    // Validate against the ordinary frontend before touching the package store.
    (void)check_file(absolute/"main.qui",{},absolute);

    const auto root=package_root();
    fs::create_directories(root);
    const auto target=root/name;
    std::error_code error;
    if(fs::exists(target,error)&&!error&&!force)
        throw std::runtime_error("package is already installed; pass --force to replace it");

    std::random_device random;
    const auto suffix=std::to_string(random())+"-"+std::to_string(random());
    const auto temporary=root/("."+name+".install-"+suffix);
    const auto backup=root/("."+name+".backup-"+suffix);
    try {
        copy_package_tree(absolute,temporary);
        if(!fs::is_regular_file(temporary/"main.qui"))
            throw std::runtime_error("copied package lost main.qui");

        const bool had_target=fs::exists(target);
        if(had_target) fs::rename(target,backup);
        try {
            fs::rename(temporary,target);
        } catch(...) {
            if(had_target&&fs::exists(backup)&&!fs::exists(target)) fs::rename(backup,target);
            throw;
        }
        if(had_target) fs::remove_all(backup);
    } catch(...) {
        std::error_code ignored;
        fs::remove_all(temporary,ignored);
        if(fs::exists(backup,ignored)&&!fs::exists(target,ignored)) {
            std::error_code restore_error;
            fs::rename(backup,target,restore_error);
        }
        throw;
    }
    std::cout<<"installed "<<name<<" -> "<<target.string()<<"\n";
}

void remove_package(std::string_view name) {
    if(!valid_package_name(name))
        throw std::runtime_error("invalid package name");
    const auto target=package_root()/std::string(name);
    std::error_code error;
    if(!fs::is_directory(target,error)||error)
        throw std::runtime_error("package is not installed: "+std::string(name));
    fs::remove_all(target,error);
    if(error) throw std::runtime_error("cannot remove package: "+error.message());
    std::cout<<"removed "<<name<<"\n";
}

void list_packages() {
    const auto root=package_root();
    std::error_code error;
    if(!fs::is_directory(root,error)||error) return;
    std::vector<std::string> names;
    for(const auto& entry:fs::directory_iterator(root)) {
        if(!entry.is_directory()) continue;
        const auto name=entry.path().filename().string();
        if(!name.empty()&&name.front()!='.'&&fs::is_regular_file(entry.path()/"main.qui"))
            names.push_back(name);
    }
    std::sort(names.begin(),names.end());
    for(const auto& name:names) std::cout<<name<<"\n";
}

std::string read_text_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

void write_lock_file(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + path.string());
    output << text;
    output.flush();
    if (!output) throw std::runtime_error("cannot finish writing " + path.string());
}

int lock_packages(const fs::path& source, bool check_only) {
    const auto absolute = fs::absolute(source).lexically_normal();
    std::error_code error;
    if (!fs::is_regular_file(absolute, error) || error || absolute.extension() != ".qui")
        throw std::runtime_error("package lock requires an existing .qui root source file");

    const auto cwd = fs::current_path();
    const auto packages = resolve_package_dependencies(absolute, cwd);
    const auto expected = package_lock_text(packages);
    const auto path = package_lock_path(cwd);

    if (check_only) {
        if (read_text_file(path) == expected) return 0;
        std::cerr << "quidra package: quidra.lock is missing or out of date\n";
        return 1;
    }

    write_lock_file(path, expected);
    std::cout << "locked " << packages.size() << " package(s) -> " << path.string() << "\n";
    return 0;
}

void usage() {
    std::cerr
        <<"usage:\n"
        <<"  quidra package install DIR [--name NAME] [--force]\n"
        <<"  quidra package lock FILE.qui [--check]\n"
        <<"  quidra package remove NAME\n"
        <<"  quidra package list\n"
        <<"  quidra package path\n";
}

} // namespace

int run_package_cli(int argc,char** argv) {
    try {
        if(argc<1) { usage(); return 2; }
        const std::string command=argv[0];
        if(command=="list") {
            if(argc!=1) throw std::runtime_error("package list takes no arguments");
            list_packages();
            return 0;
        }
        if(command=="path") {
            if(argc!=1) throw std::runtime_error("package path takes no arguments");
            std::cout<<package_root().string()<<"\n";
            return 0;
        }
        if(command=="remove") {
            if(argc!=2) throw std::runtime_error("package remove requires exactly one package name");
            remove_package(argv[1]);
            return 0;
        }
        if(command=="lock") {
            if(argc<2||argc>3) throw std::runtime_error("package lock expects FILE.qui and optional --check");
            bool check_only=false;
            if(argc==3) {
                if(std::string(argv[2])!="--check")
                    throw std::runtime_error("unknown package lock option: "+std::string(argv[2]));
                check_only=true;
            }
            return lock_packages(argv[1],check_only);
        }
        if(command=="install") {
            if(argc<2) throw std::runtime_error("package install requires a source directory");
            const fs::path source=argv[1];
            std::string name=source.filename().string();
            if(name.empty()) name=source.parent_path().filename().string();
            bool force=false;
            for(int index=2;index<argc;++index) {
                const std::string option=argv[index];
                if(option=="--force") force=true;
                else if(option=="--name"&&index+1<argc) name=argv[++index];
                else throw std::runtime_error("unknown package install option: "+option);
            }
            install_package(source,std::move(name),force);
            return 0;
        }
        throw std::runtime_error("unknown package command: "+command);
    } catch(const CompileErrors& errors) {
        for(const auto& diagnostic:errors.diagnostics())
            std::cerr<<"error["<<diagnostic.code<<"] "<<diagnostic.message<<"\n";
        return 1;
    } catch(const CompileError& error) {
        std::cerr<<"error["<<error.diagnostic().code<<"] "<<error.diagnostic().message<<"\n";
        return 1;
    } catch(const std::exception& error) {
        std::cerr<<"quidra package: "<<error.what()<<"\n";
        return 1;
    }
}

} // namespace quidra::cli
