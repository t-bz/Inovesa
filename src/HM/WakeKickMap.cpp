/******************************************************************************/
/* Inovesa - Inovesa Numerical Optimized Vlesov-Equation Solver Application   */
/* Copyright (c) 2014-2015: Patrik Schönfeldt                                 */
/*                                                                            */
/* This file is part of Inovesa.                                              */
/* Inovesa is free software: you can redistribute it and/or modify            */
/* it under the terms of the GNU General Public License as published by       */
/* the Free Software Foundation, either version 3 of the License, or          */
/* (at your option) any later version.                                        */
/*                                                                            */
/* Inovesa is distributed in the hope that it will be useful,                 */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of             */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              */
/* GNU General Public License for more details.                               */
/*                                                                            */
/* You should have received a copy of the GNU General Public License          */
/* along with Inovesa.  If not, see <http://www.gnu.org/licenses/>.           */
/******************************************************************************/

#include "HM/WakeKickMap.hpp"

vfps::WakeKickMap::WakeKickMap(vfps::PhaseSpace* in, vfps::PhaseSpace* out,
					const unsigned int xsize, const unsigned int ysize,
					const std::vector<integral_t> wake,
					const InterpolationType it) :
	KickMap(in,out,xsize,ysize,it),
	_wake(wake)
{
}

void vfps::WakeKickMap::apply()
{
	integral_t* density = _in->projectionToX();
	for (unsigned int i=0;i<_xsize;i++) {
		_force[i] = 0;
		for (unsigned int j=0;j<_xsize;j++) {
			_force[i] += meshaxis_t(density[j]*_wake[_xsize+i-j]);
		}
	}

	// gridpoint matrix used for interpolation
	hi* ph = new hi[_it];

	// arrays of interpolation coefficients
	interpol_t* hmc = new interpol_t[_it];

	// translate force into HM
	for (unsigned int q_i=0; q_i< _xsize; q_i++) {
		for(unsigned int p_i=0; p_i< _ysize; p_i++) {
			// interpolation type specific q and p coordinates
			meshaxis_t pcoord;
			meshaxis_t qp_int;
			//Scaled arguments of interpolation functions:
			unsigned int jd; //numper of lower mesh point from p'
			interpol_t xip; //distance of p' from lower mesh point
			pcoord = _force[q_i];
			xip = modf(pcoord, &qp_int);
			jd = qp_int;

			if (jd < _ysize)
			{
				// create vectors containing interpolation coefficiants
				switch (_it) {
				case InterpolationType::none:
					hmc[0] = 1;
					break;
				case InterpolationType::linear:
					hmc[0] = interpol_t(1)-xip;
					hmc[1] = xip;
					break;
				case InterpolationType::quadratic:
					hmc[0] = xip*(xip-interpol_t(1))/interpol_t(2);
					hmc[1] = interpol_t(1)-xip*xip;
					hmc[2] = xip*(xip+interpol_t(1))/interpol_t(2);
					break;
				case InterpolationType::cubic:
					hmc[0] = (xip-interpol_t(1))*(xip-interpol_t(2))*xip
							* interpol_t(-1./6.);
					hmc[1] = (xip+interpol_t(1))*(xip-interpol_t(1))
							* (xip-interpol_t(2)) / interpol_t(2);
					hmc[2] = (interpol_t(2)-xip)*xip*(xip+interpol_t(1))
							/ interpol_t(2);
					hmc[3] = xip*(xip+interpol_t(1))*(xip-interpol_t(1))
							* interpol_t(1./6.);
					break;
				}

				// renormlize to minimize rounding errors
//				renormalize(hmc.size(),hmc.data());

				// write heritage map
				for (unsigned int j1=0; j1<_it; j1++) {
					unsigned int j0 = jd+j1-(_it-1)/2;
					if(j0 < _ysize ) {
						ph[j0].index = q_i*_ysize+j0;
						ph[j0].weight = hmc[j1];
					} else {
						ph[j0].index = 0;
						ph[j0].weight = 0;
					}
					_hinfo[(q_i*_ysize+p_i)*_ip+j1] = ph[j1];
				}
			}
		}
	}

	delete [] ph;
	delete [] hmc;

	// call original apply method
	HeritageMap::apply();
}

