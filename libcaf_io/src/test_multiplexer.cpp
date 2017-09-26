/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2017                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/io/network/test_multiplexer.hpp"

#include "caf/scheduler/abstract_coordinator.hpp"

#include "caf/io/scribe.hpp"
#include "caf/io/doorman.hpp"
#include "caf/io/dgram_servant.hpp"

namespace caf {
namespace io {
namespace network {

test_multiplexer::scribe_data::scribe_data(shared_buffer_type input,
                                           shared_buffer_type output)
    : vn_buf_ptr(std::move(input)),
      wr_buf_ptr(std::move(output)),
      vn_buf(*vn_buf_ptr),
      wr_buf(*wr_buf_ptr),
      stopped_reading(false),
      passive_mode(false),
      ack_writes(false) {
  // nop
}

test_multiplexer::doorman_data::doorman_data()
    : port(0),
      stopped_reading(false),
      passive_mode(false) {
  // nop
}

test_multiplexer::dgram_servant_data::dgram_servant_data(shared_job_buffer_type input,
                                                         shared_job_buffer_type output)
    : vn_buf_ptr(std::move(input)),
      wr_buf_ptr(std::move(output)),
      vn_buf(*vn_buf_ptr),
      wr_buf(*wr_buf_ptr),
      stopped_reading(false),
      passive_mode(false),
      ack_writes(false),
      port(0),
      local_port(0),
      datagram_size(1500) {
  // nop
}

test_multiplexer::test_multiplexer(actor_system* sys)
    : multiplexer(sys),
      tid_(std::this_thread::get_id()),
      inline_runnables_(0),
      servant_ids_(0) {
  CAF_ASSERT(sys != nullptr);
}

test_multiplexer::~test_multiplexer() {
  // get rid of extra ref count
  for (auto& ptr : resumables_)
    intrusive_ptr_release(ptr.get());
}

scribe_ptr test_multiplexer::new_scribe(native_socket) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  std::cerr << "test_multiplexer::add_tcp_scribe called with native socket"
            << std::endl;
  abort();
}

scribe_ptr test_multiplexer::new_scribe(connection_handle hdl) {
  CAF_LOG_TRACE(CAF_ARG(hdl));
  class impl : public scribe {
  public:
    impl(connection_handle ch, test_multiplexer* mpx) : scribe(ch), mpx_(mpx) {
      // nop
    }
    void configure_read(receive_policy::config config) override {
      mpx_->read_config(hdl()) = config;
    }
    void ack_writes(bool enable) override {
      mpx_->ack_writes(hdl()) = enable;
    }
    std::vector<char>& wr_buf() override {
      return mpx_->output_buffer(hdl());
    }
    std::vector<char>& rd_buf() override {
      return mpx_->input_buffer(hdl());
    }
    void stop_reading() override {
      mpx_->stopped_reading(hdl()) = true;
      detach(mpx_, false);
    }
    void flush() override {
      // nop
    }
    std::string addr() const override {
      return "test";
    }
    uint16_t port() const override {
      return static_cast<uint16_t>(hdl().id());
    }
    void add_to_loop() override {
      mpx_->passive_mode(hdl()) = false;
    }
    void remove_from_loop() override {
      mpx_->passive_mode(hdl()) = true;
    }
  private:
    test_multiplexer* mpx_;
  };
  CAF_LOG_DEBUG(CAF_ARG(hdl));
  auto sptr = make_counted<impl>(hdl, this);
  { // lifetime scope of guard
    guard_type guard{mx_};
    impl_ptr(hdl) = sptr;
  }
  CAF_LOG_INFO("opened connection" << sptr->hdl());
  return sptr;
}

expected<scribe_ptr> test_multiplexer::new_tcp_scribe(const std::string& host,
                                                      uint16_t port) {
  CAF_LOG_TRACE(CAF_ARG(host) << CAF_ARG(port));
  connection_handle hdl;
  { // lifetime scope of guard
    guard_type guard{mx_};
    auto i = scribes_.find(std::make_pair(host, port));
    if (i != scribes_.end()) {
      hdl = i->second;
      scribes_.erase(i);
    } else {
      return sec::cannot_connect_to_node;
    }
  }
  return new_scribe(hdl);
}

doorman_ptr test_multiplexer::new_doorman(native_socket) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  std::cerr << "test_multiplexer::add_tcp_doorman called with native socket"
            << std::endl;
  abort();
}

doorman_ptr test_multiplexer::new_doorman(accept_handle hdl, uint16_t port) {
  CAF_LOG_TRACE(CAF_ARG(hdl));
  class impl : public doorman {
  public:
    impl(accept_handle ah, test_multiplexer* mpx) : doorman(ah), mpx_(mpx) {
      // nop
    }
    bool new_connection() override {
      connection_handle ch;
      { // Try to get a connection handle of a pending connect.
        guard_type guard{mpx_->mx_};
        auto& pc = mpx_->pending_connects();
        auto i = pc.find(hdl());
        if (i == pc.end())
          return false;
        ch = i->second;
        pc.erase(i);
      }
      CAF_LOG_INFO("accepted connection" << ch << "on acceptor" << hdl());
      parent()->add_scribe(mpx_->new_scribe(ch));
      return doorman::new_connection(mpx_, ch);
    }
    void stop_reading() override {
      mpx_->stopped_reading(hdl()) = true;
      detach(mpx_, false);
    }
    void launch() override {
      // nop
    }
    std::string addr() const override {
      return "test";
    }
    uint16_t port() const override {
      guard_type guard{mpx_->mx_};
      return mpx_->port(hdl());
    }
    void add_to_loop() override {
      mpx_->passive_mode(hdl()) = false;
    }
    void remove_from_loop() override {
      mpx_->passive_mode(hdl()) = true;
    }
  private:
    test_multiplexer* mpx_;
  };
  auto dptr = make_counted<impl>(hdl, this);
  { // lifetime scope of guard
    guard_type guard{mx_};
    auto& ref = doorman_data_[hdl];
    ref.ptr = dptr;
    ref.port = port;
  }
  CAF_LOG_INFO("opened port" << port << "on acceptor" << hdl);
  return dptr;
}

expected<doorman_ptr> test_multiplexer::new_tcp_doorman(uint16_t desired_port,
                                                        const char*, bool) {
  CAF_LOG_TRACE(CAF_ARG(desired_port));
  accept_handle hdl;
  uint16_t port = 0;
  { // Lifetime scope of guard.
    guard_type guard{mx_};
    if (desired_port == 0) {
      // Start with largest possible port and reverse iterate until we find a
      // port that's not assigned to a known doorman.
      port = std::numeric_limits<uint16_t>::max();
      while (is_known_port(port))
        --port;
      // Do the same for finding an acceptor handle.
      auto y = std::numeric_limits<int64_t>::max();
      while (is_known_handle(accept_handle::from_int(y)))
        --y;
      hdl = accept_handle::from_int(y);
    } else {
      auto i = doormen_.find(desired_port);
      if (i != doormen_.end()) {
        hdl = i->second;
        doormen_.erase(i);
        port = desired_port;
      } else {
        return sec::cannot_open_port;
      }
    }
  }
  return new_doorman(hdl, port);
}

dgram_servant_ptr test_multiplexer::new_dgram_servant(native_socket) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  std::cerr << "test_multiplexer::new_dgram_servant called with native socket"
            << std::endl;
  abort();
}

