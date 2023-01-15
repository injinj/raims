#ifndef __rai_raims__session_h__
#define __rai_raims__session_h__

#include <raims/user_db.h>
#include <raims/sub_const.h>
#include <raims/console.h>
#include <raims/event_rec.h>
#include <raims/stats.h>

extern "C" {
const char *ms_get_version( void );
}

namespace rai {
namespace ms {

/* quick lookup of PublishType to resolve _subscripions:
 *  _X.HELLO -> U_SESSION_HELLO
 *  _X.HB    -> U_SESSION_HB
 *  _X.BYE   -> U_SESSION_BYE
 *  _Z.ADD   -> U_PEER_ADD
 *  _Z.DEL   -> U_PEER_DEL
 *  _Z.BLM   -> U_BLOOM_FILTER
 *  _Z.ADJ   -> U_ADJACENCY
 *  _S.JOIN. -> U_SUB_JOIN
 *  _S.LEAV. -> U_SUB_LEAV
 *  _P.PSUB. -> U_PSUB_START
 *  _P.STOP. -> U_PSUB_STOP
 */
struct UScoreTab {
  uint8_t tab[ 64 ];
  uint64_t len_valid;
  UScoreTab() { this->init(); }

  void init( void ) { memset( this->tab, 0, sizeof( this->tab ) );
                      this->len_valid = 0; }

  PublishType lookup( uint32_t h,  size_t len ) {
    if ( len <= 63 && ( this->len_valid & ( (uint64_t) 1 << len ) ) != 0 ) {
      size_t  pos   = ( h & 127 ) >> 1;
      uint8_t shift = ( h & 1 ) * 4;
      uint8_t mask  = 0xf << shift;
      return (PublishType) ( ( this->tab[ pos ] & mask ) >> shift );
    }
    return U_NORMAL;
  }
  bool set( uint32_t h,  size_t len,  PublishType t ) {
    if ( len > 63 || t > 15 ) return false;       /* no longer than 63 */
    this->len_valid |= ( (uint64_t) 1 << len );   /* 6,7,8,10,11,12,15 valid */
    if ( this->lookup( h, len ) != U_NORMAL ) return false; /* unique entry */
    size_t  pos   = ( h & 127 ) >> 1;             /* only need 4 bits */
    uint8_t shift = ( h & 1 ) * 4;                /* shift is 0 or 4 */
    this->tab[ pos ] = ( this->tab[ pos ] | ( (uint8_t) t << shift ) );
    return true;
  }
};
/* varialbe length subject */
struct SubjectVar : public md::MDMsgMem, public MsgBuf {
  SubjectVar( const char *pre,  size_t pre_len,  const char *suf,  size_t len )
      : MsgBuf( (char *) this->make( pre_len + len ) ) {
    this->b( pre, pre_len );
    if ( len > 0 ) this->b( suf, len );
  }
};

template < PublishType max, PublishType min, class Subject >
struct HashPub {
  uint32_t hash; /* hash( _INBOX.Nonce. ) */
  uint16_t len;  /* len ( _INBOX.Nonce. ) */
  uint64_t sub[ max - min ];

  HashPub() : hash( 0 ), len( 0 ) {
    ::memset( this->sub, 0, sizeof( this->sub ) );
  }
  void init( Subject &subj,  const char *sub,  PublishType val ) {
    size_t len = subj.len();
    subj.s( sub );
    this->sub[ val - min ] = _U64( subj.hash(), subj.len() );
    subj.set_len( len );
  }
  bool is_full( void ) {
    for ( uint8_t i = 0; i < max - min; i++ )
      if ( this->sub[ i ] == 0 )
        return false;
    return true;
  }
  PublishType lookup( uint32_t h,  uint16_t l ) const {
    uint64_t val = _U64( h, l );
    for ( uint8_t i = 0; i < max - min; i++ )
      if ( this->sub[ i ] == val )
        return PublishType ( i + min );
    return min;
  }
};

typedef HashPub< U_INBOX, U_INBOX_AUTH, InboxBuf > InboxHash;
typedef HashPub< U_MCAST, U_MCAST_PING, McastBuf > McastHash;

/* subscribed message recvd, either mcast or point to point */
struct SubMsgData {
  MsgFramePublish & pub;
  UserBridge      * src_bridge; /* which peer it's from */
  uint64_t          seqno,   /* the seqno of the published message */
                    stamp,   /* the optional time of message at the publisher */
                    token,     /* rpc token */
                    ref_seqno;
  const void      * data;    /* message data */
  size_t            datalen; /* message data length */
  uint32_t          fmt,     /* format of data */
                    reply;   /* non-zero when peer wants a ptp reply */
  /* start_seqno : seqno of subscripton */
  /* sub2, sublen2 : subject of inbox */

