#include <stdio.h>
#include <thread>
#include <chrono>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <csi_http_client/http_client.h>

/*
void handle_get(csi::http_client::call_context* state) {
  if(state->http_result() >= 200 && state->http_result() < 300)
    BOOST_LOG_TRIVIAL(info) << "handle_get data: " << state->uri() << " got " << state->rx_content_length() << " bytes, time=" << state->milliseconds() << " ms";
  else
    BOOST_LOG_TRIVIAL(error) << "handle_get data: " << state->uri() << " HTTPRES = " << state->http_result();
}
*/

void log_result(csi::http_client::call_context::handle h) {
  if (h->ok()) {
    auto sz = h->rx_content_length();
    int kb_per_sec = 0;
    if (h->milliseconds() > 0) {
      kb_per_sec = sz / h->milliseconds();
    }
    BOOST_LOG_TRIVIAL(info) << "http get: " << h->uri() << " got " << h->rx_content_length() << " bytes, time=" << h->milliseconds() << " ms (" << kb_per_sec << " KB/s)";
  } else if (!h->transport_result()) {
    BOOST_LOG_TRIVIAL(error) << "transport failed";
  } else {
    BOOST_LOG_TRIVIAL(error) << "http get: " << h->uri() << " HTTPRES = " << h->http_result();
  }
}

#define NR_OF_URI 9
std::string uris[NR_OF_URI] =
{
  "http://188.138.9.246/S2/HLS_LIVE/cnn_turk/index.m3u8",
  "http://188.138.9.246/S2/HLS_LIVE/cnn_turk/1000/prog_index.m3u8",
  "http://188.138.9.246/S2/HLS_LIVE/cnn_turk/1000/segm035890.ts",
  "http://188.138.9.246/S2/HLS_LIVE/cnn_turk/1000/segm035811.ts",
  "http://188.138.9.246/S2/HLS_LIVE/cnn_turk/1000/segm035812.ts",
  "http://188.138.9.246/S2/HLS_LIVE/cnn_turk/1000/segm035813.ts",
  "http://188.138.9.246/S2/HLS_LIVE/cnn_turk/1000/segm035814.ts",
  "http://google.com",
  "http://dn.se"
};


std::string uri3 = "google.com";

int main(int argc, char **argv) {
  boost::asio::io_service io_service;
  boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::info);
  curl_global_init(CURL_GLOBAL_NOTHING); /* minimal */
  csi::http_client handler(io_service);
  /* enter io_service run loop */
  boost::thread th(boost::bind(&boost::asio::io_service::run, &io_service));

  for (int i = 0; i != NR_OF_URI; ++i) {
    log_result(handler.perform(csi::create_http_request(csi::http::GET, uris[i], {}, std::chrono::milliseconds(10000)), false));
  }

  handler.close();
  th.join();

  return 0;
}
