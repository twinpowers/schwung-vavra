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