dgram_servant_ptr
test_multiplexer::new_dgram_servant_for_endpoint(native_socket, ip_endpoint&) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  std::cerr << "test_multiplexer::new_dgram_servant_for_endpoint called with "
               "native socket" << std::endl;
  abort();
}

/*
expected<scribe_ptr> test_multiplexer::new_tcp_scribe(const std::string& host,
                                                      uint16_t port) {
  CAF_LOG_TRACE(CAF_ARG(host) << CAF_ARG(port));
  connection_handle hdl;
  { // lifetime scope of guard
    guard_type guard{mx_};
    auto i = scribes_.find(std::make_pair(host, port));
    if (i != scribes_.end()) {
      hdl = i->second;
      scribes_.erase(i);
    } else {
      return sec::cannot_connect_to_node;
    }
  }
  return new_scribe(hdl);
}
*/

expected<dgram_servant_ptr>
test_multiplexer::new_remote_udp_endpoint(const std::string& host,
                                          uint16_t port) {
  CAF_LOG_TRACE(CAF_ARG(host) << CAF_ARG(port));
  dgram_handle hdl;
  { // lifetime scope of guard
    guard_type guard{mx_};
    auto i = remote_endpoints_.find(std::make_pair(host, port));
    if (i != remote_endpoints_.end()) {
      hdl = i->second;
      remote_endpoints_.erase(i);
    } else {
      return sec::cannot_connect_to_node;
    }
  }
  return new_dgram_servant(hdl, port);
}

