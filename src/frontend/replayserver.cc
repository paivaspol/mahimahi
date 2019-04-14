/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>
#include <utility> // pair, make_pair
#include <algorithm> // min, max
#include <cmath> // round

#include "exception.hh"
#include "file_descriptor.hh"
#include "http_header.hh"
#include "http_record.pb.h"
#include "http_request.hh"
#include "http_response.hh"
#include "util.hh"

using namespace std;

string safe_getenv(const string &key) {
  const char *const value = getenv(key.c_str());
  if (not value) {
    throw runtime_error("missing environment variable: " + key);
  }
  return value;
}

/* does the actual HTTP header match this stored request? */
bool header_match(const string &env_var_name, const string &header_name,
                  const HTTPRequest &saved_request) {
  const char *const env_value = getenv(env_var_name.c_str());

  /* case 1: neither header exists (OK) */
  if ((not env_value) and (not saved_request.has_header(header_name))) {
    return true;
  }

  /* case 2: headers both exist (OK if values match) */
  if (env_value and saved_request.has_header(header_name)) {
    return saved_request.get_header_value(header_name) == string(env_value);
  }

  /* case 3: one exists but the other doesn't (failure) */
  return false;
}

string strip_query(const string &request_line) {
  const auto index = request_line.find("?");
  if (index == string::npos) {
    return request_line;
  } else {
    return request_line.substr(0, index);
  }
}

string extract_url_from_request_line(const string &request_line) {
  vector<string> elems;
  stringstream ss(request_line);
  string item;
  while (getline(ss, item, ' ')) {
    elems.push_back(item);
  }
  return elems[1];
}

string construct_url_from_request_line(const string &request_line, bool is_https) {
  string scheme = "http://";
  if (is_https) {
    scheme = "https://";
  }
  string host_key = "HTTP_HOST";
  return scheme + safe_getenv(host_key.c_str()) + extract_url_from_request_line(request_line);
}

string remove_trailing_slash(const string &url) {
  if (url.length() <= 0) {
    return url;
  }

  string retval = url;
  while (retval[retval.length() - 1] == '/') {
    retval = retval.substr(0, retval.length() - 1);
  }
  return retval;
}

string strip_hostname(const string &url, const string &path) {
  string http = "http://";
  string https = "https://";
  if ((url.find(http) == 0 && path.find(http) == 0) ||
      (url.find(https) == 0 && path.find(https) == 0)) {
    return url;
  }

  string retval = url;
  if (url.find(http) == 0) {
    retval = url.substr(http.length(), url.length());
  } else if (url.find(https) == 0) {
    retval = url.substr(https.length(), url.length());
  }

  string www = "www.";
  if (retval.find(www) == 0) {
    retval = retval.substr(www.length(), retval.length());
  }

  const auto index = retval.find("/");
  retval = retval.substr(index, retval.length());

  return retval;
}

string extract_hostname(const string &url) {
  string http = "http://";
  string https = "https://";

  string retval = url;
  if (url.find(http) == 0) {
    retval = url.substr(http.length(), url.length());
  } else if (url.find(https) == 0) {
    retval = url.substr(https.length(), url.length());
  }

  const auto index = retval.find("/");
  retval = retval.substr(0, index);
  return retval;
}

string strip_www(const string &url) {
  string retval = url;

  string www = "www.";
  if (retval.find(www) == 0) {
    retval = retval.substr(www.length(), retval.length());
  }
  return retval;
}

void load_server_think_times(
		const string &server_think_time_filename, 
		map<string, int> &server_think_times) {
  ifstream infile(server_think_time_filename);
  string line;
  if (infile.is_open()) {
    while (getline(infile, line)) {
      vector<string> splitted_line = split(line, ' ');
      // We need to strip the scheme and hostname.
      const string url = splitted_line[0];
      int think_time = stoi(splitted_line[1]);
      server_think_times.insert({ url, think_time });
    }
    infile.close();
  }
}

string get_last_path_token(const string &path) {
  const auto index = path.rfind("/");
  string last_token = path;
  // First, find the last token of the path.
  if (index != string::npos) {
    last_token = path.substr(index + 1, path.length());
  }

  // Second, remove any path parameters.
  // Anything after first occurence of ";".
  const auto semicolon_index = last_token.find(";");
  if (semicolon_index == string::npos) {
    return last_token;
  } else {
    return last_token.substr(0, semicolon_index);
  }
}

string get_url_frac_after_semicolon(const string &url) {
  const auto semicolon_index = url.find(";");
  if (semicolon_index == string::npos) {
    return url;
  } else {
    return url.substr(semicolon_index + 1, url.length());
  }
}

