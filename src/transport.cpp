#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <raims/transport.h>
#include <raims/session.h>
#include <raims/ev_tcp_transport.h>
#include <raims/ev_pgm_transport.h>
#include <raims/ev_inbox_transport.h>
#include <raims/ev_telnet.h>

using namespace rai;
using namespace ms;
using namespace kv;
using namespace md;

struct rai::ms::TcpConnectionMgr :
   public ConnectionMgr<EvTcpTransportClient> {
  TcpConnectionMgr( kv::EvPoll &p,  uint8_t type ) : ConnectionMgr( p, type ) {}
};

bool
UserDB::forward_pub( const MsgFramePublish &pub,  const UserBridge &,
                     const MsgHdrDecoder &dec ) noexcept
{
  bool b = true;
  if ( dec.is_mcast_type() ) {
    size_t count = this->transport_tab.count;
    if ( count > 1 || pub.rte.connect_count > 1 ) {
      kv::EvPublish tmp( pub );
      for ( size_t i = 0; i < count; i++ ) {
        TransportRoute * rte = this->transport_tab.ptr[ i ];
        tmp.pub_type = 'p';
        if ( rte->connect_count > 0 ) {
          if ( rte != &pub.rte )
            b &= rte->forward_to_connected_auth( tmp );
          else if ( rte->connect_count > 1 )
            b &= rte->forward_to_connected_auth_not_fd( tmp, pub.src_route );
        }
      }
    }
  }
  return b;
}

TransportRoute::TransportRoute( kv::EvPoll &p,  SessionMgr &m,
                                ConfigTree::Service &s,
                                ConfigTree::Transport &t,
                                const char *svc_name,  uint32_t svc_id,
                                uint32_t id,  bool is_service ) noexcept
    : EvSocket( p, p.register_type( "transport_route" ) ),
      poll( p ), mgr( m ),
      sub_route( p.sub_route.get_service( svc_name, svc_id ) ),
      switch_rt( NULL ), router_rt( NULL ),
      uid_in_mesh( &this->mesh_connected ),
      mesh_csum( &this->mesh_csum2 ),
      hb_time( 0 ), hb_mono_time( 0 ), hb_seqno( 0 ), reachable_seqno( 0 ),
      tport_id( id ), hb_count( 0 ), last_hb_count( 0 ),
      connect_count( 0 ), last_connect_count( 0 ), state( 0 ),
      mesh_id( 0 ), listener( 0 ), connect_mgr( 0 ), pgm_tport( 0 ),
      ibx_tport( 0 ), ucast_url_addr( 0 ), mesh_url_addr( 0 ),
      ucast_url_len( 0 ), mesh_url_len( 0 ), inbox_fd( -1 ),
      mcast_fd( -1 ), mesh_conn_hash( 0 ), oldest_uid( 0 ),
      primary_count( 0 ), svc( s ), transport( t )
{
  if ( is_service )
    this->set( TPORT_IS_SVC );
  this->sock_opts = OPT_NO_POLL;
  this->switch_rt = this->sub_route.create_bloom_route( m.fd, &m.sub_db.bloom );
  this->switch_rt->add_bloom_ref( &this->mgr.sys_bloom );
  this->mesh_csum2.zero();
  this->hb_cnonce.zero();
  for ( int i = 0; i < 3; i++ )
    this->auth[ i ].zero();
}

int
TransportRoute::init( void ) noexcept
{
  int pfd = this->poll.get_null_fd();
  d_tran( "tport %s fd %d\n", this->sub_route.service_name, pfd );
  this->PeerData::init_peer( pfd, NULL, "tport" );
  this->PeerData::set_name( this->sub_route.service_name,
                            ::strlen( this->sub_route.service_name ) );
  int status = this->poll.add_sock( this );
  if ( status != 0 )
    return status;
  this->mgr.router_set.add( pfd );
  this->router_rt =
    this->sub_route.create_bloom_route( pfd, &this->mgr.router_bloom );
  return 0;
}

bool
SessionMgr::add_startup_transports( ConfigTree::Service &s ) noexcept
{
  ConfigTree::Parameters * p;
  ConfigTree::StringPair * sp;
  ConfigTree::Transport  * tport;
  bool conn;
  for ( p = this->tree.parameters.hd; p != NULL; p = p->next ) {
    for ( sp = p->parms.hd; sp != NULL; sp = sp->next ) {
      if ( sp->name.equals( "listen" ) ) {
        tport = this->tree.find_transport( sp->value.val, sp->value.len, conn );
        if ( tport == NULL ) {
          fprintf( stderr, "startup listen transport %.*s not found\n",
                   (int) sp->value.len, sp->value.val );
          return false;
        }
        if ( ! this->add_transport( s, *tport, true ) )
          return false;
      }
    }
  }
  for ( p = this->tree.parameters.hd; p != NULL; p = p->next ) {
    for ( sp = p->parms.hd; sp != NULL; sp = sp->next ) {
      if ( sp->name.equals( "connect" ) ) {
        tport = this->tree.find_transport( sp->value.val, sp->value.len, conn );
        if ( tport == NULL ) {
          fprintf( stderr, "startup connect transport %.*s not found\n",
                   (int) sp->value.len, sp->value.val );
          return false;
        }
        if ( ! this->add_transport( s, *tport, false ) )
          return false;
      }
    }
  }

  return true;
}

