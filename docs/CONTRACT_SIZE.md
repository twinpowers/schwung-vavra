# The 64 KB contract ceiling, and what it costs this module

`chain_params` and `ui_hierarchy` are each served through one
`shadow_param_t.value` buffer, sized by `SHADOW_PARAM_VALUE_LEN` (65536) in
Schwung's `src/host/shadow_constants.h`. A module whose answer is longer is not
truncated -- the chain host **rejects the module** with "UI buffer overflow"
(`chain_host.c:518`), leaving a slot that loads and shows an error.

## Where this module sits

| string | bytes | share of the ceiling |
|---|---|---|
| `chain_params` (286 params, incl. 300 preset options) | 51,904 | 79% |
| `ui_hierarchy` (23 levels) | 22,543 | 34% |

## What had to be left out to fit

The microQ publishes **449** single-sound parameters. This module exposes 286.
The omissions are not editorial:

- **Four of eight slots in each modulation matrix.** Every slot repeats its own
  source and destination option lists verbatim -- a Standard slot costs 494
  bytes, of which 440 is the 58-entry destination list -- so sixteen slots cost
  about 18 KB. Four of each fit; eight of each do not.
- **The 5.1 / surround delay parameters** (15 of them), which are meaningless
  on a stereo device. This one is a real editorial choice.
- **Arpeggiator user patterns** (80 parameters: 16 steps x step, glide, accent,
  length, timing).
- **Each envelope's trigger mode** (4), and the vocoder's 28.

## The proposal

Raise `SHADOW_PARAM_VALUE_LEN` from 65536 to 131072 -- but **not on its own**.

`shadow_param_t.value` lives in the `/schwung-param` SHM segment, so the
segment grows by 64 KB. That part is cheap and well-trodden: /dev/shm is tmpfs
and allocates by page, `shadow_shm_map()` fstats on attach and refuses a short
segment with a log line rather than a SIGBUS, and the shim and shadow_ui deploy
together. Two earlier resizes (#358, #361) used exactly that procedure.

The blocker is elsewhere. `chain_mod_refresh_target_param_cache`
(`src/modules/chain/dsp/chain_mod.c:561`) declares BOTH
`char buf[SHADOW_PARAM_VALUE_LEN]` and `chain_param_info_t parsed[MAX_CHAIN_PARAMS]`
as locals. `chain_param_info_t` is ~4.3 KB -- `options[128][32]` alone is 4 KB --
so that array is **~1.05 MB of stack**, and the function is called every
`MOD_PARAM_CACHE_REFRESH_MS` (250 ms). Doubling the constant adds another 64 KB
to that frame.

So the order is: make those two locals `static` (or heap), *then* raise the
constant. The first change is worth making regardless of this module.
