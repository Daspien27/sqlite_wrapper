/*
 * Copyright (c) 2019 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "sqlite3.h"

#include <array>
#include <optional>
#include <thread>
#include <mutex>
#include <iostream>
#include <vector>
#include <cstring>
#include <functional>

namespace sqlite {

namespace detail {

template <typename T>
struct get_fn_info {
  static_assert(!std::is_same_v<T,T>);
};

template <typename R, typename... As>
struct get_fn_info<R(* const)(As...)> {
  using ret_type = R;
  using arg_types = std::tuple<As...>;
  template <unsigned i>
  using arg_type = std::tuple_element_t<i, arg_types>;
  static constexpr int num_args = std::tuple_size_v<arg_types>;
};

inline constexpr auto maybe_invoke = [] (auto &&thing) -> decltype(auto) {
  if constexpr (std::is_invocable_v<decltype(thing)>) {
    return thing();
  } else {
    return thing;
  }
};

inline std::mutex sqlite3_config_mutex;
inline bool sqlite3_configured = false;

inline void sqlite_error_log_callback(void *, int err_code, const char *msg) {
  std::cerr << "SQLite error (" << err_code << "): " << msg << std::endl;
}

} // namespace detail

template <typename T, typename = void>
constexpr auto user_serialize_fn = nullptr;

template <typename T>
constexpr auto user_serialize_fn<std::optional<T>,
                                 std::enable_if_t<user_serialize_fn<T> != nullptr>>
  = [] (const std::optional<T> &arg) {
    return (arg ? std::make_optional(user_serialize_fn<T>(*arg))
                : std::nullopt);
  };

template <typename T, typename = void>
constexpr auto user_deserialize_fn = nullptr;

struct blob : public std::string {
  template <typename... Ts>
  blob(Ts &&...args) : std::string(std::forward<Ts>(args)...) { }
};

struct blob_view : public std::string_view {
  template <typename... Ts>
  blob_view(Ts &&...args) : std::string_view(std::forward<Ts>(args)...) { }
};

struct error {
  int err_code;
};

template <const auto &db_name>
class Database {
 private:
  struct Connection {
    sqlite3 *db_handle;

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    Connection(void) {
      // Since each thread has its own exclusive connection to the database, we
      // can safely set SQLITE_CONFIG_MULTITHREAD so that SQLite may assume
      // the database will not be accessed from the same connection by two
      // threads simultaneously.
      do {
        std::lock_guard<std::mutex> guard(detail::sqlite3_config_mutex);
        if (!detail::sqlite3_configured) {
          sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
          sqlite3_config(SQLITE_CONFIG_LOG,
                         detail::sqlite_error_log_callback, nullptr);
          detail::sqlite3_configured = true;
        }
      } while (0);
      static const auto saved_db_name = detail::maybe_invoke(db_name);
      auto ret = sqlite3_open(&saved_db_name[0], &db_handle);
      if (ret != SQLITE_OK) {
        throw error{ret};
      }

      // When the database has been temporarily locked by another process, this
      // tells SQLite to retry the command/query until it succeeds, rather than
      // returning SQLITE_BUSY immediately.
      sqlite3_busy_handler(db_handle,
          [](void *, int) {
            std::this_thread::yield();
            return 1;
          },
          nullptr);

      if (post_connection_hook) {
        post_connection_hook(db_handle);
      }
    }

    ~Connection(void) {
      // To close the database, we use sqlite3_close_v2() because unlike
      // sqlite3_close(), this function allows there to be un-finalized
      // prepared statements.  The database handle will close once
      // all prepared statements have been finalized by the thread-local
      // `PreparedStmtCache` destructors.
      sqlite3_close_v2(db_handle);
    }
  };

  // The connection to the database at `db_name` is thread-local.
  static Connection &connection_tls(void) {
    static thread_local Connection object;
    return object;
  }

 public:
  static inline std::function<void(sqlite3 *)> post_connection_hook;

 private:
  // The `PreparedStmtCache` is a cache of available prepared statements for
  // reuse corresponding to the query given by `query_str`.
  template <const auto &query_str>
  class PreparedStmtCache {
   public:
    static inline void put_tls(sqlite3_stmt *stmt) {
      singleton_tls().internal_put(stmt);
    }

    static inline sqlite3_stmt *get_tls(void) {
      return singleton_tls().internal_get();
    }

   private:
    PreparedStmtCache(void)
      : db_handle(connection_tls().db_handle),
        first_free_stmt(nullptr), other_free_stmts() { }

    ~PreparedStmtCache(void) {
      sqlite3_finalize(first_free_stmt);
      for (auto stmt : other_free_stmts) {
        sqlite3_finalize(stmt);
      }
    }

    static PreparedStmtCache<query_str> &singleton_tls() {
      static thread_local PreparedStmtCache<query_str> object;
      return object;
    }

    sqlite3_stmt *internal_get(void) {
      if (first_free_stmt != nullptr) {
        sqlite3_stmt *stmt = nullptr;
        std::swap(first_free_stmt, stmt);
        return stmt;
      } else if (!other_free_stmts.empty()) {
        sqlite3_stmt *stmt = other_free_stmts.back();
        other_free_stmts.pop_back();
        return stmt;
      } else {
        static const auto saved_query_str = detail::maybe_invoke(query_str);
        // If no prepared statement is available for reuse, make a new one.
        sqlite3_stmt *stmt;
        std::string_view query_str_view = saved_query_str;
        auto ret = sqlite3_prepare_v3(db_handle,
                                      query_str_view.data(),
                                      query_str_view.length() + 1,
                                      SQLITE_PREPARE_PERSISTENT,
                                      &stmt, nullptr);
        if (ret != SQLITE_OK) {
          throw error{ret};
        }
        return stmt;
      }
    }

    // This is called by the row fetcher returned by query<query_str, ...>().
    void internal_put(sqlite3_stmt *stmt) {
      if (first_free_stmt == nullptr) {
        first_free_stmt = stmt;
      } else {
        other_free_stmts.push_back(stmt);
      }
    }

    sqlite3 *db_handle;
    sqlite3_stmt *first_free_stmt;
    std::vector<sqlite3_stmt *> other_free_stmts;
  };

 public:
  class QueryResult;

  template <const auto &query_str, typename... Ts>
  static QueryResult query(const Ts &...args) {
    if constexpr (((user_serialize_fn<std::decay_t<Ts>> != nullptr) || ...)) {
      auto maybe_serialize = [] (const auto &arg) -> decltype(auto) {
        using arg_t = std::decay_t<decltype(arg)>;
        if constexpr (user_serialize_fn<arg_t> != nullptr) {
          return user_serialize_fn<arg_t>(arg);
        } else {
          return arg;
        }
      };
      return query<query_str>(maybe_serialize(args)...);
    } else {
      sqlite3_stmt *stmt = PreparedStmtCache<query_str>::get_tls();

      // Via the fold expression right below, `bind_dispatcher` is called on
      // each argument passed in to `query()` and binds the argument to the
      // statement according to the argument's type, using the correct SQL C
      // API function.
      int idx = 1;
      auto bind_dispatcher = [stmt, &idx] (const auto &arg, auto &self) {

        using arg_t = std::decay_t<decltype(arg)>;
        if constexpr (std::is_integral_v<arg_t>) {
          sqlite3_bind_int64(stmt, idx, arg);
        } else if constexpr (std::is_same_v<const char *, arg_t> ||
                             std::is_same_v<char *, arg_t>) {
          sqlite3_bind_text(stmt, idx, arg, strlen(arg), SQLITE_STATIC);
        } else if constexpr (std::is_same_v<std::string, arg_t>) {
          sqlite3_bind_text(stmt, idx, &arg[0], arg.size(), SQLITE_STATIC);
        } else if constexpr (std::is_same_v<blob, arg_t> ||
                             std::is_same_v<blob_view, arg_t>) {
          sqlite3_bind_blob(stmt, idx, &arg[0], arg.size(), SQLITE_STATIC);
        } else if constexpr (std::is_same_v<std::nullopt_t, arg_t>) {
          sqlite3_bind_null(stmt, idx);
        } else if constexpr (std::is_convertible_v<std::nullopt_t, arg_t>) {
          if (arg) {
            self(*arg, self);
            return;
          } else {
            sqlite3_bind_null(stmt, idx);
          }
        } else {
          static_assert(!std::is_same_v<arg_t, arg_t>);
        }
        idx++;

      };
      (void)bind_dispatcher;
      (bind_dispatcher(args, bind_dispatcher), ...);

      return QueryResult(stmt, &PreparedStmtCache<query_str>::put_tls);
    }
  }

  class QueryResult {
   public:
    QueryResult() : stmt(nullptr) { }

    QueryResult &operator=(QueryResult &&other) {
      if (this != &other) {
        stmt = other.stmt;
        ret = other.ret;
        first_invocation = other.first_invocation;
        put_cb = other.put_cb;
        other.stmt = nullptr;
      }
      return *this;
    }

    ~QueryResult() {
      if (stmt == nullptr) {
        return;
      }
      sqlite3_clear_bindings(stmt);
      sqlite3_reset(stmt);
      put_cb(stmt);
    }

    int resultCode(void) {
      return ret;
    }

    template <typename... Ts>
    bool operator()(Ts &&...args) {
      if (static_cast<int>(sizeof...(args)) > sqlite3_column_count(stmt)) {
        throw error{SQLITE_ERROR};
      }
      if (!first_invocation) {
        ret = sqlite3_step(stmt);
      }
      if (ret != SQLITE_ROW) {
        return false;
      }
      int idx = 0;
      auto column_dispatcher = [this, &idx] (auto &&arg, auto &self) {

        using arg_t = std::decay_t<decltype(arg)>;
        if constexpr (std::is_integral_v<arg_t>) {
          arg = sqlite3_column_int64(stmt, idx);
        } else if constexpr (std::is_same_v<std::string, arg_t> ||
                             std::is_same_v<std::string_view, arg_t>) {
          auto ptr = (const char *)sqlite3_column_text(stmt, idx);
          auto len = sqlite3_column_bytes(stmt, idx);
          arg = arg_t(ptr, len);
        } else if constexpr (std::is_same_v<std::nullopt_t, arg_t>) {
          ;
        } else if constexpr (std::is_convertible_v<std::nullopt_t, arg_t>) {
          if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
            arg.reset();
          } else {
            typename arg_t::value_type nonnull_arg;
            self(nonnull_arg, self);
            arg = std::move(nonnull_arg);
            return;
          }
        } else if constexpr (user_deserialize_fn<arg_t> != nullptr) {
          constexpr auto *fn_ptr = +user_deserialize_fn<arg_t>;
          using fn_info = detail::get_fn_info<decltype(fn_ptr)>;
          using from_type = typename fn_info::template arg_type<0>;
          std::decay_t<from_type> from_arg;
          self(from_arg, self);
          arg = fn_ptr(std::move(from_arg));
          return;
        } else {
          static_assert(!std::is_same_v<arg_t, arg_t>);
        }
        idx++;

      };
      (void)column_dispatcher;
      (column_dispatcher(std::forward<Ts>(args), column_dispatcher), ...);
      first_invocation = false;
      return true;
    }

   private:
    using PutCallbackType = void (sqlite3_stmt *);

    QueryResult(sqlite3_stmt *stmt_, PutCallbackType *put_cb_)
        : stmt(stmt_), put_cb(put_cb_) {
      ret = sqlite3_step(stmt);
    }

    QueryResult(const QueryResult &) = delete;
    QueryResult &operator=(const QueryResult &) = delete;

    sqlite3_stmt *stmt;
    PutCallbackType *put_cb;
    int ret;
    bool first_invocation = true;

    friend class Database<db_name>;
  };

  static void beginTransaction(void) {
    static const char begin_transaction_query[] = "begin transaction";
    query<begin_transaction_query>();
  }

  static void commit(void) {
    static const char commit_transaction_query[] = "commit transaction";
    query<commit_transaction_query>();
  }
};

} // namespace sqlite
