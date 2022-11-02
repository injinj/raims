#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <netdb.h>
#include <raims/transport.h>
#include <raims/session.h>
#include <raims/ev_tcp_transport.h>

using namespace rai;
using namespace ms;
using namespace kv;

void
TransportRoute::on_connect( kv::EvSocket &conn ) noexcept
{
  uint32_t connect_type  = 0;
  bool     is_encrypt    = false,
           first_connect = true;
  this->clear( TPORT_IS_SHUTDOWN );
  if ( ! this->is_mcast() ) {
    EvTcpTransport &tcp = (EvTcpTransport &) conn;
    is_encrypt = tcp.encrypt;

    if ( this->connect_ctx != NULL &&
         (EvConnection *) &tcp == this->connect_ctx->client &&
         this->connect_ctx->connect_tries > 1 )
      first_connect = false;

    if ( first_connect ) {
      this->printf( "connect %s %s %s using %s fd %u\n",
                    tcp.encrypt ? "encrypted" : "plaintext",
                    conn.peer_address.buf, conn.type_string(),
                    this->sub_route.service_name, conn.fd );
    }
    if ( this->is_mesh() ) {
      if ( ! tcp.is_connect ) {
        this->mgr.add_mesh_accept( *this, tcp );
        return;
      }
      connect_type = TPORT_IS_CONNECT | TPORT_IS_MESH;
    }
    else {
      if ( ! tcp.is_connect ) {
        if ( ! this->is_edge() ) {
          this->mgr.add_tcp_accept( *this, tcp );
          return;
        }
        connect_type = TPORT_IS_TCP;
      }
      else {
        connect_type = TPORT_IS_CONNECT | TPORT_IS_TCP;
      }
    }
  }
  else {
    connect_type = TPORT_IS_MCAST;
    this->printf( "connect %s %s using %s fd %u\n",
                  conn.peer_address.buf, conn.type_string(),
                  this->sub_route.service_name, conn.fd );
  }
  if ( first_connect )
    this->mgr.events.on_connect( this->tport_id, connect_type, is_encrypt );
  if ( ! this->connected.test_set( conn.fd ) )
    this->connect_count++;
}

void
TransportRoute::on_shutdown( EvSocket &conn,  const char *err,
                             size_t errlen ) noexcept
{
  const char *s = "disconnected";
  char errbuf[ 256 ];
  if ( &conn == (EvSocket *) this->listener )
    s = "listener stopped";
  else if ( errlen >= 15 && ::memcmp( err, "already connect", 15 ) == 0 )
    s = "stopped trying";
  if ( errlen == 0 ) {
    errlen = conn.print_sock_error( errbuf, sizeof( errbuf ) );
    if ( errlen > 0 )
      err = errbuf;
  }
  if ( conn.bytes_sent + conn.bytes_recv > 0  ) {
    if ( errlen > 0 )
      this->printf( "%s %s (%.*s)\n", s, conn.peer_address.buf,
                    (int) errlen, err );
    else
      this->printf( "%s %s (count=%u)\n", s, conn.peer_address.buf,
                    this->connect_count );
    this->mgr.events.on_shutdown( this->tport_id, conn.fd >= 0 );
  }
  if ( conn.fd >= 0 ) {
    this->user_db.retire_source( *this, conn.fd );
    if ( this->connected.test_clear( conn.fd ) ) {
      if ( --this->connect_count == 0 )
        if ( ! this->is_set( TPORT_IS_LISTEN ) )
          this->set( TPORT_IS_SHUTDOWN );
    }
    else if ( &conn == (EvSocket *) this->listener )
      this->set( TPORT_IS_SHUTDOWN );
  }
  else if ( this->connect_count == 0 ) {
    this->set( TPORT_IS_SHUTDOWN );
  }
  d_tran( "%s connect_count %u\n", this->name, this->connect_count );
}

bool
ConnectMgr::connect( ConnectCtx &ctx ) noexcept
{
  TransportRoute  * rte = this->user_db.transport_tab.ptr[ ctx.event_id ];
  struct addrinfo * ai  = ctx.addr_info.addr_list;

  if ( rte->is_set( TPORT_IS_MESH ) &&
       this->mgr.find_mesh( *rte, ai, rte->mesh_url_hash ) ) {
    ctx.state = ConnectCtx::CONN_SHUTDOWN;
    return true;
  }

  EvTcpTransportClient *cl =
    this->poll.get_free_list<EvTcpTransportClient>( this->sock_type );
  cl->rte      = rte;
  cl->route_id = rte->sub_route.route_id;
  cl->encrypt  = ( ( ctx.opts & TCP_OPT_ENCRYPT ) != 0 );
  ctx.client   = cl;
  if ( cl->connect( ctx.opts, &ctx, ai ) )
    return true;
  ctx.client = NULL;
  rte->on_shutdown( *cl, NULL, 0 );
  this->poll.push_free_list( cl );
  return false;
}

