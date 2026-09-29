using System.Net;
using System.Text;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public sealed class NetworkImportCompatibilityTests
{
    [Fact]
    public async Task SessionListSkipsExplicitLegacyEntriesWithoutSessionStartAndAcceptsNullOptionalFields()
    {
        using var service = new NetworkImportService("http://analyzer/", handler: new JsonHandler(request => request.RequestUri!.AbsolutePath switch {
            "/api/v1/sessions" => """{"sessions":[{"folder":"legacy","session_id":"legacy","state":"CLOSED","schema_version":"","events":null,"valid_raw_bytes":"","duration_us":"","start_utc":null,"threshold":null,"has_session_start":false},{"folder":"current","session_id":"current","state":"CLOSED","schema_version":"knx-long-session-1.1","events":0,"valid_raw_bytes":"1024","duration_us":"1000","start_utc":null,"has_session_start":true},{"folder":"old-api","session_id":"old-api","state":"CLOSED","schema_version":"knx-long-session-1.0","events":0,"valid_raw_bytes":"0","duration_us":"1000"}]}""",
            _ => throw new InvalidOperationException(request.RequestUri.AbsolutePath)
        }));
        var sessions = await service.ListSessionsAsync();
        Assert.Equal(["current", "old-api"], sessions.Select(x => x.Folder));
    }

    [Fact]
    public async Task NullSessionStartProducesSpecificCompatibilityError()
    {
        using var service = new NetworkImportService("http://analyzer/", handler: new JsonHandler(request => request.RequestUri!.AbsolutePath switch {
            "/api/v1/sessions/legacy" => """{"session_start":null,"test_result":null,"files":{}}""",
            _ => throw new InvalidOperationException(request.RequestUri.AbsolutePath)
        }));
        typeof(NetworkImportService).GetProperty(nameof(NetworkImportService.Analyzer))!.SetValue(service,
            new AnalyzerInfo("test", "test", "test", "IDLE", "127.0.0.1", -50, true, true));
        var info = new NetworkSessionInfo("legacy", "legacy", "CLOSED", "", null, "", "");
        var error = await Assert.ThrowsAsync<InvalidDataException>(() => service.ImportMetadataAsync(info));
        Assert.Equal("Session legacy has no usable session_start metadata.", error.Message);
    }

    private sealed class JsonHandler(Func<HttpRequestMessage, string> response) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(response(request), Encoding.UTF8, "application/json") });
    }
}
