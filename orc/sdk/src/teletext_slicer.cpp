/*
 * File:        teletext_slicer.cpp
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     WST (System B) teletext data-line slicer producing T42 packets,
 *              on 625-line and 525-line television systems
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/support/teletext_slicer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace orc {

namespace {

// ETSI EN 300 706 §6.1: 16-bit clock run-in, transmission order 1010…1010
// (first transmitted bit is '1'; even bit indices are ones).
constexpr int kRunInBits = 16;

// ETSI EN 300 706 §6.2: 8-bit framing code, transmission order 11100100
// (conventionally written 0xE4 MSB-first).
constexpr int kFramingBits = 8;
constexpr int kFramingCodeBits[kFramingBits] = {1, 1, 1, 0, 0, 1, 0, 0};

// The payload of a 625-line packet is 336 bits: ETSI EN 300 706 §7.1's 360-bit
// packet less the run-in (16) and framing code (8), transmitted LSB first per
// byte. A 525-line packet is shorter (ITU-R BT.653 Table 1b) and fills the
// leading bits of the same buffers, which are sized from kTeletextPayloadBits;
// the count the detectors actually work to is TeletextSlicer::payload_bits_.

// Data '1' level as a fraction of the black-to-white difference (the '0' level
// is black in both systems).
//   625 lines: ETSI EN 300 706 §5.2 and ITU-R BT.653 Table 1a — 66 % ± 6 %.
//   525 lines: ITU-R BT.653 Table 1b — 70 % ± 6 %.
constexpr double kDataOneLevelFraction625 = 0.66;
constexpr double kDataOneLevelFraction525 = 0.70;

// Minimum recovered data amplitude, as a fraction of the nominal '1' level,
// below which a line is treated as empty. Implementation choice: half the
// nominal amplitude rejects blank and noise-only lines cheaply while accepting
// the ± 6 % tolerance of either system with wide margin.
constexpr double kAmplitudeGateFraction = 0.5;

// Search window for the centre of the first clock run-in bit, in µs from the
// start of the line (which decode-orc places at 0H).
//
// ETSI EN 300 706 §6.3 fixes the timing reference at the mid point of the
// penultimate '1' of the clock run-in. The run-in is 1010…1010 over 16 bits,
// so its ones are bits 1, 3 … 15 and the penultimate one is bit 13 — the same
// bit ITU-R BT.653 Table 1a/1b names. The first bit centre therefore sits 12
// bit periods before the reference:
//
//   625 lines: reference 12,0 µs (§6.3 note, BT.653 Table 1a) → first bit
//     centre at 12,0 − 12/6,9375 = 10,27 µs, leading edge at 10,20 µs — which
//     is the 10 300 ns libzvbi tabulates and the VBI source stage places to.
//   525 lines: reference 11,7 µs ± 0,175 (BT.653 Table 1b) → first bit centre
//     at 11,7 − 12/5,727272 = 9,61 µs, leading edge at 9,52 µs. The 525-line
//     reference captures measured for the VBI source stage put the leading
//     edge at 9,3 to 9,4 µs, which agrees with this to within a bit period —
//     and disagrees with libzvbi's tabulated 10 500 ns by seven of them.
//
// The §6.3 note allows departures from the nominal for network re-timing, so
// each window spans roughly ± 2 µs around it. The lower bound also has to
// clear the colour burst, which ends ≈ 7,8 µs into the line on both systems;
// that is what makes the 525-line window asymmetric about its nominal.
constexpr double kRunInSearchStartUs625 = 8.0;
constexpr double kRunInSearchEndUs625 = 12.0;
constexpr double kRunInSearchStartUs525 = 8.0;
constexpr double kRunInSearchEndUs525 = 11.5;

// Correlation phase-search step in samples. At the ≈ 2.556 samples/bit of the
// 625-line service, or the 2.5 of the 525-line one, a quarter sample bounds
// the bit-centre placement error at ≈ 5 % of a bit period.
constexpr double kPhaseSearchStep = 0.25;

// Minimum run-in bits that must match the alternating pattern after
// thresholding. ETSI EN 300 706 §6.1 note: the two leading data ones may be
// absent or reduced in amplitude, so up to two mismatches are allowed.
constexpr int kMinRunInMatches = 14;

// Framing-code search range around the nominal position, in bit periods.
// The run-in correlation is ambiguous to even-bit shifts (the alternating
// kernel re-aligns every 2 bits), and on real recordings the correlation peak
// lands up to four bits from the true run-in start — the §6.3 note allows the
// insertion point to move for network re-timing, and the kernel also
// correlates against the framing code and the leading payload. A ± 2 window
// therefore misses the framing code outright on a third of otherwise perfect
// lines, so the search spans ± 4 bit positions.
constexpr int kFramingSearchBits = 4;

// --- MLSE detector (TeletextDetector::kMlse) ------------------------------
//
// A channel that rolls off below the 3,47 MHz run-in fundamental (§6.1)
// smears each bit across its neighbours, so no per-sample threshold recovers
// the bits: the level at a bit centre depends on the bits around it.  The
// detector therefore models the channel explicitly.  Every line starts with
// the same 24 known bits (run-in then framing code), which is enough to fit a
// short linear response; the payload bits are then whichever sequence that
// fitted channel would most likely have produced.  Nothing is trained ahead
// of time and nothing is assumed about the recorder: each line carries the
// reference needed to fit its own channel.

// ETSI EN 300 706 §6.1 + §6.2: the known preamble the channel is fitted to.
constexpr int kPreambleBits = kRunInBits + kFramingBits;

// Channel response length in bit periods, and the index of its centre tap.
// Five taps span ±2 bit periods around the bit being detected: measured on
// VHS SP captures the response is essentially spent by then, and each extra
// tap doubles the Viterbi state count.
constexpr int kChannelTaps = 5;
constexpr int kChannelCentre = 2;

// Viterbi state = the kChannelTaps - 1 preceding bits.
constexpr int kMlseStates = 1 << (kChannelTaps - 1);

// Bit periods past the end of the packet the detector reads when the line is
// long enough to hold them. ETSI EN 300 706 §7.1: nothing follows the last
// packet bit, so the line is back at black level there and those samples are
// the known-bit evidence that pins the last payload bits. Filling the whole
// state register takes kChannelTaps - 1 of them.
constexpr int kTrailingBits = kChannelTaps - 1;

// Upper bound on TeletextSlicerOptions::mlse_samples_per_bit, which sets how
// many evenly spaced positions within each bit period the detector scores.
// At the PAL 4FSC rate a bit spans ≈ 2.556 samples and at the NTSC one exactly
// 2.5, so beyond three grid points per bit two of them fall between the same
// pair of captured samples and carry no independent information.
constexpr int kMaxMlseSamplesPerBit = 3;

// Bit-phase search step for the preamble fit, in samples. Finer than the
// threshold detector's quarter sample because the fit residual — not a
// correlation peak — is what discriminates, and it sharpens with phase.
constexpr double kMlsePhaseStep = 0.2;

// Minimum fitted channel gain (sum of taps = the modelled black-to-'1' step)
// as a fraction of the nominal '1' amplitude. A fit that explains the
// preamble with almost no gain has locked onto flat noise.
constexpr double kMlseMinGainFraction = 0.25;

// Maximum RMS preamble-fit residual as a fraction of the fitted gain. A real
// preamble under a five-tap linear model leaves a small residual; picture
// content that happens to sit under the search window does not fit at all.
// Measured over VHS SP and LP captures: locks that yield packets passing
// their parity checks sit below 0.15, and beyond 0.20 the recovered bytes are
// noise. Set at the upper end of that range because the row squasher outvotes
// the occasional bad packet, whereas a packet never recovered is simply lost.
constexpr double kMlseMaxResidualFraction = 0.20;

// Maximum RMS payload reconstruction error, as a fraction of the fitted gain,
// for the recovered bits to be believed. Measured across VHS SP, VHS LP and
// LaserDisc captures, real packets sit below 0.4.
constexpr double kMlseMaxPayloadResidualFraction = 0.5;

// Minimum fraction of the data bytes (40 of them on 625 lines, 32 on 525) that
// must carry odd parity for an MLSE-recovered packet in a parity-coded row.
//
// ETSI EN 300 706 §9.3.1 gives the display bytes odd parity — ITU-R BT.653
// Table 1b says the same of the 525-line service — and every Hamming 8/4
// codeword (§8.2) is odd-parity as well, so an undamaged packet
// in rows 0-25 has all its data bytes odd — confirmed at exactly 1,000 across
// 130 000 packets of an independently decoded reference stream. Random bytes
// sit at 0,5, so this rejects a false lock with high confidence while leaving
// room for a genuinely damaged packet the row squasher can still repair.
// The threshold detector needs no equivalent: its exact framing-code match
// (§6.2) already rules out noise.
constexpr double kMlseMinDataParityFraction = 0.6;

// ETSI EN 300 706 §7.1.2: the packet opens with the two Hamming 8/4 coded
// magazine/row address bytes.
constexpr size_t kMragBytes = 2;

// ETSI EN 300 706 §9.3.1: a page header (X/0) spends its first ten bytes on
// addressing and control — the MRAG, the two page-number bytes, the four
// sub-code bytes and the two control-bit bytes — all Hamming 8/4 coded. The
// bytes that follow are the odd-parity header text (32 of them on 625 lines,
// 24 on 525; the addressing is the same either way, ITU-R BT.653 Table 1b
// §3.3).
constexpr size_t kHeaderControlBytes = 10;

// Rows above this carry Hamming 24/18 triplets (§9.6 packets X/26 to X/29) or
// independent data services (§9.8 packet X/31), neither of which is byte-wise
// odd parity, so the parity gate does not apply to them.
constexpr int kMlseLastParityCodedRow = 25;

// Observation-string encoding: two hex characters per packet byte, optionally
// followed by one per byte of quantised confidence.
constexpr char kHexDigits[] = "0123456789abcdef";

// Packet lengths a WST service transmits, and therefore the only ones an
// observation string may encode. The four resulting string lengths — 68, 84,
// 102 and 126 characters — are distinct, which is what lets a string be
// decoded without being told which system produced it.
constexpr size_t kPacketByteLengths[] = {kTeletextPacketBytes,
                                         kTeletext525PacketBytes};

// Value of one hex character (either case), or -1 when it is not one.
inline int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Mark |result| rejected for |reason| and hand it back to the caller. Every
// early return of both detectors goes through here so no path can leave the
// reason unset.
inline TeletextLineResult reject(TeletextLineResult result,
                                 TeletextRejectReason reason) {
  result.valid = false;
  result.reject_reason = reason;
  return result;
}

// ETSI EN 300 706 §9.3.1: a transmitted display byte carries odd parity over
// all eight bits.
inline bool odd_parity(uint8_t byte) {
  int ones = 0;
  for (int bit = 0; bit < 8; ++bit) {
    ones += (byte >> bit) & 1;
  }
  return (ones & 1) == 1;
}

// The known preamble bit at index |k| (0 … kPreambleBits - 1).
inline int preamble_bit(int k) {
  return k < kRunInBits ? ((k % 2 == 0) ? 1 : 0)
                        : kFramingCodeBits[k - kRunInBits];
}

// Linear interpolation between adjacent samples at fractional position |t|.
// Caller guarantees t >= 0 and t + 1 < sample_count.
inline double sample_at(const int16_t* line, double t) {
  const auto i = static_cast<size_t>(t);
  const double frac = t - static_cast<double>(i);
  return static_cast<double>(line[i]) +
         (static_cast<double>(line[i + 1]) - static_cast<double>(line[i])) *
             frac;
}

// Offset of grid phase |p| of |phases| from the bit centre, in bit periods.
// The phases straddle the centre symmetrically, so phases == 1 is the bit
// centre itself and the detector reduces to its pre-fractional form.
inline double phase_offset(int p, int phases) {
  return (static_cast<double>(p) - 0.5 * static_cast<double>(phases - 1)) /
         static_cast<double>(phases);
}

// Index of bit |bit| at phase |p| in a |phases|-per-bit grid.
inline size_t grid_index(int bit, int p, int phases) {
  return static_cast<size_t>(bit) * static_cast<size_t>(phases) +
         static_cast<size_t>(p);
}

// Resample |bit_count| bit periods of |line|, starting from the bit whose
// centre is at sample |t0|, onto a fixed |phases|-per-bit grid. Output element
// k * phases + p holds bit k at phase p. Written into |out| (resized to fit)
// so a caller sweeping candidate phases can reuse one buffer.
//
// Doing this once per lock, rather than interpolating inside the trellis,
// keeps the interpolation cost linear in the packet length however many times
// the detector reads a sample.
void resample_bit_grid(const int16_t* line, size_t sample_count, double t0,
                       double spb, int bit_count, int phases,
                       std::vector<double>& out) {
  out.resize(static_cast<size_t>(bit_count) * static_cast<size_t>(phases));
  for (int k = 0; k < bit_count; ++k) {
    for (int p = 0; p < phases; ++p) {
      const double t =
          t0 + (static_cast<double>(k) + phase_offset(p, phases)) * spb;
      out[grid_index(k, p, phases)] =
          teletext_interpolate_sample(line, sample_count, t);
    }
  }
}

// Channel model unknowns: kChannelTaps bit-spaced taps for each sample phase
// within the bit period, plus one DC offset shared by all phases (the black
// level the burst sits on, which the caller's nominal levels only approximate
// on tape; it is a property of the line, not of where in a bit it is read).
constexpr int kMaxFitUnknowns = kChannelTaps * kMaxMlseSamplesPerBit + 1;

// Nonzero entries of one fit equation: the taps of a single phase plus the
// shared offset. Every other unknown is zero for that equation.
constexpr int kFitEquationTerms = kChannelTaps + 1;

struct ChannelFit {
  bool ok = false;
  // Sample phases the fit covers; taps[p] is meaningful for p < phases.
  int phases = 1;
  std::array<std::array<double, kChannelTaps>, kMaxMlseSamplesPerBit> taps{};
  double offset = 0.0;
  // Mean over the phases of the summed taps: the modelled black-to-'1' step of
  // the recovered data. The tap sum is the channel's DC response, so it is the
  // same quantity at every phase and averaging simply steadies it.
  double gain = 0.0;
  // RMS fit residual over the preamble, in level-domain counts, taken over
  // every phase — the same per-sample scale as the single-phase fit, so the
  // residual gates carry across unchanged.
  double residual = 0.0;
};

using FitMatrix =
    std::array<std::array<double, kMaxFitUnknowns>, kMaxFitUnknowns>;
using FitVector = std::array<double, kMaxFitUnknowns>;

// Solve the leading |n| by |n| system in place by Gaussian elimination with
// partial pivoting. False when the matrix is singular to working precision.
bool solve_in_place(FitMatrix& a, FitVector& b, int n) {
  for (int col = 0; col < n; ++col) {
    int pivot = col;
    for (int row = col + 1; row < n; ++row) {
      if (std::abs(a[row][col]) > std::abs(a[pivot][col])) {
        pivot = row;
      }
    }
    if (std::abs(a[pivot][col]) < 1e-9) {
      return false;
    }
    std::swap(a[col], a[pivot]);
    std::swap(b[col], b[pivot]);
    for (int row = col + 1; row < n; ++row) {
      const double factor = a[row][col] / a[col][col];
      if (factor == 0.0) {
        continue;
      }
      for (int k = col; k < n; ++k) {
        a[row][k] -= factor * a[col][k];
      }
      b[row] -= factor * b[col];
    }
  }
  for (int row = n - 1; row >= 0; --row) {
    double sum = b[row];
    for (int k = row + 1; k < n; ++k) {
      sum -= a[row][k] * b[k];
    }
    b[row] = sum / a[row][row];
  }
  return true;
}

// Least-squares fit of the channel response to the known preamble, read off a
// |phases|-per-bit grid that starts at the first run-in bit (see
// resample_bit_grid). Only preamble positions whose whole tap window falls
// inside the known bits contribute equations; with five taps that is 20 of the
// 24 bits, so the fit has 20 × |phases| equations for kChannelTaps × |phases|
// + 1 unknowns.
//
// Each phase gets its own tap vector — a fractionally-spaced channel model.
// The bit period is not an integer number of samples, so the three positions
// scored within a bit sit at different points on the channel's pulse response
// and a single bit-spaced tap vector cannot describe all of them at once.
//
// |bit_at| supplies the transmitted bit at a grid bit index and |first|/|last|
// bound the positions that contribute equations; the preamble fit passes the
// known preamble over the preamble bits, and the decision-directed refit of the
// experiment passes the decided payload over the whole packet.
template <typename BitAt>
ChannelFit fit_channel_range(const std::vector<double>& grid, int phases,
                             BitAt bit_at, int first, int last) {
  ChannelFit fit;
  fit.phases = phases;
  const int unknowns = kChannelTaps * phases + 1;
  const int offset_index = unknowns - 1;
  // Clear only the leading |unknowns| block rather than the whole
  // kMaxFitUnknowns square. Acquisition fits one phase — 36 entries of the
  // 256 — and runs this once per candidate bit phase of every line, so the
  // difference is not academic.
  FitMatrix normal;
  FitVector rhs;
  for (int r = 0; r < unknowns; ++r) {
    rhs[static_cast<size_t>(r)] = 0.0;
    for (int c = 0; c < unknowns; ++c) {
      normal[static_cast<size_t>(r)][static_cast<size_t>(c)] = 0.0;
    }
  }

  std::array<int, kFitEquationTerms> index{};
  std::array<double, kFitEquationTerms> value{};
  value[kChannelTaps] = 1.0;  // DC offset
  index[kChannelTaps] = offset_index;
  for (int k = first; k <= last; ++k) {
    for (int j = 0; j < kChannelTaps; ++j) {
      value[static_cast<size_t>(j)] =
          static_cast<double>(bit_at(k - kChannelCentre + j));
    }
    for (int p = 0; p < phases; ++p) {
      for (int j = 0; j < kChannelTaps; ++j) {
        index[static_cast<size_t>(j)] = p * kChannelTaps + j;
      }
      const double y = grid[grid_index(k, p, phases)];
      for (int r = 0; r < kFitEquationTerms; ++r) {
        const size_t row = static_cast<size_t>(index[static_cast<size_t>(r)]);
        const double vr = value[static_cast<size_t>(r)];
        for (int c = 0; c < kFitEquationTerms; ++c) {
          normal[row][static_cast<size_t>(index[static_cast<size_t>(c)])] +=
              vr * value[static_cast<size_t>(c)];
        }
        rhs[row] += vr * y;
      }
    }
  }

  if (!solve_in_place(normal, rhs, unknowns)) {
    return fit;
  }
  for (int p = 0; p < phases; ++p) {
    double phase_gain = 0.0;
    for (int j = 0; j < kChannelTaps; ++j) {
      const int tap_index = p * kChannelTaps + j;
      const double tap = rhs[static_cast<size_t>(tap_index)];
      fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)] = tap;
      phase_gain += tap;
    }
    fit.gain += phase_gain;
  }
  fit.gain /= static_cast<double>(phases);
  fit.offset = rhs[static_cast<size_t>(offset_index)];

  double sum_sq = 0.0;
  int count = 0;
  for (int k = first; k <= last; ++k) {
    for (int p = 0; p < phases; ++p) {
      double predicted = fit.offset;
      for (int j = 0; j < kChannelTaps; ++j) {
        predicted += fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)] *
                     static_cast<double>(bit_at(k - kChannelCentre + j));
      }
      const double error = grid[grid_index(k, p, phases)] - predicted;
      sum_sq += error * error;
      ++count;
    }
  }
  fit.residual = std::sqrt(sum_sq / static_cast<double>(count));
  fit.ok = true;
  return fit;
}

// The channel fit against the known preamble: the acquisition and detection
// model of every line.
ChannelFit fit_preamble_channel(const std::vector<double>& grid, int phases) {
  return fit_channel_range(
      grid, phases, [](int k) { return preamble_bit(k); }, kChannelCentre,
      kPreambleBits - 1 - (kChannelTaps - 1 - kChannelCentre));
}

// Energy of the fitted channel's response to one bit: the squared change, over
// every sample the detector scores, that flipping a single bit makes to the
// waveform the channel would have produced. On a noiseless line that is exactly
// the path-metric penalty of getting that bit wrong, which is what makes it the
// natural full-confidence reference for the margins below.
double channel_bit_energy(const ChannelFit& fit) {
  double energy = 0.0;
  for (int p = 0; p < fit.phases; ++p) {
    for (int j = 0; j < kChannelTaps; ++j) {
      const double tap =
          fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)];
      energy += tap * tap;
    }
  }
  return energy;
}

// Viterbi detection of |bit_count| payload bits from |grid| (a
// fit.phases-per-bit grid, see resample_bit_grid), the first payload bit being
// grid bit |first_payload_bit|. The trellis starts from the known tail of the
// framing code rather than an unknown state, so the first payload bits are
// detected with the same context as the rest. Returns the RMS reconstruction
// error over the evaluated grid samples.
//
// When |bit_errors_out| is non-null it receives |bit_count| RMS reconstruction
// errors, one per payload bit, in level-domain counts — the same quantity the
// return value summarises, resolved along the packet so its shape can be read.
//
// When |bit_confidence_out| is non-null it receives |bit_count| decision
// confidences in 0 … 1: the amount by which the best path through the trellis
// carrying the opposite decision for that bit costs more than the winning path,
// as a fraction of channel_bit_energy(). The winning path's cost is a forward
// quantity and the cost of finishing from any state a backward one, so the
// exact margin for every bit at once needs one extra sweep of the trellis —
// which, with the branch metrics of the forward sweep kept, is a sweep of adds
// and comparisons rather than of arithmetic on samples.
double mlse_detect(const std::vector<double>& grid, int first_payload_bit,
                   const ChannelFit& fit, int bit_count, uint8_t* bits_out,
                   float* bit_errors_out, float* bit_confidence_out,
                   int tail_bits) {
  constexpr int kStateMask = kMlseStates - 1;
  const int phases = fit.phases;
  const auto grid_bits =
      static_cast<int>(grid.size() / static_cast<size_t>(phases));

  // The channel spreads each bit kChannelCentre bit periods forward, so the
  // last payload samples are only fully determined kChannelCentre steps after
  // the last payload bit. Run the trellis on past the payload and discard
  // those trailing bits, otherwise the final bits are decided by no sample of
  // their own and come back arbitrary.
  //
  // |tail_bits| > 0 goes further and terminates the trellis: the line returns
  // to black after the 360th bit (ETSI EN 300 706 §7.1), so those trailing bits
  // are known zeros, and forcing them constrains the last payload bits with
  // evidence on both sides — the same footing every other bit is decided on.
  const int steps = bit_count + std::max(kChannelCentre, tail_bits);

  // Expected sample for each (phase, state, new bit): the tap window spans the
  // new bit and the kChannelTaps - 1 bits held in the state. Each phase reads
  // the channel at a different point of its pulse response, so each has its
  // own tap vector and therefore its own table.
  using ExpectedTable =
      std::array<std::array<std::array<double, 2>, kMlseStates>,
                 kMaxMlseSamplesPerBit>;
  const auto build_expected = [phases](const ChannelFit& channel) {
    ExpectedTable table{};
    for (int p = 0; p < phases; ++p) {
      const auto& taps = channel.taps[static_cast<size_t>(p)];
      for (int s = 0; s < kMlseStates; ++s) {
        double base = channel.offset;
        for (int j = 0; j < kChannelTaps - 1; ++j) {
          const int shift = kChannelTaps - 2 - j;
          base += taps[static_cast<size_t>(j)] *
                  static_cast<double>((s >> shift) & 1);
        }
        table[static_cast<size_t>(p)][static_cast<size_t>(s)][0] = base;
        table[static_cast<size_t>(p)][static_cast<size_t>(s)][1] =
            base + taps[kChannelTaps - 1];
      }
    }
    return table;
  };
  const ExpectedTable expected = build_expected(fit);

  const size_t trellis = static_cast<size_t>(steps) * kMlseStates;
  std::vector<uint8_t> back_bit(trellis, 0);
  std::vector<uint8_t> back_state(trellis, 0);

  // Confidence needs the trellis read backwards as well as forwards, which
  // needs what the forward sweep knew: the cost of reaching each state at each
  // step, and the branch metric of each transition out of it. Both are kept
  // only when a caller asked for confidence — together they are an order of
  // magnitude more memory than the traceback, and the acquisition passes have
  // no use for them.
  const bool want_confidence = bit_confidence_out != nullptr;
  std::vector<double> forward_cost;
  std::vector<double> branch_metric;
  if (want_confidence) {
    forward_cost.assign(trellis, std::numeric_limits<double>::infinity());
    branch_metric.assign(trellis * 2, 0.0);
  }

  std::array<double, kMlseStates> cost{};
  cost.fill(std::numeric_limits<double>::infinity());
  int init_state = 0;
  for (int i = 0; i < kChannelTaps - 1; ++i) {
    init_state |= preamble_bit(kPreambleBits - 1 - i) << i;
  }
  cost[static_cast<size_t>(init_state)] = 0.0;

  int evaluated = 0;
  for (int k = 0; k < steps; ++k) {
    // At step k the sample fully determined by the state and the new bit is
    // the one kChannelCentre bit periods back. The first steps therefore
    // evaluate samples that sit inside the preamble: the channel has smeared
    // the leading payload bits back into them, and they are the only evidence
    // those bits get on that side, so skipping them costs accuracy in the
    // first byte.
    const int sample_bit = first_payload_bit + k - kChannelCentre;
    const bool has_sample = sample_bit >= 0 && sample_bit < grid_bits;
    const double* observed =
        has_sample ? &grid[grid_index(sample_bit, 0, phases)] : nullptr;
    evaluated += has_sample ? 1 : 0;

    // Past the payload the transmitted bits are known black, so a terminated
    // trellis has only the zero branch to offer.
    const int branches = (tail_bits > 0 && k >= bit_count) ? 1 : 2;

    // Branch metrics of this step. They depend on the state and the new bit but
    // not on the path that reached the state, so they are computed once here
    // and used by both sweeps.
    std::array<std::array<double, 2>, kMlseStates> step_metric{};
    if (has_sample) {
      for (int s = 0; s < kMlseStates; ++s) {
        for (int bit = 0; bit < branches; ++bit) {
          double sum_sq = 0.0;
          // Every phase sample of this bit is evidence for the same branch,
          // which is what multiplies the evidence per bit by |phases|.
          for (int p = 0; p < phases; ++p) {
            const double error =
                observed[p] -
                expected[static_cast<size_t>(p)][static_cast<size_t>(s)]
                        [static_cast<size_t>(bit)];
            sum_sq += error * error;
          }
          step_metric[static_cast<size_t>(s)][static_cast<size_t>(bit)] =
              sum_sq;
        }
      }
    }
    if (want_confidence) {
      const size_t base = static_cast<size_t>(k) * kMlseStates;
      for (int s = 0; s < kMlseStates; ++s) {
        forward_cost[base + static_cast<size_t>(s)] =
            cost[static_cast<size_t>(s)];
        for (int bit = 0; bit < 2; ++bit) {
          branch_metric[(base + static_cast<size_t>(s)) * 2 +
                        static_cast<size_t>(bit)] =
              step_metric[static_cast<size_t>(s)][static_cast<size_t>(bit)];
        }
      }
    }

    std::array<double, kMlseStates> next{};
    next.fill(std::numeric_limits<double>::infinity());
    for (int s = 0; s < kMlseStates; ++s) {
      const double from = cost[static_cast<size_t>(s)];
      if (!std::isfinite(from)) {
        continue;
      }
      for (int bit = 0; bit < branches; ++bit) {
        const double metric =
            from +
            step_metric[static_cast<size_t>(s)][static_cast<size_t>(bit)];
        const int to = ((s << 1) | bit) & kStateMask;
        if (metric < next[static_cast<size_t>(to)]) {
          next[static_cast<size_t>(to)] = metric;
          const size_t slot =
              static_cast<size_t>(k) * kMlseStates + static_cast<size_t>(to);
          back_bit[slot] = static_cast<uint8_t>(bit);
          back_state[slot] = static_cast<uint8_t>(s);
        }
      }
    }
    cost = next;
  }

  int state = 0;
  double best = std::numeric_limits<double>::infinity();
  if (tail_bits >= kChannelTaps - 1) {
    // Enough known-zero bits were pushed through to fill the state register, so
    // the terminal state is not a choice: it is all zeros.
    best = cost[0];
    state = 0;
  }
  const bool terminated = std::isfinite(best);
  if (!terminated) {
    for (int s = 0; s < kMlseStates; ++s) {
      if (cost[static_cast<size_t>(s)] < best) {
        best = cost[static_cast<size_t>(s)];
        state = s;
      }
    }
  }
  std::vector<uint8_t> decided(static_cast<size_t>(steps), 0);
  for (int k = steps - 1; k >= 0; --k) {
    const size_t slot =
        static_cast<size_t>(k) * kMlseStates + static_cast<size_t>(state);
    decided[static_cast<size_t>(k)] = back_bit[slot];
    state = back_state[slot];
  }
  for (int k = 0; k < bit_count; ++k) {
    bits_out[k] = decided[static_cast<size_t>(k)];
  }

  if (bit_errors_out != nullptr) {
    // Per-bit reconstruction error: the decided sequence pushed back through
    // the fitted channel, compared with the samples actually read. The trellis
    // ran on past the payload, so the bits the channel spreads into the final
    // payload samples are decided (or known, on a terminated trellis) as well,
    // and the last bits are measured on the same evidence as the rest. Bits
    // before the payload are the known preamble.
    const auto bit_value = [&](int index) {
      if (index < 0) {
        return static_cast<double>(preamble_bit(first_payload_bit + index));
      }
      return static_cast<double>(decided[static_cast<size_t>(index)]);
    };
    for (int n = 0; n < bit_count; ++n) {
      const int sample_bit = first_payload_bit + n;
      if (sample_bit >= grid_bits) {
        break;
      }
      double sum_sq = 0.0;
      for (int p = 0; p < phases; ++p) {
        double predicted = fit.offset;
        for (int j = 0; j < kChannelTaps; ++j) {
          predicted +=
              fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)] *
              bit_value(n - kChannelCentre + j);
        }
        const double error =
            grid[grid_index(sample_bit, p, phases)] - predicted;
        sum_sq += error * error;
      }
      bit_errors_out[n] =
          static_cast<float>(std::sqrt(sum_sq / static_cast<double>(phases)));
    }
  }

  if (want_confidence) {
    // Backward sweep. remaining[s] is the cheapest way of finishing the trellis
    // from state s at the step under consideration, so the cheapest path that
    // takes a named branch is the forward cost of its origin, plus the branch,
    // plus the remaining cost of where it lands. Comparing that against the
    // winning path's cost gives the margin of the decision at that step —
    // exactly, and for every bit in one sweep.
    const double energy = channel_bit_energy(fit);
    std::array<double, kMlseStates> remaining{};
    for (int s = 0; s < kMlseStates; ++s) {
      // A terminated trellis may only finish in the all-zero state; otherwise
      // any state will do, as the traceback above assumed.
      remaining[static_cast<size_t>(s)] =
          (terminated && s != 0) ? std::numeric_limits<double>::infinity()
                                 : 0.0;
    }

    for (int k = steps - 1; k >= 0; --k) {
      const int branches = (tail_bits > 0 && k >= bit_count) ? 1 : 2;
      const size_t base = static_cast<size_t>(k) * kMlseStates;

      if (k < bit_count) {
        const int flipped =
            1 - static_cast<int>(decided[static_cast<size_t>(k)]);
        double best_flipped = std::numeric_limits<double>::infinity();
        for (int s = 0; s < kMlseStates; ++s) {
          const double from = forward_cost[base + static_cast<size_t>(s)];
          const int to = ((s << 1) | flipped) & kStateMask;
          const double rest = remaining[static_cast<size_t>(to)];
          if (!std::isfinite(from) || !std::isfinite(rest)) {
            continue;
          }
          const double total =
              from +
              branch_metric[(base + static_cast<size_t>(s)) * 2 +
                            static_cast<size_t>(flipped)] +
              rest;
          best_flipped = std::min(best_flipped, total);
        }
        // Normalised by the penalty an undamaged line would impose on the same
        // flip, and clipped there: past that the decision is not usefully more
        // certain, and noise pushing in the helpful direction can carry the
        // margin above it.
        double confidence = 1.0;
        if (std::isfinite(best_flipped) && energy > 0.0) {
          confidence = std::clamp((best_flipped - best) / energy, 0.0, 1.0);
        }
        bit_confidence_out[k] = static_cast<float>(confidence);
      }

      std::array<double, kMlseStates> previous{};
      for (int s = 0; s < kMlseStates; ++s) {
        double cheapest = std::numeric_limits<double>::infinity();
        for (int bit = 0; bit < branches; ++bit) {
          const int to = ((s << 1) | bit) & kStateMask;
          const double rest = remaining[static_cast<size_t>(to)];
          if (!std::isfinite(rest)) {
            continue;
          }
          cheapest = std::min(
              cheapest, branch_metric[(base + static_cast<size_t>(s)) * 2 +
                                      static_cast<size_t>(bit)] +
                            rest);
        }
        previous[static_cast<size_t>(s)] = cheapest;
      }
      remaining = previous;
    }
  }

  // The metric accumulated one squared error per evaluated grid sample: every
  // phase of the payload's own bits, of the kChannelCentre preamble bits above,
  // and of whatever trailing bits a terminated trellis reached. Normalising by
  // that count keeps the returned residual on the same per-sample scale
  // whatever the grid rate and however the trellis was run.
  if (evaluated == 0) {
    return std::numeric_limits<double>::infinity();
  }
  return std::sqrt(best / static_cast<double>(evaluated * phases));
}

}  // namespace

double teletext_interpolate_sample(const int16_t* line, size_t sample_count,
                                   double t) {
  if (line == nullptr || sample_count == 0) {
    return 0.0;
  }
  const double floor_t = std::floor(t);
  const auto base = static_cast<int64_t>(floor_t);
  const double x = t - floor_t;
  const auto last = static_cast<int64_t>(sample_count) - 1;

  double p0 = 0.0;
  double p1 = 0.0;
  double p2 = 0.0;
  double p3 = 0.0;
  if (base >= 1 && base + 2 <= last) {
    // Whole four-point window inside the line, which is every read of a data
    // burst: this is the hot path, taken once per bit phase of every candidate
    // lock, so it skips the per-tap clamp entirely.
    const auto* window = line + (base - 1);
    p0 = static_cast<double>(window[0]);
    p1 = static_cast<double>(window[1]);
    p2 = static_cast<double>(window[2]);
    p3 = static_cast<double>(window[3]);
  } else {
    const auto at = [line, last](int64_t index) {
      const int64_t clamped = std::min(std::max(index, int64_t{0}), last);
      return static_cast<double>(line[static_cast<size_t>(clamped)]);
    };
    p0 = at(base - 1);
    p1 = at(base);
    p2 = at(base + 1);
    p3 = at(base + 2);
  }

  // Catmull-Rom cubic in Horner form.
  const double a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
  const double b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
  const double c = -0.5 * p0 + 0.5 * p2;
  return ((a * x + b) * x + c) * x + p1;
}

std::string_view teletext_reject_reason_name(TeletextRejectReason reason) {
  switch (reason) {
    case TeletextRejectReason::kNone:
      return "none";
    case TeletextRejectReason::kInsufficientSamples:
      return "insufficient samples";
    case TeletextRejectReason::kAmplitudeGate:
      return "amplitude gate";
    case TeletextRejectReason::kNoRunInLock:
      return "no run-in lock";
    case TeletextRejectReason::kRunInAmplitude:
      return "run-in amplitude";
    case TeletextRejectReason::kRunInPattern:
      return "run-in pattern";
    case TeletextRejectReason::kFramingCodeMiss:
      return "framing code miss";
    case TeletextRejectReason::kInvalidMrag:
      return "invalid MRAG";
    case TeletextRejectReason::kNoPreambleLock:
      return "no preamble lock";
    case TeletextRejectReason::kPreambleResidual:
      return "preamble residual";
    case TeletextRejectReason::kPayloadResidual:
      return "payload residual";
    case TeletextRejectReason::kParityFraction:
      return "parity fraction";
  }
  return "unknown";
}

uint8_t teletext_hamming84_encode(uint8_t value) {
  // ETSI EN 300 706 §8.2 encoding equations. Bit numbering: spec bit 1 (first
  // transmitted) is the byte LSB, so P1..P4 occupy bits 0/2/4/6 and D1..D4
  // bits 1/3/5/7.
  const int d1 = (value >> 0) & 1;
  const int d2 = (value >> 1) & 1;
  const int d3 = (value >> 2) & 1;
  const int d4 = (value >> 3) & 1;
  const int p1 = 1 ^ d1 ^ d3 ^ d4;
  const int p2 = 1 ^ d1 ^ d2 ^ d4;
  const int p3 = 1 ^ d1 ^ d2 ^ d3;
  const int p4 = 1 ^ p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ d4;
  return static_cast<uint8_t>((p1 << 0) | (d1 << 1) | (p2 << 2) | (d2 << 3) |
                              (p3 << 4) | (d3 << 5) | (p4 << 6) | (d4 << 7));
}

int teletext_hamming84_decode(uint8_t byte) {
  // ETSI EN 300 706 §8.2: Hamming 8/4 has minimum distance 4, so every byte
  // within Hamming distance 1 of a codeword decodes to that codeword (single
  // errors corrected, including protection-bit errors) and every byte at
  // distance 2 is uncorrectable (double error detected). A 256-entry table
  // realises exactly that decision rule.
  static const auto kTable = [] {
    std::array<int8_t, 256> table{};
    table.fill(-1);
    for (int value = 0; value < 16; ++value) {
      const uint8_t code =
          teletext_hamming84_encode(static_cast<uint8_t>(value));
      table[code] = static_cast<int8_t>(value);
      for (int bit = 0; bit < 8; ++bit) {
        table[code ^ (1u << bit)] = static_cast<int8_t>(value);
      }
    }
    return table;
  }();
  return kTable[byte];
}

std::string teletext_packet_to_hex(
    const std::array<uint8_t, kTeletextPacketBytes>& bytes, size_t byte_count) {
  const size_t count = std::min(byte_count, kTeletextPacketBytes);
  std::string hex;
  hex.reserve(count * 2);
  for (size_t i = 0; i < count; ++i) {
    hex.push_back(kHexDigits[bytes[i] >> 4]);
    hex.push_back(kHexDigits[bytes[i] & 0x0F]);
  }
  return hex;
}

std::string teletext_packet_to_hex(
    const std::array<uint8_t, kTeletextPacketBytes>& bytes,
    const TeletextPacketConfidence& confidence, size_t byte_count) {
  const size_t count = std::min(byte_count, kTeletextPacketBytes);
  std::string hex = teletext_packet_to_hex(bytes, count);
  hex.reserve(count * 3);
  for (size_t i = 0; i < count; ++i) {
    const double scaled = static_cast<double>(confidence[i]) *
                          static_cast<double>(kTeletextConfidenceLevels - 1);
    const int level = static_cast<int>(std::lround(std::clamp(
        scaled, 0.0, static_cast<double>(kTeletextConfidenceLevels - 1))));
    hex.push_back(kHexDigits[level]);
  }
  return hex;
}

std::optional<std::array<uint8_t, kTeletextPacketBytes>> teletext_hex_to_packet(
    std::string_view hex) {
  const auto observed = teletext_hex_to_observed_packet(hex);
  // 625-line packets only: the array carries no length, so a shorter packet
  // would reach the caller as eight bytes it never received (see the header).
  if (!observed.has_value() || observed->byte_count != kTeletextPacketBytes) {
    return std::nullopt;
  }
  return observed->bytes;
}

std::optional<TeletextObservedPacket> teletext_hex_to_observed_packet(
    std::string_view hex) {
  // The string's length names the packet length and says whether the
  // confidence suffix is there. Both are read from the same table of the
  // lengths a WST service actually transmits, so a string of any other length
  // is rejected rather than truncated to one that fits.
  size_t byte_count = 0;
  bool with_confidence = false;
  for (const size_t candidate : kPacketByteLengths) {
    if (hex.size() == candidate * 2) {
      byte_count = candidate;
      break;
    }
    if (hex.size() == candidate * 3) {
      byte_count = candidate;
      with_confidence = true;
      break;
    }
  }
  if (byte_count == 0) {
    return std::nullopt;
  }

  TeletextObservedPacket packet;
  packet.byte_count = byte_count;
  packet.confidence.fill(1.0F);
  for (size_t i = 0; i < byte_count; ++i) {
    const int high = hex_nibble(hex[i * 2]);
    const int low = hex_nibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    packet.bytes[i] = static_cast<uint8_t>((high << 4) | low);
  }
  if (with_confidence) {
    for (size_t i = 0; i < byte_count; ++i) {
      const int level = hex_nibble(hex[byte_count * 2 + i]);
      if (level < 0) {
        return std::nullopt;
      }
      packet.confidence[i] = static_cast<float>(
          static_cast<double>(level) /
          static_cast<double>(kTeletextConfidenceLevels - 1));
    }
    packet.has_confidence = true;
  }
  return packet;
}

TeletextSlicer::TeletextSlicer(double sample_rate, double bit_rate,
                               TeletextSlicerOptions options)
    : sample_rate_(sample_rate),
      samples_per_bit_(sample_rate / bit_rate),
      options_(options),
      packet_bytes_(teletext_packet_bytes(options.system)),
      payload_bits_(static_cast<int>(packet_bytes_ * 8)),
      data_one_fraction_(options.system == TeletextSystem::kWst525
                             ? kDataOneLevelFraction525
                             : kDataOneLevelFraction625),
      search_start_samples_((options.system == TeletextSystem::kWst525
                                 ? kRunInSearchStartUs525
                                 : kRunInSearchStartUs625) *
                            sample_rate / 1e6),
      search_end_samples_((options.system == TeletextSystem::kWst525
                               ? kRunInSearchEndUs525
                               : kRunInSearchEndUs625) *
                          sample_rate / 1e6) {}

TeletextSlicer::TeletextSlicer(double sample_rate, TeletextSystem system,
                               TeletextSlicerOptions options)
    : TeletextSlicer(sample_rate, teletext_bit_rate(system), [system, options] {
        TeletextSlicerOptions resolved = options;
        resolved.system = system;
        return resolved;
      }()) {}

TeletextLineResult TeletextSlicer::new_result() const {
  TeletextLineResult result;
  result.packet_bytes = packet_bytes_;
  return result;
}

TeletextLineResult TeletextSlicer::slice(const int16_t* line,
                                         size_t sample_count,
                                         int16_t black_level,
                                         int16_t white_level) const {
  TeletextLineResult result = new_result();

  const double spb = samples_per_bit_;
  // Whole packet (§7.1 / ITU-R BT.653 Table 1a-1b row 1.7) must fit after the
  // earliest search start.
  const double search_start = search_start_samples_;
  const double min_samples =
      search_start + (kRunInBits + kFramingBits + payload_bits_) * spb + 2.0;
  if (line == nullptr || static_cast<double>(sample_count) < min_samples) {
    return reject(result, TeletextRejectReason::kInsufficientSamples);
  }

  // Nominal '1' amplitude above black level (ETSI EN 300 706 §5.2 on 625
  // lines, ITU-R BT.653 Table 1b on 525).
  const double nominal_amplitude =
      data_one_fraction_ *
      (static_cast<double>(white_level) - static_cast<double>(black_level));
  if (nominal_amplitude <= 0.0) {
    return reject(result, TeletextRejectReason::kInsufficientSamples);
  }
  const double amplitude_gate = kAmplitudeGateFraction * nominal_amplitude;

  // Step 1 — coarse gate: most VBI lines are empty; reject immediately when
  // the line never rises meaningfully above black level.
  int16_t peak = black_level;
  const auto gate_begin = static_cast<size_t>(search_start);
  for (size_t i = gate_begin; i < sample_count; ++i) {
    peak = std::max(peak, line[i]);
  }
  if (static_cast<double>(peak) - static_cast<double>(black_level) <
      amplitude_gate) {
    return reject(result, TeletextRejectReason::kAmplitudeGate);
  }

  // Step 2 — detection. The threshold detector runs unless MLSE was asked
  // for outright; under kAuto the MLSE fallback sees only lines that carry a
  // data burst (they passed the coarse gate) but would not lock, so a source
  // the threshold detector handles pays nothing for the fallback.
  if (options_.detector != TeletextDetector::kMlse) {
    result = slice_threshold(line, sample_count, amplitude_gate);
    if (result.valid || options_.detector == TeletextDetector::kThreshold) {
      return result;
    }
  }
  return slice_mlse(line, sample_count, nominal_amplitude);
}

TeletextLineResult TeletextSlicer::slice_threshold(
    const int16_t* line, size_t sample_count, double amplitude_gate) const {
  TeletextLineResult result = new_result();
  result.detector = TeletextDetector::kThreshold;
  const double spb = samples_per_bit_;
  const double search_start = search_start_samples_;

  // Clock run-in acquisition (§6.1): correlate against a ±
  // alternating kernel at the known bit period across the §6.3 timing window.
  // The correlation peak yields the bit phase; the recovered 0/1 levels set
  // an adaptive slicing threshold local to the data burst.
  const double search_end = search_end_samples_;
  double best_corr = 0.0;
  double best_t0 = -1.0;
  for (double t0 = search_start; t0 <= search_end; t0 += kPhaseSearchStep) {
    double corr = 0.0;
    for (int k = 0; k < kRunInBits; ++k) {
      const double s = sample_at(line, t0 + k * spb);
      corr += (k % 2 == 0) ? s : -s;  // §6.1: even bit indices are ones
    }
    if (corr > best_corr) {
      best_corr = corr;
      best_t0 = t0;
    }
  }
  if (best_t0 < 0.0) {
    return reject(result, TeletextRejectReason::kNoRunInLock);
  }

  double ones_level = 0.0;
  double zeros_level = 0.0;
  for (int k = 0; k < kRunInBits; ++k) {
    const double s = sample_at(line, best_t0 + k * spb);
    ((k % 2 == 0) ? ones_level : zeros_level) += s;
  }
  ones_level /= kRunInBits / 2.0;
  zeros_level /= kRunInBits / 2.0;
  const double amplitude = ones_level - zeros_level;
  if (amplitude < amplitude_gate) {
    return reject(result, TeletextRejectReason::kRunInAmplitude);
  }
  const double threshold = 0.5 * (ones_level + zeros_level);

  // Validate the run-in bit pattern at the locked phase (§6.1, allowing for
  // the note's reduced leading ones).
  int run_in_matches = 0;
  for (int k = 0; k < kRunInBits; ++k) {
    const int bit = sample_at(line, best_t0 + k * spb) > threshold ? 1 : 0;
    run_in_matches += (bit == (k % 2 == 0 ? 1 : 0)) ? 1 : 0;
  }
  if (run_in_matches < kMinRunInMatches) {
    return reject(result, TeletextRejectReason::kRunInPattern);
  }

  // Framing-code lock (§6.2): resolve the run-in's even-bit-shift
  // ambiguity by searching for the framing code around the correlation lock.
  // Candidates are ranked by framing-code bit errors first, then by whether
  // the MRAG that follows is Hamming 8/4 decodable (§7.1.2, §8.2), then by
  // proximity to the lock. The MRAG tie-break matters because widening the
  // search also widens the chance of the payload happening to spell the
  // framing code; an alignment whose address bytes decode is the real one.
  const int max_framing_errors = options_.tolerant_framing ? 1 : 0;
  const auto bit_at = [&](double t) {
    return sample_at(line, t) > threshold ? 1 : 0;
  };
  const auto payload_start = [&](int shift) {
    return best_t0 + (kRunInBits + shift + kFramingBits) * spb;
  };

  int best_shift = 0;
  int best_errors = kFramingBits + 1;
  bool best_mrag_ok = false;
  bool found = false;
  // Whether any alignment matched the framing code, which separates "no
  // framing code here" from "framing code found but its MRAG was rejected".
  bool framing_matched = false;
  for (int shift = -kFramingSearchBits; shift <= kFramingSearchBits; ++shift) {
    int errors = 0;
    for (int k = 0; k < kFramingBits; ++k) {
      errors += (bit_at(best_t0 + (kRunInBits + shift + k) * spb) !=
                 kFramingCodeBits[k])
                    ? 1
                    : 0;
    }
    if (errors > max_framing_errors) {
      continue;
    }
    framing_matched = true;
    // The whole packet must fit at this alignment (§7.1).
    const double start = payload_start(shift);
    if (start < 0.0 || start + (payload_bits_ - 1) * spb + 1.0 >=
                           static_cast<double>(sample_count)) {
      continue;
    }

    std::array<uint8_t, 2> mrag{};
    for (int n = 0; n < 16; ++n) {
      if (bit_at(start + n * spb) != 0) {
        mrag[static_cast<size_t>(n) >> 3] |=
            static_cast<uint8_t>(1u << (n & 7));
      }
    }
    const bool mrag_ok = teletext_hamming84_decode(mrag[0]) >= 0 &&
                         teletext_hamming84_decode(mrag[1]) >= 0;
    if (options_.require_valid_mrag && !mrag_ok) {
      continue;
    }

    const bool better = !found || errors < best_errors ||
                        (errors == best_errors && mrag_ok && !best_mrag_ok) ||
                        (errors == best_errors && mrag_ok == best_mrag_ok &&
                         std::abs(shift) < std::abs(best_shift));
    if (better) {
      found = true;
      best_errors = errors;
      best_shift = shift;
      best_mrag_ok = mrag_ok;
    }
  }
  if (!found) {
    return reject(result, framing_matched
                              ? TeletextRejectReason::kInvalidMrag
                              : TeletextRejectReason::kFramingCodeMiss);
  }

  // Payload extraction (§7.1): the system's payload bits at bit-centre
  // positions, LSB first per byte. No Hamming/parity correction is applied:
  // the T42 contract preserves transmission coding.
  const double data_start = payload_start(best_shift);
  for (int n = 0; n < payload_bits_; ++n) {
    if (sample_at(line, data_start + n * spb) > threshold) {
      result.bytes[static_cast<size_t>(n) >> 3] |=
          static_cast<uint8_t>(1u << (n & 7));
    }
  }

  // The MRAG plausibility filter (§7.1.2, §8.2) was applied per candidate
  // alignment during the framing lock; the stored bytes stay as transmitted.

  result.framing_bit_errors = best_errors;
  result.data_start_sample = data_start;
  result.detector = TeletextDetector::kThreshold;
  result.valid = true;
  return result;
}

TeletextLineResult TeletextSlicer::slice_mlse(const int16_t* line,
                                              size_t sample_count,
                                              double nominal_amplitude) const {
  TeletextLineResult result = new_result();
  result.detector = TeletextDetector::kMlse;
  const double spb = samples_per_bit_;
  const double search_start = search_start_samples_;
  const double search_end = search_end_samples_;
  const double min_gain = kMlseMinGainFraction * nominal_amplitude;
  const int phases =
      std::clamp(options_.mlse_samples_per_bit, 1, kMaxMlseSamplesPerBit);

  // Bit-phase acquisition: fit the channel at every candidate phase and keep
  // the phase whose fit explains the known preamble best. This replaces the
  // threshold detector's run-in correlation, which needs the 3,47 MHz run-in
  // fundamental the band-limited channel has already removed — the framing
  // code and the shape of the smeared run-in still carry the timing.
  //
  // Acquisition reads one sample per bit even when detection will read three.
  // The fractionally-spaced fit gives each phase its own free tap vector, so a
  // sub-bit timing error is absorbed into the taps instead of showing in the
  // residual: its residual barely varies with t0, which is precisely what an
  // acquisition criterion must do. The bit-spaced fit does sharpen with phase,
  // and costs a fraction as much over the ~350 candidates of the §6.3 window.
  std::vector<double> grid;
  double best_t0 = -1.0;
  ChannelFit acquire_fit;
  for (double t0 = search_start; t0 <= search_end; t0 += kMlsePhaseStep) {
    // Both the preamble window and the payload that follows must fit.
    const double last_sample =
        t0 + (kPreambleBits + payload_bits_ - 1) * spb + 1.0;
    if (last_sample >= static_cast<double>(sample_count)) {
      continue;
    }
    resample_bit_grid(line, sample_count, t0, spb, kPreambleBits, 1, grid);
    const ChannelFit fit = fit_preamble_channel(grid, 1);
    if (!fit.ok || fit.gain < min_gain) {
      continue;
    }
    if (best_t0 < 0.0 || fit.residual < acquire_fit.residual) {
      best_t0 = t0;
      acquire_fit = fit;
    }
  }
  if (best_t0 < 0.0) {
    return reject(result, TeletextRejectReason::kNoPreambleLock);
  }

  // Resample the whole packet span once at the locked phase, and fit the
  // fractionally-spaced channel the detector runs against. Everything from
  // here on reads the grid rather than the line.
  //
  // The span runs kTrailingBits past the packet when the line is long enough to
  // hold them: those samples carry the known black level the line returns to
  // after the last packet bit, which is what lets the trellis be terminated
  // rather than left to end wherever it likes.
  const int trailing =
      (best_t0 + (kPreambleBits + payload_bits_ + kTrailingBits - 1) * spb +
           2.0 <
       static_cast<double>(sample_count))
          ? kTrailingBits
          : 0;
  resample_bit_grid(line, sample_count, best_t0, spb,
                    kPreambleBits + payload_bits_ + trailing, phases, grid);
  const ChannelFit best_fit = fit_preamble_channel(grid, phases);
  // Only a positive gain is required here — acquisition has already applied
  // the min-gain test, and a fractionally-spaced fit that then claims a weak
  // channel says so through its relative residual below, which is the same
  // judgement measured against the amplitude the fit itself asserts.
  if (!best_fit.ok || best_fit.gain <= 0.0) {
    return reject(result, TeletextRejectReason::kNoPreambleLock);
  }

  // A preamble the model cannot explain is not a preamble. Without this the
  // fit would happily describe whatever picture content sat under the search
  // window, and the MRAG filter alone lets through too much of it.
  const double relative_residual = best_fit.residual / best_fit.gain;
  // Recorded before the gate so a rejected line still reports how close it
  // came — the residual distribution is what the gate is tuned against.
  result.preamble_residual = relative_residual;
  if (relative_residual > kMlseMaxResidualFraction) {
    return reject(result, TeletextRejectReason::kPreambleResidual);
  }

  const double data_start = best_t0 + kPreambleBits * spb;
  std::vector<uint8_t> bits(static_cast<size_t>(payload_bits_), 0);

  // Pass 1 — detect against the channel fitted to the preamble. That channel
  // knows nothing of the payload, which is what makes this pass, and only this
  // pass, a usable judge of whether the line carries a teletext packet at all:
  // its residual is the gate below, and its per-bit errors are the timing
  // diagnostic.
  const double payload_residual =
      mlse_detect(grid, kPreambleBits, best_fit, payload_bits_, bits.data(),
                  result.payload_bit_errors.data(),
                  /*bit_confidence_out=*/nullptr, trailing) /
      best_fit.gain;
  result.payload_residual = payload_residual;
  // Per-bit errors carry the same normalisation as the residual above: a
  // fraction of the fitted gain, so profiles from lines of different amplitude
  // are on one scale.
  for (int n = 0; n < payload_bits_; ++n) {
    result.payload_bit_errors[static_cast<size_t>(n)] = static_cast<float>(
        static_cast<double>(result.payload_bit_errors[static_cast<size_t>(n)]) /
        best_fit.gain);
  }

  // The preamble gate above rules out lines whose first 24 bits are not a
  // preamble; this one rules out lines where they happened to be but the rest
  // is not a teletext packet, using every payload bit rather than 20.
  //
  // It is applied between the two passes deliberately. The refit below fits the
  // channel to the bits it is then judged against, so its residual is
  // optimistic by construction and would blunt this gate — measured on the
  // reference VHS captures, gating on the refit residual let through all but
  // one of the 7393 lines this discards. Rejecting here also spares a discarded
  // line the second detection.
  if (payload_residual > kMlseMaxPayloadResidualFraction) {
    return reject(result, TeletextRejectReason::kPayloadResidual);
  }

  // Pass 2 — refit the channel against the whole line and detect again.
  //
  // The preamble offers 20 usable equations per phase; a 625-line packet offers
  // 336 of them. Five bit-spaced taps fitted to the shorter set describe the
  // head of the channel's pulse response well and its tail poorly, and what the
  // tail costs is bytes. Refitting against the bits pass 1 decided and
  // re-running the trellis recovers them: on synthesized band-limited lines it
  // takes exact packet recovery from 29 of 48 to 48 of 48, and on the reference
  // VHS captures it raises the packets whose data bytes are all parity-clean by
  // 71 % (LP) and 34 % (SP).
  //
  // Decision-directed refitting is stable here because pass 1 is already right
  // about the overwhelming majority of the payload bits, and a handful of wrong
  // ones move a fit of that many equations by very little.
  //
  // The confidences come from this pass rather than the first: they describe
  // the bits that are emitted, and those are these. A refit that fails is the
  // one case where none are reported — re-running the first pass purely to
  // measure it would cost every line a third trellis for a case that needs a
  // singular normal matrix to arise at all.
  std::array<float, kTeletextPayloadBits> bit_confidence{};
  const int last_fittable = kPreambleBits + payload_bits_ + trailing - 1 -
                            (kChannelTaps - 1 - kChannelCentre);
  const int payload_bits = payload_bits_;
  const auto decided_bit = [&bits, payload_bits](int k) {
    if (k < kPreambleBits) {
      return preamble_bit(k);
    }
    const int index = k - kPreambleBits;
    // ETSI EN 300 706 §7.1: nothing follows the packet, so the line is black.
    return index < payload_bits
               ? static_cast<int>(bits[static_cast<size_t>(index)])
               : 0;
  };
  const ChannelFit refit = fit_channel_range(grid, phases, decided_bit,
                                             kChannelCentre, last_fittable);
  if (refit.ok && refit.gain > 0.0) {
    mlse_detect(grid, kPreambleBits, refit, payload_bits_, bits.data(),
                /*bit_errors_out=*/nullptr, bit_confidence.data(), trailing);
    result.has_byte_confidence = true;
  }

  // §7.1: LSB first per byte, transmission coding preserved.
  for (int n = 0; n < payload_bits_; ++n) {
    if (bits[static_cast<size_t>(n)] != 0) {
      result.bytes[static_cast<size_t>(n) >> 3] |=
          static_cast<uint8_t>(1u << (n & 7));
    }
  }

  // A byte is only as sure as the least sure of its eight bits: one wrong bit
  // is a wrong byte.
  if (result.has_byte_confidence) {
    for (size_t i = 0; i < packet_bytes_; ++i) {
      float lowest = 1.0F;
      for (size_t bit = 0; bit < 8; ++bit) {
        lowest = std::min(lowest, bit_confidence[i * 8 + bit]);
      }
      result.byte_confidence[i] = lowest;
    }
  }

  const int mrag_low = teletext_hamming84_decode(result.bytes[0]);
  const int mrag_high = teletext_hamming84_decode(result.bytes[1]);
  if (options_.require_valid_mrag && (mrag_low < 0 || mrag_high < 0)) {
    return reject(result, TeletextRejectReason::kInvalidMrag);
  }

  // Transmission-coding plausibility. Only reachable when the addressing
  // decoded, because the row number is what says whether the payload is
  // parity coded at all.
  if (mrag_low >= 0 && mrag_high >= 0) {
    const int row = ((mrag_low >> 3) & 0x01) | ((mrag_high << 1) & 0x1E);
    if (row <= kMlseLastParityCodedRow) {
      int odd_bytes = 0;
      for (size_t i = kMragBytes; i < packet_bytes_; ++i) {
        odd_bytes += odd_parity(result.bytes[i]) ? 1 : 0;
      }
      const double parity_fraction =
          static_cast<double>(odd_bytes) /
          static_cast<double>(packet_bytes_ - kMragBytes);
      if (parity_fraction < kMlseMinDataParityFraction) {
        return reject(result, TeletextRejectReason::kParityFraction);
      }

      // Parity-guided repair (TeletextSlicerOptions::parity_repair). ETSI EN
      // 300 706 §8.1 odd parity says a byte carries an odd number of bit
      // errors; the detector's own margins say which bit it was closest to
      // reading the other way. Flipping that one is the maximum-likelihood
      // repair of the single-bit case, and it restores parity, so the emitted
      // byte is still valid transmission coding.
      //
      // Deliberately after the plausibility gate above: repairing first would
      // manufacture the parity the gate tests for and let a false lock through.
      if (options_.parity_repair && result.has_byte_confidence) {
        // The header's page address and control bits are Hamming 8/4 (§9.3.1),
        // which is odd parity as well but carries its own correction — leave
        // those to the decoder that understands them.
        const size_t first_repairable =
            (row == 0) ? kHeaderControlBytes : kMragBytes;
        for (size_t i = first_repairable; i < packet_bytes_; ++i) {
          if (odd_parity(result.bytes[i])) {
            continue;
          }
          size_t weakest = 0;
          float lowest = std::numeric_limits<float>::infinity();
          int at_lowest = 0;
          for (size_t bit = 0; bit < 8; ++bit) {
            const float confidence = bit_confidence[i * 8 + bit];
            if (confidence < lowest) {
              lowest = confidence;
              weakest = bit;
              at_lowest = 1;
            } else if (confidence == lowest) {
              ++at_lowest;
            }
          }
          // Nothing to act on when the detector was equally unsure of two bits:
          // a coin toss between them is as likely to invent a wrong byte that
          // now passes parity as it is to mend one, and a byte that still fails
          // parity at least says so.
          if (at_lowest != 1) {
            continue;
          }
          result.bytes[i] =
              static_cast<uint8_t>(result.bytes[i] ^ (1u << weakest));
          ++result.repaired_bytes;
        }
      }
    }
  }

  // The framing code was fitted rather than matched bit by bit, so there is
  // no per-bit error count to report.
  result.framing_bit_errors = 0;
  result.data_start_sample = data_start;
  result.valid = true;
  return result;
}

}  // namespace orc
