#ifndef SERIALIZER_HH
#define SERIALIZER_HH

#include <condition_variable>
#include <mutex>
#include <queue>

class Serializer {
private:
  std::mutex m_;
  std::condition_variable cv_;
  std::unique_lock<std::mutex> lk_;

  std::queue<std::string> request_queue_;
  std::string current_request_url_;

public:
  Serializer();

  // Registers the request and blocks until this URL is next in the queue.
  void register_request(const std::string &url);

  // Finishes the request and proceed to the next request.
  void send_request_complete();
};

#endif /* SERIALIZER_HH */
