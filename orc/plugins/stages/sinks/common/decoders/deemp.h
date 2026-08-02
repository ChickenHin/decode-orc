/*
 * File:        deemp.h
 * Module:      orc-core
 * Purpose:     Noise-reduction and chroma post-filter coefficients
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2021 Chad Page
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

// The coefficients below were originally produced by ld-decode's
// scripts/filtermaker (which generated a much larger header covering the
// whole ld-decode chain); only the filters used by the orc chroma decoders
// are kept here. Each filter's scipy design call is recorded alongside it so
// the coefficients can be regenerated or adjusted without the original
// script. The sample rate is 4fsc NTSC (freq4 = 4 * 315/88 = 14.31818 MHz)
// unless noted otherwise.

#ifndef DEEMP_H
#define DEEMP_H

#include <array>

#include "iirfilter.h"

// SMPTE 170M-2004 §7.1: Luma noise-reduction high-pass FIR component.
// Used in doYNR() to extract the high-frequency luma component that is cored
// (zeroed) below the configured yNRLevel threshold.
// Design: scipy.signal.firwin(25, 1.80 / (freq4 / 2), window='hamming',
//         pass_zero=False) — 1.8 MHz high-pass at 4fsc NTSC.
const std::array<double, 25> c_nr_b = {
    1.141291975113614e-04,  -1.857019211291029e-03, -4.499636864042073e-03,
    -5.577680979937061e-03, -4.423694440267179e-04, 1.309163063177155e-02,
    2.861211356202848e-02,  3.029931283148555e-02,  1.098965697652802e-03,
    -6.398130386469833e-02, -1.492080690537196e-01, -2.223459379380252e-01,
    7.479077367478024e-01,  -2.223459379380252e-01, -1.492080690537196e-01,
    -6.398130386469833e-02, 1.098965697652803e-03,  3.029931283148557e-02,
    2.861211356202848e-02,  1.309163063177156e-02,  -4.423694440267185e-04,
    -5.577680979937061e-03, -4.499636864042074e-03, -1.857019211291030e-03,
    1.141291975113614e-04};
const std::array<double, 1> c_nr_a = {1.000000000000000e+00};

const IIRFilter<25, 1> f_nr(c_nr_b, c_nr_a);

// SMPTE 170M-2004 §7.2: Chroma noise-reduction high-pass FIR component.
// Used in doCNR() to extract the high-frequency chroma component (I and Q)
// that is cored below the configured cNRLevel threshold.
// Design: scipy.signal.firwin(17, 0.4 / (freq4 / 2), window='hamming',
//         pass_zero=False) — 400 kHz high-pass at 4fsc NTSC.
const std::array<double, 17> c_nrc_b = {
    -3.148569668063267e-03, -4.941974513425438e-03, -9.929538598536455e-03,
    -1.787793973911701e-02, -2.783702315543740e-02, -3.829928032339736e-02,
    -4.750186865627083e-02, -5.380281552534787e-02, 9.469899799540406e-01,
    -5.380281552534787e-02, -4.750186865627083e-02, -3.829928032339737e-02,
    -2.783702315543740e-02, -1.787793973911701e-02, -9.929538598536455e-03,
    -4.941974513425442e-03, -3.148569668063267e-03};
const std::array<double, 1> c_nrc_a = {1.000000000000000e+00};

const IIRFilter<17, 1> f_nrc(c_nrc_b, c_nrc_a);

// SMPTE 170M-2004 §7.2 (equiband): I and Q chroma lowpass filter.
// Design target: < 2 dB attenuation at 1.3 MHz; ≥ 20 dB at 3.6 MHz at 4fsc
// (14.3182 MHz). Verified response: −0.02 dB at 1.3 MHz, −34.8 dB at 3.6 MHz.
// Applied equally to both I and Q (equiband per SMPTE 170M-2004 §7.2 Note).
// Design: scipy.signal.remez(17, [0.0, 1.3, 3.8, freq4 / 2], [1.0, 0.0],
//         [1.0, 1.0], fs=freq4).
const std::array<double, 17> c_colorlp_b = {
    2.236562025869846e-03,  9.679572273064329e-03,  6.100849475810623e-03,
    -2.082153208645807e-02, -4.872917723725065e-02, -1.535300561979003e-02,
    1.137084573789944e-01,  2.775133160099456e-01,  3.533666167131518e-01,
    2.775133160099456e-01,  1.137084573789944e-01,  -1.535300561979003e-02,
    -4.872917723725065e-02, -2.082153208645807e-02, 6.100849475810623e-03,
    9.679572273064329e-03,  2.236562025869846e-03};

// PAL luma noise-reduction high-pass FIR component; same design as c_nr_b
// (the original generator emitted it as a separate filter, but the recipe is
// identical, so the coefficients match exactly).
// Design: scipy.signal.firwin(25, 1.80 / (freq4 / 2), window='hamming',
//         pass_zero=False).
const std::array<double, 25> c_nrpal_b = {
    1.141291975113614e-04,  -1.857019211291029e-03, -4.499636864042073e-03,
    -5.577680979937061e-03, -4.423694440267179e-04, 1.309163063177155e-02,
    2.861211356202848e-02,  3.029931283148555e-02,  1.098965697652802e-03,
    -6.398130386469833e-02, -1.492080690537196e-01, -2.223459379380252e-01,
    7.479077367478024e-01,  -2.223459379380252e-01, -1.492080690537196e-01,
    -6.398130386469833e-02, 1.098965697652803e-03,  3.029931283148557e-02,
    2.861211356202848e-02,  1.309163063177156e-02,  -4.423694440267185e-04,
    -5.577680979937061e-03, -4.499636864042074e-03, -1.857019211291030e-03,
    1.141291975113614e-04};
const std::array<double, 1> c_nrpal_a = {1.000000000000000e+00};

const IIRFilter<25, 1> f_nrpal(c_nrpal_b, c_nrpal_a);

#endif
