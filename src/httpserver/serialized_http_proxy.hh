#ifndef SERIALIZED_HTTP_PROXY_HH
#define SERIALIZED_HTTP_PROXY_HH

#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <vector>

#include "http_proxy.hh"

class HTTPProxy;
class Serializer;

class SerializedHTTPProxy : public HTTPProxy {
private:
  // Maps from the prefetch resource URL to the resource type.
  std::map<std::string, std::string> prefetch_resources_;

  // Save the escaped URL computation.
  std::map<std::string, std::string> escaped_urls_;

  // To preserve the ordering of the prefetch resources.
  std::vector<std::string> prefetch_resources_order_;

  std::string page_url_;

  template <class SocketType>
  void serialized_loop(SocketType &server, SocketType &client,
                       HTTPBackingStore &backing_store);

  void handle_tcp(HTTPBackingStore &backing_store) override;

  // Populates the prefetch_resources set.
  void
  get_prefetch_resources(std::map<std::string, std::string> &prefetch_resources,
                         std::string prefetch_resources_filename);

  std::string generate_preload_header_str(void);

  /* serialization variables and methods */
  std::mutex m_;
  std::condition_variable cv_;

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
  // // Registers the request and blocks until this URL is next in the queue.
  // void register_request(const std::string &url, bool is_high_priority);

  // // Finishes the request and proceed to the next request.
  // void send_request_complete();

public:
  SerializedHTTPProxy(const Address &listener_addr,
                      const std::string &prefetch_urls_filename,
                      const std::string &request_order_filename,
                      const std::string &page_url);
};

#endif /* SERIALIZED_HTTP_PROXY_HH */
