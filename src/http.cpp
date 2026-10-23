#include "http.h"

#include "platform.h"

#include <rapport/sys.h>

#include <atomic>
#include <thread>

namespace
{

struct Job
{
    std::string url;
    HttpHeaders headers;
    std::function<void(const HttpResponse&)> done;
    std::atomic<bool> cancelled{false};
    HttpResponse response;
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
                                [job](int* p)
                                {
                                    job->cancelled = true;
                                    delete p;
                                });
    std::thread(
        [job]
        {
            const rap::sys::http_response r = rap::sys::http_get(job->url, {.headers = job->headers, .cancelled = &job->cancelled});
            job->response = {r.status, r.body, r.error};
            platform::post_main(
                [job]
                {
                    if (!job->cancelled)
                        job->done(job->response);
                });
        })
        .detach();
    return token;
}
