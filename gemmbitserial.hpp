#pragma once
#include <stdint.h>
#include <string.h>
#include <cassert>
#include <iostream>
#include <math.h>

namespace gemmbitserial {

// Utility function to increment-and-align "in" to "af"
inline uint64_t alignTo(uint64_t in, uint64_t af) {
  if(in % af != 0) {
    return in + (af - (in % af));
  } else {
    return in;
  }
}

class BitSerialMatrix {
public:
  // static member functions for working with BitSerialMatrix

  /* Allocate buffer space for a BitSerialMatrix */
  static BitSerialMatrix alloc(uint64_t nbits, uint64_t nrows, uint64_t ncols, bool issigned, uint64_t rowalign = 1, uint64_t colalign = 64) {
    BitSerialMatrix bsm;
    bsm.nbits = nbits;
    bsm.nrows = nrows;
    bsm.ncols = ncols;
    bsm.nrows_a = alignTo(nrows, rowalign);
    bsm.ncols_a = alignTo(ncols, colalign);
    bsm.issigned = issigned;
    uint64_t wordsPerBitplane = bsm.wordsPerBitplane();
    bsm.data = new uint64_t[nbits * wordsPerBitplane];
    return bsm;
  }

  /* Deallocate buffers for a BitSerialMatrix */
  static void dealloc(BitSerialMatrix bsm) {
    delete [] bsm.data;
  }

public:
  // actual member variables and functions of BitSerialMatrix instances
  bool issigned;        // whether highest order bit pos is negative
  uint64_t nbits;       // bits of precision
  uint64_t nrows;       // number of real (actual) rows
  uint64_t ncols;       // number of real (actual) columns
  uint64_t nrows_a;     // number of allocated rows
  uint64_t ncols_a;     // number of allocated columns
  uint64_t * data;      // data buffer, layout [nbits][nrows_a][ncols_a/64]

  // print key statistics about BitSerialMatrix to stdout
  void printSummary() {
    std::cout << "BitSerialMatrix" << std::endl;
    std::cout << "Bits of precision: " << nbits << " signed: " << issigned << std::endl;
    std::cout << "Actual size: " << nrows << " x " << ncols << std::endl;
    std::cout << "Allocated size: " << nrows_a << " x " << ncols_a << std::endl;
  }

  // return whether the matrix contains bipolar binary {-1, +1} values
  inline bool isBipolar() const {
    return nbits == 1 && issigned;
  }

  // number of storage words needed for each row
  inline uint64_t wordsPerRow() const {
    const uint64_t bitsPerWord = sizeof(uint64_t) * 8;
    return ncols_a / bitsPerWord;
  }

  // number of storage words needed for each bitplane (bit matrix)
  inline uint64_t wordsPerBitplane() const {
    return nrows_a * wordsPerRow();
  }

  // get given bit. true if set, false if unset.
  inline bool get(uint64_t bit, uint64_t row, uint64_t col) {
    return ((word(bit, row, col) >> bitpos(col)) & 1L) == 1;
  }

  // set all bits to zero
  inline void clearAll() {
    memset(data, 0, nbits * wordsPerBitplane() * sizeof(uint64_t));
  }

  // set given bit to one
  inline void set(uint64_t bit, uint64_t row, uint64_t col) {
    word(bit, row, col) |= (1L << bitpos(col));
  }

  // set given bit to zero
  inline void unset(uint64_t bit, uint64_t row, uint64_t col) {
    word(bit, row, col) &= ~(1L << bitpos(col));
  }

  // access the container word for a given bit
  inline uint64_t & word(uint64_t bit, uint64_t row, uint64_t col) {
    // right shift by log2(bits per word) to get word index
    uint64_t colw = col >> 6;
    return data[bit * wordsPerBitplane() + row * wordsPerRow() + colw];
  }

  // get a pointer to a particular row
  inline uint64_t * rowptr(uint64_t bit, uint64_t row) {
    return &data[bit * wordsPerBitplane() + row * wordsPerRow()];
  }

  // get a pointer to a particular bit plane
  inline uint64_t * bitplaneptr(uint64_t bit) {
    return &data[bit * wordsPerBitplane()];
  }

  uint64_t bitpos(uint64_t col) {
    // return modulo 64 of col by using a bitmask
    return col & ((1 << 6) - 1);
  }

