using Vmf.ManagedCoverage;

using var bytes = new MemoryStream();
Console.OpenStandardInput().CopyTo(bytes);
TestCase(bytes.ToArray());
EdgeCoverage.WriteTo(args[0]);

static void TestCase(ReadOnlySpan<byte> input)
{
    if (input.SequenceEqual(new byte[] { 0, 255 }))
        Console.WriteLine("first");
    else
        Console.WriteLine("second");
}