expected<dgram_servant_ptr>
test_multiplexer::new_local_udp_endpoint(uint16_t desired_port,
                                         const char*, bool) {
  CAF_LOG_TRACE(CAF_ARG(desired_port));
  dgram_handle hdl;
  uint16_t port = 0;
  { // Lifetime scope of guard.
    guard_type guard{mx_};
    if (desired_port == 0) {
      // Start with largest possible port and reverse iterate until we find a
      // port that's not assigned to a known doorman.
      port = std::numeric_limits<uint16_t>::max();
      while (is_known_port(port))
        --port;
      // Do the same for finding a local dgram handle
      auto y = std::numeric_limits<int64_t>::max();
      while (is_known_handle(dgram_handle::from_int(y)))
        --y;
      hdl = dgram_handle::from_int(y);
    } else {
      auto i = local_endpoints_.find(desired_port);
      if (i != local_endpoints_.end()) {
        hdl = i->second;
        local_endpoints_.erase(i);
        port = desired_port;
      } else {
        return sec::cannot_open_port;
      }
    }
  }
  return new_dgram_servant(hdl, port);
}


dgram_servant_ptr
test_multiplexer::new_dgram_servant_with_data(dgram_handle hdl,
                                              dgram_servant_data& data) {
  class impl : public dgram_servant {
  public:
    impl(dgram_handle dh, dgram_servant_data& data, test_multiplexer* mpx)
      : dgram_servant(dh), mpx_(mpx), data_(data) {
      // nop
    }
    bool new_endpoint(ip_endpoint&, std::vector<char>&) override {
      // auto ep = mpx_->dgram_data_[hdl()].rd_buf.first;
      // dgram_handle ch;
      // { // Try to get a connection handle of a pending connect.
      //   guard_type guard{mpx_->mx_};
      //   auto& pc = mpx_->pending_endpoints();
      //   auto i = pc.find(ep);
      //   if (i == pc.end())
      //     return false;
      //   ch = i->second;
      //   pc.erase(i);
      // }
      // // TODO: share access to dgram_data_?
      // auto servant = mpx_->new_dgram_servant(ch, mpx_->local_port(hdl()));
      // servant->add_endpoint(addr);
      // mpx_->servants(hdl())[ep] = servant;
      // parent()->add_dgram_servant(servant);
      // return servant->consume(mpx_, buf);
      abort();
    }
    bool new_endpoint(int64_t id, std::vector<char>& buf) override {
      dgram_handle ch;
      { // Try to get a connection handle of a pending connect.
        guard_type guard{mpx_->mx_};
        auto& pc = mpx_->pending_endpoints();
        auto i = pc.find(id);
        if (i == pc.end())
          return false;
        ch = i->second;
        pc.erase(i);
      }
      CAF_LOG_INFO("new endpoint" << ch << "on servant" << hdl());
      auto& data = mpx_->dgram_data_[hdl()];
      // auto servant = mpx_->new_dgram_servant(ch, mpx_->local_port(hdl()));
      auto servant = mpx_->new_dgram_servant_with_data(ch, data);
      servant->add_endpoint();
      // mpx_->servants(hdl())[id] = servant;
      // std::cerr << "adding " << servant->hdl().id() << " as a servant to "
      //           << mpx_->dgram_data_[hdl()].ptr->hdl().id() << std::endl;
      parent()->add_dgram_servant(servant);
      return servant->consume(mpx_, buf);
    }
    void configure_datagram_size(size_t buf_size) override {
      mpx_->datagram_size(hdl()) = buf_size;
    }
    void ack_writes(bool enable) override {
      mpx_->ack_writes(hdl()) = enable;
    }
    std::vector<char>& wr_buf() override {
      auto& buf = mpx_->output_buffer(hdl());
      buf.first = hdl().id();
      return buf.second;
    }
    std::vector<char>& rd_buf() override {
      auto& buf = mpx_->input_buffer(hdl());
      return buf.second;
    }
    void stop_reading() override {
      mpx_->stopped_reading(hdl()) = true;
      detach(mpx_, false);
    }
    void launch() override {
      // nop
    }
    void flush() override {
      // nop
    }
    std::string addr() const override {
      return "test";
    }
    uint16_t port() const override {
      return static_cast<uint16_t>(hdl().id());
    }
    uint16_t local_port() const override {
      guard_type guard{mpx_->mx_};
      return mpx_->local_port(hdl());
    }
    void add_to_loop() override {
      mpx_->passive_mode(hdl()) = false;
    }
    void remove_from_loop() override {
      mpx_->passive_mode(hdl()) = true;
    }
    void add_endpoint(ip_endpoint&) override {
      abort();
    }
    void add_endpoint() override {
      // adapt endpoint from parent
      // TODO: should this be more explicit,
      // i.e., passing adapted parameters into the function?
      { // lifetime scope of guard
        guard_type guard{mpx_->mx_};
        data_.servants[hdl().id()] = this;
        mpx_->local_port(hdl()) = data_.local_port;
      }
    }
    void remove_endpoint() override {
      { // lifetime scope of guard
        guard_type guard{mpx_->mx_};
        auto itr = data_.servants.find(hdl().id());
        if (itr != data_.servants.end())
          data_.servants.erase(itr);
      }
    }
  private:
    test_multiplexer* mpx_;
    dgram_servant_data& data_;
  };
  auto dptr = make_counted<impl>(hdl, data, this);
  // { // lifetime scope of guard
  //   guard_type guard{mx_};
  //   local_port(hdl) = data.local_port;
  //   // impl_ptr(hdl) = dptr;
  // }
  CAF_LOG_INFO("new datagram servant" << hdl);
  return dptr;
}