/**
 * match_headers returns whether two conditions match.
 *
 * (1) The scheme of the saved_record matches the scheme of the saved_request.
 * (2) The values of Host header match.
 */
bool match_headers(const MahimahiProtobufs::RequestResponse &saved_record, 
    const HTTPRequest &saved_request, const bool is_https) {
  /* match HTTP/HTTPS */
  if (is_https and (saved_record.scheme() !=
      MahimahiProtobufs::RequestResponse_Scheme_HTTPS)) {
    return false;
  }
  if ((not is_https) and (saved_record.scheme() !=
        MahimahiProtobufs::RequestResponse_Scheme_HTTP)) {
    return false;
  }

  /* match host header */
  if (not header_match("HTTP_HOST", "Host", saved_request)) {
    return false;
  }
  return true;
}

/**
 * match_url returns the matching score of the given URLs.
 *
 * Returns a positive score that is the value longest 
 * common substring starting from the beginning of the string.
 * This assumes that the path already match.
 */
unsigned int match_url(const string &saved_request_url,
                       const string &request_url) {
  /* must match first line up to "?" at least */
  // ofstream myfile;
  // myfile.open("match_url.txt", ios::app);
  // myfile << "returned from strip query mismatch: " << request_url
  //        << " saved: " << saved_request_url << endl;
  // if (request_url.find("5o3Rb.5pjsDBCdfFtzlQ8w") != string::npos) {
  //   myfile << "striping url" << endl;
  // }
  // if (strip_query(request_url) != strip_query(saved_request_url)) {
  //   // if (request_url.find("5o3Rb.5pjsDBCdfFtzlQ8w") != string::npos &&
  //   // saved_request_url.find("5o3Rb.5pjsDBCdfFtzlQ8w") != string::npos) {
  //   //   myfile << "returned from strip query mismatch: " << request_url << "
  //   //   saved: " << saved_request_url << endl;
  //   // }
  //   // myfile << "\there" << endl;
  //   return 0;
  // }

  // if (request_url.find("5o3Rb.5pjsDBCdfFtzlQ8w") != string::npos) {
  //   myfile << "Matched " << request_url << " to " << saved_request_url <<
  //   endl;
  // }
  /* success! return size of common prefix */
  const auto max_match = min(request_url.size(), saved_request_url.size());
  for (unsigned int i = 0; i < max_match; i++) {
    if (request_url.at(i) != saved_request_url.at(i)) {
      // if (request_url.find("5o3Rb.5pjsDBCdfFtzlQ8w") != string::npos) {
      //   myfile << "score: " << i << endl;
      // }
      return i;
    }
  }
  // if (request_url.find("5o3Rb.5pjsDBCdfFtzlQ8w") != string::npos) {
  // myfile << "score: " << max_match << endl;
  // }
  // myfile.close();
  return max_match;
}

/* compare request_line and certain headers of incoming request and stored
 * request */
unsigned int match_score(const HTTPRequest &saved_request,
                         const string &request_line) {
  // myfile.close();
  string request_url =
      strip_hostname(extract_url_from_request_line(request_line),
                     extract_url_from_request_line(saved_request.first_line()));
  string saved_request_url =
      strip_hostname(extract_url_from_request_line(saved_request.first_line()),
                     extract_url_from_request_line(saved_request.first_line()));
  return match_url(saved_request_url, request_url);
}

