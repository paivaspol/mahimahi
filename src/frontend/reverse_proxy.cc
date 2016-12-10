/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <cstdlib>
#include <csignal>
#include <fstream>
#include <string>
#include <sstream>
#include <iostream>
#include <unistd.h>

#include "reverse_proxy.hh"
#include "system_runner.hh"
#include "config.h"
#include "util.hh"
#include "exception.hh"

using namespace std;

ReverseProxy::ReverseProxy( const Address & frontend_address, 
                            const Address & backend_address, 
                            const string & path_to_proxy,
                            const string & path_to_proxy_key,
                            const string & path_to_proxy_cert,
                            const string & page)
  : config_file_("/tmp/h2o_config.conf"),
    pidfile_("/tmp/replayshell_nghttpx.pid"),
    moved_away_(false)
{
/*
    cout << "Config: " << config_file_.name() << endl;
    cout << "Frontend: " << frontend_address.str() << " backend: " << backend_address.str() << endl;
    cout << "Proxy key: " << path_to_proxy_key << " Proxy cert: " << path_to_proxy_cert << endl;

    string path_prefix = PATH_PREFIX;

    config_file_.write("frontend=" + frontend_address.ip() + "," + 
        to_string(frontend_address.port()) + "\n");

    config_file_.write("private-key-file=" + path_to_proxy_key + "\n");
    config_file_.write("certificate-file=" + path_to_proxy_cert + "\n");

    config_file_.write("daemon=yes\n");
    config_file_.write("pid-file=" + pidfile_.name() + "\n");

    if (backend_address.port() == 443) {
      // Handle HTTPS
      config_file_.write("backend=" + backend_address.ip() + "," + 
          to_string(backend_address.port()) + ";/;tls\n");
      config_file_.write("insecure=yes\n");
      config_file_.write("client-cert-file=" + 
          string(MOD_SSL_CERTIFICATE_FILE) + "\n");
    } else {
      // Handle HTTP case
      config_file_.write("backend=" + backend_address.ip() + ",80\n");
    }

    // Add logging.
    config_file_.write("log-level=INFO\n");
    config_file_.write("errorlog-file=" + path_prefix + "/error-logs/" + page + ".log\n");

    run( { path_to_proxy, "--conf=" + config_file_.name() } );
*/


    cout << "Config name:" << config_file_.name() << endl;
    cout << "IP:" << frontend_address.ip() << endl;
    cout << "Frontend: " << frontend_address.str() << " backend: " << backend_address.str() << endl;
    cout << "Proxy key: " << path_to_proxy_key << " Proxy cert: " << path_to_proxy_cert << endl;

    string path_prefix = PATH_PREFIX;

    //hosts
    config_file_.write("hosts:\n");
    //might need to change to host name
    //config_file_.write("  \"" + frontend_address.ip() + "\":\n");
    config_file_.write("  \"abcnews.go.com\":\n");

    // listen
    config_file_.write("    listen:\n");

//     config_file_.write("      port:  8081\n");
    config_file_.write("      port: " + to_string(frontend_address.port()) + "\n");

    config_file_.write("      ssl:\n");
    config_file_.write("        certificate-file: " + path_to_proxy_cert + "\n");
    config_file_.write("        key-file: " + path_to_proxy_key + "\n");

    // path
    config_file_.write("    paths:\n");

    config_file_.write("      /:\n");

    config_file_.write("        proxy.reverse.url: \"http://" + backend_address.ip() + ":" +
        to_string(backend_address.port()) + "\"\n");

    // casper
    config_file_.write("http2-casper:\n");
    config_file_.write("   capacity-bits:  13\n");
    config_file_.write("   tracking-types: all\n");

    //log
    config_file_.write("access-log: " + path_prefix + "/error-logs/" + page + ".log\n");
    config_file_.write("error-log: " + path_prefix + "/error-logs/" + page + ".log\n");

    //run( { path_to_proxy, "-c" ,  config_file_.name() , "-m", "daemon" } );
    run( { path_to_proxy, "-c" ,  config_file_.name() } );

}

ReverseProxy::~ReverseProxy()
{
    if ( moved_away_ ) { return; }

    try {
      ifstream pidfile (pidfile_.name());
      string line;
      if (pidfile.is_open()) {
        getline(pidfile, line);
      }
      pidfile.close();
      int pid_int = atoi(line.c_str());
      if (pid_int != 0) {
        kill(pid_int, SIGTERM);
      }
    } catch ( const exception & e ) {
      print_exception( e );
    }
}

ReverseProxy::ReverseProxy( ReverseProxy && other )
    : config_file_( move( other.config_file_ ) ),
      pidfile_( move( other.pidfile_ ) ),
      moved_away_( false )
{
    other.moved_away_ = true;
}
