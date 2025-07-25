#ifndef __rai_raims__msg_h__
#define __rai_raims__msg_h__

#include <raikv/ev_publish.h>
#include <raimd/md_msg.h>
#include <raimd/tib_sass_msg.h>
#include <raimd/hex_dump.h>
#include <raims/crypt.h>
#include <raims/ed25519.h>

namespace rai {
namespace ms {

extern int64_t tz_offset_sec,
               tz_offset_ns,
               tz_stamp_sec,
               tz_stamp_ns;
extern bool    tz_stamp_gmt;
void update_tz_stamp( void );

/*
bytes 0 -> 3 are ver(1), type(2), opt(5), path(8), message size (16)
 1               8               16              24              32   
|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|
|1|0 0|0 0 0 0 0|0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 1 0 0 0 0 0|
 ^ ^.^ ^.......^ ^.............^ ^.............................^
 |    \    |            |                  |     
ver(1)|   opt(0)     path(0)         16 bit size(160)
     type(0)
bytes 4 -> 7 are the routing key hash
 1               8               16              24              32   
+-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|
|0 0 0 0 0 0 0 1 1 0 0 1 0 0 0 1 1 1 0 0 0 0 1 0 0 0 0 0 0 1 1 0|
 ^.............................................................^
                                   |
                               hash(0x191c206) = crc_c("CABA")
bytes 8 -> 25 are the bridge source 
fid = BRIDGE(3), type = OPAQUE_16(4) ( opaque 16 bytes )            144
|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+.. +
|1 1 x x 0 1 0 0 0 0 0 0 0 0 1 1|                                     |
 ^ ^     ^.....^ ^.............^ ^....................................
 | |         |        |                        |
 | primitive type(4)  fid(3)               128 bit bridge
 fixed

bytes 26 -> 43 are the hmac digest
bytes 44 -> X are the subject of the routing key
fid = SUB(0), type = SHORT_STRING(7) ( int16 string data )
|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|...      ....+
|0 1 x x 0 1 1 1 0 0 0 0 0 0 0 0|0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0|  CABA       |
   ^     ^.....^ ^.............^ ^.............................^ ^......
   |         |         |                       |                    |
   primitive type(7)   fid(0)             16 bit length(4)       string data  |

more fields, a seqno is always included
fid = SEQNO(17), type = U_SHORT(2) ( uint16 )                  32
|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|
|1 1 x x 0 0 1 0 0 0 0 0 1 0 0 1|0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1|
 ^ ^     ^.....^ ^.............^ ^.............................^
 | |         |         |                      |
 | primitive type(2)   fid(17)            16 bit seqno(1)
 fixed

the last bytes are the data payload
fid = DATA(1), type = LONG_OPAQUE(8) ( int32 opaque data )
|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+-|-+-+-+-+-+-+-+- ...     ...  +
|0 1 x x 1 0 0 0 0 0 0 0 0 0 0 1|                                             |
   ^     ^.....^ ^.............^ ^..............................
   |         |         |                      |
   primitive type(8)   fid(1)             message payload

*/
enum CabaTypeFlag {
  CABA_MCAST     = 0, /* multicast msg */
  CABA_INBOX     = 1, /* inbox msg */
  CABA_RTR_ALERT = 2, /* router alert (_M, _P, _S, _Z) */
  CABA_HEARTBEAT = 3, /* heartbeat (_X) */
};
enum CabaOptFlag {
  CABA_OPT_NONE  = 0,
  CABA_OPT_ACK   = 1<<0,  /* recver ack messages */
  CABA_OPT_TRACE = 1<<1,  /* routers trace messages */
  CABA_OPT_ANY   = 1<<2,  /* any of many */
  CABA_OPT_GLSNO = 1<<3,  /* global seqno */
  CABA_OPT_ZPATH = 1<<4   /* inbox path_select=0 seqno */
};
                      /* <ver:1><type:2><opt:5><path:8><length bits:16> */
static const int      CABA_VER_BITS    = 1,
                      CABA_TYPE_BITS   = 2,
                      CABA_OPT_BITS    = 5,
                      CABA_PATH_BITS   = 8,
                      CABA_LENGTH_BITS = 16,
                      CABA_VER_SHIFT   = 16 - CABA_VER_BITS,
                      CABA_TYPE_SHIFT  = CABA_VER_SHIFT  - CABA_TYPE_BITS,
                      CABA_OPT_SHIFT   = CABA_TYPE_SHIFT - CABA_OPT_BITS,
                      CABA_PATH_SHIFT  = CABA_OPT_SHIFT  - CABA_PATH_BITS;
static const uint16_t CABA_VER_MASK    = ( (uint16_t) 1 << CABA_VER_BITS )  - 1,
                      CABA_TYPE_MASK   = ( (uint16_t) 1 << CABA_TYPE_BITS ) - 1,
                      CABA_OPT_MASK    = ( (uint16_t) 1 << CABA_OPT_BITS )  - 1,
                      CABA_PATH_MASK   = ( (uint16_t) 1 << CABA_PATH_BITS ) - 1,
                      CABA_PATH_COUNT  = CABA_PATH_MASK + 1;
static const uint32_t CABA_LENGTH_MASK = ( (uint32_t) 1 << CABA_LENGTH_BITS ) - 1;

static inline const char *caba_type_flag_str( CabaTypeFlag fl ) {
  if ( fl == CABA_INBOX ) return "inbox";
  if ( fl == CABA_RTR_ALERT ) return "rtr_alert";
  if ( fl == CABA_HEARTBEAT ) return "heartbeat";
  return "mcast";
}
/* mask out _M. _P. _S. _Z. subjects */
static inline bool caba_rtr_alert( const char *sub ) {
  static const uint32_t mask =
    ( 1U << ( 'M' - 'M' ) ) | ( 1U << ( 'P' - 'M' ) ) |
    ( 1U << ( 'S' - 'M' ) ) | ( 1U << ( 'Z' - 'M' ) );
  if ( sub[ 0 ] != '_' || sub[ 1 ] < 'M' || sub[ 1 ] > 'Z' || sub[ 2 ] != '.' )
    return false;
  return ( mask & ( 1U << ( sub[ 1 ] - 'M' ) ) ) != 0;
}

static const uint16_t CABA_MSG_VERSION = 1;

struct CabaFlags {
  uint16_t flags;
  static uint16_t lsh( uint16_t v, int sh, uint16_t m ) { return ( v & m ) << sh; }
  static uint16_t rsh( uint16_t v, int sh, uint16_t m ) { return ( v >> sh ) & m; }

