// The one runnable check: exercises the strobe logic with no keyboard and no Synthesia.
#include <cstdio>
#include "strobe.h"

int main() {
    bool ok = Strobe::SelfTest();
    printf("strobe selftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
