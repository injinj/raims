#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wmmintrin.h>
#include <raims/aes.h>
#include <raikv/util.h>

using namespace rai;
using namespace ms;

static __m128i
key_expansion_128( __m128i key, __m128i gen )
{
  __m128i tmp;

  gen = _mm_shuffle_epi32( gen, _MM_SHUFFLE( 3, 3, 3, 3 ) );
  tmp = _mm_slli_si128( key, 4 );
  key = _mm_xor_si128( key, tmp );
  tmp = _mm_slli_si128( tmp, 4 );
  key = _mm_xor_si128( key, tmp );
  tmp = _mm_slli_si128( tmp, 4 );
  key = _mm_xor_si128( key, tmp );
  key = _mm_xor_si128( key, gen );

  return key;
}

/* key_sched is accessed with unaligned load/store so the buffer carries no
 * alignment contract (see aes.h): AES-NI operands need no alignment, and
 * vmovdqu == vmovdqa on aligned data under -mavx.  This lets the enclosing
 * socket object be allocated by plain malloc without a codegen trap. */
void
AES128::expand_key( const void *key ) noexcept
{
  __m128i * sched = (__m128i *) this->key_sched,
            genass, k;
  int i, j;

  k = _mm_loadu_si128( (const __m128i *) key );
  _mm_storeu_si128( &sched[ 0 ], k );

  genass = _mm_aeskeygenassist_si128( k, 0x1 );
  k = key_expansion_128( k, genass );
  _mm_storeu_si128( &sched[ 1 ], k );
  genass = _mm_aeskeygenassist_si128( k, 0x2 );
  k = key_expansion_128( k, genass );
  _mm_storeu_si128( &sched[ 2 ], k );
  genass = _mm_aeskeygenassist_si128( k, 0x4 );
  k = key_expansion_128( k, genass );
  _mm_storeu_si128( &sched[ 3 ], k );
  genass = _mm_aeskeygenassist_si128( k, 0x8 );
  k = key_expansion_128( k, genass );
  _mm_storeu_si128( &sched[ 4 ], k );
  genass = _mm_aeskeygenassist_si128( k, 0x10 );
  k = key_expansion_128( k, genass );
  _mm_storeu_si128( &sched[ 5 ], k );
  genass = _mm_aeskeygenassist_si128( k, 0x20 );
  k = key_expansion_128( k, genass );
  _mm_storeu_si128( &sched[ 6 ], k );
  genass = _mm_aeskeygenassist_si128( k, 0x40 );
  k = key_expansion_128( k, genass );
  _mm_storeu_si128( &sched[ 7 ], k );
  genass = _mm_aeskeygenassist_si128( k, 0x80 );
  k = key_expansion_128( k, genass );
  _mm_storeu_si128( &sched[ 8 ], k );
  genass = _mm_aeskeygenassist_si128( k, 0x1b );
  k = key_expansion_128( k, genass );
  _mm_storeu_si128( &sched[ 9 ], k );
  genass = _mm_aeskeygenassist_si128( k, 0x36 );
  k = key_expansion_128( k, genass );
  _mm_storeu_si128( &sched[ 10 ], k );
  j = 10;
  for ( i = 11; i < 20; i++ )
    _mm_storeu_si128( &sched[ i ],
                      _mm_aesimc_si128( _mm_loadu_si128( &sched[ --j ] ) ) );
}

void
AES128::encrypt( const void *plain,  void *cipher ) noexcept
{
  __m128i * sched = (__m128i *) this->key_sched,
            mix   = _mm_loadu_si128( (__m128i *) plain );

  mix = _mm_xor_si128( mix, _mm_loadu_si128( &sched[ 0 ] ) );
  for ( size_t i = 1; i < 10; i++ )
    mix = _mm_aesenc_si128( mix, _mm_loadu_si128( &sched[ i ] ) );
  mix = _mm_aesenclast_si128( mix, _mm_loadu_si128( &sched[ 10 ] ) );

  _mm_storeu_si128( (__m128i *) cipher, mix );
}

