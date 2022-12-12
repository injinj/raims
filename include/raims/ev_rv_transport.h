#ifndef __rai_raims__ev_rv_transport_h__
#define __rai_raims__ev_rv_transport_h__

#include <sassrv/ev_rv.h>
#include <raims/msg.h>
#include <raims/sub.h>
#include <raims/config_tree.h>
#include <raikv/dlinklist.h>

namespace rai {
namespace ms {

struct TransportRoute;

enum NetTransport {
  NET_NONE         = 0,
  NET_ANY          = 1,
  NET_MESH         = 2,
  NET_MESH_LISTEN  = 3,
  NET_MESH_CONNECT = 4,
  NET_TCP          = 5,
  NET_TCP_LISTEN   = 6,
  NET_TCP_CONNECT  = 7,
  NET_MCAST        = 8
};

struct RvMcast2 : public sassrv::RvMcast {
  NetTransport type;
  RvMcast2() : type( NET_NONE ) {}
  int parse_network2( const char *net,  size_t net_len ) noexcept;
  static NetTransport net_to_transport( const char *net,
                                        size_t &net_len ) noexcept;
};

struct RvHostRoute {
  RvHostRoute           * next,
                        * back;
  sassrv::RvHost        * host;       /* service/network pair */
  TransportRoute        * rte;        /* route for host */
  ConfigTree::Transport * cfg;
  uint64_t                last_active_mono;
  bool                    is_active,
                          tport_exists;

  void * operator new( size_t, void *ptr ) { return ptr; }
  void operator delete( void *ptr ) { ::free( ptr ); }

  RvHostRoute( sassrv::RvHost *h,  TransportRoute *r,
               ConfigTree::Transport *t )
    : next( 0 ), back( 0 ), host( h ), rte( r ), cfg( t ),
      last_active_mono( 0 ), is_active( false ), tport_exists( false ) {}
};

struct RvHostTab {
  kv::DLinkList<RvHostRoute> list;

  RvHostRoute *find( sassrv::RvHost *h ) {
    RvHostRoute *p = this->list.hd;
    if ( p == NULL || p->host == h )
      return p;
    for ( p = p->next; p != NULL; p = p->next ) {
      if ( p->host == h ) {
        this->list.pop( p );
        this->list.push_hd( p );
        return p;
      }
    }
    return NULL;
  }
  RvHostRoute *add( sassrv::RvHost *h,  TransportRoute *r,
                    ConfigTree::Transport *t ) {
    void * p = ::malloc( sizeof( RvHostRoute ) );
    this->list.push_hd( new ( p ) RvHostRoute( h, r, t ) );
    return this->list.hd;
  }
};

struct RvTransportService : public kv::EvTimerCallback {
  TransportRoute & rte;
  sassrv::RvHostDB db;
  RvHostTab        tab;
  uint64_t         last_active_mono;
  uint32_t         active_cnt,
                   start_cnt;
  bool             no_mcast,
                   no_permanent,
                   no_fakeip;

  void * operator new( size_t, void *ptr ) { return ptr; }
  RvTransportService( TransportRoute &r ) noexcept;

  void start( void ) noexcept;
  ConfigTree::Transport *get_rv_transport( sassrv::RvHost &host,
                                           bool create ) noexcept;
  void make_rv_transport( ConfigTree::Transport *&t,  sassrv::RvHost &host,
                          bool &is_listener ) noexcept;

  int start_host( sassrv::RvHost &host, const sassrv::RvHostNet &hn,
                  uint32_t &delay_secs ) noexcept;
  void stop_host( sassrv::RvHost &host ) noexcept;
  /* EvTimerCallback */
  virtual bool timer_cb( uint64_t, uint64_t ) noexcept;
};

struct EvRvTransportListen : public sassrv::EvRvListen {
  TransportRoute     & rte;
  RvTransportService & svc;
  EvRvTransportListen( kv::EvPoll &p,  TransportRoute &r,
                       RvTransportService &s ) noexcept;
  /* sassrv rv listen */
  virtual EvSocket *accept( void ) noexcept;
  virtual int listen( const char *ip,  int port,  int opts ) noexcept;
  virtual int start_host( sassrv::RvHost &host,
                          const sassrv::RvHostNet &hn ) noexcept;
  virtual int stop_host( sassrv::RvHost &host ) noexcept;
};

}
}

#endif
