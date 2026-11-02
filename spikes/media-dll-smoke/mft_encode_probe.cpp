// PartyUp 媒体核心 · 硬件 H.264 MFT 编码可行性探针
//
// 目的：在把任何东西接进 partyup_media.dll 之前，先用独立进程回答：
//   1. NVIDIA H.264 Encoder MFT 的 MF_TRANSFORM_ASYNC_UNLOCK 到底要设在哪
//      （IMFActivate 的属性store 不够，必须是 IMFTransform::GetAttributes()），
//      解锁后 MFT_MESSAGE_SET_D3D_MANAGER 是否 S_OK；
//   2. 以系统内存 NV12 喂它，2560x1440 每帧多少毫秒、关键帧间隔、码率什么样
//      （这是「CPU 能不能降下来」的硬证据）；
//   3. 以 D3D11 纹理（DXGI_FORMAT_NV12）喂它，每帧多少毫秒（零拷贝路径的前提）；
//   4. 编出来的字节流用 Microsoft H264 Video Decoder MFT 解回来，尺寸/帧数对得上
//      就说明不是「编出一堆垃圾」。
//
// 这个探针里的编码循环结构与生产实现（hw_encoder.cpp）一致，验证通过后直接搬。
//
// 编译（见 probe-hardware-mft.ps1 -Encode）：
//   cl /nologo /MT /std:c++20 /EHsc /O2 /utf-8 mft_encode_probe.cpp /link \
//      mfplat.lib mfuuid.lib ole32.lib d3d11.lib dxgi.lib
#include <windows.h>

#include <codecapi.h>
#include <d3d11.h>
#include <icodecapi.h>  // ICodecAPI（codecapi.h 只有 CODECAPI_* 的 GUID，没有接口本身）
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT32 kWidth = 2560;
constexpr UINT32 kHeight = 1440;
constexpr UINT32 kFps = 60;
constexpr UINT32 kBitrate = 12'000'000;
constexpr UINT32 kFrameCount = 60;

// 自己的超时哨兵值：Win32 SDK 里没有 E_TIMEOUT，用 HRESULT_FROM_WIN32 造一个。
constexpr HRESULT kWaitTimeout = HRESULT_FROM_WIN32(ERROR_TIMEOUT);

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 本进程累计 CPU 时间（内核 + 用户），单位秒。用来算「每帧花了多少 CPU」——
// 这是「硬件编码到底省不省 CPU」的唯一硬指标（墙钟里的等待不算 CPU）。
double ProcessCpuSeconds() {
  FILETIME creation = {}, exit_time = {}, kernel = {}, user = {};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) {
    return 0.0;
  }
  ULARGE_INTEGER k, u;
  k.LowPart = kernel.dwLowDateTime;
  k.HighPart = kernel.dwHighDateTime;
  u.LowPart = user.dwLowDateTime;
  u.HighPart = user.dwHighDateTime;
  return static_cast<double>(k.QuadPart + u.QuadPart) / 10'000'000.0;
}

const char* HrName(HRESULT hr) {
  switch (hr) {
    case S_OK: return "S_OK";
    case S_FALSE: return "S_FALSE";
    case E_FAIL: return "E_FAIL";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_NOTIMPL: return "E_NOTIMPL";
    case E_NOINTERFACE: return "E_NOINTERFACE";
    case E_ACCESSDENIED: return "E_ACCESSDENIED";
    case MF_E_TRANSFORM_ASYNC_LOCKED: return "MF_E_TRANSFORM_ASYNC_LOCKED";
    case MF_E_TRANSFORM_NEED_MORE_INPUT: return "MF_E_TRANSFORM_NEED_MORE_INPUT";
    case MF_E_TRANSFORM_STREAM_CHANGE: return "MF_E_TRANSFORM_STREAM_CHANGE";
    case MF_E_NOTACCEPTING: return "MF_E_NOTACCEPTING";
    case MF_E_INVALIDMEDIATYPE: return "MF_E_INVALIDMEDIATYPE";
    case MF_E_ATTRIBUTENOTFOUND: return "MF_E_ATTRIBUTENOTFOUND";
    case MF_E_UNSUPPORTED_D3D_TYPE: return "MF_E_UNSUPPORTED_D3D_TYPE";
    case MF_E_TRANSFORM_ASYNC_MFT_NOT_SUPPORTED:
      return "MF_E_TRANSFORM_ASYNC_MFT_NOT_SUPPORTED";
    case MF_E_NO_EVENTS_AVAILABLE: return "MF_E_NO_EVENTS_AVAILABLE";
    default: return "?";
  }
}

