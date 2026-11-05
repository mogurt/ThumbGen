#pragma once
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
namespace process_diagnostic {
struct Event {const char* op;long long ns;pid_t pid;int result,error;char text[512];};
inline Event events[2048]{};
inline unsigned used=0;
inline auto epoch=std::chrono::steady_clock::now();
inline Event& record(const char* op,pid_t pid,int result,int error) {
    auto& e=events[used<2048?used++:2047];e.op=op;e.pid=pid;e.result=result;e.error=error;
    e.ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-epoch).count();return e;
}
inline void observation(pid_t pid,const std::string& text) {
    auto& e=record("observe",pid,0,0);auto size=std::min(text.size(),sizeof(e.text)-1);
    std::memcpy(e.text,text.data(),size);e.text[size]=0;
}
inline int traced_waitid(idtype_t type,id_t pid,siginfo_t* info,int flags) {
    int r=::waitid(type,pid,info,flags),error=errno;
    auto& e=record("waitid",pid,r,r<0?error:0);
    std::snprintf(e.text,sizeof(e.text),"flags=%d si_pid=%d si_code=%d si_status=%d",flags,int(info->si_pid),info->si_code,info->si_status);
    errno=error;return r;
}
inline int traced_kill(pid_t pid,int signal) {
    record("kill-before",pid,signal,0);int r=::kill(pid,signal),error=errno;
    record("kill-after",pid,r,r<0?error:0);errno=error;return r;
}
inline pid_t traced_waitpid(pid_t pid,int* status,int flags) {
    pid_t r=::waitpid(pid,status,flags);int error=errno;
    auto& e=record("waitpid",pid,r,r<0?error:0);
    std::snprintf(e.text,sizeof(e.text),"flags=%d wait_status=%d",flags,*status);
    errno=error;return r;
}
inline void dump() {
    std::cerr<<"OWNER "<<getpid()<<'\n';
    std::ifstream owner("/proc/self/stat");std::string owner_stat;std::getline(owner,owner_stat);
    std::cerr<<"OWNER_STAT "<<owner_stat<<'\n';
    for(unsigned i=0;i<used;++i){auto& e=events[i];std::cerr<<"TRACE "<<e.ns<<' '<<e.op<<" pid="<<e.pid<<" result="<<e.result<<" errno="<<e.error<<" stat="<<e.text<<'\n';}
    std::ifstream file("/proc/self/status");std::string line;
    while(std::getline(file,line))if(line.rfind("Sig",0)==0 || line.rfind("NSpid",0)==0)std::cerr<<"STATUS "<<line<<'\n';
}
}
