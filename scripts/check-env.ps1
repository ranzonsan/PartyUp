$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
"vs_buildtools = $vsPath"

if ($vsPath) {
    $cl = & cmd /c "call `"$vsPath\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && cl 2>&1" | Select-String 'Microsoft'
    if ($cl) { "cl = " + ($cl.ToString().Trim()) } else { "cl = UNKNOWN" }
}

"cmake  = " + ((& 'C:\Program Files\CMake\bin\cmake.exe' --version) | Select-Object -First 1)
"go_win = " + (& 'C:\Program Files\Go\bin\go.exe' version)
"python = " + (& "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe" --version)
"dotnet = " + (dotnet --version)
