using Vmf.ManagedCoverage;

using var bytes = new MemoryStream();
Console.OpenStandardInput().CopyTo(bytes);
File.WriteAllBytes(args[1], []);
try
{
    TestCase(bytes.ToArray());
}
catch (FormatException)
{
    // This fixture treats malformed input as an expected rejection.
}
EdgeCoverage.WriteTo(args[0]);

static void TestCase(ReadOnlySpan<byte> input)
{
    if (input.SequenceEqual(new byte[] { (byte)'R', 0 }))
        throw new FormatException("Expected input rejection");
    if (input.SequenceEqual(new byte[] { (byte)'E', 0 }))
        throw new InvalidOperationException("Unexpected testcase exception");
    if (input.SequenceEqual(new byte[] { (byte)'T', 0 }))
        Thread.Sleep(Timeout.Infinite);
    if (input.SequenceEqual(new byte[] { 0, 255 }))
        Console.WriteLine("first");
    else
        Console.WriteLine("second");
}