void PrintHr(const char* what, HRESULT hr) {
  std::printf("    %-52s hr=0x%08X %s\n", what, static_cast<unsigned>(hr), HrName(hr));
  std::fflush(stdout);
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

// 造一个 2560x1440 的 NV12 帧。第一版只让一个 200x200 的方块水平移动，结果
// NVIDIA MFT 从第 9 帧起输出 ~96 字节的「无变化」帧 —— 分不清是编码器没吃到帧
// 还是画面真的没变。现在整帧都在变（棋盘 + 每帧不同的行渐变相位），并用 checksum
// 证明输入逐帧不同；如果输出还是几十字节，那就是驱动方式的问题而不是画面问题。
void FillNv12(uint8_t* data, UINT32 width, UINT32 height, UINT32 stride, UINT32 frame) {
  for (UINT32 y = 0; y < height; ++y) {
    uint8_t* row = data + static_cast<size_t>(y) * stride;
    const int phase = static_cast<int>((y + frame * 37) & 0xFF);
    for (UINT32 x = 0; x < width; ++x) {
      const bool checker = (((x + frame * 16) / 32) + (y / 32)) & 1;
      int value;
      if (y < height / 2) {
        value = (static_cast<int>(x * 255 / width) + phase) & 0xFF;
      } else {
        value = checker ? 230 : 20;
      }
      const UINT32 box_x = (frame * 64) % (width - 200);
      const UINT32 box_y = (frame * 8) % (height - 200);
      if (x >= box_x && x < box_x + 200 && y >= box_y && y < box_y + 200) value = 255 - value;
      row[x] = static_cast<uint8_t>(value);
    }
  }
  uint8_t* uv = data + static_cast<size_t>(stride) * height;
  for (UINT32 y = 0; y < height / 2; ++y) {
    uint8_t* row = uv + static_cast<size_t>(y) * stride;
    for (UINT32 x = 0; x < width; x += 2) {
      row[x] = static_cast<uint8_t>(64 + ((frame * 3 + y) & 0x7F));       // U
      row[x + 1] = static_cast<uint8_t>(192 - ((frame * 5 + y) & 0x7F));  // V
    }
  }
}

// 输入帧的 32 位校验和，用来证明「每帧内容确实不同」。
uint32_t Checksum(const uint8_t* data, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; i += 4096) {
    hash = (hash ^ data[i]) * 16777619u;
  }
  return hash;
}

// H.264 编码器的输出/输入类型协商。真实实现里 bitrate/fps 由 SetRates 驱动。
bool ConfigureEncoderTypes(IMFTransform* mft, UINT32 width, UINT32 height, UINT32 fps,
                           UINT32 bitrate, std::string* error) {
  ComPtr<IMFMediaType> output;
  HRESULT hr = MFCreateMediaType(&output);
  if (FAILED(hr)) { *error = "MFCreateMediaType(out)"; return false; }
  output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height);
  MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, fps, 1);
  MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
  output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  // Baseline 是 WebRTC H.264 的公共下限；packetization-mode=1 由 SDP 侧声明。
  output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
  output->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, fps);
  // 低延迟：不允许 B 帧、不缓冲。NVIDIA MFT 认这个属性。
  output->SetUINT32(MF_LOW_LATENCY, TRUE);
  output->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH264VLevel5_1);
  hr = mft->SetOutputType(0, output.Get(), 0);
  PrintHr("SetOutputType(H264)", hr);
  if (FAILED(hr)) {
    // 有的 MFT 不接受 LEVEL/PROFILE 的组合，退一步只给最小集。
    ComPtr<IMFMediaType> minimal;
    if (SUCCEEDED(MFCreateMediaType(&minimal))) {
      minimal->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      minimal->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
      MFSetAttributeSize(minimal.Get(), MF_MT_FRAME_SIZE, width, height);
      MFSetAttributeRatio(minimal.Get(), MF_MT_FRAME_RATE, fps, 1);
      minimal->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
      const HRESULT retry = mft->SetOutputType(0, minimal.Get(), 0);
      PrintHr("SetOutputType(H264, minimal)", retry);
      if (FAILED(retry)) { *error = "SetOutputType(H264) failed"; return false; }
    } else {
      *error = "SetOutputType(H264) failed";
      return false;
    }
  }

  MFT_OUTPUT_STREAM_INFO stream_info = {};
  mft->GetOutputStreamInfo(0, &stream_info);
  std::printf("    output stream: flags=0x%08X cbSize=%u\n", stream_info.dwFlags,
              stream_info.cbSize);

  ComPtr<IMFMediaType> input;
  hr = MFCreateMediaType(&input);
  if (FAILED(hr)) { *error = "MFCreateMediaType(in)"; return false; }
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height);
  MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1);
  MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  input->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
  hr = mft->SetInputType(0, input.Get(), 0);
  PrintHr("SetInputType(NV12)", hr);
  if (FAILED(hr)) { *error = "SetInputType(NV12) failed"; return false; }
  return true;
}

bool ForceKeyFrame(IMFTransform* mft) {
  ComPtr<ICodecAPI> codec_api;
  if (FAILED(mft->QueryInterface(IID_PPV_ARGS(&codec_api)))) return false;
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_UI4;
  value.ulVal = 1;
  const HRESULT hr = codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &value);
  VariantClear(&value);
  if (FAILED(hr)) {
    std::printf("    (force keyframe unsupported: 0x%08X)\n", static_cast<unsigned>(hr));
    return false;
  }
  return true;
}

