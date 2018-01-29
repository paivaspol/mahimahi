#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>

#include "serializer.hh"

using namespace std;

Serializer::Serializer()
    : m_(), cv_(), lk_(m_), high_priorities_(), access_guard_(),
      new_low_priority_req_id_(0), next_low_priority_req_(0),
      low_priorities_() {}

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
