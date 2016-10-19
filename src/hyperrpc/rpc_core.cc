#include <assert.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include "hyperrpc/rpc_core.h"
#include "hyperrpc/service.h"
#include "hyperrpc/protocol.h"
#include "hyperrpc/constants.h"
#include "hyperrpc/rpc_context.h"
#include "hyperrpc/rpc_message.pb.h"

namespace hrpc {

RpcCore::RpcCore(const Env& env)
  : env_(env)
  , rpc_sess_mgr_(env)
{
}

RpcCore::~RpcCore()
{
}


bool RpcCore::Init(size_t rpc_core_id, OnSendPacket on_send_pkt,
                                   OnFindService on_find_svc,
                                   OnServiceRouting on_svc_routing)
{
  rpc_core_id_ = rpc_core_id;
  on_send_packet_ = on_send_pkt;
  on_find_service_ = on_find_svc;
  on_service_routing_ = on_svc_routing;
  if (!rpc_sess_mgr_.Init(env_.opt().max_rpc_sessions, rpc_core_id,
                     ccb::BindClosure(this, &RpcCore::OnOutgoingRpcSend))) {
    ELOG("RpcSessionManager init failed!");
    return false;
  }
  return true;
}

void RpcCore::CallMethod(const ::google::protobuf::MethodDescriptor* method,
                         const ::google::protobuf::Message* request,
                         ::google::protobuf::Message* response,
                         ::ccb::ClosureFunc<void(Result)> done)
{
  const std::string& service_name = method->service()->name();
  const std::string& method_name = method->name();
  // resolve endpoints of service.method
  EndpointList endpoints;
  if (!on_service_routing_ ||
      !on_service_routing_(service_name, method_name, &endpoints) ||
      endpoints.empty()) {
    WLOG("cannot resolve endpoints for %s.%s", service_name.c_str(),
                                               method_name.c_str());
    done(kNoRoute);
    return;
  }
  rpc_sess_mgr_.AddSession(method, request, response,
                           endpoints, std::move(done));
}

size_t RpcCore::OnRecvPacket(const Buf& buf, const Addr& addr)
{
  size_t cur_rpc_core_id = rpc_core_id_;
  // parse RpcPakcetHeader
  if (buf.len() <= sizeof(RpcPacketHeader)) {
    ILOG("packet len too short!");
    return cur_rpc_core_id;
  }
  auto pkt_header = static_cast<const RpcPacketHeader*>(buf.ptr());
  if (pkt_header->hrpc_pkt_tag != kHyperRpcPacketTag) {
    ILOG("bad packet tag!");
    return cur_rpc_core_id;
  }
  if (pkt_header->hrpc_pkt_ver != kHyperRpcPacketVer) {
    ILOG("packet ver dismatch!");
    return cur_rpc_core_id;
  }
  size_t rpc_header_len = ntohs(pkt_header->rpc_header_len);
  size_t rpc_body_len = ntohl(pkt_header->rpc_body_len);
  if (buf.len() != sizeof(RpcPacketHeader) + rpc_header_len + rpc_body_len) {
    ILOG("bad packet len!");
    return cur_rpc_core_id;
  }
  const void* rpc_header_ptr = buf.char_ptr() + sizeof(RpcPacketHeader);
  const void* rpc_body_ptr = static_cast<const char*>(rpc_header_ptr)
                           + rpc_header_len;
  // parse RpcHeader
  static thread_local RpcHeader rpc_header;
  if (!rpc_header.ParseFromArray(rpc_header_ptr, rpc_header_len)) {
    ILOG("parse RpcHeader failed!");
    return cur_rpc_core_id;
  }
  if (rpc_header.packet_type() == RpcHeader::REQUEST) {
    // process REQUEST message
    OnRecvRequestMessage(rpc_header, {rpc_body_ptr, rpc_body_len}, addr);
  } else {
    // process RESPONSE message
    size_t rpc_core_id_plus_1 = rpc_header.rpc_id() >> kRpcIdSeqPartBits;
    if (rpc_core_id_plus_1 == 0 ||
        rpc_core_id_plus_1 > env_.opt().worker_num) {
      ILOG("invalid rpc_core_id_plus_1:%lu!", rpc_core_id_plus_1);
      return cur_rpc_core_id;
    }
    size_t dst_rpc_core_id = rpc_core_id_plus_1 - 1;
    if (dst_rpc_core_id != cur_rpc_core_id) { // need redirect
      DLOG("redirect to rpc_core_id:%lu", dst_rpc_core_id);
      return dst_rpc_core_id;
    }
    OnRecvResponseMessage(rpc_header, {rpc_body_ptr, rpc_body_len}, addr);
  }
  return 0;
}

void RpcCore::OnRecvRequestMessage(const RpcHeader& header,
                                   const Buf& body, const Addr& addr)
{
  Service* service = on_find_service_(header.service_name());
  if (!service) IRET("service requested not found locally!");
  auto service_desc = service->GetDescriptor();
  if (!service_desc) IRET("get service descriptor failed!");
  auto method_desc = service_desc->FindMethodByName(header.method_name());
  if (!method_desc) IRET("get method descriptor failed!");
  IncomingRpcContext* ctx = new IncomingRpcContext(method_desc,
                                  service->GetRequestPrototype(method_desc),
                                  service->GetResponsePrototype(method_desc),
                                  header.rpc_id(), addr);

  if (!ctx->request()->ParseFromArray(body.ptr(), body.len()))
    IRET("parse Request message failed!");
  // dispatch incoming rpc within receiving worker-thread
  service->CallMethod(method_desc, ctx->request(), ctx->response(),
               ccb::BindClosure(this, &RpcCore::OnIncomingRpcDone, ctx));
}

void RpcCore::OnRecvResponseMessage(const RpcHeader& header,
                                    const Buf& body, const Addr& addr)
{
  if (header.rpc_result() < 0 || header.rpc_result() > kMaxResultValue) {
    ILOG("invalid rpc_result value!");
    return;
  }
  Result rpc_result = static_cast<Result>(header.rpc_result());
  rpc_sess_mgr_.OnRecvResponse(header.service_name(), header.method_name(),
                               header.rpc_id(), rpc_result, body);
}

void RpcCore::OnIncomingRpcDone(const IncomingRpcContext* rpc, Result result)
{
  static thread_local RpcHeader rpc_header;
  rpc_header.set_packet_type(RpcHeader::RESPONSE);
  rpc_header.set_service_name(rpc->method()->service()->name());
  rpc_header.set_method_name(rpc->method()->name());
  rpc_header.set_rpc_id(rpc->rpc_id());
  rpc_header.set_rpc_result(result);
  SendMessage(rpc_header, *rpc->response(), rpc->addr());
  delete rpc;
}

void RpcCore::OnOutgoingRpcSend(
                          const google::protobuf::MethodDescriptor* method,
                          const google::protobuf::Message& request,
                          uint64_t rpc_id, const Addr& addr)
{
  static thread_local RpcHeader rpc_header;
  rpc_header.set_packet_type(RpcHeader::REQUEST);
  rpc_header.set_service_name(method->service()->name());
  rpc_header.set_method_name(method->name());
  rpc_header.set_rpc_id(rpc_id);
  SendMessage(rpc_header, request, addr);
}

void RpcCore::SendMessage(const RpcHeader& header,
                          const google::protobuf::Message& body,
                          const Addr& addr)
{
  size_t rpc_header_len = header.ByteSizeLong();
  size_t rpc_body_len = body.ByteSizeLong();
  size_t pkt_size = sizeof(RpcPacketHeader) + rpc_header_len + rpc_body_len;
  char pkt_buffer[pkt_size];
  // set RpcPacketHeader
  auto pkt_header = reinterpret_cast<RpcPacketHeader*>(pkt_buffer);
  pkt_header->hrpc_pkt_tag = kHyperRpcPacketTag;
  pkt_header->hrpc_pkt_ver = kHyperRpcPacketVer;
  pkt_header->rpc_header_len = htons(static_cast<uint16_t>(rpc_header_len));
  pkt_header->rpc_body_len = htonl(static_cast<uint32_t>(rpc_body_len));
  // set RpcHeader & body
  google::protobuf::io::ArrayOutputStream out(
      pkt_buffer + sizeof(RpcPacketHeader), rpc_header_len + rpc_body_len);
  HRPC_ASSERT(header.SerializeToZeroCopyStream(&out));
  HRPC_ASSERT(body.SerializeToZeroCopyStream(&out));
  // send to network
  on_send_packet_({pkt_buffer, pkt_size}, addr);
}

} // namespace hrpc