dgram_servant_ptr test_multiplexer::new_dgram_servant(dgram_handle hdl,
                                                      uint16_t port) {
  CAF_LOG_TRACE(CAF_ARG(hdl));
  // TODO: Does there have to be a scoped guard around this somehow?
  dgram_servant_data& data = dgram_data_[hdl];
  auto dptr = new_dgram_servant_with_data(hdl, data);
  { // lifetime scope of guard
    guard_type guard{mx_};
    data.ptr = dptr;
    data.port = port;
    data.ptr = dptr;
  }
  return dptr;
}

dgram_servant_ptr test_multiplexer::new_dgram_servant(dgram_handle,
                                                      const std::string&,
                                                      uint16_t) {
  abort();
}

int64_t test_multiplexer::next_endpoint_id() {
  return servant_ids_++;
}


bool test_multiplexer::is_known_port(uint16_t x) const {
  auto pred1 = [&](const doorman_data_map::value_type& y) {
    return x == y.second.port;
  };
  auto pred2 = [&](const dgram_data_map::value_type& y) {
    return x == y.second.port;
  };
  return (doormen_.count(x) + local_endpoints_.count(x)) > 0
         || std::any_of(doorman_data_.begin(), doorman_data_.end(), pred1)
         || std::any_of(dgram_data_.begin(), dgram_data_.end(), pred2);
}

bool test_multiplexer::is_known_handle(accept_handle x) const {
  auto pred = [&](const pending_doorman_map::value_type& y) {
    return x == y.second;
  };
  return doorman_data_.count(x) > 0
         || std::any_of(doormen_.begin(), doormen_.end(), pred);
}

bool test_multiplexer::is_known_handle(dgram_handle x) const {
  auto pred1 = [&](const pending_local_dgram_endpoints_map::value_type& y) {
    return x == y.second;
  };
  auto pred2 = [&](const pending_remote_dgram_endpoints_map::value_type& y) {
    return x == y.second;
  };
  return dgram_data_.count(x) > 0
    || std::any_of(local_endpoints_.begin(), local_endpoints_.end(), pred1)
    || std::any_of(remote_endpoints_.begin(), remote_endpoints_.end(), pred2);
}

auto test_multiplexer::make_supervisor() -> supervisor_ptr {
  // not needed
  return nullptr;
}

bool test_multiplexer::try_run_once() {
  return try_exec_runnable() || try_read_data() || try_accept_connection();
}

void test_multiplexer::run_once() {
  try_run_once();
}

void test_multiplexer::run() {
  // nop
}

void test_multiplexer::provide_scribe(std::string host, uint16_t desired_port,
                                      connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE(CAF_ARG(host) << CAF_ARG(desired_port) << CAF_ARG(hdl));
  guard_type guard{mx_};
  scribes_.emplace(std::make_pair(std::move(host), desired_port), hdl);
}

void test_multiplexer::provide_acceptor(uint16_t desired_port,
                                        accept_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  doormen_.emplace(desired_port, hdl);
  doorman_data_[hdl].port = desired_port;
}

