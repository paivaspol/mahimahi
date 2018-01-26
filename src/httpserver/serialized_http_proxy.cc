#include <arpa/inet.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <linux/netfilter_ipv4.h>
#include <string>
#include <thread>

#include "address.hh"
#include "backing_store.hh"
#include "bytestream_queue.hh"
#include "event_loop.hh"
#include "exception.hh"
#include "file_descriptor.hh"
#include "http_proxy.hh"
#include "http_request_parser.hh"
#include "http_response_parser.hh"
#include "poller.hh"
#include "secure_socket.hh"
#include "serialized_http_proxy.hh"
#include "serializer.hh"
#include "socket.hh"
#include "system_runner.hh"
#include "temp_file.hh"

using namespace std;
using namespace PollerShortNames;

SerializedHTTPProxy::SerializedHTTPProxy(const Address &listener_addr)
    : HTTPProxy(listener_addr), serializer_() {}

template <class SocketType>
void SerializedHTTPProxy::serialized_loop(SocketType &server,
                                          SocketType &client,
                                          HTTPBackingStore &backing_store) {
  Poller poller;

  HTTPRequestParser request_parser;
  HTTPResponseParser response_parser;

  const Address server_addr = client.original_dest();

  /* poll on original connect socket and new connection socket to ferry packets
   */
  /* responses from server go to response parser */
  poller.add_action(Poller::Action(server, Direction::In,
                                   [&]() {
                                     string buffer = server.read();
                                     response_parser.parse(buffer);
                                     return ResultType::Continue;
                                   },
                                   [&]() { return not client.eof(); }));

  /* requests from client go to request parser */
  poller.add_action(Poller::Action(client, Direction::In,
                                   [&]() {
                                     string buffer = client.read();
                                     request_parser.parse(buffer);
                                     return ResultType::Continue;
                                   },
                                   [&]() { return not server.eof(); }));

  /* completed requests from client are serialized and sent to server */
  poller.add_action(Poller::Action(
      server, Direction::Out,
      [&]() {
        server.write(request_parser.front().str());
        response_parser.new_request_arrived(request_parser.front());
        request_parser.pop();
        return ResultType::Continue;
      },
      [&]() { return not request_parser.empty(); }));

  /* completed responses from server are serialized and sent to client */
  poller.add_action(Poller::Action(
      client, Direction::Out,
      [&]() {
        auto url = response_parser.front().request().get_url();
        serializer_.register_request(url);
        auto start = chrono::system_clock::now();
        auto start_epoch = start.time_since_epoch();
        auto start_seconds =
            chrono::duration_cast<chrono::milliseconds>(start_epoch);
        cout << "URL: " << url << " Start: " << to_string(start_seconds.count())
             << endl;
        client.write(response_parser.front().str());
        serializer_.send_request_complete();
        auto end = chrono::system_clock::now();
        auto end_epoch = end.time_since_epoch();
        auto end_seconds =
            chrono::duration_cast<chrono::milliseconds>(end_epoch);
        cout << "URL: " << url << " End: " << to_string(end_seconds.count())
             << endl;

        backing_store.save(response_parser.front(), server_addr);
        response_parser.pop();
        return ResultType::Continue;
      },
      [&]() { return not response_parser.empty(); }));

  while (true) {
    if (poller.poll(-1).result == Poller::Result::Type::Exit) {
      return;
    }
  }
}

void SerializedHTTPProxy::handle_tcp(HTTPBackingStore &backing_store) {
  thread newthread(
      [&](TCPSocket client) {
        try {
          /* get original destination for connection request */
          Address server_addr = client.original_dest();

          /* create socket and connect to original destination and send original
           * request */
          TCPSocket server;
          server.connect(server_addr);

          if (server_addr.port() != 443) { /* normal HTTP */
            return serialized_loop(server, client, backing_store);
          }

          /* handle TLS */
          SecureSocket tls_server(
              client_context_.new_secure_socket(move(server)));
          tls_server.connect();

          SecureSocket tls_client(
              server_context_.new_secure_socket(move(client)));
          tls_client.accept();

          serialized_loop(tls_server, tls_client, backing_store);
        } catch (const exception &e) {
          print_exception(e);
        }
      },
      listener_socket_.accept());

  /* don't wait around for the reply */
  newthread.detach();
}