bool
SessionMgr::add_transport( ConfigTree::Service &s,
                           ConfigTree::Transport &t,
                           bool is_service ) noexcept
{
  if ( ! this->in_list( IN_ACTIVE_LIST ) ) {
    if ( this->init_sock() != 0 )
      return false;
  }
  if ( t.type.equals( "telnet" ) )
    return this->create_telnet( t );

  TransportRoute * rte;
  void * p = aligned_malloc( sizeof( TransportRoute ) );
  char svc_name[ 256 ];
  ::snprintf( svc_name, sizeof( svc_name ), "%s.%s", s.svc.val, t.tport.val );
  uint32_t id = this->user_db.transport_tab.count;
  d_tran( "add transport %s tport_id %u\n", svc_name, id );
  rte = new ( p ) TransportRoute( this->poll, *this, s, t, svc_name, 0, id,
                                  is_service );
  if ( rte->init() != 0 )
    return false;
  this->user_db.transport_tab[ id ] = rte;
  if ( rte->create_transport() ) {
    this->user_db.add_transport( *rte );
    return true;
  }
  rte->set( TPORT_IS_SHUTDOWN );
  return false;
}

uint32_t
SessionMgr::shutdown_transport( ConfigTree::Service &s,
                                ConfigTree::Transport &t ) noexcept
{
  if ( t.type.equals( "telnet" ) )
    return this->shutdown_telnet();

  uint32_t id, count = this->user_db.transport_tab.count, match = 0;
  for ( id = 0; id < count; id++ ) {
    TransportRoute *rte = this->user_db.transport_tab.ptr[ id ];
    if ( &rte->transport == &t && &rte->svc == &s ) {
      match += rte->shutdown();
    }
  }
  return match;
}

bool
SessionMgr::start_transport( TransportRoute &rte,
                             bool is_service ) noexcept
{
  if ( rte.transport.type.equals( "tcp" ) ) {
    if ( is_service ) {
      if ( rte.listener != NULL )
        return rte.start_listener( rte.listener, false );
    }
    else {
      if ( rte.connect_mgr != NULL ) {
        rte.connect_mgr->restart();
        //rte.connect_mgr->is_shutdown = false;
        rte.clear( TPORT_IS_SHUTDOWN );
        rte.connect_mgr->do_connect();
        return true;
      }
    }
  }
  else if ( rte.transport.type.equals( "mesh" ) ) {
    if ( rte.listener != NULL ) {
      if ( rte.is_set( TPORT_IS_SHUTDOWN ) ) {
        if ( ! rte.start_listener( rte.listener, ! is_service ) )
          return false;
        rte.create_listener_mesh_url();
      }
      if ( ! is_service )
        rte.add_mesh_connect( NULL, 0 );
      return true;
    }
  }
  else if ( rte.transport.type.equals( "pgm" ) ) {
    rte.clear( TPORT_IS_LISTEN | TPORT_IS_CONNECT );
    if ( is_service ) {
      rte.set( TPORT_IS_LISTEN );
      if ( rte.create_pgm( TPORT_IS_LISTEN ) )
        return true;
      rte.set( TPORT_IS_SHUTDOWN );
      return false;
    }
    rte.set( TPORT_IS_CONNECT );
    if ( rte.create_pgm( TPORT_IS_CONNECT ) )
      return true;
    rte.set( TPORT_IS_SHUTDOWN );
    return false;
  }
  return false;
}

static size_t
make_mesh_url( char buf[ 256 ],  EvSocket &sock ) noexcept
{
  ::memcpy( buf, "tcp://", 6 );
  size_t len = get_strlen64( sock.peer_address.buf );
  ::memcpy( &buf[ 6 ], sock.peer_address.buf, len );
  len += 6;
  buf[ len ] = '\0';
  return len;
}

bool
SessionMgr::add_mesh_accept( TransportRoute &listen_rte,
                             EvTcpTransport &conn ) noexcept
{
  TransportRoute        * rte;
  ConfigTree::Service   & s = listen_rte.svc;
  ConfigTree::Transport & t = listen_rte.transport;
  const char            * svc_name = listen_rte.sub_route.service_name;

  d_tran( "add transport %s\n", svc_name );
  //uint32_t id = this->user_db.transport_list.tport_count++;
  uint32_t id = 0, count = this->user_db.transport_tab.count;
  for ( id = 0; id < count; id++ ) {
    rte = this->user_db.transport_tab.ptr[ id ];
    if ( rte->all_set( TPORT_IS_SHUTDOWN | TPORT_IS_MESH ) &&
         rte->mesh_id == conn.rte->mesh_id ) {
      if ( rte->connect_mgr == NULL || rte->connect_mgr->is_shutdown ) {
        rte->clear( TPORT_IS_SHUTDOWN | TPORT_IS_CONNECT );
        break;
      }
    }
  }
  if ( id == count ) {
    void * p = aligned_malloc( sizeof( TransportRoute ) );
    rte = new ( p ) TransportRoute( this->poll, *this, s, t, svc_name, id, id,
                                    false );
    if ( rte->init() != 0 )
      return false;
  }
  if ( rte->connect_mgr != NULL ) {
    this->poll.push_free_list( rte->connect_mgr );
    rte->connect_mgr = NULL;
  }
  rte->mesh_url_addr = listen_rte.mesh_url_addr;
  rte->mesh_url_len  = listen_rte.mesh_url_len;
  rte->mesh_id       = listen_rte.mesh_id;
  rte->uid_in_mesh   = listen_rte.uid_in_mesh;
  rte->mesh_csum     = listen_rte.mesh_csum;
  rte->set( TPORT_IS_MESH );

  char buf[ 256 ];
  make_mesh_url( buf, conn );
  rte->mesh_conn_hash = kv_crc_c( buf, ::strlen( buf ), 0 );
  conn.rte    = rte;
  conn.notify = rte;

  printf( "%s.%u add_mesh_accept %s %x\n",
        rte->transport.tport.val, rte->tport_id, rte->mesh_url_addr,
        rte->mesh_conn_hash );
  if ( id == count ) {
    this->user_db.transport_tab[ id ] = rte;
    this->user_db.add_transport( *rte );
  }
  this->events.on_connect( rte->tport_id, TPORT_IS_MESH );
  if ( ! rte->connected.test_set( conn.fd ) )
    rte->connect_count++;
  d_tran( "%s connect_count %u\n", rte->name, rte->connect_count );
  return true;
}

