using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.XamlTypeInfo;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Windows.Foundation;
using WinForms = System.Windows.Forms;

namespace Pulse.WinUiScrollProbe;

internal static class Program {
    [STAThread]
    private static void Main(string[] args) {
        var path = args.Length > 0 ? Path.GetFullPath(args[0]) : Environment.CurrentDirectory;
        if (!Directory.Exists(path)) throw new DirectoryNotFoundException(path);
        var names = Directory.EnumerateFileSystemEntries(path).Select(Path.GetFileName).ToArray();
        var queue = DispatcherQueueController.CreateOnCurrentThread();
        using var xaml = new ProbeApplication();
        WinForms.Application.AddMessageFilter(new XamlMessageFilter());
        WinForms.Application.EnableVisualStyles();
        WinForms.Application.SetCompatibleTextRenderingDefault(false);
        WinForms.Application.Run(new ProbeForm(path, names, args.Contains("--auto")));
        queue.ShutdownQueue();
    }
}

internal sealed class ProbeApplication : Microsoft.UI.Xaml.Application, IXamlMetadataProvider, IDisposable {
    private readonly WindowsXamlManager manager;
    private readonly XamlControlsXamlMetaDataProvider provider = new();

    public ProbeApplication() => manager = WindowsXamlManager.InitializeForCurrentThread();
    protected override void OnLaunched(LaunchActivatedEventArgs args) =>
        Resources.MergedDictionaries.Add(new XamlControlsResources());
    public IXamlType GetXamlType(string name) => provider.GetXamlType(name);
    public IXamlType GetXamlType(Type type) => provider.GetXamlType(type);
    public XmlnsDefinition[] GetXmlnsDefinitions() => provider.GetXmlnsDefinitions();
    public void Dispose() => manager.Dispose();
}

internal sealed class ProbeForm : WinForms.Form {
    private readonly DesktopWindowXamlSource island = new();
    private readonly Microsoft.UI.Xaml.Controls.ListView list = new();
    private readonly Stopwatch timer = Stopwatch.StartNew();
    private readonly List<(double timeMs, double offset)> samples = [];
    private ScrollViewer? scroll;
    private readonly string outputPath;

    public ProbeForm(string path, string?[] names, bool autoScroll) {
        Text = "Pulse WinUI scroll probe";
        Width = 960;
        Height = 720;
        outputPath = Path.Combine(AppContext.BaseDirectory, "scroll_samples.csv");
        var host = new WinForms.Panel { Dock = WinForms.DockStyle.Fill };
        Controls.Add(host);
        Shown += (_, _) => {
            island.Initialize(new Microsoft.UI.WindowId((ulong)host.Handle));
            island.SiteBridge.MoveAndResize(new Windows.Graphics.RectInt32(0, 0, host.Width, host.Height));
            host.Resize += (_, _) => island.SiteBridge.MoveAndResize(
                new Windows.Graphics.RectInt32(0, 0, host.Width, host.Height));
            var title = new TextBlock { Text = $"WinUI ListView  |  {path}  |  {names.Length} items", Margin = new Thickness(12) };
            list.ItemsSource = names;
            list.SelectionMode = ListViewSelectionMode.None;
            ScrollViewer.SetVerticalScrollMode(list, ScrollMode.Enabled);
            ScrollViewer.SetVerticalScrollBarVisibility(list, ScrollBarVisibility.Auto);
            ScrollViewer.SetIsDeferredScrollingEnabled(list, false);
            var stack = new Grid();
            stack.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            stack.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
            stack.Children.Add(title);
            Grid.SetRow(list, 1);
            stack.Children.Add(list);
            island.Content = stack;
            list.Loaded += (_, _) => {
                scroll = FindScrollViewer(list);
                if (autoScroll) {
                    var kickoff = new WinForms.Timer { Interval = 1000 };
                    kickoff.Tick += (_, _) => {
                        kickoff.Stop();
                        kickoff.Dispose();
                        scroll?.ChangeView(null, Math.Min(1200.0, scroll.ScrollableHeight), null, false);
                    };
                    kickoff.Start();
                }
            };
            Microsoft.UI.Xaml.Media.CompositionTarget.Rendering += OnRendering;
        };
        FormClosed += (_, _) => {
            Microsoft.UI.Xaml.Media.CompositionTarget.Rendering -= OnRendering;
            WriteSamples();
            island.Content = null;
        };
    }

    private void OnRendering(object? sender, object e) {
        if (scroll == null) return;
        samples.Add((timer.Elapsed.TotalMilliseconds, scroll.VerticalOffset));
    }

    private static ScrollViewer? FindScrollViewer(DependencyObject root) {
        if (root is ScrollViewer viewer) return viewer;
        for (var i = 0; i < Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChildrenCount(root); ++i) {
            var found = FindScrollViewer(Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChild(root, i));
            if (found != null) return found;
        }
        return null;
    }

    private void WriteSamples() {
        using var writer = new StreamWriter(outputPath, false, Encoding.UTF8);
        writer.WriteLine("time_ms,vertical_offset_dip");
        foreach (var (time, offset) in samples) writer.WriteLine($"{time:F3},{offset:F3}");
    }
}

internal sealed class XamlMessageFilter : WinForms.IMessageFilter {
    [StructLayout(LayoutKind.Sequential)]
    private struct Msg {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public int time;
        public int x;
        public int y;
    }

    [DllImport("Microsoft.UI.Windowing.Core.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ContentPreTranslateMessage(ref Msg message);

    public bool PreFilterMessage(ref WinForms.Message message) {
        var native = new Msg {
            hwnd = message.HWnd, message = (uint)message.Msg,
            wParam = message.WParam, lParam = message.LParam
        };
        return ContentPreTranslateMessage(ref native);
    }
}
