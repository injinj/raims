#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <netdb.h>
#include <raims/ev_rv_transport.h>
#include <raims/ev_web.h>
#include <raims/transport.h>
#include <raims/session.h>

using namespace rai;
using namespace sassrv;
using namespace ms;
using namespace kv;
using namespace md;

enum {
  RV_START_TIMER,
  RV_STOP_TIMER,
  RV_QUIT_TIMER
};
static const int RV_TIMEOUT_SECS = 2 * 60 + 10;

EvRvTransportListen::EvRvTransportListen( kv::EvPoll &p,
                                          TransportRoute &r ) noexcept
    : EvRvListen( p, r.sub_route ), rte( r ),
      last_active_mono( 0 ), active_cnt( 0 ), fake_ip( 0 ), no_mcast( false ),
      no_permanent( false ), no_fakeip( false )
{
  static kv_atom_uint64_t rv_timer_id;
  this->notify = &r;
  this->timer_id = ( (uint64_t) this->sock_type << 56 ) |
                   kv_sync_add( &rv_timer_id, (uint64_t) 1 );
}

EvSocket *
EvRvTransportListen::accept( void ) noexcept
{
  EvSocket *c = this->EvRvListen::accept();
  if ( c != NULL )
    this->rte.set_peer_name( *c, "rv.acc" );
  return c;
}

int
EvRvTransportListen::listen( const char *ip,  int port,  int opts ) noexcept
{
  int res = this->EvRvListen::listen( ip, port, opts );
  if ( res == 0 ) {
    this->rte.set_peer_name( *this, "rv.list" );
    if ( this->no_permanent ) {
      this->poll.timer.add_timer_seconds( this->fd, RV_TIMEOUT_SECS,
                                          this->timer_id, RV_START_TIMER );
    }
  }
  return res;
}

bool
EvRvTransportListen::timer_expire( uint64_t tid,  uint64_t kind ) noexcept
{
  if ( tid != this->timer_id )
    return false;
  if ( kind == RV_START_TIMER ) {
    if ( this->accept_cnt == 0 ) {
      this->rte.printf( "no client connected, shutting down\n" );
      this->poll.timer.add_timer_seconds( this->fd, 1,
                                          this->timer_id, RV_QUIT_TIMER );
    }
  }
  else if ( kind == RV_STOP_TIMER ) {
    if ( this->active_cnt == 0 ) {
      uint64_t cur_mono = this->poll.timer.current_monotonic_time_ns();
      if ( this->last_active_mono +
           sec_to_ns( RV_TIMEOUT_SECS - 1 ) <= cur_mono ) {
        this->rte.printf( "no active clients, shutting down\n" );
        this->poll.timer.add_timer_seconds( this->fd, 1,
                                            this->timer_id, RV_QUIT_TIMER );
      }
    }
  }
  else if ( kind == RV_QUIT_TIMER ) {
    this->poll.quit = 1;
  }
  return false;
}

static size_t
make_rv_name( RvHost &host,  char *name,  const char *suf ) noexcept
{
  int x;
  if ( host.service_len == 0 )
    x = ::snprintf( name, 256, "rv_7500%s", suf );
  else
    x = ::snprintf( name, 256, "rv_%.*s%s", host.service_len, host.service,
                    suf );
  return min_int( x, 255 );
}

ConfigTree::Transport *
EvRvTransportListen::get_rv_transport( RvHost &host,  bool create ) noexcept
{
  ConfigTree::Transport * t;
  ConfigTree & tree = this->rte.mgr.tree;
  StringTab  & stab = this->rte.user_db.string_tab;
  char   rv_svc[ 256 ];
  size_t svc_len = make_rv_name( host, rv_svc, "" );

  t = tree.find_transport( rv_svc, svc_len );

  if ( t == NULL && create ) {
    t = stab.make<ConfigTree::Transport>();
    stab.ref_string( rv_svc, svc_len, t->tport );
    t->tport_id = tree.transport_cnt++;
    tree.transports.push_tl( t );
  }
  return t;
}

