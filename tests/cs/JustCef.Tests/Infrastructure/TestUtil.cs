using System.Runtime.CompilerServices;

namespace JustCef.Tests.Infrastructure;

public static class TestUtil
{
    public static async Task WithTimeout(this Task task, TimeSpan timeout, [CallerArgumentExpression(nameof(task))] string? expression = null)
    {
        try
        {
            await task.WaitAsync(timeout).ConfigureAwait(false);
        }
        catch (TimeoutException) when (!task.IsCompleted)
        {
            throw new TimeoutException($"Timed out after {timeout} waiting for: {expression}");
        }
    }

    public static async Task<T> WithTimeout<T>(this Task<T> task, TimeSpan timeout, [CallerArgumentExpression(nameof(task))] string? expression = null)
    {
        try
        {
            return await task.WaitAsync(timeout).ConfigureAwait(false);
        }
        catch (TimeoutException) when (!task.IsCompleted)
        {
            throw new TimeoutException($"Timed out after {timeout} waiting for: {expression}");
        }
    }

    public static Task RunScenario(Func<Task> scenario, TimeSpan timeout, [CallerMemberName] string? name = null)
        => Task.Run(scenario).WithTimeout(timeout, $"scenario {name}");

    public static async Task WaitUntil(Func<bool> condition, TimeSpan timeout, string description)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
                throw new TimeoutException($"Timed out after {timeout} waiting until {description}.");
            await Task.Delay(10).ConfigureAwait(false);
        }
    }

    private static int _quietLogs;

    public static void QuietLogs()
    {
        if (Interlocked.Exchange(ref _quietLogs, 1) != 0)
            return;

        var log = Logger.LogCallback;
        Logger.LogCallback = (level, tag, message, ex) =>
        {
            if (level <= LogLevel.Warning)
                log(level, tag, message, ex);
        };
    }

    public static string RepositoryRoot
    {
        get
        {
            var directory = new DirectoryInfo(AppContext.BaseDirectory);
            while (directory != null)
            {
                if (Directory.Exists(Path.Combine(directory.FullName, "cs")) && Directory.Exists(Path.Combine(directory.FullName, "tests")))
                    return directory.FullName;
                directory = directory.Parent;
            }

            throw new InvalidOperationException("Could not locate the repository root.");
        }
    }
}