  CabaFlags( CabaTypeFlag t )
    : flags( lsh( CABA_MSG_VERSION, CABA_VER_SHIFT,  CABA_VER_MASK ) |
             lsh( t,                CABA_TYPE_SHIFT, CABA_TYPE_MASK ) ) {}

  uint16_t get_ver( void ) const      { return rsh( this->flags, CABA_VER_SHIFT , CABA_VER_MASK  ); }
  CabaTypeFlag get_type( void ) const { return (CabaTypeFlag)
                                               rsh( this->flags, CABA_TYPE_SHIFT, CABA_TYPE_MASK ); }
  uint16_t get_opt( void ) const      { return rsh( this->flags, CABA_OPT_SHIFT , CABA_OPT_MASK  ); }
  uint8_t  get_path( void ) const     { return this->get_path( this->get_opt() ); }
  uint8_t  get_path( uint16_t opt ) const { return ( opt & CABA_OPT_ZPATH ) != 0 ? 0 :
                                               rsh( this->flags, CABA_PATH_SHIFT, CABA_PATH_MASK ); }
  uint8_t  get_ztype( uint16_t opt ) const { return ( opt & CABA_OPT_ZPATH ) == 0 ? 0 :
                                               rsh( this->flags, CABA_PATH_SHIFT, CABA_PATH_MASK ); }

  const char *type_str( void ) const  { return caba_type_flag_str( this->get_type() ); }

  static void set( uint16_t &fl,  uint16_t v,  int sh,  uint16_t m ) {
    fl = lsh( v, sh, m ) | ( fl & ~lsh( m, sh, m ) );
  }
  void set_ver( uint16_t ver )        { set( this->flags, ver, CABA_VER_SHIFT , CABA_VER_MASK  ); }
  void set_type( CabaTypeFlag t )     { set( this->flags, t  , CABA_TYPE_SHIFT, CABA_TYPE_MASK ); }
  void set_opt( uint16_t opt )        { set( this->flags, opt, CABA_OPT_SHIFT , CABA_OPT_MASK  ); }
  void set_path( uint16_t p )         { set( this->flags, p  , CABA_PATH_SHIFT, CABA_PATH_MASK ); }
};

struct CabaMsg : public md::TibSassMsg {
  const char * sub;    /* pointer into the message after sub fid and length */
  uint32_t     subhash;/* second word of the message, unless length is large */
  uint16_t     sublen; /* length attached to the sub fid */
  CabaFlags    caba;   /* first bits of the message */

  void * operator new( size_t, void *ptr ) { return ptr; }
  CabaMsg( void *bb,  size_t off,  size_t end,  md::MDDict *d, md::MDMsgMem &m )
    : md::TibSassMsg( bb, off, end, d, m ),
      sub( 0 ), subhash( 0 ), sublen( 0 ), caba( CABA_MCAST ) {}

  virtual const char *get_proto_string( void ) noexcept final;
  virtual uint32_t get_type_id( void ) noexcept final;
  /* may return tibmsg, sass qform or rv */
  static bool is_cabamsg( void *bb,  size_t off,  size_t end,
                          uint32_t h ) noexcept;
  static CabaMsg *unpack( void *bb,  size_t off,  size_t end,  uint32_t h,
                          md::MDDict *d,  md::MDMsgMem &m ) noexcept;
  static int unpack2( uint8_t *bb,  size_t off,  size_t &end,  md::MDMsgMem &m,
                      CabaMsg *&msg ) noexcept;
  CabaMsg *submsg( void *bb,  size_t len ) noexcept;
  static void init_auto_unpack( void ) noexcept;
  bool verify( const HashDigest &key ) const noexcept;
  bool verify_hb( const HashDigest &key ) const noexcept;
  bool verify_sig( const HashDigest &key,  DSA &dsa ) const noexcept;
  uint32_t caba_to_rvmsg( md::MDMsgMem &mem,  void *&data,
                          size_t &datalen ) noexcept;
  void print_hex( void ) const { md::MDOutput mout; mout.print_hex( this ); }
};

struct MsgFrameDecoder {
  static md::MDDict * msg_dict;      /* fid index for type and size info */
  md::MDMsgMem mem;      /* memory for message unpacking and subject */
  CabaMsg    * msg;      /* msg data, has the following fields */

  MsgFrameDecoder();

  void init( void ) {
    this->msg = NULL;
  }
  void release( void ) {
    this->init();
    this->mem.reuse();
  }
  int unpack( const void *msgbuf,  size_t &msglen ) {
    this->release();
    return CabaMsg::unpack2( (uint8_t *) msgbuf, 0, msglen, this->mem,
                             this->msg );
  }
  void print( void ) noexcept;
  static md::MDDict *build_msg_dict( void ) noexcept;
};

/* msg fields */
enum MsgFid {
  FID_SUB            =  0 , /* publish subject or inbox */
  FID_DATA           =  1 , /* opaque data */

  FID_SESSION        =  2 , /* Hmac.Nonce field */
  FID_BRIDGE         =  3 , /* Bridge id nonce */
  FID_USER_HMAC      =  4 , /* User hmac */
  FID_DIGEST         =  5 , /* message digest, signed by auth key */
  FID_AUTH_KEY       =  6 , /* authentication key response to challenge */
  FID_SESS_KEY       =  7 , /* session key = hmac + bridge */
  FID_PEER_DB        =  8 , /* sync peer db */
  FID_MESH_DB        =  9 , /* mesh urls */
  FID_CNONCE         = 10 , /* challenge nonce */
  FID_SYNC_BRIDGE    = 11 , /* request target of sync request */
  FID_UID_CSUM       = 12 , /* bridge checksum of all nodes */
  FID_MESH_CSUM      = 13 , /* route checksum of all mesh nodes */
  FID_MESH_FILTER    = 14 , /* filter mesh db requests */
  FID_ADJACENCY      = 15 , /* adjacency links map */
  FID_BLOOM          = 16 , /* bloom sub map */

