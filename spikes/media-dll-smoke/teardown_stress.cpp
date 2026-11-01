// PartyUp 媒体核心 DLL · teardown 压测（ABI v2）
//
// 目的：把「建引擎 → 起共享 → 连对端 → 销毁」这个收尾循环跑几百次，专门复现
// 偶发的收尾 0xC0000005。每次迭代：
//
//   1. 新起引擎 A（共享方）与 B（观看方），A pu_screen_start("synthetic")；
//   2. 两边对抽事件，把 A 的 SIGNAL_OUT 原样喂给 B、B 的喂回 A，直到双方
//      peer_state == PU_PEER_CONNECTED 且 B 至少解出 --min-frames 帧；
//   3. 按 --order 指定的顺序做收尾（stop / destroy 的先后不同，暴露不同窗口）；
//   4. 每次迭代单独一个日志文件（默认 out/run-logs/stress/iter-NNNN.log），
//      stderr 整体重定向过去，所以库里的 `[partyup-media]` 日志也在里面，逐行 flush。
//
// 崩溃时：进程内未处理异常过滤器（SetUnhandledExceptionFilter）会写
//   crash-iter-NNNN-pid-PPP.log  —— 异常码、异常地址、模块+偏移、符号化调用栈、
//                                   崩溃瞬间的「最后阶段」标记
//   crash-iter-NNNN-pid-PPP.dmp  —— MiniDumpNormal 小转储（给以后有 cdb 时复核）
// 然后用 TerminateProcess 退出（避免 WER 对话框卡住自动化）。
//
// 符号化用的是 Windows 自带的 dbghelp.dll + 构建脚本 -Symbols 产出的 PDB
// （PDB 与 dll/exe 放在同一目录即可）。不写注册表、不装调试器。
//
// 用法（构建脚本 -StressIterations 会自动跑；也可手动）：
//   teardown_stress.exe --iterations 200 --order viewer-first --logs <dir>
//   --order: viewer-first | media | signaling | abrupt | alternate
//
// 退出码：0 = 全部迭代正常收尾；1 = 有迭代的创建/启动/连接阶段失败；
//         3 = 进程内捕获到崩溃（crash 日志已写）。
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include "partyup_media.h"

#pragma comment(lib, "dbghelp.lib")