// 等一个指定类型的 MFT 事件；把途中别的（NeedInput/HaveOutput）吃掉。
// async MFT 没有超时版的 GetEvent，这里用 NO_WAIT + 1 ms 轮询实现超时。
HRESULT WaitForEvent(IMFMediaEventGenerator* events, MediaEventType want,
                     DWORD timeout_ms) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    ComPtr<IMFMediaEvent> event;
    HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) {
      if (GetTickCount64() >= deadline) return kWaitTimeout;
      Sleep(1);
      continue;
    }
    if (FAILED(hr)) return hr;
    MediaEventType type = MEUnknown;
    event->GetType(&type);
    if (type == want) return S_OK;
  }
}

// 一个输出包（own memory 或 DXGI surface 都可能，用 IMFSample 统一接住）。
struct EncodedPacket {
  std::vector<uint8_t> bytes;
  bool keyframe = false;
  int64_t time_us = 0;
};

struct EncodeStats {
  uint32_t frames_in = 0;
  uint32_t frames_out = 0;
  uint32_t keyframes = 0;
  uint64_t bytes_out = 0;
  double avg_ms = 0.0;
  double max_ms = 0.0;
  double total_ms = 0.0;
};

// 取出 IMFSample 的连续字节（若是 DXGI 2D 缓冲就 Map 出来）。
// max_len / cur_len 都记日志：硬件 MFT 的输出包有时比 Lock 报的 cur_len 短，
// 这两种长度如果差很多，说明读法有问题，不是编码器编得小。
bool SampleToBytes(IMFSample* sample, std::vector<uint8_t>* out, std::string* error) {
  DWORD count = 0;
  sample->GetBufferCount(&count);
  if (count == 0) { *error = "output sample has no buffers"; return false; }
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(i, &buffer))) { *error = "GetBufferByIndex"; return false; }
    DWORD declared_len = 0;
    buffer->GetCurrentLength(&declared_len);
    BYTE* data = nullptr;
    DWORD max_len = 0;
    DWORD locked_len = 0;
    if (FAILED(buffer->Lock(&data, &max_len, &locked_len))) {
      *error = "IMFMediaBuffer::Lock failed"; return false;
    }
    const DWORD take = declared_len != 0 ? declared_len : locked_len;
    out->insert(out->end(), data, data + take);
    buffer->Unlock();
    std::printf("      buffer[%u]: declared=%u locked=%u max=%u\n", i, declared_len,
                locked_len, max_len);
  }
  return true;
}

// 等下一个 NeedInput / HaveOutput 事件（别的类型吃掉，直到超时）。
HRESULT WaitForAnyTransformEvent(IMFMediaEventGenerator* events, DWORD timeout_ms,
                                 MediaEventType* out_type) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    ComPtr<IMFMediaEvent> event;
    const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) {
      if (GetTickCount64() >= deadline) return kWaitTimeout;
      Sleep(1);
      continue;
    }
    if (FAILED(hr)) return hr;
    MediaEventType type = MEUnknown;
    event->GetType(&type);
    if (type == METransformNeedInput || type == METransformHaveOutput ||
        type == METransformDrainComplete || type == METransformMarker) {
      *out_type = type;
      return S_OK;
    }
  }
}

