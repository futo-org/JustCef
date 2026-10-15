namespace JustCef
{
    public enum CaptionButton : byte { Minimize = 1, Maximize = 2, Close = 3 }

    public readonly record struct CaptionButtonRegion(int X, int Y, int Width, int Height, CaptionButton Button);
}