  FID_SEQNO          = 17 , /* integer fields */
  FID_SUB_SEQNO      = 18 , /* subscription seqno */
  FID_TIME           = 19 , /* time of message */
  FID_UPTIME         = 20 , /* how long node is up */
  FID_INTERVAL       = 21 , /* heartbeat interval */
  FID_REF_CNT        = 22 , /* count of sub refs */
  FID_TOKEN          = 23 , /* token passed through by rpc */
  FID_RET            = 24 , /* return inbox number */
  FID_LINK_STATE     = 25 , /* link state seqno */
  FID_START          = 26 , /* start seqno of subs request */
  FID_END            = 27 , /* end seqno of subs request */
  FID_ADJ_INFO       = 28 , /* why peer requested adjacency */
  FID_AUTH_SEQNO     = 29 , /* seqno of message used by auth */
  FID_AUTH_TIME      = 30 , /* time of message used by auth */
  FID_FMT            = 31 , /* msg format of data, wildcard format */
  FID_HOPS           = 32 , /* whether directly attached to same tport */
  FID_REF_SEQNO      = 33 , /* ack or trace reference seqno */
  FID_TPORTID        = 34 , /* which transport adjacency belongs to */
  FID_UID            = 35 , /* uid reference */
  FID_UID_CNT        = 36 , /* how many peers, sent with hello */
  FID_SUBJ_HASH      = 37 , /* hash of subject */

  FID_SUBJECT        = 38 , /* subject of subscription */
  FID_PATTERN        = 39 , /* pattern subject wildcard */
  FID_REPLY          = 40 , /* publish reply */
  FID_UCAST_URL      = 41 , /* unicast route for inbox data */
  FID_MESH_URL       = 42 , /* mesh route for interconnecting uids */
  FID_TPORT          = 43 , /* tport name */

  FID_USER           = 44 , /* name of user */
  FID_SERVICE        = 45 , /* service of user to add */
  FID_CREATE         = 46 , /* create time of user to add */
  FID_EXPIRES        = 47 , /* expire time of user to add */
  FID_VERSION        = 48 , /* build version */

  FID_AUTH_STAGE     = 49 , /* what stage of authentication */
  FID_LINK_ADD       = 50 , /* whether to add or delete link in adjacency */

  FID_FD_CNT         = 51 , /* tport stats */
  FID_MS_TOT         = 52 ,
  FID_MR_TOT         = 53 ,
  FID_BS_TOT         = 54 ,
  FID_BR_TOT         = 55 ,
  FID_MS             = 56 ,
  FID_MR             = 57 ,
  FID_BS             = 58 ,
  FID_BR             = 59 ,
  FID_SUB_CNT        = 60 ,
  FID_COST           = 61 , /* cost value */
  FID_ADJ_COST       = 62 , /* cost of link */
  FID_PEER           = 63 ,
  FID_LATENCY        = 64 ,

  FID_PK_DIGEST      = 65 , /* public key digest, in hb before auth */
  FID_TPORT_TYPE     = 66 , /* tport type in adjacency msg */
  FID_CHAIN_SEQNO    = 67 , /* previous seqno when changing time frame */
  FID_STAMP          = 68 , /* time stamp */
  FID_CONVERGE       = 69 , /* network convergence stamp */
  FID_REPLY_STAMP    = 70 , /* reply time stamp */
  FID_HB_SKEW        = 71 , /* hb system clock skew */
  FID_PK_SIG         = 72 , /* pk key signature */
  FID_CONN_URL       = 73 , /* connect url spec */
  FID_CONN_PORT      = 74 , /* connect port */
  FID_PUBKEY         = 75 , /* ec pubkey */

