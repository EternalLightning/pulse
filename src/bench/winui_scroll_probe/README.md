# WinUI scroll trial

This is a standalone XAML Island hosted in a WinForms window. It uses a WinUI 3 `ListView` to show the names in a directory. It does not replace Pulse's navigation, operations, or renderer.

Build on Windows x64 with .NET 8 SDK and access to NuGet:

```powershell
dotnet build WinUiScrollProbe.csproj -c Release
```

Run `WinUiScrollProbe.exe <directory>` from the build output. Dragging a folder onto `try_winui.bat` in the exported trial package also works. The program writes `scroll_samples.csv` beside the executable when it exits; each row records a composition callback time and the vertical scroll offset. `--auto` triggers one animated scroll after startup for repeatable instrumentation. Normal hands-on use needs no flag.

The probe requires the .NET 8 Desktop Runtime and Windows App Runtime 2.4 on the target PC. The exported package includes a fixed 1,500-file fixture and a Pulse baseline for side-by-side testing.

XAML Island setup follows Microsoft's [WinUI in Win32 guidance](https://learn.microsoft.com/windows/apps/desktop/modernize/host-controls-existing-desktop-apps) and [Windows App SDK Islands sample](https://github.com/microsoft/WindowsAppSDK-Samples/tree/main/Samples/Islands).
