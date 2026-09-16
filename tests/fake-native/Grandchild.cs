using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;

namespace FakeNative;

public static class Grandchild
{
    private const uint HandleFlagInherit = 1;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);

    public static int Spawn(int seconds, Options options)
    {
        if (OperatingSystem.IsWindows())
        {
            foreach (var h in new[] { options.ParentToChild, options.ChildToParent })
            {
                if (h != null)
                    SetHandleInformation(new IntPtr(long.Parse(h, CultureInfo.InvariantCulture)), HandleFlagInherit, HandleFlagInherit);
            }
        }

        var self = Environment.ProcessPath ?? throw new InvalidOperationException("process path is unknown");
        var psi = new ProcessStartInfo(self) { UseShellExecute = false };
        if (Path.GetFileNameWithoutExtension(self).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
        {
            var assembly = typeof(Grandchild).Assembly.Location;
            psi.ArgumentList.Add(assembly);
        }
        psi.ArgumentList.Add("--fake-sleep");
        psi.ArgumentList.Add(seconds.ToString(CultureInfo.InvariantCulture));
        using var process = Process.Start(psi) ?? throw new InvalidOperationException("grandchild did not start");
        return process.Id;
    }
}
