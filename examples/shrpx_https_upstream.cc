/*
 * Spdylay - SPDY Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_https_upstream.h"

#include <cassert>
#include <set>

#include "shrpx_client_handler.h"
#include "shrpx_downstream.h"
#include "shrpx_http.h"
#include "shrpx_config.h"
#include "shrpx_error.h"
#include "util.h"

using namespace spdylay;

namespace shrpx {

HttpsUpstream::HttpsUpstream(ClientHandler *handler)
  : handler_(handler),
    htp_(htparser_new())
{
  if(ENABLE_LOG) {
    LOG(INFO) << "HttpsUpstream ctor";
  }
  htparser_init(htp_, htp_type_request);
  htparser_set_userdata(htp_, this);
}

HttpsUpstream::~HttpsUpstream()
{
  free(htp_);
  for(std::deque<Downstream*>::iterator i = downstream_queue_.begin();
      i != downstream_queue_.end(); ++i) {
    delete *i;
  }
}

namespace {
int htp_msg_begin(htparser *htp)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "<upstream>::<https> request start";
  }
  HttpsUpstream *upstream;
  upstream = reinterpret_cast<HttpsUpstream*>(htparser_get_userdata(htp));
  Downstream *downstream = new Downstream(upstream, 0, 0);
  upstream->add_downstream(downstream);
  return 0;
}
} // namespace

namespace {
int htp_methodcb(htparser *htp, const char *data, size_t len)
{
  HttpsUpstream *upstream;
  upstream = reinterpret_cast<HttpsUpstream*>(htparser_get_userdata(htp));
  Downstream *downstream = upstream->get_last_downstream();
  downstream->set_request_method(std::string(data, len));
  return 0;
}
} // namespace

namespace {
int htp_uricb(htparser *htp, const char *data, size_t len)
{
  HttpsUpstream *upstream;
  upstream = reinterpret_cast<HttpsUpstream*>(htparser_get_userdata(htp));
  Downstream *downstream = upstream->get_last_downstream();
  downstream->set_request_path(std::string(data, len));
  return 0;
}
} // namespace

namespace {
int htp_hdrs_begincb(htparser *htp)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "<upstream>::<https> request headers start";
  }
  HttpsUpstream *upstream;
  upstream = reinterpret_cast<HttpsUpstream*>(htparser_get_userdata(htp));
  Downstream *downstream = upstream->get_last_downstream();

  int version = htparser_get_major(htp)*100 + htparser_get_minor(htp);
  if(version < 101) {
    downstream->set_request_connection_close(true);
  }
  return 0;
}
} // namespace

namespace {
int htp_hdr_keycb(htparser *htp, const char *data, size_t len)
{
  HttpsUpstream *upstream;
  upstream = reinterpret_cast<HttpsUpstream*>(htparser_get_userdata(htp));
  Downstream *downstream = upstream->get_last_downstream();
  downstream->add_request_header(std::string(data, len), "");
  return 0;
}
} // namespace

namespace {
int htp_hdr_valcb(htparser *htp, const char *data, size_t len)
{
  HttpsUpstream *upstream;
  upstream = reinterpret_cast<HttpsUpstream*>(htparser_get_userdata(htp));
  Downstream *downstream = upstream->get_last_downstream();
  downstream->set_last_request_header_value(std::string(data, len));
  return 0;
}
} // namespace

namespace {
int htp_hdrs_completecb(htparser *htp)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "<upstream>::<https> request headers complete";
  }
  HttpsUpstream *upstream;
  upstream = reinterpret_cast<HttpsUpstream*>(htparser_get_userdata(htp));
  Downstream *downstream = upstream->get_last_downstream();

  downstream->push_request_headers();
  downstream->set_request_state(Downstream::HEADER_COMPLETE);

  downstream->start_connection();
  return 0;
}
} // namespace

namespace {
int htp_bodycb(htparser *htp, const char *data, size_t len)
{
  HttpsUpstream *upstream;
  upstream = reinterpret_cast<HttpsUpstream*>(htparser_get_userdata(htp));
  Downstream *downstream = upstream->get_last_downstream();
  downstream->push_upload_data_chunk(reinterpret_cast<const uint8_t*>(data),
                                     len);
  return 0;
}
} // namespace

namespace {
int htp_msg_completecb(htparser *htp)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "<upstream>::<https> request complete";
  }
  HttpsUpstream *upstream;
  upstream = reinterpret_cast<HttpsUpstream*>(htparser_get_userdata(htp));
  Downstream *downstream = upstream->get_last_downstream();
  downstream->end_upload_data();
  downstream->set_request_state(Downstream::MSG_COMPLETE);
  // Stop further processing to complete this request
  return 1;
}
} // namespace

namespace {
htparse_hooks htp_hooks = {
  htp_msg_begin, /*htparse_hook      on_msg_begin;*/
  htp_methodcb, /*htparse_data_hook method;*/
  0, /* htparse_data_hook scheme;*/
  0, /* htparse_data_hook host; */
  0, /* htparse_data_hook port; */
  0, /* htparse_data_hook path; */
  0, /* htparse_data_hook args; */
  htp_uricb, /* htparse_data_hook uri; */
  htp_hdrs_begincb, /* htparse_hook      on_hdrs_begin; */
  htp_hdr_keycb, /* htparse_data_hook hdr_key; */
  htp_hdr_valcb, /* htparse_data_hook hdr_val; */
  htp_hdrs_completecb, /* htparse_hook      on_hdrs_complete; */
  0, /*htparse_hook      on_new_chunk;*/
  0, /*htparse_hook      on_chunk_complete;*/
  0, /*htparse_hook      on_chunks_complete;*/
  htp_bodycb, /* htparse_data_hook body; */
  htp_msg_completecb /* htparse_hook      on_msg_complete;*/
};
} // namespace

