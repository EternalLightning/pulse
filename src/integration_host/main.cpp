#include "../app/system_ctx_registry.h"

#include <cwchar>

int wmain(int argc, wchar_t** argv) {
    if (argc != 7 || wcscmp(argv[1], L"--system-ctx-apply") != 0 ||
        wcscmp(argv[3], L"--result") != 0 || wcscmp(argv[5], L"--token") != 0) return 2;
    return pulse::app::sysctx::RunElevatedBatch(argv[2], argv[4], argv[6]);
}
