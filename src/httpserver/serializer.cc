#include <condition_variable>
#include <iostream>
#include <mutex>

#include "serializer.hh"

using namespace std;

Serializer::Serializer()
    : m_(), cv_(), lk_(m_), request_queue_(), current_request_url_("") {}

void Serializer::register_request(const std::string &url) {
  request_queue_.push(url);
  cv_.wait(lk_, [&] { return request_queue_.front() == url; });
  request_queue_.pop();
}

void Serializer::send_request_complete() {
  // lk_.unlock();
  cv_.notify_all();
}
