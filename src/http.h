#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// An HTTP(S) GET on a worker thread, answered on the main loop: rapport's
// rap::sys::http_get (WinHTTP on Windows, libcurl elsewhere).
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
