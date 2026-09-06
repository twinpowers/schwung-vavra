/* microQ for Schwung. GPL-3.0.
 * Osirus architecture: the emulator lives in a forked child; the host only
 * exchanges bounded messages and consumes a shared audio ring. */
#include "plugin_api.h"
#include "runtime.h"
#include "presets_os223.h"
#include "mqLib/device.h"
#include "mqLib/mqmiditypes.h"
#include "mqLib/rom.h"
#include "dsp56kBase/logging.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <fcntl.h>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdexcept>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
// Osirus steps its DSP 64 samples at a time and this started at 256, which is
// 5.8 ms of audio in ONE step: a step that hits a cold JIT path took 16-18 ms,
// so a single one of them drained the whole 17 ms queue no matter how deep the
// queue was made. The step size, not the queue depth, is the ceiling on a
// stall.
constexpr int Chunk=256, TargetFillDefault=768;
// The queue trades latency for tolerance of a slow step, and with 4 voices at
// 100% clock roughly one step in ten exceeds its own realtime budget -- so the
// depth is the module's headroom dial, not a constant.
constexpr int TargetFillMin=512, TargetFillMax=6144;
// Warmup, in samples so that a change to Chunk cannot silently shorten it:
// at least 2 s of chord, retriggered every second, until 2 s pass with no step
// slower than StallUs, then a 1 s tail. The limit is the backstop -- it bounds
// how long a device that never settles can hold up the host's load.
constexpr int MinWarmBlocks=88200/Chunk, QuietBlocks=88200/Chunk;
constexpr int RetriggerBlocks=44100/Chunk, WarmupTailBlocks=44100/Chunk;
constexpr int WarmupBlockLimit=(44100*20)/Chunk;
// A step this slow is the JIT compiling, not the emulator running: steady state
// is ~4.6 ms for a 256-sample step and the queue only holds 17 ms.
constexpr uint32_t StallUs=8000;
// Measured on Move. At 100% eight voices underran 137-159 times a run; at 75%
// single-timbral play is clean but MULTI mode breaks past four total voices
// (15, 227, 272 at 4, 6, 8); at 50% every cell measured zero, including eight
// voices across two parts.
//
// It is free, because it does not change the SOUND. Rendered offline at 50,
// 75 and 100 and compared, the differences sit INSIDE the emulator's own
// run-to-run variation: two renders at the SAME clock differ more (rel 0.280,
// corr 0.963) than 75 differs from 100 (0.096, 0.997). mqLib runs the 68k in
// m_ucThread and the DSP in a DSPThread, so their interleaving -- not the
// clock -- is what moves the output, and no A/B of this emulator can be
// bit-exact.
constexpr int DspClockDefault=50;
// OS 2.23 ships 3 banks of 100. Names live in docs/presets-os223.tsv, dumped
// by tools/dump_presets.cpp -- but the module reads the CURRENT name off the
// device's own display rather than from any table, so a different ROM cannot
// make it lie.
constexpr int BankCount=3, PresetsPerBank=100;
// The Multi holds 16 instruments at a fixed 22-byte stride (MultiParameter::
// Inst0..Inst15). Part n answers on MIDI channel n by default.
constexpr int MultiParts=16, MultiInstStride=22;
// Inst<n>MidiChannel encodes MIDI channel c (1-based) as c+1: measured, part 1
// carries 2 and answers on channel 1. 0 and 1 are the global/omni settings.
constexpr int PartChannelOffset=2;
// How long the queue runs deep after a preset change, to swallow its JIT.
constexpr int BoostSeconds=4;
static_assert(sizeof(vavra::g_presetNames)/sizeof(*vavra::g_presetNames)==BankCount*PresetsPerBank,
    "the generated name table must cover every Program Change slot");