void
ConnectMgr::on_connect( ConnectCtx &ctx ) noexcept
{
  TransportRoute * rte = this->user_db.transport_tab.ptr[ ctx.event_id ];
  rte->on_connect( *ctx.client );
}

bool
ConnectMgr::on_shutdown( ConnectCtx &ctx,  const char *msg,
                         size_t len ) noexcept
{
  TransportRoute * rte = this->user_db.transport_tab.ptr[ ctx.event_id ];
  rte->on_shutdown( *ctx.client, msg, len );
  return true;
}

ConnectCtx *
ConnectDB::create( uint64_t id ) noexcept
{
  ConnectCtx * ctx = new ( ::malloc( sizeof( ConnectCtx ) ) )
    ConnectCtx( this->poll, *this );
  ctx->event_id = id;
  this->ctx_array[ id ] = ctx;
  return ctx;
}

void
ConnectCtx::connect( const char *host,  int port,  int opts ) noexcept
{
  this->opts          = opts;
  this->connect_tries = 0;
  this->state         = CONN_GET_ADDRESS;
  this->start_time    = kv_current_monotonic_time_ns();
  this->addr_info.timeout_ms = this->next_timeout() / 4;
  this->addr_info.get_address( host, port, opts );
}

void
ConnectCtx::reconnect( void ) noexcept
{
  this->connect( this->addr_info.host, this->addr_info.port, this->opts );
}

void
ConnectCtx::on_connect( kv::EvSocket & ) noexcept
{
  this->state = CONN_ACTIVE;
  this->db.on_connect( *this );
}

bool
ConnectCtx::expired( uint64_t cur_time ) noexcept
{
  if ( this->timeout == 0 )
    return false;
  if ( cur_time == 0 )
    cur_time = current_monotonic_time_ns();
  return this->start_time +
    ( (uint64_t) this->timeout * 1000000000 ) < cur_time;
}

void
ConnectCtx::on_shutdown( EvSocket &,  const char *msg,  size_t len ) noexcept
{
  bool was_connected = ( this->client->bytes_recv > 0 );

  if ( ! this->db.on_shutdown( *this, msg, len ) )
    this->state = CONN_SHUTDOWN;

  this->client = NULL;
  uint64_t cur_time = current_monotonic_time_ns();
  if ( was_connected || this->state == CONN_SHUTDOWN ) {
    this->start_time    = cur_time;
    this->connect_tries = 0;
  }

  if ( this->state != CONN_SHUTDOWN ) {
    if ( ! this->expired( cur_time ) && ! this->db.poll.quit ) {
      this->state = CONN_TIMER;
      this->db.poll.timer.add_timer_millis( *this, this->next_timeout(), 0,
                                            this->event_id );
    }
    else {
      this->state = CONN_IDLE;
    }
  }
}

bool
ConnectCtx::timer_cb( uint64_t, uint64_t eid ) noexcept
{
  if ( eid == this->event_id && this->state != CONN_SHUTDOWN &&
       ! this->db.poll.quit ) {
    this->state = CONN_GET_ADDRESS;
    this->addr_info.timeout_ms = this->next_timeout() / 4;
    this->addr_info.free_addr_list();
    this->addr_info.ipv6_prefer = ! this->addr_info.ipv6_prefer;
    this->addr_info.get_address( this->addr_info.host, this->addr_info.port,
                                 this->opts );
  }
  return false;
}

void
ConnectCtx::addr_resolve_cb( CaresAddrInfo & ) noexcept
{
  if ( this->state == CONN_SHUTDOWN )
    return;
  this->connect_tries++;
  if ( this->addr_info.addr_list != NULL ) {
    if ( this->db.connect( *this ) )
      return;
  }
  if ( this->state != CONN_SHUTDOWN ) {
    if ( ! this->expired() && ! this->db.poll.quit ) {
      this->state = CONN_TIMER;
      this->db.poll.timer.add_timer_millis( *this, this->next_timeout(), 0,
                                            this->event_id );
    }
    else {
      this->state = CONN_IDLE;
    }
  }
}