// 收**一个**输出包。NVIDIA 的硬件 MFT 是 async、1 进 1 出的：在它还没收到下一帧
// 输入之前再调 ProcessOutput 会回 E_UNEXPECTED（0x8000FFFF），所以这里每次只收一个。
// 返回 true=调用成功（got 表示是否真拿到包）；false=硬失败，原因写 error。
bool DrainOneOutput(IMFTransform* mft, DWORD output_buffer_size,
                    std::vector<EncodedPacket>* packets, std::string* error, bool* got) {
  *got = false;
  MFT_OUTPUT_DATA_BUFFER out_buffer = {};
  out_buffer.dwStreamID = 0;
  out_buffer.pSample = nullptr;
  MFT_OUTPUT_STREAM_INFO info = {};
  mft->GetOutputStreamInfo(0, &info);
  ComPtr<IMFSample> allocated;
  if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
    if (output_buffer_size == 0) {
      *error = "output stream needs samples but cbSize=0";
      return false;
    }
    if (FAILED(MFCreateSample(&allocated))) {
      *error = "MFCreateSample(out)";
      return false;
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(output_buffer_size, &buffer))) {
      *error = "MFCreateMemoryBuffer(out)";
      return false;
    }
    allocated->AddBuffer(buffer.Get());
    out_buffer.pSample = allocated.Get();
  }
  DWORD status = 0;
  HRESULT hr = mft->ProcessOutput(0, 1, &out_buffer, &status);
  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
    return true;
  }
  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    std::printf("    (MF_E_TRANSFORM_STREAM_CHANGE on output, renegotiating)\n");
    ComPtr<IMFMediaType> available;
    const HRESULT type_hr = mft->GetOutputAvailableType(0, 0, &available);
    PrintHr("GetOutputAvailableType(0)", type_hr);
    if (SUCCEEDED(type_hr)) {
      UINT32 w = 0, h = 0;
      MFGetAttributeSize(available.Get(), MF_MT_FRAME_SIZE, &w, &h);
      std::printf("    new output type: %ux%u\n", w, h);
      PrintHr("SetOutputType(new)", mft->SetOutputType(0, available.Get(), 0));
    }
    if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
    return true;
  }
  if (FAILED(hr)) {
    PrintHr("ProcessOutput", hr);
    if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
    *error = std::string("ProcessOutput failed: ") + HrName(hr);
    return false;
  }
  IMFSample* delivered = out_buffer.pSample;
  if (delivered != nullptr) {
    EncodedPacket packet;
    UINT32 clean_point = 0;
    if (SUCCEEDED(delivered->GetUINT32(MFSampleExtension_CleanPoint, &clean_point))) {
      packet.keyframe = clean_point != 0;
    }
    LONGLONG sample_time = 0;
    if (SUCCEEDED(delivered->GetSampleTime(&sample_time))) packet.time_us = sample_time / 10;
    if (!SampleToBytes(delivered, &packet.bytes, error)) {
      if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
      return false;
    }
    packets->push_back(std::move(packet));
    *got = true;
  }
  // MFT 提供的输出样本所有权归我们，必须自己释放；我们分配的那个由 ComPtr 管。
  if (delivered != nullptr && delivered != allocated.Get()) delivered->Release();
  if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
  return true;
}