using Clock=std::chrono::steady_clock;
struct Stereo { int16_t l=0,r=0; };
struct Midi { uint16_t size=0; uint8_t bytes[1024]{}; };
// A firmware parameter write, queued for the child to emit as sysex. Generic
// on purpose: every Multi and Global parameter is addressed the same way, so
// exposing another one later costs a key, not a mechanism.
struct ParamWrite { uint16_t index=0; uint8_t value=0; uint8_t multi=0; };
struct Shared {
    vavra::Ring<Stereo,8192> audio;
    vavra::Ring<Midi,128> midi;
    vavra::Ring<ParamWrite,128> writes;
    std::atomic<int> ready{0}, failed{0}, clock{DspClockDefault}, gain{70}, targetFill{TargetFillDefault};
    // Preset selection is ONE index 0..299 in Program Change order, not a
    // bank plus a number: two params for one choice is two things to keep in
    // step, and the name table is indexed this way anyway.
    std::atomic<int> preset{0}, presetPending{1};
    // Multi mode: 16 parts, each with its own sound, channel and volume. The
    // firmware boots in SINGLE mode on omni, where every channel plays the one
    // sound -- which is why a slot's forward channel appears to do nothing.
    std::atomic<int> multiMode{0}, part{1};
    std::atomic<int> partPreset[MultiParts]{}, partChannel[MultiParts]{}, partVolume[MultiParts]{};
    char lcdName[24]{};
    std::atomic<uint32_t> underruns{0}, midiDrops{0}, blocks{0}, bootMs{0}, cpuMs{0};
    std::atomic<uint32_t> warmupBlocks{0};
    // Producer-side truth: the longest single emulator step, and how
    // empty the queue ever got. An underrun is one of these two, and
    // the consumer cannot tell which.
    std::atomic<uint32_t> maxProcUs{0}, warmupPeak{0};
    // Cumulative, so a per-second delta separates "the emulator ran slow"
    // (procUsTotal approaching wall time) from "the producer was not
    // scheduled" (few slow blocks, yet the queue still emptied).
    std::atomic<uint32_t> slowBlocks{0}, procMsTotal{0}, procUsCarry{0};
    // The first stalls, with the block they landed on: "when" is what separates
    // a cold JIT path at the first note from steady contention.
    std::atomic<uint32_t> stallBlock[16]{}, stallUs[16]{}, stalls{0};
    std::atomic<uint32_t> warmStallBlock[16]{}, warmStallUs[16]{}, warmStalls{0};
    char error[256]{};
};
struct Instance {
    Shared* shm=nullptr;
    pthread_t worker{};
    std::atomic<bool> stop{false};
    std::atomic<int> workerError{0};
    char moduleDir[1024]{};
};
static uint32_t cpuMs() {
    rusage r{}; getrusage(RUSAGE_SELF,&r);
    return (r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000 + (r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1000;
}
static void setupWorker(const char* name) {
    sched_param sp{}; sched_setscheduler(0,SCHED_OTHER,&sp);
    cpu_set_t cpus; CPU_ZERO(&cpus);
    CPU_SET(0,&cpus); CPU_SET(1,&cpus); CPU_SET(2,&cpus);
    sched_setaffinity(0,sizeof(cpus),&cpus);
    pthread_setname_np(pthread_self(),name);
}
static synthLib::DeviceCreateParams loadRom(const char* moduleDir) {
    const std::string directory=std::string(moduleDir)+"/roms";
    DIR* dir=opendir(directory.c_str());
    if(!dir) throw std::runtime_error("No microQ ROM directory. Put OS 2.23 .bin or .mid in roms/.");
    std::vector<std::string> names;
    while(auto* entry=readdir(dir)) {
        const char* ext=strrchr(entry->d_name,'.');
        if(ext && (!strcasecmp(ext,".bin") || !strcasecmp(ext,".mid"))) names.emplace_back(entry->d_name);
    }
    closedir(dir);
    std::sort(names.begin(),names.end());
    for(const auto& name:names) {
        auto path=directory+"/"+name;
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file) continue;
        auto size=file.tellg();
        if(size<=0 || size>524288) continue;
        std::vector<uint8_t> data(static_cast<size_t>(size));
        file.seekg(0); file.read(reinterpret_cast<char*>(data.data()),data.size());
        if(!file) continue;
        if(!strcasecmp(strrchr(name.c_str(),'.'),".mid")) {
            mqLib::ROM rom(path);
            if(!rom.isValid()) continue;
            data=rom.getData();
        }
        if(!vavra::normalizeRom(data)) continue;
        synthLib::DeviceCreateParams params;
        params.hostSamplerate=params.preferredSamplerate=44100;
        params.romData=std::move(data); params.romName=path;
        return params;
    }
    throw std::runtime_error(names.empty()
        ? "No microQ ROM found: add OS 2.23 .bin or .mid to roms/."
        : "ROM files found but rejected: expected 512 KiB OS 2.23 (either byte order), or a valid OS .mid.");
}
static void childMain(Instance* inst) {
    // Firmware boot logs thousands of flash writes; retain errors through SHM.
    Logging::setLogFunc([](const std::string&) {});
    // mc68k has a separate unconditional console logger. This child owns its
    // descriptor table, so redirecting it cannot silence the host process.
    int quiet=open("/dev/null",O_WRONLY);
    if(quiet>=0) { dup2(quiet,STDOUT_FILENO); dup2(quiet,STDERR_FILENO); close(quiet); }
    auto* shm=inst->shm;
    auto start=Clock::now();
    try {
        auto params=loadRom(inst->moduleDir);
        // Set this before construction so the 68k and DSP workers inherit the
        // same requested priority. Raising only the audio reader leaves its
        // producers vulnerable to scheduling stalls.
        sched_param realtime{}; realtime.sched_priority=20;
        sched_setscheduler(0,SCHED_FIFO,&realtime);
        // Construction boots the 68k and DSP. It must never run on the host callback.
        mqLib::Device device(params);
        if(!device.isValid()) throw std::runtime_error("microQ device rejected the firmware.");
        device.setExtraLatencySamples(0);
        int applied=shm->clock.load(); device.setDspClockPercent(applied);
        float input[Chunk]{}, output[6][Chunk]{};
        synthLib::TAudioInputs inputs{}; inputs[0]=input; inputs[1]=input;
        synthLib::TAudioOutputs outputs{};
        for(int c=0;c<6;++c) outputs[c]=output[c];
        std::vector<synthLib::SMidiEvent> midiIn,midiOut;
        midiIn.reserve(128); midiOut.reserve(128);
        // A cold first note compiles new JIT paths, and one of those compiles
        // costs 80 ms against a queue that holds 17 ms. Warm them here, where
        // the host still sees "loading" and every sample is discarded.
        //
        // The length cannot be a constant. A fixed 512 blocks left a storm of
        // 8-16 ms stalls in the first two seconds of real playing, because what
        // has to be compiled is not "a note" but note-on, voice re-allocation
        // and note-off arriving as separate events over seconds. So the warmup
        // plays a retriggering chord and runs until the emulator has been QUIET
        // -- no step slower than StallUs -- for QuietBlocks. It ends when the
        // JIT stops compiling rather than when a counter runs out.
        const uint8_t warmNotes[]={60,64,67,72,76,79,84,88};
        auto warmNotesEvent=[&](uint8_t status,uint8_t velocity){
            for(auto note:warmNotes)
                midiIn.emplace_back(synthLib::MidiEventSource::Host,status,note,velocity);
        };
        int lastStall=0, warmBlock=0;
        for(;warmBlock<WarmupBlockLimit;++warmBlock) {
            midiIn.clear(); midiOut.clear();
            // One chord per RetriggerBlocks, released just before the next, so
            // that note-on, voice stealing and note-off all compile in here.
            const int phase=warmBlock%RetriggerBlocks;
            if(phase==0) warmNotesEvent(0x90,100);
            else if(phase==RetriggerBlocks-8) warmNotesEvent(0x80,0);
            const auto warmStart=Clock::now();
            static_cast<synthLib::Device&>(device).process(inputs,outputs,Chunk,midiIn,midiOut);
            const auto warmUs=static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-warmStart).count());
            if(warmUs>StallUs) {
                lastStall=warmBlock;
                const auto slot=shm->warmStalls.fetch_add(1);
                if(slot<16) { shm->warmStallBlock[slot]=warmBlock; shm->warmStallUs[slot]=warmUs; }
            }
            float peak=0;
            for(int c=0;c<2;++c) for(int i=0;i<Chunk;++i) peak=std::max(peak,std::fabs(output[c][i]));
            shm->warmupPeak=std::max(shm->warmupPeak.load(),static_cast<uint32_t>(peak*10000.0f));
            if(warmBlock>=MinWarmBlocks && warmBlock-lastStall>=QuietBlocks) break;
        }
        // Silence the chord and let the release and effect tails run out, so the
        // queue is filled with silence rather than with the warmup's own audio.
        for(int block=0;block<WarmupTailBlocks;++block) {
            midiIn.clear(); midiOut.clear();
            if(!block) { warmNotesEvent(0x80,0);
                midiIn.emplace_back(synthLib::MidiEventSource::Host,0xb0,120,0); }
            static_cast<synthLib::Device&>(device).process(inputs,outputs,Chunk,midiIn,midiOut);
        }
        shm->warmupBlocks=warmBlock;
        shm->bootMs=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();
        auto boostUntil=Clock::now();
        const auto cpuStart=cpuMs();
        for(;;) {
            // The warmup can only compile the patch that was loaded at boot.
            // Selecting another one brings its own cold paths, which stall the
            // emulator on that patch's FIRST notes -- measured as 5-8 dropped
            // blocks, at every DSP clock, so it is not a capacity problem. The
            // queue is run deep for a few seconds after a change instead: the
            // extra audio is produced out of the ~20% steady-state headroom,
            // and latency returns to the user's setting once the JIT is quiet.
            uint32_t targetFill=static_cast<uint32_t>(shm->targetFill.load());
            if(Clock::now()<boostUntil) targetFill=std::min<uint32_t>(targetFill*4,TargetFillMax);
            if(shm->audio.available()>=targetFill) { usleep(500); continue; }
            int requested=shm->clock.load();
            if(requested!=applied) { device.setDspClockPercent(requested); applied=requested; }
            midiIn.clear(); midiOut.clear();
            // Bank select is CC 32 on this firmware. CC 0 is accepted and
            // ignored -- every bank then reads back as A001-A100.
            // Queued firmware parameter writes, emitted the way mqLib frames
            // them: index split into a 7-bit high and low byte.
            ParamWrite write;
            while(shm->writes.pop(write)) {
                synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
                event.sysex={0xf0,0x3e,static_cast<uint8_t>(mqLib::IdMicroQ),0x7f,
                    static_cast<uint8_t>(write.multi ? mqLib::SysexCommand::MultiParameterChange
                                                     : mqLib::SysexCommand::GlobalParameterChange),
                    static_cast<uint8_t>(write.index>>7),static_cast<uint8_t>(write.index&0x7f),
                    write.value,0xf7};
                midiIn.push_back(std::move(event));
                boostUntil=Clock::now()+std::chrono::seconds(BoostSeconds);
            }
            if(shm->presetPending.exchange(0)) {
                boostUntil=Clock::now()+std::chrono::seconds(BoostSeconds);
                const int index=std::clamp(shm->preset.load(),0,BankCount*PresetsPerBank-1);
                const int bank=index/PresetsPerBank, program=index%PresetsPerBank;
                // In Multi mode a Program Change would retarget whatever the
                // front panel has selected; the part is addressed by writing
                // its own sound fields instead.
                if(shm->multiMode.load()) {
                    const int slot=std::clamp(shm->part.load(),1,MultiParts)-1;
                    const int base=static_cast<int>(mqLib::MultiParameter::Inst0)+slot*MultiInstStride;
                    for(auto [field,value] : {std::pair{0,bank},std::pair{1,program}}) {
                        synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
                        const int address=base+field;
                        event.sysex={0xf0,0x3e,static_cast<uint8_t>(mqLib::IdMicroQ),0x7f,
                            static_cast<uint8_t>(mqLib::SysexCommand::MultiParameterChange),
                            static_cast<uint8_t>(address>>7),static_cast<uint8_t>(address&0x7f),
                            static_cast<uint8_t>(value),0xf7};
                        midiIn.push_back(std::move(event));
                    }
                } else {
                    midiIn.emplace_back(synthLib::MidiEventSource::Host,0xb0,32,static_cast<uint8_t>(bank));
                    midiIn.emplace_back(synthLib::MidiEventSource::Host,0xc0,static_cast<uint8_t>(program),0);
                }
            }
            Midi message;
            while(shm->midi.pop(message)) {
                synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
                if(message.bytes[0]==0xf0) event.sysex.assign(message.bytes,message.bytes+message.size);
                else {
                    event.a=message.bytes[0];
                    event.b=message.size>1 ? message.bytes[1] : 0;
                    event.c=message.size>2 ? message.bytes[2] : 0;
                }
                midiIn.push_back(std::move(event));
            }
            const auto procStart=Clock::now();
            static_cast<synthLib::Device&>(device).process(inputs,outputs,Chunk,midiIn,midiOut);
            // The firmware streams its 2x20 display as SysexCommand::EmuLCD.
            // Row 2 is the current sound's 16-char name, which is the only
            // authority for it: the names are not plain text in the ROM.
            for(const auto& event:midiOut) {
                const auto& sysex=event.sysex;
                if(sysex.size()<46) continue;
                if(sysex[4]!=static_cast<uint8_t>(mqLib::SysexCommand::EmuLCD)) continue;
                // The whole 20-char row: the name field is inset by two
                // spaces, so copying only 16 from the row start dropped the
                // last two characters of every long name.
                char row[21]{};
                for(int i=0;i<20;++i) row[i]=static_cast<char>(sysex[5+20+i]);
                int end=20; while(end>0 && row[end-1]==' ') row[--end]=0;
                const char* start=row; while(*start==' ') ++start;
                memmove(row,start,strlen(start)+1);
                // Transient firmware messages ("[dumping Sound A001]") land on
                // row 1, never here, so row 2 needs no filtering.
                memcpy(shm->lcdName,row,sizeof(row));
            }
            const auto procUs=static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-procStart).count());
            if(procUs>shm->maxProcUs.load()) shm->maxProcUs=procUs;
            // A block is worth Chunk/44100 of wall; over that is slower than realtime.
            if(procUs>(Chunk*1000000)/44100) ++shm->slowBlocks;
            if(procUs>StallUs) { const auto slot=shm->stalls.fetch_add(1);
                if(slot<16) { shm->stallBlock[slot]=shm->blocks.load(); shm->stallUs[slot]=procUs; } }
            const auto carry=shm->procUsCarry.load()+procUs;
            shm->procMsTotal.fetch_add(carry/1000); shm->procUsCarry=carry%1000;
            float gain=shm->gain.load()*0.01f;
            for(int i=0;i<Chunk;++i) {
                Stereo sample;
                sample.l=static_cast<int16_t>(std::clamp(output[0][i]*gain*32767.0f,-32768.0f,32767.0f));
                sample.r=static_cast<int16_t>(std::clamp(output[1][i]*gain*32767.0f,-32768.0f,32767.0f));
                // A full ring never overwrites unread samples.
                shm->audio.push(sample);
            }
            auto block=shm->blocks.fetch_add(1)+1;
            if((block&255)==0) shm->cpuMs=cpuMs()-cpuStart;
            if(shm->audio.available()>=targetFill) shm->ready.store(1,std::memory_order_release);
        }
    } catch(const std::exception& error) {
        snprintf(shm->error,sizeof(shm->error),"%s",error.what());
        shm->failed.store(1,std::memory_order_release);
    } catch(...) {
        snprintf(shm->error,sizeof(shm->error),"Unknown microQ emulator failure");
        shm->failed.store(1,std::memory_order_release);
    }
}
static void* workerMain(void* context) {
    setupWorker("vavra-loader");
    auto* inst=static_cast<Instance*>(context);
    const pid_t parent=getpid();
    pid_t pid=fork();
    if(pid<0) { inst->workerError=1; return nullptr; }
    if(pid==0) {
        prctl(PR_SET_PDEATHSIG,SIGKILL);
        if(getppid()!=parent) _exit(1);
        pthread_setname_np(pthread_self(),"vavra-emu");
        childMain(inst); _exit(0);
    }
    auto start=Clock::now(), heartbeat=start;
    uint32_t lastBlock=0;
    while(!inst->stop.load()) {
        int status=0;
        if(waitpid(pid,&status,WNOHANG)==pid) {
            if(!inst->shm->failed.load()) inst->workerError=2;
            return nullptr;
        }
        auto now=Clock::now();
        uint32_t block=inst->shm->blocks.load();
        if(block!=lastBlock || inst->shm->audio.available()>=static_cast<uint32_t>(inst->shm->targetFill.load())) { heartbeat=now; lastBlock=block; }
        if(!inst->shm->ready.load() && now-start>std::chrono::seconds(90)) { inst->workerError=3; break; }
        if(inst->shm->ready.load() && now-heartbeat>std::chrono::seconds(10)) { inst->workerError=4; break; }
        usleep(5000);
    }
    kill(pid,SIGKILL); waitpid(pid,nullptr,0);
    return nullptr;
}
static void setParam(void*,const char*,const char*);
static void* create(const char* directory,const char*) {
    if(!directory || strlen(directory)>=1024) return nullptr;
    auto* inst=new(std::nothrow) Instance;
    if(!inst) return nullptr;
    void* region=mmap(nullptr,sizeof(Shared),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(region==MAP_FAILED) { delete inst; return nullptr; }
    inst->shm=new(region) Shared;
    // Mirror the multi the firmware boots with (createInitState's "sequencer"
    // multi): part n on channel n, full volume, sounds A1 A2 A3 then A1.
    for(int slot=0;slot<MultiParts;++slot) {
        inst->shm->partChannel[slot]=slot+1;
        inst->shm->partVolume[slot]=127;
        inst->shm->partPreset[slot]=slot<3 ? slot : 0;
    }
    snprintf(inst->moduleDir,sizeof(inst->moduleDir),"%s",directory);
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr,SCHED_OTHER);
    sched_param normal{};
    pthread_attr_setschedparam(&attr,&normal);
    int result=pthread_create(&inst->worker,&attr,workerMain,inst);
    pthread_attr_destroy(&attr);
    if(result) { munmap(region,sizeof(Shared)); delete inst; return nullptr; }
    return inst;
}
static void destroy(void* context) {
    auto* inst=static_cast<Instance*>(context); if(!inst) return;
    inst->stop=true; pthread_join(inst->worker,nullptr);
    inst->shm->~Shared(); munmap(inst->shm,sizeof(Shared)); delete inst;
}
static void onMidi(void* context,const uint8_t* bytes,int size,int) {
    auto* inst=static_cast<Instance*>(context);
    if(!inst || !bytes || size<1 || size>1024 || !inst->shm->ready.load()) return;
    Midi message; message.size=size; memcpy(message.bytes,bytes,size);
    if(!inst->shm->midi.push(message)) ++inst->shm->midiDrops;
}
static void setParam(void* context,const char* key,const char* value) {
    auto* inst=static_cast<Instance*>(context); if(!inst || !key || !value) return;
    if(!strcmp(key,"dsp_clock")) inst->shm->clock=std::clamp(atoi(value),25,100);
    if(!strcmp(key,"gain")) inst->shm->gain=std::clamp(atoi(value),0,100);
    // Buffer in milliseconds: what the user actually trades away is latency.
    if(!strcmp(key,"buffer_ms")) inst->shm->targetFill=std::clamp(atoi(value)*44100/1000,TargetFillMin,TargetFillMax);
    // The child owns the MIDI; these only record the wish and wake it.
    if(!strcmp(key,"preset")) {
        inst->shm->preset=std::clamp(atoi(value),0,BankCount*PresetsPerBank-1);
        if(inst->shm->multiMode.load())
            inst->shm->partPreset[std::clamp(inst->shm->part.load(),1,MultiParts)-1]=inst->shm->preset.load();
        inst->shm->presetPending=1;
    }
    if(!strcmp(key,"mode")) {
        const int multi=atoi(value) ? 1 : 0;
        inst->shm->multiMode=multi;
        inst->shm->writes.push({static_cast<uint16_t>(mqLib::GlobalParameter::SingleMultiMode),
                                static_cast<uint8_t>(multi),0});
    }
    // The part selector is the module's own: it says which part the part keys
    // below address, and writes nothing to the firmware.
    if(!strcmp(key,"part")) {
        const int slot=std::clamp(atoi(value),1,MultiParts);
        inst->shm->part=slot;
        inst->shm->preset=inst->shm->partPreset[slot-1].load();
    }
    auto partWrite=[&](int field,int raw){
        const int slot=std::clamp(inst->shm->part.load(),1,MultiParts)-1;
        const int address=static_cast<int>(mqLib::MultiParameter::Inst0)+slot*MultiInstStride+field;
        inst->shm->writes.push({static_cast<uint16_t>(address),static_cast<uint8_t>(raw),1});
        return slot;
    };
    if(!strcmp(key,"part_channel")) {
        // 0 selects the global channel; 1..16 are MIDI channels, which the
        // firmware stores offset by one.
        const int channel=std::clamp(atoi(value),0,MultiParts);
        inst->shm->partChannel[partWrite(2,channel ? channel+PartChannelOffset-1 : 0)]=channel;
    }
    if(!strcmp(key,"part_volume")) {
        const int volume=std::clamp(atoi(value),0,127);
        inst->shm->partVolume[partWrite(3,volume)]=volume;
    }
}
static int getError(void* context,char* buffer,int size) {
    auto* inst=static_cast<Instance*>(context); if(!inst || !buffer || size<=0) return 0;
    if(inst->shm->failed.load(std::memory_order_acquire)) return snprintf(buffer,size,"%s",inst->shm->error);
    const char* errors[]={"","Could not fork microQ emulator","microQ emulator exited unexpectedly","microQ boot timed out (90 seconds)","microQ audio stalled"};
    int error=inst->workerError.load();
    if(error) return snprintf(buffer,size,"%s",errors[error]);
    buffer[0]=0; return 0;
}
static int getParam(void* context,const char* key,char* buffer,int size) {
    auto* inst=static_cast<Instance*>(context); if(!inst || !key || !buffer || size<=0) return -1;
    auto* s=inst->shm;
    if(!strcmp(key,"name")) return snprintf(buffer,size,"microQ");
    if(!strcmp(key,"loading") || !strcmp(key,"is_loading")) return snprintf(buffer,size,"%d",!s->ready.load() && !s->failed.load() && !inst->workerError.load());
    if(!strcmp(key,"chain_params")) {
        // Built here rather than stored: 300 option strings are ~7 KB, the
        // host's ceiling is 65535 (SHADOW_PARAM_VALUE_LEN) and it REJECTS the
        // module outright above it, so the write is bounded and checked.
        int used=snprintf(buffer,size,"%s",R"([{"key":"preset","name":"Preset","type":"enum","default":0,"options":[)");
        for(size_t i=0;i<sizeof(vavra::g_presetNames)/sizeof(*vavra::g_presetNames) && used<size;++i)
            used+=snprintf(buffer+used,size-used,"%s\"%s\"",i?",":"",vavra::g_presetNames[i]);
        if(used>=size) { buffer[0]=0; return -1; }
        used+=snprintf(buffer+used,size-used,"%s",R"(]},{"key":"mode","name":"Mode","type":"enum","options":["Single","Multi"],"default":0},{"key":"part","name":"Part","type":"int","min":1,"max":16,"default":1,"visible_if":{"key":"mode","equals":1}},{"key":"part_channel","name":"Part Ch","short_name":"Ch","type":"int","min":0,"max":16,"default":0,"visible_if":{"key":"mode","equals":1}},{"key":"part_volume","name":"Part Vol","short_name":"Vol","type":"int","min":0,"max":127,"default":127,"visible_if":{"key":"mode","equals":1}},{"key":"dsp_clock","name":"DSP Clock","short_name":"Clock","type":"int","min":25,"max":100,"default":50,"unit":"%"},{"key":"gain","name":"Gain","type":"int","min":0,"max":100,"default":70},{"key":"buffer_ms","name":"Buffer","short_name":"Buf","type":"int","min":11,"max":139,"default":17,"unit":"ms"}])");
        return used<size ? used : -1;
    }
    if(!strcmp(key,"ui_hierarchy")) return snprintf(buffer,size,"%s",R"({"pad_layout":"chromatic","levels":{"root":{"label":"microQ","knobs":["preset","mode","part","part_channel","part_volume","gain","dsp_clock","buffer_ms"],"params":["preset","mode","part","part_channel","part_volume","gain","dsp_clock","buffer_ms"]}}})");
    if(!strcmp(key,"loading_status")) return snprintf(buffer,size,"%s",s->ready.load() ? "Ready" : "Booting microQ...");
    if(!strcmp(key,"dsp_clock")) return snprintf(buffer,size,"%d",s->clock.load());
    if(!strcmp(key,"gain")) return snprintf(buffer,size,"%d",s->gain.load());
    if(!strcmp(key,"buffer_ms")) return snprintf(buffer,size,"%d",s->targetFill.load()*1000/44100);
    if(!strcmp(key,"preset")) return snprintf(buffer,size,"%d",s->preset.load());
    if(!strcmp(key,"mode")) return snprintf(buffer,size,"%d",s->multiMode.load());
    if(!strcmp(key,"part")) return snprintf(buffer,size,"%d",s->part.load());
    if(!strcmp(key,"part_channel")) return snprintf(buffer,size,"%d",s->partChannel[std::clamp(s->part.load(),1,MultiParts)-1].load());
    if(!strcmp(key,"part_volume")) return snprintf(buffer,size,"%d",s->partVolume[std::clamp(s->part.load(),1,MultiParts)-1].load());
    // The LIVE name, off the device's own display -- so a ROM whose sounds
    // differ from the compiled-in table still reports the truth.
    if(!strcmp(key,"preset_name")) return snprintf(buffer,size,"%s",s->lcdName);
    if(!strcmp(key,"stalls")) {
        int used=0;
        used+=snprintf(buffer+used,size-used,"warm_stalls=%u",s->warmStalls.load());
        for(uint32_t i=0;i<std::min<uint32_t>(16,s->warmStalls.load()) && used<size-24;++i)
            used+=snprintf(buffer+used,size-used," %u@%u",s->warmStallUs[i].load(),s->warmStallBlock[i].load());
        used+=snprintf(buffer+used,size-used," | stalls=%u",s->stalls.load());
        for(uint32_t i=0;i<std::min<uint32_t>(16,s->stalls.load()) && used<size-24;++i)
            used+=snprintf(buffer+used,size-used," %u@%u",s->stallUs[i].load(),s->stallBlock[i].load());
        return used;
    }
    if(!strcmp(key,"diagnostics")) return snprintf(buffer,size,"boot_ms=%u cpu_ms=%u blocks=%u underruns=%u midi_drops=%u warmup_blocks=%u warmup_peak=%u max_proc_us=%u slow_blocks=%u proc_ms=%u",s->bootMs.load(),s->cpuMs.load(),s->blocks.load(),s->underruns.load(),s->midiDrops.load(),s->warmupBlocks.load(),s->warmupPeak.load(),s->maxProcUs.load(),s->slowBlocks.load(),s->procMsTotal.load());
    buffer[0]=0; return -1;
}
static void render(void* context,int16_t* out,int frames) {
    if(!out || frames<=0) return;
    auto* inst=static_cast<Instance*>(context);
    bool ready=inst && inst->shm->ready.load(std::memory_order_acquire);
    bool missed=false;
    for(int i=0;i<frames;++i) {
        Stereo sample;
        if(ready && !inst->shm->audio.pop(sample)) missed=true;
        out[i*2]=sample.l; out[i*2+1]=sample.r;
    }
    if(missed) ++inst->shm->underruns;
}
plugin_api_v2_t api{2,create,destroy,onMidi,setParam,getParam,getError,render};
}
extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t*) { return &api; }
