namespace JustCef
{
    public class JustCefView : JustCefBrowser
    {
        public JustCefWindow Parent { get; }

        internal JustCefView(JustCefProcess process, int identifier, JustCefWindow parent) : base(process, identifier, false)
        {
            Parent = parent;
        }

        public void SetRequestProxy(Func<JustCefView, IPCRequest, Task<IPCResponse?>>? requestProxy)
            => SetRequestProxyCore(requestProxy == null ? null : request => requestProxy(this, request));

        public void SetRequestModifier(Func<JustCefView, IPCRequest, IPCRequest?>? requestModifier)
            => SetRequestModifierCore(requestModifier == null ? null : request => requestModifier(this, request));
    }
}