NetTransport
RvMcast2::net_to_transport( const char *net,  size_t &net_len ) noexcept
{
  static const struct {
    const char * name;
    NetTransport type;
  } protos[] = {
   { T_ANY, NET_ANY },
   { T_MESH, NET_MESH }, { T_MESH_LISTEN, NET_MESH_LISTEN },
   { T_MESH_CONNECT, NET_MESH_CONNECT },
   { T_TCP, NET_TCP },   { T_TCP_LISTEN, NET_TCP_LISTEN },
   { T_TCP_CONNECT, NET_TCP_CONNECT } };
  static const size_t nprotos = sizeof( protos ) / sizeof( protos[ 0 ] );

  const char * p;
  if ( net_len == 0 )
    return NET_NONE;
  if ( (p = (const char *) ::memchr( net, ';', net_len )) == NULL )
    return NET_ANY;

  size_t len = &net[ net_len ] - p;
  for ( size_t i = 0; i < nprotos; i++ ) {
    size_t plen = ::strlen( protos[ i ].name );
    if ( len == plen + 1 && ::memcmp( p + 1, protos[ i ].name, plen ) == 0 ) {
      net_len -= plen + 1;
      return protos[ i ].type;
    }
  }
  return NET_MCAST;
}

int
RvMcast2::parse_network2( const char *net,  size_t net_len ) noexcept
{
  this->type = net_to_transport( net, net_len );
  return this->RvMcast::parse_network( net, net_len );
}

static bool
match_route_str( ConfigTree::Transport &t,  const char *name,
                 const char *value,  size_t value_len ) noexcept
{
  const char *tmp = NULL;
  return t.get_route_str( name, tmp ) &&
         ::strlen( tmp ) == value_len &&
         ::memcmp( value, tmp, value_len ) == 0;
}

static bool
net_equals( RvHost &host,  ConfigTree::Transport &t ) noexcept
{
  size_t net_len = host.network_len;

  switch ( RvMcast2::net_to_transport( host.network, net_len ) ) {
    default:
    case NET_NONE:
      return false;

    case NET_ANY:
      return t.type.equals( T_ANY, T_ANY_SZ ) &&
             match_route_str( t, R_DEVICE, host.host_ip, host.host_ip_len );

    case NET_MESH_CONNECT:
    case NET_MESH:
    case NET_MESH_LISTEN:
      return t.type.equals( T_MESH, T_MESH_SZ ) &&
             match_route_str( t, R_DEVICE, host.host_ip, host.host_ip_len );

    case NET_TCP_CONNECT:
    case NET_TCP:
    case NET_TCP_LISTEN:
      return t.type.equals( T_TCP, T_TCP_SZ ) &&
             match_route_str( t, R_DEVICE, host.host_ip, host.host_ip_len );

    case NET_MCAST:
      return t.type.equals( T_PGM, T_PGM_SZ ) &&
             match_route_str( t, R_LISTEN, host.network, host.network_len ) &&
             match_route_str( t, R_PORT, host.service, host.service_len );
  }
}

int
RvMcast2::device_ip( char *buf,  size_t len ) const noexcept
{
  const uint8_t * p = (const uint8_t *) (const void *) &this->host_ip;
  int x = ::snprintf( buf, len, "%u.%u.%u.%u", p[ 0 ], p[ 1 ], p[ 2 ], p[ 3 ] );
  return min_int( x, (int) len - 1 );
}

