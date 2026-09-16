using System.Collections.Concurrent;
using System.Text;
using System.Text.Json;
using JustCef.Tests.Infrastructure;
using Xunit;

namespace JustCef.Tests.RealNative;

public sealed class RealNativeFactAttribute : FactAttribute
{
    public RealNativeFactAttribute()
    {
        string? path = RealNativeHarness.NativePath;
        if (string.IsNullOrEmpty(path))
            Skip = "JUSTCEF_NATIVE_PATH is not set.";
        else if (!File.Exists(path))
            Skip = $"JUSTCEF_NATIVE_PATH does not exist: {path}";
    }
}

public sealed class RealNativeHarness : IAsyncDisposable
{
    public const string Origin = "https://justcef.test";
    public static readonly TimeSpan Wait = TimeSpan.FromSeconds(60);

    public static string? NativePath => Environment.GetEnvironmentVariable("JUSTCEF_NATIVE_PATH");
    public static string? NativeArguments => Environment.GetEnvironmentVariable("JUSTCEF_NATIVE_ARGS");

    private readonly ConcurrentDictionary<string, Func<IPCRequest, Task<IPCResponse?>>> _routes = new();

    public JustCefProcess Process { get; }

    static RealNativeHarness()
    {
        TestUtil.QuietLogs();
    }

    private RealNativeHarness(JustCefProcess process)
    {
        Process = process;
        Route("/index.html", _ => Task.FromResult<IPCResponse?>(Html("<!doctype html><html><body><h1>JustCef test</h1></body></html>")));
    }

    public static async Task<RealNativeHarness> StartAsync(Action<JustCefProcess>? configure = null)
    {
        var process = new JustCefProcess
        {
            NativeExecutablePath = NativePath
        };
        configure?.Invoke(process);

        var harness = new RealNativeHarness(process);
        try
        {
            harness.Process.Start(NativeArguments);
            await harness.Process.WaitForReadyAsync().WithTimeout(Wait).ConfigureAwait(false);
            return harness;
        }
        catch
        {
            await harness.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    public void Route(string path, Func<IPCRequest, Task<IPCResponse?>> handler) => _routes[path] = handler;

    public Task<IPCResponse?> ServeAsync(JustCefWindow window, IPCRequest request)
    {
        var uri = new Uri(request.Url);
        if (_routes.TryGetValue(uri.AbsolutePath, out var handler))
            return handler(request);
        return Task.FromResult<IPCResponse?>(Text("not found", 404));
    }

    public async Task<JustCefWindow> OpenPageAsync(Func<JustCefWindow, string, string?, Task<string?>>? bridgeRpcHandler = null, Func<JustCefWindow, IPCRequest, IPCRequest?>? requestModifier = null, string path = "/index.html")
    {
        var window = await Process.CreateWindowAsync(Origin + path, 320, 240, 800, 600,
            developerToolsEnabled: true,
            proxyRequests: true,
            requestProxy: ServeAsync,
            modifyRequests: requestModifier != null,
            requestModifier: requestModifier,
            modifyRequestBody: requestModifier != null,
            bridgeEnabled: true,
            bridgeRpcHandler: bridgeRpcHandler).WithTimeout(Wait);
        await window.WaitUntilLoadedAsync().WithTimeout(Wait);
        return window;
    }

    public Task WaitForChildExitAsync() => Process.ChildProcess?.WaitForExitAsync() ?? Task.CompletedTask;

    public static IPCResponse Html(string html) => new()
    {
        StatusCode = 200,
        StatusText = "OK",
        Headers = new Dictionary<string, List<string>>(StringComparer.InvariantCultureIgnoreCase)
        {
            ["Content-Type"] = new List<string> { "text/html; charset=utf-8" }
        },
        Body = Encoding.UTF8.GetBytes(html)
    };

    public static IPCResponse Text(string text, int statusCode = 200) => new()
    {
        StatusCode = statusCode,
        StatusText = statusCode == 200 ? "OK" : "Error",
        Headers = new Dictionary<string, List<string>>(StringComparer.InvariantCultureIgnoreCase)
        {
            ["Content-Type"] = new List<string> { "text/plain" },
            ["Access-Control-Allow-Origin"] = new List<string> { "*" }
        },
        Body = Encoding.UTF8.GetBytes(text)
    };

    public static async Task<JsonElement> EvaluateAsync(JustCefBrowser browser, string expression, bool awaitPromise = true, CancellationToken cancellationToken = default)
    {
        string parameters = JsonSerializer.Serialize(new
        {
            expression,
            awaitPromise,
            returnByValue = true
        });

        var (success, data) = await browser.ExecuteDevToolsMethodAsync("Runtime.evaluate", parameters, cancellationToken).ConfigureAwait(false);
        using var document = JsonDocument.Parse(data);
        if (!success)
            throw new InvalidOperationException($"Runtime.evaluate failed: {Encoding.UTF8.GetString(data)}");
        if (document.RootElement.TryGetProperty("exceptionDetails", out var exception))
            throw new InvalidOperationException($"Script threw: {exception}");

        var result = document.RootElement.GetProperty("result");
        return result.TryGetProperty("value", out var value) ? value.Clone() : default;
    }

    public async ValueTask DisposeAsync()
    {
        try
        {
            Process.Dispose();
            await WaitForChildExitAsync().WithTimeout(TimeSpan.FromSeconds(30)).ConfigureAwait(false);
        }
        catch
        {
        }
    }
}
