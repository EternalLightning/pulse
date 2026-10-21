#include <windows.h>
#include <cstdint>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    const std::wstring mode = argv[1];
    if (mode == L"progress" || mode == L"progress-error" || mode == L"progress-cancel") {
        const char update[] = "\r 25%";
        DWORD written = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), update, sizeof(update) - 1, &written, nullptr)) return 2;
        if (mode == L"progress-cancel") Sleep(INFINITE);
        for (int i = 0; i < 20000; ++i)
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), update, sizeof(update) - 1, &written, nullptr)) return 2;
        const char final[] = "\r100%";
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), final, sizeof(final) - 1, &written, nullptr)) return 2;
        if (mode == L"progress-error") {
            const char error[] = "fixture-error";
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), error, sizeof(error) - 1, &written, nullptr);
            return 2;
        }
        return 0;
    }
    if (mode == L"inherit" && argc == 3) {
        HANDLE event = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[2])));
        return SetEvent(event) ? 1 : 0;
    }
    if (mode == L"output" || mode == L"stream") {
        char buffer[8192];
        for (char& byte : buffer) byte = 'x';
        const unsigned blocks = mode == L"stream" ? 2048 : 128;
        for (unsigned i = 0; i < blocks; ++i) {
            DWORD written = 0;
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, sizeof(buffer), &written, nullptr)) return 2;
        }
        return 0;
    }
    if (mode == L"wait" && argc == 3) {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]);
        if (!ready) return 2;
        SetEvent(ready);
        CloseHandle(ready);
        Sleep(INFINITE);
        return 0;
    }
    if (mode == L"final") {
        const char text[] = "final-output";
        DWORD written = 0;
        return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, sizeof(text) - 1, &written, nullptr) ? 0 : 2;
    }
    return 2;
}
