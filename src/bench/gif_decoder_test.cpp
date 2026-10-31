// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#define PULSE_GIF_DECODER_TESTING 1
#include "../preview_host/main.cpp"
#include <array>
#include <cstdio>
#include <filesystem>

namespace {
int failures = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (!value) ++failures;
}

std::vector<uint8_t> FixtureBytes(uint16_t canvas_w = 3, uint16_t canvas_h = 2,
                                  uint8_t red_green = 0, bool small_first = false) {
    std::vector<uint8_t> bytes{'G', 'I', 'F', '8', '9', 'a'};
    const auto word = [&](uint16_t value) {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    };
    word(canvas_w); word(canvas_h);
    bytes.insert(bytes.end(), {0x81, 0, 0,
        0, 0, 0, 255, red_green, 0, 0, 255, 0, 0, 0, 255});
    bytes.insert(bytes.end(), {0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
                              3, 1, 2, 0, 0});
    const auto frame = [&](uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint8_t color, uint8_t disposal, uint16_t delay) {
        bytes.insert(bytes.end(), {0x21, 0xF9, 4, static_cast<uint8_t>((disposal << 2) | 1)});
        word(delay); bytes.insert(bytes.end(), {0, 0, 0x2C});
        word(x); word(y); word(w); word(h); bytes.push_back(0);
        bytes.push_back(2);
        // A clear code before each literal keeps every code three bits wide.
        std::vector<uint8_t> packed;
        uint32_t bits = 0; unsigned bit_count = 0;
        const auto code = [&](uint32_t value) {
            bits |= value << bit_count; bit_count += 3;
            while (bit_count >= 8) {
                packed.push_back(static_cast<uint8_t>(bits));
                bits >>= 8; bit_count -= 8;
            }
        };
        for (uint32_t i = 0; i < static_cast<uint32_t>(w) * h; ++i) { code(4); code(color); }
        code(5);
        if (bit_count) packed.push_back(static_cast<uint8_t>(bits));
        bytes.push_back(static_cast<uint8_t>(packed.size()));
        bytes.insert(bytes.end(), packed.begin(), packed.end()); bytes.push_back(0);
    };
    frame(0, 0, small_first ? 1 : 3, small_first ? 1 : 2, 1, 1, 5);
    frame(0, 0, 1, 1, 3, 2, 6);
    frame(1, 0, 1, 1, 2, 3, 7);
    frame(2, 1, 1, 1, 3, 1, 8);
    bytes.push_back(0x3B);
    return bytes;
}

bool WriteFixture(const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
                  const FILETIME* modified = nullptr) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size();
    if (modified) ok &= SetFileTime(file, nullptr, nullptr, modified) != 0;
    CloseHandle(file);
    return ok;
}

using Pixel = std::array<uint8_t, 4>;
constexpr Pixel red{0, 0, 255, 255}, green{0, 255, 0, 255}, blue{255, 0, 0, 255}, clear{0, 0, 0, 0};
bool PixelsEqual(const std::vector<uint8_t>& actual, const std::array<Pixel, 6>& expected) {
    if (actual.size() != 24) return false;
    for (size_t i = 0; i < expected.size(); ++i)
        for (size_t c = 0; c < 4; ++c) if (actual[i * 4 + c] != expected[i][c]) return false;
    return true;
}
}

