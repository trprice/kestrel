#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kestrel/net/http_client.hpp"
#include "test_helpers.hpp"

namespace kestrel::net {
namespace {

// A keep-alive HTTP/1.1 server on the loopback interface. It counts the
// connections it accepts and records each request's method, target and body,
// and answers every request with 200 "ok".
class LoopbackServer {
   public:
    struct Seen {
        std::string method;
        std::string target;
        std::string body;
    };

    LoopbackServer() {
        mListen = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(mListen, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        ::listen(mListen, 8);
        socklen_t len = sizeof addr;
        ::getsockname(mListen, reinterpret_cast<sockaddr*>(&addr), &len);
        mPort = ntohs(addr.sin_port);
        mAccepter = std::thread([this] { AcceptLoop(); });
    }

    ~LoopbackServer() {
        ::shutdown(mListen, SHUT_RDWR);
        ::close(mListen);
        mAccepter.join();
        std::lock_guard lock(mMutex);
        for (int fd : mConnections) {
            ::shutdown(fd, SHUT_RDWR);
        }
        for (auto& t : mServers) {
            t.join();
        }
        for (int fd : mConnections) {
            ::close(fd);
        }
    }

    std::string Url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(mPort) + path;
    }
    int Accepted() const { return mAccepted; }
    std::vector<Seen> Requests() {
        std::lock_guard lock(mMutex);
        return mSeen;
    }

   private:
    void AcceptLoop() {
        for (;;) {
            const int fd = ::accept(mListen, nullptr, nullptr);
            if (fd < 0) {
                return;  // shut down
            }
            ++mAccepted;
            std::lock_guard lock(mMutex);
            mConnections.push_back(fd);
            mServers.emplace_back([this, fd] { Serve(fd); });
        }
    }

    void Serve(int fd) {
        std::string buffer;
        char chunk[4096];
        for (;;) {
            std::size_t end;
            while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
                const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
                if (n <= 0) {
                    return;
                }
                buffer.append(chunk, static_cast<std::size_t>(n));
            }
            const std::string head = buffer.substr(0, end);
            buffer.erase(0, end + 4);
            std::size_t length = 0;
            if (auto at = head.find("Content-Length: "); at != std::string::npos) {
                length = std::strtoul(head.c_str() + at + 16, nullptr, 10);
            }
            while (buffer.size() < length) {
                const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
                if (n <= 0) {
                    return;
                }
                buffer.append(chunk, static_cast<std::size_t>(n));
            }
            Seen seen;
            const auto space = head.find(' ');
            seen.method = head.substr(0, space);
            seen.target = head.substr(space + 1, head.find(' ', space + 1) - space - 1);
            seen.body = buffer.substr(0, length);
            buffer.erase(0, length);
            {
                std::lock_guard lock(mMutex);
                mSeen.push_back(std::move(seen));
            }
            const std::string reply =
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            ::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
        }
    }

    int mListen = -1;
    int mPort = 0;
    std::atomic<int> mAccepted{0};
    std::thread mAccepter;
    std::mutex mMutex;  // guards the members below
    std::vector<int> mConnections;
    std::vector<std::thread> mServers;
    std::vector<Seen> mSeen;
};

HttpRequest Get(const std::string& url) {
    HttpRequest r;
    r.url = url;
    return r;
}

TEST(CprHttpClient, ReusesTheConnectionForLaterRequests) {
    LoopbackServer server;
    CprHttpClient client;
    for (int i = 0; i < 5; ++i) {
        auto r = client.Send(Get(server.Url("/api/v3/time")));
        ASSERT_OK(r);
        EXPECT_EQ(r->body, "ok");
    }
    EXPECT_EQ(server.Accepted(), 1);
    EXPECT_EQ(server.Requests().size(), 5u);
}

TEST(CprHttpClient, AReusedSessionCarriesNothingFromTheLastRequest) {
    LoopbackServer server;
    CprHttpClient client;

    HttpRequest post;
    post.method = HttpMethod::kPost;
    post.url = server.Url("/api/v3/order");
    post.raw_query = "symbol=ZECUSDT&signature=abc";
    post.body = "payload";
    ASSERT_OK(client.Send(post));

    HttpRequest get = Get(server.Url("/api/v3/openOrders"));
    get.query = {{"symbol", "ZECUSDT"}};
    ASSERT_OK(client.Send(get));

    HttpRequest del;
    del.method = HttpMethod::kDelete;
    del.url = server.Url("/api/v3/order");
    del.raw_query = "orderId=7";
    ASSERT_OK(client.Send(del));

    const auto seen = server.Requests();
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0].method, "POST");
    EXPECT_EQ(seen[0].target, "/api/v3/order?symbol=ZECUSDT&signature=abc");
    EXPECT_EQ(seen[0].body, "payload");
    EXPECT_EQ(seen[1].method, "GET");
    EXPECT_EQ(seen[1].target, "/api/v3/openOrders?symbol=ZECUSDT");
    EXPECT_EQ(seen[1].body, "");  // the POST's body is not sent again
    EXPECT_EQ(seen[2].method, "DELETE");
    EXPECT_EQ(seen[2].target, "/api/v3/order?orderId=7");  // no old query
    EXPECT_EQ(seen[2].body, "");
    EXPECT_EQ(server.Accepted(), 1);
}

TEST(CprHttpClient, ConcurrentRequestsEachGetTheirOwnSession) {
    LoopbackServer server;
    CprHttpClient client;
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20; ++i) {
                if (client.Send(Get(server.Url("/x")))) {
                    ++ok;
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(ok.load(), 60);
    // At most one connection per thread at a time, reused after that.
    EXPECT_LE(server.Accepted(), 3);
}

TEST(CprHttpClient, ATransportFailureIsReportedAndNotReused) {
    CprHttpClient client(std::chrono::milliseconds(500));
    // Nothing listens on port 1.
    auto r = client.Send(Get("http://127.0.0.1:1/x"));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().category, types::ErrorCategory::kNetwork);

    // The client still works afterwards, on a fresh connection.
    LoopbackServer server;
    ASSERT_OK(client.Send(Get(server.Url("/x"))));
    EXPECT_EQ(server.Accepted(), 1);
}

}  // namespace
}  // namespace kestrel::net
