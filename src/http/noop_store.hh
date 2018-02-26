#ifndef NOOP_STORE_HH
#define NOOP_STORE_HH

#include "backing_store.hh"

#include "address.hh"
#include "http_response.hh"

class NoopStore : public HTTPBackingStore {
public:
  NoopStore();
  void save(const HTTPResponse &response,
            const Address &server_address) override;
};

#endif /* NOOP_STORE_HH */
