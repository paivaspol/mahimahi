/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef HTTP_REQUEST_HH
#define HTTP_REQUEST_HH

#include "http_message.hh"

class HTTPRequest : public HTTPMessage {
private:
  /* for a request, will always be known */
  void calculate_expected_body_size(void) override;

  /* we have no complex bodies */
  size_t read_in_complex_body(const std::string &str) override;

  /* connection closed while body was pending */
  bool eof_in_body(void) const override;

  /* Represents the request id. */
  int request_id_ = -1;

public:
  void set_request_id(int request_id);

  int get_request_id(void) { return request_id_; }

  bool is_head(void) const;

  std::string get_url(void) const;

  using HTTPMessage::HTTPMessage;
};

#endif /* HTTP_REQUEST_HH */
