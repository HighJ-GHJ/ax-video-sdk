// 文件说明：用测试专用TCP对端验证真实Demux→RtspClient断开、收尾、重开，不模拟生产重连。
#include "pipeline/ax_demuxer.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::to_string(__LINE__)+": " #x); } while (0)
using Bytes = std::vector<uint8_t>;
using namespace std::chrono_literals;
namespace {
// 测试替身：同一监听地址接收多个会话；协议事件推进，不依赖send/recv边界一致。
class Peer {
public:
    explicit Peer(bool fail_first=false) : fail_first_(fail_first) {
        listener_=socket(AF_INET,SOCK_STREAM,0); CHECK(listener_>=0);
        sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        CHECK(bind(listener_,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);
        CHECK(listen(listener_,8)==0); socklen_t len=sizeof(addr);
        CHECK(getsockname(listener_,reinterpret_cast<sockaddr*>(&addr),&len)==0);
        port_=ntohs(addr.sin_port);
        worker_=std::thread([this]{ Run(); });
    }
    ~Peer() { stop_=true; Disconnect(); if(worker_.joinable()) worker_.join(); ::close(listener_); }
    std::string Url() const { return "rtsp://127.0.0.1:"+std::to_string(port_)+"/live"; }
    // 仅关闭当前连接，监听仍可服务下一次会话。
    void Disconnect() { std::lock_guard<std::mutex> lock(mutex_); if(fd_>=0) shutdown(fd_,SHUT_RDWR); }
    unsigned Plays() { std::lock_guard<std::mutex> lock(mutex_); return plays_; }
    void CheckError() { std::lock_guard<std::mutex> lock(mutex_); if(error_) std::rethrow_exception(error_); }
private:
    int listener_=-1,fd_=-1; unsigned port_=0,plays_=0,connections_=0;
    bool fail_first_; std::atomic<bool> stop_{false}; std::mutex mutex_;
    std::thread worker_; std::exception_ptr error_;
    // 单线程发送，完整推进短写；测试退出造成的断开不是服务器脚本失败。
    static bool Send(int fd,const Bytes& data) {
        size_t pos=0; while(pos<data.size()) { auto n=send(fd,data.data()+pos,data.size()-pos,0); if(n<=0)return false; pos+=size_t(n); } return true;
    }
    // 参数集取自已有sample.h264；IDR标记只用于压缩包身份断言，不作图像解码验收。
    static Bytes Frame(unsigned session,unsigned sequence) {
        const Bytes sps={0x67,0x42,0xc0,0x0b,0x8c,0x8d,0x41,0x42,0x2f,0x2e,2,3,0xc2,0x21,0x1a,0x80};
        const Bytes pps={0x68,0xce,0x3c,0x80};
        Bytes payload={0x78};
        for(const auto& nal: {sps,pps,Bytes{0x65,0x88,0x84,uint8_t(session)}}) {
            payload.push_back(0); payload.push_back(uint8_t(nal.size())); payload.insert(payload.end(),nal.begin(),nal.end());
        }
        const unsigned ts=9000+sequence*3600, size=12+payload.size();
        Bytes wire={'$',0,uint8_t(size>>8),uint8_t(size),0x80,0xe0,0,uint8_t(sequence),
            uint8_t(ts>>24),uint8_t(ts>>16),uint8_t(ts>>8),uint8_t(ts),0,0,0,1};
        wire.insert(wire.end(),payload.begin(),payload.end()); return wire;
    }
    // 接收器只解析测试所需请求，保留body和超读尾部，缓存有界。
    void Serve(int fd,unsigned session) {
        std::string pending;
        while(!stop_) {
            auto end=pending.find("\r\n\r\n");
            size_t length=0;
            if(end!=std::string::npos) { auto p=pending.substr(0,end).find("Content-Length:"); if(p!=std::string::npos)length=std::stoul(pending.substr(p+15)); }
            if(end==std::string::npos || pending.size()<end+4+length) {
                char buf[4096]; auto n=recv(fd,buf,sizeof(buf),0); if(n<=0)return;
                pending.append(buf,size_t(n)); CHECK(pending.size()<=65536); continue;
            }
            auto request=pending.substr(0,end+4+length); pending.erase(0,end+4+length);
            auto method=request.substr(0,request.find(' ')); auto cseq=request.find("CSeq:"); CHECK(cseq!=std::string::npos);
            auto seq=std::stoul(request.substr(cseq+5)); std::string body,extra;
            if(method=="DESCRIBE") {
                body="v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Test\r\nt=0 0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=framesize:96 320-240\r\na=control:stream\r\n";
                extra="Content-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size())+"\r\n";
            } else if(method=="SETUP") extra="Session: test\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
            std::string status=fail_first_ && session==1 && method=="DESCRIBE" ? "404 Not Found" : "200 OK";
            auto reply="RTSP/1.0 "+status+"\r\nCSeq: "+std::to_string(seq)+"\r\n"+extra+"\r\n"+body;
            if(!Send(fd,Bytes(reply.begin(),reply.end())))return;
            if(method=="PLAY") {
                { std::lock_guard<std::mutex> lock(mutex_); ++plays_; }
                // 首包供真实Prime读取参数集，第二包供ReadPacket校验新会话身份。
                if(!Send(fd,Frame(session,1)) || !Send(fd,Frame(session,2)))return;
            }
        }
    }
    void Run() {
        try {
            while(!stop_) {
                pollfd wait{listener_,POLLIN,0}; if(poll(&wait,1,50)<=0)continue;
                int fd=accept(listener_,nullptr,nullptr); if(fd<0)continue;
                { std::lock_guard<std::mutex> lock(mutex_); fd_=fd; }
                Serve(fd,++connections_);
                { std::lock_guard<std::mutex> lock(mutex_); fd_=-1; ::close(fd); }
            }
        } catch(...) { std::lock_guard<std::mutex> lock(mutex_); error_=std::current_exception(); if(fd_>=0){::close(fd_);fd_=-1;} }
    }
};
// 有界等待真实生产ReadPacket；失败也先Interrupt，使future析构不无限等待。
void ReadIdentity(axvsdk::pipeline::Demuxer& demux,unsigned session) {
    axvsdk::codec::EncodedPacket packet;
    auto result=std::async(std::launch::async,[&]{return demux.ReadPacket(&packet);});
    bool ready=result.wait_for(4s)==std::future_status::ready;
    if(!ready)demux.Interrupt();
    bool ok=result.get(); CHECK(ready && ok);
    Bytes marker={0x65,0x88,0x84,uint8_t(session)};
    CHECK(std::search(packet.data.begin(),packet.data.end(),marker.begin(),marker.end())!=packet.data.end());
}
}
// 通过公开Demux执行回归，检查在Release下不被优化掉。
int main(int argc,char** argv) {
    signal(SIGPIPE,SIG_IGN);
    try {
        CHECK(argc>=2); std::string mode=argv[1]; Peer peer(mode=="startup_failure");
        auto demux=axvsdk::pipeline::CreateDemuxer(); axvsdk::pipeline::DemuxerConfig config; config.uri=peer.Url();
        if(mode=="startup_failure")CHECK(!demux->Open(config));
        CHECK(demux->Open(config)); ReadIdentity(*demux,mode=="startup_failure"?2:1);
        if(mode=="reconnect") {
            for(unsigned next=2;next<=4;++next) { peer.Disconnect(); ReadIdentity(*demux,next); CHECK(peer.Plays()==next); }
        } else if(mode=="interrupt") {
            peer.Disconnect(); demux->Interrupt(); axvsdk::codec::EncodedPacket packet;
            CHECK(!demux->ReadPacket(&packet));
        } else if(mode=="mp4") {
            CHECK(argc==3); config.uri=argv[2]; config.realtime_playback=false;
            CHECK(demux->Open(config)); axvsdk::codec::EncodedPacket first,again;
            CHECK(demux->ReadPacket(&first)); CHECK(demux->Reset()); CHECK(demux->ReadPacket(&again)); CHECK(first.data==again.data && first.pts==again.pts);
        }
        demux->Close(); demux->Close();
        if(mode!="control") { config.uri=peer.Url(); CHECK(demux->Open(config)); demux->Close(); }
        peer.CheckError(); std::cout<<"PASS "<<mode<<'\n'; return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
