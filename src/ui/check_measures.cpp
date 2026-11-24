#include "gpu_view.h"
#include <cstdio>
#include "decoders/decoder.h"
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <rapport/gpu_d3d11.h>
#endif

// --d3d-debug is deliberately strict: absence of the debug layer is a
// failed prerequisite, never a successful check with diagnostics skipped.
int check_measures(int argc, char** argv)
{
    bool debug = false, detail = false;
    std::vector<std::string> files;
    for (int i = 0; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--d3d-debug") == 0)
            debug = true;
        else if (std::strcmp(argv[i], "--detail") == 0)
            detail = true;
        else if (argv[i][0] == '-')
        {
            std::fprintf(stderr, "check-measures: unknown option %s\n", argv[i]);
            return 2;
        }
        else
            files.emplace_back(argv[i]);
    }
    if (detail && files.empty())
    {
        std::fprintf(stderr, "check-measures: --detail requires at least one image\n");
        return 2;
    }
    DecodeOptions options;
    options.gpu_develop = options.gpu_decode = true;
    set_decode_options(options);
    std::shared_ptr<rap::gpu::device> device;
#ifdef _WIN32
    Microsoft::WRL::ComPtr<ID3D11Device> native;
    Microsoft::WRL::ComPtr<ID3D11InfoQueue> messages;
    if (debug)
    {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,
                                     nullptr, 0, D3D11_SDK_VERSION, &native, nullptr, nullptr);
        if (FAILED(hr) || FAILED(native.As(&messages)))
        {
            std::fprintf(stderr, "check-measures: D3D11 debug layer required (HRESULT %08lx)\n", hr);
            return 1;
        }
        device = rap::gpu::d3d11::wrap(native.Get(), nullptr);
        if (!device)
        {
            std::fprintf(stderr, "check-measures: cannot wrap the D3D11 debug device\n");
            return 1;
        }
        messages->ClearStoredMessages();
    }
#else
    if (debug)
    {
        std::fprintf(stderr, "check-measures: --d3d-debug requires Windows\n");
        return 1;
    }
#endif
    if (!device)
        device = rap::gpu::device::headless();
    if (!device)
    {
        std::fprintf(stderr, "SKIPPED check-measures: no headless GPU\n");
        return 77;
    }
    GpuView view(device);
    int failed = view.check_measures(files, detail);
#ifdef _WIN32
    if (messages)
    {
        unsigned errors = 0;
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            messages->GetMessage(i, nullptr, &size);
            std::vector<std::byte> storage(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (FAILED(messages->GetMessage(i, message, &size)))
                return 1;
            // Binding hazards may be reported as warnings (for example an
            // overlapping SRV/UAV automatically nulled by the runtime).
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
            {
                ++errors;
                std::fprintf(stderr, "D3D11 diagnostic %u: %s\n", unsigned(message->ID), message->pDescription);
            }
        }
        std::printf("%s D3D11 debug layer: %u warnings/errors\n", errors ? "FAILED" : "ok    ", errors);
        failed += errors != 0;
    }
#endif
    return failed ? 1 : 0;
}
