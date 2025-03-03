#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <errno.h>
#include <raikv/logger.h>
#include <raids/term.h>
#define IMPORT_CONSOLE_CMDS
#define IMPORT_EVENT_DATA
#define IMPORT_DEBUG_STRINGS
#define IMPORT_CONSOLE_CONST
#include <raims/session.h>
#include <raims/ev_tcp_transport.h>
#include <raims/ev_rv_transport.h>
#include <raims/ev_telnet.h>
#include <raims/ev_web.h>
#include <raims/ev_name_svc.h>
#include <raims/adj_graph.h>
#include <linecook/linecook.h>
#include <linecook/ttycook.h>
#include <raimd/json_msg.h>
#include <sassrv/ev_rv.h>
#include <natsmd/ev_nats.h>
#if defined( _MSC_VER ) || defined( __MINGW32__ )
#include <raikv/win.h>
#endif
#include <raikv/os_file.h>

namespace rai {
namespace sassrv {
extern uint32_t rv_debug;
extern uint32_t rv_host_debug;
}
namespace natsmd {
extern uint32_t nats_debug;
}
}

using namespace rai;
using namespace ms;
using namespace kv;
using namespace md;
using namespace ds;

#if defined( _MSC_VER ) || defined( __MINGW32__ )
static inline int64_t ms_localtime( time_t t, struct tm &tmbuf ) {
  ::localtime_s( &tmbuf, &t );
  TIME_ZONE_INFORMATION tzinfo;
  if ( GetTimeZoneInformation( &tzinfo ) == TIME_ZONE_ID_INVALID )
    return 0;
  return (int64_t) tzinfo.Bias * (int64_t) 60;
}
#else
static inline int64_t ms_localtime( time_t t, struct tm &tmbuf ) {
  ::localtime_r( &t, &tmbuf );
  return (int64_t) tmbuf.tm_gmtoff;
}
#endif
int64_t rai::ms::tz_offset_sec,
        rai::ms::tz_offset_ns,
        rai::ms::tz_stamp_sec,
        rai::ms::tz_stamp_ns;
bool    rai::ms::tz_stamp_gmt;

static time_t
update_tz_offset( void ) noexcept
{
  time_t now = ::time( NULL );
  struct tm local;
  tz_offset_sec = ms_localtime( now, local );
  tz_offset_ns  = tz_offset_sec * (int64_t) 1000000000;
  if ( tz_stamp_gmt ) {
    tz_stamp_sec = 0;
    tz_stamp_ns  = 0;
  }
  else {
    tz_stamp_sec = tz_offset_sec;
    tz_stamp_ns  = tz_offset_ns;
  }
  local.tm_sec = 0;
  local.tm_min = 0;
  local.tm_hour = 0;
  return mktime( &local );
}

void
rai::ms::update_tz_stamp( void )
{
  update_tz_offset();
}

Console::Console( SessionMgr &m ) noexcept
       : MDOutput( MD_OUTPUT_OPAQUE_TO_B64 ), mgr( m ), user_db( m.user_db ),
         sub_db( m.sub_db ), tree( m.tree ), startup( m.startup ),
         string_tab( m.user_db.string_tab ),
         cfg_tport( 0 ), fname_fmt( ANSI_GREEN "%-18s" ANSI_NORMAL " : " ),
         type_fmt( ANSI_BLUE "%-10s %3d" ANSI_NORMAL " : " ),
         prompt( 0 ), max_log( 64 * 1024 ), log_index( 0 ), log_ptr( 0 ),
         inbox_num( 0 ), log_max_rotate( 0 ), log_rotate_time( 0 ),
         log_max_size( 0 ), log_filename( 0 ), log_fd( -1 ), next_rotate( 1 ),
         log_status( 0 ), last_log_hash( 0 ), last_log_repeat_count( 0 ),
         mute_log( false ), log_rate_total( 0 ),
         max_terminal_log_rate( 256 * 1024 )
{
  time_t t = update_tz_offset();
  t += 24 * 60 * 60;
  this->log_rotate_time = (uint64_t) t * 1000000000;
  this->make_prompt();
  ::memset( this->log_rate, 0, sizeof( this->log_rate ) );
  ::memset( this->log_time, 0, sizeof( this->log_time ) );
  this->last_rate = 0;
}

static const char *nc = ANSI_NORMAL;
static int         nz = ANSI_NORMAL_SIZE;
static const char *bc = ANSI_BLUE;
static int         bz = ANSI_BLUE_SIZE;
static const char *rc = ANSI_RED;
static int         rz = ANSI_RED_SIZE;
static const char *gc = ANSI_GREEN;
static int         gz = ANSI_GREEN_SIZE;
/*static const char *yc = ANSI_YELLOW;
static int         yz = ANSI_YELLOW_SIZE;*/
static const char *cc = ANSI_CYAN;
static int         cz = ANSI_CYAN_SIZE;

static const char  html_rc[] = "<span style=\"color:red\">";
static int         html_rz   = sizeof( html_rc ) - 1;
static const char  html_gc[] = "<span style=\"color:green\">";
static int         html_gz   = sizeof( html_gc ) - 1;
static const char  html_nc[] = "</span>";
static int         html_nz   = sizeof( html_nc ) - 1;

bool
Console::open_log( const char *fn,  bool add_hdr ) noexcept
{
  if ( this->log_fd >= 0 ) {
    os_close( this->log_fd );
    this->log_fd = -1;
  }
  if ( fn != NULL ) {
    this->log_fd = os_open( fn, O_APPEND | O_WRONLY | O_CREAT, 0666 );
    if ( this->log_fd < 0 ) {
      ::perror( fn );
      return false;
    }
    if ( add_hdr && ! Console::log_header( this->log_fd ) ) {
      ::perror( fn );
      os_close( this->log_fd );
      this->log_fd = -1;
      return false;
    }
  }
  this->log_filename = fn;
  return true;
}

bool
Console::log_header( int fd ) noexcept
{
  static const char sep[] = "=--=--=--=\n";
  time_t now = ::time( NULL );
  char   line[ 256 ];
  size_t off = 0;

  ::strcpy( &line[ off ], "=--=--=--=\n" );  off = sizeof( sep ) - 1;
  ::strcpy( &line[ off ], ::ctime( &now ) ); off = ::strlen( line );
#if ! defined( _MSC_VER ) && ! defined( __MINGW32__ )
  const char *tz = tzname[ daylight ];
#else
  const char *tz = _tzname[ _daylight ];
#endif
  if ( tz_offset_sec == 0 )
    update_tz_offset();
  int diff_min = (int)( tz_offset_sec / (int64_t) 60 ),
      diff_hr  = diff_min / 60;
  diff_min %= 60;
  if ( diff_min < 0 )
    diff_min = -diff_min;

  off += ::snprintf( &line[ off ], sizeof( line ) - off,
    "UTC offset: %d:%02d (%s)\n", diff_hr, diff_min, tz );
  off += ::snprintf( &line[ off ], sizeof( line ) - off,
    "PID: %d, ms_server version: %s\n", ::getpid(), ms_get_version() );
  ::strcpy( &line[ off ], "=--=--=--=\n" );  off += sizeof( sep ) - 1;
  if ( (size_t) os_write( fd, line, off ) != off )
    return false;
  return true;
}

bool
Console::rotate_log( void ) noexcept
{
  time_t t = update_tz_offset();
  t += 24 * 60 * 60;
  uint64_t next = (uint64_t) t * 1000000000;
  if ( next > this->log_rotate_time )
    this->log_rotate_time = next;
  else {
    next = 24 * 60 * 60 * (uint64_t) 1000000000;
    this->log_rotate_time += next;
  }
  if ( this->log_fd >= 0 ) {
    os_close( this->log_fd );
    this->log_fd = -1;

    size_t len = ::strlen( this->log_filename );
    char   newpath[ 1024 ];
    if ( len > sizeof( newpath ) - 24 )
      len = sizeof( newpath ) - 24;
    ::memcpy( newpath, this->log_filename, len );
    newpath[ len ] = '.';
    for ( uint32_t i = this->next_rotate; ; i++ ) {
      size_t j = uint32_to_string( i, &newpath[ len + 1 ] );
      newpath[ len + 1 + j ] = '\0';
      if ( os_access( newpath, R_OK | W_OK ) != 0 ) {
        this->next_rotate = i + 1;
        break;
      }
    }
    if ( os_rename( this->log_filename, newpath ) != 0 ) {
      ::perror( newpath );
      return false;
    }
    return this->open_log( this->log_filename, true );
  }
  return true;
}

void
Console::parse_debug_flags( const char *arg,  size_t len,
                            int &dist_dbg,  int &check_blooms ) noexcept
{
  dbg_flags             = 0;
  kv_pub_debug          = 0;
  kv_ps_debug           = 0;
  sassrv::rv_debug      = 0;
  sassrv::rv_host_debug = 0;
  natsmd::nats_debug    = 0;
  no_tcp_aes            = 0;

  for ( size_t i = 0; i < debug_str_count; i++ ) {
    size_t dlen = ::strlen( debug_str[ i ] );
    void * p;
    if ( (p = kv_memmem( arg, len, debug_str[ i ], dlen )) != NULL ) {
      if ( (char *) p == arg || ((char *) p)[ -1 ] == ' ' || 
           ((char *) p)[ -1 ] == ',' || ((char *) p)[ -1 ] == '|' )
        dbg_flags |= ( 1 << i );
    }
  }
  if ( len >= 4 && kv_memmem( arg, len, "dist", 4 ) != NULL )
    dist_dbg = 1;
  if ( len >= 5 && kv_memmem( arg, len, "bloom", 5 ) != NULL )
    check_blooms = 1;
  if ( len >= 2 && kv_memmem( arg, len, "kvpub", 5 ) != NULL )
    kv_pub_debug = 1;
  if ( len >= 4 && kv_memmem( arg, len, "kvps", 4 ) != NULL )
    kv_ps_debug = 1;
  if ( len >= 2 && kv_memmem( arg, len, "rv", 2 ) != NULL )
    sassrv::rv_debug = 1;
  if ( len >= 4 && kv_memmem( arg, len, "host", 4 ) != NULL )
    sassrv::rv_host_debug = 1;
  if ( len >= 2 && kv_memmem( arg, len, "nats", 4 ) != NULL )
    natsmd::nats_debug = 1;
  if ( len >= 5 && kv_memmem( arg, len, "noaes", 5 ) != NULL )
    no_tcp_aes = 1;
  if ( dbg_flags == 0 && len > 0 && arg[ 0 ] >= '0' && arg[ 0 ] <= '9' )
    dbg_flags = (int) string_to_uint64( arg, len );
}

char *
cat_prompt( char *p,  char *e,  const char *s1,  const char *s2 = NULL,
            const char *s3 = NULL ) noexcept
{
  for ( ; *s1 != '\0'; s1++ ) if ( p < e ) *p++ = *s1;
  if ( s2 != NULL ) {
    for ( ; *s2 != '\0'; s2++ ) if ( p < e ) *p++ = *s2;
    if ( s2 != NULL ) {
      for ( ; *s3 != '\0'; s3++ ) if ( p < e ) *p++ = *s3;
    }
  }
  return p;
}

size_t
Console::make_prompt( const char *where,  size_t wsz ) noexcept
{
  char       * p, * e,
               blank[ NONCE_B64_LEN + 1 ];
  const char * us = this->user_db.user.user.val,
             * sv = this->user_db.svc.svc.val;
  size_t       off, boff;

  if ( this->prompt == NULL )
    this->prompt = (char *) ::malloc( 256 );
  p   = this->prompt;
  off = ( ANSI_NORMAL_SIZE + 2 );
  e   = &this->prompt[ 255 - off ];
  memset( blank, '_', NONCE_B64_LEN );
  blank[ NONCE_B64_LEN ] = '\0';
  p = cat_prompt( p, e, ANSI_CYAN, us, "." );
  p = cat_prompt( p, e, sv, ANSI_NORMAL "[", ANSI_GREEN );
  boff = p - this->prompt;
  p = cat_prompt( p, e,
                  blank,             ANSI_NORMAL "]@",
                  ANSI_MAGENTA "\\h" ANSI_NORMAL
                  ANSI_BLUE    "["   ANSI_NORMAL
                  ANSI_RED     "\\#" ANSI_NORMAL
                  ANSI_BLUE    "]"   );
  if ( where != NULL ) {
    p = cat_prompt( p, e, ANSI_YELLOW "(" );
    for ( size_t i = 0; i < wsz; i++ ) {
      if ( p < e )
        *p++ = where[ i ];
    }
    p = cat_prompt( p, e, ")" );
  }
  e = &e[ off ];
  p = cat_prompt( p, e, ANSI_NORMAL "> " );
  *p = '\0';
  return boff;
}

void
Console::update_prompt( const char *where,  size_t wsz ) noexcept
{
  size_t off = this->make_prompt( where, wsz );
  this->user_db.bridge_id.nonce.to_base64( &this->prompt[ off ] );
}

bool
ConsoleOutput::on_output( const char *,  size_t ) noexcept
{
  return true; /* virtual override by terminal class */
}

void
ConsoleOutput::on_prompt( const char * ) noexcept
{
}

void
ConsoleOutput::on_quit( void ) noexcept
{
}

void
ConsoleOutput::on_remove( void ) noexcept
{
}

void
LastTimeStamp::update( uint64_t stamp ) noexcept
{
  uint64_t secs, ms;
  ms     = stamp / (uint64_t) ( 1000 * 1000 );
  secs   = ms / (uint64_t) 1000;

  if ( secs != this->last_secs ) {
    uint64_t day = secs / ( 24 * 60 * 60 );
    if ( day != this->last_day ) {
      time_t t = (time_t) secs;
      struct tm x;
#if defined( _MSC_VER ) || defined( __MINGW32__ )
      ::gmtime_s( &x, &t );
#else
      ::gmtime_r( &t, &x );
#endif
      x.tm_mon++;
      this->ts[ 0 ] = ( x.tm_mon / 10 ) + '0';
      this->ts[ 1 ] = ( x.tm_mon % 10 ) + '0';
      this->ts[ 2 ] = ( x.tm_mday / 10 ) + '0';
      this->ts[ 3 ] = ( x.tm_mday % 10 ) + '0';
      this->ts[ 4 ] = ' ';
      this->last_day = day;
    }
    uint32_t ar[ 3 ], j = TS_MON_DAY_LEN;
    ar[ 2 ] = secs % 60,
    ar[ 1 ] = ( secs / 60 ) % 60;
    ar[ 0 ] = ( secs / 3600 ) % 24;
    for ( int i = 0; i < 3; i++ ) {
      this->ts[ j++ ] = ( ar[ i ] / 10 ) + '0';
      this->ts[ j++ ] = ( ar[ i ] % 10 ) + '0';
      this->ts[ j++ ] = ( i == 2 ? '.' : ':' );
    }
    this->last_secs = secs;
  }
  if ( ms != this->last_ms ) {
    this->ts[ TS_FRACTION_OFF+1 ] = ( ( ms / 100 ) % 10 ) + '0';
    this->ts[ TS_FRACTION_OFF+2 ] = ( ( ms / 10 ) % 10 ) + '0';
    this->ts[ TS_FRACTION_OFF+3 ] = ( ms % 10 ) + '0';
    this->last_ms = ms;
  }
}

void
Console::log_repeated( void ) noexcept
{
  static const char   repeat_cnt[]  = " line repeated ";
  static const size_t repeat_cnt_sz = sizeof( repeat_cnt ) - 1;
  char   repeat[ 16 ];
  size_t cnt_sz = uint32_to_string( this->last_log_repeat_count, repeat );

  size_t sz = cnt_sz + repeat_cnt_sz + TS_HDR_LEN + 1;
  char * p  = this->log.make( this->log.count + sz );
  p  = &p[ this->log.count ];

  ::memcpy( p, this->log_ts.ts, TS_ERR_OFF );
  p = &p[ TS_ERR_OFF ];
  *p++ = ' ';
  *p++ = ' ';
  ::memcpy( p, repeat_cnt, repeat_cnt_sz );
  p = &p[ repeat_cnt_sz ];
  ::memcpy( p, repeat, cnt_sz );
  p = &p[ cnt_sz ];
  p[ 0 ] = '\n';

  this->log.count += sz;
  this->last_log_repeat_count = 0;
  this->last_log_hash = 0;
}

bool
Console::timer_cb( uint64_t timer_id, uint64_t ) noexcept
{
  if ( this->last_log_repeat_count > 0 &&
       timer_id == ( this->log_ts.last_ms >> 10 ) ) {
    this->log_repeated();
    this->flush_output( NULL );
  }
  return false;
}

void
Console::log_output( int stream,  uint64_t stamp,  size_t len,
                     const char *buf ) noexcept
{
  size_t sz;
  char * p;

  stamp += tz_stamp_ns;
  this->log_ts.update( stamp );

  uint32_t last = (uint32_t)( this->log_ts.last_ms >> 10 );
  uint32_t h = kv_crc_c( buf, len, last );
  if ( h == this->last_log_hash ) {
    if ( this->last_log_repeat_count++ == 0 ) {
      EvPoll & poll = this->mgr.poll;
      poll.timer.add_timer_millis( *this, 5, last, 0 );
    }
    return;
  }
  if ( this->last_log_repeat_count > 0 )
    this->log_repeated();
  this->last_log_hash = h;

  sz = len + TS_HDR_LEN;
  p  = this->log.make( this->log.count + sz );
  p = &p[ this->log.count ];
  ::memcpy( p, this->log_ts.ts, TS_ERR_OFF );
  p = &p[ TS_ERR_OFF ];
  *p++ = ( stream == 1 ? ' ' : '!' );
  *p++ = ' ';
  ::memcpy( p, buf, len );
  this->log.count += sz;
}

bool
Console::colorize_log( ConsoleOutput *p, const char *buf, size_t len ) noexcept
{
  const bool is_html = ( p != NULL && p->is_html );
  const bool is_json = ( p != NULL && p->is_json );
  const char * end = &buf[ len ];
  bool b = true, first = true;

  if ( is_html )
    p->on_output( "<pre>", 5 );
  while ( buf < end ) {
    len = end - buf;
    const char *ptr = (const char *) ::memchr( buf, '\n', len );
    if ( ptr == NULL ) {
      ptr = &buf[ len ];
    }
    else {
      if ( ptr > buf && *( ptr - 1 ) == '\r' )
        ptr--;
    }
    if ( &buf[ TS_HDR_LEN ] < ptr ) {
      if ( ! is_json ) {
        const char * color    = is_html ? html_gc : gc;
        size_t       color_sz = is_html ? html_gz : gz;
        const char * no_col   = is_html ? html_nc : nc;
        size_t       no_sz    = is_html ? html_nz : nz;

        if ( buf[ TS_ERR_OFF ] != ' ' ) {
          color    = is_html ? html_rc : rc;
          color_sz = is_html ? html_rz : rz;
        }
        size_t off  = 0,
               sz = ptr - &buf[ TS_HDR_LEN ];
        char * line = this->tmp.str_make( TS_HDR_LEN + color_sz + sz + nz + 1 );

        ::memcpy( line, buf, TS_HDR_LEN );                off += TS_HDR_LEN;
        ::memcpy( &line[ off ], color, color_sz );        off += color_sz;
        ::memcpy( &line[ off ], &buf[ TS_HDR_LEN ], sz ); off += sz;
        ::memcpy( &line[ off ], no_col, no_sz );          off += no_sz;
        line[ off++ ] = '\n';

        if ( p != NULL )
          b &= p->on_output( line, off );
        else {
          for ( ConsoleOutput *o = this->term_list.hd; o!= NULL; o = o->next )
            b &= o->on_output( line, off );
        }
        this->tmp.reuse();
      }
      else {
        const char * ln = &buf[ TS_ERR_OFF ];
        size_t       sz = ptr - ln;
        const char * q;
        #define STR( s ) s, sizeof( s ) - 1
        b &= p->on_output( first ? "[" : ",", 1 ); first = false;
        b &= p->on_output( STR(   "{\"time\":\"" ) );
        b &= p->on_output( buf, TS_ERR_OFF );
        b &= p->on_output( STR( "\",\"text\":\"" ) );
        while ( (q = (char *) ::memchr( ln, '\"', sz )) != NULL ) {
          size_t seg = q - ln;
          if ( seg > 0 )
            b &= p->on_output( ln, seg );
          p->on_output( "\\\"", 2 );
          ln = &q[ 1 ];
          sz = ptr - ln;
        }
        if ( sz > 0 )
          b &= p->on_output( ln, sz );
        b &= p->on_output( STR( "\"}" ) );
        #undef STR
      }
    }
    buf = ptr;
    if ( buf < end && buf[ 0 ] == '\r' )
      buf++;
    if ( buf < end && buf[ 0 ] == '\n' )
      buf++;
  }
  if ( is_json ) {
    if ( first )
      b &= p->on_output( "[]\n", 3 );
    else
      b &= p->on_output( "]\n", 2 );
  }
  return b;
}

void
Console::add_output( ConsoleOutput *p ) noexcept
{
  this->term_list.push_tl( p );
}

void
Console::remove_output( ConsoleOutput *p ) noexcept
{
  this->term_list.pop( p );
  for ( ConsoleRPC *rpc = this->rpc_list.hd; rpc != NULL; rpc = rpc->next ) {
    if ( rpc->is_complete )
      continue;
    rpc->out.remove( p );
  }
}

bool
Console::flush_output( ConsoleOutput *p ) noexcept
{
  bool b = true;
  if ( this->out.count > 0 ) {
    size_t len = this->out.count;
    if ( p != NULL )
      b &= p->on_output(  this->out.ptr, len );
    else {
      for ( ConsoleOutput *o = this->term_list.hd; o != NULL; o = o->next ) {
        b &= o->on_output( this->out.ptr, len );
      }
    }
#if 0
    {
      static int test_fd;
      if ( test_fd == 0 )
        test_fd = os_open( "test.txt", O_APPEND | O_WRONLY | O_CREAT, 0666 );
      os_write( test_fd, this->out.ptr, len );
    }
#endif
    this->out.count = 0;
  }
  if ( ( p == NULL || ! ( p->is_html | p->is_json ) ) &&
       this->log_index < this->log.count ) {
    const char * lptr = &this->log.ptr[ this->log_index ];
    size_t       lsz  = this->log.count - this->log_index;
    if ( this->log_fd >= 0 ) {
      if ( (size_t) os_write( this->log_fd, lptr, lsz ) != lsz )
        this->log_status = errno;
      else
        this->log_status = 0;
    }
    if ( ! this->mute_log )
      b &= this->colorize_log( p, lptr, lsz );
    if ( this->log_ptr == 0 && this->log_index >= this->max_log )
      this->log_ptr = this->log_index;
    this->log_index = this->log.count;
    if ( this->log_index >= this->max_log * 2 ) {
      ::memmove( this->log.ptr, &this->log.ptr[ this->log_ptr ],
                 this->log_index - this->log_ptr );
      this->log_index -= this->log_ptr;
      this->log.count  = this->log_index;
      this->log_ptr = 0;
    }
  }
  return b;
}

void
Console::throttle_rate( uint64_t stamp,  size_t len ) noexcept
{
  uint32_t i = (uint32_t) ( stamp >> 30 ) & ( LOG_RATE_PERIOD - 1 );
  if ( this->log_time[ i ] != 0 ) {
    uint32_t j = (uint32_t) ( this->log_time[ i ] >> 30 ) &
                            ( LOG_RATE_PERIOD - 1 );
    if ( i != j ) {
      this->log_rate_total -= this->log_rate[ j ];
      this->log_rate[ j ] = 0;
    }
  }
  this->log_rate[ i ]  += len;
  this->log_time[ i ]   = stamp;
  this->log_rate_total += len;
  this->last_rate       = i;
}

uint64_t
Console::throttle_total( uint64_t &period ) noexcept
{
  uint32_t k = this->last_rate,
           i = k;
  uint64_t head_time = this->log_time[ i ],
           tail_time = head_time;
  for ( uint32_t j = 0; j < LOG_RATE_PERIOD - 1; j++ ) {
    k = ( k > 0 ? k - 1 : LOG_RATE_PERIOD - 1 );
    if ( this->log_time[ k ] == 0 )
      continue;
    if ( ( ( head_time - this->log_time[ k ] ) >> 30 ) > j + 2 ) {
      this->log_rate_total -= this->log_rate[ k ];
      this->log_rate[ k ] = 0;
      this->log_time[ k ] = 0;
    }
    if ( this->log_time[ k ] < tail_time )
      tail_time = this->log_time[ k ];
  }
  period = head_time - tail_time;
  return this->log_rate_total;
}

bool
Console::on_log( Logger &log ) noexcept
{
  char     out[ 4 * 1024 ],
           err[ 4 * 1024 ];
  size_t   out_len   = sizeof( out ),
           err_len   = sizeof( err );
  uint64_t out_stamp = 0,
           err_stamp = 0;
  bool     out_done  = false,
           err_done  = false,
           b         = false;

  if ( ! log.avail() )
    return false;
  while ( ! out_done || ! err_done ) {
    if ( ! out_done && out_stamp == 0 ) {
      out_stamp = log.read_stdout( out, out_len );
      if ( out_stamp == 0 )
        out_done = true;
      else if ( ! this->mute_log && this->term_list.hd != NULL )
        this->throttle_rate( out_stamp, out_len );
    }
    if ( ! err_done && err_stamp == 0 ) {
      err_stamp = log.read_stderr( err, err_len );
      if ( err_stamp == 0 )
        err_done = true;
      else if ( ! this->mute_log && this->term_list.hd != NULL )
        this->throttle_rate( err_stamp, err_len );
    }
    bool do_out = ! out_done, do_err = ! err_done;
    if ( do_out && do_err ) {
      if ( out_stamp < err_stamp )
        do_err = false;
      else
        do_out = false;
    }
    if ( do_out ) {
      if ( out_len > 1 || out[ 0 ] != '\n' )
        this->log_output( 1, out_stamp, out_len, out );
      out_stamp = 0; out_len = sizeof( out );
      b = true;
    }
    if ( do_err ) {
      if ( err_len > 1 || err[ 0 ] != '\n' )
        this->log_output( 2, err_stamp, err_len, err );
      err_stamp = 0; err_len = sizeof( err );
      b = true;
    }
  }
  if ( b ) {
    uint64_t period;
    if ( this->log_rate_total > this->max_terminal_log_rate &&
         this->throttle_total( period ) > this->max_terminal_log_rate ) {
      this->printf( "log muted, %" PRIu64 " bytes logged in %.3f seconds\n",
                    this->log_rate_total, (double) period / 1000000000.0 );
      ::memset( this->log_rate, 0, sizeof( this->log_rate ) );
      ::memset( this->log_time, 0, sizeof( this->log_time ) );
      this->log_rate_total = 0;
      this->change_prompt( "mute", 4 );
      this->mute_log = true;
    }
    this->flush_output( NULL );
  }
  return b;
}

void
Console::flush_log( Logger &log ) noexcept
{
  /*log.flush();*/
#if ! defined( _MSC_VER ) && ! defined( __MINGW32__ )
  usleep( 500 );
  while ( this->on_log( log ) )
    usleep( 100 );
#else
  Sleep( 1 );
  while ( this->on_log( log ) )
    Sleep( 1 );
#endif
}

JsonFileOutput *
JsonFileOutput::create( const char *path,  size_t pathlen ) noexcept
{
  char fn[ 1024 ];
  int  fnlen = ::snprintf( fn, sizeof( fn ), "%.*s", (int) pathlen, path );
  int  fd    = os_open( fn, O_APPEND | O_WRONLY | O_CREAT, 0666 );
  if ( fd < 0 )
    return NULL;
  if ( (size_t) fnlen >= sizeof( fn ) )
    return NULL;
  void       * p   = ::malloc( sizeof( JsonFileOutput ) + fnlen + 1 );
  JsonFileOutput * out = new ( p ) JsonFileOutput( fd );

  ::memcpy( (void *) &out[ 1 ], fn, fnlen );
  out->path = (char *) (void *) &out[ 1 ];
  out->path[ fnlen ] = '\0';
  out->pathlen = fnlen;

  return out;
}

bool
JsonFileOutput::open( void ) noexcept
{
  int fd = os_open( this->path, O_APPEND | O_WRONLY | O_CREAT, 0666 );
  if ( fd < 0 )
    return false;
  this->fd = fd;
  return true;
}

bool
JsonFileOutput::on_output( const char *buf,  size_t buflen ) noexcept
{
  if ( os_write( this->fd, buf, buflen ) != (ssize_t) buflen ) {
    ::perror( this->path );
    os_close( this->fd );
    this->fd = -1;
  }
  return true;
}

void
JsonFileOutput::on_remove( void ) noexcept
{
  if ( this->fd != -1 ) {
    os_close( this->fd );
    this->fd = -1;
  }
}

JsonFileOutput *
JsonOutArray::open( const char *path,  size_t pathlen ) noexcept
{
  JsonFileOutput *out = NULL;
  for ( size_t i = 0; i < this->count; i++ ) {
    if ( pathlen == this->ptr[ i ]->pathlen &&
         ::memcmp( path, this->ptr[ i ]->path, pathlen ) == 0 ) {
      if ( this->ptr[ i ]->rpc == NULL ) {
        out = this->ptr[ i ];
        break;
      }
    }
  }
  if ( out == NULL ) {
    out = JsonFileOutput::create( path, pathlen );
    if ( out == NULL )
      return NULL;
  }
  else {
    if ( ! out->open() )
      return NULL;
  }
  this->operator[]( this->count ) = out;
  return out;
}

JsonFileOutput *
JsonOutArray::find( const char *path,  size_t pathlen ) noexcept
{
  for ( size_t i = 0; i < this->count; i++ ) {
    if ( pathlen == this->ptr[ i ]->pathlen &&
         ::memcmp( path, this->ptr[ i ]->path, pathlen ) == 0 ) {
      if ( this->ptr[ i ]->rpc != NULL )
        return this->ptr[ i ];
    }
  }
  return NULL;
}

static void
make_valid( ValidTportCmds &valid,
            const ConsoleCmdString *cmd,  size_t ncmds,
            const ConsoleCmdString *help,  size_t nhelps ) noexcept
{
  ConsoleCmdString * x = (ConsoleCmdString *)
                      ::malloc( sizeof( cmd[ 0 ] ) * ( valid.nvalid + 1 ) * 2 ),
                   * y = &x[ valid.nvalid + 1 ];
  size_t             xcnt = 0,
                     ycnt = 0,
                     j, k;
  for ( j = 0; j < valid.nvalid; j++ ) {
    for ( k = 0; k < ncmds && xcnt < valid.nvalid + 1; k++ ) {
      if ( cmd[ k ].cmd == valid.valid[ j ] ) {
        x[ xcnt++ ] = cmd[ k ]; /* quit/exit have two enties */
      }
    }
    for ( k = 0; k < nhelps && ycnt < valid.nvalid + 1; k++ ) {
      if ( help[ k ].cmd == valid.valid[ j ] ) {
        y[ ycnt++ ] = help[ k ];
        break;
      }
    }
  }
  valid.cmd    = x;
  valid.ncmds  = xcnt;
  valid.help   = y;
  valid.nhelps = ycnt;
}

void
Console::get_valid_cmds( const ConsoleCmdString *&cmds,
                         size_t &ncmds ) noexcept
{
  if ( this->cfg_tport == NULL ) {
    cmds  = console_cmd;
    ncmds = num_console_cmds;
  }
  else {
    for ( size_t i = 0; i < num_valid_tport_cmds; i++ ) {
      if ( this->cfg_tport->type.equals( valid_tport_cmd[ i ].type ) ) {
        ValidTportCmds &valid = valid_tport_cmd[ i ];
        if ( valid.ncmds == 0 )
          make_valid( valid, tport_cmd, num_tport_cmds, tport_help_cmd,
                      num_tport_help_cmds );
        cmds  = valid.cmd;
        ncmds = valid.ncmds;
        return;
      }
    }
    cmds  = tport_cmd;
    ncmds = num_tport_cmds;
  }
}

void
Console::get_valid_help_cmds( const ConsoleCmdString *&cmds,
                              size_t &ncmds ) noexcept
{
  if ( this->cfg_tport == NULL ) {
    cmds  = help_cmd;
    ncmds = num_help_cmds;
  }
  else {
    for ( size_t i = 0; i < num_valid_tport_cmds; i++ ) {
      if ( this->cfg_tport->type.equals( valid_tport_cmd[ i ].type ) ) {
        ValidTportCmds &valid = valid_tport_cmd[ i ];
        if ( valid.nhelps == 0 )
          make_valid( valid, tport_cmd, num_tport_cmds, tport_help_cmd,
                      num_tport_help_cmds );
        cmds  = valid.help;
        ncmds = valid.nhelps;
        return;
      }
    }
    cmds  = tport_help_cmd;
    ncmds = num_tport_help_cmds;
  }
}

