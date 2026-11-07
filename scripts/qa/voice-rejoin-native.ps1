param(
  [string]$DllDir = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client'),
  [int]$HoldSeconds = 5,
  [int]$Rounds = 3
)

# Native ABI probe: use the product partyup_media.dll directly, run join -> leave -> join
# rounds and sample pu_voice_level (real accumulated PCM frames of the local mic sink).
# This is the send-side evidence: does local capture still run after re-join?
# No server / no peer needed.
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class WinApi {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern IntPtr LoadLibraryW(string path);
  [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
  public static extern IntPtr GetProcAddress(IntPtr module, string name);
}

[StructLayout(LayoutKind.Sequential)]
public struct PuConfig {
  public uint struct_size;
  public IntPtr client_name;
  public uint log_level;
  public uint event_queue_capacity;
}

[StructLayout(LayoutKind.Sequential)]
public struct PuVoiceParams {
  public uint struct_size;
  public IntPtr channel_id;
  public int mode;
  public uint flags;
}

[StructLayout(LayoutKind.Sequential)]
public struct PuVoiceLevelInfo {
  public uint struct_size;
  public int valid;
  public uint peak;
  public uint rms;
  public uint frames;
  public uint source;
  public ulong last_frame_ms;
}

public static class PuVoiceProbe {
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  public delegate IntPtr EngineCreateFn(ref PuConfig cfg);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  public delegate void EngineDestroyFn(IntPtr engine);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  public delegate int VoiceJoinFn(IntPtr engine, ref PuVoiceParams p);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  public delegate int VoiceLeaveFn(IntPtr engine);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  public delegate int VoiceLevelFn(IntPtr engine, ref PuVoiceLevelInfo info);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  public delegate IntPtr LastErrorFn(IntPtr engine);

  private static IntPtr module;
  private static EngineCreateFn create;
  private static EngineDestroyFn destroy;
  private static VoiceJoinFn join;
  private static VoiceLeaveFn leave;
  private static VoiceLevelFn level;
  private static LastErrorFn lastError;

  public static readonly uint ConfigSize = (uint)Marshal.SizeOf(typeof(PuConfig));
  public static readonly uint VoiceParamsSize = (uint)Marshal.SizeOf(typeof(PuVoiceParams));
  public static readonly uint VoiceLevelSize = (uint)Marshal.SizeOf(typeof(PuVoiceLevelInfo));

  public static void Load(string dir) {
    module = WinApi.LoadLibraryW(System.IO.Path.Combine(dir, "partyup_media.dll"));
    if (module == IntPtr.Zero) throw new Exception("LoadLibrary failed: " + Marshal.GetLastWin32Error());
    create = (EngineCreateFn)Marshal.GetDelegateForFunctionPointer(WinApi.GetProcAddress(module, "pu_engine_create"), typeof(EngineCreateFn));
    destroy = (EngineDestroyFn)Marshal.GetDelegateForFunctionPointer(WinApi.GetProcAddress(module, "pu_engine_destroy"), typeof(EngineDestroyFn));
    join = (VoiceJoinFn)Marshal.GetDelegateForFunctionPointer(WinApi.GetProcAddress(module, "pu_voice_join"), typeof(VoiceJoinFn));
    leave = (VoiceLeaveFn)Marshal.GetDelegateForFunctionPointer(WinApi.GetProcAddress(module, "pu_voice_leave"), typeof(VoiceLeaveFn));
    level = (VoiceLevelFn)Marshal.GetDelegateForFunctionPointer(WinApi.GetProcAddress(module, "pu_voice_level"), typeof(VoiceLevelFn));
    lastError = (LastErrorFn)Marshal.GetDelegateForFunctionPointer(WinApi.GetProcAddress(module, "pu_last_error"), typeof(LastErrorFn));
  }

  public static IntPtr EngineCreate(ref PuConfig cfg) { return create(ref cfg); }
  public static void EngineDestroy(IntPtr engine) { destroy(engine); }
  public static int VoiceJoinCall(IntPtr engine, ref PuVoiceParams p) { return join(engine, ref p); }
  public static int VoiceLeaveCall(IntPtr engine) { return leave(engine); }
  public static int VoiceLevelCall(IntPtr engine, ref PuVoiceLevelInfo info) { return level(engine, ref info); }
  public static string LastErrorText(IntPtr engine) {
    IntPtr p = lastError(engine);
    return p == IntPtr.Zero ? "" : Marshal.PtrToStringAnsi(p);
  }
}
"@

# Use [Console]::WriteLine instead of Write-Output: the latter becomes part of a
# function's return value, so reading the join return code would get log strings.
function Log([string]$m) { [Console]::WriteLine("[" + (Get-Date -Format 'HH:mm:ss.fff') + "] " + $m) }

[PuVoiceProbe]::Load($DllDir)
Log ("DLL_LOADED dir=" + $DllDir)

$clientName = [System.Runtime.InteropServices.Marshal]::StringToHGlobalAnsi('voice-rejoin-native-probe')
$cfg = New-Object PuConfig
$cfg.struct_size = [PuVoiceProbe]::ConfigSize
$cfg.client_name = $clientName
$cfg.log_level = 3
$cfg.event_queue_capacity = 4096
$engine = [PuVoiceProbe]::EngineCreate([ref]$cfg)
if ($engine -eq [IntPtr]::Zero) {
  Log ("ENGINE_CREATE_FAILED err=" + [PuVoiceProbe]::LastErrorText([IntPtr]::Zero))
  exit 2
}
Log ("ENGINE_CREATED handle=" + $engine.ToString())
Log ("SIZES cfg=" + [PuVoiceProbe]::ConfigSize + " voiceParams=" + [PuVoiceProbe]::VoiceParamsSize +
     " level=" + [PuVoiceProbe]::VoiceLevelSize + " rounds=" + $Rounds + " hold=" + $HoldSeconds)

$channelId = [System.Runtime.InteropServices.Marshal]::StringToHGlobalAnsi('voice-rejoin-probe')

function Invoke-Join([int]$round) {
  $p = New-Object PuVoiceParams
  $p.struct_size = [PuVoiceProbe]::VoiceParamsSize
  $p.channel_id = $channelId
  $p.mode = 1          # PU_MODE_DIRECT: native only implements P2P; the product client also passes 1
  $p.flags = 0         # active (offerer)
  $rc = [PuVoiceProbe]::VoiceJoinCall($engine, [ref]$p)
  Log ("JOIN" + $round + "_RC=" + $rc + " err=" + [PuVoiceProbe]::LastErrorText($engine))
  return $rc
}

function Measure-Level([int]$round, [int]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $maxFrames = [uint32]0
  $maxPeak = [uint32]0
  $samples = 0
  while ((Get-Date) -lt $deadline) {
    $info = New-Object PuVoiceLevelInfo
    $info.struct_size = [PuVoiceProbe]::VoiceLevelSize
    $rc = [PuVoiceProbe]::VoiceLevelCall($engine, [ref]$info)
    if ($rc -eq 0) {
      if ($info.frames -gt $maxFrames) { $maxFrames = $info.frames }
      if ($info.peak -gt $maxPeak) { $maxPeak = $info.peak }
      $samples++
      Log ("LEVEL r=" + $round + " rc=" + $rc + " valid=" + $info.valid + " frames=" + $info.frames +
           " peak=" + $info.peak + " rms=" + $info.rms + " source=" + $info.source)
    } else {
      Log ("LEVEL r=" + $round + " rc=" + $rc + " err=" + [PuVoiceProbe]::LastErrorText($engine))
    }
    Start-Sleep -Milliseconds 500
  }
  Log ("SUMMARY r=" + $round + " samples=" + $samples + " maxFrames=" + $maxFrames + " maxPeak=" + $maxPeak)
  return @{ Frames = $maxFrames; Peak = $maxPeak }
}

try {
  $results = @()
  for ($r = 1; $r -le $Rounds; $r++) {
    $joinRc = Invoke-Join $r
    if ($joinRc -ne 0) {
      Start-Sleep -Seconds 1
      continue
    }
    Start-Sleep -Milliseconds 1200
    $res = Measure-Level $r $HoldSeconds
    $results += ("ROUND" + $r + "_maxFrames=" + $res.Frames + " maxPeak=" + $res.Peak)
    $leaveRc = [PuVoiceProbe]::VoiceLeaveCall($engine)
    Log ("LEAVE" + $r + "_RC=" + $leaveRc + " err=" + [PuVoiceProbe]::LastErrorText($engine))
    Start-Sleep -Milliseconds 1500
  }
  $results | ForEach-Object { Log $_ }
} finally {
  [PuVoiceProbe]::EngineDestroy($engine)
  Log 'ENGINE_DESTROYED'
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($clientName)
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($channelId)
}