  FID_UCAST_DB       = 76 , /* ucast urls */
  FID_UCAST_FILTER   = 77 , /* filter ucast db requests */
  FID_IDL_SERVICE    = 78 , /* inbound data loss service */
  FID_IDL_MSG_LOSS   = 79 , /* inbound data loss message loss */
  FID_REM_BRIDGE     = 80 , /* remote bridge directly connected to a link */
  FID_REM_TPORTID    = 81 , /* remote tport on the directly connected link */
  FID_MESH_INFO      = 82 , /* status of mesh in mesh reply */
  FID_LINK_STATE_SUM = 83 , /* sum of peers link_state seqno */
  FID_SUB_SEQNO_SUM  = 84 , /* sum of peers sub seqno */
  FID_HOST_ID        = 85 , /* host id */
  FID_QUEUE          = 86 , /* queue name */
  FID_QUEUE_HASH     = 87 , /* queue hash */
  FID_QUEUE_REFS     = 88 , /* how queue many subs */
  FID_HDR_LEN        = 89 , /* len of message hdr */
  FID_SUF_LEN        = 90 , /* len of message suffix  */
  FID_BLOOM_UPD      = 91 , /* peer bloom updated  */
  FID_IDL_RESTART    = 92 , /* if is idl restart or loss */
  FID_PATH_LIMIT     = 93   /* limit on path count */
};
static const int FID_TYPE_SHIFT = 8,
                 FID_MAX        = 1 << FID_TYPE_SHIFT; /* 256 */

inline uint16_t fid_value( uint16_t fid ) {
  return fid & ( FID_MAX - 1 );
}
/* class ids used:  fid = ( FldTypeClass << FID_TYPE_SHIFT ) | FID_xxx */
enum FldTypeClass {
  BOOL_CLASS         = 0, /* bool 1 byte */
  U_SHORT_CLASS      = 1, /* uint8_t */
  U_INT_CLASS        = 2, /* uint16_t */
  U_LONG_CLASS       = 3, /* uint64_t */
  OPAQUE_16_CLASS    = 4, /* 16 byte opaque */
  OPAQUE_32_CLASS    = 5, /* 32 byte opaque */
  OPAQUE_64_CLASS    = 6, /* 64 byte opaque */
  SHORT_STRING_CLASS = 7, /* veriable, 0 -> 64K */
  LONG_OPAQUE_CLASS  = 8  /* veriable, 0 -> 4G */
};
#ifdef INCLUDE_MSG_CONST
static inline MDType cls_to_md( FldTypeClass cl ) {
  #define X( x, y ) ( (uint64_t) x << ( y * 4 ) )
  static const uint64_t bits =
  X( MD_BOOLEAN, BOOL_CLASS      ) | X( MD_UINT, U_SHORT_CLASS     ) |
  X( MD_UINT,    U_INT_CLASS     ) | X( MD_UINT, U_LONG_CLASS      ) |
  X( MD_OPAQUE,  OPAQUE_16_CLASS ) | X( MD_OPAQUE, OPAQUE_32_CLASS ) |
  X( MD_OPAQUE,  OPAQUE_64_CLASS ) |
  X( MD_STRING,  SHORT_STRING_CLASS ) |
  X( MD_OPAQUE,  LONG_OPAQUE_CLASS );
  #undef X
  return (MDType) ( ( bits >> ( (int) cl * 4 ) ) & 0xf );
};
static inline FldTypeClass fid_type( uint16_t fid ) {
  return (FldTypeClass) ( ( fid >> FID_TYPE_SHIFT ) & 0xf );
}
static inline uint32_t fid_size( FldTypeClass cl ) {
  if ( cl <= OPAQUE_64_CLASS )
    return 1U << cl;
  return 0;
}
#endif

/* type masks that a field can be */
enum FldTypeBit {
  BOOL_1       = 1 << BOOL_CLASS,         /* bool 1 byte */
  U_SHORT      = 1 << U_SHORT_CLASS,      /* uint16_t */
  U_INT        = 1 << U_INT_CLASS,        /* uint32_t */
  U_LONG       = 1 << U_LONG_CLASS,       /* uint64_t */
  OPAQUE_16    = 1 << OPAQUE_16_CLASS,    /* 16 byte opaque */
  OPAQUE_32    = 1 << OPAQUE_32_CLASS,    /* 32 byte opaque */
  OPAQUE_64    = 1 << OPAQUE_64_CLASS,    /* 64 byte opaque */
  SHORT_STRING = 1 << SHORT_STRING_CLASS, /* veriable, 0 -> 64K */
  LONG_OPAQUE  = 1 << LONG_OPAQUE_CLASS   /* veriable, 0 -> 4G */
};

enum PublishType {
  U_NORMAL          = 0,  /* other _ABC */
  U_SESSION_HELLO   = 1,  /* _X.HELO    */
  U_SESSION_HB      = 2,  /* _X.HB      */
  U_SESSION_BYE     = 3,  /* _X.BYE     */
  U_SESSION_LINK    = 4,  /* _X.LINK    */
  U_PEER_ADD        = 5,  /* _Z.ADD     */
  U_PEER_DEL        = 6,  /* _Z.DEL     */
  U_BLOOM_FILTER    = 7,  /* _Z.BLM     */
  U_ADJACENCY       = 8,  /* _Z.ADJ     */
  U_SUB_JOIN        = 9,  /* _S.JOIN.>  */
  U_SUB_LEAVE       = 10, /* _S.LEAV.>  */
  U_PSUB_START      = 11, /* _P.PSUB.>  */
  U_PSUB_STOP       = 12, /* _P.PSTP.>  */
  /* unused bits, 13 -> 15 */
  U_INBOX_AUTH      = 16, /* _I.Nonce.auth (not a wildcard) */
  U_INBOX_SUBS      = 17, /* _I.Nonce.subs */
  U_INBOX_PING      = 18, /* _I.Nonce.ping */
  U_INBOX_PONG      = 19, /* _I.Nonce.pong */
  U_INBOX_REM       = 20, /* _I.Nonce.rem */
  U_INBOX_RESUB     = 21, /* _I.Nonce.resub */
  U_INBOX_REPSUB    = 22, /* _I.Nonce.repsub */
  U_INBOX_ADD_RTE   = 23, /* _I.Nonce.add_rte */
  U_INBOX_SYNC_REQ  = 24, /* _I.Nonce.sync_req */
  U_INBOX_SYNC_RPY  = 25, /* _I.Nonce.sync_rpy */
  U_INBOX_BLOOM_REQ = 26, /* _I.Nonce.bloom_req */
  U_INBOX_BLOOM_RPY = 27, /* _I.Nonce.bloom_rpy */
  U_INBOX_ADJ_REQ   = 28, /* _I.Nonce.adj_req */
  U_INBOX_ADJ_RPY   = 29, /* _I.Nonce.adj_rpy */
  U_INBOX_MESH_REQ  = 30, /* _I.Nonce.mesh_req */
  U_INBOX_MESH_RPY  = 31, /* _I.Nonce.mesh_rpy */
  U_INBOX_UCAST_REQ = 32, /* _I.Nonce.ucast_req */
  U_INBOX_UCAST_RPY = 33, /* _I.Nonce.ucast_rpy */
  U_INBOX_TRACE     = 34, /* _I.Nonce.trace */
  U_INBOX_ACK       = 35, /* _I.Nonce.ack */
  U_INBOX_SYNC      = 36, /* _I.Nonce.sync */
  U_INBOX_LOSS      = 37, /* _I.Nonce.sync */
  U_INBOX_CONSOLE   = 38, /* _I.Nonce.X console rpc, X is integer */
  U_INBOX_ANY_RTE   = 39, /* _I.Nonce.any, ipc inbox */
  U_INBOX           = 40, /* _I.Nonce.any */

