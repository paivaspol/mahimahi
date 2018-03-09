#include <algorithm>
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
    : HTTPProxy(listener_addr), prefetch_resources_(), escaped_urls_(),
      prefetch_resources_order_(), page_url_(page_url),
      prev_resp_t_(std::chrono::high_resolution_clock::now()),
      seen_high_pri_resp_(), seen_low_pri_resp_(), m_(), cv_(),
      request_order_(), url_to_req_id_(), access_guard_(), next_req_id_(0),
      low_priorities_() {
  get_prefetch_resources(prefetch_resources_, prefetch_urls_filename);
  get_request_order(request_order_, request_order_filename);
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

        /* We are done with with a request */
        // Add this request ID to URL map.
        auto url = request_parser.front().get_url();
        string escaped_url = escape_page_url(url);

        // Logic before when getting the request.
        std::unique_lock<std::mutex> access_lock(access_guard_);
        int req_id = next_req_id_;
        cout << "[NEW_REQUEST] req_id: " << to_string(req_id) << " url: " << url
             << endl;
        next_req_id_++;
        url_to_req_id_[url] = req_id;

        // A resource is considered high priority if it is:
        //   (1) not a prefetch request
        //   (2) the main HTML.
        bool is_high_priority = prefetch_resources_.find(escaped_url) ==
                                    prefetch_resources_.end() ||
                                escaped_url == page_url_;

        // Make sure that everything has high priority. Only for debugging
        // purposes.
        // bool is_high_priority = true;

        // if (is_high_priority) {
        //   // This is not a prefetch resource, thus, a high priority resource.
        // } else {
        //   low_priorities_[req_id] = url;
        // }
        if (!is_high_priority) {
          cout << "[LOW_PRI] req_id: " << to_string(req_id) << " url: " << url
               << endl;
          low_priorities_[req_id] = url;
        }
        reprioritize(url);
        access_lock.unlock();

        response_parser.new_request_arrived(request_parser.front());
        request_parser.pop();
        return ResultType::Continue;
      },
      [&]() { return not request_parser.empty(); }));

  /* completed responses from server are serialized and sent to client */
  poller.add_action(Poller::Action(
      client, Direction::Out,
      [&]() {
        std::unique_lock<std::mutex> access_lock(access_guard_);
        /* If this is the main HTML. Make sure to inject Link preload into the
         * response */
        auto url = response_parser.front().request().get_url();
        auto req_id = url_to_req_id_[url];
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

          /* This is the main HTML. Make sure to clear out both high priority
           * and low priority queues. */
          clear_queues();
        }

        if (low_priorities_.find(req_id) != low_priorities_.end()) {
          /* This is a low priority resource. */
          seen_low_pri_resp_.insert(req_id);
        } else {
          seen_high_pri_resp_.insert(req_id);
        }

        cout << "[INSERTED] req_id: " << to_string(req_id) << " URL: " << url
             << endl;
        // std::chrono::high_resolution_clock::time_point cur_t =
        //     std::chrono::high_resolution_clock::now();
        // if (std::chrono::duration_cast<std::chrono::milliseconds>(
        //         cur_t - prev_resp_t_) >=
        //     std::chrono::milliseconds(
        //         100)) { /* If the last time we have seen a valid response is
        //                   greater than 50ms */

        //   // Make sure that the next response to send back is a response that
        //   we
        //   // have already seen.
        //   print_seen_responses();
        //   next_low_priority_req_ = *seen_resp_.begin();
        //   cout << "[TIMED OUT] next_low_pri_req_: "
        //        << to_string(next_low_priority_req_) << " URL: " << url <<
        //        endl;
        //   cv_.notify_all();
        // }
        // TODO: this is probably wrong.
        if (next_req_id_ - req_id >= 5) {
          cout << "[LAGGING_BEHIND] notifying other threads req_id: "
               << to_string(req_id)
               << " next_req_id_: " << to_string(next_req_id_) << endl;
          access_lock.unlock();
          cv_.notify_all(); // To make progress.
        } else {
          access_lock.unlock();
        }

        // Wake up if the front of the high priority queue is us or
        // if nothing is left in the high priority queue and we are the next low
        // priority
        cout << "[WAITING] req_id: " << to_string(req_id) << " URL: " << url
             << " next_req_id_ " << to_string(next_req_id_) << endl;
        std::unique_lock<std::mutex> lk(m_);
        if (escaped_url == page_url_) {
          /* if this is the main page, make sure that we are not blocking
           * forever. */
          // cv_.wait_for(lk, chrono::milliseconds(50), [&] { return true; });
          cv_.wait_for(lk, chrono::milliseconds(50));
          next_req_id_ =
              req_id; // This is to make sure that we can make progress
        } else {
          cv_.wait(lk, [&] {
            access_lock.lock();
            cout << "\t\tThread waking up to check: req_id: " << req_id
                 << " high_pri.size(): "
                 << to_string(seen_high_pri_resp_.size())
                 << " low_pri.size(): " << to_string(seen_low_pri_resp_.size())
                 << endl;
            if (!seen_high_pri_resp_.empty()) {
              cout << "\t\t[HIGH_PRI_FRONT]: "
                   << to_string(*seen_high_pri_resp_.begin()) << endl;
            }
            if (!seen_low_pri_resp_.empty()) {
              cout << "\t\t[LOW_PRI_FRONT]: "
                   << to_string(*seen_low_pri_resp_.begin()) << endl;
            }
            bool condition = (!seen_high_pri_resp_.empty() &&
                              *seen_high_pri_resp_.begin() == req_id) ||
                             (!seen_low_pri_resp_.empty() &&
                              *seen_low_pri_resp_.begin() == req_id);
            access_lock.unlock();
            return condition;
          });
        }

        cout << "[SENDING_RESPONSE] req_id: " << req_id << " URL: " << url
             << " next_req_id_: " << to_string(next_req_id_) << endl;

        access_lock.lock();
        // This is the time when we are able to process a response.
        prev_resp_t_ = std::chrono::high_resolution_clock::now();
        next_req_id_ = max(req_id + 1, next_req_id_ + 1);

        // auto start = chrono::system_clock::now();
        // auto start_epoch = start.time_since_epoch();
        // auto start_seconds =
        //     chrono::duration_cast<chrono::milliseconds>(start_epoch);
        // cout << "\tURL: " << url
        //      << " Start: " << to_string(start_seconds.count()) << endl;

        client.write(response_parser.front().str());

        // auto end = chrono::system_clock::now();
        // auto end_epoch = end.time_since_epoch();
        // auto end_seconds =
        //     chrono::duration_cast<chrono::milliseconds>(end_epoch);
        // cout << "\tURL: " << url << " End: " <<
        // to_string(end_seconds.count())
        //      << endl;
        cout << "SENT: req_id: " << to_string(req_id) << " url: " << url
             << endl;

        backing_store.save(response_parser.front(), server_addr);
        response_parser.pop();

        // Done with this request so delete this req_id
        seen_high_pri_resp_.erase(req_id);
        seen_low_pri_resp_.erase(req_id);
        low_priorities_.erase(req_id);

        cout << "[DONE]: " << url << " req_id: " << req_id << endl;
        if (!seen_high_pri_resp_.empty()) {
          cout << "\t\t[HIGH_PRI_FRONT]: "
               << to_string(*seen_high_pri_resp_.begin()) << endl;
        }
        if (!seen_low_pri_resp_.empty()) {
          cout << "\t\t[LOW_PRI_FRONT]: "
               << to_string(*seen_low_pri_resp_.begin()) << endl;
        }
        access_lock.unlock();

        // We are done with this response, unlock the lock and notify the
        // waiting threads.
        lk.unlock();
        cv_.notify_all();
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

void SerializedHTTPProxy::get_request_order(map<string, int> &request_order,
                                            string request_order_filename) {
  ifstream prefetch_resources_file(request_order_filename);
  string line;
  int order = 0;
  while (getline(prefetch_resources_file, line)) {
    request_order[remove_scheme(line)] = order;
    order++;
  }
}

void SerializedHTTPProxy::reprioritize(const string &current_url) {
  // Get the current url request order based on a previous request order.
  cout << "[BEGINNING REPRI] for " << current_url << " high_pri.size() "
       << to_string(seen_high_pri_resp_.size()) << " seen_resp_low_pri.size() "
       << to_string(seen_low_pri_resp_.size())
       << " all_low_pris: " << to_string(low_priorities_.size()) << endl;
  string removed_scheme = remove_scheme(current_url);
  int url_request_order = request_order_[removed_scheme];
  vector<int> delete_list;
  for (auto it = low_priorities_.begin(); it != low_priorities_.end(); it++) {
    string low_pri_url = it->second;
    cout << "\t[low_pri_url] url: " << low_pri_url << endl;
    auto find_request_order_it = request_order_.find(low_pri_url);
    if (find_request_order_it == request_order_.end()) {
      // We have no idea about the request ordering of this resource.
      // Just leave the resource as low priority as it is now.
      continue;
    }
    // This is the order from a previous load.
    int low_pri_request_order = find_request_order_it->second;
    if (low_pri_request_order >= url_request_order) {
      // This resource is not really needed yet based on a prior request
      // ordering. Leave it low priority as is.
      continue;
    }

    // Need to reprioritize this resource. Move it from the low priority map to
    // the high priority queue.
    int low_pri_req_id = it->first;
    cout << "[REPRIORITIZING]: req_id: " << to_string(low_pri_req_id)
         << " url: " << low_pri_url << " current_url: " << current_url << endl;
    delete_list.push_back(low_pri_req_id);
    cout << "[FIN_REPRIORITIZING]: req_id: " << to_string(low_pri_req_id)
         << " url: " << low_pri_url << " current_url: " << current_url << endl;
  }

  for (auto it = delete_list.begin(); it != delete_list.end(); it++) {
    auto req_id = *it;
    low_priorities_.erase(req_id);
    seen_low_pri_resp_.erase(req_id);
    seen_high_pri_resp_.insert(req_id);
  }
  cout << "[ENDING REPRI] for " << current_url << " high_pri.size() "
       << to_string(seen_high_pri_resp_.size()) << " seen_resp_low_pri.size() "
       << to_string(seen_low_pri_resp_.size())
       << " all_low_pris: " << to_string(low_priorities_.size()) << endl;
}

void SerializedHTTPProxy::clear_queues(void) {
  // Clear all the requests from the previous load.
  low_priorities_.clear();
  // seen_resp_.clear();
  seen_high_pri_resp_.clear();
  seen_low_pri_resp_.clear();
}

void SerializedHTTPProxy::print_seen_responses(void) {
  // cout << "[SEEN RESP]: ";
  // for (auto it = seen_resp_.begin(); it != seen_resp_.end(); it++) {
  //   cout << to_string(*it) << ", ";
  // }
  // cout << endl;
}