int
Console::which_cmd( const ConsoleCmdString *cmds,  size_t ncmds,
                    const char *buf, size_t buflen, CmdMask *cmd_mask ) noexcept
{
  CmdMask  match;
  size_t   off, last = 0;
  bool     matched = false;
  match.mask( ncmds );
  for ( off = 0; match.count() != 0; off++ ) {
    if ( off == buflen || buf[ off ] == ' ' ) {
      matched = true;
      break;
    }
    if ( buf[ off ] >= '0' && buf[ off ] <= '9' &&
         ( off + 1 == buflen || buf[ off + 1 ] == ' ' ) ) {
      if ( match.count() == 1 ) {
        switch ( cmds[ last ].cmd ) { /* allow digits */
          case CMD_TPORT_LISTEN:
          case CMD_TPORT_CONNECT:
          case CMD_TPORT_DEVICE:
          case CMD_TPORT_COST:
            matched = true;
            break;
          default:
            break;
        }
      }
      if ( matched )
        break;
    }
    for ( size_t i = 0; i < ncmds; i++ ) {
      if ( match.is_member( i ) ) {
        if ( cmds[ i ].str[ off ] == buf[ off ] ) {
          last = i;
          continue;
        }
        match.remove( i );
      }
    }
  }
  if ( cmd_mask != NULL ) {
    cmd_mask->zero();
    if ( match.count() != 0 ) {
      for ( size_t i = 0; i < ncmds; i++ ) {
        if ( match.is_member( i ) )
          cmd_mask->add( cmds[ i ].cmd );
      }
    }
  }
  if ( match.count() == 1 && matched )
    return cmds[ last ].cmd;
  return CMD_BAD;
}

static size_t
scan_args( const char *buf,  const char *end,  const char **args,
           size_t *arglen,  size_t maxargs ) noexcept
{
  size_t argc = 0;
  for (;;) {
    while ( buf < end && *buf <= ' ' )
      buf++;
    if ( buf == end || argc == maxargs )
      break;

    bool matched_quote = false;
    if ( ( buf[ 0 ] == '\"' || buf[ 0 ] == '\'' ) && buf + 1 < end ) {
      char quote = buf[ 0 ];
      const char * ptr = buf + 1;
      args[ argc ] = ptr;
      while ( ptr < end && ptr[ 0 ] != quote )
        ptr++;
      if ( ptr < end ) {
        buf = ptr + 1;
        arglen[ argc ] = ptr - args[ argc ];
        matched_quote = true;
      }
    }
    if ( ! matched_quote ) {
      args[ argc ] = buf;
      while ( buf < end && *buf > ' ' )
        buf++;
      arglen[ argc ] = buf - args[ argc ];
    }
    argc++;
  }
  return argc;
}

int
Console::parse_command( const char *buf,  const char *end,
                        const char *&arg,  size_t &len,
                        const char **args,  size_t *arglen,
                        size_t &argcount ) noexcept
{
  const ConsoleCmdString * cmds;
  size_t     argc = scan_args( buf, end, args, arglen, MAXARGS ),
             j    = 0,
             ncmds;
  ConsoleCmd cmd;

  this->get_valid_cmds( cmds, ncmds );
  argcount = argc;
  arg = NULL;
  len = 0;

  if ( argc == 0 )
    return CMD_EMPTY;
  cmd = (ConsoleCmd) which_cmd( cmds, ncmds, args[ 0 ], arglen[ 0 ], NULL );

  if ( cmd == CMD_BAD && cmds == console_cmd ) {
    ConsoleCmd cmd2 = which_show( args[ 0 ], arglen[ 0 ] );
    if ( cmd2 != CMD_BAD ) {
      cmd = cmd2;
      j   = -1; /* show optional */
      goto skip_parse_show;
    }
    return CMD_BAD;
  }
  switch ( cmd ) {
    default:
      if ( argc > 1 ) {
        arg = args[ 1 ];
        len = arglen[ 1 ];
      }
      break;
    case CMD_CONFIGURE: /* configure ... */
      if ( argc > 1 ) {
        cmd = which_config( args[ 1 ], arglen[ 1 ] );
        if ( cmd != CMD_BAD && argc > 2 ) {
          arg = args[ 2 ];
          len = arglen[ 2 ];
        }
      }
      break;
    case CMD_SHOW: /* show ... */
      if ( argc > 1 ) {
        cmd = which_show( args[ 1 ], arglen[ 1 ] );
      skip_parse_show:;
        /* show run ... */
        if ( cmd == CMD_SHOW_RUN || cmd == CMD_SHOW_START ) {
          if ( argc > j+2 ) {
            cmd = which_run( args[ j+2 ], arglen[ j+2 ] );
            if ( cmd != CMD_BAD && j+3 < argc ) {
              arg = args[ j+3 ];
              len = arglen[ j+3 ];
            }
          }
        }
        else {
          if ( cmd != CMD_BAD && j+2 < argc ) {
            arg = args[ j+2 ];
            len = arglen[ j+2 ];
          }
        }
      }
      break;
  }
  return cmd;
}

int
Console::shift_command( size_t shift,  const char **&args,  size_t *&arglen,
                        size_t &argcount ) noexcept
{
  const ConsoleCmdString * cmds;
  size_t ncmds;

  if ( shift >= argcount )
    return CMD_EMPTY;
  argcount -= shift;
  args      = &args[ shift ];
  arglen    = &arglen[ shift ];

  this->get_valid_cmds( cmds, ncmds );
  return which_cmd( cmds, ncmds, args[ 0 ], arglen[ 0 ], NULL );
}

int
console_complete( struct LineCook_s *state,  const char *buf,  size_t off,
                  size_t len,  void *me ) noexcept
{
  const ConsoleCmdString * cmds;
  size_t         ncmds;
  const char *   args[ Console::MAXARGS ];
  size_t         arglen[ Console::MAXARGS ],
                 argc,
                 arg_complete;
  int            j = 0;
  CmdMask        mask;
  ConsoleArgType type = NO_ARG;
  char           trail = 0;
  ConsoleCmd     cmd  = CMD_EMPTY;
  Console      * cons = (Console *) me;

  argc = scan_args( buf, &buf[ off + len ], args, arglen, Console::MAXARGS );
  arg_complete = argc;
  cons->get_valid_cmds( cmds, ncmds );

  if ( off + len > 0 ) {
    trail = buf[ off + len - 1 ];
    if ( trail == ' ' )
      arg_complete++;
  }
  if ( argc > 0 ) {
    cmd = (ConsoleCmd)
      Console::which_cmd( cmds, ncmds, args[ 0 ], arglen[ 0 ], &mask );

    if ( cmd == CMD_BAD && mask.count() == 0 && cmds == console_cmd ) {
      ConsoleCmd cmd2 = which_show( args[ 0 ], arglen[ 0 ] );
      if ( cmd2 != CMD_BAD ) {
        cmds  = show_cmd;
        ncmds = num_show_cmds;
        cmd   = cmd2;
        j     = -1; /* show optional */
        goto skip_parse_show;
      }
    }
    if ( ( argc == 1 && trail == ' ' ) ||
         ( argc == 2 && trail != ' ' ) )
      type = console_command_type( cmd );

    if ( cmd == CMD_SHOW && arg_complete > 1 ) {
      cmds  = show_cmd;
      ncmds = num_show_cmds;
      if ( argc > 1 ) {
        cmd = which_show( args[ 1 ], arglen[ 1 ] );
      skip_parse_show:;
        if ( ( argc == (size_t) ( j + 2 ) && trail == ' ' ) ||
             ( argc == (size_t) ( j + 3 ) && trail != ' ' ) )
          type = console_command_type( cmd );

        if ( ( cmd == CMD_SHOW_RUN || cmd == CMD_SHOW_START ) &&
             arg_complete > (size_t) ( j + 2 ) ) {
          cmds  = run_cmd;
          ncmds = num_run_cmds;

          if ( argc > (size_t) ( j + 2 ) ) {
            cmd = which_run( args[ j + 2 ], arglen[ j + 2 ] );

            if ( ( argc == (size_t) ( j + 3 ) && trail == ' ' ) ||
                 ( argc == (size_t) ( j + 4 ) && trail != ' ' ) )
              type = console_command_type( cmd );
          }
        }
      }
    }
    else if ( cmd == CMD_CONFIGURE && arg_complete > 1 ) {
      cmds  = config_cmd;
      ncmds = num_config_cmds;
      if ( argc > 1 ) {
        cmd = which_config( args[ 1 ], arglen[ 1 ] );
        if ( ( argc == 2 && trail == ' ' ) ||
             ( argc == 3 && trail != ' ' ) )
          type = console_command_type( cmd );
      }
    }
  }

  if ( type == NO_ARG ) {
    for ( size_t k = 0; k < ncmds; k++ )
      lc_add_completion( state, cmds[ k ].str, ::strlen( cmds[ k ].str ) );
  }
  else if ( type == TPORT_ARG || type == PEER_ARG || type == USER_ARG ||
            type == PARM_ARG || type == SVC_ARG ) {
    if ( type == PEER_ARG ) {
      for ( uint32_t uid = 1; uid < cons->user_db.next_uid; uid++ ) {
        UserBridge * n = cons->user_db.bridge_tab[ uid ];
        if ( n != NULL && n->is_set( AUTHENTICATED_STATE ) ) {
          lc_add_completion( state, n->peer.user.val, n->peer.user.len );
        }
      }
    }
    else if ( type == USER_ARG ) {
      for ( ConfigTree::User * user = cons->tree.users.hd;
            user != NULL; user = user->next )
        lc_add_completion( state, user->user.val, user->user.len );
    }
    else if ( type == TPORT_ARG ) {
      for ( ConfigTree::Transport * tport = cons->tree.transports.hd;
            tport != NULL; tport = tport->next )
        lc_add_completion( state, tport->tport.val, tport->tport.len );
    }
    else if ( type == SVC_ARG ) {
      for ( ConfigTree::Service * svc = cons->tree.services.hd;
            svc != NULL; svc = svc->next )
        lc_add_completion( state, svc->svc.val, svc->svc.len );
    }
    else if ( type == PARM_ARG ) {
      for ( ConfigTree::Parameters *p = cons->tree.parameters.hd;
            p != NULL; p = p->next ) {
        for ( ConfigTree::StringPair *sp = p->list.hd; sp != NULL;
              sp = sp->next ) {
          lc_add_completion( state, sp->name.val, sp->name.len );
        }
      }
    }
  }
  return 0;
}

void
console_help( struct Term_s *t ) noexcept
{
  const ConsoleCmdString * cmds, * help;
  size_t ncmds, nhelp;
  Term * term = (Term *) t;
  int    arg_num,   /* which arg is completed, 0 = first */
         arg_count, /* how many args */
         arg_off[ 32 ],  /* offset of args */
         arg_len[ 32 ];  /* length of args */
  char   buf[ 1024 ];
  CmdMask cmd_mask;
  Console * cons = (Console *) term->closure;

  int n = lc_tty_get_completion_cmd( term->tty, buf, sizeof( buf ),
                                     &arg_num, &arg_count, arg_off,
                                     arg_len, 32 );
  if ( n < 0 )
    return;
  cons->get_valid_cmds( cmds, ncmds );
  if ( arg_count > 0 && arg_len[ 0 ] > 0 ) {
    ConsoleCmd cmd;
    cmd = (ConsoleCmd)
      Console::which_cmd( cmds, ncmds, &buf[ arg_off[ 0 ] ],
                          arg_len[ 0 ], &cmd_mask );
    if ( cmd == CMD_SHOW ) {
      if ( arg_count > 1 )
        cmd = which_show( &buf[ arg_off[ 1 ] ], arg_len[ 1 ], &cmd_mask );
      if ( cmd == CMD_SHOW || cmd_mask.count() == 0 ) {
        cmd_mask.zero();
        for ( size_t i = 0; i < num_show_cmds; i++ )
          cmd_mask.add( show_cmd[ i ].cmd );
      }
    }
    else if ( cmd == CMD_CONFIGURE ) {
      if ( arg_count > 1 )
        cmd = which_config( &buf[ arg_off[ 1 ] ], arg_len[ 1 ], &cmd_mask );
    }
  }
  /*Console * cons = (Console *) term->closure;*/
  cons->get_valid_help_cmds( help, nhelp );
  for ( size_t i = 0; i < nhelp; i++ ) {
    if ( cmd_mask.count() == 0 || cmd_mask.is_member( help[ i ].cmd ) ) {
      const char * s = help[ i ].str;
      for (;;) {
        const char * e = (const char *) ::memchr( s, '\n', ::strlen( s ) );
        if ( e != NULL ) {
          lc_add_completion( term->lc, s, ( e - s ) );
          s = &e[ 1 ];
        }
        else {
          lc_add_completion( term->lc, s, ::strlen( s ) );
          break;
        }
      }
    }
  }
}

void
Console::output_help( ConsoleOutput *, int c ) noexcept
{
  const ConsoleCmdString * help;
  size_t nhelp;
  size_t i = 0;
  this->get_valid_help_cmds( help, nhelp );
  for ( i = 0; i < nhelp; i++ ) {
    if ( c != help[ i ].cmd && c != CMD_BAD )
      continue;
    static const int width = 28;
    const char * s = help[ i ].str,
               * a = help[ i ].args,
               * d = help[ i ].descr;
    int len = ::strlen( s ) + 1 + ::strlen( a ) + 1;
    this->printf( "%s %s ", s, a );
    if ( width > len )
      this->printf( "%*s", width - len, "" );
    for (;;) {
      const char * e = (const char *) ::memchr( d, '\n', ::strlen( d ) );
      if ( e != NULL ) {
        this->printf( "%.*s\n", (int) ( e - d ), d );
        d = &e[ 1 ];
      }
      else {
        this->printf( "%s\n", d );
        break;
      }
    }
  }
}

void
Console::tab_connection( const char *proto,  const char *remote,  uint32_t rsz,  
                         const char *local,  uint32_t lsz,
                         const UserBridge &n,  TabPrint &pr ) noexcept
{
  size_t psz = ::strlen( proto ),
         dig = uint32_digits( n.uid ),
         nsz = n.peer.user.len,
         sz  = psz + rsz + lsz + dig + nsz + 7, i = 0;
  char * str = this->tmp.str_make( sz );

  ::memcpy( str, proto, psz );                 i += psz;
  ::memcpy( &str[ i ], local, lsz );           i += lsz;
  ::memcpy( &str[ i ], " -> ", 4 );            i += 4;
  ::memcpy( &str[ i ], n.peer.user.val, nsz ); i += nsz;
  str[ i++ ] = '.';
  uint32_to_string( n.uid, &str[ i ], dig );   i += dig;
  str[ i++ ] = '@';
  ::memcpy( &str[ i ], remote, rsz );          i += rsz;
  str[ i ] = '\0';
  pr.set( str, (uint32_t) i );
}

void
Console::tab_url( const char *proto, const char *addr, uint32_t addrlen,
                  TabPrint &pr ) noexcept
{
  size_t psz = ::strlen( proto ),
         sz  = psz + addrlen + 4, i = 0;
  char * str = this->tmp.str_make( sz );

  ::memcpy( str, proto, psz );          i += psz;
  ::memcpy( &str[ i ], "://", 3 );      i += 3;
  ::memcpy( &str[ i ], addr, addrlen ); i += addrlen;
  str[ i ] = '\0';
  pr.set( str, (uint32_t) i );
}

void
Console::tab_user_id( uint32_t uid,  TabPrint &pr ) noexcept
{
  if ( uid == MY_UID ) {
    pr.set( this->user_db.user.user, PRINT_SELF );
  }
  else {
    UserBridge *n = NULL;
    if ( uid < this->user_db.next_uid )
      n = this->user_db.bridge_tab[ uid ];
    if ( n != NULL )
      pr.set( n->peer.user, uid, PRINT_ID );
    else
      pr.set_null();
  }
}

void
Console::tab_concat( const char *s,  size_t sz1,  const char *s2,
                     TabPrint &pr ) noexcept
{
  size_t sz2 = ::strlen( s2 ),
         sz  = sz1 + sz2 + 4;
  char * str = this->tmp.str_make( sz );
  str[ 0 ] = '(';
  ::memcpy( &str[ 1 ], s2, sz2 );
  str[ sz2 + 1 ] = ')';
  str[ sz2 + 2 ] = ' ';
  ::memcpy( &str[ sz2 + 3 ], s, sz1 );
  str[ sz1 + sz2 + 3 ] = '\0';
  pr.set( str, (uint32_t) ( sz1 + sz2 + 3 ) );
}

void
Console::tab_concat( const char *s,  const char *s2,  TabPrint &pr ) noexcept
{
  return this->tab_concat( s, ::strlen( s ), s2, pr );
}

void
Console::tab_string( const char *buf,  size_t len,  TabPrint &pr ) noexcept
{
  char * str = this->tmp.str_make( len + 1 );
  ::memcpy( str, buf, len );
  str[ len ] = '\0';
  pr.set( str, (uint32_t) len );
}

void
Console::tab_string( const char *buf,  TabPrint &pr ) noexcept
{
  this->tab_string( buf, ::strlen( buf ), pr );
}

void
Console::tab_nonce( const Nonce &nonce,  TabPrint &pr ) noexcept
{
  char * str = this->tmp.str_make( NONCE_B64_LEN + 1 );
  nonce.to_base64_str( str );
  pr.set( str, NONCE_B64_LEN );
}

static int
latency_string( int64_t lat,  char *buf ) noexcept
{
  const char * units = "us";
  while ( lat >= 1000000 || lat <= -1000000 ) {
    lat /= 1000;
    if ( units[ 0 ] == 'u' )
      units = "ms";
    else {
      units = "se";
      if ( lat >= 1000000 || lat <= -1000000 ) {
        lat /= 60;
        units = "mi";
        if ( lat >= 1000000 || lat <= -1000000 ) {
          lat /= 60;
          units = "hr";
          if ( lat >= 1000000 || lat <= -1000000 ) {
            lat /= 24;
            units = "da";
          }
        }
      }
      break;
    }
  }
  return ::snprintf( buf, 80, "%.3g%s", (double) lat / 1000.0, units );
}

static size_t
uint64_to_bits( uint64_t *b,  uint32_t cnt,  char *buf ) noexcept
{
  size_t sz = 0;
  for ( uint32_t i = 0; i < cnt; i++ ) {
    for ( uint32_t n = 0; n < 64; n++ ) {
      uint64_t mask = (uint64_t) 1 << n;

      if ( ( b[ i ] & mask ) != 0 ) {
        uint32_t x = i * 64 + n,
                 d = uint32_digits( x );
        if ( sz + 1 + d < 80 ) {
          if ( sz > 0 ) {
            if ( buf != NULL ) buf[ sz ] = ',';
            sz++;
          }
          if ( buf != NULL )
            uint32_to_string( x, &buf[ sz ], d );
          sz += d;
        }
      }
      else if ( b[ i ] < mask )
        break;
    }
  }
  if ( buf != NULL )
    buf[ sz ] = '\0';
  return sz;
}

static size_t
uint64_to_percent( uint64_t b,  char *buf ) noexcept
{
  size_t sz;
  if ( b < 100 ) {
    if ( buf == NULL )
      return sizeof( "0.00%" ) - 1;
    b += 100;
    sz = uint64_to_string( b, buf, 3 );
    buf[ 0 ] = '0';
  }
  else {
    uint32_t d = uint64_digits( b );
    if ( buf == NULL )
      return d + 2;
    sz = uint64_to_string( b, buf, d );
  }
  ::memmove( &buf[ sz - 1 ], &buf[ sz - 2 ], 2 );
  buf[ sz - 2 ] = '.';
  buf[ sz + 1 ] = '\0';
  return sz + 1;
}

uint32_t
TabPrint::width( Console &console,  char *buf ) noexcept
{
  UserRoute * u_ptr;
  size_t sz = 0;
  switch ( this->type() ) {
    case PRINT_STRING:
      return min_int<uint32_t>( this->len, 79 );

    case PRINT_SELF:
      return min_int<uint32_t>( this->len + 2, 79 );

    case PRINT_ID:
      if ( this->len != 0xffffffffU ) {
        return (uint32_t) min_int<size_t>( ::strlen( this->val ) + 1 +
                                           uint32_digits( this->len ), 79 );
      }
      return (uint32_t) min_int<size_t>( ::strlen( this->val ), 79 );

    case PRINT_USER:
      return (uint32_t) min_int<size_t>( this->n->peer.user.len + 1 +
                                         uint32_digits( this->n->uid ), 79 );
    case PRINT_ADDR:
    case PRINT_UADDR:
      if ( this->len == 0 )
        return 0;
      if ( this->type() == PRINT_UADDR ) {
        sz = this->n->peer.user.len + uint32_digits( this->n->uid ) + 2;
      }
      if ( this->pre != NULL )
        sz += ::strlen( this->pre ) + 3;
      sz += this->len;
      return (uint32_t) min_int<size_t>( sz, 79 );
    case PRINT_TPORT:
      if ( this->len == 0 )
        return 0;
      sz += this->len;
      if ( this->pre != NULL )
        sz += ::strlen( this->pre ) + 1;
      return (uint32_t) min_int<size_t>( sz, 79 );
    case PRINT_LATENCY: {
      if ( this->ival == 0 )
        return 0;
      return latency_string( (int64_t) this->ival, buf );
    }
    case PRINT_NONCE:
      if ( this->n == NULL )
        return 0;
      return NONCE_B64_LEN;

    case PRINT_DIST:
      if ( this->n == NULL )
        return 0;
      u_ptr = n->primary( console.user_db );
      this->len = console.user_db.peer_dist.calc_transport_cache( this->n->uid,
                                                       u_ptr->rte.tport_id, 0 );
      if ( this->len == COST_MAXIMUM )
        return 1;
      /* fall through */
    case PRINT_INT:
      return (uint32_t) uint32_digits( this->len );
    case PRINT_SINT:
      return (uint32_t) int32_digits( (int32_t) this->len );
    case PRINT_SLONG:
      return (uint32_t) int64_digits( (int64_t) this->ival );
    case PRINT_SHORT_HEX:
      return 4 + 2;
    case PRINT_LONG_HEX:
      if ( this->ival <= (uint64_t) 0xffffffffU ) {
        if ( this->ival <= (uint64_t) 0xffffU ) {
          if ( this->ival == 0 )
            return 1;
          return 4 + 2;
        }
        return 8 + 2;
      }
      return 8 + 8 + 2;
    case PRINT_STATE: {
      char buf[ 5 * 10 + 1 ];
      return (uint32_t) ::strlen( user_state_abrev( this->len &
                                     ( INBOX_ROUTE_STATE |
                                       UCAST_URL_STATE |
                                       UCAST_URL_SRC_STATE |
                                       MESH_URL_STATE |
                                       HAS_HB_STATE ), buf ) );
    }
    case PRINT_LONG:
      return (uint32_t) uint64_digits( this->ival );
    case PRINT_BITS:
      return uint64_to_bits( &this->ival, 1, NULL );
    case PRINT_PERCENT:
      return uint64_to_percent( this->ival, NULL );
    case PRINT_STAMP:
      if ( this->ival == 0 )
        return 0;
      return LastTimeStamp::TS_LEN;
    case PRINT_TPORT_STATE:
    case PRINT_SOCK_STATE:
      return kv_popcountw( this->len );
    default:
      return 0;
  }
}

static inline size_t
cat80( char *buf,  size_t off,  const char *s,  size_t len )
{
  len = min_int<size_t>( off + len, 79 );
  const char * end = &buf[ len ];
  buf = &buf[ off ];
  while ( buf < end )
    *buf++ = *s++;
  return len;
}

static inline size_t
cat80( char *buf,  size_t off,  const char *s )
{
  return cat80( buf, off, s, ::strlen( s ) );
}

static inline size_t
cat80( char *buf,  size_t off,  uint32_t i )
{
  char str[ 16 ];
  size_t sz = uint32_to_string( i, str );
  return cat80( buf, off, str, sz );
}

const char *
TabPrint::string( Console &console,  char *buf ) noexcept
{
  size_t sz = 0;
  switch ( this->type() ) {
    case PRINT_STRING:
      if ( this->len == 0 )
        return "";
      if ( this->len > 79 || ! this->null_term() ) {
        sz = cat80( buf, 0, this->val, this->len );
        buf[ sz ] = '\0';
        return buf;
      }
      return this->val;
    case PRINT_SELF:
      sz = cat80( buf, 0, this->val, this->len );
      sz = cat80( buf, sz, ".*" );
      buf[ sz ] = '\0';
      return buf;
    case PRINT_ID:
      sz = cat80( buf, 0, this->val, ::strlen( this->val ) );
      if ( this->len != 0xffffffffU ) {
        sz = cat80( buf, sz, "." );
        sz = cat80( buf, sz, this->len );
      }
      buf[ sz ] = '\0';
      return buf;
    case PRINT_USER:
      sz = cat80( buf, 0, this->n->peer.user.val );
      sz = cat80( buf, sz, "." );
      sz = cat80( buf, sz, this->n->uid );
      buf[ sz ] = '\0';
      return buf;
    case PRINT_ADDR:
    case PRINT_UADDR:
      if ( this->len == 0 )
        return "";
      if ( this->type() == PRINT_UADDR ) {
        sz = cat80( buf, 0, this->n->peer.user.val );
        sz = cat80( buf, sz, "." );
        sz = cat80( buf, sz, this->n->uid );
        sz = cat80( buf, sz, "@" );
      }
      if ( this->pre != NULL ) {
        sz = cat80( buf, sz, this->pre );
        sz = cat80( buf, sz, "://" );
      }
      sz = cat80( buf, sz, this->val, this->len );
      buf[ sz ] = '\0';
      return buf;
    case PRINT_TPORT:
      if ( this->len == 0 )
        return "";
      sz = cat80( buf, sz, this->val, this->len );
      if ( this->pre != NULL ) {
        sz = cat80( buf, sz, "." );
        sz = cat80( buf, sz, this->pre );
      }
      buf[ sz ] = '\0';
      return buf;
    case PRINT_LATENCY: {
      if ( this->ival == 0 )
        return "";
      latency_string( (int64_t) this->ival, buf );
      return buf;
    }
    case PRINT_NONCE:
      if ( this->n == NULL )
        return "";
      this->n->bridge_id.nonce.to_base64_str( buf );
      return buf;
    case PRINT_DIST:
      if ( this->n == NULL )
        return "";
      if ( this->len == COST_MAXIMUM )
        return "X";
      /* fall through */
    case PRINT_INT:
      sz = uint32_to_string( this->len, buf );
      buf[ sz ] = '\0';
      return buf;
    case PRINT_SINT:
      sz = int32_to_string( (int32_t) this->len, buf );
      buf[ sz ] = '\0';
      return buf;
    case PRINT_SLONG:
      sz = int64_to_string( (int64_t) this->ival, buf );
      buf[ sz ] = '\0';
      return buf;
    case PRINT_SHORT_HEX:
      buf[ 0 ] = '0';
      buf[ 1 ] = 'x';
      hexstr16( this->len, &buf[ 2 ] );
      buf[ 2 + 4 ] = '\0';
      return buf;
    case PRINT_LONG_HEX:
      buf[ 0 ] = '0';
      buf[ 1 ] = 'x';
      if ( this->ival <= (uint64_t) 0xffffffffU ) {
        if ( this->ival <= (uint64_t) 0xffffU ) {
          if ( this->ival == 0 ) {
            buf[ 1 ] = '\0';
            return buf;
          }
          hexstr16( (uint16_t) this->ival, &buf[ 2 ] );
          buf[ 4 + 2 ] = '\0';
          return buf;
        }
        hexstr32( (uint32_t) this->ival, &buf[ 2 ] );
        buf[ 8 + 2 ] = '\0';
        return buf;
      }
      hexstr64( this->ival, &buf[ 2 ] );
      buf[ 2 + 8 + 8 ] = '\0';
      return buf;
    case PRINT_STATE:
      return user_state_abrev( this->len &
                               ( INBOX_ROUTE_STATE |
                                 UCAST_URL_STATE |
                                 UCAST_URL_SRC_STATE |
                                 MESH_URL_STATE |
                                 HAS_HB_STATE ), buf );
    case PRINT_LONG:
      sz = uint64_to_string( this->ival, buf );
      buf[ sz ] = '\0';
      return buf;
    case PRINT_BITS:
      sz = uint64_to_bits( &this->ival, 1, buf );
      buf[ sz ] = '\0';
      return buf;
    case PRINT_PERCENT:
      sz = uint64_to_percent( this->ival, buf );
      buf[ sz ] = '\0';
      return buf;
    case PRINT_STAMP: {
      if ( this->ival == 0 )
        return "";
      uint64_t stamp = this->ival + tz_stamp_ns;
      console.stamp_ts.update( stamp );
      ::memcpy( buf, console.stamp_ts.ts, LastTimeStamp::TS_LEN );
      buf[ LastTimeStamp::TS_LEN ] = '\0';
      return buf;
    }
    case PRINT_TPORT_STATE: {
      uint32_t j = 0;
      if ( this->len & TPORT_IS_LISTEN     ) buf[ j++ ] = 'L';
      if ( this->len & TPORT_IS_MCAST      ) buf[ j++ ] = 'M';
      if ( this->len & TPORT_IS_MESH       ) buf[ j++ ] = 'X';
      if ( this->len & TPORT_IS_CONNECT    ) buf[ j++ ] = 'C';
      if ( this->len & TPORT_IS_TCP        ) buf[ j++ ] = 'T';
      if ( this->len & TPORT_IS_EDGE       ) buf[ j++ ] = 'E';
      if ( this->len & TPORT_IS_IPC        ) buf[ j++ ] = 'I';
      if ( this->len & TPORT_IS_SHUTDOWN   ) buf[ j++ ] = '-';
      if ( this->len & TPORT_IS_DEVICE     ) buf[ j++ ] = 'D';
      if ( this->len & TPORT_IS_INPROGRESS ) buf[ j++ ] = '*';
      buf[ j ] = '\0';
      return buf;
    }
    case PRINT_SOCK_STATE: {
      uint32_t j = 0;
      if ( this->len & ( 1 << EV_READ_HI    ) ) buf[ j++ ] = 'R';
      if ( this->len & ( 1 << EV_CLOSE      ) ) buf[ j++ ] = 'C';
      if ( this->len & ( 1 << EV_WRITE_POLL ) ) buf[ j++ ] = '>';
      if ( this->len & ( 1 << EV_WRITE_HI   ) ) buf[ j++ ] = 'W';
      if ( this->len & ( 1 << EV_READ       ) ) buf[ j++ ] = 'r';
      if ( this->len & ( 1 << EV_PROCESS    ) ) buf[ j++ ] = '+';
      if ( this->len & ( 1 << EV_PREFETCH   ) ) buf[ j++ ] = 'f';
      if ( this->len & ( 1 << EV_WRITE      ) ) buf[ j++ ] = 'w';
      if ( this->len & ( 1 << EV_SHUTDOWN   ) ) buf[ j++ ] = 'x';
      if ( this->len & ( 1 << EV_READ_LO    ) ) buf[ j++ ] = '<';
      if ( this->len & ( 1 << EV_BUSY_POLL  ) ) buf[ j++ ] = 'z';
      buf[ j ] = '\0';
      return buf;
    }
    default:
      return "";
  }
}

void
Console::print_dashes( const uint32_t *width,  uint32_t ncols ) noexcept
{
  for ( uint32_t j = 0; j < ncols; j++ ) {
    char dashes[ 84 ];
    ::memset( dashes, '-', width[ j ] + 3 );
    if ( j < ncols - 1 ) {
      dashes[ width[ j ] + 1 ] = '+';
      dashes[ width[ j ] + 3 ] = '\0';
    }
    else {
      dashes[ width[ j ] + 0 ] = '\n';
      dashes[ width[ j ] + 1 ] = '\0';
    }
    this->puts( dashes );
  }
}

static const char *
hdr_full( const char *h )
{
  static const char *abrev[] = {
    "ac",   "accept",
    "bs",   "bytes sent",
    "br",   "bytes recv",
    "fl",   "flags",
    "lat",  "latency",
    "link", "link seqno",
    "sub",  "sub seqno",
    "ms",   "msgs sent",
    "mr",   "msgs recv",
    "rq",   "recv queue",
    "wq",   "send queue",
  };
  static uint8_t ht[ 256 ];
  static int init = 0;
  uint32_t k;
  uint8_t j;
  if ( ! init ) {
    static const uint32_t abrev_cnt = sizeof( abrev ) / sizeof( abrev[ 0 ] );
    for ( k = 0; k < abrev_cnt; k += 2 ) {
      j = (uint8_t) abrev[ k ][ 0 ] ^ (uint8_t) abrev[ k ][ 1 ];
      while ( ht[ j ] != 0 )
        j++;
      ht[ j ] = (uint8_t) ( k / 2 + 1 );
    }
    init = 1;
  }
  j = (uint8_t) h[ 0 ] ^ (uint8_t) h[ 1 ];
  for ( ; ht[ j ] != 0; j++ ) {
    k = (uint32_t) ( ht[ j ] - 1 ) * 2;
    if ( ::strcmp( h, abrev[ k ] ) == 0 )
      return abrev[ k + 1 ];
  }
  return h;
}