namespace {

// ---------------------------------------------------------------------------
// 崩溃现场：全局状态（崩溃过滤器运行在出错线程上，尽量不分配堆内存）
// ---------------------------------------------------------------------------
char g_log_dir[MAX_PATH] = ".";
char g_phase[192] = "startup";
std::atomic<int> g_iteration{0};
volatile LONG g_crashing = 0;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t T0() {
  static const int64_t t0 = NowUs();
  return t0;
}

double SinceMs() { return static_cast<double>(NowUs() - T0()) / 1000.0; }

void LogLine(const char* fmt, ...) {
  char body[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  std::fprintf(stderr, "[stress][%8.1fms][tid %5lu] %s\n", SinceMs(),
               static_cast<unsigned long>(GetCurrentThreadId()), body);
  std::fflush(stderr);
}

void Phase(const char* fmt, ...) {
  char body[160];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  // 崩溃过滤器要读 g_phase：这里不加锁，宁可读到半行也不能在崩溃线程上死等。
  std::snprintf(g_phase, sizeof(g_phase), "%s", body);
  LogLine("== %s", body);
}

// ---------------------------------------------------------------------------
// 符号化（dbghelp）。在 main 里预初始化，崩溃时只做查找+写文件。
// ---------------------------------------------------------------------------
void WriteFileText(HANDLE file, const char* text) {
  if (file == INVALID_HANDLE_VALUE || text == nullptr) return;
  DWORD written = 0;
  WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

void WriteFileTextF(HANDLE file, const char* fmt, ...) {
  char body[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  WriteFileText(file, body);
}

void WriteSymbol(HANDLE file, int index, DWORD64 address) {
  HANDLE process = GetCurrentProcess();
  char module[MAX_PATH] = "?";
  DWORD64 module_base = 0;
  IMAGEHLP_MODULE64 mi;
  std::memset(&mi, 0, sizeof(mi));
  mi.SizeOfStruct = sizeof(mi);
  if (SymGetModuleInfo64(process, address, &mi)) {
    std::snprintf(module, sizeof(module), "%s", mi.ModuleName);
    module_base = mi.BaseOfImage;
  }

  char sym_storage[sizeof(SYMBOL_INFO) + 512];
  SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(sym_storage);
  std::memset(sym_storage, 0, sizeof(sym_storage));
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 500;

  DWORD64 displacement = 0;
  if (SymFromAddr(process, address, &displacement, symbol)) {
    IMAGEHLP_LINE64 line;
    std::memset(&line, 0, sizeof(line));
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    const bool have_line =
        SymGetLineFromAddr64(process, address, &line_displacement, &line) != FALSE;
    WriteFileTextF(file, "  #%02d %s!%s+0x%llx  (+0x%llx in module)", index, module,
                   symbol->Name, static_cast<unsigned long long>(displacement),
                   static_cast<unsigned long long>(address - module_base));
    if (have_line) {
      WriteFileTextF(file, "  %s:%lu\n", line.FileName, line.LineNumber);
    } else {
      WriteFileText(file, "\n");
    }
  } else {
    WriteFileTextF(file, "  #%02d %s+0x%llx  (no symbol)\n", index, module,
                   static_cast<unsigned long long>(address - module_base));
  }
}

void WriteStackTrace(HANDLE file, CONTEXT* context) {
  STACKFRAME64 frame;
  std::memset(&frame, 0, sizeof(frame));
  frame.AddrPC.Offset = context->Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context->Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context->Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;

  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) break;
    WriteSymbol(file, i, frame.AddrPC.Offset);
  }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  if (InterlockedExchange(&g_crashing, 1) != 0) {
    return EXCEPTION_EXECUTE_HANDLER;
  }
  std::fflush(stderr);
  std::fflush(stdout);

  const DWORD pid = GetCurrentProcessId();
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%s\\crash-iter-%04d-pid-%lu.log", g_log_dir,
                g_iteration.load(), static_cast<unsigned long>(pid));
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);

  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  WriteFileTextF(file, "PartyUp teardown stress crash\n");
  WriteFileTextF(file, "phase      : %s\n", g_phase);
  WriteFileTextF(file, "iteration  : %d\n", g_iteration.load());
  WriteFileTextF(file, "pid        : %lu\n", static_cast<unsigned long>(pid));
  WriteFileTextF(file, "thread id  : %lu\n", static_cast<unsigned long>(GetCurrentThreadId()));
  WriteFileTextF(file, "exception  : 0x%08lX\n",
                 static_cast<unsigned long>(record->ExceptionCode));
  WriteFileTextF(file, "address    : 0x%p\n", record->ExceptionAddress);
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    const ULONG_PTR kind = record->ExceptionInformation[0];
    const ULONG_PTR target = record->ExceptionInformation[1];
    WriteFileTextF(file, "access     : %s 0x%llx\n",
                   kind == 0 ? "read" : (kind == 1 ? "write" : "execute"),
                   static_cast<unsigned long long>(target));
  }
  WriteFileTextF(file, "\nfaulting frame:\n");
  WriteSymbol(file, 0, reinterpret_cast<DWORD64>(record->ExceptionAddress));
  WriteFileTextF(file, "\nstack:\n");
  WriteStackTrace(file, info->ContextRecord);

  // 小转储：以后有 cdb / WinDbg 时可以复核寄存器与线程栈。
  char dump_path[MAX_PATH];
  std::snprintf(dump_path, sizeof(dump_path), "%s\\crash-iter-%04d-pid-%lu.dmp", g_log_dir,
                g_iteration.load(), static_cast<unsigned long>(pid));
  HANDLE dump = CreateFileA(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (dump != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info_for_dump;
    info_for_dump.ThreadId = GetCurrentThreadId();
    info_for_dump.ExceptionPointers = info;
    info_for_dump.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), pid, dump,
                      static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                                 MiniDumpWithUnloadedModules),
                      &info_for_dump, nullptr, nullptr);
    CloseHandle(dump);
  }
  CloseHandle(file);

  // 不走 WER：直接结束进程，让脚本能立刻看到退出码。
  TerminateProcess(GetCurrentProcess(), 3);
  return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------