  SubMsgData( MsgFramePublish &p,  UserBridge *n,  const void *d,
              size_t dl )
    : pub( p ), src_bridge( n ), seqno( 0 ), stamp( 0 ), token( 0 ),
      ref_seqno( 0 ), data( d ), datalen( dl ), fmt( 0 ), reply( 0 ) {}
};
/* a publish sent to all subscribers */
struct PubMcastData {
  const char * sub;       /* subject to publish */
  uint16_t     sublen,    /* subject length */
               option;    /* message options for the opt field */
  uint64_t     seqno,     /* seqno filled in by the publish */
               stamp,     /* optional time of publish */
               token;     /* token rpc val */
  const void * data;      /* data to publish */
  size_t       datalen;   /* data length */
  uint32_t     fmt,       /* format of data */
               reply;     /* if rpc style point to point reply wanted */

  PubMcastData( const char *s,  size_t sl,  const void *d,  size_t dl,
                uint32_t f,  uint32_t rep = 0 )
    : sub( s ), sublen( (uint16_t) sl ), option( 0 ), seqno( 0 ), stamp( 0 ),
      token( 0 ), data( d ), datalen( dl ), fmt( f ), reply( rep ) {}
  PubMcastData( const PubMcastData &mc ) :
    sub( mc.sub ), sublen( mc.sublen ), option( mc.option ), seqno( mc.seqno ),
   stamp( mc.stamp ), token( mc.token ), data( mc.data ), datalen( mc.datalen ),
    fmt( mc.fmt ), reply( mc.reply ) {}
};
/* a publish sent point to point to an inbox */
struct PubPtpData : public PubMcastData {
  UserBridge & peer;
  uint32_t     reply2;