void test_multiplexer::provide_dgram_servant(uint16_t desired_port,
                                             dgram_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE(CAF_ARG(desired_port) << CAF_ARG(hdl));
  guard_type guard{mx_};
  local_endpoints_.emplace(desired_port, hdl);
  dgram_data_[hdl].local_port = desired_port;
}

void test_multiplexer::provide_dgram_servant(std::string host,
                                             uint16_t desired_port,
                                             dgram_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE(CAF_ARG(host) << CAF_ARG(desired_port) << CAF_ARG(hdl));
  guard_type guard{mx_};
  remote_endpoints_.emplace(std::make_pair(std::move(host), desired_port), hdl);
}

/// The external input buffer should be filled by
/// the test program.
test_multiplexer::buffer_type&
test_multiplexer::virtual_network_buffer(connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return scribe_data_[hdl].vn_buf;
}

test_multiplexer::job_buffer_type&
test_multiplexer::virtual_network_buffer(dgram_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return dgram_data_[hdl].vn_buf;
}

test_multiplexer::buffer_type&
test_multiplexer::output_buffer(connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return scribe_data_[hdl].wr_buf;
}

test_multiplexer::buffer_type&
test_multiplexer::input_buffer(connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return scribe_data_[hdl].rd_buf;
}

test_multiplexer::job_type&
test_multiplexer::output_buffer(dgram_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  auto& buf = dgram_data_[hdl].wr_buf;
  buf.emplace_back();
  return buf.back();
}

test_multiplexer::job_buffer_type&
test_multiplexer::output_queue(dgram_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return dgram_data_[hdl].wr_buf;
}

test_multiplexer::job_type&
test_multiplexer::input_buffer(dgram_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  // TODO: should probably return a job_type
  return dgram_data_[hdl].rd_buf;
}

receive_policy::config& test_multiplexer::read_config(connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return scribe_data_[hdl].recv_conf;
}

bool& test_multiplexer::ack_writes(connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return scribe_data_[hdl].ack_writes;
}

bool& test_multiplexer::ack_writes(dgram_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return dgram_data_[hdl].ack_writes;
}

bool& test_multiplexer::stopped_reading(connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return scribe_data_[hdl].stopped_reading;
}

bool& test_multiplexer::stopped_reading(dgram_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return dgram_data_[hdl].stopped_reading;
}

bool& test_multiplexer::passive_mode(connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return scribe_data_[hdl].passive_mode;
}

bool& test_multiplexer::passive_mode(dgram_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return dgram_data_[hdl].passive_mode;
}

scribe_ptr& test_multiplexer::impl_ptr(connection_handle hdl) {
  return scribe_data_[hdl].ptr;
}

uint16_t& test_multiplexer::port(accept_handle hdl) {
  return doorman_data_[hdl].port;
}

uint16_t& test_multiplexer::port(dgram_handle hdl) {
  return dgram_data_[hdl].port;
}

uint16_t& test_multiplexer::local_port(dgram_handle hdl) {
  return dgram_data_[hdl].local_port;
}

size_t& test_multiplexer::datagram_size(dgram_handle hdl) {
  return dgram_data_[hdl].datagram_size;
}

dgram_servant_ptr& test_multiplexer::impl_ptr(dgram_handle hdl) {
  return dgram_data_[hdl].ptr;
}

test_multiplexer::servants_map&
test_multiplexer::servants(dgram_handle hdl) {
  return dgram_data_[hdl].servants;
}

bool& test_multiplexer::stopped_reading(accept_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return doorman_data_[hdl].stopped_reading;
}

bool& test_multiplexer::passive_mode(accept_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return doorman_data_[hdl].passive_mode;
}

doorman_ptr& test_multiplexer::impl_ptr(accept_handle hdl) {
  return doorman_data_[hdl].ptr;
}

void test_multiplexer::add_pending_connect(accept_handle src,
                                           connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  pending_connects_.emplace(src, hdl);
}

