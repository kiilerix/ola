/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * KiNetNode.cpp
 * An KiNet node
 * Copyright (C) 2013 Simon Newton
 */

#include <algorithm>
#include <memory>

#include "ola/Constants.h"
#include "ola/Logging.h"
#include "ola/network/IPV4Address.h"
#include "ola/network/SocketAddress.h"
#include "ola/util/SequenceNumber.h"
#include "plugins/kinet/KiNetNode.h"

namespace ola {
namespace plugin {
namespace kinet {

using ola::network::HostToNetwork;
using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::network::UDPSocket;
using std::unique_ptr;

const uint16_t KiNetNode::KINET_PORT = 6038;
const uint32_t KiNetNode::KINET_MAGIC_NUMBER = 0x0401dc4a;
const uint16_t KiNetNode::KINET_VERSION_ONE = 0x0100;
const uint16_t KiNetNode::KINET_DMX_MSG = 0x0101;
const uint16_t KiNetNode::KINET_PORTOUT_MSG = 0x0801;
const uint16_t KiNetNode::KINET_PORTOUT_MIN_BUFFER_SIZE = 24;

/*
 * Create a new KiNet node.
 * @param ss a SelectServerInterface to use
 * @param socket a UDPSocket or Null. Ownership is transferred.
 */
KiNetNode::KiNetNode(ola::io::SelectServerInterface *ss,
                     ola::network::UDPSocketInterface *socket)
    : m_running(false),
      m_ss(ss),
      m_output_stream(&m_output_queue),
      m_socket(socket) {
}


/*
 * Cleanup.
 */
KiNetNode::~KiNetNode() {
  Stop();
}


/*
 * Start this node.
 */
bool KiNetNode::Start() {
  if (m_running)
    return false;

  if (!InitNetwork())
    return false;
  m_running = true;
  return true;
}


/*
 * Stop this node.
 */
bool KiNetNode::Stop() {
  if (!m_running)
    return false;

  m_ss->RemoveReadDescriptor(m_socket.get());
  m_socket.reset();
  m_running = false;
  return true;
}


/*
 * Send some DMX data
 */
bool KiNetNode::SendDMX(const IPV4Address &target_ip, const DmxBuffer &buffer) {
  static const uint8_t port = 0;
  static const uint8_t flags = 0;
  static const uint16_t timer_val = 0;
  static const uint32_t universe = 0xffffffff;

  if (!buffer.Size()) {
    OLA_DEBUG << "Not sending 0 length packet";
    return true;
  }

  m_output_queue.Clear();
  PopulatePacketHeader(KINET_DMX_MSG);
  m_output_stream << port << flags << timer_val << HostToNetwork(universe);
  m_output_stream << DMX512_START_CODE;
  m_output_stream.Write(buffer.GetRaw(), buffer.Size());

  IPV4SocketAddress target(target_ip, KINET_PORT);
  bool ok = m_socket->SendTo(&m_output_queue, target);
  if (!ok) {
    OLA_WARN << "Failed to send KiNet DMX packet";
  }

  if (!m_output_queue.Empty()) {
    OLA_WARN << "Failed to send complete KiNet packet";
    m_output_queue.Clear();
  }
  return ok;
}


/*
 * Send some PORTOUT data
 */
bool KiNetNode::SendPortOut(const IPV4Address &target_ip,
                            const uint8_t port,
                            const DmxBuffer &buffer) {
  static const uint8_t flags1 = 0;  // Definitely flags of some sort
  static const uint16_t flags2 = 0;  // Possibly always 0
  static const uint32_t universe = 0xffffffff;

  if (!buffer.Size()) {
    OLA_DEBUG << "Not sending 0 length packet";
    return true;
  }

  uint16_t buffer_size = static_cast<uint16_t>(buffer.Size());
  uint16_t buffer_size_regulated = std::max(
    buffer_size,
    KINET_PORTOUT_MIN_BUFFER_SIZE);

  m_output_queue.Clear();
  PopulatePacketHeader(KINET_PORTOUT_MSG);
  m_output_stream << HostToNetwork(universe) << port
                  << flags1 << flags2
                  << HostToNetwork(buffer_size_regulated);
  m_output_stream << static_cast<uint16_t>(DMX512_START_CODE);
  m_output_stream.Write(buffer.GetRaw(), buffer.Size());

  // TODO(Peter): Update to our new DmxBuffer padding options when we add and
  //              write them.
  // Buffer must be at least 24 bytes, pad with zeros if needed
  for (uint16_t i = buffer_size; i < KINET_PORTOUT_MIN_BUFFER_SIZE; i++) {
    m_output_stream << ((uint8_t)0);
  }

  IPV4SocketAddress target(target_ip, KINET_PORT);
  bool ok = m_socket->SendTo(&m_output_queue, target);
  if (!ok) {
    OLA_WARN << "Failed to send KiNet PORTOUT packet";
  }

  if (!m_output_queue.Empty()) {
    OLA_WARN << "Failed to send complete KiNet packet";
    m_output_queue.Clear();
  }
  return ok;
}


/*
 * Called when there is data on this socket. Right now we discard all packets.
 */
void KiNetNode::SocketReady() {
  uint8_t packet[1500];
  ssize_t packet_size = sizeof(packet);
  ola::network::IPV4SocketAddress source;

  if (!m_socket->RecvFrom(reinterpret_cast<uint8_t*>(&packet),
                          &packet_size,
                          &source))
    return;

  OLA_INFO << "Received Kinet packet from " << source << ", discarding";
}


/*
 * Fill in the header for a packet
 */
void KiNetNode::PopulatePacketHeader(uint16_t msg_type) {
  m_output_stream << KINET_MAGIC_NUMBER << KINET_VERSION_ONE;
  m_output_stream << msg_type << HostToNetwork(m_transaction_number.Next());
}


/*
 * Setup the networking components.
 */
bool KiNetNode::InitNetwork() {
  std::unique_ptr<ola::network::UDPSocketInterface> socket(m_socket.release());

  if (!socket.get())
    socket.reset(new UDPSocket());

  if (!socket->Init()) {
    OLA_WARN << "Socket init failed";
    return false;
  }

  if (!socket->Bind(IPV4SocketAddress(IPV4Address::WildCard(), KINET_PORT))) {
    return false;
  }

  socket->SetOnData(NewCallback(this, &KiNetNode::SocketReady));
  m_ss->AddReadDescriptor(socket.get());
  m_socket.reset(socket.release());
  return true;
}
}  // namespace kinet
}  // namespace plugin
}  // namespace ola
