#pragma once
#include <cstdint>
// Stable prefix of Schwung's host API; only these fields are used by this module.
extern "C" {
struct host_api_v1_t {
    uint32_t api_version;
    int sample_rate, frames_per_block;
    uint8_t* mapped_memory;
    int audio_out_offset, audio_in_offset;
    void (*log)(const char*);
    int (*midi_send_internal)(const uint8_t*, int);
    int (*midi_send_external)(const uint8_t*, int);
};
struct plugin_api_v2_t {
    uint32_t api_version;
    void* (*create_instance)(const char*, const char*);
    void (*destroy_instance)(void*);
    void (*on_midi)(void*, const uint8_t*, int, int);
    void (*set_param)(void*, const char*, const char*);
    int (*get_param)(void*, const char*, char*, int);
    int (*get_error)(void*, char*, int);
    void (*render_block)(void*, int16_t*, int);
};
}