  U_MCAST_PING      = 41, /* _M.ping */
  U_MCAST_SYNC      = 42, /* _M.sync */
  U_MCAST_STAT_MON  = 43, /* _M.stat_mon */
  U_MCAST           = 44, /* _M.> */
  /* other subject */
  MCAST_SUBJECT     = 45, /* not _XX subject */
  UNKNOWN_SUBJECT   = 46, /* init, not resolved */
  MAX_PUB_TYPE      = 64
};
static inline bool is_u_inbox( PublishType t ) {
  return ( t >= U_INBOX_AUTH && t <= U_INBOX_CONSOLE ) || t == U_INBOX_ANY_RTE;
}
#ifdef INCLUDE_MSG_CONST
static const char *publish_type_str[] = {
  "u_normal",
  "u_session_hello",
  "u_session_hb",
  "u_session_bye",
  "u_session_link",
  "u_peer_add",
  "u_peer_del",
  "u_bloom_filter",
  "u_adjacency",
  "u_sub_join",
  "u_sub_leave",
  "u_psub_start",
  "u_psub_stop",
  "unused13", "unused14", "unused15",
  "u_inbox_auth",
  "u_inbox_subs",
  "u_inbox_ping",
  "u_inbox_pong",
  "u_inbox_rem",
  "u_inbox_resub",
  "u_inbox_repsub",
  "u_inbox_add_rte",
  "u_inbox_sync_req",
  "u_inbox_sync_rpy",
  "u_inbox_bloom_req",
  "u_inbox_bloom_rpy",
  "u_inbox_adj_req",
  "u_inbox_adj_rpy",
  "u_inbox_mesh_req",
  "u_inbox_mesh_rpy",
  "u_inbox_ucast_req",
  "u_inbox_ucast_rpy",
  "u_inbox_trace",
  "u_inbox_ack",
  "u_inbox_sync",
  "u_inbox_loss",
  "u_inbox_console",
  "u_inbox_any_rte",
  "u_inbox",

  "u_mcast_ping",
  "u_mcast_sync",
  "u_mcast_stat_mon",
  "u_mcast",

  "mcast_subject",
  "unknown_subject"
};
#if __cplusplus >= 201103L
static_assert( UNKNOWN_SUBJECT + 1 == ( sizeof( publish_type_str ) / sizeof( publish_type_str[ 0 ] ) ), "publish_type_str" );
#endif
#endif
const char *publish_type_to_string( PublishType t ) noexcept;

struct MsgFldSet {
  /* does not tolerate repeated fields, last one wins */
  uint64_t is_set[ FID_MAX / 64 ];     /* a bit for each FID indexed */

  MsgFldSet() { ::memset( this->is_set, 0, sizeof( this->is_set ) ); }

  static uint64_t idx( int opt ) { return opt / 64 ; }
  static uint64_t bit( int opt ) { return (uint64_t) 1 << ( opt % 64 ); }
  uint64_t &ref( int opt )       { return this->is_set[ idx( opt ) ]; }
  uint64_t con( int opt ) const  { return this->is_set[ idx( opt ) ]; }
  MsgFldSet &set( int opt )  { this->ref( opt ) |= bit( opt ); return *this; }
  bool test( int opt ) const { return ( this->con( opt ) & bit( opt ) ) != 0; }
  bool test_2( int opt,  int opt2 ) const {
    return this->test( opt ) & this->test( opt2 );
  }
  bool test_3( int opt,  int opt2,  int opt3 ) const {
    return this->test_2( opt, opt2 ) & this->test( opt3 );
  }
  bool test_4( int opt,  int opt2,  int opt3,  int opt4 ) const {
    return this->test_3( opt, opt2, opt3 ) & this->test( opt4 );
  }
  bool test_5( int opt, int opt2, int opt3, int opt4,int opt5 ) const {
    return this->test_4( opt, opt2, opt3, opt4 ) & this->test( opt5 );
  }
  bool test_6( int opt, int opt2, int opt3, int opt4,int opt5, int opt6 ) const{
    return this->test_5( opt, opt2, opt3, opt4, opt5 ) & this->test( opt6 );
  }
};

/* iterate through the fields and create references for them */
struct MsgHdrDecoder : public MsgFldSet {
  CabaMsg       * msg;
  uint64_t        seqno;
  uint32_t        inbox_ret;
  PublishType     type;
  md::MDReference mref[ FID_MAX ]; /* reference for the fields found */
  md::MDMsgMem    mem;

  MsgHdrDecoder( CabaMsg *m ) : msg( m ), seqno( 0 ), inbox_ret( 0 ),
                                type( UNKNOWN_SUBJECT ) {}
  bool is_ucast_type( void ) const {
    return this->type >= U_INBOX_AUTH && this->type <= U_INBOX;
  }
  bool is_mcast_type( void ) const {
    return ! this->is_ucast_type();
  }
  bool get_bridge( Nonce &bridge ) const noexcept;
  int decode_msg( void ) noexcept;
  const char *get_return( char *ret_buf,
                          const char *default_suf ) const noexcept;
  const char *get_type_string( void ) const {
    return publish_type_to_string( this->type );
  }
  bool get_nonce( MsgFid fid,  Nonce &nonce ) const {
    if ( this->test( fid ) ) {
      nonce.copy_from( this->mref[ fid ].fptr );
      return true;
    }
    nonce.zero();
    return false;
  }
  bool get_pubkey( MsgFid fid,  ec25519_key &pubkey ) const {
    if ( this->test( fid ) ) {
      pubkey.copy_from( this->mref[ fid ].fptr );
      return true;
    }
    pubkey.zero();
    return false;
  }
  bool get_hmac( MsgFid fid,  HmacDigest &hmac ) const {
    if ( this->test( fid ) ) {
      hmac.copy_from( this->mref[ fid ].fptr );
      return true;
    }
    hmac.zero();
    return false;
  }
  template <class T>
  bool get_ival( MsgFid fid,  T &ival ) const {
    ival = 0;
    if ( this->test( fid ) ) {
      md::cvt_number<T>( this->mref[ fid ], ival );
      return true;
    }
    return false;
  }
  template <class T>
  T * decode_rec_list( MsgFid fid ) {
    void       * data    = this->mref[ fid ].fptr;
    size_t       datalen = this->mref[ fid ].fsize;
    CabaMsg    * m       = this->msg->submsg( data, datalen );
    md::MDFieldIter * iter;
    md::MDReference   mref;
    md::MDName        nm;

    if ( m == NULL || m->get_field_iter( iter ) != 0 ) 
      return NULL;
    if ( iter->first() != 0 ||
         iter->get_name( nm ) != 0 ||
         iter->get_reference( mref ) != 0 ) 
      return NULL;
      
    T * hd  = new ( this->mem.make( sizeof( T ) ) ) T(),
      * rec = hd;
    uint32_t opt;
    do {
      opt = fid_value( nm.fid );
      if ( rec->test( opt ) ) {
        rec->next = new ( this->mem.make( sizeof( T ) ) ) T();
        rec = rec->next;
      }
      rec->set( opt );
      rec->set_field( opt, mref );
    } while ( iter->next() == 0 &&
              iter->get_name( nm ) == 0 &&
              iter->get_reference( mref ) == 0 );
    return hd;
  }
};

enum MsgFrameStatus {
  FRAME_STATUS_UNKNOWN    = 0,
  FRAME_STATUS_OK         = 1,
  FRAME_STATUS_DUP_SEQNO  = 2,
  FRAME_STATUS_NO_AUTH    = 3,
  FRAME_STATUS_NO_USER    = 4,
  FRAME_STATUS_BAD_MSG    = 5,
  FRAME_STATUS_MY_MSG     = 6,
  FRAME_STATUS_HB_NO_AUTH = 7,
  FRAME_STATUS_INBOX_AUTH = 8,
  FRAME_STATUS_MAX        = 9
};
#ifdef INCLUDE_FRAME_CONST
static const char *frame_status_str[] = {
  "unknown",
  "ok",
  "dup_seqno",
  "no_auth",
  "no_user",
  "bad_msg",
  "my_msg",
  "hb_no_auth",
  "inbox_auth"
};
static_assert( FRAME_STATUS_MAX == ( sizeof( frame_status_str ) / sizeof( frame_status_str[ 0 ] ) ), "frame_status" );
#endif

enum MsgFrameFlags {
  MSG_FRAME_ACK_CONTROL     = 1, /* if ack / trace was handled */
  MSG_FRAME_TPORT_CONTROL   = 2, /* if transport routing was handled */
  MSG_FRAME_IPC_CONTROL     = 4, /* if ipc routing was handled */
  MSG_FRAME_CONSOLE_CONTROL = 8, /* if console routing was handled */
  MSG_FRAME_IPC_ROUTE       = 16 /* if ipc route has dest */
};

struct TransportRoute;
struct UserBridge;
/* msg decoding is expensive, pass along the codec with the msg publish */
struct MsgFramePublish : public kv::EvPublish {
  UserBridge     * n;     /* bridge of sender */
  TransportRoute & rte;   /* where msg came from */
  MsgFrameStatus   status;/* what to do with msg */
  uint32_t         flags; /* MsgFrameFlags */
  MsgHdrDecoder    dec;   /* hdr field decoder */