bool
SessionMgr::add_tcp_accept( TransportRoute &listen_rte,
                            EvTcpTransport &conn ) noexcept
{
  TransportRoute        * rte;
  ConfigTree::Service   & s = listen_rte.svc;
  ConfigTree::Transport & t = listen_rte.transport;
  const char            * svc_name = listen_rte.sub_route.service_name;

  d_tran( "add transport %s\n", svc_name );
  //uint32_t id = this->user_db.transport_list.tport_count++;
  uint32_t id = 0, count = this->user_db.transport_tab.count;
  for ( id = 0; id < count; id++ ) {
    rte = this->user_db.transport_tab.ptr[ id ];
    if ( rte->all_set( TPORT_IS_SHUTDOWN | TPORT_IS_TCP ) ) {
      if ( rte->connect_mgr == NULL || rte->connect_mgr->is_shutdown ) {
        rte->clear( TPORT_IS_SHUTDOWN | TPORT_IS_CONNECT );
        break;
      }
    }
  }
  if ( id == count ) {
    void * p = aligned_malloc( sizeof( TransportRoute ) );
    rte = new ( p ) TransportRoute( this->poll, *this, s, t, svc_name, id, id,
                                    false );
    if ( rte->init() != 0 )
      return false;
  }
  if ( rte->connect_mgr != NULL ) {
    this->poll.push_free_list( rte->connect_mgr );
    rte->connect_mgr = NULL;
  }
  rte->set( TPORT_IS_TCP );

  conn.rte    = rte;
  conn.notify = rte;

  printf( "%s.%u add_tcp_accept\n", rte->transport.tport.val, rte->tport_id );
  if ( id == count ) {
    this->user_db.transport_tab[ id ] = rte;
    this->user_db.add_transport( *rte );
  }
  this->events.on_connect( rte->tport_id, TPORT_IS_TCP );
  if ( ! rte->connected.test_set( conn.fd ) )
    rte->connect_count++;
  d_tran( "%s connect_count %u\n", rte->name, rte->connect_count );
  return true;
}

static void
parse_tcp_param( EvTcpTransportParameters &parm,  const char *name,
                 ConfigTree::Transport &tport,  char host_buf[ 256 ] ) noexcept
{
  size_t len = 256;
  tport.get_route_str( name, parm.host );
  if ( ! tport.get_route_int( "port", parm.port ) )
    parm.port = tport.get_host_port( parm.host, host_buf, len );
  if ( ! tport.get_route_int( "timeout", parm.timeout ) )
    parm.timeout = 15;
  if ( ! tport.get_route_bool( "edge", parm.edge ) )
    parm.edge = false;
  if ( tport.is_wildcard( parm.host ) )
    parm.host = NULL;
}

static size_t
make_mesh_url( char buf[ 256 ],  ConfigTree::Transport &tport ) noexcept
{
  EvTcpTransportParameters parm;
  char host_buf[ 256 ];
  size_t i = 6, j = 0;
  ::memcpy( buf, "tcp://", 6 );
  parse_tcp_param( parm, "connect", tport, host_buf );
  if ( parm.host == NULL ) {
    ::memcpy( &buf[ 6 ], "127.0.0.1", 9 );
    i = 15;
  }
  else {
    for ( j = 0; parm.host[ j ] != '\0'; j++ ) {
      if ( i < 255 )
        buf[ i++ ] = parm.host[ j ];
    }
  }
  char pbuf[ 24 ];
  if ( parm.port != 0 ) {
    uint32_to_string( parm.port, pbuf );
  }
  else {
    ::strcpy( pbuf, "28989" );
  }
  if ( i < 255 )
    buf[ i++ ] = ':';
  for ( j = 0; pbuf[ j ] != '\0'; j++ ) {
    if ( i < 255 )
      buf[ i++ ] = pbuf[ j ];
  }
  buf[ i ] = '\0';
  return i;
}

bool
TransportRoute::add_mesh_connect( const char *mesh_url,
                                  uint32_t mesh_hash ) noexcept
{
  return this->mgr.add_mesh_connect( *this, mesh_url, mesh_hash );
}