  PubPtpData( UserBridge &p,  uint32_t rep,  const void *d,  size_t dl,
              uint32_t f )
    : PubMcastData( NULL, 0, d, dl, f, rep ), peer( p ), reply2( 0 ) {}
  PubPtpData( UserBridge &p,  const PubMcastData &mc )
    : PubMcastData( mc ), peer( p ), reply2( mc.reply ) {
    this->reply = 0;
  }
  /* same as mcast without a subject, message is sent to _INBOX.Nonce.<rep> */
};
struct EvTcpTransport;
struct StringTab;
struct TelnetListen;
struct WebListen;
struct NameSvc;
struct SessionMgr;

struct IpcRoute : public kv::EvSocket, public kv::BPData {
  SessionMgr & mgr;
  UserDB     & user_db;
  SubDB      & sub_db;
  IpcRoute( kv::EvPoll &p,  SessionMgr &m ) noexcept;
  /* EvSocket */
  virtual bool on_msg( kv::EvPublish &pub ) noexcept;
  bool on_inbox( const MsgFramePublish &pub,  UserBridge &n,
                 MsgHdrDecoder &dec ) noexcept;
  virtual void write( void ) noexcept;
  virtual void read( void ) noexcept;
  virtual void process( void ) noexcept;
  virtual void release( void ) noexcept;
  virtual void on_write_ready( void ) noexcept;
  bool check_flow_control( bool b ) {
    if ( ! b && this->bp_in_list() )
      this->push( kv::EV_WRITE_POLL );
    return b;
  }
};

struct ConsoleRoute : public kv::EvSocket {
  SessionMgr & mgr;
  UserDB     & user_db;
  SubDB      & sub_db;
  ConsoleRoute( kv::EvPoll &p,  SessionMgr &m ) noexcept;
  /* EvSocket */
  virtual bool on_msg( kv::EvPublish &pub ) noexcept;
  uint32_t fwd_console( kv::EvPublish &pub,  bool is_caba ) noexcept;
  virtual void write( void ) noexcept;
  virtual void read( void ) noexcept;
  virtual void process( void ) noexcept;
  virtual void release( void ) noexcept;
};

struct Unrouteable {
  TelnetListen          * telnet;
  WebListen             * web;
  NameSvc               * name;
  ConfigTree::Transport * tport;
  uint32_t                un_id;
  bool is_active( void ) const;
};

struct UnrouteableList : public kv::ArrayCount<Unrouteable, 4> {
  Unrouteable & upsert( ConfigTree::Transport * tport ) {
    for ( size_t i = 0; i < this->count; i++ ) {
      if ( tport == this->ptr[ i ].tport )
        return this->ptr[ i ];
    }
    Unrouteable & x = this->push();
    x.tport = tport;
    x.un_id = this->count;
    return x;
  }
  Unrouteable *find( ConfigTree::Transport * tport ) const {
    for ( size_t i = 0; i < this->count; i++ ) {
      if ( tport == this->ptr[ i ].tport )
        return &this->ptr[ i ];
    }
    return NULL;
  }
  bool is_active( ConfigTree::Transport * tport ) const {
    Unrouteable *un = this->find( tport );
    return ( un != NULL && un->is_active() );
  }
};

struct SessionMgr : public kv::EvSocket, public kv::BPData {
  IpcRoute              ipc_rt;         /* network -> rv sub, ds sub, etc */
  ConsoleRoute          console_rt;     /* rv pub, ds pub -> console sub */
  ConfigTree          & tree;           /* config db */
  ConfigTree::User    & user;           /* my user */
  ConfigTree::Service & svc;            /* this transport */
  UScoreTab             u_tab;          /* table of _subscriptions */
  uint64_t              next_timer,     /* session start gets a timer_id */
                        timer_id,       /* timer_id for this session */
                        timer_mono_time,/* mono updated at timer expire */
                        timer_time,     /* real updated at timer expire */
                        timer_converge_time, /* when publishers convege */
                        converge_seqno, /* zero seqno of converge */
                        timer_start_mono,/* start mono */
                        timer_start,     /* start time */
                        timer_ival;     /* interval for timer */
  InboxHash             ibx;            /* match inbox hashes */
  McastHash             mch;            /* match mcast hashes */
  UserDB                user_db;        /* db of user nonce routes */
  SubDB                 sub_db;         /* track subscriptions */
  kv::BitSpace          router_set;
  kv::BloomRef          sys_bloom;
  EventRecord           events;
  Console               console;
  kv::Logger          & log;
  SessionStats          stats;
  UnrouteableList       unrouteable;
  ConnectMgr            connect_mgr;
  uint64_t              pub_window_mono_time, /* when pub window expires */
                        sub_window_mono_time, /* when sub window expires */
                        name_svc_mono_time;
  size_t                pub_window_size, /* maximum size of pub window */
                        sub_window_size; /* maximum size of sub window */
  uint64_t              pub_window_ival, /* pub interval of rotate */
                        sub_window_ival; /* sub interval of rotate */
  uint8_t               tcp_accept_sock_type, /* free list sock types */
                        tcp_connect_sock_type;
  int                   tcp_timeout;
  bool                  tcp_noencrypt,
                        tcp_ipv4,
                        tcp_ipv6,
                        session_started;
  uint32_t              msg_counter[ 64 ],
                        idle_busy;

