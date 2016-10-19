//
// http_client.h
// ~~~~~~~~~~
// Copyright 2014 Svante Karlsson CSI AB (svante.karlsson at csi dot se)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <chrono>
#include <future>
#include <sstream>
#include <curl/curl.h>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/function.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string.hpp>
#include <csi_http_common/csi_http.h>
#include <csi_http_common/header.h>
#include "internal/spinlock.h"
#pragma once

namespace csi {
class http_client
{
  public:

  // dummy implementetation
  class buffer
  {
    public:
    buffer() { _data.reserve(32 * 1024); }
    void reserve(size_t sz) { _data.reserve(sz); }
    void append(const uint8_t* p, size_t sz) { _data.insert(_data.end(), p, p + sz); }
    void append(uint8_t s) { _data.push_back(s); }
    const uint8_t* data() const { return &_data[0]; }
    size_t size() const { return _data.size(); }
    void pop_back() { _data.resize(_data.size() - 1); }

    private:
    std::vector<uint8_t> _data;
  };

  class call_context
  {
    friend http_client;
    public:
    typedef boost::function <void(std::shared_ptr<call_context>)> callback;
    typedef std::shared_ptr<call_context>                         handle;

    call_context(csi::http::method_t method, const std::string& uri, const std::vector<std::string>& headers, const std::chrono::milliseconds& timeout, bool verbose = false) :
      _method(method),
      _uri(uri),
      _curl_verbose(verbose),
      _timeoutX(timeout),
      _http_result(csi::http::undefined),
      _tx_headers(headers),
      _curl_easy(NULL),
      _curl_headerlist(NULL),
      _curl_done(false) {
      _curl_easy = curl_easy_init();
    }

    ~call_context() {
      if (_curl_easy)
        curl_easy_cleanup(_curl_easy);
      if (_curl_headerlist)
        curl_slist_free_all(_curl_headerlist);
    }

    public:
    inline void curl_start(call_context::handle h) { _curl_shared = h; }
    inline void curl_stop() { _curl_shared.reset(); }
    inline call_context::handle curl_handle() { return _curl_shared; }
    inline int64_t milliseconds() const { std::chrono::milliseconds duration = std::chrono::duration_cast<std::chrono::milliseconds>(_end_ts - _start_ts); return duration.count(); }
    inline int64_t microseconds() const { std::chrono::microseconds duration = std::chrono::duration_cast<std::chrono::microseconds>(_end_ts - _start_ts); return duration.count(); }
    std::string tx_content() const { return _tx_stream.str(); }
    const char* rx_content() const { return _rx_buffer.size() ? (const char*) _rx_buffer.data() : ""; }
    inline size_t tx_content_length() const { return _tx_stream.str().size(); }
    inline size_t rx_content_length() const { return _rx_buffer.size(); }
    inline int    rx_kb_per_sec() const { auto sz = rx_content_length(); int64_t ms = milliseconds();  return (int) (ms == 0 ? 0 : sz / ms); }
    inline void set_verbose(bool state) { _curl_verbose = state; }
    inline const std::string& uri() const { return _uri; }
    inline csi::http::status_type http_result() const { return _http_result; }
    inline bool transport_result() const { return _transport_ok; }
    inline bool ok() const { return _transport_ok && (_http_result >= 200) && (_http_result < 300); }

    std::string get_rx_header(const std::string& header) const {
      for (std::vector<csi::http::header_t>::const_iterator i = _rx_headers.begin(); i != _rx_headers.end(); ++i) {
        if (boost::iequals(header, i->name))
          return i->value;
      }
      return "";
    }

    private:
    csi::http::method_t                   _method;
    std::string                           _uri;
    std::vector<std::string>              _tx_headers;
    std::vector<csi::http::header_t>      _rx_headers;
    std::chrono::steady_clock::time_point _start_ts;
    std::chrono::steady_clock::time_point _end_ts;
    std::chrono::milliseconds             _timeoutX;
    callback                              _callback;

    //TX
    std::stringstream                     _tx_stream; // temporary for test...
    //RX
    buffer                                _rx_buffer;

    csi::http::status_type                _http_result;
    bool                                  _transport_ok;

    //curl stuff
    CURL*                                 _curl_easy;
    curl_slist*                           _curl_headerlist;
    bool                                  _curl_done;
    handle                                _curl_shared; // used to keep object alive when only curl knows about the context
    bool                                  _curl_verbose;
  };

