using JustCef.Tests.Infrastructure;
using Xunit;

namespace JustCef.Tests.Unit;

[Trait("Category", "Unit")]
public class LoadingStateTests : IDisposable
{
    private readonly JustCefProcess _process = new();

    public void Dispose() => _process.Dispose();

    private JustCefWindow Window(bool isLoading) => new(_process, 1, null, null, null, isLoading);

    [Fact]
    public void InitialStateFollowsUrl()
    {
        Assert.False(Window(true).WaitUntilLoadedAsync().IsCompleted);
        Assert.True(Window(false).WaitUntilLoadedAsync().IsCompleted);
    }

    [Fact]
    public async Task WaitUntilLoadedCompletesOnLoadingFalse()
    {
        var window = Window(true);
        var wait = window.WaitUntilLoadedAsync();
        window.InvokeOnLoadingStateChanged(true, false, false);
        Assert.False(wait.IsCompleted);
        window.InvokeOnLoadingStateChanged(false, true, false);
        await wait.WithTimeout(TimeSpan.FromSeconds(5));
        Assert.True(window.WaitUntilLoadedAsync().IsCompleted);
    }

    [Fact]
    public async Task NavigationIgnoresFalseBeforeStart()
    {
        var window = Window(true);
        var navigation = window.BeginNavigation();
        window.MarkNavigationSent(navigation);
        window.InvokeOnLoadingStateChanged(false, false, false);
        Assert.False(navigation.Task.IsCompleted);
        window.InvokeOnLoadingStateChanged(true, false, false);
        Assert.False(navigation.Task.IsCompleted);
        window.InvokeOnLoadingStateChanged(false, false, false);
        await navigation.Task.WithTimeout(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task NavigationIgnoresTheLoadThatWasAlreadyRunning()
    {
        var window = Window(true);
        window.InvokeOnLoadingStateChanged(true, false, false);
        var navigation = window.BeginNavigation();
        window.InvokeOnFrameLoadStart("main", true, "https://previous/");
        window.InvokeOnLoadingStateChanged(false, false, false);
        Assert.False(navigation.Task.IsCompleted);

        window.MarkNavigationSent(navigation);
        window.InvokeOnLoadingStateChanged(true, false, false);
        window.InvokeOnFrameLoadStart("main", true, "https://next/");
        window.InvokeOnLoadingStateChanged(false, false, false);
        await navigation.Task.WithTimeout(TimeSpan.FromSeconds(5));
    }


    [Fact]
    public async Task MainFrameErrorFailsNavigationExceptAborted()
    {
        var window = Window(false);
        var navigation = window.BeginNavigation();
        window.MarkNavigationSent(navigation);
        window.InvokeOnFrameLoadError("main", true, -3, "aborted", "https://a/");
        window.InvokeOnFrameLoadError("sub", false, -105, "sub frame", "https://b/");
        Assert.False(navigation.Task.IsCompleted);
        window.InvokeOnFrameLoadError("main", true, -105, "net::ERR_NAME_NOT_RESOLVED", "https://c/");
        var exception = await Assert.ThrowsAsync<InvalidOperationException>(() => navigation.Task.WithTimeout(TimeSpan.FromSeconds(5)));
        Assert.Contains("ERR_NAME_NOT_RESOLVED", exception.Message);
    }

    [Fact]
    public async Task CloseFailsWaiters()
    {
        var window = Window(true);
        var wait = window.WaitUntilLoadedAsync();
        var navigation = window.BeginNavigation();
        window.MarkNavigationSent(navigation);
        window.InvokeOnClose();
        await Assert.ThrowsAsync<InvalidOperationException>(() => wait.WithTimeout(TimeSpan.FromSeconds(5)));
        await Assert.ThrowsAsync<InvalidOperationException>(() => navigation.Task.WithTimeout(TimeSpan.FromSeconds(5)));
        Assert.True(window.BeginNavigation().Task.IsFaulted);
        Assert.True(window.WaitForExitAsync().IsCompleted);
    }

    [Fact]
    public async Task FailedLoadUrlClearsLoading()
    {
        var window = Window(false);
        await Assert.ThrowsAnyAsync<Exception>(() => window.NavigateAsync("https://a/"));
        Assert.True(window.WaitUntilLoadedAsync().IsCompleted);
    }

    [Fact]
    public async Task EventsAreRaisedOffTheCallingThreadInOrder()
    {
        var window = Window(false);
        var log = new List<(string Name, int Thread)>();
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        window.OnFocused += () => log.Add(("focused", Environment.CurrentManagedThreadId));
        window.OnLoadingStateChanged += info => log.Add(($"loading:{info.IsLoading}", Environment.CurrentManagedThreadId));
        window.OnClose += () =>
        {
            log.Add(("close", Environment.CurrentManagedThreadId));
            done.TrySetResult();
        };

        window.InvokeOnFocused();
        window.InvokeOnLoadingStateChanged(true, false, false);
        window.InvokeOnLoadingStateChanged(false, false, false);
        window.InvokeOnClose();
        await done.Task.WithTimeout(TimeSpan.FromSeconds(5));

        Assert.Equal(new[] { "focused", "loading:True", "loading:False", "close" }, log.Select(e => e.Name));
        Assert.DoesNotContain(log, e => e.Thread == Environment.CurrentManagedThreadId);
    }
}
