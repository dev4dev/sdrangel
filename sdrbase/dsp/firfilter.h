///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2020 Edouard Griffiths, F4EXB <f4exb06@gmail.com>               //
// Copyright (C) 2020 Kacper Michajłow <kasper93@gmail.com>                      //
// Copyright (C) 2021, 2023 Jon Beniston, M7RCE <jon@beniston.com>               //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
// (at your option) any later version.                                           //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cmath>
#include <cstdio>
#include "dsp/dsptypes.h"
#include "export.h"

namespace FirFilterGenerators
{
    SDRBASE_API void generateLowPassFilter(int nTaps, double sampleRate, double cutoff, std::vector<Real> &taps);
};

template <class Type>
class FirFilter
{
public:
    Type filter(Type sample)
    {
        Type acc = 0;
        unsigned int n_samples = m_samples.size();
        unsigned int n_taps = m_taps.size() - 1;
        unsigned int a = m_ptr;
        unsigned int b = a == n_samples - 1 ? 0 : a + 1;

        m_samples[m_ptr] = sample;

        for (unsigned int i = 0; i < n_taps; ++i)
        {
            acc += (m_samples[a] + m_samples[b]) * m_taps[i];

            a = (a == 0)             ? n_samples - 1 : a - 1;
            b = (b == n_samples - 1) ? 0             : b + 1;
        }

        acc += m_samples[a] * m_taps[n_taps];

        m_ptr = (m_ptr == n_samples - 1) ? 0 : m_ptr + 1;

        return acc;
    }

    void reset()
    {
        std::fill(m_samples.begin(), m_samples.end(), Type(0));
        m_ptr = 0;
    }

    // Print taps as a Matlab vector
    // To view:
    //   h=fvtool(filter);
    //   h.Fs=...
    void printTaps(const char *name)
    {
        printf("%s = [", name);
        for (int i = 0; i <= m_taps.size() - 1; ++i) {
            printf("%g ", m_taps[i]);
        }
        for (int i = m_taps.size() - 2; i >= 0; --i) {
            printf("%g ", m_taps[i]);
        }
        printf("];\n");
    }

protected:
    void init(int nTaps)
    {
        m_ptr = 0;
        m_samples.resize(nTaps);

        for (int i = 0; i < nTaps; i++) {
            m_samples[i] = 0;
        }
    }

protected:
    std::vector<Real> m_taps;
    std::vector<Type> m_samples;
    size_t m_ptr;
};

template <class T>
struct Lowpass : public FirFilter<T>
{
public:
    void create(int nTaps, double sampleRate, double cutoff)
    {
        this->init(nTaps);
        FirFilterGenerators::generateLowPassFilter(nTaps, sampleRate, cutoff, this->m_taps);
    }
};

template <class T>
struct Bandpass : public FirFilter<T>
{
    void create(int nTaps, double sampleRate, double lowCutoff, double highCutoff)
    {
        this->init(nTaps);
        FirFilterGenerators::generateLowPassFilter(nTaps, sampleRate, highCutoff, this->m_taps);
        std::vector<Real> highPass;
        FirFilterGenerators::generateLowPassFilter(nTaps, sampleRate, lowCutoff, highPass);

        for (size_t i = 0; i < highPass.size(); ++i) {
            highPass[i] = -highPass[i];
        }

        highPass[highPass.size() - 1] += 1;

        for (size_t i = 0; i < this->m_taps.size(); ++i) {
            this->m_taps[i] = -(this->m_taps[i] + highPass[i]);
        }

        this->m_taps[this->m_taps.size() - 1] += 1;
    }
};

template <class T>
struct Highpass : public FirFilter<T>
{
    void create(int nTaps, double sampleRate, double cutoff)
    {
        this->init(nTaps);
        FirFilterGenerators::generateLowPassFilter(nTaps, sampleRate, cutoff, this->m_taps);

        for (size_t i = 0; i < this->m_taps.size(); ++i) {
            this->m_taps[i] = -this->m_taps[i];
        }

        this->m_taps[this->m_taps.size() - 1] += 1;
    }
};

// Gaussian low-pass filter for chrominance bandwidth limiting.
// Tap generation ported from hacktv fir_gaussian_low_pass().
template <class T>
struct GaussianLowpass : public FirFilter<T>
{
    void create(int nTaps, double sampleRate, double cutoff)
    {
        this->init(nTaps);
        generateGaussianTaps(nTaps, sampleRate, cutoff, this->m_taps);
    }

    static int calcNtaps(double sampleRate, double cutoff)
    {
        int ntaps = (int)std::ceil(sampleRate / 1.35e6 / (cutoff / 1.4e6));
        return ntaps | 1;
    }

private:
    static void generateGaussianTaps(int nTaps, double sampleRate, double cutoff, std::vector<Real>& taps)
    {
        nTaps |= 1; // ensure odd
        int h = nTaps / 2;

        double f = 13.5e6 / sampleRate;
        double s = 354372.0 / cutoff;

        taps.resize(h + 1);

        double sum = 0.0;

        for (int x = 0; x <= h; x++)
        {
            double t = (double)x / 5.0 * f;
            double r = 1.0 / s * std::sqrt(2.0 * M_PI) * std::exp(-t * t / (2.0 * s * s));

            sum += r * (x > 0 ? 2.0 : 1.0);
            taps[h - x] = (Real)r; // taps[h] = center, taps[0] = outermost
        }

        // Normalize to unity gain
        double gain = 1.0 / sum;

        for (int i = 0; i <= h; i++) {
            taps[i] *= (Real)gain;
        }
    }
};
