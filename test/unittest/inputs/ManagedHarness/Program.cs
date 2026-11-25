using Vmf.ManagedCoverage;

if (args.Length == 3 && args[2] == ReusableStdinHarness.Argument)
{
    if (File.Exists(args[1]))
    {
        if (File.ReadAllText(args[1]) == "wait")
            Thread.Sleep(Timeout.Infinite);
        return 1;
    }
    int executions = 0;
    bool failReset = false;
    try
    {
        return ReusableStdinHarness.Run(args[0], stream =>
        {
            using var testcase = new MemoryStream();
            stream.CopyTo(testcase);
            byte[] data = testcase.ToArray();
            executions++;
            if (data.AsSpan().SequenceEqual(new byte[] { (byte)'P', 0 }) && executions != 2)
                throw new InvalidOperationException("Expected an already running harness");
            if (data.AsSpan().SequenceEqual(new byte[] { (byte)'F', 0 }) && executions != 1)
                throw new InvalidOperationException("Expected a replacement harness");
            if (data.Length == 2 && data[1] == 0 && (data[0] == (byte)'N' || data[0] == (byte)'W'))
            {
                File.WriteAllText(args[1], data[0] == (byte)'W' ? "wait" : "exit");
                Environment.Exit(2);
            }
            failReset = data.AsSpan().SequenceEqual(new byte[] { (byte)'S', 0 });
            RunTestCase(data);
        }, () =>
        {
            if (failReset)
                throw new InvalidOperationException("Testcase reset failed");
        });
    }
    catch (Exception error)
    {
        Console.Error.WriteLine(error);
        return 1;
    }
}

using var bytes = new MemoryStream();
Console.OpenStandardInput().CopyTo(bytes);
File.WriteAllBytes(args[1], []);
if (bytes.ToArray().AsSpan().SequenceEqual(new byte[] { (byte)'P', 0 }))
    return 1;
try
{
    RunTestCase(bytes.ToArray());
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    return 1;
}
EdgeCoverage.WriteTo(args[0]);
return 0;

static void RunTestCase(ReadOnlySpan<byte> input)
{
    try
    {
        TestCase(input);
    }
    catch (FormatException)
    {
        // This fixture treats malformed input as an expected rejection.
    }
}

static void TestCase(ReadOnlySpan<byte> input)
{
    if (input.SequenceEqual(new byte[] { (byte)'R', 0 }))
        throw new FormatException("Expected input rejection");
    if (input.SequenceEqual(new byte[] { (byte)'E', 0 }))
        throw new InvalidOperationException("Unexpected testcase exception");
    if (input.SequenceEqual(new byte[] { (byte)'T', 0 }))
        Thread.Sleep(Timeout.Infinite);
    if (input.SequenceEqual(new byte[] { (byte)'D', 0 }))
        Environment.Exit(2);
    if (input.SequenceEqual(new byte[] { 0, 255 }))
        Console.WriteLine("first");
    else
        Console.WriteLine("second");
}