// 每次迭代一个日志文件：把 stderr 指到该文件，库日志 + 阶段标记都落在里面。
// ---------------------------------------------------------------------------
bool BeginIterationLog(int iteration) {
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%s\\iter-%04d.log", g_log_dir, iteration);
  // 重定向本进程（exe 的 CRT）的 stderr。注意 DLL 是 /MT 编译的，自带一份 CRT，
  // _dup2 改不到它的 fd 表，所以还要用 PARTYUP_MEDIA_LOG 告诉 DLL 往同一文件写。
  const int fd = _open(path, _O_CREAT | _O_WRONLY | _O_TRUNC, _S_IREAD | _S_IWRITE);
  if (fd < 0) return false;
  _dup2(fd, 2);
  if (fd != 2) _close(fd);
  SetEnvironmentVariableA("PARTYUP_MEDIA_LOG", path);
  return true;
}

// ---------------------------------------------------------------------------
// 引擎操作
// ---------------------------------------------------------------------------
struct Tally {
  int frames = 0;
  int remote_frames = 0;
  int errors = 0;
  int signals = 0;
};

pu_engine* CreateEngine(const char* name) {
  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = name;
  config.log_level = 3;
  config.event_queue_capacity = 1024;
  return pu_engine_create(&config);
}

// 抽干一个引擎的事件；SIGNAL_OUT 原样转给对端（真实客户端发 WebSocket 的等价物）。
bool PumpEngine(pu_engine* from, const char* name, pu_engine* to, Tally* tally) {
  for (;;) {
    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    const int rc = pu_poll_event(from, &event, 0);
    if (rc == PU_ERR_TIMEOUT) return true;
    if (rc != PU_OK) {
      LogLine("FAIL: [%s] pu_poll_event rc=%d (%s)", name, rc, pu_last_error(from));
      return false;
    }
    switch (event.type) {
      case PU_EVENT_SIGNAL_OUT: {
        ++tally->signals;
        if (to == nullptr) break;
        const pu_signal_event& signal = event.data.signal;
        const int in_rc = pu_signal_in(to, signal.payload, signal.payload_len);
        if (in_rc != PU_OK) {
          LogLine("FAIL: [%s] pu_signal_in rc=%d (%s)", name, in_rc, pu_last_error(to));
          return false;
        }
        break;
      }
      case PU_EVENT_REMOTE_FRAME:
        ++tally->remote_frames;
        break;
      case PU_EVENT_FRAME:
        ++tally->frames;
        break;
      case PU_EVENT_ERROR:
        ++tally->errors;
        LogLine("[%s] ERROR event code=%d message=%s", name, event.data.error.code,
                event.data.error.message);
        break;
      default:
        break;
    }
  }
}

bool ReadStats(pu_engine* engine, pu_stats* out) {
  std::memset(out, 0, sizeof(*out));
  out->struct_size = static_cast<uint32_t>(sizeof(*out));
  return pu_peer_stats(engine, out) == PU_OK;
}

