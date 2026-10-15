/* Opt-in CPU frame profiling. GPU execution is deliberately not inferred. */
#ifndef LQ_FRAME_TIMING_H
#define LQ_FRAME_TIMING_H

typedef enum {
    LQ_PROFILE_FRAME,
    LQ_PROFILE_INPUT_COMMANDS,
    LQ_PROFILE_NETWORK,
    LQ_PROFILE_SIMULATION,
    LQ_PROFILE_CLIENT,
    LQ_PROFILE_SCENE,
    LQ_PROFILE_UI,
    LQ_PROFILE_PRESENT,
    LQ_PROFILE_AUDIO,
    LQ_PROFILE_COUNT
} lq_profile_stage;

void LQ_ProfileInit(void);
void LQ_ProfileReset(void);
void LQ_ProfileBegin(lq_profile_stage stage);
void LQ_ProfileEnd(lq_profile_stage stage);
void LQ_ProfileReport(void);

#endif
