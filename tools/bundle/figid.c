/* host tool: print the LEGO Dimensions figure id of each 180-byte tag file given on the command line */
#include <stdio.h>
#include <stdint.h>
#include "ldproto.h"
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        uint8_t t[LD_TAG_SIZE];
        FILE *f = fopen(argv[i], "rb");
        size_t n = f ? fread(t, 1, sizeof t, f) : 0;
        if (f) fclose(f);
        printf("%u\t%s\n", n == sizeof t ? ld_tag_figure_id(t) : 0u, argv[i]);
    }
    return 0;
}
