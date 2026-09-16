using System.Globalization;
using System.IO.Pipes;

namespace FakeNative;

public sealed class Options
{
    public string? ParentToChild;
    public string? ChildToParent;
    public int? ControlPort;
    public string? LogPath;
    public bool NoReady;
    public uint ReadyVersion = Protocol.Version;
    public bool SelfTest;
    public string? SelfTestPath;
    public int? SleepSeconds;

    public static Options Parse(string[] args)
    {
        var o = new Options();
        for (var i = 0; i < args.Length; i++)
        {
            var arg = args[i];
            var hasValue = i + 1 < args.Length;
            switch (arg)
            {
                case "--parent-to-child" when hasValue:
                    o.ParentToChild = args[++i];
                    break;
                case "--child-to-parent" when hasValue:
                    o.ChildToParent = args[++i];
                    break;
                case "--fake-control-port" when hasValue:
                    o.ControlPort = int.Parse(args[++i], CultureInfo.InvariantCulture);
                    break;
                case "--fake-log" when hasValue:
                    o.LogPath = args[++i];
                    break;
                case "--fake-no-ready":
                    o.NoReady = true;
                    break;
                case "--fake-ready-version" when hasValue:
                    o.ReadyVersion = uint.Parse(args[++i], CultureInfo.InvariantCulture);
                    break;
                case "--fake-sleep" when hasValue:
                    o.SleepSeconds = int.Parse(args[++i], CultureInfo.InvariantCulture);
                    break;
                case "--self-test":
                    o.SelfTest = true;
                    if (hasValue && !args[i + 1].StartsWith("--", StringComparison.Ordinal))
                        o.SelfTestPath = args[++i];
                    break;
            }
        }
        return o;
    }
}

public static class Program
{
    public static int Main(string[] args)
    {
        var options = Options.Parse(args);
        if (options.SleepSeconds is int seconds)
        {
            Thread.Sleep(TimeSpan.FromSeconds(seconds));
            return 0;
        }
        if (options.SelfTest)
            return SelfTest.Run(options.SelfTestPath);
        if (options.ParentToChild == null || options.ChildToParent == null)
        {
            Console.Error.WriteLine("usage: fake-native --parent-to-child <handle> --child-to-parent <handle> [--fake-control-port <port>] [--fake-log <path>] [--fake-no-ready] [--fake-ready-version <n>]");
            Console.Error.WriteLine("       fake-native --self-test [vectors.json]");
            return 2;
        }

        var log = new Logger(options.LogPath);
        log.Write($"start: {string.Join(' ', args)}");
        ControlChannel? control = null;
        if (options.ControlPort is int port)
        {
            try
            {
                control = ControlChannel.Connect(port, log, TimeSpan.FromSeconds(10));
            }
            catch (Exception e)
            {
                log.Write($"control connect failed: {e.Message}");
                Console.Error.WriteLine($"fake-native: control connect to port {port} failed: {e.Message}");
                return 2;
            }
        }

        var input = new AnonymousPipeClientStream(PipeDirection.In, options.ParentToChild);
        var output = new AnonymousPipeClientStream(PipeDirection.Out, options.ChildToParent);
        Host? host = null;
        var loop = new EventLoop(log, e => host?.OnLoopError(e));
        var writer = new PacketWriterThread(output, log);
        host = new Host(options, loop, input, writer, control, log);
        host.Start();
        loop.Run();
        return 0;
    }
}
