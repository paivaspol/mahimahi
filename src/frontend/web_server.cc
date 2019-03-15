/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <iostream>
#include <string>
#include <unistd.h>

#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "apache_configuration.hh"
#include "config.h"
#include "exception.hh"
#include "system_runner.hh"
#include "util.hh"
#include "web_server.hh"

using namespace std;

void write_config_file(TempFile &config_file, const Address &addr,
                       const string &working_directory,
                       const string &record_path, const string &page) {
  cout << "Apache Config File: " << config_file.name()
       << " listening on: " << addr.str() << endl;
  config_file.write(apache_main_config);

  string path_prefix = PATH_PREFIX;
  config_file.write("LoadModule unixd_module " + path_prefix +
                    "/modules/mod_unixd.so\n");
  config_file.write("LoadModule log_config_module " + path_prefix +
                    "/modules/mod_log_config.so\n");
  config_file.write("LoadModule mpm_prefork_module " + path_prefix +
                    "/modules/mod_mpm_prefork.so\n");

  config_file.write("WorkingDir " + working_directory + "\n");
  config_file.write("RecordingDir " + record_path + "\n");
  config_file.write("LoadingPage " + page + "\n");
  config_file.write("AllowEncodedSlashes NoDecode\n");

  /* add pid file, log files, user/group name, and listen line to config file
   * and run apache */
  config_file.write("PidFile /tmp/replayshell_apache_pid." +
                    to_string(getpid()) + "." + to_string(random()) + "\n");
  /* Apache will check if this file exists before clobbering it,
     so we think it's ok for Apache to write here as root */

  config_file.write("ServerName mahimahi.\n");

  config_file.write("ErrorLog " + path_prefix + "/logs/apache_errors.log\n");

  string log_format =
      "%{usec}t %r %D %{Referer}i %{Host}i port:%p %{x-requested-with}i";
  config_file.write("CustomLog \"" + path_prefix + "/logs/" + page + "\" \"" +
                    log_format + "\"\n");

  config_file.write("User #" + to_string(getuid()) + "\n");

  config_file.write("Group #" + to_string(getgid()) + "\n");

  config_file.write("Listen " + addr.str() + "\n");
}

WebServer::WebServer(const Address &addr, const string &working_directory,
                     const string &record_path)
    : config_file_("/tmp/replayshell_apache_config"), moved_away_(false) {
  write_config_file(config_file_, addr, working_directory, record_path,
                    "custom.log");

  /* if port 443, add ssl components */
  if (addr.port() == 443) { /* ssl */
    config_file_.write(apache_ssl_config);
  }

  run({APACHE2, "-f", config_file_.name(), "-k", "start"});
}

WebServer::WebServer(const Address &addr, const string &working_directory,
                     const string &record_path, const string &page)
    : config_file_("/tmp/replayshell_apache_config"), moved_away_(false) {
  write_config_file(config_file_, addr, working_directory, record_path, page);

  /* if port 443, add ssl components */
  if (addr.port() == 443) { /* ssl */
    config_file_.write(apache_ssl_config);
  }

  run({APACHE2, "-f", config_file_.name(), "-k", "start"});
}

void populate_push_configurations(TempFile &config_file,
                                  const string &dependency_file) {
  map<string, vector<string>> dependencies_map;
  ifstream infile(dependency_file);
  string line;
  if (infile.is_open()) {
    while (getline(infile, line)) {
      vector<string> splitted_line = split(line, ' ');
      if (dependencies_map.find(splitted_line[0]) == dependencies_map.end()) {
        dependencies_map[splitted_line[0]] = {};
      }
      dependencies_map[splitted_line[0]].push_back(splitted_line[2]);
    }
    infile.close();
  }
  config_file.write("Header add MyHeader test\n");

  if (!dependencies_map.empty()) {
    // Write the dependencies to the configuration file.
    for (auto it = dependencies_map.begin(); it != dependencies_map.end();
         ++it) {
      auto key = it->first;
      auto values = it->second;
      // config_file.write("<Location " + key  + ">\n"); // Set location
      // condition. config_file.write("<IfModule mod_headers.c>\n");
      for (auto list_it = values.begin(); list_it != values.end(); ++list_it) {
        // Push all dependencies for the location.
        // string link_string = "Link: \"<" + *list_it + ">;rel=preload\"";
        // config_file.write("Header add " + link_string + "\n");
      }
      // config_file.write("</IfModule>\n");
      // config_file.write("</Location>\n");
    }
  }
}

WebServer::WebServer(const Address &addr, const string &working_directory,
                     const string &record_path, const string &escaped_page,
                     const string &dependency_file)
    : config_file_("/tmp/replayshell_apache_config"), moved_away_(false) {
  // string path_prefix = PATH_PREFIX;
  // config_file_.write( "LoadModule headers_module " + path_prefix +
  // "/modules/mod_headers.so\n" );

  // populate_push_configurations(config_file_, dependency_file);

  cout << "Dependency File: " << dependency_file << endl;
  string line = "DependencyFile " + dependency_file + "\n";
  config_file_.write(line);

  write_config_file(config_file_, addr, working_directory, record_path,
                    escaped_page);

  /* if port 443, add ssl components */
  if (addr.port() == 443) { /* ssl */
    config_file_.write(apache_ssl_config);
  }

  run({APACHE2, "-f", config_file_.name(), "-k", "start"});
}

WebServer::~WebServer() {
  if (moved_away_) {
    return;
  }

  try {
    run({APACHE2, "-f", config_file_.name(), "-k", "graceful-stop"});
  } catch (const exception &e) { /* don't throw from destructor */
    print_exception(e);
  }
}

WebServer::WebServer(WebServer &&other)
    : config_file_(move(other.config_file_)), moved_away_(false) {
  other.moved_away_ = true;
}
