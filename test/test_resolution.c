/* The resolution safety net (src/injector/main.c res_effective): a launch above 1080 that never
 * reaches the toy pad drops one step at the next launch; a launch that does reaches is kept. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DATA_DIR "/tmp/ldres_test"
#define LOG(...) ((void)0)
static int notified;
static void notify(const char *fmt, ...) { (void)fmt; notified++; }
static struct { unsigned resolution; } cfg;
#include RES_INC
static void trial(unsigned r) { FILE *f = fopen(RES_TRIAL, "w"); fprintf(f, "%u\n", r); fclose(f); }   /* apply_resolution */
static void talked(void) { remove(RES_TRIAL); }                                                         /* monitor() */
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    if (system("rm -rf " DATA_DIR " && mkdir -p " DATA_DIR)) return 1;
    cfg.resolution = 2160;
    unsigned r = res_effective(); CHECK(r == 2160); trial(r);   /* 1st 4K start ... crashes (no talk) */
    r = res_effective(); CHECK(r == 1440 && notified == 1); trial(r); talked();   /* 1440 works */
    r = res_effective(); CHECK(r == 1440 && notified == 1); trial(r);             /* stays 1440, then crashes */
    r = res_effective(); CHECK(r == 1080 && notified == 2);                      /* drops to 1080 */
    r = res_effective(); CHECK(r == 1080);                                         /* 1080 is never a trial */
    cfg.resolution = 1440;                                                         /* config changed: cap reset */
    r = res_effective(); CHECK(r == 1440); trial(r); talked();
    cfg.resolution = 2160; remove(RES_CAP);                                        /* retry 4K by deleting the file */
    r = res_effective(); CHECK(r == 2160); trial(r); talked();                     /* works this time */
    r = res_effective(); CHECK(r == 2160);
    printf(fails ? "resolution: %d FAILED\n" : "resolution: all checks passed\n", fails);
    return fails != 0;
}