// 驱动一次编码会话（1 进 1 出，锁步）。texture_input==nullptr 时用系统内存 NV12。
bool RunEncodeSession(IMFTransform* mft, bool async, UINT32 width, UINT32 height,
                      UINT32 fps, uint32_t frame_count, ID3D11Device* device,
                      ID3D11DeviceContext* context, bool use_texture,
                      std::vector<EncodedPacket>* packets, EncodeStats* stats,
                      std::string* error) {
  MFT_OUTPUT_STREAM_INFO out_info = {};
  mft->GetOutputStreamInfo(0, &out_info);
  const DWORD buffer_size = out_info.cbSize;
  const UINT32 stride = width;
  const UINT32 nv12_size = stride * height + stride * (height / 2);

  // 系统内存输入样本（每帧一个，探针里不复用，避免隐藏的数据竞争）。
  std::vector<uint8_t> nv12(nv12_size);

  // D3D11 纹理 + staging（只有 texture 模式用）
  ComPtr<ID3D11Texture2D> nv12_texture;
  ComPtr<ID3D11Texture2D> upload_texture;
  if (use_texture && device != nullptr) {
    UINT format_support = 0;
    const HRESULT support_hr =
        device->CheckFormatSupport(DXGI_FORMAT_NV12, &format_support);
    std::printf("CheckFormatSupport(NV12) hr=0x%08X support=0x%08X%s%s%s%s\n",
                static_cast<unsigned>(support_hr), format_support,
                (format_support & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? " TEXTURE2D" : "",
                (format_support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? " RENDER_TARGET" : "",
                (format_support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ? " SHADER_SAMPLE" : "",
                (format_support & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT) ? " VP_OUTPUT" : "");

    // NV12 的 BindFlags 各驱动要求不一样：先给最小集，再按需要补（RENDER_TARGET 是
    // VideoProcessorBlt / 上传拷贝需要的；DECODER 只有解码器才需要，放上去反而 E_INVALIDARG）。
    const UINT bind_candidates[] = {
        D3D11_BIND_RENDER_TARGET,
        0,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    };
    HRESULT hr = E_FAIL;
    for (UINT bind : bind_candidates) {
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = width;
      desc.Height = height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_NV12;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = bind;
      hr = device->CreateTexture2D(&desc, nullptr, &nv12_texture);
      std::printf("CreateTexture2D(NV12 default) bind=0x%08X -> hr=0x%08X %s\n", bind,
                  static_cast<unsigned>(hr), HrName(hr));
      if (SUCCEEDED(hr)) break;
      nv12_texture.Reset();
    }
    if (FAILED(hr)) return false;
    D3D11_TEXTURE2D_DESC upload_desc = {};
    upload_desc.Width = width;
    upload_desc.Height = height;
    upload_desc.MipLevels = 1;
    upload_desc.ArraySize = 1;
    upload_desc.Format = DXGI_FORMAT_NV12;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Usage = D3D11_USAGE_STAGING;
    upload_desc.BindFlags = 0;
    upload_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateTexture2D(&upload_desc, nullptr, &upload_texture);
    PrintHr("CreateTexture2D(NV12 staging)", hr);
    if (FAILED(hr)) return false;
  }

  PrintHr("ProcessMessage(NOTIFY_BEGIN_STREAMING)",
          mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
  PrintHr("ProcessMessage(NOTIFY_START_OF_STREAM)",
          mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

  ComPtr<IMFMediaEventGenerator> events;
  if (async) mft->QueryInterface(IID_PPV_ARGS(&events));

  LONGLONG timestamp = 0;  // 100 ns
  const LONGLONG duration = 10'000'000LL / fps;
  uint32_t pushed = 0;
  uint32_t collected = 0;
  std::vector<int64_t> push_times_us;

  // 造一帧输入样本：系统内存 NV12，或者 D3D11 NV12 纹理（零拷贝路径的前提）。
  const auto make_input_sample = [&](uint32_t frame, ComPtr<IMFSample>* out_sample) -> bool {
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    FillNv12(nv12.data(), width, height, stride, frame);
    if (frame < 5) {
      std::printf("      input frame %u checksum=0x%08X\n", frame,
                  Checksum(nv12.data(), nv12.size()));
    }
    if (use_texture && nv12_texture != nullptr) {
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      const HRESULT map_hr =
          context->Map(upload_texture.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
      if (FAILED(map_hr)) return false;
      std::memcpy(mapped.pData, nv12.data(), nv12.size());
      context->Unmap(upload_texture.Get(), 0);
      context->CopyResource(nv12_texture.Get(), upload_texture.Get());
      ComPtr<IMFMediaBuffer> dxgi_buffer;
      if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),
                                           nv12_texture.Get(), 0, FALSE, &dxgi_buffer))) {
        return false;
      }
      sample->AddBuffer(dxgi_buffer.Get());
    } else {
      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(MFCreateMemoryBuffer(nv12_size, &buffer))) return false;
      BYTE* data = nullptr;
      DWORD max_len = 0;
      if (FAILED(buffer->Lock(&data, &max_len, nullptr))) return false;
      std::memcpy(data, nv12.data(), nv12_size);
      buffer->Unlock();
      buffer->SetCurrentLength(nv12_size);
      sample->AddBuffer(buffer.Get());
    }
    sample->SetSampleTime(timestamp);
    sample->SetSampleDuration(duration);
    timestamp += duration;
    // 第 30 帧再要一次关键帧：libwebrtc 的 RequestKeyFrame / PLI 走的就是这条路径，
    // 只有「中途也能要出 IDR」才算能用。
    if (frame == 30) ForceKeyFrame(mft);
    *out_sample = sample;
    return true;
  };

  const int64_t loop_start_us = NowUs();

  // 驱动方式（对应生产实现 hw_encoder.cpp 的编码线程）：
  //   * 尽量把输入推进去（MFT 缓冲满时 ProcessInput 回 MF_E_NOTACCEPTING，就停下）；
  //   * 收到 METransformHaveOutput 才 ProcessOutput 收一个包（异步 MFT 不允许提前收，
  //     否则 E_UNEXPECTED）；
  //   * NeedInput 事件会被循环顶部「再推一帧」消化掉。
  while (collected < frame_count) {
    while (pushed < frame_count) {
      ComPtr<IMFSample> input_sample;
      if (!make_input_sample(pushed, &input_sample)) { *error = "build input sample"; return false; }
      if (pushed == 0) ForceKeyFrame(mft);
      const HRESULT push_hr = mft->ProcessInput(0, input_sample.Get(), 0);
      if (push_hr == MF_E_NOTACCEPTING) break;
      if (FAILED(push_hr)) {
        PrintHr("ProcessInput", push_hr);
        *error = std::string("ProcessInput failed: ") + HrName(push_hr);
        return false;
      }
      ++stats->frames_in;
      push_times_us.push_back(NowUs());
      ++pushed;
    }

    MediaEventType event_type = METransformHaveOutput;
    if (async && events != nullptr) {
      const HRESULT wait_hr = WaitForAnyTransformEvent(events.Get(), 1000, &event_type);
      if (wait_hr == kWaitTimeout) {
        std::printf("    (timeout waiting MFT event: pushed=%u collected=%u)\n", pushed,
                    collected);
        break;
      }
      if (FAILED(wait_hr)) {
        *error = "IMFMediaEventGenerator::GetEvent failed";
        return false;
      }
    }

    if (event_type == METransformNeedInput) continue;

    bool got = false;
    int idle_tries = 0;
    while (!got && idle_tries < 200) {
      if (!DrainOneOutput(mft, buffer_size, packets, error, &got)) return false;
      if (!got) {
        if (async) break;  // 异步 MFT 没有 HaveOutput 时不要硬收
        Sleep(1);
        ++idle_tries;
      }
    }
    if (!got) {
      if (!async) break;
      continue;
    }
    ++collected;
    const EncodedPacket& packet = packets->back();
    stats->bytes_out += packet.bytes.size();
    if (packet.keyframe) ++stats->keyframes;
    const int64_t latency_us =
        NowUs() - (collected <= push_times_us.size() ? push_times_us[collected - 1] : loop_start_us);
    std::printf("    [%u] %zu bytes keyframe=%d push->out=%.2f ms\n", collected,
                packet.bytes.size(), packet.keyframe ? 1 : 0,
                static_cast<double>(latency_us) / 1000.0);
  }

  // 收尾：通知 EOS（不再收剩余包，探针只关心稳态）。
  PrintHr("ProcessMessage(NOTIFY_END_OF_STREAM)",
          mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
  PrintHr("ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH)",
          mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
  PrintHr("ProcessMessage(NOTIFY_END_STREAMING)",
          mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0));

  const double wall_ms = static_cast<double>(NowUs() - loop_start_us) / 1000.0;
  stats->frames_out = static_cast<uint32_t>(packets->size());
  stats->total_ms = wall_ms;
  stats->avg_ms = stats->frames_out > 0 ? wall_ms / stats->frames_out : 0.0;
  return true;
}

// 用 Microsoft H264 Video Decoder MFT 把编出来的包解回来，验证不是垃圾。
// 收解码器当前所有可用输出。MF_E_TRANSFORM_STREAM_CHANGE 表示解码器刚从 SPS/PPS
// 里拿到真实尺寸、要求换输出类型 —— 必须重新 SetOutputType 再继续收，不是失败。
// 注意：stream change 之后输出缓冲的 cbSize 也会变，所以每轮都要重新
// GetOutputStreamInfo（第一版用了 stream change 之前的 cbSize，结果 E_FAIL）。
bool DrainDecoderOutputs(IMFTransform* decoder, uint32_t* decoded, UINT32* decoded_width,
                         UINT32* decoded_height) {
  for (;;) {
    MFT_OUTPUT_STREAM_INFO stream_info = {};
    decoder->GetOutputStreamInfo(0, &stream_info);
    MFT_OUTPUT_DATA_BUFFER out_buffer = {};
    DWORD status = 0;
    ComPtr<IMFSample> out_sample;
    if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
      if (stream_info.cbSize == 0) {
        std::printf("    (decoder output stream has no cbSize and does not provide samples)\n");
        return false;
      }
      if (FAILED(MFCreateSample(&out_sample))) return false;
      ComPtr<IMFMediaBuffer> out_mem;
      if (FAILED(MFCreateMemoryBuffer(stream_info.cbSize, &out_mem))) return false;
      out_sample->AddBuffer(out_mem.Get());
      out_buffer.pSample = out_sample.Get();
    }
    const HRESULT out_hr = decoder->ProcessOutput(0, 1, &out_buffer, &status);
    if (out_hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
    if (out_hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      // 逐个试可用输出类型，挑 NV12；换完之后 cbSize 会变，下一轮重新查。
      ComPtr<IMFMediaType> chosen;
      for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> available;
        if (FAILED(decoder->GetOutputAvailableType(0, index, &available))) break;
        GUID subtype = {};
        available->GetGUID(MF_MT_SUBTYPE, &subtype);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(available.Get(), MF_MT_FRAME_SIZE, &w, &h);
        std::printf("    decoder offered output[%u] subtype=%08X %ux%u\n", index,
                    static_cast<unsigned>(subtype.Data1), w, h);
        if (subtype == MFVideoFormat_NV12) {
          chosen = available;
          break;
        }
      }
      if (chosen == nullptr) {
        std::printf("    (decoder stream change but no NV12 output type)\n");
        return false;
      }
      PrintHr("decoder SetOutputType(after stream change)",
              decoder->SetOutputType(0, chosen.Get(), 0));
      if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
      continue;
    }
    if (FAILED(out_hr)) {
      PrintHr("decoder ProcessOutput", out_hr);
      if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
      return false;
    }
    IMFSample* delivered = out_buffer.pSample;
    if (delivered != nullptr) {
      ++(*decoded);
      if (*decoded == 1) {
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(decoder->GetOutputCurrentType(0, &current))) {
          MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, decoded_width, decoded_height);
        }
      }
      if (delivered != out_sample.Get()) delivered->Release();
    }
    if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
  }
}

