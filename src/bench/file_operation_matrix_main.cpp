// SPDX-License-Identifier: Apache-2.0
#include "file_operation_matrix.h"
#include <cstdio>
#include <exception>
#include <string_view>

int wmain(int argc, wchar_t** argv) {
    const std::wstring_view mode = argc > 1 ? argv[1] : L"";
    const bool pair = mode == L"--file-operation-matrix-pair";
    const bool performance = mode == L"--file-operation-perf" || mode == L"--file-operation-perf-few";
    if ((!pair && !performance && mode != L"--file-operation-matrix") ||
        (pair ? argc != 4 : argc != 3 && argc != 4)) {
        std::fprintf(stderr, "usage: pulse_file_operation_matrix_test --file-operation-matrix|--file-operation-matrix-pair|--file-operation-perf|--file-operation-perf-few FIRST_PARENT [SECOND_PARENT]\n");
        return 2;
    }
    try {
        return pulse::bench::RunFileOperationMatrix(argv[2], argc == 4 ? argv[3] : L"",
            performance, mode == L"--file-operation-perf-few", pair);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] matrix aborted: %s\n", error.what());
        return 1;
    }
}
