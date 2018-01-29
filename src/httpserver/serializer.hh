#ifndef SERIALIZER_HH
#define SERIALIZER_HH

#include <condition_variable>
#include <mutex>
#include <queue>
#include <set>

class Serializer {
private:
  std::mutex m_;
  std::condition_variable cv_;
  std::unique_lock<std::mutex> lk_;

  std::queue<std::string> high_priorities_;
  std::set<std::string> prefetch_resources_;

  // Keeps track of the request ID of the next low priority request.
  std::mutex access_guard_;
  int new_low_priority_req_id_;
  int next_low_priority_req_;
  std::map<std::int, std::string> low_priorities_;

  // Populates the prefetch_resources set.
  void get_prefetch_resources(std::set<std::string> prefetch_resources,
                              std::string prefetch_resources_filename);

public:
  Serializer(std::string prefetch_resources_filename);

  // Registers the request and blocks until this URL is next in the queue.
  void register_request(const std::string &url);

  // Finishes the request and proceed to the next request.
  void send_request_complete();
};

#endif /* SERIALIZER_HH */