bool VerifyWithDecoder(const std::vector<EncodedPacket>& packets, UINT32 width,
                       UINT32 height, uint32_t* decoded_frames, std::string* error) {
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  MFT_REGISTER_TYPE_INFO in_info = {MFMediaType_Video, MFVideoFormat_H264};
  MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, MFVideoFormat_NV12};
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                             MFT_ENUM_FLAG_SORTANDFILTER,
                         &in_info, &out_info, &activates, &count);
  PrintHr("MFTEnumEx(decoder)", hr);
  if (FAILED(hr) || count == 0) { *error = "no H264 decoder MFT"; return false; }
  ComPtr<IMFTransform> decoder;
  hr = activates[0]->ActivateObject(__uuidof(IMFTransform), (void**)&decoder);
  for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
  CoTaskMemFree(activates);
  PrintHr("decoder ActivateObject", hr);
  if (FAILED(hr) || decoder == nullptr) { *error = "decoder activate failed"; return false; }

  ComPtr<IMFMediaType> input;
  if (FAILED(MFCreateMediaType(&input))) { *error = "MFCreateMediaType(dec-in)"; return false; }
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  PrintHr("decoder SetInputType(H264)", decoder->SetInputType(0, input.Get(), 0));

  // 逐个试输出类型，找到 NV12。
  ComPtr<IMFMediaType> chosen_output;
  for (DWORD index = 0;; ++index) {
    ComPtr<IMFMediaType> available;
    if (FAILED(decoder->GetOutputAvailableType(0, index, &available))) break;
    GUID subtype = {};
    available->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (subtype == MFVideoFormat_NV12) {
      chosen_output = available;
      break;
    }
  }
  if (chosen_output == nullptr) { *error = "decoder offers no NV12 output"; return false; }
  PrintHr("decoder SetOutputType(NV12)", decoder->SetOutputType(0, chosen_output.Get(), 0));
  PrintHr("decoder NOTIFY_BEGIN_STREAMING",
          decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
  PrintHr("decoder NOTIFY_START_OF_STREAM",
          decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

  uint32_t decoded = 0;
  UINT32 decoded_width = 0;
  UINT32 decoded_height = 0;

  for (const EncodedPacket& packet : packets) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(packet.bytes.size()), &buffer))) {
      *error = "decoder MFCreateMemoryBuffer"; return false;
    }
    BYTE* data = nullptr;
    DWORD max_len = 0;
    if (FAILED(buffer->Lock(&data, &max_len, nullptr))) { *error = "decoder Lock"; return false; }
    std::memcpy(data, packet.bytes.data(), packet.bytes.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(packet.bytes.size()));
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) { *error = "decoder MFCreateSample"; return false; }
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(packet.time_us * 10);

    // MF_E_NOTACCEPTING：解码器输入队列满了，先把已有输出收干净再推。
    HRESULT input_hr = decoder->ProcessInput(0, sample.Get(), 0);
    if (input_hr == MF_E_NOTACCEPTING) {
      if (!DrainDecoderOutputs(decoder.Get(), &decoded, &decoded_width, &decoded_height)) {
        break;
      }
      input_hr = decoder->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(input_hr)) {
      PrintHr("decoder ProcessInput", input_hr);
      continue;
    }
    if (!DrainDecoderOutputs(decoder.Get(), &decoded, &decoded_width, &decoded_height)) {
      break;
    }
  }
  *decoded_frames = decoded;
  std::printf("    decoded frames=%u  first frame=%ux%u (encoder asked %ux%u)\n", decoded,
              decoded_width, decoded_height, width, height);
  return decoded > 0;
}