unsigned int compute_edit_distance(const string &s1,
                                   const string &s2) {
  int maxOffset = 500;
  int maxDistance = 200;
  int l1 = s1.length();
  int l2 = s2.length();

  int c1 = 0;  //cursor for string 1
  int c2 = 0;  //cursor for string 2
  int lcss = 0;  //largest common subsequence
  int local_cs = 0; //local common substring
  int trans = 0;  //number of transpositions ('ab' vs 'ba')
  vector<pair<int, int>> offset_arr = {};  //offset pair array, for computing the transpositions

  while ((c1 < l1) && (c2 < l2)) {
    if (s1.at(c1) == s2.at(c2)) {
      local_cs++;
      //see if current match is a transposition
      while (offset_arr.size() > 0) {
        auto first_offset_el = offset_arr[0];
        if (c1 <= first_offset_el.first || c2 <= first_offset_el.second) {
          trans++;
          break;
        } else {
          offset_arr.erase(offset_arr.begin());
        }
      }
      offset_arr.push_back(make_pair(c1, c2));
    } else {
      lcss += local_cs;
      local_cs = 0;
      if (c1 != c2) {
        c1 = min(c1, c2);
        c2 = min(c1, c2);  //using min allows the computation of transpositions
      }
      // if matching characters are found, remove 1 from both cursors 
      // (they get incremented at the end of the loop)
      // so that we can have only one code block handling matches 
      for (int i = 0; i < maxOffset && (c1 +i < l1 || c2 + i < l2); i++) {
        if ((c1 + i < l1) && (s1.at(c1 + i) == s2.at(c2))) {
          c1 += i - 1; 
          c2--;
          break;
        }
        if ((c2 + i < l2) && (s1.at(c1) == s2.at(c2 + i))) {
          c1--;
          c2 += i - 1;
          break;
        }
      }
    }

    c1++;
    c2++;

    if (maxDistance) {
      int temporaryDistance = max(c1, c2) - lcss + trans;
      if (temporaryDistance >= maxDistance) 
        return round(temporaryDistance);
    }

    // this covers the case where the last match is on the last token in list, 
    // so that it can compute transpositions correctly
    if ((c1 >= l1) || (c2 >= l2)) {
      lcss += local_cs;
      local_cs = 0;
      c1 = min(c1, c2);
      c2 = min(c1, c2);
    }
  }
  lcss += local_cs;
  return round(max(l1,l2)- lcss +trans); //add the cost of transpositions to the final result
}

void populate_push_configurations(const string &dependency_file,
                                  const string &request_url,
                                  HTTPResponse &response,
                                  const string &current_loading_page) {
  map<string, vector<string>> dependencies_map;
  map<string, string> dependency_type_map;
  map<string, string> dependency_priority_map;
  ifstream infile(dependency_file);
  string line;
  if (infile.is_open()) {
    while (getline(infile, line)) {
      vector<string> splitted_line = split(line, ' ');
      string parent = remove_trailing_slash(splitted_line[0]);
      if (dependencies_map.find(parent) == dependencies_map.end()) {
        dependencies_map[parent] = {};
      }
      string child = splitted_line[2];
      string resource_type = splitted_line[4];
      string resource_priority = splitted_line[5];
      dependencies_map[parent].push_back(child);
      dependency_type_map[child] = resource_type;
      dependency_priority_map[child] = resource_priority;
    }
    infile.close();
  }

  if (!dependencies_map.empty()) {
    // Write the dependencies to the configuration file.
    string removed_slash_request_url = remove_trailing_slash(request_url);
    vector<string> link_resources;
    vector<string> semi_important_resources;
    vector<string> unimportant_resources;
    if (dependencies_map.find(removed_slash_request_url) !=
        dependencies_map.end()) {
      auto key = removed_slash_request_url;
      auto values = dependencies_map[key];
      for (auto list_it = values.begin(); list_it != values.end(); ++list_it) {
        // Push all dependencies for the location.
        string dependency_filename = *list_it;
        // if (dependency_type_map[dependency_filename] == "Document" ||
        //     dependency_type_map[dependency_filename] == "Script" ||
        //     dependency_type_map[dependency_filename] == "Stylesheet") {
        if ((dependency_priority_map[dependency_filename] == "VeryHigh" ||
             dependency_priority_map[dependency_filename] == "High" ||
             dependency_priority_map[dependency_filename] == "Medium") &&
            (dependency_type_map[dependency_filename] == "Document" ||
             dependency_type_map[dependency_filename] == "Script" ||
             dependency_type_map[dependency_filename] == "Stylesheet")) {
          string dependency_type = dependency_type_map[dependency_filename];
          string link_resource_string =
              "<" + dependency_filename + ">;rel=preload" +
              infer_resource_type(dependency_type_map[dependency_filename]);

          // Add push or nopush directive based on the hostname of the URL.
          string request_hostname =
              strip_www(extract_hostname(dependency_filename));
          if (request_hostname != current_loading_page) {
            link_resource_string += ";nopush";
          }

          link_resources.push_back(link_resource_string);
        } else if (dependency_type_map[dependency_filename] != "XHR") {
          string resource_string = dependency_filename + ";" +
                                   dependency_type_map[dependency_filename];
          if (dependency_type_map[dependency_filename] == "Document" ||
              dependency_type_map[dependency_filename] == "Script" ||
              dependency_type_map[dependency_filename] == "Stylesheet") {
            semi_important_resources.push_back(resource_string);
          } else {
            unimportant_resources.push_back(resource_string);
          }
        }
      }
    }

    string delimeter = "|$de|";

    if (link_resources.size() > 0) {
      string link_string_value = "";
      for (auto it = link_resources.begin(); it != link_resources.end(); ++it) {
        link_string_value += *it + ", ";
      }
      string link_string =
          "Link: " + link_string_value.substr(0, link_string_value.size() - 2);
      response.add_header_after_parsing(link_string);
    }
    if (semi_important_resources.size() > 0) {
      string semi_important_resource_value = "";
      for (auto it = semi_important_resources.begin();
           it != semi_important_resources.end(); ++it) {
        semi_important_resource_value += *it + delimeter;
      }
      string x_systemname_semi_important_resource_string =
          "x-systemname-semi-important: " +
          semi_important_resource_value.substr(
              0, semi_important_resource_value.size() - delimeter.length());
      response.add_header_after_parsing(
          x_systemname_semi_important_resource_string);
    }
    if (unimportant_resources.size() > 0) {
      string unimportant_resource_value = "";
      for (auto it = unimportant_resources.begin();
           it != unimportant_resources.end(); ++it) {
        unimportant_resource_value += *it + delimeter;
      }
      string x_systemname_unimportant_resource_string =
          "x-systemname-unimportant: " +
          unimportant_resource_value.substr(
              0, unimportant_resource_value.size() - delimeter.length());
      response.add_header_after_parsing(
          x_systemname_unimportant_resource_string);
    }
  }
}