void
Console::print_table( ConsoleOutput *p,  const char **hdr,
                      uint32_t ncols ) noexcept
{
  const bool is_html = ( p != NULL && p->is_html );
  const bool is_json = ( p != NULL && p->is_json );
  uint32_t     i, j,
               tabsz = (uint32_t) this->table.count;
  TabPrint   * tab   = this->table.ptr;
  uint32_t   * width, wbuf[ 16 ];
  char         buf[ 80 ];
  const char * v, * fmt;

  if ( is_html ) {
    this->puts( "<table><thead><tr>" );
    for ( j = 0; j < ncols; j++ ) {
      this->printf( "<th>%s</th>", hdr_full( hdr[ j ] ) );
    }
    this->puts( "</tr></thead><tbody>" );
    for ( i = 0; i < tabsz; i += ncols ) {
      this->puts( "<tr>" );
      for ( j = 0; j < ncols; j++ ) {
        this->puts( "<td>" );
        if ( tab[ i + j ].type() == PRINT_DIST )
          tab[ i + j ].width( *this, buf );
        this->puts( tab[ i + j ].string( *this, buf ) );
        this->puts( "</td>" );
      }
      this->puts( "</tr>" );
    }
    this->puts( "</tbody></table>\n" );
    return;
  }
  if ( is_json ) {
    this->putchar( '[' );
    for ( i = 0; i < tabsz; i += ncols ) {
      this->putchar( '{' );
      for ( j = 0; j < ncols; j++ ) {
        this->putchar( '\"' );
        this->puts( hdr[ j ] );
        this->putchar( '\"' );
        this->putchar( ':' );
        switch ( tab[ i + j ].typ ) {
          case PRINT_INT:
          case PRINT_SINT:
          case PRINT_LONG:
            this->puts( tab[ i + j ].string( *this, buf ) );
            break;
          default:
            if ( tab[ i + j ].type() == PRINT_DIST )
              tab[ i + j ].width( *this, buf );
            this->putchar( '\"' );
            this->puts( tab[ i + j ].string( *this, buf ) );
            this->putchar( '\"' );
            break;
        }
        if ( j < ncols - 1 ) {
          this->putchar( ',' ); this->putchar( ' ' );
        }
      }
      this->putchar( '}' );
      if ( i < tabsz - ncols ) {
        this->putchar( ',' ); this->putchar( '\n' );
      }
    }
    this->putchar( ']' );
    this->putchar( '\n' );
    return;
  }
  if ( ncols <= 16 ) {
    width = wbuf;
    ::memset( wbuf, 0, sizeof( wbuf ) );
  }
  else
    width = (uint32_t *) ::malloc( sizeof( uint32_t ) * ncols );
  for ( j = 0; j < ncols; j++ ) {
    width[ j ] = (uint32_t) ::strlen( hdr[ j ] );
  }
  for ( i = 0; i < tabsz; i += ncols ) {
    for ( j = 0; j < ncols; j++ ) {
      uint32_t w = tab[ i + j ].width( *this, buf );
      width[ j ] = max_int( width[ j ], w );
    }
  }
  for ( j = 0; j < ncols; j++ ) {
    uint32_t len = (uint32_t) ::strlen( hdr[ j ] );
    if ( width[ j ] > len ) {
      uint32_t back = len / 2,        front = len - back,
               wb   = width[ j ] / 2, wf    = width[ j ] - wb;
      front += wf;
      this->printf( "%*s%*s", front, hdr[ j ], width[ j ] - front, "" );
    }
    else {
      this->puts( hdr[ j ] );
    }
    this->puts( ( j < ncols - 1 ) ? " | " : "\n" );
  }
  this->print_dashes( width, ncols );

  for ( i = 0; i < tabsz; i += ncols ) {
    bool overflow = false;
    for ( j = 0; j < ncols; j++ ) {
      v   = tab[ i + j ].string( *this, buf );
      fmt = ( tab[ i + j ].left() ? "%-*s%s" : "%*s%s" );
      this->printf( fmt, width[ j ], v,
                    ( j < ncols - 1 ) ? " | " : "\n" );
      if ( tab[ i + j ].type() == PRINT_STRING &&
           tab[ i + j ].len > width[ j ] )
        overflow = true;
    }
    if ( overflow ) {
      uint32_t n = 1;
      do {
        overflow = false;
        for ( j = 0; j < ncols; j++ ) {
          v = "";
          if ( tab[ i + j ].type() == PRINT_STRING &&
               tab[ i + j ].len > n * width[ j ] ) {
            v = &tab[ i + j ].val[ n * width[ j ] ];
            if ( tab[ i + j ].len > ( n + 1 ) * width[ j ] ) {
              overflow = true;
              ::memcpy( buf, v, width[ j ] );
              buf[ width[ j ] ] = '\0';
              v = buf;
            }
          }
          fmt = ( tab[ i + j ].left() ? "%-*s%s" : "%*s%s" );
          this->printf( fmt, width[ j ], v,
                        ( j < ncols - 1 ) ? " | " : "\n" );
        }
        n++;
      } while ( overflow );
    }
    if ( tab[ i + ncols - 1 ].separator() )
      this->print_dashes( width, ncols );
  }
  if ( width != wbuf )
    ::free( width );
}

static uint32_t
int_arg( const char *arg,  size_t len ) noexcept
{
  uint32_t i = 0;
  while ( len > 0 && *arg >= '0' && *arg <= '9' ) {
    i = i * 10 + (uint32_t) ( *arg - '0' );
    len--; arg++;
  }
  return i;
}

UserBridge *
Console::find_user( const char *name,  size_t len ) const noexcept
{
  uint32_t uid;
  if ( len == 1 && name[ 0 ] == '*' )
    len = 0;
  if ( len > 0 ) {
    for ( uid = 1; uid < this->user_db.next_uid; uid++ ) {
      UserBridge * n = this->user_db.bridge_tab[ uid ];
      if ( n != NULL && n->is_set( AUTHENTICATED_STATE ) ) {
        if ( n->peer.user.equals( name, len ) )
          return n;
      }
    }
  }
  return NULL;
}

bool
Console::is_self( const char *name,  size_t len ) const noexcept
{
  return this->user_db.user.user.equals( name, len ) ||
         ( len == 4 && ::memcmp( name, "self", 4 ) == 0 );
}

UserBridge *
Console::find_uid( const char *name,  size_t len ) noexcept
{
  if ( len > 0 && name[ 0 ] >= '0' && name[ 0 ] <= '9' ) {
    uint32_t uid = int_arg( name, len );
    if ( uid > 0 && uid < this->user_db.next_uid ) {
      UserBridge * n = this->user_db.bridge_tab[ uid ];
      if ( n != NULL && n->is_set( AUTHENTICATED_STATE ) )
        return n;
    }
  }
  return NULL;
}

bool
Console::on_input( ConsoleOutput *p,  const char *buf,
                   size_t buflen ) noexcept
{
  ConsoleCmd cmd = CMD_BAD;
  if ( 0 ) {
  help:;
    if ( p != NULL && p->is_json )
      return true;
    this->output_help( p, cmd );
    return this->flush_output( p );
  }
  const char    * args[ MAXARGS + 3 ]; /* all args */
  size_t          arglen[ MAXARGS + 3 ], argc;
  const char    * arg;   /* arg after command */
  size_t          len;   /* len of arg */
  ConsoleOutput * sub_output = p;
  uint32_t        count;

  cmd = (ConsoleCmd)
    this->parse_command( buf, &buf[ buflen ], arg, len, args, arglen, argc );
  /* empty line, skip it */
  if ( cmd == CMD_EMPTY )
    return this->flush_output( p );
  args[ argc ]   = args[ argc + 1 ]   = args[ argc + 2 ]   = NULL;
  arglen[ argc ] = arglen[ argc + 1 ] = arglen[ argc + 2 ] = 0;

  if ( cmd >= CMD_SUB_START && argc == 3 ) {
    switch ( cmd ) {
      case CMD_SUB_START:
      case CMD_PSUB_START:
      case CMD_GSUB_START:
      case CMD_SNAP:
        sub_output = this->json_files.open( args[ 2 ], arglen[ 2 ] );
        if ( sub_output == NULL ) {
          this->outf( p, "output (%.*s) file open error %d",
                        (int) arglen[ 2 ], args[ 2 ], errno );
          return true;
        }
        break;
      case CMD_SUB_STOP:
      case CMD_PSUB_STOP:
      case CMD_GSUB_STOP:
        sub_output = this->json_files.find( args[ 2 ], arglen[ 2 ] );
        if ( sub_output == NULL ) {
          this->outf( p, "output (%.*s) not found",
                        (int) arglen[ 2 ], args[ 2 ] );
          return true;
        }
        break;
      default:
        break;
    }
  }
  switch ( cmd ) {
    default:
      if ( cmd >= (ConsoleCmd) CMD_TPORT_BASE && cmd < CMD_TPORT_SHOW ) {
        if ( this->cfg_tport != NULL ) {
          if ( this->config_transport_param( cmd, args, arglen, argc ) )
            break;
        }
      }
      goto help;

    case CMD_SHOW_TIME:    this->show_time( p ); break;
    case CMD_SHOW_VERSION: this->show_version( p ); break;

    case CMD_DIE: {
      int status = 1;
      if ( len > 0 )
        status = (int) int_arg( arg, len );
      ::exit( status );
    }
    case CMD_SLEEP: {
      int secs = 60;
      if ( len > 0 )
        secs = (int) int_arg( arg, len );
      ::sleep( secs );
      break;
    }
    case CMD_QUIT: {
      bool b = this->flush_output( p );
      p->on_quit();
      return b;
    }
    case CMD_WEVENTS: {
      if ( len > 0 )
        sub_output = this->json_files.open( arg, len );
      if ( sub_output != NULL ) {
        this->show_events( sub_output );
        if ( len > 0 ) {
          this->flush_output( sub_output );
          sub_output->on_remove();
        }
      }
      break;
    }
    case CMD_FDCLOSE: {
      EvPoll & poll = this->mgr.poll;
      EvSocket *s;
      uint32_t n;
      if ( (n = int_arg( arg, len )) != 0 && n <= poll.maxfd &&
           (s = poll.sock[ n ]) != NULL ) {
        s->idle_push( EV_CLOSE );
      }
      else {
        this->outf( p, "fd (%.*s) not a socket", (int) len, arg );
      }
      break;
    }
    case CMD_HBSKIP : {
      uint32_t n;
      TransportRoute * rte;
      if ( (n = int_arg( arg, len )) != 0 &&
            n < this->user_db.transport_tab.count &&
            (rte = this->user_db.transport_tab.ptr[ n ]) != NULL ) {
        if ( (n = int_arg( args[ 2 ], arglen[ 2 ] )) == 0 )
          n = 3;
        rte->skip_hb = n;
      }
      else {
        this->outf( p, "n (%.*s) not a transport", (int) len, arg );
      }
      break;
    }
    case CMD_UIDREMOVE: {
      UserBridge * u = this->find_user( arg, len );
      if ( u == NULL )
        u = this->find_uid( arg, len );
      if ( u == NULL )
        this->outf( p, "no user \"%.*s\"", (int) len, arg );
      else
        this->user_db.remove_authenticated( *u, BYE_CONSOLE );
      break;
    }
    case CMD_RESEED: {
      this->sub_db.reseed_bloom();
      this->outf( p, "bloom seed 0x%x", this->sub_db.bloom.bits->seed );
      break;
    }
    case CMD_CONNECT:  this->connect( arg, len ); break;
    case CMD_LISTEN:   this->listen( arg, len ); break;
    case CMD_SHUTDOWN: this->shutdown( arg, len ); break;
    case CMD_NETWORK: {
      if ( ! this->mgr.add_network( args[ 2 ], arglen[ 2 ], arg, len, true ) )
        goto help;
      break;
    }
    case CMD_CONFIGURE_TPORT:
      if ( ! this->config_transport( args, arglen, argc ) )
        goto help;
      break;
    case CMD_SAVE:
      this->config_save();
      break;
    case CMD_TPORT_SHOW:
      this->cfg_tport->print_y( *this );
      break;
    case CMD_TPORT_QUIT:
      this->changes.add( this->cfg_tport );
      this->cfg_tport = NULL;
      if ( this->mute_log )
        this->change_prompt( "mute", 4 );
      else
        this->change_prompt( NULL, 0 );
      break;

    case CMD_CONFIGURE_PARAM:
      if ( len == 0 || argc < 3 )
        goto help;
      this->config_param( arg, len, args[ 3 ], arglen[ 3 ] );
      break;
    case CMD_MUTE_LOG:
      this->mute_log = true;
      this->change_prompt( "mute", 4 );
      break;
    case CMD_UNMUTE_LOG:
      this->mute_log = false;
      this->change_prompt( NULL, 0 );
      break;

    case CMD_SHOW_PEERS:     this->show_peers( p, arg, len ); break;
    case CMD_SHOW_PORTS:     this->show_ports( p, arg, len ); break;
    case CMD_SHOW_COST:      this->show_cost( p, arg, len ); break;
    case CMD_SHOW_STATUS:    this->show_status( p, arg, len ); break;
    case CMD_SHOW_LINKS:     this->show_links( p );     break;
    case CMD_SHOW_NODES:     this->show_nodes( p );     break;
    case CMD_SHOW_URLS:      this->show_urls( p );      break;
    case CMD_SHOW_TPORTS:    this->show_tports( p, arg, len ); break;
    case CMD_SHOW_USERS:     this->show_users( p );     break;
    case CMD_SHOW_EVENTS:    this->show_events( p );    break;
    case CMD_SHOW_UNKNOWN:   this->show_unknown( p );   break;
    case CMD_SHOW_LOGS:
      this->colorize_log( p, this->log.ptr, this->log_index );
      break;
    case CMD_SHOW_COUNTERS:  this->show_counters( p );  break;
    case CMD_SHOW_SYNC:      this->show_sync( p );  break;
    case CMD_SHOW_PUBTYPE:   this->show_pubtype( p );  break;
    case CMD_SHOW_INBOX:     this->show_inbox( p, arg, len ); break;
    case CMD_SHOW_LOSS:      this->show_loss( p );      break;
    case CMD_SHOW_SKEW:      this->show_skew( p );      break;
    case CMD_SHOW_REACHABLE: this->show_reachable( p ); break;

    case CMD_SHOW_ADJACENCY:
    case CMD_SHOW_ROUTES:
    case CMD_SHOW_TREE: {
      UserBridge * u = this->find_user( arg, len );
      uint32_t     i = (uint32_t) -1,
                 * uid_ptr  = NULL,
                   self_uid = 0;
      bool         has_path = true;
      if ( u != NULL )
        uid_ptr = &u->uid;
      if ( uid_ptr == NULL ) {
        if ( this->is_self( arg, len ) )
          uid_ptr = &self_uid;
        else if ( len > 0 )
          i = int_arg( arg, len );
      }
      if ( uid_ptr != NULL && argc > 2 )
        i = int_arg( args[ argc - 1 ], arglen[ argc - 1 ] );
      if ( i == (uint32_t) -1 ) {
        i = 0;
        has_path = false;
      }
      switch ( cmd ) {
        case CMD_SHOW_TREE:      this->show_tree( p, u, i );   break;
        case CMD_SHOW_ROUTES:    this->show_routes( p, u, i ); break;
        case CMD_SHOW_ADJACENCY: this->show_adjacency( p, uid_ptr, i, has_path ); break;
        default: break;
      }
      break;
    }
    case CMD_SHOW_PATH:      this->show_path( p, int_arg( arg, len ) ); break;
    case CMD_SHOW_FORWARD:
      if ( len > 0 && arg[ 0 ] == 'a' )
        this->show_forward_all( p );
      else
        this->show_forward( p, int_arg( arg, len ) );
      break;
    case CMD_SHOW_FDS:       this->show_fds( p );       break;
    case CMD_SHOW_BUFFERS:   this->show_buffers( p );   break;
    case CMD_SHOW_WINDOWS:   this->show_windows( p );   break;
    case CMD_SHOW_BLOOMS: {
      bool brief = argc > 1 && args[ argc - 1 ][ 0 ] == 'b';
      this->show_blooms( p, int_arg( arg, len ), brief );
      break;
    }
    case CMD_SHOW_MATCH:     this->show_match( p, arg, len ); break;

    case CMD_SHOW_RUN:        case CMD_SHOW_START:
      this->show_config( p, cmd>=CMD_SHOW_START, PRINT_NORMAL, arg, len ); break;
    case CMD_SHOW_RUN_TPORTS: case CMD_SHOW_START_TPORTS:
      this->show_config( p, cmd>=CMD_SHOW_START, PRINT_TRANSPORTS, arg, len ); break;
    case CMD_SHOW_RUN_SVCS:   case CMD_SHOW_START_SVCS:
      this->show_config( p, cmd>=CMD_SHOW_START, PRINT_SERVICES, arg, len ); break;
    case CMD_SHOW_RUN_USERS:  case CMD_SHOW_START_USERS:
      this->show_config( p, cmd>=CMD_SHOW_START, PRINT_USERS, arg, len ); break;
    case CMD_SHOW_RUN_GROUPS: case CMD_SHOW_START_GROUPS:
      this->show_config( p, cmd>=CMD_SHOW_START, PRINT_GROUPS, arg, len ); break;
    case CMD_SHOW_RUN_PARAM:  case CMD_SHOW_START_PARAM:
      this->show_config( p, cmd>=CMD_SHOW_START, PRINT_PARAMETERS, arg, len ); break;

    case CMD_SHOW_GRAPH:     this->show_graph( p ); break;
    case CMD_SHOW_CACHE:     this->show_cache( p ); break;
    case CMD_SHOW_POLL:      this->show_poll( p ); break;
    case CMD_SHOW_HOSTS:     this->show_hosts( p ); break;
    case CMD_SHOW_RVSUB:     this->show_rvsub( p ); break;
    case CMD_SHOW_RPCS:      this->show_rpcs( p ); break;

    case CMD_DEBUG: {
      int    dist_dbg = 0, check_blooms = 0;
      char   buf[ 80 ];
      size_t sz = 0;
      if ( len == 0 )
        goto help;
      parse_debug_flags( arg, len, dist_dbg, check_blooms );
      for ( size_t i = 0; i < debug_str_count; i++ ) {
        if ( ( dbg_flags & ( 1 << i ) ) != 0 ) {
          if ( sz > 0 )
            sz = cat80( buf, sz, "," );
          sz = cat80( buf, sz, debug_str[ i ] );
        }
      }
      if ( sz > 0 ) {
        buf[ sz ] = '\0';
        this->outf( p, "debug flags set to 0x%x (%s)", dbg_flags, buf );
      }
      else {
        this->outf( p, "debug flags cleared" );
      }
      if ( dist_dbg ) {
        this->user_db.peer_dist.invalidate( INVALID_NONE, 0 );
        this->outf( p, "recalculate peer dist" );
      }
      if ( check_blooms ) {
        bool b = this->user_db.check_blooms();
        this->outf( p, "check blooms = %s", b ? "ok" : "fail" );
      }
      if ( kv_pub_debug )
        this->outf( p, "kv pub debug on" );
      if ( kv_ps_debug )
        this->outf( p, "kv ps debug on" );
      if ( sassrv::rv_debug )
        this->outf( p, "rv debug on" );
      if ( natsmd::nats_debug )
        this->outf( p, "nats debug on" );
      if ( no_tcp_aes )
        this->outf( p, "disable tcp aes" );
      break;
    }
    case CMD_CANCEL: {
      uint32_t pcount = 0, scount = 0, xcount = 0;
      for ( ConsoleRPC *rpc = this->rpc_list.hd; rpc != NULL; rpc = rpc->next ){
        if ( ! rpc->is_complete ) {
          if ( rpc->type == PING_RPC ) {
            rpc->is_complete = true;
            this->on_ping( *(ConsolePing *) rpc );
            pcount++;
          }
          else if ( rpc->type == SUBS_RPC ) {
            rpc->is_complete = true;
            this->on_subs( *(ConsoleSubs *) rpc );
            scount++;
          }
          else if ( rpc->type == SNAP_RPC ) {
            rpc->is_complete = true;
            this->snap_stop( (ConsoleSnap *) rpc );
            xcount++;
          }
        }
      }
      if ( pcount > 0 )
        this->outf( p, "%u ping canceled", pcount );
      if ( scount > 0 )
        this->outf( p, "%u subs canceled", pcount );
      if ( xcount > 0 )
        this->outf( p, "%u snaps canceled", xcount );
      if ( pcount + scount + xcount == 0 )
        this->outf( p, "nothing to cancel" );
      break;
    }
    case CMD_SHOW_SEQNO:
      this->show_seqno( p, arg, len );
      break;
    case CMD_SHOW_SUBS:
      this->show_subs( p, arg, len, args[ 3 ], arglen[ 3 ] );
      break;
    case CMD_PING:      this->ping_peer( p, arg, len, false ); break;
    case CMD_TPING:     this->ping_peer( p, arg, len, true ); break;
    case CMD_MPING:     this->mcast_ping( p, int_arg( arg, len ), false ); break;

    case CMD_SUB_START: /* sub */
      if ( len == 0 )
        goto help; /* FALLTHRU */
    case CMD_SUB_STOP: /* unsub */
      count = this->do_sub( p, sub_output, arg, len, cmd == CMD_SUB_START );
      if ( cmd == CMD_SUB_STOP && len == 0 )
        count += this->do_psub( p, sub_output, NULL, 0, RV_PATTERN_FMT, false );
      if ( count == 0 )
        this->outf( p, "no matches (%.*s)", (int) len, arg );
      break;
    case CMD_PSUB_START: /* psub */
    case CMD_GSUB_START: /* gsub */
      if ( len == 0 )
        goto help; /* FALLTHRU */
    case CMD_PSUB_STOP:  /* pstop */
    case CMD_GSUB_STOP: {/* gstop */
      kv::PatternFmt fmt = ( cmd == CMD_PSUB_START || cmd == CMD_PSUB_STOP )
                           ? RV_PATTERN_FMT : GLOB_PATTERN_FMT;
      bool is_start = ( cmd == CMD_PSUB_START || cmd == CMD_GSUB_START );
      count = this->do_psub( p, sub_output, arg, len, fmt, is_start );
      if ( count == 0 )
        this->outf( p, "no matches (%.*s)", (int) len, arg );
      break;
    }
    case CMD_SNAP:
      if ( len == 0 )
        goto help;
      this->do_snap( p, sub_output, arg, len );
      break;

    case CMD_REMOTE:
    case CMD_RPC:
    case CMD_ANY:
    case CMD_PUBLISH:
    case CMD_TRACE:
    case CMD_PUB_ACK: { /* pub */
      if ( len == 0 )
        goto help;
      const char * data = &arg[ len ];
      size_t datalen    = &buf[ buflen ] - data;
      while ( datalen > 0 && *data == ' ' ) {
        data++;
        datalen--;
      }
      while ( datalen > 0 && data[ datalen - 1 ] <= ' ' )
        datalen--;
      if ( datalen >= 2 && data[ 0 ] == data[ datalen - 1 ] &&
           ( data[ 0 ] == '\"' || data[ 0 ] == '\'' ) ) {
        data++;
        datalen -= 2;
      }
      if ( cmd == CMD_REMOTE ) {
        if ( datalen == 0 )
          goto help;
        this->send_remote_request( p, arg, len, data, datalen );
        break;
      }
      PubMcastData mc( arg, len, data, datalen,
                       datalen > 0 ? MD_STRING : MD_NODATA, UNKNOWN_SUBJECT );
      if ( cmd != CMD_PUBLISH ) {
        if ( this->inbox_num == 0 )
          this->inbox_num = this->sub_db.inbox_start( 0, this );
        mc.reply = this->inbox_num;
        if ( cmd == CMD_TRACE )
          mc.option = CABA_OPT_TRACE;
        else if ( cmd == CMD_PUB_ACK )
          mc.option = CABA_OPT_ACK;
        else if ( cmd == CMD_ANY )
          mc.option = CABA_OPT_ANY;
        if ( cmd == CMD_TRACE || cmd == CMD_PUB_ACK )
          mc.stamp = current_realtime_ns();
      }
      this->mgr.publish( mc );
      this->outf( p, "pub(%.*s,h=0x%x,s=%u) (%.*s)",
                  (int) len, arg, mc.subj_hash, mc.path_select,
                  (int) datalen, data );
      break;
    }
  }
  return this->flush_output( p );
}

void
Console::do_snap( ConsoleOutput *p,  ConsoleOutput *sub_output,
                  const char *arg,  size_t len ) noexcept
{
  ConsoleSnap *sub = NULL;
  uint32_t h   = kv_crc_c( arg, len, 0 ),
           ibx = 0;
  char     inbox[ EvSocket::MAX_SESSION_LEN + 64 ];
  size_t   inbox_len = 0;
  uint16_t svc = SessionMgr::sub_has_rv_service( arg, len );

  if ( svc == 0 ) {
    this->outf( p, "No rv service (%.*s)", (int) len, arg );
    return;
  }
  for (;;) {
    sub = this->find_rpc<ConsoleSnap>( sub, SNAP_RPC );
    if ( sub == NULL || sub->matches( arg, len ) )
      break;
  }
  if ( sub == NULL )
    sub = this->create_rpc<ConsoleSnap>( sub_output, SNAP_RPC );
  ibx = this->start_rv_inbox( svc, sub, inbox, inbox_len );
  sub->set_snap( arg, len, h, ibx );

  PubMcastData mc( arg, len, NULL, 0, MD_NODATA, UNKNOWN_SUBJECT );
  mc.inbox = inbox;
  mc.inbox_len = inbox_len;
  this->mgr.publish( mc );
}

uint32_t
Console::do_snap_stop( ConsoleOutput *p,  ConsoleOutput *sub_output,
                       const char *arg,  size_t len ) noexcept
{
  ConsoleSnap * sub = NULL;
  for ( uint32_t count = 0; ; ) {
    for (;;) {
      sub = this->find_rpc<ConsoleSnap>( sub, SNAP_RPC );
      if ( sub == NULL || len == 0 || sub->matches( arg, len ) )
        break;
    }
    if ( sub == NULL )
      return count;
    if ( sub->out.remove( sub_output ) ) {
      count++;
      if ( sub->out.count == 0 ) {
        this->snap_stop( sub );
        this->outf( p, "done(%.*s)", (int) sub->sublen, sub->sub );
      }
    }
    if ( len != 0 )
      return count;
  }
}

void
Console::check_stat_monitor( const char *arg,  size_t len,
                             bool is_start ) noexcept
{
  static const char n_stat[] = _STAT ".";
  if ( len > sizeof( n_stat ) &&
       ::memcmp( arg, n_stat, sizeof( n_stat ) - 1 ) == 0 ) {
    if ( is_start ) {
      if ( this->mgr.stats.n_stat_sub_count++ == 0 ) {
        this->mgr.stats.m_stat_mono_time = this->mgr.poll.mono_ns;
        this->mgr.publish_stat_monitor();
      }
    }
    else {
      this->mgr.stats.n_stat_sub_count -= 1;
    }
  }
}

uint32_t
Console::do_sub( ConsoleOutput *p,  ConsoleOutput *sub_output,
                 const char *arg,  size_t len,  bool is_start ) noexcept
{
  ConsoleSubStart * sub = NULL;
  for ( uint32_t count = 0; ; ) {
    for (;;) {
      sub = this->find_rpc<ConsoleSubStart>( sub, SUB_START );
      if ( sub == NULL || len == 0 || sub->matches( arg, len ) )
        break;
    }
    if ( sub == NULL ) {
      if ( is_start ) {
        count++;
        sub = this->sub_start( sub_output, arg, len );
        this->outf( p, "start(%.*s,h=0x%x,s=%u) seqno = %" PRIu64,
                    (int) sub->sublen, sub->sub, sub->hash,
                    this->user_db.peer_dist.hash_to_path( sub->hash ),
                    sub->start_seqno );
      }
      return count;
    }
    if ( is_start ) {
      if ( sub->out.add( sub_output ) ) {
        count++;
        this->outf( p, "start(%.*s) add to existing stream",
                    (int) sub->sublen, sub->sub );
      }
      return count;
    }
    /* unsub */
    if ( sub->out.remove( sub_output ) ) {
      count++;
      if ( sub->out.count == 0 ) {
        this->sub_stop( sub );
        this->outf( p, "stop(%.*s) seqno = %" PRIu64,
                    (int) sub->sublen, sub->sub, sub->start_seqno );
      }
      else {
        this->outf( p, "stop(%.*s) remove from existing stream",
                    (int) sub->sublen, sub->sub );
      }
    }
    if ( len != 0 ) /* if no arg, unsub all */
      return count;
  }
}

uint32_t
Console::do_psub( ConsoleOutput *p,  ConsoleOutput *sub_output,
                  const char *arg,  size_t len,  kv::PatternFmt fmt,
                  bool is_start ) noexcept
{
  ConsolePSubStart * sub = NULL;
  for ( uint32_t count = 0; ; ) {
    for (;;) {
      sub = this->find_rpc<ConsolePSubStart>( sub, PSUB_START );
      if ( sub == NULL || sub->matches( arg, len, fmt ) )
        break;
    }
    if ( sub == NULL ) {
      if ( is_start ) {
        count++;
        sub = this->psub_start( sub_output, arg, len, fmt );
        this->outf( p, "pstart(%.*s) seqno = %" PRIu64,
          (int) sub->psublen, sub->psub, sub->start_seqno );
      }
      return count;
    }
    if ( is_start ) {
      if ( sub->out.add( sub_output ) ) {
        count++;
        this->outf( p, "pstart(%.*s) add to existing stream",
                    (int) sub->psublen, sub->psub );
      }
      return count;
    }
    /* punsub */
    if ( sub->out.remove( sub_output ) ) {
      count++;
      if ( sub->out.count == 0 ) {
        this->psub_stop( sub );
        this->outf( p, "pstop(%.*s) seqno = %" PRIu64,
                    (int) sub->psublen, sub->psub, sub->start_seqno );
      }
      else {
        this->outf( p, "pstop(%.*s) remove from existing stream",
                    (int) sub->psublen, sub->psub );
      }
    }
    if ( len != 0 ) /* if no arg, unsub all */
      return count;
  }
}

ConsoleSubStart *
Console::sub_start( ConsoleOutput *sub_output,
                    const char *arg,  size_t len ) noexcept
{
  ConsoleSubStart *sub;
  uint32_t h   = kv_crc_c( arg, len, 0 ),
           ibx = 0;
  char     inbox[ EvSocket::MAX_SESSION_LEN + 64 ];
  size_t   inbox_len = 0;
  uint16_t svc = SessionMgr::sub_has_rv_service( arg, len );

  sub = this->create_rpc<ConsoleSubStart>( sub_output, SUB_START );
  if ( svc != 0 )
    ibx = this->start_rv_inbox( svc, sub, inbox, inbox_len );
  sub->set_sub( arg, len, h, ibx );
  sub->start_seqno = this->sub_db.console_sub_start( arg, len, inbox,
                                                     inbox_len, sub );
  this->check_stat_monitor( arg, len, true );
  return sub;
}

uint32_t
Console::start_rv_inbox( uint16_t svc,  SubOnMsg *sub,
                         char *inbox,  size_t &inbox_len ) noexcept
{
  RvSvc *rv_svc = this->mgr.get_rv_session( svc, true );
  if ( rv_svc == NULL )
    return 0;
  rv_svc->ref_count++;
  /* _7500._INBOX.<session>.<num> */
  CatPtr p( inbox );
  p.c( '_' ).u( svc, uint16_digits( svc ) )
   .s( "._INBOX." )
   .x( rv_svc->session, rv_svc->session_len )
   .c( '.' );
  uint32_t h = kv_crc_c( inbox, p.len(), 0 );

  RouteLoc loc;
  ConsoleInboxRoute * rt = this->inbox_tab.upsert( h, inbox, p.len(), loc );
  if ( loc.is_new ) {
    inbox[ p.len() ] = '>';
    inbox[ p.len() + 1 ] = '\0';
    rt->start_seqno =
      this->sub_db.console_psub_start( inbox, p.len() + 1, RV_PATTERN_FMT,
                                       &this->inbox_tab );
    rt->ref_count = 0;
  }
  rt->ref_count++;
  uint32_t i;
  if ( this->inbox_tab.free_inbox > 0 ) {
    for ( i = 1; i < this->inbox_tab.cb.count; i++ ) {
      if ( this->inbox_tab.cb.ptr[ i ] == NULL ) {
        this->inbox_tab.free_inbox--;
        break;
      }
    }
  }
  else {
    i = ++this->inbox_tab.next_inbox;
  }
  this->inbox_tab.cb[ i ] = sub;
  p.u( i, uint32_digits( i ) );
  inbox_len = p.end();
  return i;
}

void
Console::sub_stop( ConsoleSubStart *sub ) noexcept
{
  uint16_t svc = SessionMgr::sub_has_rv_service( sub->sub, sub->sublen );

  this->check_stat_monitor( sub->sub, sub->sublen, false );
  this->sub_db.console_sub_stop( sub->sub, (uint16_t) sub->sublen );
  sub->is_complete = true;
  if ( svc != 0 )
    this->stop_rv_inbox( svc, sub->inbox, sub );
  while ( sub->out.pop() )
    ;
}

void
Console::snap_stop( ConsoleSnap *sub ) noexcept
{
  uint16_t svc = SessionMgr::sub_has_rv_service( sub->sub, sub->sublen );

  sub->is_complete = true;
  if ( svc != 0 )
    this->stop_rv_inbox( svc, sub->inbox, sub );
  while ( sub->out.pop() )
    ;
}

