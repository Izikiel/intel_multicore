#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char * argv[]) {
    /* ./genbin bin_in bin_out size addr */

    char * bin_in  = argv[1];
    char * bin_out = argv[2];

    int size = atoi(argv[3]);

    unsigned int addr = 0;

    if (argc > 4 && strlen(argv[4]) > 0) {
        addr = (unsigned int) strtoll(argv[4], NULL, 16);
    }

    printf("bin_in  : %s\n", bin_in);
    printf("bin_out : %s\n", bin_out);
    printf("size    : %d\n", size);
    printf("addr    : 0x%08x\n", addr);

    FILE * f_in  = fopen(bin_in, "r");
    FILE * f_out = fopen(bin_out, "w");

    int byte_count = 0;

    unsigned char byte = 0;

    while (fread(&byte, 1, 1, f_in) != 0) {
        fwrite(&byte, 1, 1, f_out);
        byte_count++;
    }

    byte = 0;
    while (byte_count < size - 4) {
        fwrite(&byte, 1, 1, f_out);
        byte_count++;
    }

    fwrite(&addr, 1, 4, f_out);

    fclose(f_in);
    fclose(f_out);

    return 0;
}
