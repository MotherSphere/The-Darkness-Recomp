#include "renderer/d3d11/texture_budget_policy.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace DarkRecomp::TextureBudgetPolicy;
static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main() {
    try {
        const auto hardware=hardwareCap(16ull*1024*MiB);
        require(hardware==maximum,"Dedicated cap must preserve the existing2GiB maximum");
        require(hardwareCap(0)==defaultHardware,"Unknown dedicated memory must preserve512MiB fallback");
        require(hardwareCap(256ull*MiB)==minimum,"Small dedicated memory must preserve256MiB floor");
        require(hardwareCap(4ull*1024*MiB)==1024*MiB,"Dedicated memory quarter differs below ceiling");
        require(hardwareCap(std::numeric_limits<uint64_t>::max())==maximum,"Dedicated cap overflow");
        const auto startup=initialBudget(hardware,true,512ull*MiB);
        require(startup==minimum,"Low startup memory must select256MiB");
        require(refreshBudget(startup,hardware,true,10ull*1024*MiB)==maximum,"Recovered host memory must release startup cap");
        require(initialBudget(hardware,false,0)==hardware,"Failed initial sample must preserve hardware-only fallback");
        require(refreshBudget(startup,hardware,false,10ull*1024*MiB)==startup,"Failed refresh must not authorize growth");
        require(refreshBudget(startup,hardware,true,512ull*MiB)==startup,"Continued pressure must not grow cache");
        require(refreshBudget(startup,hardware,true,4ull*(minimum+32*MiB))==startup,"Small growth must wait for headroom");
        require(refreshBudget(startup,hardware,true,4ull*(minimum+64*MiB))==minimum+64*MiB,"Absolute growth boundary differs");
        require(refreshBudget(1024*MiB,hardware,true,4ull*(1024*MiB+64*MiB))==1024*MiB,"Large cache must require proportional headroom");
        require(refreshBudget(1024*MiB,hardware,true,4ull*(1024*MiB+128*MiB))==1152*MiB,"Proportional growth boundary differs");
        require(refreshBudget(maximum,hardware,true,4ull*1024*MiB)==1024*MiB,"New pressure must shrink at next sample");
        require(refreshBudget(1024*MiB,hardwareCap(2ull*1024*MiB),true,16ull*1024*MiB)==512*MiB,"Hardware cap must win over host headroom");
        require(refreshBudget(maximum,hardware,true,0)==minimum,"Measured zero memory must retain existing floor");
        require(hostCap(std::numeric_limits<uint64_t>::max())==maximum,"Host cap overflow");
        for(const auto available:{0ull,512ull*MiB,1024ull*MiB,4ull*1024*MiB,8ull*1024*MiB,32ull*1024*MiB})
            for(const auto current:{minimum,size_t(512*MiB),size_t(1024*MiB),maximum}) {
                const auto chosen=refreshBudget(current,hardware,true,available);
                require(chosen>=minimum && chosen<=hardware && chosen<=hostCap(available),"Refresh selected outside existing bounds");
            }
        uint64_t deadline=6000;
        require(!sampleDue(5999,deadline) && deadline==6000,"Early logical frame must not consume memory-sample deadline");
        require(sampleDue(6000,deadline) && deadline==11000,"Exact5s sample boundary differs");
        require(!sampleDue(6000,deadline) && !sampleDue(10999,deadline),"Repeated frame/part must not trigger another sample");
        require(sampleDue(11000,deadline) && deadline==16000,"Next5s sample boundary differs");
        require(sampleDue(25000,deadline) && deadline==30000,"Delayed frame must sample once without catch-up loop");
        const auto recovered=refreshBudget(startup,hardware,true,10ull*1024*MiB);
        require(refreshBudget(recovered,hardware,true,10ull*1024*MiB)==recovered &&
                refreshBudget(recovered,hardware,false,0)==recovered,"Unchanged/failed sample must not create a capacity change");
        std::puts("TextureBudgetPolicy passed: existing caps, low-start recovery, pressure, hysteresis, failed queries,5s scheduling and unchanged selection.");
        return 0;
    }catch(const std::exception& error) {std::fprintf(stderr,"TextureBudgetPolicy failed: %s\n",error.what());return 1;}
}
