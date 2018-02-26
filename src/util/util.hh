/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef UTIL_HH
#define UTIL_HH

#include <cstring>
#include <string>
#include <vector>

#include <sys/types.h>

#include "address.hh"

std::string shell_path(void);
void drop_privileges(void);
void check_requirements(const int argc, const char *const argv[]);
void make_directory(const std::string &directory);
Address first_nameserver(void);
std::vector<Address> all_nameservers(void);
std::vector<std::string> list_directory_contents(const std::string &dir);
void prepend_shell_prefix(const std::string &str);
template <typename T> void zero(T &x) { memset(&x, 0, sizeof(x)); }
std::string join(const std::vector<std::string> &command);
std::string get_working_directory(void);
std::string escape_page_url(const std::string &page_url_str);
std::string infer_resource_type(const std::string &resource_type);
void split(const std::string &s, char delim, std::vector<std::string> &elems);
std::vector<std::string> split(const std::string &s, char delim);
std::string remove_scheme(const std::string &url);

class TemporarilyUnprivileged {
private:
  const uid_t orig_euid;
  const gid_t orig_egid;

public:
  TemporarilyUnprivileged();
  ~TemporarilyUnprivileged();
};

void assert_not_root(void);

#endif /* UTIL_HH */