  /* Imports a regular matrix into this BitSerialMatrix.
  */
  template <typename T>
  void importRegular(T * matrix, bool readColMajor=false) {
    this->clearAll();
    for(uint64_t r = 0; r < this->nrows; r++) {
      for(uint64_t c = 0; c < this->ncols; c++) {
        T currentElem = readColMajor ? matrix[c * this->nrows + r] : matrix[r * this->ncols + c];
        if(this->isBipolar()) {
          // use bipolar binary encoding: -1 and +1 only (sign)
          if(currentElem > 0) {
            this->set(0, r, c);
          }
        } else {
          // use two's complement
          uint8_t currentElem_uint8 = 0;
          if(this->issigned && currentElem < 0) {
            // convert to two's complement for this bitwidth
            currentElem_uint8 += (uint8_t)(1 << (this->nbits - 1));
            currentElem_uint8 += (uint8_t)(currentElem + (1 << (this->nbits - 1)));
          } else {
            currentElem_uint8 = (uint8_t) currentElem;
          }
          for(uint64_t b = 0; b < this->nbits; b++) {
            if(currentElem_uint8 & (1 << b)) {
              this->set(b, r, c);
            }
          }
        }
      }
    }
  }

  /* Imports a regular matrix after applying threshold quantization into this BitSerialMatrix.
  *  The threshold array is assumped to have the shape thresholds[nThres][nrows],
  *  and is assumed to be sorted s.t. the largest thresholds have the largest index.
  */
  template <typename T>
  void importRegularAndQuantize(T * matrix, T * thresholds, int nThres, bool readColMajor=false) {
    assert(!this->issigned); // threshold qnt. only makes sense for unsigned
    this->clearAll();
    for(uint64_t r = 0; r < this->nrows; r++) {
      for(uint64_t c = 0; c < this->ncols; c++) {
        T currentElem = readColMajor ? matrix[c * this->nrows + r] : matrix[r * this->ncols + c];
        // quantize this element by finding the index of the largest crossed
        // threshold
        for(int t = 0; t < nThres; t++) {
          if(currentElem <= thresholds[t * this->nrows + r]) {
            currentElem = t;
            break;
          } else if(t == nThres - 1) {
            // all thresholds crossed, set to largest quantization level
            currentElem = t + 1;
          }
        }
        // now convert to bit serial form
        uint8_t currentElem_uint8 = (uint8_t) currentElem;;
        for(uint64_t b = 0; b < this->nbits; b++) {
          if(currentElem_uint8 & (1 << b)) {
            this->set(b, r, c);
          }
        }
      }
    }
  }

  /* Convert this BitSerialMatrix back to a regular matrix.
  */
  template <typename T>
  void exportRegular(T * matrix) {
    for(uint64_t r = 0; r < this->nrows; r++) {
      for(uint64_t c = 0; c < this->ncols; c++) {
        if(this->isBipolar()) {
          matrix[r * this->ncols + c] = (T) this->get(0, r, c) ? +1 : -1;
        } else {
          T currentElem = 0;
          for(uint64_t b = 0; b < this->nbits; b++) {
            if(this->get(b, r, c)) {
              if((b == this->nbits-1) && this->issigned) {
                currentElem -= 1 << b;
              } else {
                currentElem += 1 << b;
              }
            }
          }
          matrix[r * this->ncols + c] = (T) currentElem;
        }
      }
    }
  }
};

/* Utility function to find block size under the following assumptions:
   - size of lhs block + rhs block + result block <= cacheBits
   - no blocking along depth (i.e. only entire rows of dBits bits)
   - lhsMult and rhsMult determine the ratio for lhs and rhs rows in cache
   - returned lhsRows and rhsRows are divisible by lhsMult and rhsMult, respectively
   - each result elem takes bitsPerRes bits
*/
static void computeBlockSize(float lhsMult, float rhsMult, float cacheBits, float dBits, uint64_t & lhsBlock, uint64_t & rhsBlock) {
  float a = sizeof(int32_t) * lhsMult * rhsMult;
  float b = dBits*(lhsMult + rhsMult);
  float c = -cacheBits;
  float discr = sqrt(b*b - 4 * a * c);
  assert(discr > 0);
  int64_t x0 = floor((-b + discr) / (2*a));
  int64_t x1 = floor((-b - discr) / (2*a));
  int64_t x = x0 > x1 ? x0 : x1;
  assert(x > 0);
  lhsBlock = lhsMult * x;
  rhsBlock = rhsMult * x;
};

// rather naive, iterative search for a better block size
// how could this be improved?
static uint64_t finetuneBlockSize(uint64_t rows, uint64_t bs_max, uint64_t bs_div) {
  uint64_t best_cand = bs_max;
  uint64_t min_penalty = alignTo(rows, best_cand) - rows;
  for(uint64_t ccand = bs_max; ccand > bs_div; ccand = ccand - bs_div ) {
    if(ccand % bs_div == 0) {
      uint64_t penalty = alignTo(rows, ccand) - rows;
      if(penalty < min_penalty) {
        best_cand = ccand;
        min_penalty = penalty;
      }
    }
  }
  return best_cand;
}

class GEMMContext {
public:
  BitSerialMatrix lhs, rhs;
  uint64_t lhsBlock, rhsBlock;
  int32_t * res;

