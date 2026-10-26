/* LegoToypad web remote hosted by the injector (see webremote.c) */
#pragma once
#include <stdint.h>
#include <sys/types.h>
int  web_start(void);                         /* starts the HTTP server if /data/ldmod/web exists */
void web_attach(pid_t pid, intptr_t mem);     /* game injected: figure commands and pad state go live */
void web_detach(void);                        /* game exited */
void web_feed_out(const uint8_t *msg, int len); /* one OUT report the game sent to the Toy Pad (LED mirror) */
