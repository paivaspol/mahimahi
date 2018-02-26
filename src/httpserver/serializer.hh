#ifndef SERIALIZER_HH
#define SERIALIZER_HH

#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>

class Serializer {
private:
  std::mutex m_;
  std::condition_variable cv_;
  std::unique_lock<std::mutex> lk_;

  std::queue<std::string> high_priorities_;

  std::map<std::string, int> request_order_;

  // Keeps track of the request ID of the next low priority request.
  std::mutex access_guard_;
  int new_low_priority_req_id_;
  int next_low_priority_req_;
  std::map<int, std::string> low_priorities_;

  void get_request_order(std::map<std::string, int> &request_order,
                         std::string request_order_filename);

  void reprioritize(const std::string &current_url);

public:
  Serializer(std::string request_order_filename);

  // Registers the request and blocks until this URL is next in the queue.
  void register_request(const std::string &url, bool is_high_priority);

  // Finishes the request and proceed to the next request.
  void send_request_complete();
};

#endif /* SERIALIZER_HH */