void
AES128::decrypt( const void *cipher,  void *plain ) noexcept
{
  __m128i * sched = (__m128i *) this->key_sched,
            mix   = _mm_loadu_si128( (__m128i *) cipher );

  mix = _mm_xor_si128( mix, _mm_loadu_si128( &sched[ 10 ] ) );
  for ( size_t i = 11; i < 20; i++ )
    mix = _mm_aesdec_si128( mix, _mm_loadu_si128( &sched[ i ] ) );
  mix = _mm_aesdeclast_si128( mix, _mm_loadu_si128( &sched[ 0 ] ) );

  _mm_storeu_si128( (__m128i *) plain, mix );
}

static inline uint64_t bswap( uint64_t x ) { return kv_bswap64( x ); }

void
AES128::encrypt_ctr( uint64_t ctr[ 2 ], void *out, size_t out_blocks ) noexcept
{
  uint8_t * ptr = (uint8_t *) out;
  uint64_t  i   = bswap( ctr[ 1 ] ),
            j   = ctr[ 0 ];

  if ( ~i >= out_blocks - 1 && out_blocks >= 8 ) {
    do {
      __m128i * sched = (__m128i *) this->key_sched, sk,
                mix0, mix1, mix2, mix3, mix4, mix5, mix6, mix7;

      mix0 = _mm_set_epi64x( bswap( i     ), j );
      mix1 = _mm_set_epi64x( bswap( i + 1 ), j );
      mix2 = _mm_set_epi64x( bswap( i + 2 ), j );
      mix3 = _mm_set_epi64x( bswap( i + 3 ), j );
      mix4 = _mm_set_epi64x( bswap( i + 4 ), j );
      mix5 = _mm_set_epi64x( bswap( i + 5 ), j );
      mix6 = _mm_set_epi64x( bswap( i + 6 ), j );
      mix7 = _mm_set_epi64x( bswap( i + 7 ), j );

      sk = _mm_loadu_si128( &sched[ 0 ] );
      mix0 = _mm_xor_si128( mix0, sk );
      mix1 = _mm_xor_si128( mix1, sk );
      mix2 = _mm_xor_si128( mix2, sk );
      mix3 = _mm_xor_si128( mix3, sk );
      mix4 = _mm_xor_si128( mix4, sk );
      mix5 = _mm_xor_si128( mix5, sk );
      mix6 = _mm_xor_si128( mix6, sk );
      mix7 = _mm_xor_si128( mix7, sk );

      for ( size_t k = 1; k < 10; k++ ) {
        sk = _mm_loadu_si128( &sched[ k ] );
        mix0 = _mm_aesenc_si128( mix0, sk );
        mix1 = _mm_aesenc_si128( mix1, sk );
        mix2 = _mm_aesenc_si128( mix2, sk );
        mix3 = _mm_aesenc_si128( mix3, sk );
        mix4 = _mm_aesenc_si128( mix4, sk );
        mix5 = _mm_aesenc_si128( mix5, sk );
        mix6 = _mm_aesenc_si128( mix6, sk );
        mix7 = _mm_aesenc_si128( mix7, sk );
      }
      sk = _mm_loadu_si128( &sched[ 10 ] );
      mix0 = _mm_aesenclast_si128( mix0, sk );
      mix1 = _mm_aesenclast_si128( mix1, sk );
      mix2 = _mm_aesenclast_si128( mix2, sk );
      mix3 = _mm_aesenclast_si128( mix3, sk );
      mix4 = _mm_aesenclast_si128( mix4, sk );
      mix5 = _mm_aesenclast_si128( mix5, sk );
      mix6 = _mm_aesenclast_si128( mix6, sk );
      mix7 = _mm_aesenclast_si128( mix7, sk );

      _mm_storeu_si128( (__m128i *) ptr,          mix0 );
      _mm_storeu_si128( (__m128i *) &ptr[ 16 ],   mix1 );
      _mm_storeu_si128( (__m128i *) &ptr[ 16*2 ], mix2 );
      _mm_storeu_si128( (__m128i *) &ptr[ 16*3 ], mix3 );
      _mm_storeu_si128( (__m128i *) &ptr[ 16*4 ], mix4 );
      _mm_storeu_si128( (__m128i *) &ptr[ 16*5 ], mix5 );
      _mm_storeu_si128( (__m128i *) &ptr[ 16*6 ], mix6 );
      _mm_storeu_si128( (__m128i *) &ptr[ 16*7 ], mix7 );

      ptr = &ptr[ 16*8 ];
      i  += 8;
      out_blocks -= 8;
    } while ( out_blocks >= 8 );
    if ( i == 0 ) {
      j = bswap( bswap( j ) + 1 );
      ctr[ 0 ] = j;
    }
  }
  while ( out_blocks > 0 ) {
    uint64_t in[ 2 ] = { j, bswap( i ) };
    this->encrypt( in, ptr );
    ptr = &ptr[ 16 ];
    if ( ++i == 0 ) {
      j = bswap( bswap( j ) + 1 );
      ctr[ 0 ] = j;
    }
    out_blocks -= 1;
  }
  ctr[ 1 ] = bswap( i );
}

