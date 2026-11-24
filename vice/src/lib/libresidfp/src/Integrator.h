/*
 * This file is part of libsidplayfp, a SID player engine.
 *
 * Copyright 2011-2024 Leandro Nini <drfiemost@users.sourceforge.net>
 * Copyright 2007-2010 Antti Lankila
 * Copyright 2004, 2010 Dag Lem <resid@nimrod.no>
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

#ifndef INTEGRATOR_H
#define INTEGRATOR_H

#include <cstdint>

namespace reSIDfp
{

class Integrator
{
protected:
    mutable int32_t vx;
    mutable int32_t vc;

    /**
     * 1/C relative to the nominal capacitor, scaled by 2^16.
     * The charge on the capacitor changes by I*dt/C, so a capacitor that is
     * off by some percent from its nominal value (10 % ceramics on the C64
     * boards) changes the integration rate by the inverse of that.
     */
    int32_t capScale;

    Integrator() :
        vx(0),
        vc(0),
        capScale(1 << 16) {}

    /// Change in capacitor charge for a current, scaled by the capacitor.
    inline int32_t charge(int32_t dq) const
    {
        return (capScale == (1 << 16)) ? dq :
            static_cast<int32_t>((static_cast<int64_t>(dq) * capScale) >> 16);
    }

public:
    virtual int32_t solve(int32_t vi) const = 0;

    virtual ~Integrator() = default;

    void restart() { vx = 16384; vc = 16384; }

    /**
     * Set the capacitor value relative to the nominal one (1.0 = nominal).
     */
    void setCapacitor(double relative)
    {
        if (relative < 0.5) relative = 0.5;
        if (relative > 2.0) relative = 2.0;
        capScale = static_cast<int32_t>(65536.0 / relative + 0.5);
    }
};

} // namespace reSIDfp

#endif