void test_multiplexer::prepare_connection(accept_handle src,
                                          connection_handle hdl,
                                          test_multiplexer& peer,
                                          std::string host, uint16_t port,
                                          connection_handle peer_hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_ASSERT(this != &peer);
  CAF_LOG_TRACE(CAF_ARG(src) << CAF_ARG(hdl) << CAF_ARG(host) << CAF_ARG(port)
                << CAF_ARG(peer_hdl));
  auto input = std::make_shared<buffer_type>();
  auto output = std::make_shared<buffer_type>();
  CAF_LOG_DEBUG("insert scribe data for" << CAF_ARG(hdl));
  auto res1 = scribe_data_.emplace(hdl, scribe_data{input, output});
  if (!res1.second)
    CAF_RAISE_ERROR("prepare_connection: handle already in use");
  CAF_LOG_DEBUG("insert scribe data on peer for" << CAF_ARG(peer_hdl));
  auto res2 = peer.scribe_data_.emplace(peer_hdl, scribe_data{output, input});
  if (!res2.second)
    CAF_RAISE_ERROR("prepare_connection: peer handle already in use");
  CAF_LOG_INFO("acceptor" << src << "has connection" << hdl
               << "ready for incoming connect from" << host << ":"
               << port << "from peer with connection handle" << peer_hdl);
  if (doormen_.count(port) == 0)
    provide_acceptor(port, src);
  add_pending_connect(src, hdl);
  peer.provide_scribe(std::move(host), port, peer_hdl);
}

void test_multiplexer::add_pending_endpoint(int64_t ep, dgram_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  pending_endpoints_.emplace(ep, hdl);
}

test_multiplexer::pending_connects_map& test_multiplexer::pending_connects() {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return pending_connects_;
}

test_multiplexer::pending_endpoints_map& test_multiplexer::pending_endpoints() {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  return pending_endpoints_;
}

bool test_multiplexer::has_pending_scribe(std::string x, uint16_t y) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  guard_type guard{mx_};
  return scribes_.count(std::make_pair(std::move(x), y)) > 0;
}

bool test_multiplexer::has_pending_remote_endpoint(std::string x,
                                                   uint16_t y) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  guard_type guard{mx_};
  return remote_endpoints_.count(std::make_pair(std::move(x), y)) > 0;
}

void test_multiplexer::accept_connection(accept_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE(CAF_ARG(hdl));
  // Filled / initialized in the critical section.
  doorman_data* dd;
  { // Access `doorman_data_` and `pending_connects_` while holding `mx_`.
    guard_type guard{mx_};
    dd = &doorman_data_[hdl];
  }
  CAF_ASSERT(dd->ptr != nullptr);
  if (!dd->ptr->new_connection())
    dd->passive_mode = true;
}

bool test_multiplexer::try_accept_connection() {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  // Filled / initialized in the critical section.
  std::vector<doorman_data*> doormen;
  { // Access `doorman_data_` and `pending_connects_` while holding `mx_`.
    guard_type guard{mx_};
    doormen.reserve(doorman_data_.size());
    for (auto& kvp : doorman_data_)
      doormen.emplace_back(&kvp.second);
  }
  // Try accepting a new connection on all existing doorman.
  return std::any_of(doormen.begin(), doormen.end(),
                     [](doorman_data* x) { return x->ptr->new_connection(); });
}

bool test_multiplexer::try_read_data() {
  std::cerr << "[tm] try_read_data() called" << std::endl;
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE("");
  // scribe_data might change while we traverse it
  std::vector<connection_handle> xs;
  xs.reserve(scribe_data_.size());
  for (auto& kvp : scribe_data_)
    xs.emplace_back(kvp.first);
  for (auto x : xs)
    if (try_read_data(x))
      return true;
  return false;
}

bool test_multiplexer::try_read_data(connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE(CAF_ARG(hdl));
  scribe_data& sd = scribe_data_[hdl];
  if (sd.passive_mode || sd.ptr == nullptr || sd.ptr->parent() == nullptr
      || !sd.ptr->parent()->getf(abstract_actor::is_initialized_flag)) {
    return false;
  }
  switch (sd.recv_conf.first) {
    case receive_policy_flag::exactly:
      if (sd.vn_buf.size() >= sd.recv_conf.second) {
        sd.rd_buf.clear();
        auto first = sd.vn_buf.begin();
        auto last = first + static_cast<ptrdiff_t>(sd.recv_conf.second);
        sd.rd_buf.insert(sd.rd_buf.end(), first, last);
        sd.vn_buf.erase(first, last);
        if (!sd.ptr->consume(this, sd.rd_buf.data(), sd.rd_buf.size()))
          sd.passive_mode = true;
        return true;
      }
      break;
    case receive_policy_flag::at_least:
      if (sd.vn_buf.size() >= sd.recv_conf.second) {
        sd.rd_buf.clear();
        sd.rd_buf.swap(sd.vn_buf);
        if (!sd.ptr->consume(this, sd.rd_buf.data(), sd.rd_buf.size()))
          sd.passive_mode = true;
        return true;
      }
      break;
    case receive_policy_flag::at_most:
      auto max_bytes = static_cast<ptrdiff_t>(sd.recv_conf.second);
      if (!sd.vn_buf.empty()) {
        sd.rd_buf.clear();
        auto xbuf_size = static_cast<ptrdiff_t>(sd.vn_buf.size());
        auto first = sd.vn_buf.begin();
        auto last = (max_bytes < xbuf_size) ? first + max_bytes : sd.vn_buf.end();
        sd.rd_buf.insert(sd.rd_buf.end(), first, last);
        sd.vn_buf.erase(first, last);
        if (!sd.ptr->consume(this, sd.rd_buf.data(), sd.rd_buf.size()))
          sd.passive_mode = true;
        return true;
      }
  }
  return false;
}

