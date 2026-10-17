#include "aec48.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
static std::vector<float> run(bool bypass, bool gate, float threshold) {
    Aec48* a = aec48_create(48000, 384);
    if (!a) return {};
    aec48_set_enabled(a, !bypass);
    aec48_set_talk_highpass(a, 0); // isolate the pre-existing talk-gate parity check from the new post-gate HPF
    aec48_set_talk_gate(a, gate, threshold);
    std::vector<float> y(384*350), m(384), r(384);
    for (int b=0;b<350;++b) {
        for (int i=0;i<384;++i) { int t=b*384+i; r[i]=.1f*std::sin(t*.17f); m[i]=.07f*std::sin(t*.13f); }
        if (aec48_process(a,m.data(),r.data(),y.data()+b*384,384)) return {};
    }
    aec48_destroy(a); return y;
}
int main() {
    Aec48* a=aec48_create(48000,384); if (!a) return 1;
    const bool talk=std::strstr(aec48_version(),"mask talk-")!=nullptr ||
                    std::strstr(aec48_version(),"mask sing-")!=nullptr; // dual 创建后为讲话档
    aec48_set_talk_highpass(a, 0);
    bool ok=aec48_set_talk_gate(nullptr,1,-2.75f)==-1;
    ok &= aec48_set_talk_gate(a,1,-2.75f)==(talk?0:-1);
    ok &= aec48_set_talk_gate(a,1,std::numeric_limits<float>::infinity())==-1;
    ok &= aec48_set_talk_gate(a,1,std::numeric_limits<float>::quiet_NaN())==-1;
    ok &= aec48_latency_samples(a)==1152;
    aec48_destroy(a);
    auto off=run(false,false,100), closed=run(false,true,100), opened=run(false,true,-100);
    auto pass=run(true,false,100), passGate=run(true,true,100);
    const auto equal=[](const auto& x,const auto& y) { return !x.empty()&&x.size()==y.size()&&!std::memcmp(x.data(),y.data(),x.size()*4); };
    ok &= equal(off,opened)&&equal(pass,passGate);
    if (talk) {
        double e=0, d=0;
        for (size_t i=1152;i<off.size();++i) { e+=double(off[i])*off[i]; double v=closed[i]-off[i]*.01f; d+=v*v; }
        ok &= d<1e-12*(e+1e-30);
    } else ok &= equal(off,closed);
    std::printf("%s talk=%d: API/NaN/Inf/1152 latency/gate off/threshold/bypass\n",ok?"PASS":"FAIL",talk);
    return ok?0:1;
}
