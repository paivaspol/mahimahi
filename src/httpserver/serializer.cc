#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>

#include "serializer.hh"
#include "util.hh"

using namespace std;

Serializer::Serializer(string request_order_filename)
    : m_(), cv_(), lk_(m_), high_priorities_(), request_order_(),
      access_guard_(), new_low_priority_req_id_(0), next_low_priority_req_(0),
      low_priorities_() {
  get_request_order(request_order_, request_order_filename);
}

void Serializer::register_request(const std::string &url,
                                  bool is_high_priority) {
  access_guard_.lock();
  int req_id = new_low_priority_req_id_;
  new_low_priority_req_id_++;
  if (is_high_priority) {
    // This is not a prefetch resource, thus, a high priority resource.
    high_priorities_.push(url);
  } else {
    low_priorities_[req_id] = url;
  }
  reprioritize(url);
  access_guard_.unlock();

  // Wake up if the front of the high priority queue is us or
  // if nothing is left in the high priority queue and we are the next low
  // priority
  cv_.wait(lk_, [&] {
    return high_priorities_.front() == url ||
           (high_priorities_.empty() && next_low_priority_req_ == req_id);
  });

  if (high_priorities_.front() == url) {
    high_priorities_.pop();
  } else if (next_low_priority_req_ == req_id) {
    low_priorities_.erase(req_id);
  }
  next_low_priority_req_++;
}

void Serializer::send_request_complete() {
  // lk_.unlock();
  cv_.notify_all();
}

void Serializer::get_request_order(map<string, int> &request_order,
                                   string request_order_filename) {
  ifstream prefetch_resources_file(request_order_filename);
  string line;
  int order = 0;
  while (getline(prefetch_resources_file, line)) {
    request_order[remove_scheme(line)] = order;
    order++;
  }
}

void Serializer::reprioritize(const string &current_url) {
  // Get the current url request order based on a previous request order.
  string removed_scheme = remove_scheme(current_url);
  int url_request_order = request_order_[removed_scheme];
  for (auto it = low_priorities_.begin(); it != low_priorities_.end(); it++) {
    int low_pri_req_id = it->first;
    string low_pri_url = it->second;
    auto find_request_order_it = request_order_.find(low_pri_url);
    if (find_request_order_it == request_order_.end()) {
      // We have no idea about the request ordering of this resource.
      // Just leave the resource as low priority as it is now.
      continue;
    }
    int low_pri_request_order = find_request_order_it->second;
    if (low_pri_request_order > url_request_order) {
      // This resource is not really needed yet based on a prior request
      // ordering. Leave it low priority as is.
      continue;
    }

    // Need to reprioritize this resource. Move it from the low priority map to
    // the high priority queue.
    cout << "Reprioritizing: " << low_pri_url << endl;
    high_priorities_.push(low_pri_url);
    low_priorities_.erase(low_pri_req_id);
  }
}
