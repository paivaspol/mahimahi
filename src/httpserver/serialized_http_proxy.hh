#ifndef SERIALIZED_HTTP_PROXY_HH
#define SERIALIZED_HTTP_PROXY_HH

#include "http_proxy.hh"

#include "serializer.hh"

class HTTPProxy;
class Serializer;

class SerializedHTTPProxy : public HTTPProxy {
private:
  Serializer serializer_;

  template <class SocketType>
  void serialized_loop(SocketType &server, SocketType &client,
                       HTTPBackingStore &backing_store);

  void handle_tcp(HTTPBackingStore &backing_store) override;

public:
  SerializedHTTPProxy(const Address &listener_addr,
                      const std::string &prefetch_urls_filename);
};

#endif /* SERIALIZED_HTTP_PROXY_HH */
