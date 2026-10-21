#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// A minimal HTTP(S) GET over GIO sockets: one request, `Connection: close`,
// no redirects or compression. TLS comes from GIO's backend (glib-networking,
// present on desktop systems); without it, https requests just fail.
struct HttpResponse
{
    int status = 0;  // 0: no response (connection, TLS or parse failure)
    std::string body;
    std::string error;
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// Runs on a worker thread; `done` is called on the main loop. Dropping the
// returned token before then cancels the request and `done` is never called.
std::shared_ptr<void> http_get(const std::string& url, const HttpHeaders& headers, std::function<void(const HttpResponse&)> done);
