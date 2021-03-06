// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * This file is part of Inovesa (github.com/Inovesa/Inovesa).
 * It's copyrighted by the contributors recorded
 * in the version control history of the file.
 */

#include "SM/WakePotentialMap.hpp"

vfps::WakePotentialMap::WakePotentialMap( std::shared_ptr<PhaseSpace> in
                                        , std::shared_ptr<PhaseSpace> out
                                        , const vfps::meshindex_t xsize
                                        , const vfps::meshindex_t ysize
                                        , ElectricField* field
                                        , const InterpolationType it
                                        , bool interpol_clamp
                                        , oclhptr_t oclh
                                        )
  : WakeKickMap( in,out,xsize,ysize,it,interpol_clamp,oclh
               #if INOVESA_USE_OPENCL == 1 and INOVESA_USE_OPENGL == 1
               , field->wakepotential_glbuf
               #endif // INOVESA_USE_OPENCL and INOVESA_USE_OPENGL
               )
  , _field(field)
{
}

vfps::WakePotentialMap::~WakePotentialMap() noexcept
#if INOVESA_ENABLE_CLPROFILING == 1
{
    saveTimings("WakePotentialMap");
}
#else
= default;
#endif // INOVESA_ENABLE_CLPROFILING

void vfps::WakePotentialMap::update()
{
    #if INOVESA_USE_OPENCL == 1
    if (_oclh) {
        _field->wakePotential();
        _oclh->enqueueCopyBuffer(_field->wakepotential_clbuf,_offset_clbuf,
                                0,0,sizeof(meshaxis_t)*_xsize);
        #if INOVESA_SYNC_CL == 1
        syncCLMem(OCLH::clCopyDirection::dev2cpu);
        #endif // INOVESA_SYNC_CL
    } else
    #endif // INOVESA_USE_OPENCL
    {
        std::copy_n(_field->wakePotential(),PhaseSpace::nb*_xsize,_offset.data());
    }
    updateSM();
}
