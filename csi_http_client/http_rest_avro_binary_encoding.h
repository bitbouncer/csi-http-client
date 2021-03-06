#include <csi_http_client/http_client.h>
#include <csi_http_common/encoding/avro_raw_binary_encoding.h>
#pragma once

namespace csi {
  template<class Request>
  std::shared_ptr<http_client::call_context> create_avro_binary_rest(const std::string& uri, const Request& request, const std::vector<std::string>& headers, const std::chrono::milliseconds& timeout) {
    std::shared_ptr<http_client::call_context> p(new http_client::call_context(csi::http::POST, uri, headers, timeout));
    avro_raw_binary_encode(request, p->tx_content());
    return p;
  }
}