int check_redirect(MahimahiProtobufs::RequestResponse saved_record,
                   int previous_score) {
  HTTPRequest saved_request(saved_record.request());
  HTTPResponse saved_response(saved_record.response());

  if (previous_score == 0) {
    return previous_score;
  }

  /* check response code */
  string response_first_line = saved_response.first_line();
  vector<string> splitted_response_first_line = split(response_first_line, ' ');
  string status_code = splitted_response_first_line[1];
  if (status_code != "301" && status_code != "302") {
    // Not redirection.
    return previous_score;
  }

  /* Check the response with the redirected location. */
  string request_first_line = saved_request.first_line();
  vector<string> splitted_request_first_line = split(request_first_line, ' ');
  string request_url =
      splitted_request_first_line[1]; // This shouldn't contain the HOST
  if (!saved_response.has_header("Location"))
    return previous_score;

  string redirected_location = saved_response.get_header_value("Location");
  string redirected_location_host = extract_hostname(redirected_location);
  string path = strip_hostname(redirected_location, request_url);

  if (path == request_url &&
      redirected_location_host == saved_request.get_header_value("Host")) {
    return 0;
  }
  return previous_score;
}

int main(void) {
  try {
    assert_not_root();

    const string working_directory = safe_getenv("MAHIMAHI_CHDIR");
    const string recording_directory = safe_getenv("MAHIMAHI_RECORD_PATH");
    const string current_loading_page = safe_getenv("LOADING_PAGE");
    const string dependency_filename = safe_getenv("DEPENDENCY_FILE");
    const string path = safe_getenv("REQUEST_URI");
    const string request_line = safe_getenv("REQUEST_METHOD") + " " + path +
                                " " + safe_getenv("SERVER_PROTOCOL");
    const bool is_https = getenv("HTTPS");

    SystemCall("chdir", chdir(working_directory.c_str()));

    const vector<string> files = list_directory_contents(recording_directory);
    map<string, int> server_think_times;
    load_server_think_times("/home/vaspol/Research/MobileWebOptimization/page_load_setup/build/bin/server_think_time_map.txt", server_think_times);

    unsigned int best_mm_score = 0;
    MahimahiProtobufs::RequestResponse best_mm_match;

    // unsigned int best_edit_score = 1000000; // For actual edit distance.
    unsigned int best_edit_score = 0; // Quick hack for matching only last token.
    MahimahiProtobufs::RequestResponse best_edit_match;

    for (const auto &filename : files) {
      FileDescriptor fd(SystemCall("open", open(filename.c_str(), O_RDONLY)));
      MahimahiProtobufs::RequestResponse current_record;
      if (not current_record.ParseFromFileDescriptor(fd.fd_num())) {
        throw runtime_error(filename + ": invalid HTTP request/response");
      }

      HTTPRequest saved_request(current_record.request());
      // First, we match the HTTP headers.
      if (!match_headers(current_record, saved_request, is_https)) {
        // Headers did not match, skip it.
        continue;
      }

      // We passed the hostname check. Remove it.
      string request_url =
          strip_hostname(extract_url_from_request_line(request_line),
                         extract_url_from_request_line(saved_request.first_line()));
      string saved_request_url =
          strip_hostname(extract_url_from_request_line(saved_request.first_line()),
                         extract_url_from_request_line(saved_request.first_line()));

      // Next, check if the paths match. If they match, get the Mahimahi match score.
      string request_url_stripped_query = strip_query(request_url);
      string saved_request_url_stripped_query = strip_query(saved_request_url);
      if (request_url_stripped_query == saved_request_url_stripped_query) {
        unsigned int mm_score = match_url(saved_request_url, request_url);
        if (mm_score > best_mm_score) {
          best_mm_match = current_record;
          best_mm_score = mm_score;
        }
        // We found a match using Mahimahi logic. Skip computing edit distance.
        continue;
      }

      // Second, if we still haven't found a match using the Mahimahi matching algorithm, 
      // there is a potential that we cannot find a match. So, also compute the edit distance.
      if (best_mm_score > 0) {
        continue;
      }

      // We still haven't got a match for this resource. Also, try edit distance.
      string request_url_last_path_token = get_last_path_token(request_url_stripped_query);
      string saved_request_url_last_path_token = get_last_path_token(saved_request_url_stripped_query);
      if (request_url_last_path_token == saved_request_url_last_path_token) {
        // Find match score.
        string request_url_frac_after_semi = get_url_frac_after_semicolon(request_url);
        string saved_request_url_frac_after_semi = get_url_frac_after_semicolon(saved_request_url);
        unsigned int edit_score = match_url(saved_request_url_frac_after_semi, request_url_frac_after_semi);
        if (edit_score > best_edit_score) {
          best_edit_match = current_record;
          best_edit_score = edit_score;
        }
      }
        
      /* logic for sift4 edit distance. */
      // unsigned int edit_score = compute_edit_distance(saved_request_url_stripped_query, saved_request_url);
      // if (edit_score < best_edit_score) {
      //   if (1.0 * edit_score / saved_request_url_stripped_query.length() >= 0.3) {
      //     // Ignore if the edit score is greater than 30% of the length.
      //     continue;
      //   }
      //   best_edit_score = edit_score;
      //   best_edit_match = current_record;
      // }
    }

    // best_score = check_redirect(best_match, best_score);

    if (best_mm_score > 0 || best_edit_score != 1000000) { /* give client the best match */
      MahimahiProtobufs::RequestResponse best_match = best_mm_match;
      if (best_mm_score == 0) {
        best_match = best_edit_match;
      }
      HTTPRequest request(best_match.request());
      HTTPResponse response(best_match.response());

      /* Remove all cache-related headers. */
      // vector<string> headers = {"Cache-control", "Expires", "Last-modified",
      //                           "Date",          "Age",     "Etag"};
      // for (auto it = headers.begin(); it != headers.end(); ++it) {
      //   response.remove_header(*it);
      // }

      // /* Add the cache-control header and set to 3600. */
      // response.add_header_after_parsing("Cache-Control: max-age=3600");
      // response.add_header_after_parsing( "Cache-Control: no-cache, no-store,
      // must-revalidate max-age=0" );

      // if (dependency_filename != "None") {
      //   string scheme = is_https ? "https://" : "http://";
      //   string request_url = scheme + request.get_header_value("Host") +
      //   path;
      //   populate_push_configurations(dependency_filename, request_url,
      //   response,
      //                                current_loading_page);
      // }
      // string url = extract_url_from_request_line(request_line);
      string url = construct_url_from_request_line(request_line, is_https);
      if (server_think_times.find(url) != server_think_times.end()) {
        int delay = server_think_times[url];
        this_thread::sleep_for(chrono::milliseconds(delay));
      }

      cout << response.str();
      return EXIT_SUCCESS;
    } else { /* no acceptable matches for request */
      // string response_body = "replayserver: could not find a match for " +
      // request_line;
      string response_body = "replayserver: could not find a match.";
      cout << "HTTP/1.1 404 Not Found" << CRLF;
      cout << "Content-Type: text/plain" << CRLF;
      cout << "Content-Length: " << response_body.length() << CRLF;
      cout << "Cache-Control: max-age=60" << CRLF << CRLF;
      cout << response_body << CRLF;
      return EXIT_FAILURE;
    }
  } catch (const exception &e) {
    cout << "HTTP/1.1 500 Internal Server Error" << CRLF;
    cout << "Content-Type: text/plain" << CRLF << CRLF;
    cout << "mahimahi mm-webreplay received an exception:" << CRLF << CRLF;
    print_exception(e, cout);
    return EXIT_FAILURE;
  }
}
