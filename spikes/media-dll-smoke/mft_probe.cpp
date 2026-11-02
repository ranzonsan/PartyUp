// PartyUp 媒体核心 · 硬件 H.264 MFT 可用性探针
//
// 只是探测，不参与生产构建。回答四个问题：
//   1. 本机 MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, NV12→H264) 到底枚举出什么？
//      硬件（MFT_ENUM_FLAG_HARDWARE）和软件各几个、友好名是什么、CLSID 是什么？
//   2. 这些 MFT 是不是 D3D11-aware（MF_SA_D3D11_AWARE），异步（MF_TRANSFORM_ASYNC），
//      支持动态格式变更（MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE）？
//   3. 把 MFCreateDXGIDeviceManager 造出来的 D3D11 device manager 用
//      MFT_MESSAGE_SET_D3D_MANAGER 塞给编码器 MFT，HRESULT 是多少？
//      （0x80004001 = E_NOTIMPL 说明这个 MFT 不吃 D3D11，只能吃系统内存 NV12）
//   4. 解码侧：H264→NV12 有没有硬件 MFT（观看方要用它解 H.264）？
//
// 编译（见 probe-hardware-mft.ps1）：
//   cl /nologo /MT /std:c++20 /EHsc /O2 /utf-8 mft_probe.cpp /link \
//      mfplat.lib mfuuid.lib ole32.lib d3d11.lib dxgi.lib
#include <windows.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::string GuidToString(REFGUID guid) {
  wchar_t buffer[64] = {};
  StringFromGUID2(guid, buffer, 64);
  char utf8[128] = {};
  WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
  return utf8;
}

std::string FriendlyNameOf(IMFActivate* activate) {
  LPWSTR wide = nullptr;
  UINT32 len = 0;
  if (FAILED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wide, &len)) ||
      wide == nullptr) {
    return "<no name>";
  }
  char utf8[512] = {};
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr);
  CoTaskMemFree(wide);
  return utf8;
}

std::string HardwareUrlOf(IMFActivate* activate) {
  LPWSTR wide = nullptr;
  UINT32 len = 0;
  if (FAILED(activate->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute, &wide, &len)) ||
      wide == nullptr) {
    return "";
  }
  char utf8[512] = {};
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr);
  CoTaskMemFree(wide);
  return utf8;
}

const char* YesNo(bool v) { return v ? "yes" : "no"; }
const char* HrText(HRESULT hr) {
  switch (hr) {
    case S_OK: return "S_OK";
    case E_NOTIMPL: return "E_NOTIMPL";
    case MF_E_INVALIDMEDIATYPE: return "MF_E_INVALIDMEDIATYPE";
    case MF_E_UNSUPPORTED_D3D_TYPE: return "MF_E_UNSUPPORTED_D3D_TYPE";
    case MF_E_TOPO_CODEC_NOT_FOUND: return "MF_E_TOPO_CODEC_NOT_FOUND";
    case E_FAIL: return "E_FAIL";
    case E_INVALIDARG: return "E_INVALIDARG";
    default: return "";
  }
}

// 打印一组 MFT 的属性。flags 是 MFT_ENUM_FLAG_* 的位组合（MFTEnumEx 收 UINT32，
// SDK 没有 MFT_ENUM_FLAG 这个 typedef，只有 enum _MFT_ENUM_FLAG）。
void DumpMfts(REFGUID category, REFGUID input_type, REFGUID output_type,
              const char* type_label, UINT32 flags) {
  MFT_REGISTER_TYPE_INFO in_info = {MFMediaType_Video, input_type};
  MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, output_type};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const HRESULT hr = MFTEnumEx(category, flags, &in_info, &out_info, &activates, &count);
  std::printf("\n--- %s %s flags=0x%08X hr=0x%08X count=%u\n", type_label,
              IsEqualGUID(category, MFT_CATEGORY_VIDEO_ENCODER) ? "encoder" : "decoder",
              flags, static_cast<unsigned>(hr), count);
  if (FAILED(hr)) return;
  for (UINT32 i = 0; i < count; ++i) {
    IMFActivate* a = activates[i];
    UINT32 async = 0;
    UINT32 d3d11_aware = 0;
    UINT32 dynamic_format = 0;
    UINT32 low_latency = 0;
    if (SUCCEEDED(a->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async != 0) {
      a->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, 1);
    }
    a->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware);
    a->GetUINT32(MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE, &dynamic_format);
    a->GetUINT32(MF_LOW_LATENCY, &low_latency);
    GUID clsid = {};
    a->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
    const std::string name = FriendlyNameOf(a);
    IMFTransform* mft = nullptr;
    if (FAILED(a->ActivateObject(__uuidof(IMFTransform), (void**)&mft)) || mft == nullptr) {
      std::printf("  [%u] %s  ACTIVATE FAILED\n", i, name.c_str());
      continue;
    }
    DWORD input_count = 0;
    DWORD output_count = 0;
    mft->GetStreamCount(&input_count, &output_count);
    std::printf(
        "  [%u] %s\n      clsid=%s\n      async=%s d3d11_aware=%u dynfmt=%s lowlat=%u "
        "streams=%u/%u hwurl=%s\n",
        i, name.c_str(), GuidToString(clsid).c_str(), YesNo(async != 0), d3d11_aware,
        YesNo(dynamic_format != 0), low_latency, static_cast<unsigned>(input_count),
        static_cast<unsigned>(output_count), HardwareUrlOf(a).c_str());
    mft->Release();
  }
  // IMFActivate 的所有权归调用方：循环里不要逐个 Release，否则这里就双重释放了
  // （第一版就是这么崩的：0xC0000409 STATUS_STACK_BUFFER_OVERRUN）。
  for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
  CoTaskMemFree(activates);
}

}  // namespace

