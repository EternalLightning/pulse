// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/frame_profile.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace pulse::bench {

class GpuTimestampProfile {
public:
    static bool Requested() {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"PULSE_PROFILE_GPU_TIMESTAMPS", value, 2) == 1 && value[0] == L'1';
    }
    HRESULT Init(ID2D1DeviceContext* dc) {
        Microsoft::WRL::ComPtr<ID2D1Image> image;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        dc->GetTarget(&image);
        HRESULT hr = image ? image.As(&target) : E_POINTER;
        if (SUCCEEDED(hr)) hr = target->GetSurface(&surface);
        if (SUCCEEDED(hr)) hr = surface->GetDevice(IID_PPV_ARGS(&device_));
        if (FAILED(hr)) return hr;
        device_->GetImmediateContext(&context_);
        for (auto& slot : slots_) {
            D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(hr = device_->CreateQuery(&desc, &slot.disjoint))) return hr;
            desc.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(hr = device_->CreateQuery(&desc, &slot.begin))) return hr;
            if (FAILED(hr = device_->CreateQuery(&desc, &slot.end))) return hr;
        }
        rows_.reserve(16000);
        usable_ = true;
        std::printf("[GPU_TIMESTAMP] enabled=1 slots=%zu metric=gpu_timestamp_elapsed_ms "
            "range=before-BeginDraw-to-after-EndDraw excludes=Present "
            "limit=may-include-command-supply-gaps-and-scheduling-not-busy-or-display-timing\n", slots_.size());
        return S_OK;
    }
    app::FrameDrawProfileHooks Hooks() {
        return {this, [](void* self) noexcept { static_cast<GpuTimestampProfile*>(self)->Begin(); },
            [](void* self, int32_t result) noexcept { static_cast<GpuTimestampProfile*>(self)->End(result); }};
    }
    void SetFrame(int scenario, int repeat, int frame, bool measurement) {
        metadata_ = {scenario, repeat, frame, measurement};
        Poll();
    }
    void Print(const char* const* scenarios, size_t scenario_count) {
        Poll(); // Finite final poll: never flush or wait for the last frames.
        std::printf("GPU_CSV,id,scenario,repeat,frame,measurement,status,gpu_timestamp_elapsed_ms,"
            "begin_ticks,end_ticks,frequency_hz,disjoint,hresult\n");
        for (const auto& row : rows_) {
            std::printf("GPU_CSV,%llu,%s,%d,%d,%d,%s,", static_cast<unsigned long long>(row.id),
                scenarios[row.metadata.scenario], row.metadata.repeat, row.metadata.frame,
                row.metadata.measurement, Name(row.status));
            if (row.status == Status::Valid) std::printf("%.6f", row.elapsed_ms);
            std::printf(",%llu,%llu,%llu,%d,%08lx\n", static_cast<unsigned long long>(row.begin),
                static_cast<unsigned long long>(row.end), static_cast<unsigned long long>(row.frequency),
                row.disjoint, static_cast<unsigned long>(row.result));
        }
        for (size_t scenario = 0; scenario < scenario_count; ++scenario) {
            std::vector<double> values;
            size_t invalid = 0, pending = 0, dropped = 0;
            for (const auto& row : rows_) {
                if (!row.metadata.measurement || row.metadata.scenario != static_cast<int>(scenario)) continue;
                if (row.status == Status::Valid) values.push_back(row.elapsed_ms);
                else if (row.status == Status::Invalid) ++invalid;
                else if (row.status == Status::Pending) ++pending;
                else ++dropped;
            }
            std::sort(values.begin(), values.end());
            std::printf("[GPU_PROFILE] scenario=%s metric=gpu_timestamp_elapsed_ms valid=%zu invalid=%zu "
                "pending=%zu dropped=%zu", scenarios[scenario], values.size(), invalid, pending, dropped);
            if (!values.empty()) std::printf(" p50=%.6f p95=%.6f p99=%.6f max=%.6f",
                values[(values.size() * 50 - 1) / 100], values[(values.size() * 95 - 1) / 100],
                values[(values.size() * 99 - 1) / 100], values.back());
            std::printf("\n");
        }
    }
private:
    enum class Status { Pending, Valid, Invalid, Dropped };
    struct Metadata { int scenario = 0, repeat = 0, frame = 0; bool measurement = false; };
    struct Row {
        uint64_t id;
        Metadata metadata;
        Status status = Status::Pending;
        double elapsed_ms = 0;
        UINT64 begin = 0, end = 0, frequency = 0;
        bool disjoint = false;
        HRESULT result = S_FALSE;
    };
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end;
        size_t row = 0;
        bool pending = false;
    };
    static const char* Name(Status status) {
        switch (status) {
        case Status::Valid: return "valid";
        case Status::Invalid: return "invalid";
        case Status::Dropped: return "dropped";
        default: return "pending";
        }
    }
    void Disable(HRESULT result) {
        usable_ = false;
        failure_ = result;
        for (auto& slot : slots_) if (slot.pending) {
            auto& row = rows_[slot.row];
            row.status = Status::Invalid;
            row.result = result;
            slot.pending = false;
        }
        active_ = -1;
    }
    void Poll() {
        if (!usable_) return;
        for (auto& slot : slots_) {
            if (!slot.pending) continue;
            auto& row = rows_[slot.row];
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
            constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            const HRESULT disjoint = context_->GetData(slot.disjoint.Get(), &data, sizeof(data), flags);
            const HRESULT begin = context_->GetData(slot.begin.Get(), &row.begin, sizeof(row.begin), flags);
            const HRESULT end = context_->GetData(slot.end.Get(), &row.end, sizeof(row.end), flags);
            if (FAILED(disjoint) || FAILED(begin) || FAILED(end)) {
                Disable(FAILED(disjoint) ? disjoint : FAILED(begin) ? begin : end);
                return;
            }
            if (disjoint != S_OK || begin != S_OK || end != S_OK) continue;
            row.frequency = data.Frequency;
            row.disjoint = data.Disjoint != FALSE;
            row.result = S_OK;
            if (row.disjoint || !row.frequency || row.end < row.begin) row.status = Status::Invalid;
            else {
                row.status = Status::Valid;
                row.elapsed_ms = static_cast<double>(row.end - row.begin) * 1000.0 / static_cast<double>(row.frequency);
            }
            slot.pending = false;
        }
    }
    void Begin() {
        active_ = -1;
        rows_.push_back({next_id_++, metadata_});
        auto& row = rows_.back();
        if (!usable_) { row.status = Status::Invalid; row.result = failure_; return; }
        for (size_t i = 0; i < slots_.size(); ++i) {
            auto& slot = slots_[i];
            if (slot.pending) continue;
            slot.row = rows_.size() - 1;
            slot.pending = true;
            active_ = static_cast<int>(i);
            context_->Begin(slot.disjoint.Get());
            context_->End(slot.begin.Get());
            return;
        }
        row.status = Status::Dropped;
    }
    void End(int32_t result) {
        if (active_ < 0) return;
        auto& slot = slots_[static_cast<size_t>(active_)];
        context_->End(slot.end.Get());
        context_->End(slot.disjoint.Get());
        active_ = -1;
        if (FAILED(result)) Disable(result);
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::array<Slot, 8> slots_;
    std::vector<Row> rows_;
    Metadata metadata_;
    uint64_t next_id_ = 0;
    int active_ = -1;
    bool usable_ = false;
    HRESULT failure_ = E_FAIL;
};

} // namespace pulse::bench