  SessionMgr( kv::EvPoll &p,  kv::Logger &l,  ConfigTree &c,
              ConfigTree::User &u,  ConfigTree::Service &s,
              StringTab &st ) noexcept;
  int init_sock( void ) noexcept;
  bool init_param( void ) noexcept;
  bool add_transport( ConfigTree::Transport &t,  bool is_listener ) noexcept;
  bool add_transport2( ConfigTree::Transport &t,  bool is_listener,
                       TransportRoute *&rte ) noexcept;
  bool add_ipc_transport( void ) noexcept;
  bool add_network( const char *net,  size_t net_len, 
                    const char *svc,  size_t svc_len ) noexcept;
  bool start_transport( TransportRoute &rte,  bool is_listener ) noexcept;
  bool add_startup_transports( void ) noexcept;
  bool add_startup_transports( const char *name,  size_t name_sz,
                               bool is_listen ) noexcept;
  bool add_rvd_transports( const char *listen,  const char *http,
                           int flags ) noexcept;
  uint32_t shutdown_transport( ConfigTree::Transport &t ) noexcept;
  bool add_mesh_accept( TransportRoute &listen_rte,
                        EvTcpTransport &conn ) noexcept;
  TransportRoute * add_tcp_rte( TransportRoute &src_rte,
                                uint32_t conn_hash ) noexcept;
  bool add_tcp_accept( TransportRoute &listen_rte,
                       EvTcpTransport &conn ) noexcept;
  bool add_mesh_connect( TransportRoute &mesh_rte ) noexcept;
  TransportRoute * find_mesh( const StringVal &mesh_url ) noexcept;
  TransportRoute * find_ucast( const StringVal &ucast_url ) noexcept;
  TransportRoute * find_mesh( TransportRoute &mesh_rte,
                              struct addrinfo *addr_list ) noexcept;
  bool add_mesh_connect( TransportRoute &mesh_rte,  const char **mesh_url,
                         uint32_t *mesh_hash,  uint32_t url_count ) noexcept;
  bool add_mesh_connect( TransportRoute &mesh_rte,
                         const char *url,  uint32_t url_hash ) noexcept;
  int init_session( const CryptPass &pwd ) noexcept;
  void add_rte( const char *sub, size_t sub_len, uint32_t hash,
                PublishType type ) noexcept;
  uint32_t add_wildcard_rte( const char *prefix, size_t pref_len,
                             PublishType type ) noexcept;
  void fork_daemon( int err_fd ) noexcept;
  bool loop( uint32_t &idle ) noexcept;
  void start( void ) noexcept;
  void name_hb( uint64_t cur_mono ) noexcept;
  void stop( void ) noexcept;
  bool is_running( void ) {
    return this->timer_id != 0;
  }
  bool check_flow_control( bool b ) {
    if ( ! b && this->bp_in_list() )
      this->push( kv::EV_WRITE_POLL );
    return b;
  }
  /* EvSocket */
  virtual bool on_msg( kv::EvPublish &pub ) noexcept;
  virtual bool timer_expire( uint64_t tid,  uint64_t eid ) noexcept;
  virtual void write( void ) noexcept;
  virtual void read( void ) noexcept;
  virtual void process( void ) noexcept;
  virtual void release( void ) noexcept;
  virtual void on_write_ready( void ) noexcept;
  MsgFrameStatus parse_msg_hdr( MsgFramePublish &fpub,  bool is_ipc ) noexcept;
  void ignore_msg( const MsgFramePublish &fpub ) noexcept;
  void show_debug_msg( const MsgFramePublish &fpub,
                       const char *where ) noexcept;
  /* publish data on a subject */
  void publish_stats( uint64_t cur_time ) noexcept;
  void fwd_port_stat_msg( SubjectVar &s,  TransportRoute *rte,  PortStats &rate,
                          PortStats &total,  uint64_t cur_time, uint32_t fd_cnt,
                          uint32_t uid_cnt,  uint32_t &rcount,
                          uint32_t &ipc_count ) noexcept;
  void fwd_stat_msg( SubjectVar &s,  MsgCat &m,  uint32_t h,
                     uint32_t &rcount,  uint32_t &ipc_count ) noexcept;
  bool publish( PubMcastData &mc ) noexcept;
  bool publish_any( PubMcastData &mc ) noexcept;
  bool publish_to( PubPtpData &ptp ) noexcept;
  void send_ack( const MsgFramePublish &pub,  UserBridge &,
                 const MsgHdrDecoder &dec,  const char *suf ) noexcept;
  bool forward_inbox( TransportRoute &src_rte,  kv::EvPublish &pub ) noexcept;
  bool forward_ipc( TransportRoute &src_rte, kv::EvPublish &mc ) noexcept;
  bool listen_start_noencrypt( ConfigTree::Transport &tport,
                               kv::EvTcpListen *l, const char *k ) noexcept;
  bool create_telnet( ConfigTree::Transport &tport ) noexcept;
  bool create_web( ConfigTree::Transport &tport ) noexcept;
  bool create_name( ConfigTree::Transport &tport ) noexcept;
  uint32_t start_name_services( ConfigTree::Transport &tport,
                                NameSvc **name_svc ) noexcept;
  uint32_t shutdown_telnet( ConfigTree::Transport &tport ) noexcept;
  uint32_t shutdown_web( ConfigTree::Transport &tport ) noexcept;
  uint32_t shutdown_name( ConfigTree::Transport &tport ) noexcept;
};

}
}
#endif