std::set<HttpsUpstream*> cache;

// on_read() does not consume all available data in input buffer if
// one http request is fully received.
int HttpsUpstream::on_read()
{
  if(cache.count(this) == 0) {
    LOG(INFO) << "HttpsUpstream::on_read";
    cache.insert(this);
  }
  bufferevent *bev = handler_->get_bev();
  evbuffer *input = bufferevent_get_input(bev);
  unsigned char *mem = evbuffer_pullup(input, -1);
  int nread = htparser_run(htp_, &htp_hooks,
                           reinterpret_cast<const char*>(mem),
                           evbuffer_get_length(input));
  evbuffer_drain(input, nread);
  htpparse_error htperr = htparser_get_error(htp_);
  if(htperr == htparse_error_user) {
    bufferevent_disable(bev, EV_READ);
    if(ENABLE_LOG) {
      LOG(INFO) << "<upstream> remaining bytes " << evbuffer_get_length(input);
    }
  } else if(htperr != htparse_error_none) {
    if(ENABLE_LOG) {
      LOG(INFO) << "<upstream> http parse failure: "
                << htparser_get_strerror(htp_);
    }
    return SHRPX_ERR_HTTP_PARSE;
  }
  return 0;
}

int HttpsUpstream::on_event()
{
  return 0;
}

ClientHandler* HttpsUpstream::get_client_handler() const
{
  return handler_;
}

void HttpsUpstream::resume_read()
{
  bufferevent_enable(handler_->get_bev(), EV_READ);
  // Process remaining data in input buffer here.
  on_read();
}

namespace {
void https_downstream_readcb(bufferevent *bev, void *ptr)
{
  Downstream *downstream = reinterpret_cast<Downstream*>(ptr);
  HttpsUpstream *upstream;
  upstream = static_cast<HttpsUpstream*>(downstream->get_upstream());
  int rv = downstream->parse_http_response();
  if(rv == 0) {
    if(downstream->get_response_state() == Downstream::MSG_COMPLETE) {
      assert(downstream == upstream->get_top_downstream());
      upstream->pop_downstream();
      delete downstream;
      upstream->resume_read();
    }
  } else {
    if(downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
      delete upstream->get_client_handler();
    } else {
      upstream->error_reply(downstream, 502);
      assert(downstream == upstream->get_top_downstream());
      upstream->pop_downstream();
      delete downstream;
      upstream->resume_read();
    }
  }
}
} // namespace

namespace {
void https_downstream_writecb(bufferevent *bev, void *ptr)
{
}
} // namespace