void
EvRvTransportListen::make_rv_transport( ConfigTree::Transport *&t, RvHost &host,
                                        bool &is_listener ) noexcept
{
  ConfigTree & tree = this->rte.mgr.tree;
  StringTab  & stab = this->rte.user_db.string_tab;
  size_t       net_len = host.network_len;

  is_listener = true;
  NetTransport type = RvMcast2::net_to_transport( host.network, net_len );
  if ( type == NET_NONE )
    t = NULL;
  else {
    char name[ 256 ];
    size_t name_len = make_rv_name( host, name, "_old" );
    for ( ConfigTree::Transport * t = tree.transports.hd; t != NULL;
          t = t->next ) {
      if ( t->tport.equals( name, name_len ) ) {
        if ( net_equals( host, *t ) ) {
          name_len = make_rv_name( host, name, "" );
          stab.ref_string( name, name_len, t->tport );
          break;
        }
      }
    }

    if ( t == NULL )
      t = get_rv_transport( host, true );
    switch ( type ) {
      default: break;
      case NET_ANY:
        stab.reref_string( T_ANY, T_ANY_SZ, t->type );
        tree.set_route_str( *t, stab, R_DEVICE,
                            host.host_ip, host.host_ip_len );
        break;

      case NET_MESH_CONNECT:
        is_listener = false; /* FALLTHRU */
      case NET_MESH:
      case NET_MESH_LISTEN:
        stab.reref_string( T_MESH, T_MESH_SZ, t->type );
        tree.set_route_str( *t, stab, R_DEVICE,
                            host.host_ip, host.host_ip_len );
        break;

      case NET_TCP_CONNECT:
        is_listener = false; /* FALLTHRU */
      case NET_TCP:
      case NET_TCP_LISTEN:
        stab.reref_string( T_TCP, T_TCP_SZ, t->type );
        tree.set_route_str( *t, stab, R_DEVICE,
                            host.host_ip, host.host_ip_len );
        break;

      case NET_MCAST:
        if ( ! this->no_mcast ) {
          stab.reref_string( T_PGM, T_PGM_SZ, t->type );
          tree.set_route_str( *t, stab, R_LISTEN,
                              host.network, host.network_len );
          tree.set_route_str( *t, stab, R_PORT,
                              host.service, host.service_len );
          tree.set_route_str( *t, stab, R_MCAST_LOOP, "2", 1 );
        }
        break;
    }
  }
}

uint32_t
EvRvTransportListen::get_fake_ip( void ) noexcept
{
  if ( this->fake_ip == 0 ) {
    UserDB & user_db = this->rte.mgr.user_db;
    this->fake_ip = (uint32_t) ( user_db.start_time / 1000 );
    for ( uint32_t uid = 1; uid < user_db.next_uid; uid++ ) {
      if ( user_db.bridge_tab[ uid ] != NULL ) {
        uint64_t t = user_db.bridge_tab[ uid ]->start_time;
        if ( (uint32_t) ( t / 1000 ) == this->fake_ip ) {
          rand::fill_urandom_bytes( &this->fake_ip, 4 );
          break;
        }
      }
    }
  }
  return this->fake_ip;
}