bool
SessionMgr::add_mesh_connect( TransportRoute &mesh_rte,
                              const char *mesh_url,
                              uint32_t mesh_hash ) noexcept
{
  TransportRoute * rte;
  char url_buf[ 256 ];

  if ( mesh_rte.mesh_id == NULL )
    return true;

  if ( mesh_url == NULL ) {
    size_t sz = make_mesh_url( url_buf, mesh_rte.transport );
    mesh_url  = url_buf;
    mesh_hash = kv_crc_c( url_buf, sz, 0 );
  }

  uint32_t id    = 0,
           count = this->user_db.transport_tab.count;

  printf( "%s.%u add_mesh_connect %s: %x\n",
        mesh_rte.transport.tport.val, mesh_rte.tport_id, mesh_url, mesh_hash );
  for ( id = 0; id < count; id++ ) {
    rte = this->user_db.transport_tab.ptr[ id ];
    if ( rte->mesh_id == mesh_rte.mesh_id ) {
      if ( rte->mesh_conn_hash == mesh_hash ) {
        if ( ! rte->is_set( TPORT_IS_SHUTDOWN ) ) {
          printf( "already connected (%s)\n", mesh_url );
          return true;
        }
        rte->clear( TPORT_IS_SHUTDOWN );
        break;
      }
    }
  }
  if ( id == count ) {
    for ( id = 0; id < count; id++ ) {
      rte = this->user_db.transport_tab.ptr[ id ];
      if ( rte->all_set( TPORT_IS_SHUTDOWN | TPORT_IS_MESH ) &&
           rte->mesh_id == mesh_rte.mesh_id ) {
        if ( rte->connect_mgr == NULL || rte->connect_mgr->is_shutdown ) {
          rte->clear( TPORT_IS_SHUTDOWN );
          break;
        }
      }
    }
    if ( id == count ) {
      void * p = aligned_malloc( sizeof( TransportRoute ) );
      ConfigTree::Service &s = mesh_rte.svc;
      ConfigTree::Transport &t = mesh_rte.transport;
      const char * svc_name = mesh_rte.mesh_id->sub_route.service_name;
      d_tran( "add transport %s\n", svc_name );
      rte = new ( p ) TransportRoute( this->poll, *this, s, t, svc_name,
                                      id, id, false );
      if ( rte->init() != 0 )
        return false;
    }
  }
  rte->mesh_url_addr  = mesh_rte.mesh_url_addr;
  rte->mesh_url_len   = mesh_rte.mesh_url_len;
  rte->mesh_id        = mesh_rte.mesh_id;
  rte->uid_in_mesh    = mesh_rte.uid_in_mesh;
  rte->mesh_csum      = mesh_rte.mesh_csum;
  rte->mesh_conn_hash = mesh_hash;
  rte->set( TPORT_IS_MESH | TPORT_IS_CONNECT );
  this->user_db.transport_tab[ id ] = rte;
  this->user_db.add_transport( *rte );
#if 0
  EvTcpTransportClient *conn;
  uint8_t sock_type = this->tcp_connect_sock_type;
  conn = this->poll.get_free_list<EvTcpTransportClient>( sock_type );
  conn->rte = rte;

  EvTcpTransportParameters parm;
  char host_buf[ 256 ];
  size_t len = sizeof( host_buf );
  parm.port = ConfigTree::Transport::get_host_port( mesh_url, host_buf, len );
  parm.host = host_buf;

  EvTcpTransportParameters &p = conn->parm;
  if ( len > 0 ) {
    p.host = conn->host_buf;
    ::memcpy( conn->host_buf, parm.host, len + 1 );
  }
  else {
    p.host = NULL;
  }
  p.port = parm.port;

  if ( ! conn->connect( rte ) ) {
    this->poll.push_free_list( conn );
    rte->set( TPORT_IS_SHUTDOWN );
    return false;
  }
  return true;
#endif
  EvTcpTransportParameters parm;
  char   host_buf[ 256 ];
  size_t len = sizeof( host_buf );
  parm.port = ConfigTree::Transport::get_host_port( mesh_url, host_buf, len );
  parm.host = host_buf;
  if ( ! mesh_rte.transport.get_route_int( "timeout", parm.timeout ) )
    parm.timeout = 15;

  TcpConnectionMgr *conn = rte->connect_mgr;
  if ( conn == NULL ) {
    uint8_t sock_type = this->tcp_conn_mgr_sock_type;
    conn = this->poll.get_free_list<TcpConnectionMgr>( sock_type );
    rte->connect_mgr = conn;
  }
  conn->EvTcpTransport::rte = rte;
  conn->ReconnectMgr::rte = rte;
  conn->ReconnectMgr::connect_timeout_secs = parm.timeout;
  conn->parm.copy( parm, conn->host_buf );
  if ( ! conn->do_connect() ) {
    rte->set( TPORT_IS_SHUTDOWN );
    return false;
  }
  return true;
}

