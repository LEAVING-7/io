#include "io/Executor.hpp"
#include "io/async/Task.hpp"
#include "io/async/tcp.hpp"
using namespace std::chrono_literals;
// int main()
// {
//   auto executor = Executor {};
//   auto reactor = io::Reactor();
//   LOG_INFO("before blockOn");
//   auto future = executor.blockOn(
//       [](io::Reactor& reactor, Executor& e) -> Task<> {
//         LOG_INFO("hello world");
//         auto listener = io::async::TcpListener::Bind(&reactor, io::SocketAddrV4 {io::ANY, 2333});
//         if (!listener) {
//           LOG_ERROR("bind failed: {}", listener.error().message());
//           co_return;
//         } else {
//           LOG_INFO("bind success");
//         }
//         for (int i = 0;; i++) {
//           auto stream = co_await listener->accept();
//           if (!stream) {
//             LOG_ERROR("accept failed: {}", stream.error().message());
//             co_return;
//           } else {
//             LOG_INFO("accept success, stream fd: {}", stream.value().socket().raw());
//           }

//           e.spawn([](io::async::TcpStream stream, Executor& e) mutable -> Task<> {
//             LOG_INFO("spawned");
//             char buf[] = "HTTP/1.1 200 OK\r\n"
//                          "Content-Type: text/html; charset=UTF-8\r\n"
//                          "Connection: keep-alive\r\n"
//                          "Content-Length: 11\r\n"
//                          "\r\n"
//                          "hello world";
//             // LOG_INFO("begin read");
//             auto recvBuf = std::array<u8, 1024> {};
//             LOG_WARN("before read");
//             auto rlen = co_await stream.read(recvBuf.data(), recvBuf.size());
//             LOG_WARN("after read");
//             if (rlen) {
//             } else {
//               co_return;
//             }
//             auto len = co_await stream.write(buf, std::size(buf));
//             // LOG_INFO("after read");
//             if (!len) {
//               LOG_ERROR("read failed: {}", len.error().message());
//               co_return;
//             } else {
//               // LOG_INFO("read {} bytes", len.value());
//             }
//           }(std::move(stream).value(), e));
//         }
//         LOG_CRITICAL("main task exit");
//         co_return;
//       }(reactor, executor),
//       reactor);
//   while (future.wait_for(0s) != std::future_status::ready && !executor.isEmpty()) {
//     reactor.lock().react(std::nullopt, executor);
//   }
//   executor.waitEmpty();
//   LOG_INFO("main thread exit");
// }
std::atomic_size_t gCount = 0;

#include <random>
int main()
{
  auto e = io::MutilThreadExecutor {8};
  auto r = io::Reactor {};
  auto now = std::chrono::steady_clock::now();
  e.block(
      [](io::MutilThreadExecutor& e, io::Reactor& r) -> Task<> {
        for (int i = 0; i < 30; i++) {
          e.spawn(
              [](io::Reactor& r, io::MutilThreadExecutor& e, int i) -> Task<> {
                co_await r.sleep(4s);
                e.spawn(
                    [](io::Reactor& r) -> Task<> {
                      co_await r.sleep(1s);
                      gCount += 1;
                      co_return;
                    }(r),
                    r);
                co_return;
              }(r, e, i),
              r);
        }
        co_return;
      }(e, r),
      r);
  auto done = std::chrono::steady_clock::now();
  LOG_INFO("elapsed: {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(done - now).count());
  LOG_INFO("main thread end, with return value: {}", gCount);
}