void
AES128::block_xor( const void *in,  void *out,  size_t blocks ) noexcept
{
  const uint8_t * in_ptr = (const uint8_t *) in;
  uint8_t * out_ptr = (uint8_t *) out;

  if ( blocks >= 4 ) {
    do {
      __m128i b0, b1, b2, b3, x0, x1, x2, x3;

      b0 = _mm_loadu_si128( (__m128i *) in_ptr           );
      b1 = _mm_loadu_si128( (__m128i *) &in_ptr[ 16 ]    );
      b2 = _mm_loadu_si128( (__m128i *) &in_ptr[ 16*2 ]  );
      b3 = _mm_loadu_si128( (__m128i *) &in_ptr[ 16*3 ]  );

      x0 = _mm_loadu_si128( (__m128i *) out_ptr          );
      x1 = _mm_loadu_si128( (__m128i *) &out_ptr[ 16 ]   );
      x2 = _mm_loadu_si128( (__m128i *) &out_ptr[ 16*2 ] );
      x3 = _mm_loadu_si128( (__m128i *) &out_ptr[ 16*3 ] );

      b0 = _mm_xor_si128( x0, b0 );
      b1 = _mm_xor_si128( x1, b1 );
      b2 = _mm_xor_si128( x2, b2 );
      b3 = _mm_xor_si128( x3, b3 );

      _mm_storeu_si128( (__m128i *) out_ptr,          b0 );
      _mm_storeu_si128( (__m128i *) &out_ptr[ 16 ],   b1 );
      _mm_storeu_si128( (__m128i *) &out_ptr[ 16*2 ], b2 );
      _mm_storeu_si128( (__m128i *) &out_ptr[ 16*3 ], b3 );

      in_ptr  = &in_ptr[ 16*4 ];
      out_ptr = &out_ptr[ 16*4 ];
      blocks -= 4;
    } while ( blocks >= 4 );
  }
  while ( blocks > 0 ) {
    __m128i b, x;

    b = _mm_loadu_si128( (__m128i *) in_ptr  );
    x = _mm_loadu_si128( (__m128i *) out_ptr );

    b = _mm_xor_si128( x, b );

    _mm_storeu_si128( (__m128i *) out_ptr, b );

    in_ptr  = &in_ptr[ 16 ];
    out_ptr = &out_ptr[ 16 ];
    blocks -= 1;
  }
}

void
AES128::byte_xor( const void *in,  void *out,  size_t bytes ) noexcept
{
  size_t blocks = bytes / 16;
  bytes %= 16;
  if ( blocks > 0 ) {
    AES128::block_xor( in, out, blocks );
    in  = &((const uint8_t *) in)[ 16 * blocks ];
    out = &((uint8_t *) out)[ 16 * blocks ];
  }
  if ( bytes >= 8 ) {
    uint64_t in_w, out_w;
    ::memcpy( &in_w, in, sizeof( in_w ) );
    ::memcpy( &out_w, out, sizeof( out_w ) );
    out_w ^= in_w;
    ::memcpy( out, &out_w, sizeof( out_w ) );
    in  = &((const uint8_t *) in)[ 8 ];
    out = &((uint8_t *) out)[ 8 ];
    bytes -= 8;
  }
  for ( size_t i = 0; i < bytes; i++ ) {
    ((uint8_t *) out)[ i ] ^= ((const uint8_t *) in)[ i ];
  }
}

