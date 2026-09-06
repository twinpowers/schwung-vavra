// Ask the booted firmware for every ROM single and print its name.
//
// The names are NOT plain text in the OS image -- a scan of all 524288 bytes
// for a 16-char name table at any plausible stride finds nothing -- so the only
// authority for what a preset is called is the running device. This boots it
// once (~13 s) and requests each single by bank and program.
#include "mqLib/device.h"
#include "mqLib/mqmiditypes.h"
#include "dsp56kBase/logging.h"
#include "dsp/runtime.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
    if(argc < 2) { fprintf(stderr, "usage: dump_presets <rom.bin> [banks]\n"); return 2; }
    Logging::setLogFunc([](const std::string&) {});
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if(!file) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> rom(static_cast<size_t>(file.tellg()));
    file.seekg(0); file.read(reinterpret_cast<char*>(rom.data()), rom.size());
    if(!vavra::normalizeRom(rom)) { fprintf(stderr, "not a 512 KiB OS 2.23 image\n"); return 1; }

    synthLib::DeviceCreateParams params;
    params.hostSamplerate = params.preferredSamplerate = 44100;
    params.romData = rom; params.romName = argv[1];
    // mc68k logs every flash write through its own unconditional console
    // logger, and it keeps doing so long after boot -- so stdout stays on
    // /dev/null for the whole run and the names go out on a saved descriptor.
    FILE* out = fdopen(dup(STDOUT_FILENO), "w");
    int quiet = open("/dev/null", O_WRONLY);
    if(quiet >= 0) { dup2(quiet, STDOUT_FILENO); dup2(quiet, STDERR_FILENO); close(quiet); }
    mqLib::Device device(params);
    if(!device.isValid()) { fprintf(out, "device rejected the firmware\n"); return 1; }

    constexpr int Chunk = 64;
    float input[Chunk]{}, output[6][Chunk]{};
    synthLib::TAudioInputs inputs{}; inputs[0] = input; inputs[1] = input;
    synthLib::TAudioOutputs outputs{};
    for(int c = 0; c < 6; ++c) outputs[c] = output[c];
    std::vector<synthLib::SMidiEvent> midiIn, midiOut;

    // The firmware does not answer a bank Single Request here, but it does
    // stream its emulated front panel: SysexCommand::EmuLCD carries the whole
    // 2x20 display, and in Sound mode the top line is the current sound's
    // name. So the list is read off the screen, exactly as a person would.
    // synthLib carries sysex in a pmr vector, so take it generically.
    auto lcdLine = [](const auto& sysex, int line) {
        std::string text(sysex.begin() + 5, sysex.begin() + 5 + 40);
        text = text.substr(line * 20, 20);
        while(!text.empty() && text.back() == ' ') text.pop_back();
        return text;
    };
    auto run = [&](int blocks, std::string* lcd) {
        for(int block = 0; block < blocks; ++block) {
            midiOut.clear();
            static_cast<synthLib::Device&>(device).process(inputs, outputs, Chunk, midiIn, midiOut);
            midiIn.clear();
            for(const auto& event : midiOut) {
                const auto& sysex = event.sysex;
                if(sysex.size() < 46) continue;
                if(sysex[4] != static_cast<uint8_t>(mqLib::SysexCommand::EmuLCD)) continue;
                if(lcd) *lcd = lcdLine(sysex, 0) + "\t" + lcdLine(sysex, 1);
            }
        }
    };
    // "state": report the mode the firmware is actually in, and how the Multi
    // maps MIDI channels to sounds. Guessing at this from createInitState()
    // was wrong once already -- the display says Multi where the code says it
    // asked for Single.
    if(argc > 2 && !strcmp(argv[2], "state")) {
        auto request = [&](std::initializer_list<uint8_t> bytes, uint8_t wantCmd, std::vector<uint8_t>& into) {
            synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
            event.sysex.assign(bytes.begin(), bytes.end());
            midiIn.clear(); midiIn.push_back(event);
            for(int block = 0; block < 2000 && into.empty(); ++block) {
                midiOut.clear();
                static_cast<synthLib::Device&>(device).process(inputs, outputs, Chunk, midiIn, midiOut);
                midiIn.clear();
                for(const auto& reply : midiOut)
                    if(reply.sysex.size() > 8 && reply.sysex[4] == wantCmd)
                        into.assign(reply.sysex.begin(), reply.sysex.end());
            }
        };
        constexpr uint8_t Waldorf = 0x3e, MicroQ = 0x10, Omni = 0x7f;
        std::vector<uint8_t> global, multi;
        request({0xf0, Waldorf, MicroQ, Omni,
                 static_cast<uint8_t>(mqLib::SysexCommand::GlobalRequest), 0xf7},
                static_cast<uint8_t>(mqLib::SysexCommand::GlobalDump), global);
        if(global.empty()) { fprintf(out, "no Global dump\n"); return 1; }
        auto g = [&](mqLib::GlobalParameter param) {
            return static_cast<int>(global[mqLib::IdxGlobalParamFirst + static_cast<uint32_t>(param)]);
        };
        fprintf(out, "global: mode=%s midi_channel=%d instrument_selection=%d multi=%d\n",
                g(mqLib::GlobalParameter::SingleMultiMode) ? "MULTI" : "SINGLE",
                g(mqLib::GlobalParameter::MidiChannel),
                g(mqLib::GlobalParameter::InstrumentSelection),
                g(mqLib::GlobalParameter::MultiNumber) + 1);
        fprintf(out, "global: instrument sounds A=%d:%d B=%d:%d C=%d:%d D=%d:%d\n",
                g(mqLib::GlobalParameter::InstrumentABankNumber), g(mqLib::GlobalParameter::InstrumentASingleNumber) + 1,
                g(mqLib::GlobalParameter::InstrumentBBankNumber), g(mqLib::GlobalParameter::InstrumentBSingleNumber) + 1,
                g(mqLib::GlobalParameter::InstrumentCBankNumber), g(mqLib::GlobalParameter::InstrumentCSingleNumber) + 1,
                g(mqLib::GlobalParameter::InstrumentDBankNumber), g(mqLib::GlobalParameter::InstrumentDSingleNumber) + 1);

        request({0xf0, Waldorf, MicroQ, Omni,
                 static_cast<uint8_t>(mqLib::SysexCommand::MultiRequest),
                 static_cast<uint8_t>(mqLib::MidiBufferNum::MultiEditBuffer), 0x00, 0xf7},
                static_cast<uint8_t>(mqLib::SysexCommand::MultiDump), multi);
        if(multi.empty()) { fprintf(out, "no Multi dump\n"); return 0; }
        auto m = [&](int instrument, mqLib::MultiParameter field) {
            const int base = static_cast<int>(mqLib::MultiParameter::Inst0) + instrument * 22;
            const int offset = static_cast<int>(field) - static_cast<int>(mqLib::MultiParameter::Inst0SoundBank);
            return static_cast<int>(multi[mqLib::IdxMultiParamFirst + base + offset]);
        };
        for(int instrument = 0; instrument < 16; ++instrument) {
            const int channel = m(instrument, mqLib::MultiParameter::Inst0MidiChannel);
            fprintf(out, "multi inst%-2d channel=%-3d sound=%d:%-3d volume=%-3d keys=%d-%d\n",
                    instrument + 1, channel,
                    m(instrument, mqLib::MultiParameter::Inst0SoundBank),
                    m(instrument, mqLib::MultiParameter::Inst0SoundNumber) + 1,
                    m(instrument, mqLib::MultiParameter::Inst0Volume),
                    m(instrument, mqLib::MultiParameter::Inst0KeyLow),
                    m(instrument, mqLib::MultiParameter::Inst0KeyHigh));
        }
        return 0;
    }
    // "render <clock%> <preset> <out.wav>": deterministic offline render --
    // no fork, no ring, no host timing -- so two clocks can be compared sample
    // for sample. This is the only way to answer "does a lower DSP clock CHANGE
    // THE SOUND, or only how many voices fit?"
    if(argc > 2 && !strcmp(argv[2], "render")) {
        const int clockPercent = argc > 3 ? atoi(argv[3]) : 100;
        const int preset = argc > 4 ? atoi(argv[4]) : 0;
        const char* wavPath = argc > 5 ? argv[5] : "render.wav";
        const int voices = argc > 6 ? atoi(argv[6]) : 2;
        device.setDspClockPercent(clockPercent);
        auto spin = [&](int blocks, std::vector<int16_t>* into) {
            for(int block = 0; block < blocks; ++block) {
                midiOut.clear();
                static_cast<synthLib::Device&>(device).process(inputs, outputs, Chunk, midiIn, midiOut);
                midiIn.clear();
                if(!into) continue;
                for(int i = 0; i < Chunk; ++i) {
                    for(int channel = 0; channel < 2; ++channel) {
                        float sample = output[channel][i] * 32767.0f;
                        sample = std::max(-32768.0f, std::min(32767.0f, sample));
                        into->push_back(static_cast<int16_t>(sample));
                    }
                }
            }
        };
        spin(400, nullptr);
        midiIn.emplace_back(synthLib::MidiEventSource::Host, 0xb0, 32, static_cast<uint8_t>(preset / 100));
        midiIn.emplace_back(synthLib::MidiEventSource::Host, 0xc0, static_cast<uint8_t>(preset % 100), 0);
        spin(600, nullptr);
        const uint8_t notes[] = {60, 64, 67, 72, 76, 79, 84, 88};
        for(int i = 0; i < std::min(voices, 8); ++i)
            midiIn.emplace_back(synthLib::MidiEventSource::Host, 0x90, notes[i], 100);
        std::vector<int16_t> pcm;
        spin(1400, &pcm);
        for(int i = 0; i < std::min(voices, 8); ++i)
            midiIn.emplace_back(synthLib::MidiEventSource::Host, 0x80, notes[i], 0);
        spin(700, &pcm);

        const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
        FILE* wav = fopen(wavPath, "wb");
        if(!wav) { fprintf(out, "cannot write %s\n", wavPath); return 1; }
        auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, wav); };
        auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, wav); };
        fwrite("RIFF", 1, 4, wav); put32(36 + dataBytes); fwrite("WAVEfmt ", 1, 8, wav);
        put32(16); put16(1); put16(2); put32(44100); put32(44100 * 4); put16(4); put16(16);
        fwrite("data", 1, 4, wav); put32(dataBytes);
        fwrite(pcm.data(), 2, pcm.size(), wav);
        fclose(wav);
        fprintf(out, "wrote %s: clock=%d%% preset=%d voices=%d frames=%zu\n",
                wavPath, clockPercent, preset, voices, pcm.size() / 2);
        return 0;
    }
    // "multi": switch the firmware to Multi mode and play one channel at a
    // time, to establish that the 16 parts really do answer separately before
    // any of it is exposed as module parameters.
    if(argc > 2 && !strcmp(argv[2], "multi")) {
        constexpr uint8_t Waldorf = 0x3e, MicroQ = 0x10, Omni = 0x7f;
        auto globalParam = [&](mqLib::GlobalParameter param, uint8_t value) {
            const auto p = static_cast<uint8_t>(param);
            synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
            event.sysex = {0xf0, Waldorf, MicroQ, Omni,
                           static_cast<uint8_t>(mqLib::SysexCommand::GlobalParameterChange),
                           static_cast<uint8_t>(p >> 7), static_cast<uint8_t>(p & 0x7f), value, 0xf7};
            midiIn.clear(); midiIn.push_back(event);
        };
        auto spin = [&](int blocks) {
            for(int block = 0; block < blocks; ++block) {
                midiOut.clear();
                static_cast<synthLib::Device&>(device).process(inputs, outputs, Chunk, midiIn, midiOut);
                midiIn.clear();
            }
        };
        const int wantMulti = argc > 3 ? atoi(argv[3]) : 1;
        globalParam(mqLib::GlobalParameter::SingleMultiMode, static_cast<uint8_t>(wantMulti));
        spin(600);
        fprintf(out, "requested mode=%s\n", wantMulti ? "MULTI" : "SINGLE");
        for(int channel = 0; channel < 4; ++channel) {
            synthLib::SMidiEvent on(synthLib::MidiEventSource::Host,
                static_cast<uint8_t>(0x90 | channel), 60, 100);
            midiIn.clear(); midiIn.push_back(on);
            double sum = 0; int peak = 0; long count = 0;
            for(int block = 0; block < 700; ++block) {
                midiOut.clear();
                static_cast<synthLib::Device&>(device).process(inputs, outputs, Chunk, midiIn, midiOut);
                midiIn.clear();
                for(int i = 0; i < Chunk; ++i) {
                    const int sample = static_cast<int>(output[0][i] * 32767.0f);
                    peak = std::max(peak, abs(sample)); sum += double(sample) * sample; ++count;
                }
            }
            synthLib::SMidiEvent off(synthLib::MidiEventSource::Host,
                static_cast<uint8_t>(0x80 | channel), 60, 0);
            midiIn.clear(); midiIn.push_back(off);
            spin(400);
            fprintf(out, "channel=%d peak=%d rms=%.1f\n", channel + 1, peak, sqrt(sum / count));
            fflush(out);
        }
        return 0;
    }
    const int banks = argc > 2 ? atoi(argv[2]) : 3;
    const int bankCC = argc > 3 ? atoi(argv[3]) : 32;
    std::string lcd;
    run(400, &lcd);
    fprintf(out, "# boot screen: %s\n", lcd.c_str());
    for(int bank = 0; bank < banks; ++bank) {
        for(int program = 0; program < 100; ++program) {
            // Bank Select: CC 0 (MSB) is ignored by this firmware -- every
            // bank came back reading A001-A100 -- so the CC is selectable.
            synthLib::SMidiEvent bankSelect(synthLib::MidiEventSource::Host, 0xb0, static_cast<uint8_t>(bankCC), static_cast<uint8_t>(bank));
            synthLib::SMidiEvent change(synthLib::MidiEventSource::Host, 0xc0, static_cast<uint8_t>(program), 0);
            midiIn.clear(); midiIn.push_back(bankSelect); midiIn.push_back(change);
            lcd.clear();
            run(200, &lcd);
            fprintf(out, "%c%d\t%s\n", 'A' + bank, program + 1, lcd.c_str());
            fflush(out);
        }
    }
    return 0;
}
