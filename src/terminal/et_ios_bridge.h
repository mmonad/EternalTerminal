#ifndef ET_IOS_BRIDGE_H
#define ET_IOS_BRIDGE_H

#include <stdio.h>
#include <sys/ioctl.h>

#if __cplusplus
extern "C"
#endif
int et_client_main(
    FILE *f_in, FILE *f_out, struct winsize *ws,
    const char *host, int port,
    const char *id, const char *passkey,
    int keepalive_secs);

#endif