int main() {
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::printf("CoInitializeEx hr=0x%08X\n", static_cast<unsigned>(hr));
  MFStartup(MF_VERSION);
  std::printf("MFStartup done\n");

  std::printf("\n================ GPU / hardware info ================\n");
  {
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
      ComPtr<IDXGIAdapter1> adapter;
      for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr,
                            nullptr);
        std::printf("adapter[%u] %s  vendor=0x%04X device=0x%04X vram=%.0f MB\n", i, name,
                    desc.VendorId, desc.DeviceId,
                    static_cast<double>(desc.DedicatedVideoMemory) / (1024.0 * 1024.0));
        adapter.Reset();
      }
    }
  }

  std::printf("\n================ encoders NV12 -> H264 ================\n");
  DumpMfts(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_NV12, MFVideoFormat_H264,
           "HW+SW", MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER |
                        MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SYNCMFT);
  DumpMfts(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_NV12, MFVideoFormat_H264, "ALL",
           MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER);

  std::printf("\n================ decoders H264 -> NV12 ================\n");
  DumpMfts(MFT_CATEGORY_VIDEO_DECODER, MFVideoFormat_H264, MFVideoFormat_NV12, "HW+SW",
           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER |
               MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SYNCMFT);
  DumpMfts(MFT_CATEGORY_VIDEO_DECODER, MFVideoFormat_H264, MFVideoFormat_NV12, "ALL",
           MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER);

  std::printf("\n=== D3D11 device manager handshake ===\n");
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
  HRESULT device_hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
      ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, &chosen, &context);
  std::printf("D3D11CreateDevice(hw) hr=0x%08X feature_level=0x%X\n",
              static_cast<unsigned>(device_hr), static_cast<unsigned>(chosen));

  UINT32 reset_token = 0;
  ComPtr<IMFDXGIDeviceManager> device_manager;
  const HRESULT dm_hr = MFCreateDXGIDeviceManager(&reset_token, &device_manager);
  std::printf("MFCreateDXGIDeviceManager hr=0x%08X token=%u\n",
              static_cast<unsigned>(dm_hr), reset_token);
  if (SUCCEEDED(dm_hr) && device != nullptr) {
    const HRESULT reset_hr = device_manager->ResetDevice(device.Get(), reset_token);
    std::printf("IMFDXGIDeviceManager::ResetDevice hr=0x%08X (%s)\n",
                static_cast<unsigned>(reset_hr), HrText(reset_hr));
    MFT_REGISTER_TYPE_INFO in_info = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT enum_hr =
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                  MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in_info,
                  &out_info, &activates, &count);
    std::printf("enum hardware encoders via HW flag hr=0x%08X count=%u\n",
                static_cast<unsigned>(enum_hr), count);
    if (SUCCEEDED(enum_hr)) {
      for (UINT32 i = 0; i < count; ++i) {
        IMFActivate* a = activates[i];
        UINT32 async = 0;
        if (SUCCEEDED(a->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async != 0) {
          a->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, 1);
        }
        IMFTransform* mft = nullptr;
        const HRESULT activate_hr = a->ActivateObject(__uuidof(IMFTransform), (void**)&mft);
        if (FAILED(activate_hr) || mft == nullptr) {
          std::printf("  [%u] %s activate hr=0x%08X\n", i, FriendlyNameOf(a).c_str(),
                      static_cast<unsigned>(activate_hr));
          continue;
        }
        const HRESULT set_hr = mft->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<UINT_PTR>(device_manager.Get()));
        std::printf("  [%u] %s  MFT_MESSAGE_SET_D3D_MANAGER hr=0x%08X %s\n", i,
                    FriendlyNameOf(a).c_str(), static_cast<unsigned>(set_hr),
                    HrText(set_hr));
        mft->Release();
      }
      for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
      CoTaskMemFree(activates);
    }
  }

  MFShutdown();
  CoUninitialize();
  std::printf("\nprobe done\n");
  return 0;
}