int
EvRvTransportListen::start_host( RvHost &host,  const char *net, size_t net_len,
                                 const char *svc,  size_t svc_len ) noexcept
{
  bool not_running = ! host.start_in_progress && ! host.network_started;
  if ( host.network_started ) {
    if ( host.mcast.host_ip == 0 ||
         ! host.is_same_network( net, net_len, svc, svc_len ) )
      return ERR_SAME_SVC_TWO_NETS;
    return HOST_OK;
  }
  if ( ! host.start_in_progress ) {
    if ( host.mcast.host_ip == 0 ||
         ! host.is_same_network( net, net_len, svc, svc_len ) ) {
      RvMcast2 mc;
      int status = mc.parse_network2( net, net_len );
      if ( status == HOST_OK ) {
        if ( mc.fake_ip == 0 && ! this->no_fakeip )
          mc.fake_ip = this->get_fake_ip();
        host.host_id_len = (uint16_t)
          min_int( (size_t) this->rte.mgr.user_db.user.user.len,
                   MAX_RV_HOST_ID_LEN - 1 );
        ::memcpy( host.host_id, this->rte.mgr.user_db.user.user.val,
                  host.host_id_len );
        ::memset( &host.host_id[ host.host_id_len ], 0,
                  MAX_RV_HOST_ID_LEN - host.host_id_len );
        for ( uint32_t i = 0; i < this->rte.mgr.unrouteable.count; i++ ) {
          Unrouteable & un = this->rte.mgr.unrouteable.ptr[ i ];
          if ( un.web != NULL ) {
            char         tmp[ 256 ];
            const char * addr = un.web->http_url.val;
            size_t       len  = sizeof( tmp );
            int          port = un.tport->get_host_port( addr, tmp, len );
            if ( port != 0 && len > 0 ) {
              AddrInfo info;
              if ( info.get_address(addr, port, OPT_AF_INET|OPT_LISTEN) == 0 ) {
                for ( addrinfo * ai = info.ai; ai != NULL; ai = ai->ai_next ) {
                  if ( ai->ai_family == AF_INET ) {
                    host.http_addr =
                      ((struct sockaddr_in *) ai->ai_addr)->sin_addr.s_addr;
                    host.http_port =
                      ((struct sockaddr_in *) ai->ai_addr)->sin_port;
                    goto break_loop;
                  }
                }
              }
            }
          break_loop:;
            break;
          }
        }
        status = host.start_network( mc, net, net_len, svc, svc_len );
      }
      if ( status != HOST_OK )
        return status;
    }
    host.start_in_progress = true;
  }

  RvHostRoute           * hr   = this->tab.find( &host );
  TransportRoute        * rte  = NULL;
  ConfigTree::Transport * t    = NULL;
  uint32_t                delay_secs = 0;
  bool                    exists = false;

  /* exists -> do not shutdown, do not startup
   * equals -> do not shutdown, do startup
   * not equals -> do shutdown, do startup */
  if ( hr != NULL ) {
    rte    = hr->rte;
    t      = hr->cfg;
    exists = hr->tport_exists;
  }
  if ( t != NULL ) {
    if ( ! exists ) {
      if ( ! net_equals( host, *t ) ) {
        StringTab  & stab = this->rte.user_db.string_tab;
        this->rte.mgr.shutdown_transport( *t );
        char old_name[ 256 ];
        int  name_len = ::snprintf( old_name, sizeof( old_name ), "%s_old",
                                    t->tport.val );
        stab.ref_string( old_name,
          min_int( (int) sizeof( old_name ) - 1, name_len ), t->tport );
        t = NULL;
        rte = NULL;
      }
    }
  }
  else {
    t = this->get_rv_transport( host, false );
    if ( t != NULL && hr == NULL )
      exists = true;
  }
  if ( host.network_len == 0 || exists )
    rte = NULL;
  else if ( rte == NULL || rte->is_set( TPORT_IS_SHUTDOWN ) ) {
    if ( t != NULL && rte == NULL )
      rte = this->rte.user_db.transport_tab.find_transport( t );

    if ( rte == NULL || rte->is_set( TPORT_IS_SHUTDOWN ) ) {
      bool b, is_listener;
      this->make_rv_transport( t, host, is_listener );

      if ( t == NULL )
        rte = NULL;
      else {
        if ( rte != NULL )
          b = this->rte.mgr.start_transport( *rte, is_listener );
        else
          b = this->rte.mgr.add_transport2( *t, is_listener, rte );
        if ( ! b )
          return -1;
        delay_secs = 1;
      }
    }
  }
  if ( hr == NULL ) {
    hr = this->tab.add( &host, rte, t );
    hr->tport_exists = exists;
  }
  else {
    hr->rte = rte;
    hr->cfg = t;
    hr->tport_exists = exists;
  }
  if ( not_running ) {
    printf( "start_network:        service %.*s, \"%.*s\"\n",
            (int) host.service_len, host.service, (int) host.network_len,
            host.network );
    this->last_active_mono = this->poll.timer.current_monotonic_time_ns();
    if ( hr != NULL ) {
      hr->last_active_mono = this->last_active_mono;
      hr->is_active = true;
    }
    this->active_cnt++;
  }
  return this->EvRvListen::start_host2( host, delay_secs );
}

int
EvRvTransportListen::stop_host( RvHost &host ) noexcept
{
  printf( "stop_network:         service %.*s, \"%.*s\"\n",
          (int) host.service_len, host.service, (int) host.network_len,
          host.network );
  uint64_t cur_mono = this->poll.timer.current_monotonic_time_ns();
  RvHostRoute * hr  = this->tab.find( &host );
  if ( hr != NULL ) {
    hr->last_active_mono = cur_mono;
    hr->is_active = false;
  }
  if ( --this->active_cnt == 0 && this->no_permanent ) {
    this->last_active_mono = cur_mono;
    this->poll.timer.add_timer_seconds( this->fd, RV_TIMEOUT_SECS,
                                        this->timer_id, RV_STOP_TIMER );
  }
  return this->EvRvListen::stop_host( host );
}