bool test_multiplexer::read_data() {
  std::cerr << "[tm] read_data() called" << std::endl;
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE("");
  // scribe_data might change while we traverse it
  std::vector<connection_handle> xs;
  xs.reserve(scribe_data_.size());
  for (auto& kvp : scribe_data_)
    xs.emplace_back(kvp.first);
  long hits = 0;
  for (auto x : xs)
    if (scribe_data_.count(x) > 0)
      if (read_data(x))
        ++hits;
  return hits > 0;
}

bool test_multiplexer::read_data(connection_handle hdl) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE(CAF_ARG(hdl));
  flush_runnables();
  if (passive_mode(hdl))
    return false;
  scribe_data& sd = scribe_data_[hdl];
  if (sd.ptr == nullptr || sd.ptr->parent() == nullptr
      || !sd.ptr->parent()->getf(abstract_actor::is_initialized_flag)) {
    return false;
  }
  // count how many data packets we could dispatch
  long hits = 0;
  for (;;) {
    switch (sd.recv_conf.first) {
      case receive_policy_flag::exactly:
        if (sd.vn_buf.size() >= sd.recv_conf.second) {
          ++hits;
          sd.rd_buf.clear();
          auto first = sd.vn_buf.begin();
          auto last = first + static_cast<ptrdiff_t>(sd.recv_conf.second);
          sd.rd_buf.insert(sd.rd_buf.end(), first, last);
          sd.vn_buf.erase(first, last);
          if (!sd.ptr->consume(this, sd.rd_buf.data(), sd.rd_buf.size()))
            passive_mode(hdl) = true;
        } else {
          return hits > 0;
        }
        break;
      case receive_policy_flag::at_least:
        if (sd.vn_buf.size() >= sd.recv_conf.second) {
          ++hits;
          sd.rd_buf.clear();
          sd.rd_buf.swap(sd.vn_buf);
          if (!sd.ptr->consume(this, sd.rd_buf.data(), sd.rd_buf.size()))
            passive_mode(hdl) = true;
        } else {
          return hits > 0;
        }
        break;
      case receive_policy_flag::at_most:
        auto max_bytes = static_cast<ptrdiff_t>(sd.recv_conf.second);
        if (!sd.vn_buf.empty()) {
          ++hits;
          sd.rd_buf.clear();
          auto xbuf_size = static_cast<ptrdiff_t>(sd.vn_buf.size());
          auto first = sd.vn_buf.begin();
          auto last = (max_bytes < xbuf_size) ? first + max_bytes : sd.vn_buf.end();
          sd.rd_buf.insert(sd.rd_buf.end(), first, last);
          sd.vn_buf.erase(first, last);
          if (!sd.ptr->consume(this, sd.rd_buf.data(), sd.rd_buf.size()))
            passive_mode(hdl) = true;
        } else {
          return hits > 0;
        }
    }
  }
}