int wmain() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(com), "initialize GIF decoder helper apartment");
    if (FAILED(com)) return 1;
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"PulseGifDecoderTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::error_code error;
    const bool created = std::filesystem::create_directories(root, error);
    Check(created && !error, "create isolated GIF decoder fixture directory");
    if (!created || error) { CoUninitialize(); return 1; }
    {
        GifDecodeState state;
        const auto path = root / L"animation.gif";
        Check(WriteFixture(path, FixtureBytes()), "write disposal 2 and 3 GIF fixture");
        std::vector<uint8_t> pixels;
        UINT w = 0, h = 0, stride = 0, sw = 0, sh = 0;
        uint32_t count = 0, delay = 0, loops = 0;
        const auto decode = [&](uint32_t frame, UINT requested_cap = 512) {
            return DecodeGifFrame(state, path.wstring(), FILE_ATTRIBUTE_NORMAL,
                ipc::ClampPreviewPixelSize(requested_cap, true), frame, pixels,
                w, h, stride, count, delay, loops, sw, sh);
        };
        Check(decode(0) && PixelsEqual(pixels, {red, red, red, red, red, red}) &&
              count == 4 && delay == 50 && w == 3 && h == 2 && sw == 3 && sh == 2,
              "first GIF frame preserves canvas dimensions and delay");
        Check(decode(1) && PixelsEqual(pixels, {blue, red, red, red, red, red}) && delay == 60,
              "forward GIF frame overlays its offset rectangle");
        Check(decode(2) && PixelsEqual(pixels, {clear, green, red, red, red, red}) && delay == 70,
              "disposal 2 clears only the previous frame rectangle");
        Check(decode(3) && PixelsEqual(pixels, {clear, red, red, red, red, blue}) && delay == 80,
              "disposal 3 restores the canvas saved before previous overlay");
        Check(state.composed_frames == 4 && state.opened_decoders == 1,
              "sequential playback composes N frames once using one decoder");
        Check(decode(3) && state.composed_frames == 4 &&
              PixelsEqual(pixels, {clear, red, red, red, red, blue}),
              "repeating current GIF frame does not recomposite its prefix");
        Check(decode(0) && PixelsEqual(pixels, {red, red, red, red, red, red}),
              "playback loop resets to the initial transparent canvas");
        Check(decode(3) && PixelsEqual(pixels, {clear, red, red, red, red, blue}) &&
              state.composed_frames == 8 && state.opened_decoders == 1,
              "forward skipped frames apply all disposal operations once");
        Check(decode(1) && PixelsEqual(pixels, {blue, red, red, red, red, red}),
              "backward seek reconstructs correct earlier pixels");
        const auto before_cap = state.opened_decoders;
        Check(decode(0, 1024) && state.opened_decoders == before_cap,
              "requests clamped to the same effective cap share GIF identity");
        Check(decode(0, 64) && state.opened_decoders == before_cap + 1,
              "different effective output cap replaces GIF state");

        FILETIME changed = state.version.ftLastWriteTime;
        ULARGE_INTEGER timestamp{};
        timestamp.LowPart = changed.dwLowDateTime; timestamp.HighPart = changed.dwHighDateTime;
        timestamp.QuadPart += 10000000;
        changed = {timestamp.LowPart, timestamp.HighPart};
        const auto before_version = state.opened_decoders;
        Check(WriteFixture(path, FixtureBytes(3, 2, 255), &changed) && decode(0, 64) &&
              state.opened_decoders == before_version + 1 && pixels.size() == 24 && pixels[1] == 255,
              "same path and file size with new actual write version invalidates cached pixels");
        const auto replacement = root / L"replacement.gif";
        const FILETIME same_time = state.version.ftLastWriteTime;
        const auto before_replace = state.opened_decoders;
        const bool written = WriteFixture(replacement, FixtureBytes(), &same_time);
        const bool replaced = written && MoveFileExW(replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        const DWORD replace_error = replaced ? ERROR_SUCCESS : GetLastError();
        const bool decoded = replaced && decode(0, 64);
        if (!written || !replaced || !decoded || state.opened_decoders != before_replace + 1 ||
            !PixelsEqual(pixels, {red, red, red, red, red, red})) {
            std::printf("[DIAG] replacement written=%d replaced=%d error=%lu decoded=%d opens=%llu expected=%llu\n",
                written, replaced, replace_error, decoded,
                static_cast<unsigned long long>(state.opened_decoders),
                static_cast<unsigned long long>(before_replace + 1));
        }
        Check(written && replaced && decoded &&
              state.opened_decoders == before_replace + 1 &&
              PixelsEqual(pixels, {red, red, red, red, red, red}),
              "same path size and timestamp with replaced file identity invalidates GIF state");
        state.Clear();
        const auto budget = root / L"canvas-budget.gif";
        Check(WriteFixture(budget, FixtureBytes(4096, 2048, 0, true)) &&
              DecodeGifFrame(state, budget.wstring(), FILE_ATTRIBUTE_NORMAL, 32, 0,
                  pixels, w, h, stride, count, delay, loops, sw, sh) &&
              state.canvas.size() == GifDecodeState::max_canvas_bytes && w == 32 && h == 16,
              "canvas at bounded memory limit still scales through production WIC path");
        state.Clear();
        Check(WriteFixture(budget, FixtureBytes(4097, 2048, 0, true)) &&
              !DecodeGifFrame(state, budget.wstring(), FILE_ATTRIBUTE_NORMAL, 32, 0,
                  pixels, w, h, stride, count, delay, loops, sw, sh) &&
              state.canvas.empty() && state.saved.empty() && !state.decoder,
              "oversized original canvas fails before retained canvas allocation");
        Check(WriteFixture(budget, FixtureBytes()), "write encoded size budget fixture");
        std::filesystem::resize_file(budget, GifDecodeState::max_encoded_bytes + 1, error);
        Check(!error && !DecodeGifFrame(state, budget.wstring(), FILE_ATTRIBUTE_NORMAL, 32, 0,
                  pixels, w, h, stride, count, delay, loops, sw, sh) &&
              state.encoded.empty() && state.canvas.empty() && !state.decoder,
              "oversized encoded input fails before retained decoder allocation");
    }
    std::filesystem::remove_all(root, error);
    Check(!error && !std::filesystem::exists(root), "remove isolated GIF fixture directory");
    CoUninitialize();
    return failures ? 1 : 0;
}
