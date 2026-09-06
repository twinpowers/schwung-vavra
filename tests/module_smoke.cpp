// Real API integration test. Run on Move with a module directory and optional clock/voices.
#include "dsp/plugin_api.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

using Clock = std::chrono::steady_clock;
static double seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now()-start).count();
}
static int underrunsOf(plugin_api_v2_t* api, void* inst) {
    char value[512]{};
    if(api->get_param(inst,"diagnostics",value,sizeof(value))<1) return -1;
    const char* field=strstr(value,"underruns=");
    return field ? atoi(field+10) : -1;
}
int main(int argc, char** argv) {
    if(argc < 2) return 2;
#ifdef __linux__
    if(geteuid()==0) {
        cpu_set_t cpu; CPU_ZERO(&cpu); CPU_SET(3,&cpu);
        sched_setaffinity(0,sizeof(cpu),&cpu);
        sched_param priority{}; priority.sched_priority=70;
        if(sched_setscheduler(0,SCHED_FIFO,&priority)) { perror("harness priority"); return 1; }
        printf("harness=core3/FIFO70\n");
    } else {
        // Unprivileged, this harness is SCHED_OTHER on the same cores as the
        // emulator's FIFO 20 threads: it gets descheduled, then bursts its
        // catch-up render_block calls and drains the queue itself. Every
        // underrun it then reports is its own. Say so on every line of output
        // that could be pasted into a result.
        printf("harness=UNPRIVILEGED underruns_measure_this_harness_not_the_module\n");
        fprintf(stderr,"WARNING: not root -- underrun counts are meaningless. Re-run as root.\n");
    }
#endif
    char path[1024];
    snprintf(path,sizeof(path),"%s/dsp.so",argv[1]);
    void* lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!lib) { fprintf(stderr,"FAIL: %s\n",dlerror()); return 1; }
    auto init=reinterpret_cast<plugin_api_v2_t* (*)(const host_api_v1_t*)>(dlsym(lib,"move_plugin_init_v2"));
    if(!init) return 1;
    host_api_v1_t host{}; host.api_version=1; host.sample_rate=44100; host.frames_per_block=128;
    auto* api=init(&host);
    auto start=Clock::now();
    void* inst=api->create_instance(argv[1],nullptr);
    double create=seconds(start);
    printf("create_seconds=%.6f\n",create); fflush(stdout);
    if(!inst || create>0.1) { fprintf(stderr,"FAIL: create_instance returned %p in %.6fs\n",inst,create); return 1; }
    if(argc>2 && !strcmp(argv[2],"cancel")) {
        api->destroy_instance(inst); dlclose(lib);
        printf("cancel_seconds=%.6f\n",seconds(start));
        return seconds(start)<0.5 ? 0 : 1;
    }
    char contract[2048]{};
    if(api->get_param(inst,"is_loading",contract,sizeof(contract))<1 || strcmp(contract,"1")) {
        fprintf(stderr,"FAIL: host is_loading contract\n"); api->destroy_instance(inst); dlclose(lib); return 1;
    }
    if(api->get_param(inst,"chain_params",contract,sizeof(contract))<1 || !strstr(contract,"dsp_clock")) {
        fprintf(stderr,"FAIL: host chain_params contract\n"); api->destroy_instance(inst); dlclose(lib); return 1;
    }
    if(argc>2) api->set_param(inst,"dsp_clock",argv[2]);
    if(argc>4) api->set_param(inst,"buffer_ms",argv[4]);
    char value[512]{};
    int16_t audio[256];
    bool ready=false;
    while(seconds(start)<120) {
        api->render_block(inst,audio,128);
        if(api->get_error(inst,value,sizeof(value))>0) {
            fprintf(stderr,"FAIL: %s\n",value);
            api->destroy_instance(inst); dlclose(lib);
            return argc>2 && !strcmp(argv[2],"missing-rom") ? 0 : 1;
        }
        api->get_param(inst,"loading",value,sizeof(value));
        if(!strcmp(value,"0")) { ready=true; break; }
        for(auto sample:audio) if(sample) {
            fprintf(stderr,"FAIL: nonzero sample while loading=%s\n",value);
            api->destroy_instance(inst); dlclose(lib); return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    printf("boot_seconds=%.3f ready=%d\n",seconds(start),ready); fflush(stdout);
    if(!ready) { api->destroy_instance(inst); dlclose(lib); return 1; }
    // Let firmware initialization and the initial audio queue settle before notes.
    // Underruns are localized to the block they appear in: a stall during this
    // silent window is inaudible, one during the note loop is a dropout.
    int seen=underrunsOf(api,inst);
    printf("underruns_at_ready=%d\n",seen); fflush(stdout);
    for(int i=0;i<345;++i) {
        api->render_block(inst,audio,128);
        int now=underrunsOf(api,inst);
        if(now!=seen) { printf("settle_block=%d underruns=%d t=%.3f\n",i,now,seconds(start)); fflush(stdout); seen=now; }
        std::this_thread::sleep_for(std::chrono::microseconds(2902));
    }
    const int underrunsBeforeNotes=seen;
    printf("notes_at_block=0 t=%.3f\n",seconds(start)); fflush(stdout);
    const int notes[]={60,64,67,72,76,79,84,88};
    const int voices=argc>3 ? std::clamp(atoi(argv[3]),1,8) : 4;
    for(int i=0;i<voices;++i) { uint8_t msg[]={0x90,(uint8_t)notes[i],100}; api->on_midi(inst,msg,3,0); }
    double sum=0,maxRender=0; int peak=0; long count=0;
    auto next=Clock::now();
    for(int block=0;block<3446;++block) {
        if(block>0 && block%345==0) {
            api->get_param(inst,"diagnostics",value,sizeof(value));
            printf("second=%d %s\n",block/345,value); fflush(stdout);
        }
        if(block>0 && block%345==0) {
            for(int i=0;i<voices;++i) { uint8_t msg[]={0x80,(uint8_t)notes[i],0}; api->on_midi(inst,msg,3,0); }
        }
        if(block>0 && block%345==10) {
            for(int i=0;i<voices;++i) { uint8_t msg[]={0x90,(uint8_t)notes[i],100}; api->on_midi(inst,msg,3,0); }
        }
        auto render=Clock::now(); api->render_block(inst,audio,128);
        maxRender=std::max(maxRender,seconds(render));
        int now=underrunsOf(api,inst);
        if(now!=seen) { printf("play_block=%d underruns=%d t=%.3f\n",block,now,seconds(start)); fflush(stdout); seen=now; }
        for(int16_t sample:audio) { peak=std::max(peak,abs((int)sample)); sum+=double(sample)*sample; ++count; }
        next+=std::chrono::nanoseconds(2902494);
        std::this_thread::sleep_until(next);
    }
    for(int i=0;i<voices;++i) { uint8_t msg[]={0x80,(uint8_t)notes[i],0}; api->on_midi(inst,msg,3,0); }
    api->get_param(inst,"diagnostics",value,sizeof(value));
    const char* underrunField=strstr(value,"underruns=");
    const int underruns=underrunField ? atoi(underrunField+10) : -1;
    const int playUnderruns=underruns-underrunsBeforeNotes;
    printf("voices=%d peak=%d rms=%.2f max_render_us=%.1f play_underruns=%d %s\n",
           voices,peak,sqrt(sum/count),maxRender*1e6,playUnderruns,value);
    if(api->get_param(inst,"stalls",value,sizeof(value))>0) printf("%s\n",value);
    start=Clock::now(); api->destroy_instance(inst);
    printf("destroy_seconds=%.6f\n",seconds(start));
    dlclose(lib);
    // Only underruns inside the note window are audible; a stall in the silent
    // settle window costs nothing and is reported rather than failed on.
    return peak>100 && sqrt(sum/count)>10 && maxRender<0.01 && playUnderruns==0 ? 0 : 1;
}
