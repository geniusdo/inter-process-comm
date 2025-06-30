// Copyright 2025 Chuangye Liu <chuangyeliu0206@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <span>
#include <variant>
#include <unordered_map>
#include <utility>
#include <asio.hpp>

#include "msgs/common.hpp"
#include "itc/MsgQueue.hpp"

template <typename T>
concept ValidParser = requires {
  typename T::DataType;
  {T::parser_type};
  {T::header};
  {T::length};
  {T::name};
}
&&std::is_convertible_v<decltype(T::parser_type), uint8_t>
&&std::is_convertible_v<decltype(T::header), uint16_t>
&&std::is_convertible_v<decltype(T::name), std::string_view>
&&((requires(std::span<std::byte> in, typename T::DataType out) {
     { T::Process(in, out) } -> std::same_as<bool>;
   })
   || (requires(typename T::DataType in, std::span<std::byte> out) {
        { T::Process(in, out) } -> std::same_as<void>;
      }));

template <ChannelMode Mode = ChannelMode::UDP, ValidParser... Parsers>
class CommChannel {
  using UdpSocket  = asio::ip::udp::socket;
  using UnixSocket = asio::local::datagram_protocol::socket;

  using UdpEp  = asio::ip::udp::endpoint;
  using UnixEp = asio::local::datagram_protocol::endpoint;

  using SocketType   = std::conditional_t<Mode == ChannelMode::Unix, UnixSocket, UdpSocket>;
  using EndpointType = std::conditional_t<Mode == ChannelMode::Unix, UnixEp, UdpEp>;

public:
  CommChannel() = delete;

  CommChannel(asio::io_context &io_context, std::string local_ip, int local_port, std::string remote_ip,
              int remote_port) requires(Mode == ChannelMode::UDP)
      : local_endpoint_(asio::ip::make_address(local_ip), local_port),
        remote_endpoint_(asio::ip::make_address(remote_ip), remote_port),
        socket_(io_context, asio::ip::udp::v4()),
        timer_(io_context) {
    socket_.bind(local_endpoint_);
  }

  CommChannel(asio::io_context &io_context, std::string_view local_path, std::string_view remote_path) requires(Mode == ChannelMode::Unix)
      : local_endpoint_(createEp(local_path)), remote_endpoint_(createEp(remote_path)), socket_(io_context), timer_(io_context) {
    socket_.open();
    socket_.bind(local_endpoint_);
  }

  // forcefully bind a constexpr name
  template <std::size_t N>
  void bind_message_queue(const char (&name)[N], const ParserType attribute, MsgQueue &mq) {
    std::string_view sv{name, N - 1};
    MsgQueue *mq_ptr = &mq;
    if (attribute == ParserType::Sender) {
      send_mq_vec_.emplace_back(std::make_pair(sv, mq_ptr));
    } else if (attribute == ParserType::Receiver) {
      recv_mq_map_.emplace(sv, mq_ptr);
    }
    return;
  }

  bool enable_receiver() {
    // do bind checking
    bool mq_registered = (((Parsers::parser_type == ParserType::Receiver) && (recv_mq_map_.find(Parsers::name) != recv_mq_map_.end())) || ...);
    if (!mq_registered) return false;

    tmp_endpoint_ = remote_endpoint_;
    this->socket_.async_receive_from(asio::buffer(recv_buffer_), remote_endpoint_,
                                     std::bind(&CommChannel::receiver_handler, this, std::placeholders::_1, std::placeholders::_2));
    return true;
  }

  bool enable_sender() {
    bool mq_registered
        = (((Parsers::parser_type == ParserType::Sender)
            && (std::find_if(send_mq_vec_.begin(), send_mq_vec_.end(), [](const auto &curr_mq) { return curr_mq.first == Parsers::name; })
                != send_mq_vec_.end()))
           || ...);
    if (!mq_registered) return false;
    timer_.async_wait(std::bind(&CommChannel::timer_handler, this, std::placeholders::_1));
    return true;
  }

  void set_loop_rate(uint32_t rate) { loop_rate = rate; }

private:
  void timer_handler(const asio::error_code &ec) {
    if (!ec) {
      auto curr_mq = send_mq_vec_[loop_cnt];
      if (!curr_mq.second->empty()) {
        std::span<std::byte> buffer_view(this->send_buffer_);
        bool matched = (([this, &curr_mq, &buffer_view]() -> bool {
                          if constexpr (Parsers::parser_type == ParserType::Sender) {
                            if (curr_mq.first == Parsers::name) {
                              typename Parsers::DataType data;
                              curr_mq.second->dequeue(data);
                              Parsers::Process(data, buffer_view);
                              this->socket_.async_send_to(asio::buffer(this->send_buffer_, Parsers::length), this->remote_endpoint_,
                                                          [](const asio::error_code &error, std::size_t bytes_transferred) {});
                              return true;
                            } else
                              return false;
                          } else if constexpr (Parsers::parser_type == ParserType::DirectSender) {
                            if (curr_mq.first == Parsers::name) {
                              typename Parsers::DataType data;
                              curr_mq.second->dequeue(data);
                              // Parsers::Process(data, buffer_view);
                              this->socket_.async_send_to(asio::buffer(data), this->remote_endpoint_,
                                                          [](const asio::error_code &error, std::size_t bytes_transferred) {});
                              return true;
                            } else
                              return false;
                          }
                          return false;
                        }())
                        || ...);
      }
      loop_cnt++;
      // reset counter
      if (loop_cnt >= send_mq_vec_.size()) loop_cnt = 0;
    }
    timer_.expires_after(asio::chrono::microseconds(loop_rate));
    timer_.async_wait(std::bind(&CommChannel::timer_handler, this, std::placeholders::_1));
    return;
  }

  void receiver_handler(const asio::error_code &ec, std::size_t bytes_transferred) {
    if (!ec && bytes_transferred > 0) {
      std::span<std::byte> buffer_view(this->recv_buffer_);

      uint16_t header;
      memcpy(&header, buffer_view.data(), sizeof(uint16_t));

      bool matched = ([this, header, &buffer_view]() -> bool {
        typename Parsers::DataType recv_data;
        if constexpr (Parsers::parser_type == ParserType::Receiver) {
          if (header == Parsers::header) {
            Parsers::Process(buffer_view, recv_data);
          } else {
            std::cout<<header<<std::endl;
            return false;
          }
        } else {
          return false;
        }

        auto it = this->recv_mq_map_.find(Parsers::name);
        if (it != this->recv_mq_map_.end()) {
          it->second->enqueue(recv_data);
        }
        return true;
      }() || ...);

      // TODO: Add warning here
    }
    tmp_endpoint_ = remote_endpoint_;
    this->socket_.async_receive_from(asio::buffer(recv_buffer_), tmp_endpoint_,
                                     std::bind(&CommChannel::receiver_handler, this, std::placeholders::_1, std::placeholders::_2));
  }

  static UnixEp createEp(std::string_view sv) requires(Mode == ChannelMode::Unix) {
    std::remove(std::string(sv).c_str());
    return UnixEp(std::string(sv));
  }

  asio::steady_timer timer_;
  SocketType socket_;
  EndpointType local_endpoint_;
  EndpointType remote_endpoint_;
  EndpointType tmp_endpoint_;

  std::vector<std::pair<std::string_view, MsgQueue *>> send_mq_vec_;
  std::unordered_map<std::string_view, MsgQueue *> recv_mq_map_;
  std::array<std::byte, 1500> send_buffer_;
  std::array<std::byte, 1500> recv_buffer_;

  uint32_t loop_cnt  = 0;
  uint32_t loop_rate = 1000;
};