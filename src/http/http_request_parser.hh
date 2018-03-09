/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef HTTP_REQUEST_PARSER_HH
#define HTTP_REQUEST_PARSER_HH

#include "http_message_sequence.hh"
#include "http_request.hh"

class HTTPRequestParser : public HTTPMessageSequence<HTTPRequest> {
private:
  void initialize_new_message(void) override {}

public:
  int get_request_id(void) { return message_in_progress_.get_request_id(); }
  void set_request_id(int request_id) {
    message_in_progress_.set_request_id(request_id);
  }
};

#endif /* HTTP_REQUEST_PARSER_HH */
