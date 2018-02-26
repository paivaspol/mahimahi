#include "noop_store.hh"

using namespace std;

NoopStore::NoopStore() {}

void NoopStore::save(const HTTPResponse &response,
                     const Address &server_address) {
  // Do random things, so that compiler does not complain.
  response.request();
  server_address.str();
}
