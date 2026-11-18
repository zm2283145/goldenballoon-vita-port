#ifndef MDKR_VITA_NETWORK_RUNTIME_H
#define MDKR_VITA_NETWORK_RUNTIME_H

#if defined(__vita__)

#ifdef __cplusplus
extern "C" {
#endif

int mdkr_vita_network_acquire(void);
void mdkr_vita_network_release(void);
int mdkr_vita_network_restore_infrastructure(void);
void mdkr_vita_network_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

#endif