void
Console::stop_rv_inbox( uint16_t svc,  uint32_t &ibx,  SubOnMsg *sub ) noexcept
{
  RvSvc *rv_svc = this->mgr.get_rv_session( svc, false );
  if ( rv_svc == NULL )
    return;

  if ( this->inbox_tab.cb[ ibx ] != sub )
    return;
  this->inbox_tab.cb[ ibx ] = NULL;
  ibx = 0;
  this->inbox_tab.free_inbox++;

  char   inbox[ EvSocket::MAX_SESSION_LEN + 64 ];
  CatPtr p( inbox );
  p.c( '_' ).u( svc, uint16_digits( svc ) )
   .s( "._INBOX." )
   .x( rv_svc->session, rv_svc->session_len )
   .c( '.' );
  uint32_t h = kv_crc_c( inbox, p.len(), 0 );

  RouteLoc loc;
  ConsoleInboxRoute * rt = this->inbox_tab.find( h, inbox, p.len(), loc );
  if ( rt == NULL )
    return;
  if ( --rt->ref_count == 0 ) {
    this->inbox_tab.remove( loc );
    inbox[ p.len() ] = '>';
    this->sub_db.console_psub_stop( inbox, p.len() + 1, RV_PATTERN_FMT );
  }
  if ( --rv_svc->ref_count == 0 )
    this->mgr.stop_rv_session( rv_svc );
}

void
ConsoleInboxTab::on_data( const SubMsgData &val ) noexcept
{
  const MsgHdrDecoder & dec = val.pub.dec;
  size_t       sublen = val.pub.subject_len;
  const char * sub    = val.pub.subject;
  uint32_t     inbox  = 0;

  if ( dec.test( FID_SUBJECT ) ) {
    sublen = dec.mref[ FID_SUBJECT ].fsize;
    sub    = (const char *) dec.mref[ FID_SUBJECT ].fptr;
  }
  for ( size_t i = sublen; i > 0; i-- ) {
    if ( sub[ i - 1 ] < '0' || sub[ i - 1 ] > '9' )
      break;
    inbox = inbox * 10 + (uint32_t) (uint8_t) ( sub[ i - 1 ] - '0' );
  }
  if ( inbox >= this->cb.count || this->cb.ptr[ inbox ] == NULL )
    return;
  this->cb.ptr[ inbox ]->on_data( val );
}

ConsolePSubStart *
Console::psub_start( ConsoleOutput *sub_output,  const char *arg,  size_t len,
                     PatternFmt fmt ) noexcept
{
  ConsolePSubStart *sub;
  uint16_t svc = SessionMgr::sub_has_rv_service( arg, len );

  sub = this->create_rpc<ConsolePSubStart>( sub_output, PSUB_START );
  sub->set_psub( arg, len, fmt );
  if ( svc != 0 ) {
    RvSvc *rv_svc = this->mgr.get_rv_session( svc, true );
    if ( rv_svc != NULL )
      rv_svc->ref_count++;
  }
  sub->start_seqno = this->sub_db.console_psub_start( arg, len, fmt, sub );
  this->check_stat_monitor( arg, len, true );
  return sub;
}

void
Console::psub_stop( ConsolePSubStart *sub ) noexcept
{
  uint16_t svc = SessionMgr::sub_has_rv_service( sub->psub, sub->psublen );

  this->check_stat_monitor( sub->psub, sub->psublen, false );
  this->sub_db.console_psub_stop( sub->psub, sub->psublen, sub->pat_fmt );
  sub->is_complete = true;
  if ( svc != 0 ) {
    RvSvc *rv_svc = this->mgr.get_rv_session( svc, false );
    if ( rv_svc != NULL ) {
      if ( --rv_svc->ref_count == 0 )
        this->mgr.stop_rv_session( rv_svc );
    }
  }
  while ( sub->out.pop() )
    ;
}

void
Console::stop_rpc( ConsoleOutput *p,  ConsoleRPC *rpc ) noexcept
{
  if ( rpc->out.remove( p ) && rpc->out.count == 0 ) {
    if ( rpc->type == SUB_START ) {
      ConsoleSubStart *sub = (ConsoleSubStart *) rpc;
      this->check_stat_monitor( sub->sub, sub->sublen, false );
      this->sub_db.console_sub_stop( sub->sub, sub->sublen );
      rpc->is_complete = true;
    }
    else if ( rpc->type == PSUB_START ) {
      ConsolePSubStart *psub = (ConsolePSubStart *) rpc;
      this->check_stat_monitor( psub->psub, psub->psublen, false );
      this->sub_db.console_psub_stop( psub->psub, psub->psublen, psub->pat_fmt);
      rpc->is_complete = true;
    }
  }
}

enum {
  T_NO_EXIST   = 0,
  T_CFG_EXISTS = 1,
  T_IS_RUNNING = 2,
  T_IS_DOWN    = 3
};

int
Console::find_tport( const char *name,  size_t len,
                     ConfigTree::Transport *&tree_idx,
                     uint32_t &tport_id ) noexcept
{
  if ( len > 0 ) {
    ConfigTree::Transport * tport = this->tree.find_transport( name, len );
    TransportRoute * rte;
    if ( tport != NULL ) {
      rte = this->user_db.transport_tab.find_transport( tport );
      if ( rte != NULL ) {
        tree_idx = tport;
        tport_id = rte->tport_id;
        if ( rte->is_set( TPORT_IS_SHUTDOWN ) )
          return T_IS_DOWN;
        this->printf( "Transport (%.*s) is running tport %u\n",
                      (int) len, name, tport_id );
        return T_IS_RUNNING;
      }
      tree_idx = tport;
      tport_id = (uint32_t) this->user_db.transport_tab.count;
      return T_CFG_EXISTS;
    }
  }
  this->printf( "Transport (%.*s) not found\n", (int) len, name );
  return T_NO_EXIST;
}

void
Console::connect( const char *name,  size_t len ) noexcept
{
  ConfigTree::Transport * tree_idx = NULL;
  uint32_t tport_id;
  int res = this->find_tport( name, len, tree_idx, tport_id );
  bool b;
  if ( res == T_NO_EXIST || res == T_IS_RUNNING ) {
    if ( res == T_IS_RUNNING ) {
      TransportRoute *rte = this->user_db.transport_tab[ tport_id ];
      if ( rte->transport.type.equals( "mesh" ) )
        b = this->mgr.start_transport( *rte, false );
    }
    return;
  }
  if ( res == T_IS_DOWN ) {
    TransportRoute *rte = this->user_db.transport_tab[ tport_id ];
    b = this->mgr.start_transport( *rte, false );
  }
  else {
    b = this->mgr.add_transport( *tree_idx, false );
  }
  if ( b )
    this->printf( "Transport (%.*s) started connecting\n", (int) len, name );
  else
    this->printf( "Transport (%.*s) connect failed\n", (int) len, name );
}

void
Console::listen( const char *name,  size_t len ) noexcept
{
  ConfigTree::Transport * tree_idx = NULL;
  uint32_t tport_id;
  int res = this->find_tport( name, len, tree_idx, tport_id );
  bool b;
  if ( res == T_NO_EXIST || res == T_IS_RUNNING )
    return;
  if ( res == T_IS_DOWN ) {
    TransportRoute *rte = this->user_db.transport_tab[ tport_id ];
    b = this->mgr.start_transport( *rte, true );
  }
  else {
    b = this->mgr.add_transport( *tree_idx, true );
  }
  if ( b )
    this->printf( "Transport (%.*s) started listening\n", (int) len, name );
  else
    this->printf( "Transport (%.*s) listen failed\n", (int) len, name );
}

void
Console::shutdown( const char *name,  size_t len ) noexcept
{
  ConfigTree::Transport * tree_idx = NULL;
  uint32_t tport_id;
  int res = this->find_tport( name, len, tree_idx, tport_id );
  if ( res == T_NO_EXIST )
    return;
  /*if ( res != T_IS_RUNNING ) {
    this->printf( "Transport (%.*s) not running\n", (int) len, name );
    return;
  }*/
  uint32_t count = this->mgr.shutdown_transport( *tree_idx );
  if ( count > 0 )
    this->printf( "Transport (%.*s) shutdown (%u instances down)\n",
                  (int) len, name, count );
  else
    this->printf( "No transport (%.*s) running\n", (int) len, name );
}

void
Console::get_active_tports( ConfigTree::TransportArray &listen,
                            ConfigTree::TransportArray &connect ) noexcept
{
  ConfigTree::Transport * tport;
  for ( tport = this->tree.transports.hd; tport != NULL; tport = tport->next ) {
    size_t count = this->user_db.transport_tab.count;
    for ( size_t t = 0; t < count; t++ ) {
      TransportRoute *rte = this->user_db.transport_tab.ptr[ t ];
      if ( &rte->transport == tport ) {
        if ( ! rte->is_set( TPORT_IS_SHUTDOWN ) ) {
          if ( rte->is_set( TPORT_IS_IPC ) ) {
            for ( IpcRte *ext = rte->ext->list.hd; ext != NULL;
                  ext = ext->next ) {
              listen.push_unique( &ext->transport );
            }
          }
          else {
            if ( tport->is_temp )
              continue;
            if ( rte->is_set( TPORT_IS_MESH ) ) {
              if ( rte->is_set( TPORT_IS_LISTEN ) )
                listen.push_unique( tport );
              else
                connect.push_unique( tport );
            }
            else if ( rte->is_set( TPORT_IS_LISTEN ) )
              listen.push_unique( tport );
            else
              connect.push_unique( tport );
          }
        }
        break;
      }
    }
  }
  for ( size_t i = 0; i < this->mgr.unrouteable.count; i++ ) {
    Unrouteable & un = this->mgr.unrouteable.ptr[ i ];
    if ( un.telnet != NULL ) {
      if ( un.telnet->in_list( IN_ACTIVE_LIST ) )
        if ( ! un.tport->is_temp )
          listen.push( un.tport );
    }
    else if ( un.web != NULL ) {
      if ( un.web->in_list( IN_ACTIVE_LIST ) )
        if ( ! un.tport->is_temp )
          listen.push( un.tport );
    }
    else if ( un.name != NULL ) {
      if ( ! un.name->is_closed )
        if ( ! un.tport->is_temp )
          listen.push( un.tport );
    }
  }
}

void
Console::get_startup_tports( ConfigTree::TransportArray &listen,
                             ConfigTree::TransportArray &connect ) noexcept
{
  const ConfigTree::Parameters *pa = this->startup.tree->startup.hd;
  for ( ; pa != NULL; pa = pa->next ) {
    const ConfigTree::StringPair *sp = pa->list.hd;
    for ( ; sp != NULL; sp = sp->next ) {
      if ( sp->name.equals( R_LISTEN, R_LISTEN_SZ ) )
        listen.push_unique( sp->value );
      else if ( sp->name.equals( R_CONNECT, R_CONNECT_SZ ) )
        connect.push_unique( sp->value );
    }
  }
}

void
Console::config_save( void ) noexcept
{
  ConfigChange * c;
  ConfigTree::TransportArray listen, connect;

  this->get_active_tports( listen, connect );
  if ( this->tree.is_dir ) {
    for ( c = this->changes.hd; c != NULL; c = c->next ) {
      if ( c->tport != NULL )
        if ( this->tree.save_tport( *c->tport ) != 0 )
          return;
    }
    this->tree.save_startup( listen, connect );
  }
  else {
    if ( this->tree.save_file( listen, connect ) != 0 )
      return;
  }
  int n = this->tree.save_new();
  if ( n >= 0 ) {
    this->changes.release();
    if ( n == 0 )
      this->printf( "No changes\n" );
    else {
      this->printf( "Config saved\n" );
      this->startup.copy( this->tree, &listen, &connect );
    }
  }
  else {
    this->printf( "Failed to save config updates\n" );
  }
}

void
Console::config_param( const char *param, size_t plen,
                       const char *value, size_t vlen ) noexcept
{
  ConfigTree::StringPair *sp;
  StringVal p( param, plen ), v( value, vlen );
  sp = this->tree.parameters.update( this->string_tab, p, v );
  if ( sp != NULL )
    sp->is_temp = false;

  if ( p.equals( "log_file" ) ) {
    if ( this->log_filename == NULL && sp == NULL )
      return;
    if ( this->log_filename != NULL && sp == NULL ) {
      this->printf( "closed log \"%s\"\n", this->log_filename );
      this->open_log( NULL, false );
      return;
    }
    if ( this->log_filename == NULL ||
         ! sp->value.equals( this->log_filename ) ) {
      bool b = this->open_log( sp->value.val, true );
      this->printf( "%s new log file: \"%s\"\n",
                     b ? "Opened" : "Failed to open", this->log_filename );
      if ( b )
        this->next_rotate = 1;
    }
  }
  else {
    this->mgr.reload_parameters();
  }
}

void
Console::change_prompt( const char *param,  size_t plen ) noexcept
{
  this->update_prompt( param, plen );
  for ( ConsoleOutput *o = this->term_list.hd; o!= NULL; o = o->next )
    o->on_prompt( this->prompt );
}

bool
Console::config_transport( const char *args[],  size_t *arglen,
                           size_t argc ) noexcept
{
  if ( argc < 3 )
    return false;
  const char * tport     = args[ 2 ];
  size_t       tport_len = arglen[ 2 ];
  if ( tport_len == 0 )
    return false;
  this->cfg_tport = this->tree.find_transport( tport, tport_len );
  if ( this->cfg_tport == NULL ) {
    this->cfg_tport = this->string_tab.make<ConfigTree::Transport>();
    this->string_tab.ref_string( tport, tport_len, this->cfg_tport->tport );
    this->cfg_tport->tport_id = this->tree.transport_cnt++;
    this->tree.transports.push_tl( this->cfg_tport );
  }
  if ( argc > 3 ) {
    size_t shift = 3;
    for (;;) {
      int cmd = this->shift_command( shift, args, arglen, argc );
      if ( cmd == CMD_BAD || cmd == CMD_EMPTY ) {
        if ( cmd == CMD_BAD )
          this->printf( "Bad cmd: %.*s\n", (int) arglen[ 0 ], args[ 0 ] );
        break;
      }
      if ( ! this->config_transport_param( cmd, args, arglen, argc ) )
        break;
      shift = 2;
    }
    this->changes.add( this->cfg_tport );
    this->printf( "Transport (%s) updated\n", this->cfg_tport->tport.val );
    this->cfg_tport = NULL;
  }
  else {
    this->change_prompt( tport, tport_len );
  }
  return true;
}

bool
Console::config_transport_param( int cmd,  const char *args[],
                                 size_t *arglen,  size_t argc ) noexcept
{
  switch ( cmd ) {
    case CMD_TPORT_TPORT:
      if ( argc < 1 )
        return false;
      this->string_tab.reref_string( args[ 1 ], arglen[ 1 ],
                                     this->cfg_tport->tport );
      break;
    case CMD_TPORT_TYPE:
      if ( argc < 1 )
        return false;
      this->string_tab.reref_string( args[ 1 ], arglen[ 1 ],
                                     this->cfg_tport->type );
      break;
    default:
      if ( argc < 1 )
        return false;
      if ( argc < 2 ) {
        args[ 1 ] = NULL;
        arglen[ 1 ] = 0;
      }
      this->config_transport_route( args[ 0 ], arglen[ 0 ],
                                    args[ 1 ], arglen[ 1 ] );
      break;
  }
  return true;
}

void
Console::config_transport_route( const char *param, size_t plen,
                                 const char *value, size_t vlen ) noexcept
{
  ConfigTree::PairList & route = this->cfg_tport->route;
  ConfigTree::StringPair *sp = route.get_pair( param, plen );

  if ( sp == NULL ) {
    if ( vlen == 0 ) {
      this->printf( "Not found: %.*s\n", (int) plen, param );
      return;
    }
  }
  if ( sp != NULL ) {
    ConfigTree::StringPair *next;
    for (;;) {
      next = sp->next;
      if ( vlen != 0 && ( next == NULL || ! next->name.equals( param, plen ) ) )
        break;
      route.unlink( sp );
      this->string_tab.release( sp );
      if ( vlen == 0 && ( next == NULL || ! next->name.equals( param, plen ) ) )
        return;
      sp = next;
    }
  }
  if ( sp == NULL ) {
    sp = this->string_tab.make<ConfigTree::StringPair>();
    route.push_tl( sp );
  }
  this->string_tab.reref_string( param, plen, sp->name );
  this->string_tab.reref_string( value, vlen, sp->value );
}

void
Console::show_subs( ConsoleOutput *p,  const char *arg,
                    size_t arglen,  const char *arg2,
                    size_t arglen2 ) noexcept
{
  UserBridge  * n;
  char          isub[ UserDB::INBOX_BASE_SIZE + sizeof( _SUBS ) ];
  uint32_t      len;
  ConsoleSubs * rpc = this->create_rpc<ConsoleSubs>( p, SUBS_RPC );

  if ( arglen == 1 && arg[ 0 ] == '*' )
    arglen = 0;
  if ( arglen2 == 1 && arg2[ 0 ] == '*' )
    arglen2 = 0;

  if ( arglen != 0 ) {
    if ( this->user_db.user.user.equals( arg, arglen ) ||
         ( arglen == 4 && ::memcmp( arg, "self", 4 ) == 0 ) )
      rpc->show_self = true;
  }
  else {
    rpc->show_self = true;
  }
    
  if ( rpc->show_self && arglen2 > 0 )
    rpc->set_match( arg2, arglen2 );

  if ( ! rpc->show_self || arglen == 0 ) {
    for ( uint32_t uid = 1; uid < this->user_db.next_uid; uid++ ) {
      n = this->user_db.bridge_tab[ uid ];
      if ( n != NULL && n->is_set( AUTHENTICATED_STATE ) ) {
        if ( arglen != 0 ) {
          if ( ! n->peer.user.equals( arg, arglen ) )
            continue;
        }
        if ( n->sub_seqno > 0 ) { /* must have subs seqno */
          len = n->make_inbox_subject( isub, _SUBS );

          PubMcastData mc( isub, len, arg2, arglen2,
                           arglen2 == 0 ? MD_NODATA : MD_STRING,
                           U_INBOX_SUBS );
          mc.reply = rpc->inbox_num;
          mc.stamp = current_realtime_ns();
          mc.token = rpc->token;
          this->mgr.publish( mc );
          rpc->count++;
          if ( arglen != 0 )
            break;
        }
      }
    }
  }
  if ( rpc->count == 0 ) {
    rpc->is_complete = true;
    this->on_subs( *(ConsoleSubs *) rpc );
  }
}

void
Console::send_remote_request( ConsoleOutput *p,  const char *arg,
                              size_t arglen,  const char *cmd,
                              size_t cmdlen ) noexcept
{
  UserBridge    * n;
  char            isub[ UserDB::INBOX_BASE_SIZE + sizeof( _REM ) ];
  uint32_t        len;
  ConsoleRemote * rpc = this->create_rpc<ConsoleRemote>( p, REMOTE_RPC );

  if ( arglen == 1 && arg[ 0 ] == '*' )
    arglen = 0;
  if ( arglen != 0 ) {
    if ( this->user_db.user.user.equals( arg, arglen ) ||
         ( arglen == 4 && ::memcmp( arg, "self", 4 ) == 0 ) )
      rpc->show_self = true;
  }
  else {
    rpc->show_self = true;
  }

  if ( rpc->show_self && cmdlen > 0 )
    rpc->set_command( cmd, cmdlen );
    
  for ( uint32_t uid = 1; uid < this->user_db.next_uid; uid++ ) {
    n = this->user_db.bridge_tab[ uid ];
    if ( n != NULL && n->is_set( AUTHENTICATED_STATE ) ) {
      if ( arglen != 0 ) {
        if ( ! n->peer.user.equals( arg, arglen ) )
          continue;
      }
      len = n->make_inbox_subject( isub, _REM );

      PubMcastData mc( isub, len, cmd, cmdlen,
                       cmdlen == 0 ? MD_NODATA : MD_STRING,
                       U_INBOX_REM );
      mc.reply = rpc->inbox_num;
      mc.token = rpc->token;
      this->mgr.publish( mc );
      rpc->count++;
      if ( arglen != 0 )
        break;
    }
  }
  if ( rpc->count == 0 ) {
    rpc->is_complete = true;
    if ( rpc->show_self )
      this->on_remote( *rpc );
    else if ( arglen > 0 )
      this->outf( p, "no users matched: %.*s", (int) arglen, arg );
    else
      this->outf( p, "no users" );
  }
}

bool
JsonBufOutput::on_output( const char *buf,  size_t buflen ) noexcept
{
  char * p  = this->result.make( this->result.count + buflen );
  p = &p[ this->result.count ];
  this->result.count += buflen;
  ::memcpy( p, buf, buflen );
  return true;
}

bool
Console::recv_remote_request( const MsgFramePublish &,  UserBridge &n,
                              const MsgHdrDecoder &dec ) noexcept
{
  const void * data    = NULL;
  size_t       datalen = 0;
  char         ret_buf[ 16 ];
  const char * suf = dec.get_return( ret_buf, NULL );

  if ( dec.test( FID_DATA ) ) {
    data    = dec.mref[ FID_DATA ].fptr;
    datalen = dec.mref[ FID_DATA ].fsize;
  }
  if ( suf == NULL || datalen == 0 )
    return true;

  JsonBufOutput out;
  out.is_remote = true;
  this->on_input( &out, (const char *) data, datalen );
  if ( out.result.count == 0 )
    out.result.puts( "\"no data\"\n" );

  InboxBuf ibx( n.bridge_id, suf );
  uint64_t token = 0;

  if ( dec.test( FID_TOKEN ) )
    cvt_number<uint64_t>( dec.mref[ FID_TOKEN ], token );

  MsgEst e( ibx.len() );
  e.seqno ()
   .token ()
   .data  ( out.result.count );

  MsgCat m;
  m.reserve( e.sz );

  m.open( this->user_db.bridge_id.nonce, ibx.len() )
   .seqno( n.inbox.next_send( U_INBOX_REM ) );
  if ( token != 0 )
    m.token( token );
  m.data( out.result.ptr, out.result.count );
  uint32_t h = ibx.hash();
  m.close_zpath( e.sz, h, CABA_INBOX, U_INBOX_REM );
  m.sign( ibx.buf, ibx.len(), *this->user_db.session_key );
  return this->user_db.forward_to_inbox( n, ibx, h, m.msg, m.len() );
}

void
Console::ping_peer( ConsoleOutput *p,  const char *arg,
                    size_t arglen,  bool add_trace ) noexcept
{
  UserBridge  * n;
  char          isub[ UserDB::INBOX_BASE_SIZE + sizeof( _PING ) ];
  uint32_t      len;
  ConsolePing * rpc = this->create_rpc<ConsolePing>( p, PING_RPC );

  if ( arglen == 1 && arg[ 0 ] == '*' )
    arglen = 0;
  rpc->fwd.count = 0;
  for ( uint32_t uid = 1; uid < this->user_db.next_uid; uid++ ) {
    n = this->user_db.bridge_tab[ uid ];
    if ( n != NULL && n->is_set( AUTHENTICATED_STATE ) ) {
      if ( arglen != 0 ) {
        if ( ! n->peer.user.equals( arg, arglen ) )
          continue;
      }
      len = n->make_inbox_subject( isub, _PING );

      PubMcastData mc( isub, len, NULL, 0, MD_NODATA, U_INBOX_CONSOLE );
      mc.reply = rpc->inbox_num;
      mc.stamp = current_realtime_ns();
      mc.token = rpc->token;
      if ( add_trace )
        mc.option = CABA_OPT_TRACE;
      this->mgr.publish( mc );
      if ( mc.fwd_cnt == 1 )
        rpc->fwd.push( mc.forward_tport[ 0 ] );
      rpc->count++;
    }
  }
  if ( rpc->count == 0 ) {
    rpc->is_complete = true;
    if ( arglen > 0 )
      this->outf( p, "no users matched: %.*s", (int) arglen, arg );
    else
      this->outf( p, "no users" );
  }
  else {
    rpc->reply.zero();
    rpc->reply.make( rpc->count, true );
  }
}

void
Console::mcast_ping( ConsoleOutput *p,  uint16_t path,  bool add_trace ) noexcept
{
  ConsolePing * rpc = this->create_rpc<ConsolePing>( p, PING_RPC );

  rpc->count = this->user_db.uid_auth_count;
  if ( rpc->count == 0 ) {
    rpc->is_complete = true;
    this->outf( p, "no users" );
  }
  else {
    static const char m_ping[] = _MCAST "." _PING;
    PubMcastData mc( m_ping, sizeof( m_ping ) - 1, NULL, 0, MD_NODATA,
                     U_MCAST_PING );
    mc.reply = rpc->inbox_num;
    mc.stamp = current_realtime_ns();
    mc.token = rpc->token;
    mc.path  = path;
    if ( add_trace )
      mc.option = CABA_OPT_TRACE;
    this->mgr.publish( mc );
    rpc->fwd.count = 0;
    for ( uint32_t i = 0; i < mc.fwd_cnt; i++ )
      rpc->fwd.push( mc.forward_tport[ i ] );
    rpc->reply.zero();
    rpc->reply.make( rpc->count, true );
  }
}

void
Console::on_ping( ConsolePing &ping ) noexcept
{
  static const uint32_t ncols = 6;
  TabOut out( this->table, this->tmp, ncols );
  TransportRoute *rte;
  uint32_t p, i = 0, tid;
  BitSpace used;

  for ( p = 0; p < ping.count; p++ ) {
    PingReply & reply = ping.reply.ptr[ p ];
    if ( reply.uid < this->user_db.bridge_tab.count ) {
      if ( this->user_db.bridge_tab[ reply.uid ] != NULL )
        used.add( reply.uid );
    }
  }
  for ( p = 0; p < ping.count; p++ ) {
    PingReply & reply = ping.reply.ptr[ p ];
    bool no_route = true;
    TabPrint * tab = out.make_row();
    UserBridge * n = NULL;
    if ( reply.uid < this->user_db.bridge_tab.count ) {
      n = this->user_db.bridge_tab[ reply.uid ];
      if ( n != NULL ) {
        tab[ i++ ].set( n->peer.user, reply.uid, PRINT_ID );
        tab[ i++ ].set( n, PRINT_DIST );
        no_route = false;
      }
    }
    if ( no_route ) {
      uint32_t uid;
      for ( uid = 1; uid < this->user_db.bridge_tab.count; uid++ ) {
        if ( ! used.test_set( uid ) ) {
          UserBridge * n = this->user_db.bridge_tab[ uid ];
          if ( n != NULL && n->is_set( AUTHENTICATED_STATE ) ) {
            tab[ i++ ].set( n->peer.user, uid, PRINT_ID );
            tab[ i++ ].set_null();
            break;
          }
        }
      }
      if ( uid == this->user_db.bridge_tab.count ) {
        tab[ i++ ].set_null();
        tab[ i++ ].set_null();
      }
    }
    tab[ i++ ].set_long( reply.recv_time - reply.sent_time, PRINT_LATENCY );

    if ( p < ping.fwd.count &&
         (tid = ping.fwd.ptr[ p ]) < this->user_db.transport_tab.count ) {
      rte = this->user_db.transport_tab.ptr[ tid ];
      tab[ i++ ].set( rte->transport.tport, tid, PRINT_ID );
    }
    else {
      tab[ i++ ].set_null();
    }
    if ( reply.tid < this->user_db.transport_tab.count ) {
      rte = this->user_db.transport_tab.ptr[ reply.tid ];
      tab[ i++ ].set( rte->transport.tport, reply.tid, PRINT_ID );
    }
    else
      tab[ i++ ].set_null();

    if ( reply.rem_tid != 0 ) {
      if ( n != NULL && reply.rem_tid < n->adjacency.count &&
           n->adjacency.ptr[ reply.rem_tid ] != NULL ) {
        tab[ i++ ].set( n->adjacency.ptr[ reply.rem_tid ]->tport,
                        reply.rem_tid, PRINT_ID );
      }
      else {
        tab[ i++ ].set_int( reply.rem_tid );
      }
    }
    else {
      tab[ i++ ].set_null();
    }
  }
  static const char *hdr[ ncols ] = { "user", "cost", "lat", "send", "recv", "peer_recv" };
  for ( size_t n = 0; n < ping.out.count; n++ ) {
    ConsoleOutput * p = ping.out.ptr[ n ];
    this->print_table( p, hdr, ncols );
    this->flush_output( p );
  }
}

void
Console::on_subs( ConsoleSubs &subs ) noexcept
{
  static const uint32_t ncols = 4;
  TabOut out( this->table, this->tmp, ncols );
  TabPrint * tab;
  uint32_t s, i = 0, uid;
  BitSpace users;

  if ( subs.show_self ) {
    SubListIter iter( this->sub_db.sub_list, 0, this->sub_db.sub_seqno );
    const char * match = subs.match;
    size_t match_len = subs.match_len;

    for ( bool ok = iter.first(); ok; ok = iter.next() ) {
      if ( iter.action == ACTION_SUB_JOIN ) {
        SubRoute   * sub;
        const char * q = NULL;
        sub = this->sub_db.sub_tab.find_sub( iter.hash, iter.seqno );

        if ( sub == NULL ) {
          for ( uint32_t i = 0; i < this->sub_db.queue_tab.count; i++ ) {
            SubTab &tab = this->sub_db.queue_tab.ptr[ i ]->sub_tab; 
            if ( (sub = tab.find_sub( iter.hash, iter.seqno )) != NULL ) {
              q = this->sub_db.queue_tab.ptr[ i ]->queue;
              break;
            }
          }
        }
        if ( sub != NULL ) {
          if ( match_len == 0 ||
               kv_memmem( sub->value, sub->len, match, match_len ) != NULL ) { 
            tab = out.make_row();
            if ( i == 0 )
              tab[ i++ ].set( this->user_db.user.user, PRINT_SELF ); /* user */
            else
              tab[ i++ ].set_null();
            if ( q != NULL )
              tab[ i++ ].set( q );
            else
              tab[ i++ ].set_null();
            tab[ i++ ].set_null();
            tab[ i++ ].set( sub->value, sub->len );
            tab[ i-1 ].typ |= PRINT_LEFT;
          }
        }
      }
      else {
        PatRoute   * pat;
        const char * q = NULL;
        pat = this->sub_db.pat_tab.find_sub( iter.hash, iter.seqno );

        if ( pat == NULL ) {
          for ( uint32_t i = 0; i < this->sub_db.queue_tab.count; i++ ) {
            PatTab &ptab = this->sub_db.queue_tab.ptr[ i ]->pat_tab; 
            if ( (pat = ptab.find_sub( iter.hash, iter.seqno )) != NULL ) {
              q = this->sub_db.queue_tab.ptr[ i ]->queue;
              break;
            }
          }
        }
        if ( pat != NULL ) {
          if ( match_len == 0 ||
               kv_memmem( pat->value, pat->len, match, match_len ) != NULL ) { 
            tab = out.make_row();
            if ( i == 0 )
              tab[ i++ ].set( this->user_db.user.user, PRINT_SELF ); /* user */
            else
              tab[ i++ ].set_null();
            if ( q != NULL )
              tab[ i++ ].set( q );
            else
              tab[ i++ ].set_null();
            tab[ i++ ].set( pat->fmt == GLOB_PATTERN_FMT ? "g" : "p" );
            tab[ i++ ].set( pat->value, pat->len );
            tab[ i-1 ].typ |= PRINT_LEFT;
          }
        }
      }
    }
  }
  for ( s = 0; s < subs.reply.count; s++ ) {
    SubsReply & reply = subs.reply.ptr[ s ];
    users.add( reply.uid );
  }
  for ( bool ok = users.first( uid ); ok; ok = users.next( uid ) ) {
    uint32_t last_uid = 0;
    for ( s = 0; s < subs.reply.count; s++ ) {
      SubsReply & reply = subs.reply.ptr[ s ];
      if ( reply.uid != uid )
        continue;
      bool no_user = true;
      tab = out.make_row();
      if ( last_uid == MY_UID && reply.uid < this->user_db.bridge_tab.count ) {
        UserBridge * n = this->user_db.bridge_tab[ reply.uid ];
        if ( n != NULL ) {
          if ( i > 0 )
            tab[ i - 1 ].typ |= PRINT_SEP;
          tab[ i++ ].set( n->peer.user, reply.uid, PRINT_ID );
          no_user = false;
        }
        last_uid = reply.uid;
      }
      if ( no_user )
        tab[ i++ ].set_null();
      const char *str = &subs.strings.ptr[ reply.sub_off ];
      if ( reply.queue_len != 0 ) {
        const char *q = &str[ reply.sub_len + 1 ];
        tab[ i++ ].set( q );
      }
      else {
        tab[ i++ ].set_null();
      }
      if ( reply.is_pattern )
        tab[ i++ ].set( reply.fmt == GLOB_PATTERN_FMT ? "g" : "p" );
      else
        tab[ i++ ].set_null();
      tab[ i++ ].set( str, reply.sub_len );
      tab[ i-1 ].typ |= PRINT_LEFT;
    }
  }

  static const char *hdr[ ncols ] = { "user", "q", "p", "subject" };
  for ( size_t n = 0; n < subs.out.count; n++ ) {
    ConsoleOutput * p = subs.out.ptr[ n ];
    this->print_table( p, hdr, ncols );
    this->flush_output( p );
  }
}

