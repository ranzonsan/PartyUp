$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class MmDev {
  [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  public interface IMMDeviceEnumerator {
    int NotImpl1();
    int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice ppDevice);
    int GetDevice(string pwstrId, out IMMDevice ppDevice);
    int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection ppDevices);
  }
  [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  public interface IMMDevice {
    int Activate(ref Guid iid, int dwClsCtx, IntPtr pActivationParams, [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface);
    int OpenPropertyStore(int stgmAccess, out IPropertyStore ppProperties);
    int GetId([MarshalAs(UnmanagedType.LPWStr)] out string ppstrId);
    int GetState(out int pdwState);
  }
  [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  public interface IPropertyStore {
    int GetCount(out int cProps);
    int GetAt(int iProp, out PROPERTYKEY pkey);
    int GetValue(ref PROPERTYKEY key, out PROPVARIANT pv);
  }
  [StructLayout(LayoutKind.Sequential)]
  public struct PROPERTYKEY { public Guid fmtid; public int pid; }
  [StructLayout(LayoutKind.Sequential)]
  public struct PROPVARIANT { public short vt; public short r1; public short r2; public short r3; public IntPtr p; public int p2; }
  [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  public interface IMMDeviceCollection {
    int GetCount(out int pcDevices);
    int Item(int nDevice, out IMMDevice ppDevice);
  }
  [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
  public class MMDeviceEnumeratorComObject { }
  public static string[] List(int flow) {
    var en = (IMMDeviceEnumerator)(new MMDeviceEnumeratorComObject());
    IMMDeviceCollection col;
    int hr = en.EnumAudioEndpoints(flow, 1, out col); // 1 = DEVICE_STATE_ACTIVE
    if (hr != 0) return new string[] { "ENUM_FAILED_HR=" + hr };
    int n; col.GetCount(out n);
    var res = new string[n];
    for (int i = 0; i < n; i++) {
      IMMDevice dev; col.Item(i, out dev);
      string id; dev.GetId(out id);
      IPropertyStore ps; dev.OpenPropertyStore(0, out ps);
      var key = new PROPERTYKEY();
      key.fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"); key.pid = 14; // PKEY_Device_FriendlyName
      PROPVARIANT pv; ps.GetValue(ref key, out pv);
      string name = Marshal.PtrToStringUni(pv.p, pv.p2 * 2);
      if (string.IsNullOrEmpty(name) && pv.p != IntPtr.Zero) name = Marshal.PtrToStringUni(pv.p);
      int st; dev.GetState(out st);
      res[i] = (flow == 0 ? "RENDER" : "CAPTURE") + " | state=" + st + " | " + name;
    }
    return res;
  }
}
"@
Write-Output '== RENDER =='
[MmDev]::List(0) | ForEach-Object { Write-Output $_ }
Write-Output '== CAPTURE =='
[MmDev]::List(1) | ForEach-Object { Write-Output $_ }