bool test_multiplexer::read_data(dgram_handle hdl) {
  std::cout << "[rd] read on endpoint " << hdl.id() << std::endl;
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE(CAF_ARG(hdl));
  flush_runnables();
  if (passive_mode(hdl))
    return false;
  auto& dd = dgram_data_[hdl];
  // CAF_ASSERT(dd.ptr != nullptr);
  if (dd.ptr == nullptr || dd.ptr->parent() == nullptr
      || !dd.ptr->parent()->getf(abstract_actor::is_initialized_flag))
    return false;
  if (dd.vn_buf.back().second.empty())
    return false;
  dd.rd_buf.second.clear();
  std::swap(dd.rd_buf, dd.vn_buf.front());
  dd.vn_buf.pop_front();
  std::cerr << "available servants: " << std::endl;
  for (auto& s : dd.servants)
    std::cerr << " > " << s.first << std::endl;
  std::cerr << "looking for: " << dd.rd_buf.first << std::endl;
  auto& delegate = dd.servants[dd.rd_buf.first];
  // TODO: failure should shutdown all related servants
  if (delegate == nullptr) {
    std::cout << "[rd] datgram with " << dd.rd_buf.second.size() << " bytes "
              << "on new endpoint "<< dd.rd_buf.first << std::endl;
    if (!dd.ptr->new_endpoint(dd.rd_buf.first, dd.rd_buf.second))
      passive_mode(hdl) = true;
  } else {
    std::cout << "[rd] datgram with " << dd.rd_buf.second.size() << " bytes "
              << "on known endpoint "<< delegate->hdl().id() << std::endl;
    if (!delegate->consume(this, dd.rd_buf.second))
      passive_mode(hdl) = true;
  }
  return true;
}

void test_multiplexer::virtual_send(connection_handle hdl,
                                    const buffer_type& buf) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE(CAF_ARG(hdl));
  auto& vb = virtual_network_buffer(hdl);
  vb.insert(vb.end(), buf.begin(), buf.end());
  read_data(hdl);
}

void test_multiplexer::virtual_send(dgram_handle dst, int64_t ep,
                                    const buffer_type& buf) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE(CAF_ARG(hdl));
  auto& vb = virtual_network_buffer(dst);
  vb.emplace_back(ep, buf);
  read_data(dst);
}

void test_multiplexer::exec_runnable() {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE("");
  resumable_ptr ptr;
  { // critical section
    guard_type guard{mx_};
    while (resumables_.empty())
      cv_.wait(guard);
    resumables_.front().swap(ptr);
    resumables_.pop_front();
  }
  exec(ptr);
}

bool test_multiplexer::try_exec_runnable() {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE("");
  resumable_ptr ptr;
  { // critical section
    guard_type guard{mx_};
    if (resumables_.empty())
      return false;
    resumables_.front().swap(ptr);
    resumables_.pop_front();
  }
  exec(ptr);
  return true;
}

void test_multiplexer::flush_runnables() {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_LOG_TRACE("");
  // execute runnables in bursts, pick a small size to
  // minimize time in the critical section
  constexpr size_t max_runnable_count = 8;
  std::vector<resumable_ptr> runnables;
  runnables.reserve(max_runnable_count);
  // runnables can create new runnables, so we need to double-check
  // that `runnables_` is empty after each burst
  do {
    runnables.clear();
    { // critical section
      guard_type guard{mx_};
      while (!resumables_.empty() && runnables.size() < max_runnable_count) {
        runnables.emplace_back(std::move(resumables_.front()));
        resumables_.pop_front();
      }
    }
    for (auto& ptr : runnables)
      exec(ptr);
  } while (!runnables.empty());
}

void test_multiplexer::exec_later(resumable* ptr) {
  CAF_ASSERT(ptr != nullptr);
  CAF_LOG_TRACE("");
  switch (ptr->subtype()) {
    case resumable::io_actor:
    case resumable::function_object: {
      if (inline_runnables_ > 0) {
        --inline_runnables_;
        resumable_ptr tmp{ptr};
        exec(tmp);
        if (inline_runnable_callback_) {
          using std::swap;
          std::function<void()> f;
          swap(f, inline_runnable_callback_);
          f();
        }
      } else {
        std::list<resumable_ptr> tmp;
        tmp.emplace_back(ptr);
        guard_type guard{mx_};
        resumables_.splice(resumables_.end(), std::move(tmp));
        cv_.notify_all();
      }
      break;
    }
    default:
      system().scheduler().enqueue(ptr);
  }
}

void test_multiplexer::exec(resumable_ptr& ptr) {
  CAF_ASSERT(std::this_thread::get_id() == tid_);
  CAF_ASSERT(ptr != nullptr);
  CAF_LOG_TRACE("");
  switch (ptr->resume(this, 1)) {
    case resumable::resume_later:
      exec_later(ptr.get());
      break;
    case resumable::done:
    case resumable::awaiting_message:
      intrusive_ptr_release(ptr.get());
      break;
    default:
      ; // ignored
  }
}

} // namespace network
} // namespace io
} // namespace caf