bool
Console::print_json_table( ConsoleOutput *p,  const void * data,
                           size_t datalen ) noexcept
{
  MDMsgMem      mem;
  JsonMsgCtx    ctx;
  char       ** hdr = NULL;
  TabPrint    * tab;
  MDReference   aref;
  MDMsg       * amsg;
  MDFieldIter * f;
  MDReference   mref;
  MDName        name;
  TabOut        out( this->table, this->tmp, 0 );
  size_t        el, num_entries;
  uint32_t      i = 0, j;
  bool          b;

  const char *dp = (const char *) data;
  if ( datalen > 0 &&
    ( dp[ 0 ] != '[' && dp[ 0 ] != '{' && dp[ 0 ] != '\"' && dp[ 0 ] != '\'' ) )
    return false;
  if ( ctx.parse( (void *) data, 0, datalen, NULL, mem, false ) != 0 )
    return false;
  if ( ctx.msg->get_reference( aref ) != 0 )
    return false;
  if ( aref.ftype != MD_ARRAY ) {
    if ( aref.ftype == MD_STRING ) {
      this->printf( "%.*s\n", (int) aref.fsize, (char *) aref.fptr );
      return true;
    }
    return false;
  }
  /* expecting an array of messages */
  num_entries = aref.fsize;
  if ( aref.fentrysz > 0 )
    num_entries /= aref.fentrysz;
  if ( num_entries == 0 ) {
    this->printf( "no data\n" );
    return true;
  }
  /* parse each message */
  for ( el = 0; el < num_entries; el++ ) {
    if ( ctx.msg->get_array_ref( aref, el, mref ) != 0 ||
         mref.ftype != MD_MESSAGE ||
         ctx.msg->get_sub_msg( mref, amsg, NULL ) != 0 ||
         amsg->get_field_iter( f ) != 0 )
      continue;
    /* the column headers are the field names of the object */
    if ( out.ncols == 0 ) {
      for ( b = ( f->first() == 0 ); b; b = ( f->next() == 0 ) ) {
        if ( f->get_name( name ) == 0 )
          out.ncols++;
      }
      if ( out.ncols > 0 ) {
        hdr = (char **) mem.make( sizeof( hdr[ 0 ] ) * out.ncols );
        j = 0;
        for ( b = ( f->first() == 0 ); b; b = ( f->next() == 0 ) ) {
          if ( f->get_name( name ) == 0 )
            hdr[ j++ ] = (char *) name.fname;
        }
      }
    }
    if ( out.ncols == 0 )
      continue;
    /* place each object as a table row */
    tab = out.make_row();
    j = 0;
    for ( b = ( f->first() == 0 ); b; b = ( f->next() == 0 ) ) {
      if ( f->get_reference( mref ) != 0 )
        continue;
      tab[ i + j ].set_null();
      if ( mref.ftype == MD_DECIMAL ) {
        MDDecimal dec;
        if ( dec.get_decimal( mref ) == 0 ) {
          if ( dec.hint == MD_DEC_INTEGER )
            tab[ i + j ].set_long( dec.ival, PRINT_SLONG );
          else {
            char * tmp_s = (char *) mem.make( 16 );
            size_t len = dec.get_string( tmp_s, 16 );
            tab[ i + j ].set( tmp_s, len );
          }
        }
      }
      else if ( mref.ftype == MD_STRING ) {
        tab[ i + j ].set( (char *) mref.fptr, mref.fsize );
      }
      if ( ++j == out.ncols )
        break;
    }
    for ( ; j < out.ncols; j++ )
      tab[ i + j ].set_null();
    i += out.ncols;
  }
  if ( out.ncols > 0 )
    this->print_table( p, (const char **) hdr, out.ncols );
  return out.ncols > 0;
}

void
Console::on_remote( ConsoleRemote &remote ) noexcept
{
  ArrayOutput tmp;

  if ( this->out.count > 0 )
    this->flush_output( NULL );

  if ( remote.show_self ) {
    JsonBufOutput out;
    out.is_remote = true;
    this->on_input( &out, (const char *) remote.cmd, remote.cmd_len );
    if ( out.result.count == 0 )
      out.result.puts( "\"no data\"\n" );
    remote.append_data( 0, out.result.ptr, out.result.count );
  }
  /* for each reply by peers */
  for ( size_t n = 0; n < remote.out.count; n++ ) {
    ConsoleOutput * p = remote.out.ptr[ n ];

    for ( size_t num = 0; num < remote.reply.count; num++ ) {
      RemoteReply & rep     = remote.reply.ptr[ num ];
      char        * data    = &remote.strings.ptr[ rep.data_off ];
      size_t        datalen = rep.data_len;
      UserBridge  * n       = this->user_db.bridge_tab[ rep.uid ];
      const char  * user    = ( n != NULL ? n->peer.user.val :
                   ( rep.uid == MY_UID ? this->user_db.user.user.val : NULL ) );

      if ( p == NULL || ! p->is_json ) {
        if ( user != NULL )
          this->printf( "%.*sfrom %s.%u%.*s:\n", cz, cc, user, rep.uid, nz, nc);
        else
          this->printf( "%.*sfrom uid %u%.*s:\n", cz, cc, rep.uid, nz, nc );

        if ( ! this->print_json_table( p, data, datalen ) ) {
          this->printf( "%.*s", (int) datalen, (char *) data );
          /*this->outf( p, "unable to parse" );*/
        }
      }
      else if ( remote.reply.count == 1 ) {
        p->on_output( data, datalen );
      }
      else {
        if ( num == 0 ) /* { "A" : [ data ], "B" : [ data ] } */
          tmp.s( "{\"" );
        else
          tmp.s( ",\"" );
        if ( user != NULL )
          tmp.s( user );
        else
          tmp.i( rep.uid );
        tmp.s( "\":" ).b( data, datalen );
        if ( num == remote.reply.count - 1 ) {
          tmp.s( "}\n" );
          p->on_output( tmp.ptr, tmp.count );
          tmp.count = 0;
        }
      }
    }
    this->flush_output( p );
  }
}

