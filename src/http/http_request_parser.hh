/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef HTTP_REQUEST_PARSER_HH
#define HTTP_REQUEST_PARSER_HH

#include "http_message_sequence.hh"
#include "http_request.hh"

class HTTPRequestParser : public HTTPMessageSequence<HTTPRequest> {

public:
  std::string get_current_message_hostname(void) {
    if (message_in_progress_.state() == FIRST_LINE_PENDING) {
      return "";
    }
    return message_in_progress_.get_hostname();
  };

private:
  void initialize_new_message(void) override {}
};

#endif /* HTTP_REQUEST_PARSER_HH */
