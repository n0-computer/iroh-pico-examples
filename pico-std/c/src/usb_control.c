#include <stdio.h>
#include "pico/stdlib.h"

int main(void) {
    stdio_init_all();
    while (true) {
        puts("Pure Pico SDK USB control image");
        stdio_flush();
        sleep_ms(1000);
    }
}