// 激活第一个能用的硬件 H.264 编码器（按友好名跳过激活失败的 Intel/AMD 空壳）。
bool OpenHardwareEncoder(bool unlock_on_activate, bool unlock_on_transform,
                         UINT32 width, UINT32 height, ComPtr<IMFTransform>* out_mft,
                         std::string* used_name, std::string* error) {
  MFT_REGISTER_TYPE_INFO in_info = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const HRESULT hr =
      MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in_info, &out_info,
                &activates, &count);
  PrintHr("MFTEnumEx(hw encoder)", hr);
  if (FAILED(hr) || count == 0) { *error = "no hardware H264 encoder MFT"; return false; }
  bool opened = false;
  for (UINT32 i = 0; i < count && !opened; ++i) {
    IMFActivate* activate = activates[i];
    const std::string name = FriendlyNameOf(activate);
    std::printf("  trying [%u] %s\n", i, name.c_str());
    UINT32 async = 0;
    if (SUCCEEDED(activate->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async != 0 &&
        unlock_on_activate) {
      activate->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }
    ComPtr<IMFTransform> mft;
    const HRESULT activate_hr =
        activate->ActivateObject(__uuidof(IMFTransform), (void**)&mft);
    PrintHr("ActivateObject", activate_hr);
    if (FAILED(activate_hr) || mft == nullptr) continue;

    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(mft->GetAttributes(&attributes))) {
      UINT32 mft_async = 0;
      UINT32 d3d11_aware = 0;
      attributes->GetUINT32(MF_TRANSFORM_ASYNC, &mft_async);
      attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware);
      std::printf("    mft attributes: async=%u d3d11_aware=%u\n", mft_async, d3d11_aware);
      if (unlock_on_transform) {
        PrintHr("SetUINT32(MFT_TRANSFORM_ASYNC_UNLOCK) on MFT",
                attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
      }
    }
    // 能配出类型就算可用；SET_D3D_MANAGER 在 main 里单独试。
    if (ConfigureEncoderTypes(mft.Get(), width, height, kFps, kBitrate, error)) {
      *out_mft = mft;
      *used_name = name;
      opened = true;
    } else {
      std::printf("    (configure failed: %s)\n", error->c_str());
    }
  }
  for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
  CoTaskMemFree(activates);
  if (!opened) { *error = "all hardware H264 encoder MFTs failed to open"; }
  return opened;
}

}  // namespace