void
Console::show_users( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 5;
  TabOut out( this->table, this->tmp, ncols );

  out.add_row()
     .set_int( 0 )
     .set( this->user_db.user.user )
     .set( this->user_db.user.svc )
     .set( this->user_db.user.create )
     .set( this->user_db.user.expires );

  for ( ConfigTree::User *user = this->tree.users.hd; user != NULL;
        user = user->next ) {
    if ( user->user.equals( this->user_db.user.user ) &&
         user->svc.equals( this->user_db.user.svc ) &&
         user->create.equals( this->user_db.user.create ) )
      continue;
    UserBridge * n = this->find_user( user->user.val, user->user.len );

    out.add_row()
       .set_null()
       .set( user->user )
       .set( user->svc )
       .set( user->create )
       .set( user->expires );

    if ( n != NULL )
      out.row( 0 ).set_int( n->uid );
  }
  static const char *hdr[ ncols ] = { "uid", "user", "svc", "create",
                                      "expires" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_events( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 6;
  TabOut out( this->table, this->tmp, ncols );

  const EventRec * ev;
  uint32_t         n, tid, uid;
  const char     * s, * s2;
  char             buf[ 80 ];

  for ( ev = this->mgr.events.first( n ); ev != NULL;
        ev = this->mgr.events.next( n ) ) {

    TabPrint * tab = out.add_row_p();
    uint32_t i = 0;

    tab[ i++ ].set_time( ev->stamp ); /* stamp */
    if ( ev->has_tport( tid ) &&
         tid < this->user_db.transport_tab.count ) { /* tport */
      TransportRoute *rte = this->user_db.transport_tab.ptr[ tid ];
      tab[ i++ ].set( rte->transport.tport, tid, PRINT_ID );
    }
    else if ( ev->is_flood() )
      tab[ i++ ].set( "(mcast)", 7 );
    else
      tab[ i++ ].set_null();

    this->tab_user_id( ev->source_uid, tab[ i++ ] ); /* user */

    if ( ev->has_peer( uid ) ) /* source */
      this->tab_user_id( uid, tab[ i++ ] ); /* peer */
    else if ( ev->is_ecdh() )
      tab[ i++ ].set( "(ecdh)", 6 );
    else if ( ev->is_encrypt() )
      tab[ i++ ].set( "(aes)", 5 );
    else 
      tab[ i++ ].set_null();

    tab[ i++ ].set( event_strings[ ev->event_type() ].val,
         (uint32_t) event_strings[ ev->event_type() ].len ); /* event */
    if ( (s = ev->data_tag( this->string_tab, buf )) != NULL ) { /* data */
      if ( (s2 = ev->reason_str()) != NULL )
        this->tab_concat( s, s2, tab[ i++ ] );
      else {
        if ( s == buf )
          this->tab_string( s, tab[ i++ ] );
        else
          tab[ i++ ].set( s );
      }
    }
    else if ( (s = ev->reason_str()) != NULL )
      tab[ i++ ].set( s );
    else
      tab[ i++ ].set_null();
  }
  static const char *hdr[ ncols ] = { "stamp", "tport", "user", "peer", "event",
                                      "data" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_unknown( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 5;
  TabOut out( this->table, this->tmp, ncols );
  const AdjPending * u;

  if ( this->user_db.adjacency_unknown.is_empty() ) {
    this->outf( p, "empty" );
    return;
  }
  for ( u = this->user_db.adjacency_unknown.hd; u != NULL; u = u->next ) {
    TabPrint * tab = out.add_row_p();
    uint32_t i = 0;
    tab[ i++ ].set( u->rte.transport.tport, u->rte.tport_id ); /* tport */
    this->tab_nonce( u->rec_list[ 0 ].nonce, tab[ i++ ] ); /* bridge */
    if ( u->uid != 0 )
      this->tab_user_id( u->uid, tab[ i++ ] ); /* source */
    else
      tab[ i++ ].set_null();
    tab[ i++ ].set( u->rec_list[ 0 ].user );  /* unknown user */
    tab[ i++ ].set( peer_sync_reason_string( u->reason ) ); /* reason */
    tab[ i-1 ].typ |= PRINT_LEFT;
  }
  static const char *hdr[ ncols ] = { "tport", "bridge", "source",
                                      "user", "reason" };
  this->print_table( p, hdr, ncols );
}

PortOutput::PortOutput( Console &c,  TabOut &o,  uint32_t t ) noexcept :
    console( c ), mgr( c.mgr ), user_db( c.user_db ), out( o ), tport_id( t ),
    cur_time( current_realtime_ns() ), unrouteable( 0 ) {}

PortOutput::PortOutput( Console &c,  TabOut &o,  Unrouteable *u ) noexcept :
    console( c ), mgr( c.mgr ), user_db( c.user_db ), out( o ),
    tport_id( 0xffffffffU ), cur_time( current_realtime_ns() ),
    unrouteable( u ) {}

void
PortOutput::init( TransportRoute *rte,  int fl,  int fd,
                  UserBridge *user ) noexcept
{
  this->stats.zero(); 
  this->rte   = rte;
  this->type  = &rte->transport.type;
  this->tport = &rte->transport.tport;
  this->state = rte->state;
  this->n     = user;
  this->fd    = fd;
  this->flags = fl;
  this->local.zero();
  this->remote.zero();
}

void
PortOutput::init( ConfigTree::Transport &tport,  int fl,  int fd ) noexcept
{
  this->stats.zero(); 
  this->rte   = NULL;
  this->type  = &tport.type;
  this->tport = &tport.tport;
  this->state = 0;
  this->n     = NULL;
  this->fd    = fd;
  this->flags = fl;
  this->local.zero();
  this->remote.zero();
}

void
PortOutput::init( TransportRoute *rte,  IpcRte *ext ) noexcept
{
  this->stats.zero(); 
  this->rte   = rte;
  this->type  = &ext->transport.type;
  this->tport = &ext->transport.tport;
  this->state = rte->state;
  this->n     = NULL;
  this->fd    = ext->listener->fd;
  this->flags = P_IS_LOCAL;
  this->local.zero();
  this->remote.zero();
}

void
PortOutput::output( void ( PortOutput::*put )( void ) ) noexcept
{
  uint32_t mcast_fd, ucast_fd;
  EvPoll & poll = this->mgr.poll;

  if ( this->tport_id != 0xffffffffU ) {
    TransportRoute *rte = this->user_db.transport_tab.ptr[ this->tport_id ];
    if ( rte->is_set( TPORT_IS_SHUTDOWN ) ||
         rte->is_set( TPORT_IS_INPROGRESS ) ) {
      if ( rte->is_set( TPORT_IS_INPROGRESS ) && rte->connect_ctx != NULL ) {
        this->init( rte, P_IS_REMOTE, -1 );
        uint64_t ns = rte->mgr.timer_mono_time -
                      rte->connect_ctx->start_mono_time;
        this->stats.active_ns = rte->mgr.timer_time - ns;

        size_t len = 8;
        const char * h = rte->connect_ctx->addr_info.host;
        if ( h != NULL )
          len += ::strlen( h );
        else
          h = "";
        char * tmp = this->console.tmp.str_make( len );
        int    x   = ::snprintf( tmp, len, "%s:%d", h,
                                 rte->connect_ctx->addr_info.port );
        this->remote.len = min_int( x, (int) len - 1 );
        this->remote.val = tmp;
      }
      else {
        this->init( rte, P_IS_DOWN, -1 );
      }
      (this->*put)();
    }
    else if ( rte->is_set( TPORT_IS_IPC ) ) {
      for ( IpcRte * ext = rte->ext->list.hd; ext != NULL; ext = ext->next ) {
        this->init( rte, ext );
        ext->listener->client_stats( this->stats );
        this->local_addr( ext->listener->peer_address.buf );
        (this->*put)();
      }
    }
    else if ( rte->listener != NULL ) {
      this->init( rte, P_IS_LOCAL, rte->listener->fd );
      rte->listener->client_stats( this->stats );
      this->local_addr( rte->listener->peer_address.buf );
      (this->*put)();
    }
    else if ( rte->is_mcast() ) {
      mcast_fd = rte->mcast.fd;
      ucast_fd = rte->inbox.fd;
      this->init( rte, P_IS_LOCAL, mcast_fd );

      if ( mcast_fd <= poll.maxfd && poll.sock[ mcast_fd ] != NULL ) {
        this->local_addr( poll.sock[ mcast_fd ]->peer_address.buf );
        poll.sock[ mcast_fd ]->client_stats( this->stats );
      }
      (this->*put)();
      this->init( rte, P_IS_LOCAL | P_IS_INBOX, ucast_fd );
      if ( ucast_fd <= poll.maxfd && poll.sock[ ucast_fd ] != NULL ) {
        this->local_addr( poll.sock[ ucast_fd ]->peer_address.buf );
        poll.sock[ ucast_fd ]->client_stats( this->stats );
      }
      (this->*put)();
    }
    else if ( rte->transport.type.equals( "any" ) ) {
      this->init( rte, P_IS_DOWN, -1 );
      (this->*put)();
    }
    uint32_t uid;
    for ( bool ok = rte->uid_connected.first( uid ); ok;
          ok = rte->uid_connected.next( uid ) ) {
      UserBridge * n = this->user_db.bridge_tab[ uid ];
      if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
        continue;
      UserRoute * u_ptr = n->user_route_ptr( this->user_db, this->tport_id, 27 );
      if ( rte->is_mcast() ) {
        ucast_fd = (uint32_t) u_ptr->inbox.fd;
        this->init( rte, P_IS_REMOTE | P_IS_INBOX, ucast_fd, n );
        this->stats.bytes_sent = u_ptr->bytes_sent;
        this->stats.msgs_sent  = u_ptr->msgs_sent;
        if ( u_ptr->is_set( UCAST_URL_STATE ) ) {
          const char *addr = u_ptr->ucast_url.val;
          uint32_t    len  = u_ptr->ucast_url.len;
          if ( len > sizeof( "inbox://" ) &&
               ::memcmp( "inbox://", addr, 8 ) == 0 ) {
            len -= 8; addr += 8;
          }
          this->remote_addr( addr, len );
        }
        (this->*put)();
      }
      else {
        mcast_fd = u_ptr->mcast.fd;
        this->init( rte, P_IS_REMOTE, mcast_fd, n );
        if ( mcast_fd <= poll.maxfd && poll.sock[ mcast_fd ] != NULL ) {
          this->remote_addr( poll.sock[ mcast_fd ]->peer_address.buf );
          poll.sock[ mcast_fd ]->client_stats( this->stats );
        }
        (this->*put)();
      }
    }
  }
  else if ( this->unrouteable != NULL ) {
    Unrouteable &u = *this->unrouteable;
    int fd = -1, fd2 = -1;
    if ( u.telnet != NULL )
      fd = u.telnet->fd;
    else if ( u.web != NULL )
      fd = u.web->fd;
    else if ( u.name != NULL ) {
      fd  = u.name->mcast_recv.fd;
      fd2 = u.name->mcast_send.fd;
    }
    if ( fd != -1 && (uint32_t) fd <= poll.maxfd && poll.sock[ fd ] != NULL ) {
      this->init( *u.tport, P_IS_LOCAL, fd );
      poll.sock[ fd ]->client_stats( this->stats );
      if ( fd2 != -1 && (uint32_t) fd2 <= poll.maxfd &&
           poll.sock[ fd2 ] != NULL ) {
        PeerAddrStr paddr;
        PeerAddrStr & maddr = poll.sock[ fd ]->peer_address;
        paddr.set_sock_addr( fd2 );
        size_t len = paddr.len() + maddr.len() + 8;
        char * tmp = this->console.tmp.str_make( len );
        int    x   = ::snprintf( tmp, len, "%s;%s", paddr.buf, maddr.buf );
        this->local_addr( tmp, min_int( x, (int) len - 1 ) );
        poll.sock[ fd2 ]->client_stats( this->stats );
      }
      else {
        this->local_addr( poll.sock[ fd ]->peer_address.buf );
      }
      (this->*put)();
    }
  }
}

void
PortOutput::put_show_ports( void ) noexcept
{
  TabPrint *tab = this->out.add_row_p();
  uint32_t i = 0;
  const char * type = this->type->val;
  if ( ( this->flags & P_IS_INBOX ) != 0 )
    type = "inbox";
  tab[ i++ ].set( *this->tport, this->tport_id, PRINT_ID );
  tab[ i++ ].set( type ); /* type */
  if ( this->rte != NULL && ! this->rte->is_set( TPORT_IS_IPC ) ) {
    tab[ i++ ].set_int( this->rte->uid_connected.rem_tport_id ); /* rem */
  }
  else {
    tab[ i++ ].set_null();
  }
  if ( ( this->flags & P_IS_DOWN ) == 0 && this->fd >= 0 )
    tab[ i++ ].set_int( this->fd ); /* fd */
  else
    tab[ i++ ].set_null();
  if ( this->stats.bytes_sent != 0 )
    tab[ i++ ].set_long( this->stats.bytes_sent ); /* bs */
  else
    tab[ i++ ].set_null();
  if ( this->stats.bytes_recv != 0 )
    tab[ i++ ].set_long( this->stats.bytes_recv ); /* br */
  else
    tab[ i++ ].set_null();
  if ( this->stats.msgs_sent != 0 )
    tab[ i++ ].set_long( this->stats.msgs_sent ); /* ms */
  else
    tab[ i++ ].set_null();
  if ( this->stats.msgs_recv != 0 )
    tab[ i++ ].set_long( this->stats.msgs_recv ); /* mr */
  else
    tab[ i++ ].set_null();
  if ( this->n != NULL )
    tab[ i++ ].set_long( this->n->round_trip_time, PRINT_LATENCY ); /* lat */
  else
    tab[ i++ ].set_null();
  if ( this->stats.active_ns > 0 )
    tab[ i++ ].set_long( this->cur_time - this->stats.active_ns,
                         PRINT_LATENCY ); /* wridle */
  else
    tab[ i++ ].set_null();
  if ( this->stats.read_ns > 0 )
    tab[ i++ ].set_long( this->cur_time - this->stats.read_ns,
                         PRINT_LATENCY ); /* rdidle */
  else
    tab[ i++ ].set_null();

  tab[ i++ ].set_int( this->state, PRINT_TPORT_STATE );

  if ( ( this->flags & P_IS_DOWN ) != 0 ) {
    tab[ i++ ].set_null();
  }
  else if ( ( this->flags & P_IS_LOCAL ) != 0 ) { /* address */
    if ( ! this->local.is_null() )
      tab[ i++ ].set_url( type, this->local );
    else
      tab[ i++ ].set_null();
  }
  else {
    if ( ! this->remote.is_null() ) {
      if ( this->n != NULL )
        tab[ i++ ].set_url_dest( this->n, type, this->remote );
      else
        tab[ i++ ].set_url( type, this->remote );
    }
    else
      tab[ i++ ].set_null();
  }
}

void
Console::show_ports( ConsoleOutput *p, const char *name,  size_t len ) noexcept
{
  static const uint32_t ncols = 13;
  size_t count = this->user_db.transport_tab.count;

  if ( len == 1 && name[ 0 ] == '*' )
    len = 0;
  TabOut out( this->table, this->tmp, ncols );
  for ( size_t t = 0; t < count; t++ ) {
    TransportRoute *rte = this->user_db.transport_tab.ptr[ t ];

    if ( len != 0 ) {
      if ( len != rte->transport.tport.len )
        continue;
      if ( ::memcmp( name, rte->transport.tport.val, len ) != 0 )
        continue;
    }
    PortOutput port( *this, out, (uint32_t) t );
    port.output( &PortOutput::put_show_ports );
  }
  for ( size_t u = 0; u < this->mgr.unrouteable.count; u++ ) {
    PortOutput port( *this, out, &this->mgr.unrouteable.ptr[ u ] );
    port.output( &PortOutput::put_show_ports );
  }
  static const char *hdr[ ncols ] =
    { "tport", "type", "rem", "fd", "bs", "br",
      "ms", "mr", "lat", "wridle", "rdidle", "fl", "address" };
  this->print_table( p, hdr, ncols );
}

void
PortOutput::put_show_cost( void ) noexcept
{
  TabPrint *tab = this->out.add_row_p();
  uint32_t i = 0;
  const char * type = this->type->val;
  if ( ( this->flags & P_IS_INBOX ) != 0 )
    type = "inbox";
  tab[ i++ ].set( *this->tport, this->tport_id, PRINT_ID );
  tab[ i++ ].set( type ); /* type */
  if ( this->rte != NULL && ! this->rte->is_set( TPORT_IS_IPC ) ) {
    tab[ i++ ].set_int( this->rte->uid_connected.cost[ 0 ] ); /* cost */
    tab[ i++ ].set_int( this->rte->uid_connected.cost[ 1 ] );
    tab[ i++ ].set_int( this->rte->uid_connected.cost[ 2 ] );
    tab[ i++ ].set_int( this->rte->uid_connected.cost[ 3 ] );
    tab[ i++ ].set_int( this->rte->uid_connected.rem_tport_id );
  }
  else {
    tab[ i++ ].set_null();
    tab[ i++ ].set_null();
    tab[ i++ ].set_null();
    tab[ i++ ].set_null();
    tab[ i++ ].set_null();
  }
  if ( ( this->flags & P_IS_DOWN ) == 0 && this->fd >= 0 )
    tab[ i++ ].set_int( this->fd ); /* fd */
  else
    tab[ i++ ].set_null();

  tab[ i++ ].set_int( this->state, PRINT_TPORT_STATE );

  if ( ( this->flags & P_IS_DOWN ) != 0 ) {
    tab[ i++ ].set_null();
  }
  else if ( ( this->flags & P_IS_LOCAL ) != 0 ) { /* address */
    if ( ! this->local.is_null() )
      tab[ i++ ].set_url( type, this->local );
    else
      tab[ i++ ].set_null();
  }
  else {
    if ( ! this->remote.is_null() ) {
      if ( this->n != NULL )
        tab[ i++ ].set_url_dest( this->n, type, this->remote );
      else
        tab[ i++ ].set_url( type, this->remote );
    }
    else
      tab[ i++ ].set_null();
  }
}

void
Console::show_cost( ConsoleOutput *p, const char *name,  size_t len ) noexcept
{
  static const uint32_t ncols = 10;
  size_t count = this->user_db.transport_tab.count;

  if ( len == 1 && name[ 0 ] == '*' )
    len = 0;
  TabOut out( this->table, this->tmp, ncols );
  for ( size_t t = 0; t < count; t++ ) {
    TransportRoute *rte = this->user_db.transport_tab.ptr[ t ];

    if ( len != 0 ) {
      if ( len != rte->transport.tport.len )
        continue;
      if ( ::memcmp( name, rte->transport.tport.val, len ) != 0 )
        continue;
    }
    PortOutput port( *this, out, (uint32_t) t );
    port.output( &PortOutput::put_show_cost );
  }
  for ( size_t u = 0; u < this->mgr.unrouteable.count; u++ ) {
    PortOutput port( *this, out, &this->mgr.unrouteable.ptr[ u ] );
    port.output( &PortOutput::put_show_cost );
  }
  static const char *hdr[ ncols ] =
    { "tport", "type", "cost", "cost2",
      "cost3", "cost4", "rem", "fd", "fl", "address" };
  this->print_table( p, hdr, ncols );
}

void
PortOutput::put_status( void ) noexcept
{
  TabPrint *tab = this->out.add_row_p();
  uint32_t i = 0;
  const char * type = this->type->val;
  if ( ( this->flags & P_IS_INBOX ) != 0 )
    type = "inbox";
  tab[ i++ ].set( *this->tport, this->tport_id, PRINT_ID );
  tab[ i++ ].set( type ); /* type */
  if ( ( this->flags & P_IS_DOWN ) == 0 && this->fd >= 0 )
    tab[ i++ ].set_int( this->fd ); /* fd */
  else
    tab[ i++ ].set_null();

  tab[ i++ ].set_int( this->state, PRINT_TPORT_STATE );

  if ( ( this->flags & P_IS_DOWN ) != 0 ) {
    char status_buf[ 256 ];
    size_t len = this->rte->port_status( status_buf, sizeof( status_buf ) );
    if ( len != 0 )
      this->console.tab_string( status_buf, tab[ i++ ] );
    else
      tab[ i++ ].set_null();
  }
  else if ( ( this->flags & P_IS_LOCAL ) != 0 ) { /* address */
    if ( ! this->local.is_null() )
      tab[ i++ ].set_url( type, this->local );
    else
      tab[ i++ ].set_null();
  }
  else {
    if ( ! this->remote.is_null() && this->n != NULL )
      tab[ i++ ].set_url_dest( this->n, type, this->remote );
    else
      tab[ i++ ].set_null();
  }
  tab[ i-1 ].typ |= PRINT_LEFT;
}

void
Console::show_status( ConsoleOutput *p, const char *name,  size_t len ) noexcept
{
  static const uint32_t ncols = 5;
  size_t count = this->user_db.transport_tab.count;

  if ( len == 1 && name[ 0 ] == '*' )
    len = 0;
  TabOut out( this->table, this->tmp, ncols );
  for ( size_t t = 0; t < count; t++ ) {
    TransportRoute *rte = this->user_db.transport_tab.ptr[ t ];

    if ( len != 0 ) {
      if ( len != rte->transport.tport.len )
        continue;
      if ( ::memcmp( name, rte->transport.tport.val, len ) != 0 )
        continue;
    }
    PortOutput port( *this, out, (uint32_t) t );
    port.output( &PortOutput::put_status );
  }
  static const char *hdr[ ncols ] = { "tport", "type", "fd", "fl", "status" };
  this->print_table( p, hdr, ncols );
}

bool
Unrouteable::is_active( void ) const
{
  return
    ( ( this->telnet != NULL && this->telnet->in_list( IN_ACTIVE_LIST ) ) ||
      ( this->web != NULL && this->web->in_list( IN_ACTIVE_LIST ) ) );
}

void
Console::show_tports( ConsoleOutput *p, const char *name,  size_t len ) noexcept
{
  static const uint32_t ncols = 6;
  TabOut           out( this->table, this->tmp, ncols );
  size_t           t, count = this->user_db.transport_tab.count;
  TransportRoute * rte;

  if ( len == 1 && name[ 0 ] == '*' )
    len = 0;

  for ( ConfigTree::Transport * tport = this->tree.transports.hd;
        tport != NULL; tport = tport->next ) {
    if ( len != 0 ) {
      if ( len != tport->tport.len )
        continue;
      if ( ::memcmp( name, tport->tport.val, len ) != 0 )
        continue;
    }
    for ( t = 0; t < count; t++ ) {
      rte = this->user_db.transport_tab.ptr[ t ];
      if ( &rte->transport == tport )
        break;
    }
    if ( t == count )
      rte = NULL;

    const char * listen  = NULL,
               * connect = NULL,
               * device  = NULL;
    int          port    = 0;
    tport->get_route_int( R_PORT, port );
    tport->get_route_str( R_LISTEN, listen );
    tport->get_route_str( R_CONNECT, connect );
    tport->get_route_str( R_DEVICE, device );

    char   buf[ 80 ];
    size_t len = sizeof( buf );
    bool   is_accepting = false;

    if ( rte != NULL )
      is_accepting = ( rte->listener != NULL );
    else if ( this->mgr.unrouteable.is_active( tport ) )
      is_accepting = true;
    else if ( this->user_db.ipc_transport != NULL ) {
      for ( IpcRte *ext = this->user_db.ipc_transport->ext->list.hd;
            ext != NULL; ext = ext->next ) {
        if ( tport == &ext->transport ) {
          rte = this->user_db.ipc_transport;
          is_accepting = true;
          break;
        }
      }
    }
    const char * state_string;
    if ( is_accepting )
      state_string = "accepting";
    else if ( rte == NULL )
      state_string = "-";
    else if ( rte->is_set( TPORT_IS_SHUTDOWN ) )
      state_string = "shutdown";
    else if ( rte->is_set( TPORT_IS_IPC ) )
      state_string = "ipc";
    else if ( rte->is_mcast() )
      state_string = "joined";
    else
      state_string = "connected";

    TabPrint & listen_col =
    out.add_row()
       .set( tport->tport )  /* tport */
       .set( tport->type )   /* type */
       .set( state_string ); /* state */

    TabPrint & connect_col = listen_col.set_null();
    TabPrint & device_col  = connect_col.set_null();
    device_col.set_null();

    if ( listen != NULL ) {
      size_t off = ::snprintf( buf, len, "%s://%s", tport->type.val, listen );
      if ( port != 0 && off < len )
        ::snprintf( &buf[ off ], len - off, ":%u", port );
      this->tab_string( buf, listen_col ); /* listen */
    }
    if ( connect != NULL ) {
      size_t off = ::snprintf( buf, len, "%s://%s", tport->type.val, connect );
      if ( port != 0 && off < len )
        ::snprintf( &buf[ off ], len - off, ":%u", port );
      this->tab_string( buf, connect_col ); /* connect */
    }
    if ( device != NULL ) {
      size_t off = ::snprintf( buf, len, "%s://%s", tport->type.val, device );
      if ( port != 0 && off < len )
        ::snprintf( &buf[ off ], len - off, ":%u", port );
      this->tab_string( buf, device_col ); /* connect */
    }
  }
  static const char *hdr[ ncols ] = { "tport", "type", "state", "listen",
                                      "connect", "device" };
  this->print_table( p, hdr, ncols );
}

int
UserBridgeList::cmp_user( const UserBridgeElem &e1,
                          const UserBridgeElem &e2 ) noexcept
{
  UserBridge * n1 = e1.user_db.bridge_tab.ptr[ e1.uid ],
             * n2 = e1.user_db.bridge_tab.ptr[ e2.uid ];
  if ( n1 != NULL && n2 != NULL )
    return ::strcmp( n1->peer.user.val, n2->peer.user.val );
  if ( n1 == NULL )
    return ::strcmp( e1.user_db.user.user.val, n2->peer.user.val );
  return ::strcmp( n1->peer.user.val, e1.user_db.user.user.val );
}

int
UserBridgeList::cmp_nonce( const UserBridgeElem &e1,
                           const UserBridgeElem &e2 ) noexcept
{
  UserBridge * n1 = e1.user_db.bridge_tab.ptr[ e1.uid ],
             * n2 = e1.user_db.bridge_tab.ptr[ e2.uid ];
  if ( n1 != NULL && n2 != NULL )
    return n1->bridge_id.nonce.cmp( n2->bridge_id.nonce );
  if ( n1 == NULL )
    return e1.user_db.bridge_id.nonce.cmp( n2->bridge_id.nonce );
  return n1->bridge_id.nonce.cmp( e1.user_db.bridge_id.nonce );
}

int
UserBridgeList::cmp_start( const UserBridgeElem &e1,
                           const UserBridgeElem &e2 ) noexcept
{
  UserBridge * n1 = e1.user_db.bridge_tab.ptr[ e1.uid ],
             * n2 = e1.user_db.bridge_tab.ptr[ e2.uid ];
  if ( n1 != NULL && n2 != NULL )
    return n1->start_time < n2->start_time ? -1 : 1;
  if ( n1 == NULL )
    return e1.user_db.start_time < n2->start_time ? -1 : 1;
  return n1->start_time < e1.user_db.start_time ? -1 : 1;
}

int
UserBridgeList::cmp_stop( const UserBridgeElem &e1,
                          const UserBridgeElem &e2 ) noexcept
{
  UserBridge * n1 = e1.user_db.bridge_tab.ptr[ e1.uid ],
             * n2 = e1.user_db.bridge_tab.ptr[ e2.uid ];
  return n1->remove_auth_time > n2->remove_auth_time ? -1 : 1;
}

void
UserBridgeList::add_users( UserDB &user_db,  md::MDMsgMem &mem ) noexcept
{
  this->push_tl( new ( mem.make( sizeof( UserBridgeElem ) ) )
                 UserBridgeElem( user_db, 0 ) );
  for ( uint32_t uid = 1; uid < user_db.next_uid; uid++ ) {
    UserBridge * n = user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
      continue;

    this->push_tl( new ( mem.make( sizeof( UserBridgeElem ) ) )
                   UserBridgeElem( user_db, uid ) );
  }
}

void
UserBridgeList::add_zombie( UserDB &user_db,  md::MDMsgMem &mem ) noexcept
{
  for ( uint32_t uid = 1; uid < user_db.next_uid; uid++ ) {
    UserBridge * n = user_db.bridge_tab[ uid ];
    if ( n == NULL || n->is_set( AUTHENTICATED_STATE ) )
      continue;

    this->push_tl( new ( mem.make( sizeof( UserBridgeElem ) ) )
                   UserBridgeElem( user_db, uid ) );
  }
}

void
Console::show_peers( ConsoleOutput *p, const char *name,  size_t len ) noexcept
{
  static const uint32_t ncols = 11;
  TabOut         out( this->table, this->tmp, ncols );
  const char   * nstr;
  char           nonce[ NONCE_B64_LEN + 1 ];
  UserBridgeList list;
  bool           show_ip = false, show_host = false, show_zombie = false;

  /* if name == user              , sort by user name
   *         == nonce | ip | host , sort by nonce
   *         == start             , sort by start time */
  if ( len > 0 ) {
    if ( name[ 0 ] == 'z' || name[ 0 ] == 'Z' )       /* zombie */
      show_zombie = true;
  }
  if ( ! show_zombie )
    list.add_users( this->user_db, this->tmp );
  else
    list.add_zombie( this->user_db, this->tmp );

  if ( len > 0 ) {
    if ( name[ 0 ] == 'u' || name[ 0 ] == 'U' )       /* user */
      list.sort<UserBridgeList::cmp_user>();
    else if ( name[ 0 ] == 'n' || name[ 0 ] == 'N' || /* nonce */
              name[ 0 ] == 'h' || name[ 0 ] == 'H' || /* host */
              name[ 0 ] == 'i' || name[ 0 ] == 'I' )  /* ip */
      list.sort<UserBridgeList::cmp_nonce>();
    else if ( name[ 0 ] == 's' || name[ 0 ] == 'S' )  /* start */
      list.sort<UserBridgeList::cmp_start>();
    else if ( name[ 0 ] == 'z' || name[ 0 ] == 'Z' )  /* zombie */
      list.sort<UserBridgeList::cmp_stop>();

    if ( name[ 0 ] == 'i' || name[ 0 ] == 'I' )       /* ip */
      show_ip = true;
    else if ( name[ 0 ] == 'h' || name[ 0 ] == 'H' )  /* host */
      show_host = true;
  }
  for ( UserBridgeElem * el = list.hd; el != NULL; el = el->next ) {
    if ( el->uid == MY_UID ) {
      if ( show_ip )
        sassrv::RvMcast::ip4_string( this->user_db.host_id, nonce );
      else if ( show_host )
        sassrv::RvMcast::ip4_hex_string( this->user_db.host_id, nonce );
      else
        this->user_db.bridge_id.nonce.to_base64_str( nonce );
      nstr = this->tmp.stralloc( ::strlen( nonce ), nonce );
      out.add_row()
         .set( this->user_db.user.user, PRINT_SELF )  /* user */
         .set( nstr ) /* bridge */
         .set_long( this->sub_db.bloom.bits->count )  /* sub */
         .set_long( this->sub_db.sub_seqno )          /* seq */
         .set_long( this->user_db.link_state_seqno )  /* link */
         .set_null()  /* lat */
         .set_null()  /* max */
         .set_null()  /* avg */
         .set_time( this->user_db.start_time )
         .set_null()  /* tport */
         .set_null(); /* cost */
      continue;
    }
    UserBridge * n = this->user_db.bridge_tab[ el->uid ];

    if ( show_ip )
      sassrv::RvMcast::ip4_string( n->host_id, nonce );
    else if ( show_host )
      sassrv::RvMcast::ip4_hex_string( n->host_id, nonce );
    else
      n->bridge_id.nonce.to_base64_str( nonce );
    nstr = this->tmp.stralloc( ::strlen( nonce ), nonce );
    uint64_t rtt     = 0,
             rtt_max = 0,
             rtt_sum = 0,
             rtt_avg = 0;
    for ( uint32_t k = 0; k < n->rtt.count; k++ ) {
      rtt = n->rtt.ptr[ k ].latency;
      rtt_sum += rtt;
      if ( rtt > rtt_max )
        rtt_max = rtt;
    }
    if ( n->rtt.count > 0 )
      rtt_avg = rtt_sum / n->rtt.count;
    rtt = n->round_trip_time;
    TabPrint & row =
    out.add_row()
       .set( n, PRINT_USER )             /* user */
       .set( nstr )                      /* bridge */
       .set_long( n->bloom.bits->count ) /* sub */
       .set_long( n->sub_seqno )         /* seq */
       .set_long( n->link_state_seqno )  /* link */
       .set_long( rtt, PRINT_LATENCY )   /* lat */
       .set_long( rtt_max, PRINT_LATENCY )  /* max */
       .set_long( rtt_avg, PRINT_LATENCY ); /* avg */

    if ( ! show_zombie ) {
      UserRoute *u_ptr = n->primary( this->user_db );

      if ( ! u_ptr->is_valid() ) {
        row.set_time( n->start_time )
           .set_null()  /* tport */
           .set_null(); /* cost */
      }
      else {
        row.set_time( n->start_time )
           .set( u_ptr->rte.transport.tport, u_ptr->rte.tport_id, PRINT_ID )
           .set( n, PRINT_DIST ); /* cost */
      }
    }
    else {
      row.set_time( n->remove_auth_time ) /* time */
         .set( auth_stage_string( n->last_auth_type ) )
         .set_null();
    }
  }
  static const char *hdr[ ncols ] = { "user", "bridge", "sub", "seq", "link",
                                      "lat", "max", "avg", "time", "tport",
                                      "cost"  };
  this->print_table( p, hdr, ncols );
}

void
Console::output_user_route( TabPrint &ptp,  UserRoute &u_rte ) noexcept
{
  EvPoll     & poll = this->mgr.poll;
  const char * url_type = u_rte.rte.transport.type.val;

  switch ( u_rte.is_set( UCAST_URL_STATE | UCAST_URL_SRC_STATE |
                         MESH_URL_STATE ) ) {
    default: { /* normal tcp */
      uint32_t ucast_fd = (uint32_t) u_rte.inbox.fd;
      if ( ucast_fd <= poll.maxfd && poll.sock[ ucast_fd ] != NULL ) {
        uint32_t     uid2;
        bool         found    = false;
        const char * address  = poll.sock[ ucast_fd ]->peer_address.buf;
        uint32_t     addr_len = (uint32_t) get_strlen64( address );
        if ( u_rte.rte.uid_connected.first( uid2 ) ) {
          if ( uid2 != u_rte.n.uid ) { /* if routing through another uid */
            UserBridge * n = this->user_db.bridge_tab[ uid2 ];
            if ( n != NULL && n->is_set( AUTHENTICATED_STATE ) ) { /* ptp */
              ptp.set_url_dest( n, url_type, address, addr_len );
              found = true;
            }
          }
        }
        if ( ! found )
          ptp.set_url( url_type, address, addr_len ); /* ptp */
      }
      else {
        ptp.set_null();
      }
      break;
    }
    case UCAST_URL_STATE:
      ptp.set( u_rte.ucast_url.val, u_rte.ucast_url.len ); /* ptp */
      break;
    case UCAST_URL_SRC_STATE: {
      const UserRoute & u_src = *u_rte.ucast_src;
      ptp.set_url_dest( &u_src.n, NULL, /* address */
                    u_src.ucast_url.val, u_src.ucast_url.len, PRINT_UADDR );
      break;
    }
    case MESH_URL_STATE:
      ptp.set( u_rte.mesh_url.val, u_rte.mesh_url.len ); /* ptp */
      break;
  }
}

void
Console::show_adjacency( ConsoleOutput *p,  const uint32_t *src_uid,
                         uint16_t path_select,  bool has_path ) noexcept
{
  size_t     ncols = 6;
  TabOut     out( this->table, this->tmp, ncols );
  TabPrint * tab = NULL;
  uint32_t   count, i = 0, r = 0, sep,
             last_user, last_tport;
  bool       is_json = ( p != NULL && p->is_json );
  UserBridgeList list;
  AdjDistance & peer_dist = this->user_db.peer_dist;

  sep = i;
  last_user = last_tport = -1;

  /* if name == user              , sort by user name
   *         == nonce | ip | host , sort by nonce
   *         == start             , sort by start time */
  list.add_users( this->user_db, this->tmp );
  list.sort<UserBridgeList::cmp_start>();

  /* print each users port */
  ArrayCount<uint32_t, 32> uid_map;
  uint32_t start_order = 0;
  for ( UserBridgeElem * el = list.hd; el != NULL; el = el->next ) {
    uid_map[ el->uid ] = start_order++;
  }
  for ( UserBridgeElem * el = list.hd; el != NULL; el = el->next ) {
    UserBridge * n = NULL;
    if ( src_uid != NULL && *src_uid != el->uid )
      continue;
    if ( el->uid != 0 )
      n = this->user_db.bridge_tab[ el->uid ];
    StringVal & user = ( n == NULL ? this->user_db.user.user : n->peer.user );
    count = peer_dist.adjacency_count( el->uid );
    if ( count == 0 )
      continue;
    last_tport = -1;
    /* for each tport populated */
    for ( uint32_t t = 0; t < count; t++ ) {
      AdjacencySpace *set = peer_dist.adjacency_set( el->uid, t );
      uint32_t b;
      if ( set == NULL )
        continue;
      /* for each user on the port */
      for ( bool ok = set->first( b ); ok; ok = set->next( b ) ) {
        UserBridge * n2 = NULL;
        if ( b != 0 )
          n2 = this->user_db.bridge_tab[ b ];
        StringVal &user2 = ( n2 == NULL ? this->user_db.user.user :
                             n2->peer.user );
        if ( b == 0 || n2 != NULL ) {
          tab = out.make_row();
          if ( last_user != el->uid ) {
            uint32_t uid = uid_map.ptr[ el->uid ];
            tab[ i++ ].set( user, uid, PRINT_ID );
          }
          else
            tab[ i++ ].set_null();
          uint32_t uid2 = uid_map.ptr[ b ];
          tab[ i++ ].set( user2, uid2, PRINT_ID );

          if ( last_tport != t ) {
            if ( set->tport.len > 0 )
              tab[ i++ ].set( set->tport, t, PRINT_ID );
            else
              tab[ i++ ].set_int( t );
            if ( set->tport_type.len > 0 )
              tab[ i++ ].set( set->tport_type );
            else
              tab[ i++ ].set_null();
          }
          while ( i < r+4 )
            tab[ i++ ].set_null();

          if ( ! has_path ) {
            char cost_buf[ 64 ];
            size_t sz = set->cost.str_size( cost_buf, sizeof( cost_buf ) );
            tab[ i++ ].set( this->tmp.stralloc( sz, cost_buf ) );
          }
          else {
            tab[ i++ ].set_int( set->cost[ path_select ] );
          }

          if ( set->rem_tport_id != 0 ) {
            if ( set->rem_uid == b )
              tab[ i++ ].set_int( set->rem_tport_id );
          }
          r += ncols;
          if ( i < r )
            tab[ i++ ].set_null();
          last_user  = el->uid;
          last_tport = t;
        }
      }
    }
    if ( i > sep ) {
      tab[ i - 1 ].typ |= PRINT_SEP;
      sep = i;
    }
  }
  const char *hdr[ 6 ] =
   { "user", "adj", "tport", "type", "cost", "rem" };
  this->print_table( p, hdr, ncols );

  if ( ! is_json ) {
    uint32_t path_cnt = peer_dist.get_path_count();
    this->printf( "consistent: %s, ",
      peer_dist.found_inconsistency ? "false" : "true" );
    if ( has_path )
      this->printf( "path %u of %u\n", path_select, path_cnt );
    else
      this->printf( "path count: %u\n", path_cnt );
    char lbuf1[ 80 ], lbuf2[ 80 ];
    latency_string( peer_dist.adjacency_run_time, lbuf1 );
    latency_string( peer_dist.adjacency_this_time, lbuf2 );
    this->printf( "current calc: %u, cpu: %s; "
                  "total calc: %u, total cpu: %s\n",
      peer_dist.adjacency_this_count, lbuf2,
      peer_dist.adjacency_run_count, lbuf1 );

    for (;;) {
      UserBridge * from, * to;
      int state = peer_dist.find_inconsistent2( from, to );
      if ( state == AdjDistance::CONSISTENT )
        break;
      const char * u1   = ( from == NULL ? this->user_db.user.user.val :
                            from->peer.user.val );
      const char * u2   = ( to == NULL ? this->user_db.user.user.val :
                            to->peer.user.val );
      uint32_t     uid1 = ( from == NULL ? 0 : from->uid ),
                   uid2 = ( to == NULL ? 0 : to->uid );
      printf( "find_inconsistent state %s from %s.%u to %s.%u\n",
              state == AdjDistance::LINK_MISSING ? "link missing" : "orphaned",
              u1, uid1, u2, uid2 );
    }
  }
}

void
Console::show_links( ConsoleOutput *p ) noexcept
{
  static const size_t ncols = 4;
  TabOut   out( this->table, this->tmp, ncols );
  size_t   count;
  uint32_t uid, i, last_uid = -1;
  TabPrint * tab;

  count = this->user_db.transport_tab.count;
  for ( size_t t = 0; t < count; t++ ) {
    TransportRoute *rte = this->user_db.transport_tab.ptr[ t ];
    /* print users on the tport */
    for ( bool ok = rte->uid_connected.first( uid ); ok;
          ok = rte->uid_connected.next( uid ) ) {
      UserBridge * n = this->user_db.bridge_tab[ uid ];
      if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
        continue;

      tab = out.add_row_p();
      i = 0;
      if ( last_uid != 0 ) {
        tab[ i++ ].set( this->user_db.user.user, PRINT_SELF );
        last_uid = 0;
      }
      else {
        tab[ i++ ].set_null();
      }
      tab[ i++ ].set( n, PRINT_USER );
      tab[ i++ ].set( rte->transport.tport, (uint32_t) t, PRINT_ID );
      tab[ i++ ].set_int( rte->uid_connected.rem_tport_id );
    }
  }
  for ( uid = 1; uid < this->user_db.next_uid; uid++ ) {
    UserBridge * n = this->user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
      continue;
    if ( out.table.count > 0 )
      out.row( ncols - 1 ).typ |= PRINT_SEP;
    count = n->adjacency.count;
    /* for each tport populated */
    for ( uint32_t j = 0; j < count; j++ ) {
      AdjacencySpace *set = n->adjacency[ j ];
      uint32_t b;
      if ( set == NULL )
        continue;
      /* for each user on the port */
      for ( bool ok = set->first( b ); ok; ok = set->next( b ) ) {
        tab = out.add_row_p();
        i = 0;
        if ( last_uid != uid ) {
          UserBridge * n = this->user_db.bridge_tab[ uid ];
          if ( n == NULL )
            tab[ i++ ].set( this->user_db.user.user, PRINT_SELF );
          else
            tab[ i++ ].set( n, PRINT_USER );
          last_uid = uid;
        }
        else {
          tab[ i++ ].set_null();
        }

        UserBridge * m = this->user_db.bridge_tab[ b ];
        if ( m == NULL )
          tab[ i++ ].set( this->user_db.user.user, PRINT_SELF );
        else
          tab[ i++ ].set( m, PRINT_USER );
        if ( set->tport.len > 0 )
          tab[ i++ ].set( set->tport, j, PRINT_ID );
        else
          tab[ i++ ].set_int( j );
        tab[ i++ ].set_int( set->rem_tport_id );
      }
    }
  }
  const char *hdr[ ncols ] = { "source", "target", "tport", "rem" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_nodes( ConsoleOutput *p ) noexcept
{
  static const size_t ncols = 3;
  TabOut    out( this->table, this->tmp, ncols );
  size_t    count;
  StringVal user;
  uint32_t  uid;

  count = this->user_db.transport_tab.count;
  out.add_row()
     .set( this->user_db.user.user )
     .set_int( 0 )
     .set_int( count );

  for ( uid = 1; uid < this->user_db.next_uid; uid++ ) {
    UserBridge * n = this->user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) ) {
      user.zero();
      count = 0;
    }
    else {
      user  = n->peer.user;
      count = n->adjacency.count;
    }
    out.add_row()
       .set( user )
       .set_int( uid )
       .set_int( count );
  }
  const char *hdr[ ncols ] = { "user", "uid", "tports" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_routes( ConsoleOutput *p,  const UserBridge *src,
                      uint16_t path_select ) noexcept
{
  static const uint32_t ncols = 9;
  TabOut       out( this->table, this->tmp, ncols );
  uint32_t     i = 0;
  const char * address;
  uint32_t     addr_len, ucast_fd, mcast_fd, uid;
  EvPoll     & poll = this->mgr.poll;
  bool         first_tport;

  if ( src != NULL )
    uid = src->uid;
  else
    uid = 1;
  for ( ; uid < this->user_db.next_uid; uid++ ) {
    UserBridge * n = this->user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
      continue;

    TabPrint * tab = out.make_row();
    if ( i > 0 )
      tab[ i - 1 ].typ |= PRINT_SEP;
    tab[ i++ ].set( n, PRINT_USER ); /* user */

    uint32_t count = this->user_db.transport_tab.count;
    first_tport = true;
    for ( uint32_t tport_id = 0; tport_id < count; tport_id++ ) {
      UserRoute *u_ptr = n->user_route_ptr( this->user_db, tport_id, 28 );
      if ( ! u_ptr->is_valid() )
        continue;

      if ( ! first_tport ) {
        tab = out.make_row();
        tab[ i++ ].set_null(); /* user */
      }
      else {
        first_tport = false;
      }
      TransportRoute *rte = this->user_db.transport_tab.ptr[ tport_id ];
      uint32_t cost = this->user_db.peer_dist.calc_transport_cache( uid,
                                                        tport_id, path_select );
      tab[ i++ ].set( rte->transport.tport, tport_id, PRINT_ID );
      tab[ i++ ].set_int( rte->uid_connected.rem_tport_id );
      tab[ i++ ].set_int( u_ptr->state, PRINT_STATE ); /* state */

      if ( cost != COST_MAXIMUM )
        tab[ i++ ].set_int( cost );  /* cost */
      else
        tab[ i++ ].set( "X" );  /* cost */
      uint64_t b[ CABA_PATH_COUNT / 64 ];
      uint16_t path_cnt = 0, max_path = 0;
      ::memset( b, 0, sizeof( b ) );
      for ( uint16_t path = 0; path < n->bloom_rt.count; path++ ) {
        if ( n->bloom_rt[ path ] != NULL &&
             n->bloom_rt[ path ]->r == (uint32_t) u_ptr->mcast.fd ) {
          b[ path / 64 ] |= 1U << ( path % 64 );
          path_cnt++;
          max_path = path;
        }
      }
      if ( path_cnt == 0 )
        tab[ i++ ].set_null();
      else if ( max_path < 64 )
        tab[ i++ ].set_long( b[ 0 ], PRINT_BITS );
      else {
        uint32_t sz = ( max_path + 63 ) / 64;
        size_t n = uint64_to_bits( b, sz, NULL );
        char * s = this->tmp.str_make( n + 1 );
        uint64_to_bits( b, sz, s );
        tab[ i++ ].set( s );
      }
      if ( n->primary_route == tport_id )
        tab[ i++ ].set_long( n->round_trip_time, PRINT_LATENCY ); /* lat */
      else
        tab[ i++ ].set_null();
      tab[ i++ ].set_int( u_ptr->mcast.fd );
      const char * url_type = u_ptr->rte.transport.type.val;
      switch ( u_ptr->is_set( UCAST_URL_STATE | UCAST_URL_SRC_STATE |
                              MESH_URL_STATE ) ) {
        case MESH_URL_STATE:
        #if 0
          if ( cost == 1 ) {
            tab[ i++ ].set( u_ptr->mesh_url.val, u_ptr->mesh_url.len ); /* ptp */
            break;
          }
        #endif
          /* fall through */
        default: {
          ucast_fd = u_ptr->inbox.fd;
          if ( ucast_fd <= poll.maxfd && poll.sock[ ucast_fd ] != NULL ) {
            uint32_t uid2;
            bool found = false;
            address  = poll.sock[ ucast_fd ]->peer_address.buf;
            addr_len = (uint32_t) get_strlen64( address );
            if ( cost > 1 && u_ptr->rte.uid_connected.first( uid2 ) ) {
              UserBridge * n = this->user_db.bridge_tab[ uid2 ];
              if ( n != NULL && n->is_set( AUTHENTICATED_STATE ) ) { /* ptp */
                tab[ i++ ].set_url_dest( n, url_type, address, addr_len );
                found = true;
              }
            }
            if ( ! found )
              tab[ i++ ].set_url( url_type, address, addr_len ); /* ptp */
          }
          else {
            tab[ i++ ].set_null();
          }
          break;
        }
        case UCAST_URL_STATE:
          tab[ i++ ].set( u_ptr->ucast_url.val, u_ptr->ucast_url.len ); /* ptp */
          tab = out.make_row();

          mcast_fd = rte->mcast.fd;
          if ( mcast_fd <= poll.maxfd && poll.sock[ mcast_fd ] != NULL ) {
            for ( uint32_t k = 0; k < ncols - 1; k++ ) {
              tab[ i++ ].set_null();
            }
            address  = poll.sock[ mcast_fd ]->peer_address.buf;
            addr_len = (uint32_t) get_strlen64( address );
            tab[ i++ ].set_url( url_type, address, addr_len );
          }
          break;
        case UCAST_URL_SRC_STATE: {
          const UserRoute & u_src = *u_ptr->ucast_src;
          tab[ i++ ].set_url_dest( &u_src.n, NULL, /* ptp */
                        u_src.ucast_url.val, u_src.ucast_url.len, PRINT_UADDR );
          break;
        }
      }
    }
    if ( src != NULL )
       break;
  }
  static const char *hdr[ ncols ] = { "user", "tport", "rem", "state",
                                      "cost", "path", "lat", "fd", "route" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_urls( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 9;
  TabOut     out( this->table, this->tmp, ncols );
  EvPoll   & poll = this->mgr.poll;
  TabPrint * tab;
  uint32_t   i = 0;

  for ( uint32_t tid = 0; tid < this->user_db.transport_tab.count; tid++ ) {
    TransportRoute *rte = this->user_db.transport_tab.ptr[ tid ];
    uint32_t uid;
    tab = out.add_row_p();
    i = 0;
    if ( rte->uid_connected.first( uid ) ) {
      UserBridge * n = this->user_db.bridge_tab[ uid ];
      if ( n != NULL && n->is_set( AUTHENTICATED_STATE ) )
        tab[ i++ ].set( n, PRINT_USER ); /* user */
    }
    if ( i == 0 ) {
      tab[ i++ ].set_null(); /* user */
    }
    tab[ i++ ].set( rte->transport.tport, rte->tport_id, PRINT_ID );
    tab[ i++ ].set_int( rte->state, PRINT_TPORT_STATE ); /* state */
    tab[ i++ ].set_null(); /* cost */

    if ( rte->mesh_id == NULL ) /* mesh */
      tab[ i++ ].set_null();
    else
      tab[ i++ ].set( rte->mesh_id->transport.tport );
    tab[ i++ ].set_int( rte->fd );

    if ( ! rte->mesh_url.is_null() ) /* url */
      tab[ i++ ].set( rte->mesh_url.val, rte->mesh_url.len );
    else if ( ! rte->ucast_url.is_null() )
      tab[ i++ ].set( rte->ucast_url.val, rte->ucast_url.len );
    else if ( ! rte->conn_url.is_null() )
      tab[ i++ ].set( rte->conn_url.val, rte->conn_url.len );
    else
      tab[ i++ ].set_null();

    uint32_t fd;
    for ( bool ok = rte->connected.first( fd ); ok;
          ok = rte->connected.next( fd ) ) {
      if ( fd <= poll.maxfd && poll.sock[ fd ] != NULL ) {
        EvSocket   * s = poll.sock[ fd ];
        const char * url_type = rte->transport.type.val,
                   * address;
        uint32_t     addr_len;
        if ( i == ncols ) {
          tab = out.add_row_p();
          for ( i = 0; i < ncols - 2; )
            tab[ i++ ].set_null();
        }
        PeerAddrStr paddr;
        paddr.set_sock_addr( fd );
        this->tab_url( url_type, paddr.buf,
          (uint32_t) get_strlen64( paddr.buf ), tab[ i++ ] ); /* local */
        address  = s->peer_address.buf;
        addr_len = (uint32_t) get_strlen64( address );
        tab[ i++ ].set_url( url_type, address, addr_len ); /* remote */
      }
    }
    while ( i < ncols )
      tab[ i++ ].set_null();
  }
  for ( uint32_t uid = 1; uid < this->user_db.next_uid; uid++ ) {
    UserBridge * n = this->user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
      continue;

    if ( out.table.count > 0 )
      out.row( ncols - 1 ).typ |= PRINT_SEP;
    tab = out.add_row_p();
    i = 0;
    tab[ i++ ].set( n, PRINT_USER ); /* user */

    uint32_t count = this->user_db.transport_tab.count;
    for ( uint32_t tport_id = 0; tport_id < count; tport_id++ ) {
      UserRoute *u_ptr = n->user_route_ptr( this->user_db, tport_id, 29 );
      if ( ! u_ptr->is_valid() )
        continue;

      if ( i == ncols ) {
        tab = out.add_row_p();
        i = 0;
        tab[ i++ ].set_null(); /* user */
      }
      TransportRoute *rte = this->user_db.transport_tab.ptr[ tport_id ];
      uint32_t cost =
        this->user_db.peer_dist.calc_transport_cache( uid, tport_id, 0 );
      tab[ i++ ].set( rte->transport.tport, tport_id, PRINT_ID );
      tab[ i++ ].set_int( u_ptr->state, PRINT_STATE ); /* state */

      if ( cost != COST_MAXIMUM )
        tab[ i++ ].set_int( cost );  /* cost */
      else
        tab[ i++ ].set( "X" );  /* cost */

      if ( rte->mesh_id != NULL )
        tab[ i++ ].set( rte->mesh_id->transport.tport ); /* mesh */
      else
        tab[ i++ ].set_null();
      tab[ i++ ].set_int( u_ptr->mcast.fd );

      const char * url_type = u_ptr->rte.transport.type.val,
                 * address;
      uint32_t     ucast_fd,
                   addr_len;
      switch ( u_ptr->is_set( UCAST_URL_STATE | UCAST_URL_SRC_STATE |
                              MESH_URL_STATE ) ) {
        case MESH_URL_STATE:
        default:
          if ( u_ptr->is_set( MESH_URL_STATE ) )
            tab[ i++ ].set( u_ptr->mesh_url.val, u_ptr->mesh_url.len ); /* ptp */
          else
            tab[ i++ ].set_null();
          ucast_fd = (uint32_t) u_ptr->inbox.fd;
          if ( ucast_fd <= poll.maxfd && poll.sock[ ucast_fd ] != NULL &&
               u_ptr->is_set( HAS_HB_STATE ) ) {
            PeerAddrStr paddr;
            paddr.set_sock_addr( ucast_fd );
            this->tab_url( url_type, paddr.buf,
              (uint32_t) get_strlen64( paddr.buf ), tab[ i++ ] ); /* local */
            address  = poll.sock[ ucast_fd ]->peer_address.buf;
            addr_len = (uint32_t) get_strlen64( address );
            tab[ i++ ].set_url( url_type, address, addr_len ); /* remote */
          }
          else {
            tab[ i++ ].set_null();
            tab[ i++ ].set_null();
          }
          break;

        case UCAST_URL_STATE:
          tab[ i++ ].set( u_ptr->ucast_url.val, u_ptr->ucast_url.len ); /* ptp */
          /* fallthru */
          if ( 0 ) {
        case UCAST_URL_SRC_STATE:
            const UserRoute & u_src = *u_ptr->ucast_src;
            tab[ i++ ].set_url_dest( &u_src.n, NULL, /* ptp */
                        u_src.ucast_url.val, u_src.ucast_url.len, PRINT_UADDR );
          }
          ucast_fd = (uint32_t) u_ptr->inbox.fd;
          if ( ucast_fd <= poll.maxfd && poll.sock[ ucast_fd ] != NULL ) {
            address  = poll.sock[ ucast_fd ]->peer_address.buf;
            addr_len = (uint32_t) get_strlen64( address );
            tab[ i++ ].set_url( "inbox", address, addr_len ); /* local */
          }
          else {
            tab[ i++ ].set_null();
          }
          tab[ i++ ].set_null();
          break;
      }
    }
  }
  static const char *hdr[ ncols ] = { "user", "tport", "state",
                                      "cost", "mesh", "fd", "url",
                                      "local", "remote" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_counters( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 8;
  TabOut out( this->table, this->tmp, ncols );

  for ( uint32_t uid = 0; uid < this->user_db.next_uid; uid++ ) {

    if ( uid == MY_UID ) {
      TabPrint *tab = out.add_row_p();
      uint32_t  i = 0;
      tab[ i++ ].set( this->user_db.user.user, PRINT_SELF ); /* user */
      tab[ i++ ].set_time( this->user_db.start_time );       /* start */
      while ( i < ncols )
        tab[ i++ ].set_null();
      continue;
    }
    UserBridge * n = this->user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
      continue;

    out.add_row()
       .set( n, PRINT_USER )                  /* user */
       .set_time( n->start_time )             /* start */
       .set_long( n->hb_seqno )               /* hb */
       .set_time( n->hb_time )                /* hb_time */
       .set_long( n->ping_send_count )        /* pisnd */
       .set_time( n->ping_send_time )         /* ping_stime */
       .set_long( n->pong_recv_count )        /* porcv */
       .set_long( n->ping_recv_count );       /* pircv */
  }
  static const char *hdr[ ncols ] =
    { "user", "start", "hb seqno", "hb time",
      "ping snd", "ping stime", "pong rcv", "ping rcv" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_sync( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 12;
  TabOut out( this->table, this->tmp, ncols );

  for ( uint32_t uid = 0; uid < this->user_db.next_uid; uid++ ) {

    if ( uid == MY_UID ) {
      TabPrint *tab = out.add_row_p();
      uint32_t  i = 0;
      tab[ i++ ].set( this->user_db.user.user, PRINT_SELF ); /* user */
      tab[ i++ ].set_time( this->user_db.start_time );       /* start */
      tab[ i++ ].set_long( this->user_db.link_state_seqno );
      tab[ i++ ].set_long( this->user_db.link_state_sum );
      tab[ i++ ].set_long( this->sub_db.sub_seqno );
      tab[ i++ ].set_long( this->sub_db.sub_seqno_sum );
      tab[ i++ ].set_null();
      tab[ i++ ].set_null();
      tab[ i++ ].set_null();
      tab[ i++ ].set_null();
      tab[ i++ ].set_null();
      tab[ i++ ].set_null();
      continue;
    }
    UserBridge * n = this->user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
      continue;

    out.add_row()
       .set( n, PRINT_USER )
       .set_time( n->start_time )
       .set_long( n->link_state_seqno )
       .set_long( n->link_state_sum )
       .set_long( n->sub_seqno )
       .set_long( n->sub_seqno_sum )
       .set_int( n->sync_sum_diff )
       .set_int( n->sync_sum_req )
       .set_int( n->sync_sum_res )
       .set_int( n->sync_sum_req_adj )
       .set_int( n->sync_sum_res_adj )
       .set_int( n->ping_sync_adj );
  }
  static const char *hdr[ ncols ] =
  { "user", "start", "link_seqno", "link_sum", "sub_seqno", "sub_sum",
    "hb_diff", "mc_req", "mc_res", "req_adj", "res_adj", "ping_adj" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_pubtype( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 3;
  TabOut out( this->table, this->tmp, ncols );

  for ( uint32_t k = 0; k < MAX_PUB_TYPE; k++ ) {
    uint32_t cnt = this->mgr.msg_recv_counter[ k ] |
                   this->user_db.msg_send_counter[ k ];
    if ( cnt != 0 ) {
      TabPrint *tab = out.add_row_p();
      uint32_t  i = 0;
      tab[ i++ ].set( publish_type_to_string( (PublishType) k ) );

      cnt = this->mgr.msg_recv_counter[ k ];
      tab[ i++ ].set_int( cnt );

      cnt = this->user_db.msg_send_counter[ k ];
      tab[ i++ ].set_int( cnt );
    }
  }
  static const char *hdr[ ncols ] =
    { "type", "recv_count", "send_count" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_inbox( ConsoleOutput *p,  const char *arg,
                     size_t arglen ) noexcept
{
  static const uint32_t ncols = 5;
  TabOut out( this->table, this->tmp, ncols );

  for ( uint32_t uid = 1; uid < this->user_db.next_uid; uid++ ) {
    UserBridge * n = this->user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
      continue;
    if ( arglen > 0 && ! n->peer.user.equals( arg, arglen ) )
      continue;

    if ( out.table.count > 0 )
      out.row( ncols - 1 ).typ |= PRINT_SEP;

    bool first = true;
    uint32_t u,
             cnt = max_int( n->inbox.seqno.count, n->inbox_recv.seqno.count );

    for ( u = U_INBOX_AUTH; u <= U_INBOX + cnt; u++ ) {
      static const char * path_str[ MAX_PATH_MASK + 1 ];
      uint64_t     recv_seqno,
                   send_seqno,
                   miss_seqno;
      const char * type_str;

      if ( u > U_INBOX ) {
        uint32_t x = u - U_INBOX;
        recv_seqno = n->inbox_recv.seqno[ x ];
        miss_seqno = ( x < n->inbox_miss.seqno.count ? n->inbox_miss.seqno[ x ] : 0 );
        send_seqno = n->inbox.seqno[ x ];
        type_str   = path_str[ x ];
        if ( type_str == NULL ) {
          CatPtr p( (char *) ::malloc( 16 ) );
          p.s( publish_type_to_string( U_INBOX ) ).s( "_" ).u( x ).end();
          path_str[ x ] = type_str = p.start;
        }
      }
      else {
        recv_seqno = n->inbox_recv.zpath_seqno[ u ];
        miss_seqno = ( u < n->inbox_miss.seqno.count ? n->inbox_miss.seqno[ u ] : 0 );
        send_seqno = n->inbox.zpath_seqno[ u ];
        type_str   = publish_type_to_string( (PublishType) u );
      }

      if ( send_seqno + recv_seqno + miss_seqno != 0 ) {
        TabPrint * tab = out.add_row_p();
        uint32_t i = 0;
        if ( first ) {
          tab[ i++ ].set( n, PRINT_USER );    /* user */
          first = false;
        }
        else {
          tab[ i++ ].set_null();
        }
        tab[ i++ ].set( type_str );
        tab[ i++ ].set_long( recv_seqno );
        tab[ i++ ].set_long( send_seqno );
        tab[ i++ ].set_long( miss_seqno );
      }
    }
  }
  static const char *hdr[ ncols ] =
    { "user", "inbox type", "recv seqno", "send seqno", "loss" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_loss( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 11;
  TabOut out( this->table, this->tmp, ncols );

  for ( uint32_t uid = 1; uid < this->user_db.next_uid; uid++ ) {
    UserBridge * n = this->user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
      continue;

    out.add_row()
       .set( n, PRINT_USER )                 /* user */
       .set_long( n->null_route_count )      /* null rte */
       .set_time( n->null_route_time )       /* null time */
       .set_long( n->msg_repeat_count )      /* repeat */
       .set_time( n->msg_repeat_time )       /* rep time */
       .set_long( n->msg_not_subscr_count )  /* not sub */
       .set_time( n->msg_not_subscr_time )   /* not time */
       .set_long( n->msg_loss_count )        /* msg loss */
       .set_time( n->msg_loss_time )         /* loss time */
       .set_long( n->inbox_msg_loss_count )  /* ibx loss */
       .set_time( n->inbox_msg_loss_time );  /* ibx time */
  }
  static const char *hdr[ ncols ] =
    { "user", "null rte", "null time", "repeat", "rep time", "not sub",
      "not time", "msg loss", "loss time", "ibx loss", "ibx time" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_skew( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 7;
  TabOut out( this->table, this->tmp, ncols );
  uint64_t cur_time = current_realtime_ns();

  for ( uint32_t uid = 1; uid < this->user_db.next_uid; uid++ ) {
    UserBridge * n = this->user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
      continue;

    out.add_row()
       .set( n, PRINT_USER )
       .set_long( n->round_trip_time, PRINT_LATENCY ) /* lat */
       .set_long( n->hb_skew, PRINT_LATENCY )
       .set_int( n->hb_skew_ref )
       .set_long( n->ping_skew, PRINT_LATENCY )
       .set_long( n->pong_skew, PRINT_LATENCY )
       .set_time( cur_time - this->user_db.min_skew( *n ) );
  }
  static const char *hdr[ ncols ] =
    { "user", "lat", "hb", "ref", "ping", "pong", "time" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_reachable( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 5;
  TabOut out( this->table, this->tmp, ncols );
  uint32_t fd;

  for ( fd = 0; fd < this->user_db.route_list.count; fd++ ) {
    UserRouteList & list = this->user_db.route_list[ fd ];
    if ( list.is_empty() )
      continue;

    if ( out.table.count > 0 )
      out.row( ncols - 1 ).typ |= PRINT_SEP;
    for ( UserRoute * u_ptr = list.hd; u_ptr != NULL; u_ptr = u_ptr->next ) {
      UserBridge & n   = u_ptr->n;
      TabPrint   * tab = out.add_row_p();
      uint32_t     i   = 0;

      tab[ i++ ].set( &n, PRINT_USER );
      if ( u_ptr->hops() == 0 )
        tab[ i++ ].set( "*" );
      else
        tab[ i++ ].set_null();
      uint32_t b = 0;
      for ( uint16_t path = 0; path < n.bloom_rt.count; path++ ) {
        if ( n.bloom_rt[ path ] != NULL &&
             n.bloom_rt[ path ]->r == fd ) {
          b |= 1U << path;
        }
      }
      if ( b == 0 )
        tab[ i++ ].set_null();
      else
        tab[ i++ ].set_long( b, PRINT_BITS );
      if ( u_ptr == list.hd ) {
        tab[ i++ ].set_int( fd );
        tab[ i++ ].set( u_ptr->rte.transport.tport,
                        u_ptr->rte.tport_id, PRINT_ID );
      }
      else {
        tab[ i++ ].set_null();
        tab[ i++ ].set_null();
      }
    }
  }
  static const char *hdr[ ncols ] = { "user", "c", "path", "fd", "tport" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_tree( ConsoleOutput *p,  const UserBridge *src,
                    uint16_t path_select ) noexcept
{
  static const uint32_t ncols = 4;
  TabOut out( this->table, this->tmp, ncols );
  AdjDistance & peer_dist = this->user_db.peer_dist;
  uint32_t      src_uid = ( src != NULL ? src->uid : 0 );

  if ( peer_dist.graph == NULL )
    peer_dist.update_graph( true );

  uint32_t   * idx = peer_dist.graph_idx_order;
  AdjUser    * u   = peer_dist.graph->user_tab.ptr[ idx[ src_uid ] ];
  AdjFwdTab  & fwd = u->fwd[ path_select ];

  for ( uint32_t src = 0; src < u->links.count; src++ ) {
    uint32_t j;
    for ( j = 0; j < fwd.links.count; j++ ) {
      if ( fwd.src.ptr[ j ] == src )
        break;
    }
    if ( j < fwd.links.count ) {
      if ( out.table.count > 0 )
        out.row( ncols - 1 ).typ |= PRINT_SEP;
      for (;;) {
        TabPrint * tab = out.add_row_p();
        uint32_t   i = 0;
        tab[ i++ ].set_int( fwd.cost.ptr[ j ] );               /* cost */
        AdjLink * link = fwd.links.ptr[ j ];
        tab[ i++ ].set( link->a.user, link->a.uid, PRINT_ID ); /* source */
        tab[ i++ ].set( link->tport, link->tid, PRINT_ID );    /* tport */
        tab[ i++ ].set( link->b.user, link->b.uid, PRINT_ID ); /* dest */

        for (;;) {
          if ( ++j == fwd.links.count )
            goto break_loop;
          if ( src == fwd.src.ptr[ j ] )
            break;
        }
      }
    break_loop:;
    }
  }
  static const char *hdr[ ncols ] =
    { "cost", "source", "tport", "dest" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_path( ConsoleOutput *p,  uint16_t path_select ) noexcept
{
  static const uint32_t ncols = 4;
  TabOut out( this->table, this->tmp, ncols );

  AdjDistance  & peer_dist = this->user_db.peer_dist;
  ForwardCache & forward   = this->user_db.forward_path[ path_select ];

  peer_dist.update_path( forward, path_select );

  uint32_t count = peer_dist.max_uid;
  for ( uint32_t uid = 1; uid < count; uid++ ) {
    UidSrcPath path  = forward.path[ uid ];
    /*uint32_t path_cost = peer_dist.calc_transport_cache( uid, path.tport,
                                                         path_select );*/
    if ( path.cost != 0 ) {
      UserBridge * n = this->user_db.bridge_tab.ptr[ uid ];
      TransportRoute * rte = this->user_db.transport_tab.ptr[ path.tport ];

      out.add_row()
         .set( rte->transport.tport, path.tport, PRINT_ID )
         .set_int( rte->uid_connected.cost[ path_select ] )  /* cost */
         .set_int( path.cost )  /* path_cost */
         .set( n, PRINT_USER ); /* user */
    }
  }

  static const char *hdr[ ncols ] =
    { "tport", "cost", "path_cost", "dest" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_forward( ConsoleOutput *p,  uint16_t path_select ) noexcept
{
  static const uint32_t ncols = 5;
  TabOut out( this->table, this->tmp, ncols );

  for ( uint32_t uid = 0; uid < this->user_db.next_uid; uid++ ) {
    UserBridge * n = NULL;
    if ( uid != 0 ) {
      n = this->user_db.bridge_tab[ uid ];
      if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
        continue;
    }
    if ( out.table.count > 0 )
      out.row( ncols - 1 ).typ |= PRINT_SEP;

    TabPrint * tab = out.add_row_p();
    uint32_t   i = 0;

    if ( uid == MY_UID )
      tab[ i++ ].set( this->user_db.user.user, PRINT_SELF ); /* user */
    else
      tab[ i++ ].set( n, PRINT_USER );  /* user */
    ForwardCache & forward = ( uid == MY_UID ?
                               this->user_db.forward_path[ path_select ] :
                               n->forward_path[ path_select ] );
    TransportRoute * rte;
    uint32_t         tport_id;
    this->user_db.peer_dist.update_source_path( forward, uid, path_select );
    if ( forward.first( tport_id ) ) {
      do {
        rte = this->user_db.transport_tab.ptr[ tport_id ];

        if ( i == ncols ) {
          tab = out.add_row_p();
          i = 0;
          tab[ i++ ].set_null(); /* user */
        }
        tab[ i++ ].set( rte->transport.tport, tport_id, PRINT_ID );
        uint32_t u = rte->uid_connected.rem_uid,
                 t = rte->uid_connected.rem_tport_id;
        if ( t != 0 ) {
          UserBridge *dest = this->user_db.bridge_tab[ u ];
          if ( dest == NULL )
            tab[ i++ ].set( this->user_db.user.user, PRINT_SELF ); /* user */
          else
            tab[ i++ ].set( dest, PRINT_USER );  /* user */
          tab[ i++ ].set_int( t );
        }
        else {
          tab[ i++ ].set_null();
          tab[ i++ ].set_null();
        }
        tab[ i++ ].set_int( rte->uid_connected.cost[ path_select ] );
      } while ( forward.next( tport_id ) );
    }
    while ( i < ncols )
      tab[ i++ ].set_null();
  }

  static const char *hdr[ ncols ] =
    { "source", "tport", "dest", "rem", "cost" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_forward_all( ConsoleOutput *p ) noexcept
{
  this->show_forward( p, 0 );
#if 0
  static const uint32_t ncols = 9;
  TabOut out( this->table, this->tmp, ncols );

  for ( uint32_t uid = 0; uid < this->user_db.next_uid; uid++ ) {
    UserBridge * n = NULL;
    if ( uid != 0 ) {
      n = this->user_db.bridge_tab[ uid ];
      if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
        continue;
    }
    if ( out.table.count > 0 )
      out.row( ncols - 1 ).typ |= PRINT_SEP;

    TabPrint * tab = out.add_row_p();
    uint32_t   i = 0;
    uint16_t   p;

    if ( uid == MY_UID )
      tab[ i++ ].set( this->user_db.user.user, PRINT_SELF ); /* user */
    else
      tab[ i++ ].set( n, PRINT_USER );  /* user */

    uint32_t path_cnt = this->user_db.peer_dist.get_path_count();

    ForwardCache   * forward[ COST_PATH_COUNT ];
    TransportRoute * rte[ COST_PATH_COUNT ];
    uint32_t         tport_id[ COST_PATH_COUNT ];
    bool             b[ COST_PATH_COUNT ];
    for ( p = 0; p < path_cnt; p++ ) {
      if ( uid == MY_UID )
        forward[ p ] = &this->user_db.forward_path[ p ];
      else
        forward[ p ] = &n->forward_path[ p ];
      this->user_db.peer_dist.update_forward_cache( *forward[ p ], uid, p );
      rte[ p ]      = NULL;
      tport_id[ p ] = 0;
      b[ p ]        = false;
    }

    for ( uint32_t cnt = 0; ; cnt++ ) {
      uint32_t k = 0;
      for ( p = 0; p < COST_PATH_COUNT; p++ ) {
        if ( cnt == 0 )
          b[ p ] = forward[ p ]->first( tport_id[ p ] );
        else if ( b[ p ] )
          b[ p ] = forward[ p ]->next( tport_id[ p ] );

        if ( b[ p ] ) {
          rte[ p ] = this->user_db.transport_tab.ptr[ tport_id[ p ] ];
          k++;
        }
      }
      if ( k == 0 ) {
        while ( i < ncols )
          tab[ i++ ].set_null();
        break;
      }
      if ( i == ncols ) {
        tab = out.add_row_p();
        i = 0;
        tab[ i++ ].set_null();
      }
      for ( p = 0; p < COST_PATH_COUNT; p++ ) {
        if ( rte[ p ] != NULL ) {
          tab[ i++ ].set( rte[ p ]->transport.tport, tport_id[ p ], PRINT_ID );
          uint32_t u = rte[ p ]->uid_connected.rem_uid,
                   t = rte[ p ]->uid_connected.rem_tport_id;
          if ( t != 0 ) {
            UserBridge *dest = this->user_db.bridge_tab[ u ];
            if ( dest != NULL )
              tab[ i++ ].set( dest, PRINT_USER );  /* user */
            else
              tab[ i++ ].set_null();
          }
        }
        else {
          tab[ i++ ].set_null();
          tab[ i++ ].set_null();
        }
      }
    }
  }

  static const char *hdr[ ncols ] =
    { "source", "t0", "d0", "t1", "d1", "t2", "d2", "t3", "d3" };
  this->print_table( p, hdr, ncols );
#endif
}

void
Console::show_fds( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 16;
  TabOut out( this->table, this->tmp, ncols );
  EvPoll     & poll = this->mgr.poll;
  uint64_t     cur_time = poll.now_ns;
  const char * address;
  uint32_t     addr_len;

  for ( uint32_t fd = 0; fd <= poll.maxfd; fd++ ) {
    EvSocket *s = poll.sock[ fd ];
    if ( s != NULL ) {
      bool is_connection = ( s->sock_base == EV_CONNECTION_BASE );
      uint64_t sumb = s->bytes_sent + s->bytes_recv,
               summ = s->msgs_sent  + s->msgs_recv;
      TabPrint * tab = out.add_row_p();
      uint32_t   i = 0;
      tab[ i++ ].set_int( (uint32_t) fd );
      tab[ i++ ].set_int( s->route_id, PRINT_SINT );
      if ( sumb != 0 || is_connection )
        tab[ i++ ].set_long( s->bytes_sent );
      else
        tab[ i++ ].set_null();
      if ( sumb != 0 || is_connection )
        tab[ i++ ].set_long( s->bytes_recv );
      else
        tab[ i++ ].set_null();
      if ( summ != 0 || is_connection )
        tab[ i++ ].set_long( s->msgs_sent );
      else
        tab[ i++ ].set_null();
      if ( summ != 0 || is_connection )
        tab[ i++ ].set_long( s->msgs_recv );
      else
        tab[ i++ ].set_null();
      if ( s->sock_base == EV_LISTEN_BASE )
        tab[ i++ ].set_long( ((EvListen *) s)->accept_cnt );
      else
        tab[ i++ ].set_null();
      if ( s->active_ns > s->start_ns )
        tab[ i++ ].set_long( cur_time - s->active_ns, PRINT_LATENCY );
      else
        tab[ i++ ].set_null();
      if ( s->read_ns > s->start_ns )
        tab[ i++ ].set_long( cur_time - s->read_ns, PRINT_LATENCY );
      else
        tab[ i++ ].set_null();
      if ( s->sock_base == EV_CONNECTION_BASE ) {
        tab[ i++ ].set_long( ((EvConnection *) s)->len -
                             ((EvConnection *) s)->off );
        tab[ i++ ].set_long( ((EvConnection *) s)->pending() );
      }
      else {
        tab[ i++ ].set_null();
        tab[ i++ ].set_null();
      }
      tab[ i++ ].set_int( s->sock_state, PRINT_SOCK_STATE );
      tab[ i++ ].set( s->type_string() );
      tab[ i++ ].set( s->kind );
      tab[ i++ ].set( s->name );
      address  = s->peer_address.buf;
      addr_len = (uint32_t) get_strlen64( address );

      bool has_ptp_link = false;
      if ( ! this->user_db.route_list.is_empty( (uint32_t) fd ) ) {
        UserRoute * u_ptr = this->user_db.route_list[ fd ].hd;
        do {
          if ( u_ptr->hops() == 0 )
            break;
        } while ( (u_ptr = u_ptr->next) != NULL );
        if ( u_ptr != NULL && ! u_ptr->rte.is_mcast() ) {
          tab[ i++ ].set_url_dest( &u_ptr->n, NULL, address, addr_len,
                                   PRINT_UADDR );
          has_ptp_link = true;
        }
      }
      if ( ! has_ptp_link )
        tab[ i++ ].set( address, addr_len );
    }
  }

  static const char *hdr[ ncols ] =
    { "fd", "rid", "bs", "br", "ms", "mr", "ac", "wridle", "rdidle",
      "rq", "wq", "fl", "type", "kind", "name", "address" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_buffers( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 11;
  TabOut out( this->table, this->tmp, ncols );
  EvPoll     & poll = this->mgr.poll;

  for ( uint32_t fd = 0; fd <= poll.maxfd; fd++ ) {
    EvSocket *s = poll.sock[ fd ];
    if ( s != NULL && s->sock_base == EV_CONNECTION_BASE ) {
      EvConnection & conn = *(EvConnection *) s;
      uint64_t wused = 0, rused = 0,
               wmax  = 0, rmax  = 0;
      TabPrint * tab = out.add_row_p();
      uint32_t   i = 0;
      tab[ i++ ].set_int( (uint32_t) fd );
      wused = conn.wr_used;
      wmax  = conn.wr_max;
      size_t snd_size = conn.tmp.mem_size();
      if ( wused < snd_size )
        wused = snd_size;
      if ( wmax < snd_size )
        wmax = snd_size;
      rused = conn.recv_size;
      rmax  = conn.recv_max;

      tab[ i++ ].set_long( wused );
      tab[ i++ ].set_long( wmax );
      tab[ i++ ].set_long( rused );
      tab[ i++ ].set_long( rmax );
      tab[ i++ ].set_long( conn.zref_count );
      tab[ i++ ].set_long( conn.send_count );
      tab[ i++ ].set_long( conn.recv_count );
      tab[ i++ ].set_long( conn.malloc_count );
      tab[ i++ ].set_long( conn.palloc_count );
      tab[ i++ ].set( conn.name );
    }
  }

  static const char *hdr[ ncols ] =
 { "fd", "wr", "wmax", "rd", "rmax", "zref", "send", "recv", "mall", "pall", "name" };
  this->print_table( p, hdr, ncols );
}


template<class Tab>
static void
show_win( Tab &t,  TabOut &out,  const char *name,
          size_t size,  size_t count,  uint64_t start_time ) noexcept
{
  size_t   max_size  = t.max_size,
           max_count = t.max_count;
  uint64_t last_time = current_realtime_ns(),
           time      = 0,
           ival      = 0;
  uint32_t j;
  bool     b;

  if ( max_size < size )   max_size  = size;
  if ( max_count < count ) max_count = count;

  for ( uint32_t k = 0; ; k++ ) {
    TabPrint * tab = out.add_row_p();
    uint32_t i = 0;
    if ( k == 0 )
      tab[ i++ ].set( name );
    else
      tab[ i++ ].set_null();

    tab[ i++ ].set_long( count );
    tab[ i++ ].set_long( size );
    if ( k == 0 ) {
      b = t.wh.first( j, time, size, count );
      tab[ i++ ].set_null();
    }
    else {
      last_time = time;
      b = t.wh.next( j, time, size, count );
      tab[ i++ ].set_time( last_time );
    }
    if ( last_time != 0 && time != 0 )
      ival = last_time - time;
    else if ( last_time != 0 )
      ival = last_time - start_time;
    else
      ival = 0;
    if ( ival != 0 )
      tab[ i++ ].set_long( ival, PRINT_LATENCY );
    else
      tab[ i++ ].set_null();
    if ( k == 0 ) {
      tab[ i++ ].set_long( max_count );
      tab[ i++ ].set_long( max_size );
    }
    else {
      tab[ i++ ].set_null();
      tab[ i++ ].set_null();
    }
    if ( ! b )
      break;
  }
}

void
Console::show_windows( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 7;
  TabOut out( this->table, this->tmp, ncols );
  size_t   count, size;
  uint64_t last_time,
           start_time = this->user_db.start_time;
  uint32_t i;

  SeqnoTab & seq = this->sub_db.seqno_tab;
  size  = seq.tab->mem_size() + seq.seqno_ht_size;
  count = seq.tab->pop_count();
  show_win<SeqnoTab>( seq, out, "sub", size, count, start_time );

  PubTab & pub = this->sub_db.pub_tab;
  size  = pub.pub->mem_size();
  count = pub.pub->pop_count();
  show_win<PubTab>( pub, out, "pub", size, count, start_time );

  AnyMatchTab & any = this->sub_db.any_tab;
  size  = any.max_off * 8 + any.ht->mem_size();
  count = any.ht->elem_count;
  last_time = any.gc_time;

  out.add_row()
     .set( "inbox" )
     .set_long( count )
     .set_long( size )
     .set_time( last_time )
     .set_null()
     .set_null()
     .set_null();

  RoutePDB & rdb = this->mgr.poll.sub_route;
  count = 0;
  size  = 0;
  for ( i = 0; i < MAX_RTE; i++ ) {
    count += rdb.rt_hash[ i ]->elem_count;
    size  += rdb.rt_hash[ i ]->mem_size();
  }
  size += rdb.zip.code_buf.size * sizeof( rdb.zip.code_buf.ptr[ 0 ] ) +
          rdb.cache.spc.size * sizeof( rdb.cache.spc.ptr[ 0 ] );
  RouteLoc pos;
  for ( RouteService * s = rdb.svc_db.first( pos ); s != NULL;
        s = rdb.svc_db.next( pos ) ) {
    for ( i = 0; i < MAX_RTE; i++ ) {
      count += s->sub_route->rt_hash[ i ]->elem_count;
      size  += s->sub_route->rt_hash[ i ]->mem_size();
    }
    size += s->sub_route->zip.code_buf.size *
              sizeof( s->sub_route->zip.code_buf.ptr[ 0 ] ) +
            s->sub_route->cache.spc.size *
              sizeof( s->sub_route->cache.spc.ptr[ 0 ] );
  }
  out.add_row()
     .set( "route" )
     .set_long( count )
     .set_long( size )
     .set_null()
     .set_null()
     .set_null()
     .set_null();

  count = 0;
  size  = 0;
  for ( i = 0; i < this->mgr.poll.g_bloom_db.count; i++ ) {
    BloomRef * b = this->mgr.poll.g_bloom_db.ptr[ i ];
    if ( b != NULL ) {
      count += b->bits->count;
      size  += b->bits->bwidth + sizeof( *b->bits ) + sizeof( *b );
    }
  }
  out.add_row()
     .set( "bloom" )
     .set_long( count )
     .set_long( size )
     .set_null()
     .set_null()
     .set_null()
     .set_null();

  PeerMatchArgs kr( "rv", 2 );
  PeerMatchIter riter( this->mgr, kr );
  count = 0;
  size  = 0;
  for ( EvSocket *p = riter.first(); p != NULL; p = riter.next() ) {
    sassrv::EvRvService *svc = (sassrv::EvRvService *) p;

    count += svc->sub_tab.sub_count() + svc->pat_tab.sub_count;
    size  += svc->sub_tab.tab.mem_size() + svc->pat_tab.tab.mem_size();
  }
  if ( count > 0 ) {
    out.add_row()
       .set( "rv" )
       .set_long( count )
       .set_long( size )
       .set_null()
       .set_null()
       .set_null()
       .set_null();
  }

  PeerMatchArgs kn( "nats", 4 );
  PeerMatchIter niter( this->mgr, kn );
  count = 0;
  size  = 0;
  for ( EvSocket *p = niter.first(); p != NULL; p = niter.next() ) {
    natsmd::EvNatsService *svc = (natsmd::EvNatsService *) p;

    count += svc->map.sub_tab.pop_count() + svc->map.pat_tab.pop_count();
    size  += svc->map.sub_tab.mem_size() + svc->map.pat_tab.mem_size() +
             svc->map.sid_tab.mem_size();
  }
  if ( count > 0 ) {
    out.add_row()
       .set( "nats" )
       .set_long( count )
       .set_long( size )
       .set_null()
       .set_null()
       .set_null()
       .set_null();
  }

  static const char *hdr[ ncols ] =
 { "tab", "count", "size", "rotate_time", "interval", "max_count", "max_size" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_blooms( ConsoleOutput *p,  uint16_t path_select,
                      bool brief ) noexcept
{
  static const uint32_t ncols = 8;
  TabOut out( this->table, this->tmp, ncols );
  uint32_t count = this->user_db.transport_tab.count;

  for ( uint32_t t = 0; t < count; t++ ) {
    TransportRoute *rte = this->user_db.transport_tab.ptr[ t ];
    if ( out.table.count > 0 )
      out.row( ncols - 1 ).typ |= PRINT_SEP;

    for ( BloomRoute *p = rte->sub_route.bloom.list.hd( path_select );
          p != NULL; p = p->next ) {
      size_t sz = 0;
      char   buf[ 80 ];
      const char * sys = NULL;
      if ( p->r == (uint32_t) this->mgr.fd )
        sys = "session";
      else if ( p->r == (uint32_t) this->mgr.console_rt.fd )
        sys = "console";
      else if ( p->r == (uint32_t) this->mgr.ipc_rt.fd )
        sys = "ipc";
      else if ( p->r == (uint32_t) rte->fd )
        sys = "route";

      uint32_t j = 0;
      BloomRef * ref;

      if ( brief ) {
        if ( sys != NULL )
          continue;

        for ( ; j < p->nblooms; j++ ) {
          ref = p->bloom[ j ];
          if ( ref == &this->user_db.peer_bloom )
            continue;
          break;
        }
        if ( j == p->nblooms )
          continue;
      }

      TabPrint * tab = out.add_row_p();
      uint32_t   i = 0;
      tab[ i++ ].set_int( p->r );
      if ( sys != NULL )
        tab[ i++ ].set( sys );
      else {
        UserRoute * u_ptr = NULL;
        if ( ! this->user_db.route_list.is_empty( p->r ) ) {
          u_ptr = this->user_db.route_list[ p->r ].hd;
          do {
            if ( u_ptr->hops() == 0 )
              break;
          } while ( (u_ptr = u_ptr->next) != NULL );
        }
        if ( u_ptr != NULL ) {
          if ( ! u_ptr->rte.is_mcast() )
            tab[ i++ ].set( u_ptr->n.peer.user, u_ptr->n.uid );
          else
            tab[ i++ ].set( "(mcast)" );
        }
        else {
          tab[ i++ ].set_null();
        }
      }
      tab[ i++ ].set( rte->transport.tport, rte->tport_id, PRINT_ID );
      uint64_t pref_mask = 0, detail_mask = 0;
      uint32_t subs = 0, total = 0, last_ref = 0;
      size_t name_len;
      bool overflow = false;

      if ( p->r == (uint32_t) rte->fd ) {
        ::strcpy( buf, "(all-peers)" );
      }
      else {
        for ( ; j < p->nblooms; j++ ) {
          ref = p->bloom[ j ];
          if ( brief && ref == &this->user_db.peer_bloom )
            continue;
          pref_mask |= ref->pref_mask;
          detail_mask |= ref->detail_mask;
          total += (uint32_t) ref->bits->count;
          subs  += ref->pref_count[ SUB_RTE ];

          if ( ! overflow ) {
            name_len = ::strlen( ref->name );
            if ( name_len > 76 )
              name_len = 76;
            if ( sz + name_len + 3 < 80 ) {
              sz = cat80( buf, sz, ref->name );
              sz = cat80( buf, sz, ", " );
            }
            else {
              overflow = true;
              last_ref = j;
            }
          }
        }

        if ( sz > 1 ) sz -= 2; /* strip , */
        buf[ sz ] = '\0';
      }
      this->tab_string( buf, tab[ i++ ] );

      tab[ i ].set_long( pref_mask, PRINT_LONG_HEX )
              .set_long( detail_mask, PRINT_LONG_HEX )
              .set_int( subs )
              .set_int( total );

      while ( overflow ) {
        sz = 0;
        overflow = false;
        for ( j = last_ref; j < p->nblooms; j++ ) {
          ref = p->bloom[ j ];
          size_t name_len = ::strlen( ref->name );
          if ( name_len > 76 )
            name_len = 76;
          if ( sz + name_len + 3 < 80 ) {
            sz = cat80( buf, sz, ref->name );
            sz = cat80( buf, sz, ", " );
          }
          else {
            overflow = true;
            last_ref = j;
            break;
          }
        }
        if ( sz > 1 ) sz -= 2; /* strip , */
        buf[ sz ] = '\0';

        tab = out.add_row_p();
        i = 0;
        tab[ i++ ].set_null();
        tab[ i++ ].set_null();
        tab[ i++ ].set_null();
        this->tab_string( buf, tab[ i++ ] );
        tab[ i ].set_null()
                .set_null()
                .set_null()
                .set_null();
      }
    }
  }
  static const char *hdr[ ncols ] =
    { "fd", "dest", "tport", "bloom", "prefix", "detail", "subs", "total" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_match( ConsoleOutput *p,  const char *sub,  size_t len ) noexcept
{
  static const uint32_t ncols = 1;
  TabOut out( this->table, this->tmp, ncols );
  uint32_t       uid,
                 h   = kv_crc_c( sub, len, 0 );
  AnyMatch     * any = this->sub_db.any_match( sub, len, h );
  BloomMatch     match;
  BloomMatchArgs args( h, sub, len );

  match.init_match( len );
  if ( match.match_sub( args, this->sub_db.bloom ) ) {
    out.add_row()
       .set( this->user_db.user.user, PRINT_SELF ); /* user */
  }
  BitSetT<uint64_t> set( any->bits() );
  for ( bool b = set.first( uid, any->max_uid ); b;
        b = set.next( uid, any->max_uid ) ) {
    UserBridge * n = this->user_db.bridge_tab[ uid ];
    if ( n == NULL || ! n->is_set( AUTHENTICATED_STATE ) )
      continue;
    out.add_row()
       .set( n, PRINT_USER );
  }
  static const char *hdr[ ncols ] = { "user" };
  this->print_table( p, hdr, ncols );
}

void
Console::tab_pub( Pub *pub,  TabOut &out ) noexcept
{
  out.add_row()
     .set( "ipc", 3 )
     .set_long( seqno_base( pub->seqno ), PRINT_LONG )
     .set_long( seqno_time( pub->seqno ), PRINT_STAMP )
     .set_long( pub->stamp, PRINT_STAMP )
     .set( pub->value, pub->len );
}

void
Console::tab_seqno( SubSeqno *sub,  TabOut &out ) noexcept
{
  TabPrint * tab = out.add_row_p();
  UserBridge * n = this->user_db.bridge_tab.ptr[ sub->last_uid ];
  if ( n == NULL )
    tab[ 0 ].set( this->user_db.user.user, PRINT_SELF ); /* user */
  else
    tab[ 0 ].set( n, PRINT_USER ); /* user */
  tab[ 1 ].set_long( seqno_base( sub->last_seqno ), PRINT_LONG )
          .set_long( seqno_time( sub->last_seqno ), PRINT_STAMP )
          .set_long( sub->last_stamp, PRINT_STAMP )
          .set( sub->value, sub->len );

  if ( sub->seqno_ht != NULL ) {
    size_t pos;
    for ( bool ok = sub->seqno_ht->first( pos ); ok;
          ok = sub->seqno_ht->next( pos ) ) {
      SeqnoSave val;
      uint64_t  seqno, time, stamp;
      uint32_t  uid;
      sub->seqno_ht->get( pos, uid, val );
      val.restore( seqno, time, stamp );

      tab = out.add_row_p();
      n = this->user_db.bridge_tab.ptr[ uid ];
      if ( n == NULL )
        tab[ 0 ].set( this->user_db.user.user, PRINT_SELF ); /* user */
      else
        tab[ 0 ].set( n, PRINT_USER ); /* user */
      tab[ 1 ].set_long( seqno_base( seqno ), PRINT_LONG )
              .set_long( seqno_time( seqno ), PRINT_STAMP )
              .set_long( stamp, PRINT_STAMP )
              .set( sub->value, sub->len );
    }
  }
}

void
Console::show_seqno( ConsoleOutput *p,  const char *arg,
                     size_t arglen ) noexcept
{
  static const uint32_t ncols = 5;
  TabOut out( this->table, this->tmp, ncols );
  /*int        count = 0;*/
  bool       b;

  if ( arglen == 1 && arg[ 0 ] == '*' )
    arglen = 0;
#if 0
  if ( arglen != 0 ) {
    uint32_t h = kv_crc_c( arg, arglen, 0 );
    pub = this->sub_db.pub_tab.find( h, arg, arglen );
    if ( pub != NULL ) {
      this->tab_pub( pub, out );
      count++;
    }
    sub = this->sub_db.seqno_tab.find( h, arg, arglen );
    if ( sub != NULL ) {
      this->tab_seqno( sub, out );
      count++;
    }
  }
  if ( count == 0 ) {
#endif
    RouteLoc loc;
    Pub * pub;
    for ( pub = this->sub_db.pub_tab.first( loc, b ); pub != NULL;
          pub = this->sub_db.pub_tab.next( loc, b ) ) {
      if ( arglen == 0 || kv_memmem( pub->value, pub->len, arg, arglen ) != NULL)
        this->tab_pub( pub, out );
    }
    SubSeqno * sub;
    int which;
    for ( sub = this->sub_db.seqno_tab.first( loc, which ); sub != NULL;
          sub = this->sub_db.seqno_tab.next( loc, which ) ) {
      if ( arglen == 0 || kv_memmem( sub->value, sub->len, arg, arglen ) != NULL)
        this->tab_seqno( sub, out );
    }
#if 0
  }
#endif
  static const char *hdr[ ncols ] =
    { "source", "seqno", "start", "time", "subject" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_config( ConsoleOutput *p,  bool is_start,  int which,
                      const char *name,  size_t namelen ) noexcept
{
  const bool is_html = ( p != NULL && p->is_html ),
             is_json = ( p != NULL && p->is_json );
  ConfigTree * t = ( is_start ? this->startup.tree : &this->tree );
  if ( is_html )
    this->puts( "<pre>" );

  ConfigTree::TransportArray listen, connect;
  StringVal filter( name, namelen );

  if ( ! is_start )
    this->get_active_tports( listen, connect );
  else
    this->get_startup_tports( listen, connect );
  if ( ! is_json )
    t->print_y( *this, which, &filter, &listen, &connect );
  else
    t->print_js( *this, which, &filter, &listen, &connect );
}

void
Console::show_graph( ConsoleOutput *p ) noexcept
{
  const bool is_html = ( p != NULL && p->is_html ),
             is_json = ( p != NULL && p->is_json );
  AdjDistance & peer_dist = this->user_db.peer_dist;
  ArrayOutput out;
  if ( is_html )
    out.s( "<pre>" );
  peer_dist.message_graph_description( out );
  if ( ! is_json ) {
    p->on_output( out.ptr, out.count );
    return;
  }
  const char * buf = out.ptr,
             * end = &buf[ out.count ];
  bool first = true;

  while ( buf < end ) {
    size_t len = end - buf;
    const char *ptr = (const char *) ::memchr( buf, '\n', len );
    if ( ptr == NULL ) {
      ptr = &buf[ len ];
    }
    else {
      if ( ptr > buf && *( ptr - 1 ) == '\r' )
        ptr--;
    }
    const char * ln = buf;
    size_t       sz = ptr - ln;
    const char * q;
    p->on_output( first ? "[" : ",", 1 ); first = false;
    p->on_output( "\"", 1 );
    while ( (q = (char *) ::memchr( ln, '\"', sz )) != NULL ) {
      if ( q > ln )
        p->on_output( ln, q - ln );
      p->on_output( "\\\"", 2 );
      ln = &q[ 1 ];
      sz = ptr - ln;
    }
    if ( sz > 0 )
      p->on_output( ln, sz );
    p->on_output( "\"", 1 );

    buf = ptr;
    if ( buf < end && buf[ 0 ] == '\r' )
      buf++;
    if ( buf < end && buf[ 0 ] == '\n' )
      buf++;
  }
  if ( first )
    p->on_output( "[]\n", 3 );
  else
    p->on_output( "]\n", 2 );
}

void
Console::show_cache( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 6;
  TabOut out( this->table, this->tmp, ncols );
  uint32_t count = this->user_db.transport_tab.count;

  for ( uint32_t t = 0; t < count; t++ ) {
    TransportRoute *rte = this->user_db.transport_tab.ptr[ t ];
    uint64_t total   = ( rte->sub_route.cache.hit_cnt +
                         rte->sub_route.cache.miss_cnt );
    uint64_t hit_pct = 0;
    if ( total > 0 )
      hit_pct = ( rte->sub_route.cache.hit_cnt * 100 * 100 ) / total;
    out.add_row()
       .set( rte->name )
       .set_long( hit_pct, PRINT_PERCENT )
       .set_long( rte->sub_route.cache.hit_cnt )
       .set_long( rte->sub_route.cache.miss_cnt )
       .set_long( rte->sub_route.cache.max_cnt )
       .set_long( rte->sub_route.cache.max_size );
  }
  static const char *hdr[ ncols ] =
    { "tport", "hit_pct", "hit", "miss", "max_cnt", "max_size" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_poll( ConsoleOutput *p ) noexcept
{
  static const uint32_t ncols = 11;
  TabOut out( this->table, this->tmp, ncols );
  EvPoll & poll = this->mgr.poll;
  uint64_t timer_lat, read_lat, route_lat, write_lat,
           timer_cnt, read_cnt, route_cnt, write_cnt,
           rd_lo_cnt, wr_poll_cnt, wr_hi_cnt;

  timer_lat = poll.state_ns[ EV_READ_HI ];
  read_lat  = poll.state_ns[ EV_READ ]
            + poll.state_ns[ EV_READ_LO ];
  route_lat = poll.state_ns[ EV_PROCESS ];
  write_lat = poll.state_ns[ EV_WRITE ]
            + poll.state_ns[ EV_WRITE_POLL ]
            + poll.state_ns[ EV_WRITE_HI ];

  timer_cnt = poll.state_cnt[ EV_READ_HI ];
  read_cnt  = poll.state_cnt[ EV_READ ]
            + (rd_lo_cnt = poll.state_cnt[ EV_READ_LO ]);
  route_cnt = poll.state_cnt[ EV_PROCESS ];
  write_cnt = poll.state_cnt[ EV_WRITE ]
            + (wr_poll_cnt = poll.state_cnt[ EV_WRITE_POLL ])
            + (wr_hi_cnt = poll.state_cnt[ EV_WRITE_HI ]);

  if ( timer_cnt > 0 ) timer_lat /= timer_cnt;
  if ( read_cnt  > 0 ) read_lat  /= read_cnt;
  if ( route_cnt > 0 ) route_lat /= route_cnt;
  if ( write_cnt > 0 ) write_lat /= write_cnt;

  out.add_row()
     .set_long( timer_lat, PRINT_LATENCY )
     .set_long( timer_cnt )
     .set_long( read_lat, PRINT_LATENCY )
     .set_long( read_cnt )
     .set_long( rd_lo_cnt )
     .set_long( route_lat, PRINT_LATENCY )
     .set_long( route_cnt )
     .set_long( write_lat, PRINT_LATENCY )
     .set_long( write_cnt )
     .set_long( wr_poll_cnt )
     .set_long( wr_hi_cnt );

  static const char *hdr[ ncols ] =
    { "timer_lat", "timer_cnt", "read_lat", "read_cnt", "rd_lo",
      "route_lat", "route_cnt", "write_lat", "write_cnt", "wr_poll", "wr_hi" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_hosts( ConsoleOutput *p ) noexcept
{
  static const size_t ncols = 12;
  TabOut out( this->table, this->tmp, ncols );

  if ( this->user_db.transport_tab.count > 0 &&
       this->user_db.transport_tab.ptr[ 0 ]->rv_svc != NULL) {
    RvTransportService * rv_svc =
      this->user_db.transport_tab.ptr[ 0 ]->rv_svc;
    if ( rv_svc->db.host_tab != NULL ) {
      for ( uint32_t k = 0; k < rv_svc->db.host_tab->count; k++ ) {
        if ( rv_svc->db.host_tab->ptr[ k ] == NULL )
          continue;

        sassrv::RvHost & host = *rv_svc->db.host_tab->ptr[ k ];
        PeerMatchArgs ka;
        PeerMatchIter iter( this->mgr, ka );
        EvSocket    * sock;

        for ( sock = iter.first(); sock != NULL; sock = iter.next() ) {
          char sess[ EvSocket::MAX_SESSION_LEN ];
          size_t n;
          if ( (n = sock->get_session( host.service_num, sess )) > 0 ) {
            char   user[ EvSocket::MAX_USERID_LEN ],
                 * usr,
                 * str;
            size_t ulen = sock->get_userid( user );

            str = this->tmp.str_make( n + 1 + ulen + 1 );
            ::memcpy( str, sess, n );
            str[ n ] = '\0';
            usr = &str[ n + 1 ];
            ::memcpy( usr, user, ulen );
            usr[ ulen ] = '\0';

            out.add_row()
               .set( host.service, host.service_len )
               .set( str, n )
               .set( usr, ulen )
               .set_int( ntohs( host.ipport ) )
               .set_time( host.start_stamp )
               .set_int( host.active_clients )
               .set_long( host.stat.bs )
               .set_long( host.stat.br )
               .set_long( host.stat.ms )
               .set_long( host.stat.mr )
               .set_long( host.stat.idl )
               .set_long( host.stat.odl );
          }
        }
      }
    }
  }
  const char *hdr[ ncols ] =
   { "svc", "session", "user", "port", "start", "cl", "bs", "br", "ms", "mr",
     "idl", "odl" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_rvsub( ConsoleOutput *p ) noexcept
{
  static const size_t ncols = 5;
  TabOut     out( this->table, this->tmp, ncols );
  TabPrint * tab;
  uint32_t   i, k;

  if ( this->user_db.transport_tab.count > 0 &&
       this->user_db.transport_tab.ptr[ 0 ]->rv_svc != NULL) {
    RvTransportService * rv_svc =
      this->user_db.transport_tab.ptr[ 0 ]->rv_svc;
    if ( rv_svc->db.host_tab != NULL ) {
      for ( k = 0; k < rv_svc->db.host_tab->count; k++ ) {
        if ( rv_svc->db.host_tab->ptr[ k ] == NULL )
          continue;

        sassrv::RvHost & host = *rv_svc->db.host_tab->ptr[ k ];
        PeerMatchArgs ka;
        PeerMatchIter iter( this->mgr, ka );
        EvSocket    * sock;
        RouteSub    * s;
        RouteLoc      loc;

        SubRouteDB daemon;
        if ( host.get_subscriptions( host.service_num, daemon ) > 0 ) {
          if ( (s = daemon.first( loc )) != NULL ) {
            tab = out.add_row_p();
            tab[ 0 ].set( host.service, host.service_len )
                    .set( host.session_ip, 8 )
                    .set( "daemon", 6 )
                    .set_null();
            this->tab_string( s->value, s->len, tab[ 4 ] );
            tab[ 4 ].typ |= PRINT_LEFT;
          }
        }
        for ( sock = iter.first(); sock != NULL; sock = iter.next() ) {
          char sess[ EvSocket::MAX_SESSION_LEN ];
          size_t n;
          if ( (n = sock->get_session( host.service_num, sess )) > 0 ) {
            SubRouteDB   subs, pats;
            char         user[ EvSocket::MAX_USERID_LEN ];
            size_t       subcnt, patcnt;
            const char * kind = "p";

            sock->get_userid( user );
            subcnt = sock->get_subscriptions( host.service_num, subs );
            patcnt = sock->get_patterns( host.service_num, RV_PATTERN_FMT,
                                         pats );
            if ( patcnt == 0 ) {
              patcnt = sock->get_patterns( host.service_num, GLOB_PATTERN_FMT,
                                           pats );
              kind = "g";
            }
            tab = out.add_row_p();
            i = 0;
            tab[ i++ ].set( host.service, host.service_len );
            this->tab_string( sess, tab[ i++ ] );
            this->tab_string( user, tab[ i++ ] );
            if ( subcnt > 0 ) {
              for ( s = subs.first( loc ); s != NULL; s = subs.next( loc ) ) {
                if ( i == ncols ) {
                  tab = out.add_row_p();
                  i = 3;
                  tab[ 0 ].set_null().set_null().set_null();
                }
                tab[ i++ ].set_null();
                this->tab_string( s->value, s->len, tab[ i++ ] );
                tab[ i-1 ].typ |= PRINT_LEFT;
              }
            }
            if ( patcnt > 0 ) {
              for ( s = pats.first( loc ); s != NULL; s = pats.next( loc ) ) {
                if ( i == ncols ) {
                  tab = out.add_row_p();
                  i = 3;
                  tab[ 0 ].set_null().set_null().set_null();
                }
                tab[ i++ ].set( kind );
                this->tab_string( s->value, s->len, tab[ i++ ] );
                tab[ i-1 ].typ |= PRINT_LEFT;
              }
            }
            while ( i < ncols )
              tab[ i++ ].set_null();
          }
        }
      }
    }
  }
  const char *hdr[ ncols ] =
   { "svc", "session", "user", "p", "subject" };
  this->print_table( p, hdr, ncols );
}

void
Console::show_rpcs( ConsoleOutput *p ) noexcept
{
  static const size_t ncols = 4;
  TabOut out( this->table, this->tmp, ncols );
  ConsoleRemote    * rem;
  ConsoleSubs      * subs;
  ConsoleSubStart  * sub;
  ConsolePSubStart * psub;
  ConsoleSnap      * snap;

  for ( ConsoleRPC *rpc = this->rpc_list.hd; rpc != NULL; rpc = rpc->next ) {
    if ( rpc->is_complete )
      continue;
    switch ( rpc->type ) {
      case PING_RPC:
        out.add_row()
           .set( "ping" )
           .set_null()
           .set_int( rpc->total_recv )
           .set_int( rpc->count );
        break;
      case REMOTE_RPC:
        rem = (ConsoleRemote *) rpc;
        out.add_row()
           .set( "remote" )
           .set( rem->cmd, rem->cmd_len )
           .set_int( rem->total_recv )
           .set_int( rem->count );
        break;
      case SUBS_RPC:
        subs = (ConsoleSubs *) rpc;
        out.add_row()
           .set( "show subs" )
           .set( subs->match, subs->match_len )
           .set_int( subs->total_recv )
           .set_int( subs->count );
        break;
      case SUB_START:
        sub = (ConsoleSubStart *) rpc;
        out.add_row()
           .set( "sub" )
           .set( sub->sub, sub->sublen )
           .set_int( sub->total_recv )
           .set_null();
        break;
      case PSUB_START:
        psub = (ConsolePSubStart *) rpc;
        out.add_row()
           .set( "psub" )
           .set( psub->psub, psub->psublen )
           .set_int( psub->total_recv )
           .set_null();
        break;
      case SNAP_RPC:
        snap = (ConsoleSnap *) rpc;
        out.add_row()
           .set( "snap" )
           .set( snap->sub, snap->sublen )
           .set_int( snap->total_recv )
           .set_int( 1 );
        break;
      default: break;
    }
  }
  const char *hdr[ ncols ] =
   { "type", "arg", "recv", "count" };
  this->print_table( p, hdr, ncols );
}

size_t
Console::get_subscriptions( uint16_t svc,  SubRouteDB &subs ) noexcept
{
  RouteLoc loc;
  char     pre[ 8 ];
  size_t   cnt = 0;
  CatPtr p( pre );
  p.c( '_' ).u( svc, uint16_digits( svc ) ).c( '.' );
  size_t   prelen = p.len();

  for ( ConsoleRPC *rpc = this->rpc_list.hd; rpc != NULL; rpc = rpc->next ) {
    if ( rpc->is_complete )
      continue;
    if ( rpc->type != SUB_START )
      continue;
    ConsoleSubStart * sub = (ConsoleSubStart *) rpc;
    if ( sub->sublen > prelen && ::memcmp( sub->sub, pre, prelen ) == 0 ) {
      const char * val = &sub->sub[ prelen ];
      size_t       len = sub->sublen - prelen;
      uint32_t     h   = kv_crc_c( val, len, 0 );
      subs.upsert( h, val, len, loc );
      if ( loc.is_new )
        cnt++;
    }
  }
  return cnt;
}

size_t
Console::get_patterns( uint16_t svc,  int pat_fmt,  SubRouteDB &pats ) noexcept
{
  RouteLoc loc;
  char     pre[ 8 ];
  size_t   cnt = 0;
  CatPtr   p( pre );
  p.c( '_' ).u( svc, uint16_digits( svc ) ).c( '.' );
  size_t   prelen = p.len();

  for ( ConsoleRPC *rpc = this->rpc_list.hd; rpc != NULL; rpc = rpc->next ) {
    if ( rpc->is_complete )
      continue;
    if ( rpc->type != PSUB_START )
      continue;
    ConsolePSubStart * sub = (ConsolePSubStart *) rpc;
    if ( (int) sub->pat_fmt != pat_fmt )
      continue;
    if ( sub->psublen > prelen && ::memcmp( sub->psub, pre, prelen ) == 0 ) {
      const char * val = &sub->psub[ prelen ];
      size_t       len = sub->psublen - prelen;
      uint32_t     h   = kv_crc_c( val, len, 0 );
      pats.upsert( h, val, len, loc );
      if ( loc.is_new )
        cnt++;
    }
  }
  return cnt;
}

int
Console::puts( const char *s ) noexcept
{
  return this->out.puts( s );
}

void
Console::putchar( char c ) noexcept
{
  return this->out.putchar( c );
}

int
Console::printf( const char *fmt, ... ) noexcept
{
  va_list args;
  va_start( args, fmt );
  int n = this->out.vprintf( fmt, args );
  va_end( args );
  return n;
}

void
Console::outf( ConsoleOutput *p,  const char *fmt, ... ) noexcept
{
  bool is_json = ( p != NULL && p->is_json );
  if ( is_json )
    this->out.putchar( '\"' );
  va_list args;
  va_start( args, fmt );
  this->out.vprintf( fmt, args );
  va_end( args );
  if ( is_json )
    this->out.putchar( '\"' );
  this->out.putchar( '\n' );
}

void
Console::print_msg( MDMsg &msg ) noexcept
{
  MDFieldIter * f;

  if ( msg.get_field_iter( f ) == 0 ) {
    if ( f->first() == 0 ) {
      do {
        f->print( this, 1, this->fname_fmt, this->type_fmt );
      } while ( f->next() == 0 );
    }
  }
  else {
    msg.print( this );
  }
}

void
Console::print_json( MDMsg &msg ) noexcept
{
  MDFieldIter * iter;
  MDReference mref, fref;
  MDName name;
  char * fname, * str;
  const char * comma = "", * q;
  size_t fnamelen, len;
  int status;

  if ( msg.get_field_iter( iter ) == 0 ) {
    this->printf( "{" );
    if ( iter->first() == 0 ) {
      do {
        if ( iter->get_name( name ) == 0 &&
             iter->get_reference( mref ) == 0 ) {
          fref.set( (void *) name.fname, name.fnamelen, MD_STRING );
          if ( msg.get_escaped_string( fref, "\"", fname, fnamelen ) == 0 ) {
            q = "";
            if ( mref.ftype == MD_STRING || mref.ftype == MD_OPAQUE ||
                 mref.ftype == MD_PARTIAL ) {
              if ( mref.fsize == 0 ) {
                static char null_char;
                q      = "\"";
                str    = &null_char;
                len    = 0;
                status = 0;
              }
              else {
                status = msg.get_escaped_string( mref, "\"", str, len );
              }
            }
            else {
              status = msg.get_string( mref, str, len );
              if ( mref.ftype != MD_UINT && mref.ftype != MD_INT &&
                   mref.ftype != MD_BOOLEAN && mref.ftype != MD_DECIMAL &&
                   mref.ftype != MD_REAL )
                q = "\"";
            }
            if ( status == 0 ) {
              this->printf( "%s%.*s: %s%.*s%s", comma, (int) fnamelen, fname,
                                                    q, (int) len, str, q );
              comma = ", ";
            }
          }
        }
      } while ( iter->next() == 0 );
    }
    this->printf( "}\n" );
  }
  else {
    msg.print( this );
  }
}

bool
ConsoleOutArray::add( ConsoleOutput *p ) noexcept
{
  for ( size_t i = 0; i < this->count; i++ )
    if ( this->ptr[ i ] == p )
      return false;
  this->operator[]( this->count ) = p;
  if ( p != NULL )
    p->rpc = this->rpc;
  return true;
}

bool
ConsoleOutArray::replace( ConsoleOutput *p,  ConsoleOutput *p2 ) noexcept
{
  for ( size_t i = 0; i < this->count; i++ ) {
    if ( this->ptr[ i ] == p ) {
      this->ptr[ i ] = p2;
      if ( p2 != NULL )
        p2->rpc = this->rpc;
      if ( p != NULL ) {
        p->rpc = NULL;
        p->on_remove();
      }
      return true;
    }
  }
  return false;
}

bool
ConsoleOutArray::remove( ConsoleOutput *p ) noexcept
{
  for ( size_t i = 0; i < this->count; i++ ) {
    if ( this->ptr[ i ] == p ) {
      for ( size_t j = i + 1; j < this->count; )
        this->ptr[ i++ ] = this->ptr[ j++ ];
      this->count = i;
      if ( p != NULL ) {
        p->rpc = NULL;
        p->on_remove();
      }
      return true;
    }
  }
  return false;
}

bool
ConsoleOutArray::pop( void ) noexcept
{
  if ( this->count > 0 ) {
    ConsoleOutput *p = this->ptr[ --this->count ];
    if ( p != NULL ) {
      p->rpc = NULL;
      p->on_remove();
    }
    return true;
  }
  return false;
}

void
ConsoleRPC::on_data( const SubMsgData &val ) noexcept
{
  this->console.on_data( val );
}

void
ConsolePing::on_data( const SubMsgData &val ) noexcept
{
  if ( this->is_complete || val.token != this->token || val.src_bridge == NULL )
    return;
  if ( val.ref_seqno != 0 ) {
    this->console.print_data( NULL, val );
    return;
  }
  uint32_t i = this->total_recv++;
  PingReply &reply = this->reply[ i ];
  if ( this->total_recv >= this->count )
    this->is_complete = true;

  reply.uid       = val.src_bridge->uid;
  reply.tid       = val.pub.rte.tport_id;
  reply.rem_tid   = val.tport_id;
  reply.sent_time = val.stamp;
  reply.recv_time = current_realtime_ns();

  if ( this->is_complete )
    this->console.on_ping( *this );
}

void
ConsoleSubs::on_data( const SubMsgData &val ) noexcept
{
  if ( this->is_complete || val.token != this->token || val.src_bridge == NULL )
    return;
  const MsgHdrDecoder & dec = val.pub.dec;
  const char * str = NULL;
  size_t       len = 0;
  bool         is_pattern = false;
  uint32_t     fmt = 0;

  if ( dec.test( FID_SUBJECT ) ) {
    len = dec.mref[ FID_SUBJECT ].fsize;
    str = (const char *) dec.mref[ FID_SUBJECT ].fptr;
  }
  else if ( dec.test( FID_PATTERN ) ) {
    len = dec.mref[ FID_PATTERN ].fsize;
    str = (const char *) dec.mref[ FID_PATTERN ].fptr;
    is_pattern = true;
    dec.get_ival<uint32_t>( FID_FMT, fmt );
  }
  if ( dec.test( FID_END ) ) {
    uint64_t end = 0;
    cvt_number<uint64_t>( dec.mref[ FID_END ], end );
    if ( end >= val.src_bridge->sub_seqno ) {
      if ( ++this->total_recv >= this->count )
        this->is_complete = true;
    }
  }
  if ( len > 0 ) {
    size_t      i     = this->reply.count;
    SubsReply & reply = this->reply[ i ];

    reply.uid        = val.src_bridge->uid;
    reply.sub_off    = this->append_string( str, len );
    reply.sub_len    = (uint16_t) len;
    reply.fmt        = fmt;
    reply.is_pattern = is_pattern;

    if ( dec.test( FID_QUEUE ) ) {
      len = dec.mref[ FID_QUEUE ].fsize;
      str = (const char *) dec.mref[ FID_QUEUE ].fptr;
      this->append_string( str, len );
      reply.queue_len = (uint16_t) len;
    }
    else {
      reply.queue_len = 0;
    }
  }
  if ( this->is_complete )
    this->console.on_subs( *this );
}

void
ConsoleRemote::append_data( uint32_t uid,  const char *str,
                            size_t len ) noexcept
{
  size_t        i     = this->reply.count,
                off   = this->strings.count;
  RemoteReply & reply = this->reply[ i ];
  char        * data  = this->strings.make( off + len + 1 );
  data = &data[ off ];
  ::memcpy( data, str, len );
  data[ len ] = '\0';
  this->strings.count += len + 1;

  reply.uid      = uid;
  reply.data_off = off;
  reply.data_len = len;
}

void
ConsoleRemote::on_data( const SubMsgData &val ) noexcept
{
  if ( this->is_complete || val.token != this->token || val.src_bridge == NULL )
    return;
  const char * str = (const char *) val.data;
  size_t       len = val.datalen;

  if ( len > 0 )
    this->append_data( val.src_bridge->uid, str, len );

  if ( ++this->total_recv >= this->count )
    this->is_complete = true;
  if ( this->is_complete )
    this->console.on_remote( *this );
}

void
ConsoleSubStart::on_data( const SubMsgData &val ) noexcept
{
  this->total_recv++;
  for ( size_t i = 0; i < this->out.count; i++ ) {
    ConsoleOutput * p = this->out.ptr[ i ];
    if ( p->is_json )
      this->console.print_json_data( p, val, this->sub, this->sublen );
    else
      this->console.print_data( p, val, this->sub, this->sublen );
  }
}

void
ConsolePSubStart::on_data( const SubMsgData &val ) noexcept
{
  this->total_recv++;
  for ( size_t i = 0; i < this->out.count; i++ ) {
    ConsoleOutput * p = this->out.ptr[ i ];
    if ( p->is_json )
      this->console.print_json_data( p, val );
    else
      this->console.print_data( p, val );
  }
}

void
ConsoleSnap::on_data( const SubMsgData &val ) noexcept
{
  this->total_recv++;
  for ( size_t i = 0; i < this->out.count; i++ ) {
    ConsoleOutput * p = this->out.ptr[ i ];
    if ( p->is_json )
      this->console.print_json_data( p, val, this->sub, this->sublen );
    else
      this->console.print_data( p, val, this->sub, this->sublen );
  }
  this->console.snap_stop( this );
}

void
Console::print_json_data( ConsoleOutput *p,  const SubMsgData &val,
                          const char *sub,  size_t sublen ) noexcept
{
  MDMsg      * m      = NULL;
  MDMsgMem     mem;

  if ( sublen == 0 ) {
    sublen = val.pub.subject_len;
    sub    = val.pub.subject;
  }
  if ( val.datalen > 0 ) {
    if ( val.fmt != 0 )
      m = MDMsg::unpack( (void *) val.data, 0, val.datalen, val.fmt,
                         MsgFrameDecoder::msg_dict, mem );
    else
      m = MDMsg::unpack( (void *) val.data, 0, val.datalen, MD_STRING,
                         NULL, mem );
  }
  else {
    void * data    = NULL;
    size_t datalen = 0;
    int fmt = val.pub.dec.msg->caba_to_rvmsg( mem, data, datalen );
    m = MDMsg::unpack( data, 0, datalen, fmt, NULL, mem );
  }
  if ( m != NULL ) {
    if ( this->out.count > 0 )
      this->flush_output( NULL );
    this->printf( "{\"%.*s\": ", (int) sublen, sub );
    this->print_json( *m );
    this->printf( "}\n" );
    this->flush_output( p );
  }
}

void
Console::on_data( const SubMsgData &val ) noexcept
{
  this->print_data( NULL, val );
}

void
Console::print_data( ConsoleOutput *p,  const SubMsgData &val,
                     const char *sub,  size_t sublen ) noexcept
{
  if ( sublen == 0 ) {
    sublen = val.pub.subject_len;
    sub    = val.pub.subject;
  }
  if ( val.stamp != 0 ) {
    uint64_t delta = current_realtime_ns() - val.stamp;
    this->printf( "%.*sdelta %.1f usecs%.*s\n",
                  rz, rc, (double) delta / 1000.0, nz, nc );
  }
  char src_nonce[ NONCE_B64_LEN + 1 ];
  if ( val.src_bridge != NULL )
    val.src_bridge->bridge_id.nonce.to_base64_str( src_nonce );
  else
    this->user_db.bridge_id.nonce.to_base64_str( src_nonce );

  const char *user_val;
  if ( val.src_bridge != NULL )
    user_val = val.src_bridge->peer.user.val;
  else
    user_val = this->user_db.user.user.val;
  if ( val.datalen > 0 ) {
    if ( val.fmt != 0 ) {
      MDMsgMem mem;
      MDMsg * m = MDMsg::unpack( (void *) val.data, 0, val.datalen, val.fmt,
                                 MsgFrameDecoder::msg_dict, mem );
      this->printf( "%.*s%.*s%.*s n=%" PRIu64 ".%" PRIu64 " (%s @ %s via %s)\n",
              bz, bc, (int) sublen, sub, nz, nc,
              seqno_frame( val.seqno ), seqno_base( val.seqno ),
              user_val, src_nonce, val.pub.rte.name );
      if ( m != NULL )
        this->print_msg( *m );
    }
    else {
      this->printf( "%.*s%.*s%.*s n=%" PRIu64 ".%" PRIu64 ""
                    " (%s @ %s via %s) : %.*s%.*s%.*s\n",
              bz, bc, (int) sublen, sub, nz, nc,
              seqno_frame( val.seqno ), seqno_base( val.seqno ),
              user_val, src_nonce, val.pub.rte.name, cz, cc,
              (int) val.datalen, (char *) val.data, nz, nc );
    }
  }
  else {
    this->printf( "%.*s%.*s%.*s n=%" PRIu64 ".%" PRIu64 " (%s @ %s via %s)\n",
              bz, bc, (int) sublen, sub, nz, nc,
              seqno_frame( val.seqno ), seqno_base( val.seqno ),
              user_val, src_nonce, val.pub.rte.name );

    this->print_msg( *val.pub.dec.msg );
  }
  this->flush_output( p );
}

void
Console::show_time( ConsoleOutput *p ) noexcept
{
  static const size_t ncols = 2;
  TabOut out( this->table, this->tmp, ncols );
  char local_buf[ 64 ],
       start_buf[ 64 ],
       uptime_buf[ 64 ],
       cpu_buf[ 64 ],
       adj_buf[ 64 ],
       last_buf[ 64 ],
       run_buf[ 64 ];
  uint64_t ns      = current_realtime_ns();
  uint64_t mono_ns = current_monotonic_time_ns();
  size_t   up_sz, cpu_sz, adj_sz, last_sz, run_sz;
  timespec ts;
  MDStamp stamp;

  timestamp( ns, 3, local_buf, sizeof( local_buf ) );
  timestamp( this->user_db.start_time, 3, start_buf, sizeof( start_buf ) );

  stamp.stamp = ( mono_ns - this->user_db.start_mono_time ) / 1000000;
  stamp.resolution = MD_RES_MILLISECS;
  up_sz = stamp.get_string( uptime_buf, sizeof( uptime_buf ) );

  clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &ts );
  stamp.stamp = ts.tv_sec * 1000000 + ( ts.tv_nsec / 1000 );
  stamp.resolution = MD_RES_MICROSECS;
  cpu_sz = stamp.get_string( cpu_buf, sizeof( cpu_buf ) );

  AdjDistance & peer_dist = this->user_db.peer_dist;
  stamp.stamp = peer_dist.adjacency_run_time / 1000;
  stamp.resolution = MD_RES_MICROSECS;
  adj_sz = stamp.get_string( adj_buf, sizeof( adj_buf ) );

  stamp.stamp = peer_dist.adjacency_this_time / 1000;
  stamp.resolution = MD_RES_MICROSECS;
  last_sz = stamp.get_string( last_buf, sizeof( last_buf ) );

  stamp.stamp = peer_dist.total_run_time / 1000;
  stamp.resolution = MD_RES_MICROSECS;
  run_sz = stamp.get_string( run_buf, sizeof( run_buf ) );

  out.add_row()
     .set( "local" )
     .set( local_buf );
  out.add_row()
     .set( "start" )
     .set( start_buf );
  out.add_row()
     .set( "uptime" )
     .set( uptime_buf, up_sz );
  out.add_row()
     .set( "cpu" )
     .set( cpu_buf, cpu_sz );
  out.add_row()
     .set( "adj" )
     .set( adj_buf, adj_sz );
  out.add_row()
     .set( "adjcnt" )
     .set_int( peer_dist.adjacency_run_count );
  out.add_row()
     .set( "last" )
     .set( last_buf, last_sz );
  out.add_row()
     .set( "run" )
     .set( run_buf, run_sz );
  out.add_row()
     .set( "runcnt" )
     .set_int( peer_dist.total_run_count );

  const char *hdr[ ncols ] =
   { "kind", "stamp" };
  this->print_table( p, hdr, ncols );
}

extern "C" {
const char *ms_get_version( void );
const char *sassrv_get_version( void );
const char *ds_get_version( void );
const char *natsmd_get_version( void );
const char *kv_get_version( void );
const char *md_get_version( void );
}
void
Console::show_version( ConsoleOutput *p ) noexcept
{
  static const size_t ncols = 2;
  TabOut out( this->table, this->tmp, ncols );

  out.add_row()
     .set( "raims" )
     .set( ms_get_version() );
  out.add_row()
     .set( "sassrv" )
     .set( sassrv_get_version() );
  out.add_row()
     .set( "raids" )
     .set( ds_get_version() );
  out.add_row()
     .set( "natsmd" )
     .set( natsmd_get_version() );
  out.add_row()
     .set( "raikv" )
     .set( kv_get_version() );
  out.add_row()
     .set( "raimd" )
     .set( md_get_version() );

  const char *hdr[ ncols ] =
   { "repo", "version" };
  this->print_table( p, hdr, ncols );
}