namespace {
void https_downstream_eventcb(bufferevent *bev, short events, void *ptr)
{
  Downstream *downstream = reinterpret_cast<Downstream*>(ptr);
  HttpsUpstream *upstream;
  upstream = static_cast<HttpsUpstream*>(downstream->get_upstream());
  if(events & BEV_EVENT_CONNECTED) {
    if(ENABLE_LOG) {
      LOG(INFO) << "<downstream> Connection established. " << downstream;
    }
  }
  if(events & BEV_EVENT_EOF) {
    if(ENABLE_LOG) {
      LOG(INFO) << "<downstream> EOF stream_id="
                << downstream->get_stream_id();
    }
    if(downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
      // Server may indicate the end of the request by EOF
      if(ENABLE_LOG) {
        LOG(INFO) << "<downstream> Assuming content-length is 0 byte";
      }
      upstream->on_downstream_body_complete(downstream);
      //downstream->set_response_state(Downstream::MSG_COMPLETE);
    } else if(downstream->get_response_state() == Downstream::MSG_COMPLETE) {
      // Nothing to do
    } else {
      // error
      if(ENABLE_LOG) {
        LOG(INFO) << "<downstream> Treated as error";
      }
      upstream->error_reply(downstream, 502);
    }
    upstream->pop_downstream();
    delete downstream;
    upstream->resume_read();
  } else if(events & (BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
    if(ENABLE_LOG) {
      LOG(INFO) << "<downstream> error/timeout. " << downstream;
    }
    if(downstream->get_response_state() == Downstream::INITIAL) {
      int status;
      if(events & BEV_EVENT_TIMEOUT) {
        status = 504;
      } else {
        status = 502;
      }
      upstream->error_reply(downstream, status);
    }
    upstream->pop_downstream();
    delete downstream;
    upstream->resume_read();
  }
}
} // namespace

void HttpsUpstream::error_reply(Downstream *downstream, int status_code)
{
  std::string html = http::create_error_html(status_code);
  std::stringstream ss;
  ss << "HTTP/1.1 " << http::get_status_string(status_code) << "\r\n"
     << "Server: " << get_config()->server_name << "\r\n"
     << "Content-Length: " << html.size() << "\r\n"
     << "Content-Type: " << "text/html; charset=UTF-8\r\n"
     << "\r\n";
  std::string header = ss.str();
  evbuffer *output = bufferevent_get_output(handler_->get_bev());
  evbuffer_add(output, header.c_str(), header.size());
  evbuffer_add(output, html.c_str(), html.size());
  downstream->set_response_state(Downstream::MSG_COMPLETE);
}

bufferevent_data_cb HttpsUpstream::get_downstream_readcb()
{
  return https_downstream_readcb;
}

bufferevent_data_cb HttpsUpstream::get_downstream_writecb()
{
  return https_downstream_writecb;
}

bufferevent_event_cb HttpsUpstream::get_downstream_eventcb()
{
  return https_downstream_eventcb;
}

void HttpsUpstream::add_downstream(Downstream *downstream)
{
  downstream_queue_.push_back(downstream);
}

void HttpsUpstream::pop_downstream()
{
  downstream_queue_.pop_front();
}

Downstream* HttpsUpstream::get_top_downstream()
{
  return downstream_queue_.front();
}

Downstream* HttpsUpstream::get_last_downstream()
{
  return downstream_queue_.back();
}

int HttpsUpstream::on_downstream_header_complete(Downstream *downstream)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "<downstream> on_downstream_header_complete";
  }
  std::string hdrs = "HTTP/1.1 ";
  hdrs += http::get_status_string(downstream->get_response_http_status());
  hdrs += "\r\n";
  for(Headers::const_iterator i = downstream->get_response_headers().begin();
      i != downstream->get_response_headers().end(); ++i) {
    if(util::strieq((*i).first.c_str(), "keep-alive") || // HTTP/1.0?
       util::strieq((*i).first.c_str(), "connection") ||
       util:: strieq((*i).first.c_str(), "proxy-connection")) {
      // These are ignored
    } else {
      if(util::strieq((*i).first.c_str(), "server")) {
        hdrs += "Server: ";
        hdrs += get_config()->server_name;
      } else {
        hdrs += (*i).first;
        hdrs += ": ";
        hdrs += (*i).second;
      }
      hdrs += "\r\n";
    }
  }
  if(downstream->get_request_connection_close()) {
    hdrs += "Connection: close\r\n";
  }
  hdrs += "\r\n";
  if(ENABLE_LOG) {
    LOG(INFO) << "<upstream>::<https> Response headers\n" << hdrs;
  }
  evbuffer *output = bufferevent_get_output(handler_->get_bev());
  evbuffer_add(output, hdrs.c_str(), hdrs.size());
  return 0;
}

int HttpsUpstream::on_downstream_body(Downstream *downstream,
                                      const uint8_t *data, size_t len)
{
  int rv;
  evbuffer *output = bufferevent_get_output(handler_->get_bev());
  if(downstream->get_chunked_response()) {
    char chunk_size_hex[16];
    rv = snprintf(chunk_size_hex, sizeof(chunk_size_hex), "%X\r\n",
                  static_cast<unsigned int>(len));
    evbuffer_add(output, chunk_size_hex, rv);
  }
  evbuffer_add(output, data, len);
  return 0;
}

int HttpsUpstream::on_downstream_body_complete(Downstream *downstream)
{
  if(downstream->get_chunked_response()) {
    evbuffer *output = bufferevent_get_output(handler_->get_bev());
    evbuffer_add(output, "0\r\n\r\n", 5);
  }
  if(ENABLE_LOG) {
    LOG(INFO) << "<downstream> on_downstream_body_complete";
  }
  if(downstream->get_request_connection_close()) {
    ClientHandler *handler = downstream->get_upstream()->get_client_handler();
    handler->set_should_close_after_write(true);
  }
  return 0;
}

} // namespace shrpx
