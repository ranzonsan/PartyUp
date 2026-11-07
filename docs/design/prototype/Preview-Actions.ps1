# Native UI Automation helper for this throwaway prototype only.
# It inspects or operates controls; it never edits source/configuration files.
param(
    [ValidateSet('Inspect', 'Invoke', 'Set', 'Toggle', 'Select', 'DoubleClick', 'Resize', 'Capture')]
    [string]$Action = 'Inspect',
    [string]$AutomationId = '',
    [string]$Name = '',
    [string]$NameBase64 = '',
    [string]$Value = '',
    [string]$ValueBase64 = '',
    [int]$MaxItems = 120,
    [int]$Width = 1280,
    [int]$Height = 800,
    [string]$OutputPath = ''
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
[void][System.Windows.Forms.Application]::SetHighDpiMode([System.Windows.Forms.HighDpiMode]::PerMonitorV2)
if ($NameBase64) { $Name = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($NameBase64)) }
if ($ValueBase64) { $Value = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ValueBase64)) }
$processes = @(Get-Process -Name 'PartyUp.UiPrototype' -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 })
if ($processes.Count -ne 1) {
    throw "Expected exactly one prototype window; found $($processes.Count)."
}
$window = [System.Windows.Automation.AutomationElement]::FromHandle($processes[0].MainWindowHandle)
if ($Action -eq 'Resize') {
    $windowPattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    $windowPattern.SetWindowVisualState([System.Windows.Automation.WindowVisualState]::Normal)
    $transform = $window.GetCurrentPattern([System.Windows.Automation.TransformPattern]::Pattern)
    if (-not $transform.Current.CanResize) { throw 'Prototype window cannot resize.' }
    $transform.Resize($Width, $Height)
    'Requested physical window size: ' + $Width + ' x ' + $Height
    exit 0
}
if ($Action -eq 'Capture') {
    if (-not $OutputPath) { throw 'Specify OutputPath for Capture.' }
    if ([System.IO.Path]::GetExtension($OutputPath) -ine '.png') { throw 'Capture output must be a PNG evidence file, never a source file.' }
    Add-Type -AssemblyName System.Drawing
    # Capture this HWND, never the foreground desktop. AppActivate + CopyFromScreen
    # races with user focus changes and can accidentally record another application.
    if (-not ('PartyUp.Preview.NativeCapture' -as [type])) {
        Add-Type -Namespace PartyUp.Preview -Name NativeCapture -MemberDefinition '[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError=true)] public static extern bool PrintWindow(System.IntPtr hwnd, System.IntPtr hdc, uint flags); [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError=true)] public static extern bool GetWindowRect(System.IntPtr hwnd, out RECT rect); public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }'
    }
    $bounds = New-Object PartyUp.Preview.NativeCapture+RECT
    if (-not [PartyUp.Preview.NativeCapture]::GetWindowRect($processes[0].MainWindowHandle, [ref]$bounds)) {
        throw 'Cannot read prototype window bounds.'
    }
    $widthPixels = $bounds.Right - $bounds.Left
    $heightPixels = $bounds.Bottom - $bounds.Top
    if ($widthPixels -le 0 -or $heightPixels -le 0) { throw 'Invalid prototype window bounds.' }
    $image = New-Object System.Drawing.Bitmap $widthPixels, $heightPixels
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($image)
        try {
            $hdc = $graphics.GetHdc()
            try {
                if (-not [PartyUp.Preview.NativeCapture]::PrintWindow($processes[0].MainWindowHandle, $hdc, 2)) {
                    throw 'Native prototype-only window capture failed; no desktop fallback is permitted.'
                }
            } finally { $graphics.ReleaseHdc($hdc) }
        } finally { $graphics.Dispose() }
        $directory = Split-Path -Parent $OutputPath
        if (-not (Test-Path -LiteralPath $directory)) {
            [void][System.IO.Directory]::CreateDirectory($directory)
        }
        $image.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $image.Dispose() }
    [pscustomobject]@{ Path=$OutputPath; Width=$widthPixels; Height=$heightPixels; Evidence='native PrintWindow pixels for the prototype HWND only; no foreground desktop capture' } | ConvertTo-Json
    exit 0
}
$condition = [System.Windows.Automation.Condition]::TrueCondition
if ($AutomationId) {
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $AutomationId)
} elseif ($Name) {
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
}
$elements = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
if ($elements.Count -eq 0 -and $Action -ne 'Inspect') {
    # MenuFlyout may own a popup HWND; restrict fallback strictly to this process.
    $processCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $processes[0].Id)
    $ownedCondition = New-Object System.Windows.Automation.AndCondition($processCondition, $condition)
    $elements = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants, $ownedCondition)
}
if ($Action -eq 'Inspect') {
    $items = @()
    foreach ($element in $elements) {
        $current = $element.Current
        if ($current.Name -or $current.AutomationId) {
            $items += [pscustomobject]@{
                Name = $current.Name
                Id = $current.AutomationId
                Type = $current.ControlType.ProgrammaticName
                Enabled = $current.IsEnabled
                Offscreen = $current.IsOffscreen
            }
        }
        if ($items.Count -ge $MaxItems) { break }
    }
    [pscustomobject]@{
        ProcessId = $processes[0].Id
        Title = $window.Current.Name
        TotalMatched = $elements.Count
        Displayed = $items.Count
        Controls = $items
    } | ConvertTo-Json -Depth 5
    exit 0
}
if (-not $AutomationId -and -not $Name) { throw 'Specify AutomationId or Name for an action.' }
$visibleMatches = @($elements | Where-Object {
    -not $_.Current.IsOffscreen -and
    ($Action -ne 'DoubleClick' -or $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::ListItem)
})
if ($visibleMatches.Count -ne 1) {
    throw "Expected exactly one visible matching control; found $($visibleMatches.Count) visible of $($elements.Count)."
}
$target = $visibleMatches[0]
if (-not $target.Current.IsEnabled) { throw 'Target control is disabled.' }
$pattern = $null
switch ($Action) {
    'DoubleClick' {
        # Targeted Win32 messages to our own HWND, never global mouse input/SendKeys.
        if (-not ('PartyUp.Preview.NativeInput' -as [type])) {
            Add-Type -Namespace PartyUp.Preview -Name NativeInput -MemberDefinition '[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern System.IntPtr SendMessage(System.IntPtr hwnd, uint msg, System.IntPtr wp, System.IntPtr lp); [System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool ScreenToClient(System.IntPtr hwnd, ref POINT point); public struct POINT { public int X; public int Y; }'
        }
        $rect = $target.Current.BoundingRectangle
        if ($rect.IsEmpty -or $rect.Width -le 0 -or $rect.Height -le 0) { throw 'List row has no clickable bounds.' }
        $point = New-Object PartyUp.Preview.NativeInput+POINT
        $point.X = [int]($rect.X + $rect.Width / 2)
        $point.Y = [int]($rect.Y + $rect.Height / 2)
        $hwnd = $processes[0].MainWindowHandle
        if (-not [PartyUp.Preview.NativeInput]::ScreenToClient($hwnd, [ref]$point)) { throw 'Cannot resolve target window coordinates.' }
        $packed = [IntPtr](($point.Y -shl 16) -bor ($point.X -band 0xffff))
        [void][PartyUp.Preview.NativeInput]::SendMessage($hwnd, 0x0200, [IntPtr]::Zero, $packed)
        [void][PartyUp.Preview.NativeInput]::SendMessage($hwnd, 0x0201, [IntPtr]1, $packed)
        [void][PartyUp.Preview.NativeInput]::SendMessage($hwnd, 0x0202, [IntPtr]::Zero, $packed)
        [void][PartyUp.Preview.NativeInput]::SendMessage($hwnd, 0x0201, [IntPtr]1, $packed)
        [void][PartyUp.Preview.NativeInput]::SendMessage($hwnd, 0x0202, [IntPtr]::Zero, $packed)
    }
    'Invoke' {
        if (-not $target.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
            throw 'Target has no InvokePattern.'
        }
        $pattern.Invoke()
    }
    'Set' {
        if (-not $target.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$pattern)) {
            throw 'Target has no ValuePattern.'
        }
        $pattern.SetValue($Value)
    }
    'Toggle' {
        if (-not $target.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pattern)) {
            throw 'Target has no TogglePattern.'
        }
        $pattern.Toggle()
    }
    'Select' {
        if (-not $target.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pattern)) {
            throw 'Target has no SelectionItemPattern.'
        }
        $pattern.Select()
    }
}
[pscustomobject]@{
    Action = $Action
    Id = $target.Current.AutomationId
    Name = $target.Current.Name
    Result = 'UI_ACTION_INVOKED_NOT_FUNCTIONAL_ACCEPTANCE'
} | ConvertTo-Json
