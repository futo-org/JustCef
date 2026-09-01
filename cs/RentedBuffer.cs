using System.Buffers;
using System.Threading;

namespace JustCef
{
    public readonly record struct RentedBufferSnapshot(int ActiveRentals, int PeakActiveRentals, long TotalRentals, long TotalReturns);

    public static class RentedBufferDiagnostics
    {
        private static int _activeRentals;
        private static int _peakActiveRentals;
        private static long _totalRentals;
        private static long _totalReturns;

        public static RentedBufferSnapshot GetSnapshot()
        {
            return new RentedBufferSnapshot(
                Volatile.Read(ref _activeRentals),
                Volatile.Read(ref _peakActiveRentals),
                Interlocked.Read(ref _totalRentals),
                Interlocked.Read(ref _totalReturns));
        }

        internal static void OnRent()
        {
            int active = Interlocked.Increment(ref _activeRentals);
            Interlocked.Increment(ref _totalRentals);

            while (true)
            {
                int currentPeak = Volatile.Read(ref _peakActiveRentals);
                if (active <= currentPeak)
                    break;

                if (Interlocked.CompareExchange(ref _peakActiveRentals, active, currentPeak) == currentPeak)
                    break;
            }
        }

        internal static void OnReturn()
        {
            Interlocked.Decrement(ref _activeRentals);
            Interlocked.Increment(ref _totalReturns);
        }
    }

    internal sealed class RentedBuffer<T> : IDisposable
    {
        private ArrayPool<T>? _pool;
        private T[]? _buffer;

        public T[] Buffer => _buffer ?? throw new ObjectDisposedException(nameof(RentedBuffer<T>));
        public int Length { get; }

        public RentedBuffer(ArrayPool<T> pool, int length)
        {
            _pool = pool;
            _buffer = pool.Rent(length);
            Length = length;
            RentedBufferDiagnostics.OnRent();
        }

        public void Dispose()
        {
            var pool = Interlocked.Exchange(ref _pool, null);
            var buffer = Interlocked.Exchange(ref _buffer, null);
            if (pool == null || buffer == null)
                return;

            pool.Return(buffer);
            RentedBufferDiagnostics.OnReturn();
        }
    }
}
