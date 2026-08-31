#include <stdio.h>
#include "pico/stdlib.h"

int main(void) {
    stdio_init_all();
    while (true) {
        puts("RP2350 USB probe alive");
        sleep_ms(1000);
    }
}