  MsgFramePublish( const char *subj,  size_t subj_len,  CabaMsg *m,
                   const kv::PeerId &src,  uint32_t hash,  uint32_t enc,
                   TransportRoute &r,  kv::RoutePublish &sub_rt ) :
    EvPublish( subj, subj_len, NULL, 0,
               &((uint8_t *) m->msg_buf)[ m->msg_off ], m->msg_end - m->msg_off,
               sub_rt, src, hash, enc, kv::PUB_TYPE_ROUTING ),
    n( 0 ), rte( r ), status( FRAME_STATUS_UNKNOWN ), flags( 0 ), dec( m ) {}

  MsgFramePublish( kv::EvPublish &pub,  CabaMsg *m, TransportRoute &r ) :
      EvPublish( pub ), n( 0 ), rte( r ), status( FRAME_STATUS_OK ),
      flags( 0 ), dec( m ) {
    if ( m != NULL ) {
      this->msg     = &((uint8_t *) m->msg_buf)[ m->msg_off ];
      this->msg_len = m->msg_end - m->msg_off;
    }
    this->publish_type = kv::PUB_TYPE_ROUTING;
    this->hash         = pub.hash;
    this->prefix       = pub.prefix;
    this->prefix_cnt   = pub.prefix_cnt;
  }
  void print( const char *what ) const noexcept;
  const char *status_string( void ) const noexcept;
};

struct MsgFragPublish : public kv::EvPublish {
  const void * trail;
  size_t       trail_sz;

  MsgFragPublish( const char *subj,  size_t subj_len,  const void *msg,
                  size_t msg_len,  kv::RoutePublish &sub_rt,
                  const kv::PeerId &src, uint32_t hash,  uint32_t enc,
                  const void *tr,  size_t tsz ) :
    EvPublish( subj, subj_len, NULL, 0, msg, msg_len,
               sub_rt, src, hash, enc, kv::PUB_TYPE_FRAGMENT ),
    trail( tr ), trail_sz( tsz ) {}
};

struct IpcPublish : public kv::EvPublish {
  UserBridge & n;

