/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* RAII class to forward packets from one interface to another. */

#include <unistd.h>
#include <iostream>

#include "forwarder.hh"
#include "exception.hh"
#include "config.h"

using namespace std;

Forwarder::Forwarder( const string from, const string to ) :
    first_forward_command({"-i", from, "-o", to, "-j", "ACCEPT"}),
    second_forward_command({"-i", to, "-o", from, "-m", "state", "--state", "ESTABLISHED,RELATED",  "-j", "ACCEPT"}),
    third_forward_command({"POSTROUTING", "-o", to, "-j", "MASQUERADE" })
{
    // In order to forward packets from one interface to another, we need
    // the following iptables commands:
    //  $ sudo iptables -A FORWARD -i [from] -o [to] -j ACCEPT
    //  $ sudo iptables -A FORWARD -i [to] -o [from] -m state --state ESTABLISHED,RELATED -j ACCEPT
    //  $ sudo iptables -t nat -A POSTROUTING -o [to] -j MASQUERADE
    vector<string> first_forward = {IPTABLES, "-w", "-A", "FORWARD"};
    first_forward.insert(first_forward.end(), this->first_forward_command.begin(), this->first_forward_command.end());
    run(first_forward);

    vector<string> second_forward = {IPTABLES, "-w", "-A", "FORWARD"};
    second_forward.insert(second_forward.end(), this->second_forward_command.begin(), this->second_forward_command.end());
    run(second_forward);

    vector<string> third_forward = {IPTABLES, "-w", "-t", "nat", "-A"};
    third_forward.insert(third_forward.end(), this->third_forward_command.begin(), this->third_forward_command.end());
    run(third_forward);
}

Forwarder::~Forwarder() {
    try {
      vector<string> first_remove_command = {IPTABLES, "-w", "-D", "FORWARD"};
      first_remove_command.insert(first_remove_command.end(), this->first_forward_command.begin(), this->first_forward_command.end());

      vector<string> second_remove_command = {IPTABLES, "-w", "-D", "FORWARD"};
      second_remove_command.insert(second_remove_command.end(), this->second_forward_command.begin(), this->second_forward_command.end());

      vector< string > third_remove_command = { IPTABLES, "-w", "-t", "nat", "-D" };
      third_remove_command.insert(third_remove_command.end(), this->third_forward_command.begin(), this->third_forward_command.end());
    } catch ( const exception & e ) { /* don't throw from destructor */
        print_exception( e );
    }

}
