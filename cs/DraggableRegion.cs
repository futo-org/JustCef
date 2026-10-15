namespace JustCef
{
    public readonly record struct DraggableRegion(int X, int Y, int Width, int Height, bool Draggable = true);
}
