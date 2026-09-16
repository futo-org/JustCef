using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json.Nodes;

namespace JustCef.Tests.Infrastructure;

public sealed class FakeNativeHarness : IAsyncDisposable
{
    public static readonly TimeSpan DefaultWait = TimeSpan.FromSeconds(15);

    private static readonly Lazy<string> ExecutablePath = new(LocateOrBuild, LazyThreadSafetyMode.ExecutionAndPublication);
    private static int _tagCounter;

    private readonly TcpListener _listener;
    private readonly object _eventsLock = new();
    private readonly List<JsonObject> _events = new();
    private readonly List<(Func<JsonObject, bool> Predicate, TaskCompletionSource<JsonObject> Completion)> _waiters = new();
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    private TcpClient? _client;
    private Stream? _stream;

    public JustCefProcess Process { get; }
    public string LogPath { get; }
    public TaskCompletionSource ControlClosed { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

    static FakeNativeHarness()
    {
        TestUtil.QuietLogs();
    }

    private FakeNativeHarness(JustCefProcess process, TcpListener listener, string logPath)
    {
        Process = process;
        _listener = listener;
        LogPath = logPath;
    }

    public static string FakeNativePath => ExecutablePath.Value;

    public static async Task<FakeNativeHarness> StartAsync(Action<JustCefProcess>? configure = null, string? extraArguments = null, bool waitForReady = true)
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        int port = ((IPEndPoint)listener.LocalEndpoint).Port;
        string logPath = Path.Combine(Path.GetTempPath(), $"fake-native-{Guid.NewGuid():N}.log");

        var process = new JustCefProcess
        {
            NativeExecutablePath = FakeNativePath,
            DefaultCallTimeout = TimeSpan.FromSeconds(20)
        };
        configure?.Invoke(process);

        var harness = new FakeNativeHarness(process, listener, logPath);
        try
        {
            harness.Process.Start($"--fake-control-port {port} --fake-log {logPath}" + (extraArguments == null ? "" : " " + extraArguments));
            var client = await listener.AcceptTcpClientAsync().WithTimeout(DefaultWait).ConfigureAwait(false);
            client.NoDelay = true;
            harness._client = client;
            harness._stream = client.GetStream();
            harness.StartReading();
            await harness.WaitForEventAsync(e => Name(e) == "hello").ConfigureAwait(false);
            if (waitForReady)
                await harness.Process.WaitForReadyAsync().WithTimeout(DefaultWait).ConfigureAwait(false);
            return harness;
        }
        catch
        {
            await harness.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    public static string Name(JsonObject e) => (string?)e["event"] ?? "";

    public static string NewTag() => "t" + Interlocked.Increment(ref _tagCounter);

    public IReadOnlyList<JsonObject> Events
    {
        get
        {
            lock (_eventsLock)
                return _events.ToList();
        }
    }

    public IReadOnlyList<JsonObject> EventsNamed(string name) => Events.Where(e => Name(e) == name).ToList();

    public async Task SendAsync(JsonObject command)
    {
        byte[] line = Encoding.UTF8.GetBytes(command.ToJsonString() + "\n");
        await _sendLock.WaitAsync().ConfigureAwait(false);
        try
        {
            await _stream!.WriteAsync(line).ConfigureAwait(false);
            await _stream.FlushAsync().ConfigureAwait(false);
        }
        finally
        {
            _sendLock.Release();
        }
    }

    public async Task<JsonObject> CommandAsync(JsonObject command, string expectedEvent, TimeSpan? timeout = null)
    {
        string tag = NewTag();
        command["tag"] = tag;
        var wait = WaitForEventAsync(e => (string?)e["tag"] == tag && (Name(e) == expectedEvent || Name(e) == "error"), timeout);
        await SendAsync(command).ConfigureAwait(false);
        var result = await wait.ConfigureAwait(false);
        if (Name(result) == "error")
            throw new InvalidOperationException($"FakeNative command failed: {result.ToJsonString()}");
        return result;
    }

    public Task<JsonObject> AckAsync(JsonObject command) => CommandAsync(command, "ack");

    public Task ConfigureAsync(JsonNode opcode, string behavior = "ok", Action<JsonObject>? extra = null)
    {
        var command = new JsonObject
        {
            ["cmd"] = "configure",
            ["opcode"] = opcode,
            ["behavior"] = behavior
        };
        extra?.Invoke(command);
        return AckAsync(command);
    }

    public Task NotifyAsync(int opcode, int browserId, Action<JsonObject>? extra = null)
    {
        var command = new JsonObject
        {
            ["cmd"] = "notify",
            ["opcode"] = opcode,
            ["browserId"] = browserId
        };
        extra?.Invoke(command);
        return AckAsync(command);
    }

    public Task<JsonObject> StateAsync() => CommandAsync(new JsonObject { ["cmd"] = "state" }, "state");

    public async Task<int> ViolationsAsync() => (int)(await StateAsync().ConfigureAwait(false))["violations"]!;

    public Task<JsonObject> WaitForEventAsync(Func<JsonObject, bool> predicate, TimeSpan? timeout = null)
    {
        TaskCompletionSource<JsonObject> completion;
        lock (_eventsLock)
        {
            var existing = _events.FirstOrDefault(predicate);
            if (existing != null)
                return Task.FromResult(existing);

            completion = new TaskCompletionSource<JsonObject>(TaskCreationOptions.RunContinuationsAsynchronously);
            _waiters.Add((predicate, completion));
        }

        return completion.Task.WithTimeout(timeout ?? DefaultWait, "FakeNative event");
    }

    private void StartReading()
    {
        var thread = new Thread(() =>
        {
            try
            {
                using var reader = new StreamReader(_stream!, new UTF8Encoding(false), false, 65536, true);
                while (true)
                {
                    string? line = reader.ReadLine();
                    if (line == null)
                        break;
                    if (line.Length == 0)
                        continue;

                    if (JsonNode.Parse(line) is not JsonObject e)
                        continue;

                    List<TaskCompletionSource<JsonObject>> matched = new();
                    lock (_eventsLock)
                    {
                        _events.Add(e);
                        for (int i = _waiters.Count - 1; i >= 0; i--)
                        {
                            if (!_waiters[i].Predicate(e))
                                continue;
                            matched.Add(_waiters[i].Completion);
                            _waiters.RemoveAt(i);
                        }
                    }

                    foreach (var completion in matched)
                        completion.TrySetResult(e);
                }
            }
            catch
            {
            }

            ControlClosed.TrySetResult();
        })
        {
            IsBackground = true,
            Name = "fake-native control reader"
        };
        thread.Start();
    }

    public Task WaitForChildExitAsync() => Process.ChildProcess?.WaitForExitAsync() ?? Task.CompletedTask;

    public async ValueTask DisposeAsync()
    {
        try
        {
            Process.Dispose();
            await WaitForChildExitAsync().WithTimeout(TimeSpan.FromSeconds(20)).ConfigureAwait(false);
        }
        catch
        {
        }

        try
        {
            _client?.Dispose();
        }
        catch
        {
        }

        _listener.Stop();
        try
        {
            File.Delete(LogPath);
        }
        catch
        {
        }
    }

    private static string LocateOrBuild()
    {
        string root = TestUtil.RepositoryRoot;
        string project = Path.Combine(root, "tests", "fake-native", "FakeNative.csproj");
        string executable = Path.Combine(root, "tests", "fake-native", "bin", "Debug", "net8.0", OperatingSystem.IsWindows() ? "fake-native.exe" : "fake-native");

        var psi = new ProcessStartInfo("dotnet", $"build \"{project}\" -c Debug -nologo -v:q")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        using var build = System.Diagnostics.Process.Start(psi)!;
        string output = build.StandardOutput.ReadToEnd() + build.StandardError.ReadToEnd();
        build.WaitForExit();
        if (build.ExitCode != 0 || !File.Exists(executable))
            throw new InvalidOperationException($"Building FakeNative failed:\n{output}");

        return executable;
    }
}
