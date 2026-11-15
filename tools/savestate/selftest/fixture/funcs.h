#include "recomp.h"
#ifdef __cplusplus
extern "C" {
#endif
void proc_entry(uint8_t* rdram, recomp_context* ctx);
void mid_fn(uint8_t* rdram, recomp_context* ctx);
void leaf_a(uint8_t* rdram, recomp_context* ctx);
void leaf_b(uint8_t* rdram, recomp_context* ctx);
void tail_fn(uint8_t* rdram, recomp_context* ctx);
void process_sleep(uint8_t* rdram, recomp_context* ctx);
void live_bad(uint8_t* rdram, recomp_context* ctx);
void no_sleep(uint8_t* rdram, recomp_context* ctx);
void blitz_osStopThread(uint8_t* rdram, recomp_context* ctx);
#ifdef __cplusplus
}
#endif