  http_client(boost::asio::io_service& io_service) :
    _io_service(io_service),
    _timer(_io_service),
    _closing(false) {
    _multi = curl_multi_init();
    curl_multi_setopt(_multi, CURLMOPT_SOCKETFUNCTION, _sock_cb);
    curl_multi_setopt(_multi, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(_multi, CURLMOPT_TIMERFUNCTION, _multi_timer_cb);
    curl_multi_setopt(_multi, CURLMOPT_TIMERDATA, this);
  }

  ~http_client() {
    close();
  }

  void close() {
    _closing = true;
    BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION;
    _timer.cancel();
    if (_multi) {
      BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << "before curl_multi_cleanup(_multi);";
      _io_service.post([this]() {
        BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << ", curl_multi_cleanup(_multi): " << _multi;
        curl_multi_cleanup(_multi);
        _multi = NULL;
      });
    }
  }


  bool done() {
    return (_curl_handles_still_running == 0);
  }

  //void perform_async(call_context::handle, call_context::callback cb);
  //call_context::handle perform(call_context::handle, bool verbose = false);
  
  void http_client::perform_async(call_context::handle request, call_context::callback cb) {
    request->_callback = cb;
    _io_service.post([this, request]() {
      _perform(request);
    });
    //_io_service.post(boost::bind(&http_client::_perform, this, request));
  }

  csi::http_client::call_context::handle http_client::perform(call_context::handle request, bool verbose) {
    request->_curl_verbose = verbose;
    std::promise<csi::http_client::call_context::handle> p;
    std::future<csi::http_client::call_context::handle>  f = p.get_future();
    perform_async(request, [&p](http_client::call_context::handle result) {
      p.set_value(result);
    });
    f.wait();
    return f.get();
  }


  protected:
  void _perform(call_context::handle request) {
    request->curl_start(request); // increments usage count and keeps object around until curl thinks its done.

    curl_easy_setopt(request->_curl_easy, CURLOPT_OPENSOCKETFUNCTION, &http_client::_opensocket_cb);
    curl_easy_setopt(request->_curl_easy, CURLOPT_OPENSOCKETDATA, this);

    curl_easy_setopt(request->_curl_easy, CURLOPT_CLOSESOCKETFUNCTION, &http_client::_closesocket_cb);
    curl_easy_setopt(request->_curl_easy, CURLOPT_CLOSESOCKETDATA, this);

    //SSL OPTIONS
    //set up curls cerfificate check
    //curl_easy_setopt(m_hcURL, CURLOPT_SSL_VERIFYPEER, 1)); 
    //curl_easy_setopt(m_hcURL, CURLOPT_CAINFO,"curl-ca-bundle.cer")); 

    // for now skip ca check
    //curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYPEER, 0); 
    // retrieve cert info
    //curl_easy_setopt(_curl, CURLOPT_CERTINFO, 1); 

    if (request->_curl_verbose)
      curl_easy_setopt(request->_curl_easy, CURLOPT_VERBOSE, 1L);


    switch (request->_method) {
      case csi::http::GET:
        curl_easy_setopt(request->_curl_easy, CURLOPT_HTTPGET, 1);
        curl_easy_setopt(request->_curl_easy, CURLOPT_FOLLOWLOCATION, 1L);
        break;

      case csi::http::PUT:
        curl_easy_setopt(request->_curl_easy, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(request->_curl_easy, CURLOPT_INFILESIZE_LARGE, (curl_off_t) request->tx_content_length());
        break;

      case csi::http::POST:
        curl_easy_setopt(request->_curl_easy, CURLOPT_POST, 1);
        curl_easy_setopt(request->_curl_easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) request->tx_content_length()); // must be different in post and put???
        break;

      default:
        assert(false);
    };

    curl_easy_setopt(request->_curl_easy, CURLOPT_URL, request->_uri.c_str());

    /* the request */
    curl_easy_setopt(request->_curl_easy, CURLOPT_READFUNCTION, read_callback_std_stream);
    curl_easy_setopt(request->_curl_easy, CURLOPT_READDATA, &request->_tx_stream);

    /* the reply */
    //curl_easy_setopt(request->_curl_easy, CURLOPT_WRITEFUNCTION, write_callback_std_stream);
    //curl_easy_setopt(request->_curl_easy, CURLOPT_WRITEDATA, &request->_rx_stream);
    curl_easy_setopt(request->_curl_easy, CURLOPT_WRITEFUNCTION, write_callback_buffer);
    curl_easy_setopt(request->_curl_easy, CURLOPT_WRITEDATA, &request->_rx_buffer);


    curl_easy_setopt(request->_curl_easy, CURLOPT_PRIVATE, request.get());
    curl_easy_setopt(request->_curl_easy, CURLOPT_LOW_SPEED_TIME, 3L);
    curl_easy_setopt(request->_curl_easy, CURLOPT_LOW_SPEED_LIMIT, 10L);

    // if this is a resend we have to clear this before using it again
    if (request->_curl_headerlist) {
      //assert(false); // is this really nessessary??
      curl_slist_free_all(request->_curl_headerlist);
      request->_curl_headerlist = NULL;
    }

    static const char buf[] = "Expect:";
    /* initalize custom header list (stating that Expect: 100-continue is not wanted */
    request->_curl_headerlist = curl_slist_append(request->_curl_headerlist, buf);
    request->_curl_headerlist = curl_slist_append(request->_curl_headerlist, "User-Agent:csi-http/0.1");
    for (std::vector<std::string>::const_iterator i = request->_tx_headers.begin(); i != request->_tx_headers.end(); ++i)
      request->_curl_headerlist = curl_slist_append(request->_curl_headerlist, i->c_str());
    curl_easy_setopt(request->_curl_easy, CURLOPT_HTTPHEADER, request->_curl_headerlist);
    curl_easy_setopt(request->_curl_easy, CURLOPT_TIMEOUT_MS, request->_timeoutX.count());
    curl_easy_setopt(request->_curl_easy, CURLOPT_HEADERDATA, &request->_rx_headers);
    curl_easy_setopt(request->_curl_easy, CURLOPT_HEADERFUNCTION, parse_headers);
    curl_easy_setopt(request->_curl_easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(request->_curl_easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

    //SSL OPTIONS
    // for now skip ca check
    curl_easy_setopt(request->_curl_easy, CURLOPT_SSL_VERIFYPEER, 0);
    // retrieve cert info
    curl_easy_setopt(request->_curl_easy, CURLOPT_CERTINFO, 1);
    request->_start_ts = std::chrono::steady_clock::now();
    BOOST_LOG_TRIVIAL(debug) << BOOST_CURRENT_FUNCTION << ", method: " << to_string(request->_method) << ", uri: " << request->_uri << ", content_length: " << request->tx_content_length();
    CURLMcode rc = curl_multi_add_handle(_multi, request->_curl_easy);
  }
  
  // must not be called within curl callbacks - post a asio message instead
  void _poll_remove(call_context::handle h) {
    BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << ", handle: " << h->_curl_easy;
    curl_multi_remove_handle(_multi, h->_curl_easy);
  }
  
  // CURL CALLBACKS
  curl_socket_t _opensocket_cb(void *clientp, curlsocktype purpose, struct curl_sockaddr *address) {
    return ((http_client*) clientp)->opensocket_cb(purpose, address);
  }

  curl_socket_t opensocket_cb(curlsocktype purpose, struct curl_sockaddr *address) {
    /* IPV4 */
    if (purpose == CURLSOCKTYPE_IPCXN && address->family == AF_INET) {
      /* create a tcp socket object */
      boost::asio::ip::tcp::socket *tcp_socket = new boost::asio::ip::tcp::socket(_io_service);

      /* open it and get the native handle*/
      boost::system::error_code ec;
      tcp_socket->open(boost::asio::ip::tcp::v4(), ec);

      if (ec) {
        BOOST_LOG_TRIVIAL(error) << this << ", " << BOOST_CURRENT_FUNCTION << ", open failed, socket: " << ec << ", (" << ec.message() << ")";
        delete tcp_socket;
        return CURL_SOCKET_BAD;
      }

      curl_socket_t sockfd = tcp_socket->native_handle();
      /* save it for monitoring */
      {
        csi::http_internal::spinlock::scoped_lock xxx(_spinlock);
        _socket_map.insert(std::pair<curl_socket_t, boost::asio::ip::tcp::socket *>(sockfd, tcp_socket));
      }
      BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << " open ok, socket: " << sockfd;
      return sockfd;
    }
    //// IPV6
    //if (purpose == CURLSOCKTYPE_IPCXN && address->family == AF_INET6)
    //{
    //    /* create a tcp socket object */
    //    boost::asio::ip::tcp::socket *tcp_socket = new boost::asio::ip::tcp::socket(_io_service);

    //    /* open it and get the native handle*/
    //    boost::system::error_code ec;
    //    tcp_socket->open(boost::asio::ip::tcp::v6(), ec);

    //    if (ec)
    //    {
    //        BOOST_LOG_TRIVIAL(error) << this << ", " << BOOST_CURRENT_FUNCTION << ", open failed, socket: " << ec << ", (" << ec.message() << ")";
    //        delete tcp_socket;
    //        return CURL_SOCKET_BAD;
    //    }

    //    curl_socket_t sockfd = tcp_socket->native_handle();
    //    /* save it for monitoring */
    //    {
    //        csi::http_internal::spinlock::scoped_lock xxx(_spinlock);
    //        _socket_map.insert(std::pair<curl_socket_t, boost::asio::ip::tcp::socket *>(sockfd, tcp_socket));
    //    }
    //    BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << " open ok, socket: " << sockfd;
    //    return sockfd;
    //}

    BOOST_LOG_TRIVIAL(error) << "http_client::opensocket_cb unsupported address family";
    return CURL_SOCKET_BAD;
  }
    
  static int _sock_cb(CURL *e, curl_socket_t s, int what, void *user_data, void* per_socket_user_data) {
    return ((http_client*) user_data)->sock_cb(e, s, what, per_socket_user_data);
  }

  int sock_cb(CURL *e, curl_socket_t s, int what, void* per_socket_user_data) {
    if (what == CURL_POLL_REMOVE) {
      call_context* context = NULL;
      curl_easy_getinfo(e, CURLINFO_PRIVATE, &context);
      assert(context);
      if (!context) {
        BOOST_LOG_TRIVIAL(warning) << this << ", " << BOOST_CURRENT_FUNCTION << ", CURL_POLL_REMOVE, socket: " << s << " - no context, skipping";
        return 0;
      }
      // do nothing and hope we get a socket close callback???? kolla om det är rätt...
      // we cannot close or destroy the boost socket since this in inside callback - it is still used...
      return 0;
    }

    boost::asio::ip::tcp::socket* tcp_socket = (boost::asio::ip::tcp::socket*) per_socket_user_data;
    call_context* context = NULL;
    curl_easy_getinfo(e, CURLINFO_PRIVATE, &context);
    assert(context);
    if (!tcp_socket) {
      //we try to find the data in our own mapping
      //if we find it - register this to curl so we dont have to do this every time.
      {
        csi::http_internal::spinlock::scoped_lock xxx(_spinlock);
        std::map<curl_socket_t, boost::asio::ip::tcp::socket *>::iterator it = _socket_map.find(s);
        if (it != _socket_map.end()) {
          tcp_socket = it->second;
          curl_multi_assign(_multi, s, tcp_socket);
        }
      }

      if (!tcp_socket) {
        BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << ", socket: " << s << " is a c - ares socket, ignoring";
        return 0;
      }
    }

    switch (what) {
      case CURL_POLL_IN:
        BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << ", CURL_POLL_IN, socket: " << s;
        tcp_socket->async_read_some(boost::asio::null_buffers(), [this, tcp_socket, context](const boost::system::error_code& ec, std::size_t bytes_transferred) {
          socket_rx_cb(ec, tcp_socket, context->curl_handle());
        });
        break;
      case CURL_POLL_OUT:
        BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << ", CURL_POLL_OUT, socket: " << s;
        tcp_socket->async_write_some(boost::asio::null_buffers(), [this, tcp_socket, context](const boost::system::error_code& ec, std::size_t bytes_transferred) {
          socket_tx_cb(ec, tcp_socket, context->curl_handle());
        });
        break;
      case CURL_POLL_INOUT:
        BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << ", CURL_POLL_INOUT, socket: " << s;
        tcp_socket->async_read_some(boost::asio::null_buffers(), [this, tcp_socket, context](const boost::system::error_code& ec, std::size_t bytes_transferred) {
          socket_rx_cb(ec, tcp_socket, context->curl_handle());
        });
        tcp_socket->async_write_some(boost::asio::null_buffers(), [this, tcp_socket, context](const boost::system::error_code& ec, std::size_t bytes_transferred) {
          socket_tx_cb(ec, tcp_socket, context->curl_handle());
        });
        break;
      case CURL_POLL_REMOVE:
        // should never happen - handled above
        break;
    };
    return 0;
  }

  //BOOST EVENTS
  void socket_rx_cb(const boost::system::error_code& ec, boost::asio::ip::tcp::socket * tcp_socket, call_context::handle context) {
    if (!ec && !context->_curl_done) {
      CURLMcode rc = curl_multi_socket_action(_multi, tcp_socket->native_handle(), CURL_CSELECT_IN, &_curl_handles_still_running);
      if (!context->_curl_done)
        tcp_socket->async_read_some(boost::asio::null_buffers(), [this, tcp_socket, context](const boost::system::error_code& ec, std::size_t bytes_transferred) {
        socket_rx_cb(ec, tcp_socket, context);
      });
      check_completed();
    }
  }

  void socket_tx_cb(const boost::system::error_code& ec, boost::asio::ip::tcp::socket * tcp_socket, call_context::handle context)     {
    if (!ec) {
      CURLMcode rc = curl_multi_socket_action(_multi, tcp_socket->native_handle(), CURL_CSELECT_OUT, &_curl_handles_still_running);
      check_completed();
    }
  }

  void timer_cb(const boost::system::error_code & ec)
  {
    if (!ec) {
      // CURL_SOCKET_TIMEOUT, 0 is corrent on timeouts http://curl.haxx.se/libcurl/c/curl_multi_socket_action.html
      CURLMcode rc = curl_multi_socket_action(_multi, CURL_SOCKET_TIMEOUT, 0, &_curl_handles_still_running);
      check_completed();
      //check_multi_info(); //TBD kolla om denna ska vara här
    }
  }

  //void keepalivetimer_cb(const boost::system::error_code & error);
  void _asio_closesocket_cb(curl_socket_t item);

  //curl callbacks
  static int _multi_timer_cb(CURLM *multi, long timeout_ms, void *userp) {
    return ((http_client*) userp)->multi_timer_cb(multi, timeout_ms);
  }

  int multi_timer_cb(CURLM* multi, long timeout_ms) {
   /* cancel running timer */
    _timer.cancel();

    if (timeout_ms > 0) {
      if (!_closing) {
        _timer.expires_from_now(std::chrono::milliseconds(timeout_ms));
        _timer.async_wait([&](const boost::system::error_code& ec) {
          timer_cb(ec);
        });
      }
    } else {
      /* call timeout function immediately */
      boost::system::error_code error; /*success*/
      timer_cb(error);
    }
    check_completed(); // ska den vara här ???
    return 0;
  }

    
  static size_t         _write_cb(void *ptr, size_t size, size_t nmemb, void *data);
  //static curl_socket_t  _opensocket_cb(void* user_data, curlsocktype purpose, struct curl_sockaddr *address);
  
  static int _closesocket_cb(void* user_data, curl_socket_t item) {
    return ((http_client*) user_data)->closesocket_cb(item);
  }

  int closesocket_cb(curl_socket_t item) {
    BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << ", socket: " << item;
    {
      csi::http_internal::spinlock::scoped_lock xxx(_spinlock);
      std::map<curl_socket_t, boost::asio::ip::tcp::socket*>::iterator it = _socket_map.find(item);
      if (it != _socket_map.end()) {
        boost::system::error_code ec;
        it->second->cancel(ec);
        it->second->close(ec);
        boost::asio::ip::tcp::socket* s = it->second;

        //curl_multi_assign(_multi, it->first, NULL); // we need to remove this at once since curl likes to reuse sockets 
        _socket_map.erase(it);
        _io_service.post([this, s]() {
          BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << ", socket: " << s;
          delete s; // must be deleted after operations on socket completed. therefore the _io_service.post
        });
      }
    }
    return 0;
  }

  void check_completed() {
    /* call curl_multi_perform or curl_multi_socket_action first, then loop
    through and check if there are any transfers that have completed */

    CURLMsg* m = NULL;
    do {
      int msgq = 0;
      m = curl_multi_info_read(_multi, &msgq);
      if (m && (m->msg == CURLMSG_DONE)) {
        CURL *e = m->easy_handle;
        call_context* context = NULL;
        curl_easy_getinfo(e, CURLINFO_PRIVATE, &context);
        assert(context);

        long http_result = 0;
        CURLcode curl_res = curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &http_result);
        if (curl_res == CURLE_OK)
          context->_http_result = (csi::http::status_type) http_result;
        else
          context->_http_result = (csi::http::status_type) 0;

        context->_end_ts = std::chrono::steady_clock::now();
        context->_curl_done = true;
        context->_transport_ok = (http_result > 0);

        std::string content_length_str = context->get_rx_header("Content-Length");
        if (content_length_str.size()) {
          if (context->rx_content_length() != atoi(content_length_str.c_str())) {
            context->_transport_ok = false;
          }
        }


        BOOST_LOG_TRIVIAL(trace) << this << ", " << BOOST_CURRENT_FUNCTION << ", CURLMSG_DONE, http : " << to_string(context->_method) << " " << context->uri() << " res = " << http_result << " " << context->milliseconds() << " ms";
        call_context::handle h(context->curl_handle());

        if (context->_callback) {
          // lets make sure the character in the buffer after the content is NULL
          // this is convinient to make parsingfast without copying to string...
          // rapidxml/rapidjson et al...
          context->_rx_buffer.append(0);
          context->_rx_buffer.pop_back(); // don't change the size..

          context->_callback(h);
        }

        context->curl_stop();
        curl_easy_setopt(context->_curl_easy, CURLOPT_PRIVATE, NULL);
        curl_multi_remove_handle(_multi, h->_curl_easy);
        //_io_service.post(boost::bind(&http_client::_poll_remove, this, h)); // ???? direct call should be possible
        //curl_multi_remove_handle(_multi, e);
        //curl_easy_cleanup(e);
      }
    } while (m);
  }

  // static helpers
  static size_t write_callback_std_stream(void *ptr, size_t size, size_t nmemb, std::ostream* stream) {
    size_t sz = size*nmemb;
    BOOST_LOG_TRIVIAL(trace) << BOOST_CURRENT_FUNCTION << ", in size: " << sz;
    stream->write((char*) ptr, sz);
    return sz;
  }

  static size_t write_callback_buffer(void *ptr, size_t size, size_t nmemb, csi::http_client::buffer* buf) {
    size_t sz = size*nmemb;
    BOOST_LOG_TRIVIAL(trace) << BOOST_CURRENT_FUNCTION << ", in size: " << sz;
    buf->append((const uint8_t*) ptr, sz);
    return sz;
  }

  static size_t read_callback_std_stream(void *ptr, size_t size, size_t nmemb, std::istream* stream) {
    size_t max_sz = size*nmemb;
    stream->read((char*) ptr, max_sz);
    size_t actual = stream->gcount();
    BOOST_LOG_TRIVIAL(trace) << BOOST_CURRENT_FUNCTION << ", out size: " << actual;
    return actual;
  }

  static size_t parse_headers(void *buffer, size_t size, size_t nmemb, std::vector<csi::http::header_t>* v) {
    size_t sz = size * nmemb;;
    char* begin = (char*) buffer;
    if (v) {
      char* separator = (char*) memchr(begin, ':', sz);
      char* newline = (char*) memchr(begin, '\r', sz); // end of value
      if (separator && newline) {
        //since get_rx_header() is case insensitive - no need to transform here
        //std::transform(begin, separator, begin, ::tolower);
        char* value_begin = separator + 1;
        while (isspace(*value_begin)) value_begin++; // leading white spaces
        size_t datalen = newline - value_begin;
        v->emplace_back(csi::http::header_t(std::string(begin, separator), std::string(value_begin, newline)));
      }
    }
    return sz;
  }


  boost::asio::io_service&             _io_service;
  mutable csi::http_internal::spinlock _spinlock;

  // check all platforms before change
  // boost::asio::steady_timer can compile on linux for now 1.54 ubuntu 13.10
  // using old template instead
  //typedef boost::asio::basic_waitable_timer<std::chrono::steady_clock> timer;
  //timer                                                   _keepalive_timer;
  //timer                                                   _timer;
  //boost::asio::steady_timer                               _keepalive_timer;

  boost::asio::steady_timer                               _timer;

  std::map<curl_socket_t, boost::asio::ip::tcp::socket *> _socket_map;
  CURLM*                                                  _multi;
  int                                                     _curl_handles_still_running;
  bool                                                    _closing;
};

inline std::shared_ptr<http_client::call_context> create_http_request(csi::http::method_t method, const std::string& uri, const std::vector<std::string>& headers, const std::chrono::milliseconds& timeout, bool verbose = false) {
  std::shared_ptr<http_client::call_context> p(new http_client::call_context(method, uri, headers, timeout, verbose));
  return p;
}
}; // namespace 