bool
TransportRoute::on_msg( EvPublish &pub ) noexcept
{
  if ( pub.src_route == (uint32_t) this->mgr.fd ) {
    d_tran( "xxx discard %s transport_route: on_msg (%.*s)\n",
            ( pub.src_route == (uint32_t) this->fd ? "from tport" : "from mgr" ),
            (int) pub.subject_len, pub.subject );
    return true;
  }
  if ( pub.pub_type != 'X' ) {
    d_tran( "pub_type != X (%c) transport_route: on_msg (%.*s)\n",
            pub.pub_type, (int) pub.subject_len, pub.subject );
    return true;
  }
  MsgFramePublish & fpub = (MsgFramePublish &) pub;
  MsgHdrDecoder   & dec  = fpub.dec;
  if ( ( fpub.flags & MSG_FRAME_TPORT_CONTROL ) != 0 ) {
    d_tran( "tport_route == true transport_route: on_msg (%.*s)\n",
            (int) pub.subject_len, pub.subject );
    return true;
  }
  fpub.flags |= MSG_FRAME_TPORT_CONTROL;
  UserDB & user_db = this->mgr.user_db;
  if ( fpub.n == NULL ) {
    if ( (fpub.n = user_db.lookup_bridge( fpub, dec )) == NULL ) {
      d_tran( "ignore_msg status %d transport_route: on_msg (%.*s)\n",
              fpub.status, (int) pub.subject_len, pub.subject );
      return true;
    }
  }
  UserBridge &n = *fpub.n;
  if ( ( fpub.flags & MSG_FRAME_ACK_CONTROL ) == 0 ) {
    uint16_t opt = dec.msg->caba.opt_flag();
    fpub.flags |= MSG_FRAME_ACK_CONTROL;
    if ( ( opt & CABA_OPT_TRACE ) != 0 ) {
      if ( dec.is_set == 0 && dec.decode_msg() != 0 )
        return true;
      this->mgr.send_ack( fpub, n, dec, _TRACE );
    }
  }
  UserBridge * ptp_bridge;
  CabaTypeFlag tflag = dec.msg->caba.type_flag();
  bool         b     = true;
  if ( tflag == CABA_INBOX ) {
    if ( (ptp_bridge = user_db.is_inbox_sub( pub.subject,
                                             pub.subject_len )) != NULL ) {
      TransportRoute &rte = ptp_bridge->primary( user_db )->rte;
      if ( &rte != this ) {
        d_tran( "transport_route: inbox (%.*s) -> %s\n",
                (int) pub.subject_len, pub.subject, rte.name );
        b = user_db.forward_to_inbox( *ptp_bridge, pub.subject,
                                      pub.subject_len, pub.subj_hash,
                                      pub.msg, pub.msg_len );
      }
      return b;
    }
  }
  /* cache of the multicast tree for messages originating at n */
  ReversePathForward & forward  = n.reverse_path_cache;
  uint32_t             i, count = user_db.transport_tab.count;
  TransportRoute     * rte;
  AdjDistance        & peer_dist = user_db.peer_dist;
  /* if forward is valid, send to the ports calculated below */
  if ( peer_dist.is_valid( forward.adjacency_cache_seqno ) ) {
    if ( forward.first( i, count ) ) {
      do {
        rte = user_db.transport_tab.ptr[ i ];
        b  &= rte->sub_route.forward_except( pub, this->mgr.router_set );
      } while ( forward.next( i, count ) );
    }
    return b;
  }
  /* recalculate forward vector */
  forward.clear( count, peer_dist.cache_seqno );

  uint32_t         min_dist    = peer_dist.max_uid,
                   my_dist     = min_dist;/*,
                 * dist        = peer_dist.tport_dist;*/
  TransportRoute * min_rte     = this;
  bool             matched_min = false;
  /* check that this route is the min distance to avoid loops */
  for ( i = 0; i < count; i++ ) {
    rte = user_db.transport_tab.ptr[ i ];
    uint32_t d = peer_dist.calc_transport_cache2( n.uid, i, *rte );
    if ( d < min_dist ) {
      min_dist    = d;
      min_rte     = rte;
      matched_min = false;
    }
    else if ( d == min_dist ) {
      if ( rte == this )
        matched_min = true; /* if multiple paths with min dist */
    }
    if ( rte == this )
      my_dist = d;
    /*dist[ i ] = d;*/
  }
  /* another route was closer */
  if ( min_rte != this && ! matched_min ) {
    d_tran( "tport=%s(d=%u): faster=%s(d=%u) (%.*s)\n",
             this->name, my_dist, min_rte->name, min_dist,
             (int) pub.subject_len, pub.subject );
    return b;
  }
  /* calculate routes at a distance */
  if ( peer_dist.calc_dist_peers( n.uid, my_dist + 1 ) != 0 ) {
    /* for each route, forward when I match */
    count = peer_dist.uid_next.idx;
    for ( i = 0; i < count; i++ ) {
      PeerUidSet * rec = peer_dist.uid_next.ptr[ i ];
      if ( rec->src_uid == UserDB::MY_UID )
        rte = user_db.transport_tab.ptr[ rec->tport_id ];
      else
        rte = NULL;

      if ( debug_tran /*|| pub.subj_hash == 0x27d61d90*/ ) {
        char buf[ 120 ];
        printf( "next(%s.%u,port=%s.%u,cnt=%u): %s\n",
           ( rec->src_uid==0 ? "me" :
             this->mgr.user_db.bridge_tab[ rec->src_uid ]->peer.user.val ),
           rec->src_uid, ( rte ? rte->transport.tport.val : "" ),
           rec->tport_id, rec->dest_count,
           peer_dist.uid_set_names( *rec, buf, sizeof( buf ) ) );
      }
      if ( rte != NULL ) {
        forward.add( rec->tport_id );
        d_tran( "forward to %s.%u\n",
                rte->transport.tport.val, rec->tport_id );
        b &= rte->sub_route.forward_except( pub, this->mgr.router_set );
      }
    }
  }
  else {
    d_tran( "no uid peers\n" );
  }
  return b;
}

const char *
TransportRoute::connected_names( char *buf,  size_t buflen ) noexcept
{
  return this->uid_names( this->uid_connected, buf, buflen );
}

const char *
TransportRoute::reachable_names( char *buf,  size_t buflen ) noexcept
{
  if ( this->reachable_seqno != this->mgr.user_db.peer_dist.update_seqno )
    this->mgr.user_db.peer_dist.calc_reachable( *this );
  return this->uid_names( this->reachable, buf, buflen );
}

const char *
TransportRoute::uid_names( const BitSpace &uids,  char *buf,
                           size_t buflen ) noexcept
{
  uint32_t uid;
  size_t   off = 0;
  buf[ 0 ] = '\0';
  for ( bool ok = uids.first( uid ); ok; ok = uids.next( uid ) ) {
    if ( this->mgr.user_db.bridge_tab.ptr[ uid ] == NULL )
      continue;
    const UserBridge &n = *this->mgr.user_db.bridge_tab.ptr[ uid ];
    off += ::snprintf( &buf[ off ], buflen - off, "%s.%u ",
                       n.peer.user.val, uid );
  }
  if ( off > 0 )
    buf[ off - 1 ] = '\0';
  return buf;
}

