using System.Collections.Concurrent;

namespace JustCef.Tests.Infrastructure;

public sealed class SingleThreadSynchronizationContext : SynchronizationContext, IDisposable
{
    private readonly BlockingCollection<(SendOrPostCallback Callback, object? State)> _queue = new();
    private readonly Thread _thread;

    public SingleThreadSynchronizationContext()
    {
        _thread = new Thread(Run)
        {
            IsBackground = true,
            Name = "single-thread context"
        };
        _thread.Start();
    }

    public int ThreadId => _thread.ManagedThreadId;

    public override void Post(SendOrPostCallback d, object? state)
    {
        try
        {
            _queue.Add((d, state));
        }
        catch (InvalidOperationException)
        {
        }
    }

    public override void Send(SendOrPostCallback d, object? state) => throw new NotSupportedException();

    public Task<T> RunAsync<T>(Func<T> func)
    {
        var completion = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        Post(_ =>
        {
            try
            {
                completion.TrySetResult(func());
            }
            catch (Exception e)
            {
                completion.TrySetException(e);
            }
        }, null);
        return completion.Task;
    }

    private void Run()
    {
        SetSynchronizationContext(this);
        foreach (var (callback, state) in _queue.GetConsumingEnumerable())
        {
            try
            {
                callback(state);
            }
            catch
            {
            }
        }
    }

    public void Dispose() => _queue.CompleteAdding();
}
