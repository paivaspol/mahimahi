/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef FORWARDER_HH
#define FORWARDER_HH

/* Packet Forwarder */

#include <string>

#include "system_runner.hh"
#include "address.hh"

/* RAII class to forward packets from one interface to another. */
class Forwarder
{
private:
    std::vector<std::string> first_forward_command;
    std::vector<std::string> second_forward_command;
    std::vector<std::string> third_forward_command;

public:
    Forwarder( const std::string from, const std::string to );
    ~Forwarder();
};

#endif /* FORWARDER_HH */