  void printSummary() {
    std::cout << "GEMMContext" << std::endl;
    std::cout << "LHS: ";
    lhs.printSummary();
    std::cout << "Block size: " << lhsBlock << std::endl;
    std::cout << "RHS: ";
    rhs.printSummary();
    std::cout << "Block size: " << rhsBlock << std::endl;
    float actual_ops = 2*lhs.nrows*lhs.ncols*rhs.nrows;
    float alloc_ops = 2*lhs.nrows_a*lhs.ncols_a*rhs.nrows_a;
    std::cout << "Actual ops: " << actual_ops << std::endl;
    std::cout << "Allocated ops: " << alloc_ops << std::endl;
    std::cout << "Actual op percentage: " << 100*actual_ops/alloc_ops << std::endl;
  }

  inline bool isBipolarTimesRegular() const {
    return (lhs.isBipolar() && !rhs.isBipolar()) || (!lhs.isBipolar() && rhs.isBipolar());
  }

  inline bool isBipolarTimesBipolar() const {
    return (lhs.isBipolar() && rhs.isBipolar());
  }
};

// Base functionality for allocating a GEMM context. Do not use directly,
// use the platform-provided allocGEMMContext instead.
static GEMMContext allocGEMMContext_base(
  const uint64_t lhsRows, const uint64_t depth, const uint64_t rhsRows,
  const uint64_t lhsBits, const uint64_t rhsBits, const bool lhsSigned,
  const bool rhsSigned, const uint64_t regblock_lhs, const uint64_t regblock_d,
  const uint64_t regblock_rhs, const uint64_t cacheBits
) {
  GEMMContext ret;
  uint64_t depth_al = alignTo(depth, regblock_d*64);
  // use cache blocking; compute sizes
  computeBlockSize(
    regblock_lhs, regblock_rhs, cacheBits, depth_al,
    ret.lhsBlock, ret.rhsBlock
  );
  if(ret.lhsBlock > lhsRows || ret.rhsBlock > rhsRows) {
    // use register blocking only
    ret.lhsBlock = alignTo(lhsRows, regblock_lhs);
    ret.rhsBlock = alignTo(rhsRows, regblock_rhs);
  } else {
    // see if there is too much wasted compute for current block sizes
    if((alignTo(lhsRows, ret.lhsBlock) - lhsRows) > 0.1*lhsRows) {
      ret.lhsBlock = finetuneBlockSize(lhsRows, ret.lhsBlock, regblock_lhs);
    }
    if((alignTo(rhsRows, ret.rhsBlock) - rhsRows) > 0.1*rhsRows) {
      ret.rhsBlock = finetuneBlockSize(rhsRows, ret.rhsBlock, regblock_rhs);
    }
  }
  // allocate aligned bit serial matrices
  ret.lhs = BitSerialMatrix::alloc(
    lhsBits, lhsRows, depth, lhsSigned, ret.lhsBlock, regblock_d*64
  );
  ret.rhs = BitSerialMatrix::alloc(
    rhsBits, rhsRows, depth, rhsSigned, ret.rhsBlock, regblock_d*64
  );
  // allocate result matrix. note that it is not aligned -- the
  // elements corresponding to alignment parts won't materialize.
  ret.res = new int32_t[lhsRows * rhsRows];
  return ret;
};

static void deallocGEMMContext(GEMMContext ctx) {
  delete [] ctx.res;
  BitSerialMatrix::dealloc(ctx.lhs);
  BitSerialMatrix::dealloc(ctx.rhs);
};



// generic implementations using regular & and __builtin_popcountll
#include "arch-generic.hpp"

// select the implementations to be used based on architecture
#if defined(__ARM_NEON) || defined(__aarch64__)
#warning "Compiling with ARM NEON"
#include "arch-neon.hpp"
// ARM NEON-specific implementations
#define gemmBitSerial     gemmBitSerial_neon
#define allocGEMMContext  allocGEMMContext_neon
#define sumRows           sumRows_neon
#else
#warning "Compiling using generic popcount"
#define gemmBitSerial     gemmBitSerial_generic
#define allocGEMMContext  allocGEMMContext_generic
#define sumRows           sumRows_generic
#endif

}
