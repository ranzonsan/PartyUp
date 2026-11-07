# Independent prototype collaboration contract

Throwaway native Avalonia UI prototype only. Namespace `PartyUp.UiPrototype`. No reference to production client/server/media-core, no real network, audio, capture, credentials, global shortcuts, tray, startup or settings persistence. All code edits use direct write/edit tools, never shell-generated code. Main owns project entry/build; state author owns DemoState.cs only; UI author owns PrototypeWindow.axaml, PrototypeWindow.axaml.cs, PrototypeTheme.axaml only. Do not edit another author's scope.

## Shared model (state author implements, UI binds)

`public partial class DemoState : CommunityToolkit.Mvvm.ComponentModel.ObservableObject`.

Writable string properties: `ServerAddress` (example.invalid), `Username` (demo), `Password`, `InviteCode`, `BrowseChannel`, `InputText`, `SelectedShare`, `Variant` (A/B/C), `ThemeChoice` (深色/浅色/跟随系统), `TextSizeChoice` (标准/大号), `InputDeviceChoice`, `OutputDeviceChoice`, `InputModeChoice` (自由说话/按住说话).
Writable bool properties: `IsRegistering`, `IsChatVisible`, `ShowInspector`, `FailNextTransfer`, `FailNextSend`, `CloseToTray`, `StartOnLogin`, `StartHidden`.
Observable state bool properties: `IsLoggedIn`, `IsBusy`, `IsConnected`, `IsWatching`, `IsFullScreen`, `ShowSettings`, `IsPublishing`, `IsTransferPending`, `HasTransferFailure`, `HasFailedMessage`, `HasNewMessages`, `IsMuted`, `IsDeafened`.
Observable state strings: `LoginError`, `StatusText`, `DeviceNotice`, `LastCallChannel`, `PendingChannel`. `CallChannel` may be empty string for no active call (not null).
Collections: `ObservableCollection<string> Channels` (开黑大厅/攻略与杂谈/一起看), `Shares` (阿岚 · 桌面/小满 · 窗口), `ObservableCollection<DemoMessage> Messages`. `DemoMessage` has public get `Author`, `Body`, `Time` string properties.
Derived bindable properties: `bool IsCallActive`, `CanSend`, `CanJoin`, `CanPublish`, `CanWatch`, `CanRetry`; `string CallSummary`, `BrowseSummary`, `WatchSummary`, `InspectorText`. Notify these whenever source state changes. Empty/no-call state remains visibly actionable.

## ICommand properties (generated RelayCommand names)

- `LoginCommand` async, validate empty fields/invite in registration; simulate busy then log in. No actual login request.
- `JoinCommand`: join BrowseChannel when no call. Different active call proposes transfer, preserve old state until confirmed. When disconnected do nothing with visible reason.
- `ConfirmTransferCommand` async: leave old, stop own publication and viewing/fullscreen, join PendingChannel or fail when FailNextTransfer; failure leaves no call, preserves previous channel as LastCallChannel. Never auto-rejoin or auto-publish.
- `CancelTransferCommand`: preserve old call/publish/watch. `ReturnPreviousCommand`: browse previous only, never auto-join.
- `LeaveCommand`: local end-call, stop publication/watch/fullscreen even offline, don't auto-resume after reconnect.
- `MuteCommand`, `DeafenCommand`: local toggles while active, work offline; display simulation.
- `PublishCommand`: while connected and active toggle own publishing, briefly show preparation via status; no actual capture. If no call/connection, status explains.
- `WatchCommand` takes optional string target; use SelectedShare if null. Only while active and connected; changes target then simulates waiting for first frame, never changes BrowseChannel. Repeated requests protected from stale completion.
- `StopWatchCommand`, `EndShareCommand` (simulated target ends): clear watching/fullscreen and return to chat, preserve BrowseChannel.
- `ToggleFullscreenCommand`, `ToggleChatCommand` (only layout).
- `OpenSettingsCommand`, `CloseSettingsCommand`: settings preserve active call/viewing, exit fullscreen on open, return prior task on close.
- `LogoutCommand`: clear identity/session/mock input/drafts and return login; preserve no old media state. No real persistence.
- `SendCommand`, `RetryCommand`: mock send to BrowseChannel, preserve original text on injected failure, per-channel drafts, no auto-send after reconnect. Clear failure only when successfully retried, not on unrelated channel navigation.
- `DisconnectCommand`, `ReconnectCommand`: simulate transport, don't send draft automatically. Disconnected disallows new media/send, never disables stopping/leaving/logout/muting.
- `InjectMessageCommand`, `MarkReadingHistoryCommand`, `JumpToLatestCommand`: demonstrate new message/reading history without forced scrolling.
- `InjectDeviceRemovalCommand`: show mock device removal/default fallback notice, no real enumeration or audio.
- `ShowMockNoticeCommand` takes string feature name, explicitly says demonstration only / pending real-device acceptance, never reports actual system success.

Initial state: login view, connected simulation, BrowseChannel=攻略与杂谈, no active call, Shares contains the two synthetic targets (UI must hide or explain unavailable until joined). On mock login show populated chat. Allow a clear demo-toolbar scenario preset through ordinary commands, not production code.

## Lead integration additions

The first integration adds `Sources` (ObservableCollection<string>), `SelectedSource` (string), `FailNextPublish` (bool), `HasPublishFailure` (bool), `PublishError` (string), and public derived `IsWaitingForFrame` (bool). Publication-source choice is independent of SelectedShare (the watch target). Publish failures preserve the selected source and never claim actual capture. IsWaitingForFrame drives the waiting stage and stale request generations still guard completion. These additions were made with direct edit tools after both implementation authors finished.

## Visual composition

Avalonia 12.1.4 + FluentTheme, CommunityToolkit.Mvvm 8.4.2, compiled bindings. Root x:DataType DemoState; data templates x:DataType DemoMessage. Use normal Avalonia controls; no manually drawn SVG icons, no web shell. Main provides App with FluentTheme + StyleInclude pointing to PrototypeTheme.axaml. Main provides icon resource `avares://PartyUp.UiPrototype/Assets/icon.ico`.

PrototypeWindow code-behind only InitializeComponent, DataContext=new DemoState, visual theme/font/variant/width handling and keyboard Enter/Escape-to-command forwarding; business stays state. Need four screens, live dark/light/system, 1280×800/min900×600, 8 native main states, clearly marked demo data everywhere. Primary A rail/stage/callbar; B top-channel-navigation wider workspace; C call-stage-first with text sidebar. Native variant selector visible in prototype tools, not production UI. Variant change must visibly alter hierarchy/layout, not just colors. Keep consistent behavior and call ownership.

Do not add new channel types, stop watch on text navigation, hide call join/failed state, lower min height to540 or use dark-orange-on-dark unreadable styles. Brand #FF5A2B, on-brand #0B0C0E, cream #F4F1EA; light separately defined. Settings panel does not end media, show mock groups: appearance, audio, shortcut (unbound), window/startup, about/diagnostics. No real device tests or global hooks. Main project will have no DevTools login hook until explicitly configured, no production native DLL.