// 反复对抽事件直到双方 DTLS connected 且观看方解出 want_frames 帧（或超时）。
bool ExchangeUntilConnected(pu_engine* a, pu_engine* b, Tally* tally_a, Tally* tally_b,
                            int want_frames, int timeout_ms) {
  const int64_t deadline = NowUs() + static_cast<int64_t>(timeout_ms) * 1000;
  while (NowUs() < deadline) {
    if (!PumpEngine(a, "A", b, tally_a)) return false;
    if (!PumpEngine(b, "B", a, tally_b)) return false;
    if (tally_a->errors != 0 || tally_b->errors != 0) return false;

    pu_stats stats_a;
    pu_stats stats_b;
    if (ReadStats(a, &stats_a) && ReadStats(b, &stats_b)) {
      if (stats_a.peer_state == PU_PEER_CONNECTED && stats_b.peer_state == PU_PEER_CONNECTED &&
          tally_b->remote_frames >= want_frames) {
        return true;
      }
      if (stats_a.peer_state == PU_PEER_FAILED || stats_b.peer_state == PU_PEER_FAILED) {
        LogLine("peer connection failed (A=%d B=%d)", stats_a.peer_state, stats_b.peer_state);
        return false;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

// ---- 收尾顺序：每种顺序暴露不同的「谁先死」窗口 ----

// 观看方先走：B 被销毁时 A 还在推流（历史上报的崩溃位置就在这一类）。
void TeardownViewerFirst(pu_engine* a, pu_engine* b) {
  Phase("stop B");
  pu_screen_stop(b);
  Phase("destroy B");
  pu_engine_destroy(b);
  Phase("stop A");
  pu_screen_stop(a);
  Phase("destroy A");
  pu_engine_destroy(a);
}

// 现行 signaling 冒烟的顺序：先 stop 两边，再 destroy A，再 destroy B。
void TeardownSignalingOrder(pu_engine* a, pu_engine* b) {
  Phase("stop A");
  pu_screen_stop(a);
  Phase("stop B");
  pu_screen_stop(b);
  Phase("stop A again (idempotent)");
  pu_screen_stop(a);
  Phase("destroy A");
  pu_engine_destroy(a);
  Phase("destroy B");
  pu_engine_destroy(b);
}

// 现行 media_smoke 的顺序：先 stop 共享方，再销毁观看方，最后销毁共享方。
void TeardownMediaOrder(pu_engine* a, pu_engine* b) {
  Phase("stop A");
  pu_screen_stop(a);
  Phase("destroy B");
  pu_engine_destroy(b);
  Phase("destroy A");
  pu_engine_destroy(a);
}

// 都不 stop，直接销毁（UI 直接退出的路径）。
void TeardownAbrupt(pu_engine* a, pu_engine* b) {
  Phase("destroy B (running)");
  pu_engine_destroy(b);
  Phase("destroy A (running)");
  pu_engine_destroy(a);
}

// 共享方先被销毁，观看方还在收帧。
void TeardownSharerFirst(pu_engine* a, pu_engine* b) {
  Phase("destroy A (running)");
  pu_engine_destroy(a);
  Phase("stop B");
  pu_screen_stop(b);
  Phase("destroy B");
  pu_engine_destroy(b);
}

void PrintUsage() {
  std::printf(
      "usage: teardown_stress.exe [--iterations N] [--order NAME] [--logs DIR]\n"
      "                           [--min-frames N] [--connect-timeout-ms N]\n"
      "                           [--jitter-ms N]\n"
      "  --order: viewer-first (default) | media | signaling | abrupt | sharer-first |\n"
      "           alternate\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  int iterations = 50;
  int min_frames = 5;
  int connect_timeout_ms = 5000;
  int jitter_ms = 0;
  std::string order = "viewer-first";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::printf("missing value for %s\n", what);
        PrintUsage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--iterations") {
      iterations = std::atoi(next("--iterations"));
    } else if (arg == "--order") {
      order = next("--order");
    } else if (arg == "--logs") {
      std::snprintf(g_log_dir, sizeof(g_log_dir), "%s", next("--logs"));
    } else if (arg == "--min-frames") {
      min_frames = std::atoi(next("--min-frames"));
    } else if (arg == "--connect-timeout-ms") {
      connect_timeout_ms = std::atoi(next("--connect-timeout-ms"));
    } else if (arg == "--jitter-ms") {
      jitter_ms = std::atoi(next("--jitter-ms"));
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::printf("unknown argument: %s\n", arg.c_str());
      PrintUsage();
      return 2;
    }
  }
  if (iterations <= 0) {
    std::printf("--iterations must be > 0\n");
    return 2;
  }
  CreateDirectoryA(g_log_dir, nullptr);

  // 符号：进程启动时就初始化好，崩溃时只做查找（避免在崩溃线程上做重活）。
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS |
                SYMOPT_NO_PROMPTS);
  if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
    std::printf("warning: SymInitialize failed (%lu) — crash stacks will be unsymbolized\n",
                static_cast<unsigned long>(GetLastError()));
  }
  SetUnhandledExceptionFilter(CrashFilter);

  std::printf("partyup teardown stress: iterations=%d order=%s min_frames=%d jitter_ms=%d\n",
              iterations, order.c_str(), min_frames, jitter_ms);
  std::printf("logs: %s\n", g_log_dir);
  {
    char symbol_check[512] = {0};
    DWORD64 displacement = 0;
    char storage[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    std::memset(storage, 0, sizeof(storage));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 250;
    if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(&main), &displacement,
                    symbol)) {
      std::snprintf(symbol_check, sizeof(symbol_check), "%s+0x%llx", symbol->Name,
                    static_cast<unsigned long long>(displacement));
    } else {
      std::snprintf(symbol_check, sizeof(symbol_check), "(no PDB? SymFromAddr failed)");
    }
    std::printf("symbol check: %s\n", symbol_check);
  }

  const char* orders[] = {"viewer-first", "media", "signaling", "abrupt", "sharer-first"};
  int failures = 0;

  for (int iteration = 1; iteration <= iterations; ++iteration) {
    g_iteration.store(iteration);
    BeginIterationLog(iteration);
    Phase("iteration %d begin", iteration);

    pu_engine* a = CreateEngine("stress-A-sharer");
    if (a == nullptr) {
      LogLine("FAIL: create A returned null: %s", pu_last_error(nullptr));
      ++failures;
      break;
    }
    pu_engine* b = CreateEngine("stress-B-viewer");
    if (b == nullptr) {
      LogLine("FAIL: create B returned null: %s", pu_last_error(nullptr));
      pu_engine_destroy(a);
      ++failures;
      break;
    }
    Phase("engines created");

    const int start_rc = pu_screen_start(a, "synthetic");
    if (start_rc != PU_OK) {
      LogLine("FAIL: pu_screen_start(A) rc=%d (%s)", start_rc, pu_last_error(a));
      pu_engine_destroy(a);
      pu_engine_destroy(b);
      ++failures;
      break;
    }
    Phase("A started");

    Tally tally_a;
    Tally tally_b;
    const bool connected =
        ExchangeUntilConnected(a, b, &tally_a, &tally_b, min_frames, connect_timeout_ms);
    Phase("exchange done: connected=%d A.frames=%d B.remote=%d A.signals=%d",
          connected ? 1 : 0, tally_a.frames, tally_b.remote_frames, tally_a.signals);
    if (!connected) {
      ++failures;
      // 仍然走一次收尾：连接失败时的 teardown 同样要干净，不能把进程留在坏状态。
    }

    if (jitter_ms > 0) {
      const int delay = static_cast<int>(GetTickCount64() % static_cast<DWORD>(jitter_ms + 1));
      Phase("jitter %d ms", delay);
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

    std::string mode = order;
    if (mode == "alternate") {
      mode = orders[(iteration - 1) % (sizeof(orders) / sizeof(orders[0]))];
      Phase("alternate -> %s", mode.c_str());
    }
    Phase("teardown begin (%s)", mode.c_str());
    const int64_t teardown_begin = NowUs();
    if (mode == "viewer-first") {
      TeardownViewerFirst(a, b);
    } else if (mode == "media") {
      TeardownMediaOrder(a, b);
    } else if (mode == "signaling") {
      TeardownSignalingOrder(a, b);
    } else if (mode == "abrupt") {
      TeardownAbrupt(a, b);
    } else if (mode == "sharer-first") {
      TeardownSharerFirst(a, b);
    } else {
      std::printf("unknown --order %s\n", mode.c_str());
      return 2;
    }
    Phase("teardown done in %.1f ms",
          static_cast<double>(NowUs() - teardown_begin) / 1000.0);
    LogLine("ITERATION %d OK", iteration);

    if (iteration % 10 == 0 || iteration == iterations) {
      std::printf("  iteration %d/%d ok (connected=%d, teardown %.1f ms)\n", iteration,
                  iterations, connected ? 1 : 0,
                  static_cast<double>(NowUs() - teardown_begin) / 1000.0);
      std::fflush(stdout);
    }
  }

  if (failures != 0) {
    std::printf("FAILED: %d iteration(s) failed before/at connect\n", failures);
    return 1;
  }
  std::printf("PASS: %d iteration(s), no crash during teardown\n", iterations);
  return 0;
}
