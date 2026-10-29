#include "session_layout_cases.h"

int main() {
    return pulse::bench::RunSessionLayoutTests() ? 0 : 1;
}
