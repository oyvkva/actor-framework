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

#ifndef CAF_IO_HANDLE_HPP
#define CAF_IO_HANDLE_HPP

#include <string>
#include <cstdint>

#include "caf/detail/comparable.hpp"

namespace caf {

/// Base class for IO handles such as `accept_handle` or `connection_handle`.
template <class Subtype, class InvalidType, int64_t InvalidId = -1>
class handle : detail::comparable<Subtype>,
               detail::comparable<Subtype, InvalidType> {
public:
  constexpr handle() : id_(InvalidId) {
    // nop
  }

  handle(const Subtype& other) {
    id_ = other.id();
  }

  handle(const handle& other) = default;

  handle& operator=(const handle& other) {
    id_ = other.id();
    return *this;
  }

  handle& operator=(const InvalidType&) {
    id_ = InvalidId;
    return *this;
  }

  /// Returns the unique identifier of this handle.
  int64_t id() const {
    return id_;
  }

  /// Sets the unique identifier of this handle.
  void set_id(int64_t value) {
    id_ = value;
  }

  int64_t compare(const Subtype& other) const {
    return id_ - other.id();
  }

  int64_t compare(const InvalidType&) const {
    return invalid() ? 0 : 1;
  }

  bool invalid() const {
    return id_ == InvalidId;
  }

  void set_invalid() {
    set_id(InvalidId);
  }

  static Subtype from_int(int64_t id) {
    return {id};
  }

  friend std::string to_string(const Subtype& x) {
    return std::to_string(x.id());
  }

protected:
  handle(int64_t handle_id) : id_{handle_id} {
    // nop
  }

  int64_t id_;
};

} // namespace caf

#endif // CAF_IO_HANDLE_HPP
