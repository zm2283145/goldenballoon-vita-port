#ifndef MDKR_VITA_ADHOC_H
#define MDKR_VITA_ADHOC_H

#if defined(__vita__) && defined(MDKR_ENABLE_VITA_ADHOC)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdkrVitaAdhocState {
    MDKR_VITA_ADHOC_IDLE = 0,
    MDKR_VITA_ADHOC_CONNECTING,
    MDKR_VITA_ADHOC_SEARCHING,
    MDKR_VITA_ADHOC_CONNECTED,
    MDKR_VITA_ADHOC_ERROR
} MdkrVitaAdhocState;

int mdkr_vita_adhoc_start(void);
void mdkr_vita_adhoc_update(void);
void mdkr_vita_adhoc_stop(void);
MdkrVitaAdhocState mdkr_vita_adhoc_state(void);
const char *mdkr_vita_adhoc_status(void);

#ifdef __cplusplus
}
#endif

#endif

#endif
