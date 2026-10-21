#include "http.h"

#include "platform.h"

#include <gio/gio.h>

#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{

constexpr size_t kMaxResponse = 4 << 20;
constexpr guint kTimeoutSeconds = 15;

struct Url
{
    bool tls = false;
    std::string host;
    guint16 port = 0;
    std::string target;  // path and query
};

bool parse_url(const std::string& url, Url& out)
{
    std::string rest;
    if (url.rfind("https://", 0) == 0)
    {
        out.tls = true;
        rest = url.substr(8);
    }
    else if (url.rfind("http://", 0) == 0)
        rest = url.substr(7);
    else
        return false;
    const size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    out.target = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t colon = authority.rfind(':');
    out.port = out.tls ? 443 : 80;
    if (colon != std::string::npos && authority.find(']') == std::string::npos)
    {
        out.port = guint16(std::atoi(authority.c_str() + colon + 1));
        authority.resize(colon);
    }
    out.host = authority;
    return !out.host.empty() && out.port != 0;
}

// Undo `Transfer-Encoding: chunked`. False if the body is malformed.
bool dechunk(const std::string& in, std::string& out)
{
    size_t pos = 0;
    while (pos < in.size())
    {
        const size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos)
            return false;
        const size_t len = std::strtoul(in.c_str() + pos, nullptr, 16);
        pos = eol + 2;
        if (len == 0)
            return true;
        if (pos + len > in.size())
            return false;
        out.append(in, pos, len);
        pos += len + 2;  // the chunk's trailing CRLF
    }
    return false;
}

bool header_is(const std::string& headers, const char* name, const char* value)
{
    // Case-insensitive search for "\r\nname: value" (good enough for one header).
    gchar* lower = g_ascii_strdown(headers.c_str(), -1);
    gchar* needle = g_ascii_strdown((std::string("\r\n") + name + ": " + value).c_str(), -1);
    const bool found = std::strstr(lower, needle) != nullptr;
    g_free(lower);
    g_free(needle);
    return found;
}

HttpResponse fetch(const std::string& url, const HttpHeaders& headers, GCancellable* cancel)
{
    HttpResponse r;
    Url u;
    if (!parse_url(url, u))
    {
        r.error = "unsupported URL";
        return r;
    }
    GSocketClient* client = g_socket_client_new();
    g_socket_client_set_tls(client, u.tls);
    g_socket_client_set_timeout(client, kTimeoutSeconds);
    GError* err = nullptr;
    GSocketConnection* conn = g_socket_client_connect_to_host(client, u.host.c_str(), u.port, cancel, &err);
    g_object_unref(client);
    if (!conn)
    {
        r.error = err ? err->message : "cannot connect";
        g_clear_error(&err);
        return r;
    }

    std::string request = "GET " + u.target + " HTTP/1.1\r\nHost: " + u.host + "\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    for (const auto& [name, value] : headers)
        request += name + ": " + value + "\r\n";
    request += "\r\n";
    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    std::string raw;
    if (g_output_stream_write_all(out, request.data(), request.size(), nullptr, cancel, &err))
    {
        GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
        char buf[16384];
        for (;;)
        {
            const gssize n = g_input_stream_read(in, buf, sizeof buf, cancel, &err);
            if (n <= 0)
                break;
            raw.append(buf, size_t(n));
            if (raw.size() > kMaxResponse)
                break;
        }
    }
    if (err)
    {
        r.error = err->message;
        g_clear_error(&err);
    }
    g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
    g_object_unref(conn);

    // "HTTP/1.1 200 OK\r\n" headers "\r\n\r\n" body
    const size_t end = raw.find("\r\n\r\n");
    if (end == std::string::npos || raw.rfind("HTTP/1.", 0) != 0 || raw.size() < 12)
    {
        if (r.error.empty())
            r.error = "malformed response";
        return r;
    }
    r.status = std::atoi(raw.c_str() + 9);
    const std::string head = raw.substr(0, end + 2);
    std::string body = raw.substr(end + 4);
    if (header_is(head, "transfer-encoding", "chunked"))
    {
        std::string plain;
        if (!dechunk(body, plain))
        {
            r.status = 0;
            r.error = "malformed chunked body";
            return r;
        }
        body = std::move(plain);
    }
    r.body = std::move(body);
    r.error.clear();
    return r;
}

struct Job
{
    std::string url;
    HttpHeaders headers;
    std::function<void(const HttpResponse&)> done;
    GCancellable* cancel = g_cancellable_new();
    HttpResponse response;
    ~Job() { g_object_unref(cancel); }
};

}  // namespace

std::shared_ptr<void> http_get(const std::string& url, const HttpHeaders& headers, std::function<void(const HttpResponse&)> done)
{
    auto job = std::make_shared<Job>();
    job->url = url;
    job->headers = headers;
    job->done = std::move(done);
    // The token: dropping it cancels. The worker holds the job itself.
    std::shared_ptr<void> token(new int(0),
                                [cancel = (GCancellable*)g_object_ref(job->cancel)](int* p)
                                {
                                    g_cancellable_cancel(cancel);
                                    g_object_unref(cancel);
                                    delete p;
                                });
    std::thread(
        [job]
        {
            job->response = fetch(job->url, job->headers, job->cancel);
            platform::post_main(
                [job]
                {
                    if (!g_cancellable_is_cancelled(job->cancel))
                        job->done(job->response);
                });
        })
        .detach();
    return token;
}
