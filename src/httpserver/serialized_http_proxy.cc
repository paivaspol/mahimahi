#include <arpa/inet.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <linux/netfilter_ipv4.h>
#include <map>
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
#include "util.hh"

using namespace std;
using namespace PollerShortNames;

SerializedHTTPProxy::SerializedHTTPProxy(const Address &listener_addr,
                                         const string &prefetch_urls_filename,
                                         const string &request_order_filename,
                                         const string &page_url)
    : HTTPProxy(listener_addr), serializer_(request_order_filename),
      prefetch_resources_(), escaped_urls_(), prefetch_resources_order_(),
      page_url_(page_url) {
  get_prefetch_resources(prefetch_resources_, prefetch_urls_filename);
}

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
        string escaped_url = escape_page_url(url);
        cout << "Escaped: " << escaped_url << " page_url: " << page_url_
             << endl;
        if (escaped_url == page_url_) {
          // This is the main HTML. We have to inject the prefetch resources in
          // the form of Link preload headers.
          string preload_header_str = generate_preload_header_str();
          cout << "Injecting preload URLs: " << preload_header_str << endl;
          response_parser.front().add_header_after_parsing(
              "Link: " + generate_preload_header_str());
        }

        // A resource is considered high priority if it is:
        //   (1) not a prefetch request
        //   (2) the main HTML.
        bool is_high_priority = prefetch_resources_.find(escaped_url) ==
                                    prefetch_resources_.end() ||
                                escaped_url == page_url_;

        serializer_.register_request(url, is_high_priority);
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

void SerializedHTTPProxy::get_prefetch_resources(
    map<string, string> &prefetch_resources,
    string prefetch_resources_filename) {
  // Get the prefetch resources.
  cout << "Populating prefetch resources..." << endl;
  ifstream prefetch_resources_file(prefetch_resources_filename);
  string line;
  cout << "Prefetch resources:" << endl;
  while (getline(prefetch_resources_file, line)) {
    vector<string> splitted_line = split(line, ' ');
    for (auto it = splitted_line.begin(); it != splitted_line.end(); it++) {
      cout << *it << endl;
    }
    string prefetch_url = splitted_line[0];
    string resource_type = splitted_line[1];
    string result = remove_scheme(prefetch_url);
    string escaped_url = escape_page_url(result);
    cout << "\t[ADDING] " << escaped_url << " " << resource_type << endl;
    prefetch_resources[escaped_url] = resource_type;
    escaped_urls_[prefetch_url] = escaped_url;
    prefetch_resources_order_.push_back(prefetch_url);
  }
  prefetch_resources_file.close();
  cout << "(1) prefetch resources size: "
       << to_string(prefetch_resources.size()) << endl;
}

string SerializedHTTPProxy::generate_preload_header_str() {
  cout << "(2) prefetch resources size: "
       << to_string(prefetch_resources_.size()) << endl;
  for (auto it = prefetch_resources_.begin(); it != prefetch_resources_.end();
       it++) {
    cout << "\tPF: " << it->first << " type: " << it->second << endl;
  }
  string prefetch_string = "";
  for (auto it = prefetch_resources_order_.begin();
       it != prefetch_resources_order_.end(); ++it) {
    string pf_url = *it;
    string escaped_url = escaped_urls_[pf_url];
    string resource_type = prefetch_resources_[escaped_url];
    cout << "\tPFURL: " << pf_url << " escaped_url: " << escaped_url
         << " resource_type: " << resource_type << endl;
    string link_resource_string =
        "<" + pf_url + ">;rel=preload" + infer_resource_type(resource_type);
    prefetch_string += link_resource_string + ",";
  }
  prefetch_string = prefetch_string.substr(0, prefetch_string.length() - 1);
  return prefetch_string;
}