size_t
TransportRoute::port_status( char *buf,  size_t buflen ) noexcept
{
  buf[ 0 ] = '\0';
  if ( this->listener != NULL )
    return this->listener->print_sock_error( buf, buflen );
  if ( this->connect_mgr != NULL )
    return this->connect_mgr->print_sock_error( buf, buflen );
  if ( this->pgm_tport != NULL )
    return this->pgm_tport->print_sock_error( buf, buflen );
  return 0;
}

void
TransportRoute::on_connect( kv::EvSocket &conn ) noexcept
{
  printf( "connected %s %s using %s fd %u\n", conn.peer_address.buf,
          conn.type_string(), this->sub_route.service_name, conn.fd );
  uint32_t connect_type = 0;
  this->clear( TPORT_IS_SHUTDOWN );
  if ( ! this->is_mcast() ) {
    EvTcpTransport &tcp = (EvTcpTransport &) conn;
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
  }
  this->mgr.events.on_connect( this->tport_id, connect_type );
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
  if ( errlen == 0 ) {
    errlen = conn.print_sock_error( errbuf, sizeof( errbuf ) );
    if ( errlen > 0 )
      err = errbuf;
  }
  if ( errlen > 0 )
    printf( "%s %s (%.*s)\n", s, conn.peer_address.buf, (int) errlen, err );
  else
    printf( "%s %s (count=%u)\n", s, conn.peer_address.buf,
            this->connect_count );
  this->mgr.events.on_shutdown( this->tport_id, conn.fd >= 0 );
  if ( conn.fd >= 0 ) {
    this->mgr.user_db.retire_source( conn.fd );
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
  /* mesh accept, tcp accept, mesh connect, but not tcp_connect_mgr */
  if ( conn.sock_type == this->mgr.tcp_accept_sock_type ||
       conn.sock_type == this->mgr.tcp_connect_sock_type ) {
    printf( "push free list %s\n", conn.type_string() );
    this->poll.push_free_list( &conn );
  }
}

void
TransportRoute::create_listener_mesh_url( void ) noexcept
{
  char   tmp[ 256 ];
  size_t len = make_mesh_url( tmp, *this->listener );
  char * url = this->mesh_url_addr;
  if ( url == NULL )
    url = (char *) ::malloc( 256 );
  ::memcpy( url, tmp, len + 1 );
  this->mesh_url_addr = url;
  this->mesh_url_len  = len;
  d_tran( "%s: %s\n", this->name, url );
}

bool
TransportRoute::create_transport( void ) noexcept
{
  if ( this->transport.type.equals( "tcp" ) ) {
    if ( this->is_svc() ) {
      this->listener = this->create_tcp_listener( false );
      if ( this->listener == NULL )
        this->set( TPORT_IS_SHUTDOWN );
      return this->listener != NULL;
    }
    bool b = this->create_tcp_connect();
    if ( ! b )
      this->set( TPORT_IS_SHUTDOWN );
    else
      this->set( TPORT_IS_CONNECT );
    return b;
  }
  if ( this->transport.type.equals( "pgm" ) ) {
    this->set( TPORT_IS_MCAST );
    if ( this->is_svc() ) {
      if ( this->create_pgm( TPORT_IS_LISTEN ) )
        return true;
      this->set( TPORT_IS_SHUTDOWN );
      return false;
    }
    if ( this->create_pgm( TPORT_IS_CONNECT ) ) {
      this->set( TPORT_IS_CONNECT );
      return true;
    }
    this->set( TPORT_IS_SHUTDOWN );
    return false;
  }
  if ( this->transport.type.equals( "mesh" ) ) {
    EvTcpTransportListen *l;
    this->set( TPORT_IS_MESH );
    this->mesh_id = this;
    if ( this->is_svc() )
      l = this->create_mesh_listener();
    else
      l = this->create_mesh_rendezvous();
    if ( l == NULL ) {
      this->set( TPORT_IS_SHUTDOWN );
      return false;
    }
    this->listener = l;
    this->create_listener_mesh_url();

    if ( ! this->is_svc() ) {
      this->add_mesh_connect( NULL, 0 );
#if 0
      TcpConnectionMgr * conn = this->create_mesh_connect();
      if ( conn == NULL )
        return false;
      this->connect_mgr = conn;
      len = make_mesh_url( tmp, *conn );
      this->mesh_conn_hash = kv_crc_c( tmp, len, 0 );
      printf( "%s: %x\n", tmp, this->mesh_conn_hash );
#endif
    }
    return true;
  }
  return false;
}

uint32_t
TransportRoute::shutdown( void ) noexcept
{
  uint32_t count = 0;
  if ( this->transport.type.equals( "tcp" ) ||
       this->transport.type.equals( "mesh" ) ) {
    if ( this->listener != NULL ) {
      uint32_t fd, uid;
      if ( ! this->test_set( TPORT_IS_SHUTDOWN ) ) {
        count++;
        this->listener->idle_push( EV_CLOSE );
      }
      if ( ! this->is_set( TPORT_IS_MESH ) ) {
        if ( this->connect_count > 0 ) {
          for ( bool ok = this->connected.first( fd ); ok;
                ok = this->connected.next( fd ) ) {
            if ( fd < this->poll.maxfd ) {
              EvSocket *s = this->poll.sock[ fd ];
              if ( s != NULL ) {
                s->idle_push( EV_SHUTDOWN );
                count++;
              }
            }
          }
        }
      }
      else {
        UserDB & user_db = this->mgr.user_db;
        uint32_t i, tport_count = user_db.transport_tab.count;
        for ( bool ok = this->uid_in_mesh->first( uid ); ok;
              ok = this->uid_in_mesh->next( uid ) ) {
          UserBridge &n = *this->mgr.user_db.bridge_tab.ptr[ uid ];
          for ( i = 0; i < tport_count; i++ ) {
            UserRoute * u_ptr = n.user_route_ptr( user_db, i );
            if ( u_ptr->is_valid() && u_ptr->rte.mesh_id == this->mesh_id ) {
              fd = u_ptr->mcast_fd;
              if ( fd < this->poll.maxfd ) {
                EvSocket *s = this->poll.sock[ fd ];
                if ( s != NULL ) {
                  s->idle_push( EV_SHUTDOWN );
                  count++;
                }
                else if ( u_ptr->rte.connect_mgr != NULL &&
                          ! u_ptr->rte.connect_mgr->is_shutdown )
                  count++;
              }
              if ( u_ptr->rte.connect_mgr != NULL ) {
                u_ptr->rte.connect_mgr->is_shutdown = true;
                u_ptr->rte.set( TPORT_IS_SHUTDOWN );
              }
            }
          }
        }
      }
    }
    else if ( this->connect_mgr != NULL ) {
      if ( this->connect_mgr->fd >= 0 &&
           (uint32_t) this->connect_mgr->fd < this->poll.maxfd ) {
        this->connect_mgr->idle_push( EV_SHUTDOWN );
        count++;
      }
      else if ( ! this->connect_mgr->is_shutdown )
        count++;
      this->connect_mgr->is_shutdown = true;
      this->set( TPORT_IS_SHUTDOWN );
    }
  }
  else if ( this->transport.type.equals( "pgm" ) ) {
    if ( ! this->test_set( TPORT_IS_SHUTDOWN ) ) {
      if ( this->pgm_tport != NULL )
        this->pgm_tport->idle_push( EV_SHUTDOWN );
      if ( this->ibx_tport != NULL )
        this->ibx_tport->idle_push( EV_SHUTDOWN );
      count++;
    }
  }
  return count;
}

bool
TransportRoute::start_listener( EvTcpTransportListen *l,
                                bool rand_port ) noexcept
{
  EvTcpTransportParameters parm;
  char host_buf[ 256 ];
  parse_tcp_param( parm, "listen", this->transport, host_buf );

  if ( rand_port )
    parm.port = 0;
  int status = l->listen( parm.host, parm.port, DEFAULT_TCP_LISTEN_OPTS );
  if ( status != 0 ) {
    printf( "listen %s:%u failed\n",
            ConfigTree::Transport::is_wildcard( parm.host ) ? "*" : parm.host,
            parm.port );
    this->mgr.events.on_shutdown( this->tport_id, false );
    this->clear( TPORT_IS_LISTEN );
    this->set( TPORT_IS_SHUTDOWN );
    return false;
  }
  this->mgr.events.on_connect( this->tport_id, TPORT_IS_LISTEN );
  this->set( TPORT_IS_LISTEN );
  if ( parm.edge )
    this->set( TPORT_IS_EDGE );
  else
    this->clear( TPORT_IS_EDGE );
  this->clear( TPORT_IS_SHUTDOWN );
  printf( "listening on %s%s\n", l->peer_address.buf,
          this->is_set( TPORT_IS_EDGE ) ? " edge is true" : "" );
  return true;
}

EvTcpTransportListen *
TransportRoute::create_tcp_listener( bool rand_port ) noexcept
{
  EvTcpTransportListen * l =
    new ( aligned_malloc( sizeof( EvTcpTransportListen ) ) )
    EvTcpTransportListen( this->poll, *this );
  this->start_listener( l, rand_port );
  return l;
}

bool
TransportRoute::create_tcp_connect( void ) noexcept
{
  EvTcpTransportParameters parm;
  char   host_buf[ 256 ];
  parse_tcp_param( parm, "connect", this->transport, host_buf );

  TcpConnectionMgr *conn;
  uint8_t sock_type = this->mgr.tcp_conn_mgr_sock_type;
  conn = this->poll.get_free_list<TcpConnectionMgr>( sock_type );
  this->connect_mgr = conn;
  conn->EvTcpTransport::rte = this;
  conn->ReconnectMgr::rte = this;
  conn->ReconnectMgr::connect_timeout_secs = parm.timeout;
  conn->parm.copy( parm, conn->host_buf );
  return conn->do_connect();
}

EvTcpTransportListen *
TransportRoute::create_mesh_listener( void ) noexcept
{
  return this->create_tcp_listener( false );
}

EvTcpTransportListen *
TransportRoute::create_mesh_rendezvous( void ) noexcept
{
  return this->create_tcp_listener( true );
}

bool
SessionMgr::create_telnet( ConfigTree::Transport &t ) noexcept
{
  if ( this->telnet == NULL ) {
    void * p = aligned_malloc( sizeof( TelnetListen ) );
    this->telnet = new ( p ) TelnetListen( this->poll );
  }
  TelnetListen * l = this->telnet;
  EvTcpTransportParameters parm;
  char host_buf[ 256 ];
  parse_tcp_param( parm, "listen", t, host_buf );
  this->telnet_tport = &t;

  if ( ! l->in_list( IN_ACTIVE_LIST ) ) {
    if ( l->listen( parm.host, parm.port, DEFAULT_TCP_LISTEN_OPTS,
                   "telnet_listen" ) != 0 )
      return false;
    l->console = &this->console;
    printf( "%s listening on %s\n", t.tport.val, l->peer_address.buf );
  }
  else {
    printf( "%s is already active on %s\n", t.tport.val, l->peer_address.buf );
  }
  return true;
}

uint32_t
SessionMgr::shutdown_telnet( void ) noexcept
{
  if ( this->telnet == NULL )
    return 0;
  TelnetListen * l = this->telnet;
  if ( l->in_list( IN_ACTIVE_LIST ) ) {
    l->idle_push( EV_SHUTDOWN );
    return 1;
  }
  return 0;
}

static void
parse_pgm_param( EvPgmTransportParameters &parm,  const char *name,
                 ConfigTree::Transport &tport,  char net_buf[ 1024 ] ) noexcept
{
  size_t len = 1024;
  int    ival;
  tport.get_route_str( name, parm.network );
  if ( ! tport.get_route_int( "port", parm.port ) )
    parm.port = tport.get_host_port( parm.network, net_buf, len );
  if ( tport.is_wildcard( parm.network ) )
    parm.network = NULL;

  if ( tport.get_route_int( "mtu", ival ) )
    parm.mtu = ival;
  if ( tport.get_route_int( "txw_sqns", ival ) )
    parm.txw_sqns = ival;
  if ( tport.get_route_int( "rxw_sqns", ival ) )
    parm.rxw_sqns = ival;
  if ( tport.get_route_int( "mcast_loop", ival ) )
    parm.mcast_loop = ival;
}

bool
TransportRoute::create_pgm( int kind ) noexcept
{
  EvPgmTransportParameters parm;
  char         net_buf[ 1024 ];
  const char * name = ( kind & TPORT_IS_LISTEN ) ? "listen" : "connect";
  parse_pgm_param( parm, name, this->transport, net_buf );

  EvPgmTransport * l;
  if ( this->pgm_tport != NULL )
    l = this->pgm_tport;
  else
    l = new ( aligned_malloc( sizeof( EvPgmTransport ) ) )
        EvPgmTransport( this->poll, *this );

  if ( ! l->connect( parm, this ) )
    return false;
  this->pgm_tport = l;
  this->state    |= kind;

  EvInboxTransport * s;
  if ( this->ibx_tport != NULL )
    s = this->ibx_tport;
  else
    s = new ( aligned_malloc( sizeof( EvInboxTransport ) ) )
        EvInboxTransport( this->poll, *this );
  this->ibx_tport = s;

  s->mtu = parm.mtu;
  uint16_t port;
  rand::fill_urandom_bytes( &port, 2 );
  port = ( port % 0xc000 ) + 0x4000U; /* port range 16384 -> 65535 */
  for ( uint32_t i = 0; ; port++ ) {
    if ( port < 0x4000 )
      port = 0x4000;
    if ( s->listen( l->pgm.gsr_addr, port ) ) /* could print error if used */
      break;
    if ( ++i == 0xc000U )
      return false;
  }
  size_t len = ::strlen( l->pgm.gsr_addr ) + sizeof( "inbox://" ) + 8;
  char * url = this->ucast_url_addr;
  if ( url == NULL )
    url = (char *) ::malloc( 256 );
  len = ::snprintf( url, len, "inbox://%s:%u", l->pgm.gsr_addr, port );
  this->ucast_url_addr = url;
  this->ucast_url_len  = len;
  this->inbox_fd       = s->fd;
  this->mcast_fd       = l->fd;
  d_tran( "set mcast_fd=%u inbox_route=%u\n", l->fd, s->fd );
  return true;
}

void
ReconnectMgr::connect_failed( EvSocket &conn ) noexcept
{
  this->rte->on_shutdown( conn, NULL, 0 );
  if ( ! this->setup_reconnect() ) {
    printf( "reconnected failed (connect_failed)\n" );
  }
  else {
    printf( "reconnect timer running (connect_failed)\n" );
  }
}

void
ReconnectMgr::on_connect( EvSocket &conn ) noexcept
{
  this->rte->on_connect( conn );
}

void
ReconnectMgr::on_shutdown( EvSocket &conn,  const char *err,
                           size_t errlen ) noexcept
{
  this->rte->on_shutdown( conn, err, errlen );
  if ( ! this->setup_reconnect() ) {
    printf( "reconnect failed (on_shutdown)\n" );
  }
  else {
    printf( "reconnect timer running (on_shutdown)\n" );
  }
}

bool
ReconnectMgr::setup_reconnect( void ) noexcept
{
  if ( this->is_reconnecting || this->is_shutdown || this->poll.quit )
    return true;

  this->is_reconnecting = true;
  double now    = current_monotonic_time_s(),
         period = 60;
  if ( this->connect_timeout_secs > 0 )
    period = this->connect_timeout_secs + 15;

  if ( this->reconnect_time + period < now ) {
    this->reconnect_timeout_secs = 1;
    this->reconnect_time = now;
  }
  else {
    this->reconnect_timeout_secs =
      min<uint16_t>( this->reconnect_timeout_secs + 2, 10 );
  }
  if ( this->connect_timeout_secs > 0 ) {
    if ( now - this->reconnect_time > (double) this->connect_timeout_secs ) {
      this->is_reconnecting = false;
      return false;
    }
  }
  printf( "reconnect in %u seconds\n", this->reconnect_timeout_secs );
  this->poll.timer.add_timer_seconds( this->cb,
                                      this->reconnect_timeout_secs, 0, 0 );
  return true;
}

void TransportRoute::write( void ) noexcept {}
void TransportRoute::read( void ) noexcept {}
void TransportRoute::process( void ) noexcept {}
void TransportRoute::release( void ) noexcept {}

