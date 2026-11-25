/*
 * This file is part of libsidplayfp, a SID player engine.
 *
 * Copyright 2011-2026 Leandro Nini <drfiemost@users.sourceforge.net>
 * Copyright 2007-2010 Antti Lankila
 * Copyright 2004,2010 Dag Lem <resid@nimrod.no>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "Filter6581.h"

#include "Integrator6581.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace reSIDfp
{

Filter6581::~Filter6581()
{
    delete [] f0_dac;
}

void Filter6581::updateCenterFrequency()
{
    const uint16_t Vw = f0_dac[getFC()];
    hpIntegrator.setVw(Vw);
    bpIntegrator.setVw(Vw);
}

void Filter6581::setFilterCurve(double curvePosition)
{
    this->curvePosition = curvePosition;
    delete [] f0_dac;
    f0_dac = FilterModelConfig6581::getInstance()->getDAC(curvePosition, curveShape, CURVE_SHAPE_KNOTS);
    updateCenterFrequency();
}

void Filter6581::setCurveShape(const double* millivolts)
{
    bool same = true;
    for (int i = 0; i < CURVE_SHAPE_KNOTS; i++)
    {
        const double v = millivolts[i] / 1000.;
        if (v != curveShape[i])
        {
            curveShape[i] = v;
            same = false;
        }
    }
    if (!same)
        setFilterCurve(curvePosition);
}

void Filter6581::estimateCutoffs(const unsigned int* fcs, int count, double clockFrequency, double* hz) const
{
    const uint16_t* summer1 = getSummerTable(1);
    const uint16_t* res15 = getResonanceTable(15);
    const int32_t vinLow = getNormalizedVoice(-1.f, 255);
    const int32_t vinHigh = getNormalizedVoice(1.f, 255);

    // settle once at the top cutoff; at DC no current flows in the
    // integrators, so the settled state doesn't depend on the cutoff
    Integrator6581 hp0(hpIntegrator);
    Integrator6581 bp0(bpIntegrator);
    hp0.restart();
    bp0.restart();
    hp0.setVw(f0_dac[0x7ff]);
    bp0.setVw(f0_dac[0x7ff]);
    int32_t vbp0 = 0, vlp0 = 0;
    for (int i = 0; i < 8000; i++)
    {
        const int32_t vhp = summer1[vinLow + vlp0 + res15[vbp0]];
        vbp0 = hp0.solve(vhp);
        vlp0 = bp0.solve(vbp0);
    }

    const int maxCycles = static_cast<int>(clockFrequency / 25.);   // down to ~25 Hz
    std::vector<int32_t> v;
    v.reserve(maxCycles + 1);

    for (int n = 0; n < count; n++)
    {
        hz[n] = 0.;
        Integrator6581 h(hp0);
        Integrator6581 b(bp0);
        h.setVw(f0_dac[fcs[n] & 0x7ff]);
        b.setVw(f0_dac[fcs[n] & 0x7ff]);
        int32_t vbp = vbp0, vlp = vlp0;

        // The bandpass rings after a full-level step. The DC level moves
        // with the input (the model is nonlinear), so time the extrema
        // instead of zero crossings. Low in the curve the ringing slows down
        // as it decays (the cutoff depends on the signal level there): the
        // first half period, at full level, matches a full-level noise
        // measurement of the cutoff within about 15 %.
        v.clear();
        v.push_back(vbp0);
        double ext[2];
        int found = 0;
        int dir = 0;            // +1 looking for a maximum, -1 for a minimum
        int cand = 0, candEnd = 0;
        const int32_t thr = 4;
        for (int t = 1; t <= maxCycles && found < 2; t++)
        {
            const int32_t vhp = summer1[vinHigh + vlp + res15[vbp]];
            vbp = h.solve(vhp);
            vlp = b.solve(vbp);
            v.push_back(vbp);
            if (dir == 0)
            {
                if (vbp > vbp0 + thr) { dir = 1; cand = candEnd = t; }
                else if (vbp < vbp0 - thr) { dir = -1; cand = candEnd = t; }
                continue;
            }
            const int32_t c = v[cand];
            if ((dir > 0 && vbp > c) || (dir < 0 && vbp < c))
            {
                cand = candEnd = t;
            }
            else if (vbp == c && candEnd == t - 1)
            {
                candEnd = t;    // plateau
            }
            else if ((dir > 0 && vbp < c - thr) || (dir < 0 && vbp > c + thr))
            {
                double te;
                if (candEnd > cand)
                {
                    te = 0.5 * (cand + candEnd);
                }
                else
                {
                    // parabola through the three samples around the extremum
                    const double ym = v[cand - 1], y0 = v[cand], yp = v[cand + 1];
                    const double den = ym - 2. * y0 + yp;
                    te = cand + ((den != 0.) ? 0.5 * (ym - yp) / den : 0.);
                }
                ext[found] = te;
                found++;
                dir = -dir;
                cand = candEnd = t;
            }
        }
        if (found < 2 || ext[1] <= ext[0])
            continue;
        hz[n] = clockFrequency / (2. * (ext[1] - ext[0]));
    }
}

} // namespace reSIDfp