  IpcPublish( UserBridge &src_b,  const char *subj,  size_t subj_len,
              const void *repl,  size_t repl_len,
              const void *mesg,  size_t mesg_len,
              kv::RoutePublish &sub_rt,  const kv::PeerId &src,  uint32_t shash,
              uint32_t msg_encoding,  uint16_t status,  uint32_t host,
              uint64_t counter )
    : EvPublish( subj, subj_len, repl, repl_len, mesg, mesg_len, sub_rt, src,
                 shash, msg_encoding, kv::PUB_TYPE_IPC, status, host, counter ),
      n( src_b ) {}
};

template <class T>
struct MsgBufT {
  char * out, * msg;
  MsgBufT( void *m ) : out( (char *) m ), msg( (char *) m ) {}
  T &nil( void ) { *this->out++ = '\0'; return (T &) *this; }
  T &s( const char *in ) { /* string out, does not copy null char */
    while ( *in != 0 ) { *this->out++ = *in++; }
    return (T &) *this;
  }
  T &b( const char *in, size_t in_len ) { /* buffer out */
    while ( in_len != 0 ) { *this->out++ = *in++; in_len -= 1; }
    return (T &) *this;
  }
  T &u( uint64_t n ) { /* uint out */
    this->out += kv::uint64_to_string( n, this->out );
    return (T &) *this;
  }
  T &i( uint32_t n ) { /* small uint out */
    this->out += kv::uint32_to_string( n, this->out );
    return (T &) *this;
  }
  T &n( const Nonce &val ) { /* digest out */
    this->out += val.to_base64( this->out );
    return (T &) *this;
  }
  T &h( const HmacDigest &val ) { /* digest out */
    this->out += val.to_base64( this->out );
    return (T &) *this;
  }
  T &k( const HashDigest &val ) { /* digest out */
    this->out += val.to_base64( this->out );
    return (T &) *this;
  }
  T &y( uint8_t n ) {
    if ( n ) return this->s( "true" );
    return this->s( "false" );
  }
  T &o( const void *in, size_t in_len ) { /* opaque out */
    this->out += kv::bin_to_base64( in, in_len, this->out, false );
    return (T &) *this;
  }
  size_t len( void ) const { return this->out - this->msg; }
  void set_len( size_t len ) { this->out = &this->msg[ len ]; }
  uint32_t hash( void ) const { return kv_crc_c( this->msg, this->len(), 0 ); }
};

#include <raims/caba_msg_buf.h>

struct MsgBuf : public MsgBufT<MsgBuf> {
  MsgBuf( void *m ) : MsgBufT( m ) {}
};
struct MsgCat : public md::MDMsgMem, public MsgBufDigestT<MsgCat> {
  MsgCat() : MsgBufDigestT( this->mem_ptr() ) {}
  void reserve( size_t len ) {
    this->reuse();
    this->out = this->msg = (char *) this->make( len );
  }
  void close( size_t rsz,  uint32_t h,  CabaFlags fl ) {
    this->close_frag( rsz, 0, h, fl );
  }
  void close_frag( size_t rsz,  uint32_t trail_sz,  uint32_t h, CabaFlags fl ) {
    this->MsgBufDigestT<MsgCat>::close_frag( h, trail_sz, fl );
    if ( rsz < this->len() )
      this->reserve_error( rsz );
  }
  void close_zpath( size_t rsz,  uint32_t h,  CabaFlags fl,  PublishType t ) {
    this->close_zpath_frag( rsz, 0, h, fl, t );
  }
  void close_zpath_frag( size_t rsz,  uint32_t trail_sz,  uint32_t h,  CabaFlags fl,  PublishType t ) {
    fl.set_opt( fl.get_opt() | CABA_OPT_ZPATH );
    fl.set_path( (uint8_t) t );
    this->close_frag( rsz, trail_sz, h, fl );
  }
  void print( void ) noexcept;
  void print_hex( void ) noexcept;
  void reserve_error( size_t rsz ) noexcept;
  uint32_t caba_to_rvmsg( MDMsgMem &mem,  void *&data,
                          size_t &datalen ) noexcept;
};
struct SubMsgBuf : public MsgBufDigestT<SubMsgBuf> {
  SubMsgBuf( MsgCat &msg ) : MsgBufDigestT( msg.out ) {}
  void close( MsgCat &msg,  uint8_t opt ) {
    this->close_submsg( opt );
    msg.out = this->out;
  }
};
/* define fields and dictionary */
#ifdef INCLUDE_MSG_CONST

struct FidTypeName {
  MsgFid       fid;
  uint16_t     type_mask;
  uint8_t      cvt_fld,
               name_len;
  const char * type_name;
};

enum { XCL, /* exclude */ LIT, /* literal */ BIN, /* base64 */ TIM  /* time */};

static FidTypeName fid_type_name[] = {
{ FID_SUB         , SHORT_STRING                , XCL , 0 ,"sub"             },
{ FID_DATA        , LONG_OPAQUE                 , XCL , 0 ,"data"            },

{ FID_SESSION     , OPAQUE_32                   , XCL , 0 ,"session"         },
{ FID_BRIDGE      , OPAQUE_16                   , XCL , 0 ,"bridge"          },
{ FID_USER_HMAC   , OPAQUE_16                   , XCL , 0 ,"user_hmac"       },
{ FID_DIGEST      , OPAQUE_16                   , XCL , 0 ,"digest"          },
{ FID_AUTH_KEY    , OPAQUE_64                   , XCL , 0 ,"auth_key"        },
{ FID_SESS_KEY    , OPAQUE_64                   , XCL , 0 ,"sess_key"        },
{ FID_PEER_DB     , LONG_OPAQUE                 , XCL , 0 ,"peer_db"         },
{ FID_MESH_DB     , LONG_OPAQUE                 , XCL , 0 ,"mesh_db"         },
{ FID_CNONCE      , OPAQUE_16                   , XCL , 0 ,"cnonce"          },
{ FID_SYNC_BRIDGE , OPAQUE_16                   , XCL , 0 ,"sync_bridge"     },
{ FID_UID_CSUM    , OPAQUE_16                   , XCL , 0 ,"uid_csum"        },
{ FID_MESH_CSUM   , OPAQUE_16                   , XCL , 0 ,"mesh_csum"       },
{ FID_MESH_FILTER , LONG_OPAQUE                 , XCL , 0 ,"mesh_filter"     },
{ FID_ADJACENCY   , LONG_OPAQUE                 , XCL , 0 ,"adjacency"       },
{ FID_BLOOM       , LONG_OPAQUE                 , XCL , 0 ,"bloom"           },

{ FID_SEQNO       , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"seqno"           },
{ FID_SUB_SEQNO   , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"sub_seqno"       },
{ FID_TIME        , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"time"            },
{ FID_UPTIME      , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"uptime"          },
{ FID_INTERVAL    , U_SHORT | U_INT             , XCL , 0 ,"interval"        },
{ FID_REF_CNT     , U_SHORT | U_INT             , LIT , 0 ,"ref_cnt"         },
{ FID_TOKEN       , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"token"           },
{ FID_RET         , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"ret"             },
{ FID_LINK_STATE  , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"link_state"      },
{ FID_START       , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"start"           },
{ FID_END         , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"end"             },
{ FID_ADJ_INFO    , U_SHORT | U_INT             , XCL , 0 ,"adj_info"        },
{ FID_AUTH_SEQNO  , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"auth_seqno"      },
{ FID_AUTH_TIME   , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"auth_time"       },
{ FID_FMT         , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"fmt"             },
{ FID_HOPS        , U_SHORT | U_INT             , XCL , 0 ,"hops"            },
{ FID_REF_SEQNO   , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"ref_seqno"       },
{ FID_TPORTID     , U_SHORT | U_INT             , LIT , 0 ,"tportid"         },
{ FID_UID         , U_SHORT | U_INT             , LIT , 0 ,"uid"             },
{ FID_UID_CNT     , U_SHORT | U_INT             , LIT , 0 ,"uid_cnt"         },
{ FID_SUBJ_HASH   , U_INT                       , XCL , 0 ,"subj_hash"       },

{ FID_SUBJECT     , SHORT_STRING                , XCL , 0 ,"subject"         },
{ FID_PATTERN     , SHORT_STRING                , XCL , 0 ,"pattern"         },
{ FID_REPLY       , SHORT_STRING                , XCL , 0 ,"reply"           },
{ FID_UCAST_URL   , SHORT_STRING                , XCL , 0 ,"ucast_url"       },
{ FID_MESH_URL    , SHORT_STRING                , XCL , 0 ,"mesh_url"        },
{ FID_TPORT       , SHORT_STRING                , LIT , 0 ,"tport"           },

{ FID_USER        , SHORT_STRING                , LIT , 0 ,"user"            },
{ FID_SERVICE     , SHORT_STRING                , XCL , 0 ,"service"         },
{ FID_CREATE      , SHORT_STRING                , XCL , 0 ,"create"          },
{ FID_EXPIRES     , SHORT_STRING                , XCL , 0 ,"expires"         },
{ FID_VERSION     , SHORT_STRING                , XCL , 0 ,"version"         },

{ FID_AUTH_STAGE  , U_SHORT                     , XCL , 0 ,"auth_stage"      },
{ FID_LINK_ADD    , BOOL_1                      , XCL , 0 ,"link_add"        },

{ FID_FD_CNT      , U_SHORT | U_INT             , LIT , 0 ,"fd_cnt"          },
{ FID_MS_TOT      , U_SHORT | U_INT | U_LONG    , LIT , 0 ,"ms_tot"          },
{ FID_MR_TOT      , U_SHORT | U_INT | U_LONG    , LIT , 0 ,"mr_tot"          },
{ FID_BS_TOT      , U_SHORT | U_INT | U_LONG    , LIT , 0 ,"bs_tot"          },
{ FID_BR_TOT      , U_SHORT | U_INT | U_LONG    , LIT , 0 ,"br_tot"          },
{ FID_MS          , U_SHORT | U_INT | U_LONG    , LIT , 0 ,"ms"              },
{ FID_MR          , U_SHORT | U_INT | U_LONG    , LIT , 0 ,"mr"              },
{ FID_BS          , U_SHORT | U_INT | U_LONG    , LIT , 0 ,"bs"              },
{ FID_BR          , U_SHORT | U_INT | U_LONG    , LIT , 0 ,"br"              },
{ FID_SUB_CNT     , U_SHORT | U_INT | U_LONG    , LIT , 0 ,"sub_cnt"         },
{ FID_COST        , U_SHORT | U_INT             , LIT , 0 ,"cost"            },
{ FID_ADJ_COST    , SHORT_STRING                , LIT , 0 ,"adj_cost"        },
{ FID_PEER        , SHORT_STRING                , LIT , 0 ,"peer"            },
{ FID_LATENCY     , SHORT_STRING                , LIT , 0 ,"latency"         },

{ FID_PK_DIGEST   , OPAQUE_16                   , XCL , 0 ,"pk_digest"       },
{ FID_TPORT_TYPE  , SHORT_STRING                , XCL , 0 ,"tport_type"      },
{ FID_CHAIN_SEQNO , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"chain_seqno"     },
{ FID_STAMP       , U_SHORT | U_INT | U_LONG    , TIM , 0 ,"stamp"           },
{ FID_CONVERGE    , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"converge"        },
{ FID_REPLY_STAMP , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"reply_stamp"     },
{ FID_HB_SKEW     , U_SHORT | U_INT | U_LONG    , XCL , 0 ,"hb_skew"         },
{ FID_PK_SIG      , OPAQUE_64                   , XCL , 0 ,"pk_sig"          },
{ FID_CONN_URL    , SHORT_STRING                , XCL , 0 ,"conn_url"        },
{ FID_CONN_PORT   , U_SHORT                     , XCL , 0 ,"conn_port"       },
{ FID_PUBKEY      , OPAQUE_32                   , XCL , 0 ,"pubkey"          },

{ FID_UCAST_DB    , LONG_OPAQUE                 , XCL , 0 ,"ucast_db"        },
{ FID_UCAST_FILTER, LONG_OPAQUE                 , XCL , 0 ,"ucast_filter"    },
{ FID_IDL_SERVICE , U_SHORT                     , XCL , 0 ,"idl_service"     },
{ FID_IDL_MSG_LOSS, U_SHORT | U_INT | U_LONG    , XCL , 0 ,"idl_msg_loss"    },
{ FID_REM_BRIDGE  , OPAQUE_16                   , XCL , 0 ,"rem_bridge"      },
{ FID_REM_TPORTID , U_SHORT | U_INT             , XCL , 0 ,"rem_tportid"     },
{ FID_MESH_INFO   , U_SHORT | U_INT             , XCL , 0 ,"mesh_info"       },
{ FID_LINK_STATE_SUM,U_SHORT | U_INT | U_LONG   , XCL , 0 ,"link_state_sum"  },
{ FID_SUB_SEQNO_SUM ,U_SHORT | U_INT | U_LONG   , XCL , 0 ,"sub_seqno_sum"   },
{ FID_HOST_ID     , U_SHORT | U_INT             , XCL , 0 ,"host_id"         },
{ FID_QUEUE       , SHORT_STRING                , XCL , 0 ,"queue"           },
{ FID_QUEUE_HASH  , U_SHORT | U_INT             , XCL , 0 ,"queue_hash"      },
{ FID_QUEUE_REFS  , U_SHORT | U_INT             , XCL , 0 ,"queue_refs"      },
{ FID_HDR_LEN     , U_SHORT | U_INT             , XCL , 0 ,"hdr_len"         },
{ FID_SUF_LEN     , U_SHORT | U_INT             , XCL , 0 ,"suf_len"         },
{ FID_BLOOM_UPD   , BOOL_1                      , XCL , 0 ,"bloom_upd"       },
{ FID_IDL_RESTART , BOOL_1                      , XCL , 0 ,"idl_restart"     },
{ FID_PATH_LIMIT  , U_SHORT | U_INT             , XCL , 0 ,"path_limit"      }
};

#endif
}
}

#endif
