#include <stdint.h>
/* Example native Purplecode Controlang plugin ABI. */
int64_t purple_demo_double(int64_t value, int64_t a1, int64_t a2,
                           int64_t a3, int64_t a4, int64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return value * 2;
}
