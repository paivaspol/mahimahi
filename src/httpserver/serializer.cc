#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>

#include "serializer.hh"

using namespace std;

const string HTTPS = "https://";
const string HTTP = "http://";

Serializer::Serializer(string prefetch_resources_filename)
    : m_(), cv_(), lk_(m_), high_priorities_(), prefetch_resources_(),
      access_guard_(), new_low_priority_req_id_(0), next_low_priority_req_(0),
      low_priorities_() {
  get_prefetch_resources(prefetch_resources_, prefetch_resources_filename);
}

void Serializer::get_prefetch_resources(set<string> prefetch_resources,
                                        string prefetch_resources_filename) {
  // Get the prefetch resources.
  ifstream prefetch_resources_file(prefetch_resources_filename);
  string line;
  cout << "Prefetch resources:" << endl;
  while (getline(prefetch_resources_file, line)) {
    string result;
    if (line.find(HTTPS) != string::npos) {
      result = line.substr(HTTPS.length());
    } else if (line.find(HTTP) != string::npos) {
      result = line.substr(HTTP.length());
    }
    cout << result << endl;
    prefetch_resources.insert(result);
  }
  prefetch_resources_file.close();
}

void Serializer::register_request(const std::string &url) {
  access_guard_.lock();
  if (prefetch_resources_.find(url) == set::end) {
    // This is not a prefetch resource, thus, a high priority resource.
    high_priorities_.push(url);
  } else {
    int req_id = new_low_priority_req_id_;
    new_low_priority_req_id_++;
    low_priorities[req_id] = url;
    access_guard_.unlock();
  }

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
    delete (low_priorities_, req_id);
  }
  next_low_priority_req_++;
}

void Serializer::send_request_complete() {
  // lk_.unlock();
  cv_.notify_all();
}
