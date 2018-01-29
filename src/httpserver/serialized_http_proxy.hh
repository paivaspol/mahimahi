#ifndef SERIALIZED_HTTP_PROXY_HH
#define SERIALIZED_HTTP_PROXY_HH

#include <map>
#include <vector>

#include "http_proxy.hh"

#include "serializer.hh"

class HTTPProxy;
class Serializer;

class SerializedHTTPProxy : public HTTPProxy {
private:
  Serializer serializer_;

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

public:
  SerializedHTTPProxy(const Address &listener_addr,
                      const std::string &prefetch_urls_filename,
                      const std::string &page_url);
};

#endif /* SERIALIZED_HTTP_PROXY_HH */