int main(int argc, char** argv) {
  // 探针崩溃时缓冲区里的 printf 会全丢，直接关掉 stdout 缓冲。
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  bool run_texture = false;
  bool use_d3d_manager = true;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--texture") == 0) run_texture = true;
    if (std::strcmp(argv[i], "--memory-nod3d") == 0) use_d3d_manager = false;
  }
  std::printf("=== MFT H.264 encode probe  %ux%u @%u  frames=%u  input=%s  d3d_mgr=%d ===\n",
              kWidth, kHeight, kFps, kFrameCount, run_texture ? "texture" : "memory",
              use_d3d_manager ? 1 : 0);

  const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  PrintHr("CoInitializeEx", co_hr);
  PrintHr("MFStartup", MFStartup(MF_VERSION));

  // D3D11 设备（视频编码要求 VIDEO_SUPPORT）。
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
      ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, &chosen, &context);
  PrintHr("D3D11CreateDevice", hr);
  if (device != nullptr) {
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
  }

  UINT32 reset_token = 0;
  ComPtr<IMFDXGIDeviceManager> device_manager;
  PrintHr("MFCreateDXGIDeviceManager", MFCreateDXGIDeviceManager(&reset_token, &device_manager));
  if (device_manager != nullptr && device != nullptr) {
    PrintHr("ResetDevice", device_manager->ResetDevice(device.Get(), reset_token));
  }

  ComPtr<IMFTransform> encoder;
  std::string used_name;
  std::string error;
  if (!OpenHardwareEncoder(/*unlock_on_activate=*/true, /*unlock_on_transform=*/true,
                           kWidth, kHeight, &encoder, &used_name, &error)) {
    std::printf("FAILED to open hardware encoder: %s\n", error.c_str());
    MFShutdown();
    CoUninitialize();
    return 1;
  }
  std::printf("opened: %s\n", used_name.c_str());

  // 解锁后 SET_D3D_MANAGER 的关键一步。
  if (use_d3d_manager && device_manager != nullptr) {
    PrintHr("SET_D3D_MANAGER (after unlock)",
            encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                    reinterpret_cast<UINT_PTR>(device_manager.Get())));
  } else {
    std::printf("    (D3D manager NOT set; MFT must convert system-memory NV12 itself)\n");
  }

  UINT32 async = 0;
  {
    ComPtr<IMFAttributes> encoder_attributes;
    if (SUCCEEDED(encoder->GetAttributes(&encoder_attributes)) &&
        encoder_attributes != nullptr) {
      encoder_attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
    }
  }
  std::printf("encoder async=%u\n", async);

  std::vector<EncodedPacket> packets;
  EncodeStats stats;
  const double cpu_before = ProcessCpuSeconds();
  if (!RunEncodeSession(encoder.Get(), async != 0, kWidth, kHeight, kFps, kFrameCount,
                        device.Get(), context.Get(), run_texture, &packets, &stats,
                        &error)) {
    std::printf("encode session FAILED: %s\n", error.c_str());
    MFShutdown();
    CoUninitialize();
    return 2;
  }
  const double cpu_after = ProcessCpuSeconds();
  const double cpu_seconds = cpu_after - cpu_before;
  std::printf(
      "\n--- encode stats: in=%u out=%u keyframes=%u bytes=%llu avg=%.2f ms/帧 "
      "max=%.2f total=%.1f ms ---\n",
      stats.frames_in, stats.frames_out, stats.keyframes,
      static_cast<unsigned long long>(stats.bytes_out), stats.avg_ms, stats.max_ms,
      stats.total_ms);
  std::printf(
      "--- CPU: %.3f s for %u frames = %.3f ms CPU/帧, %.2f ms/帧 墙钟 "
      "(%.1f fps) ---\n",
      cpu_seconds, stats.frames_out,
      stats.frames_out > 0 ? cpu_seconds * 1000.0 / stats.frames_out : 0.0,
      stats.avg_ms, stats.avg_ms > 0 ? 1000.0 / stats.avg_ms : 0.0);

  uint32_t decoded = 0;
  if (!VerifyWithDecoder(packets, kWidth, kHeight, &decoded, &error)) {
    std::printf("decode verify FAILED: %s\n", error.c_str());
    MFShutdown();
    CoUninitialize();
    return 3;
  }
  std::printf("decode verify OK: %u frames\n", decoded);

  PrintHr("ProcessMessage(FLUSH)", encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
  encoder.Reset();
  MFShutdown();
  CoUninitialize();
  std::printf("\nprobe done\n");
  return 0;
}
