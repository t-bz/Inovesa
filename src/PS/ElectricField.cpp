// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * This file is part of Inovesa (github.com/Inovesa/Inovesa).
 * It's copyrighted by the contributors recorded
 * in the version control history of the file.
 */

#include "PS/ElectricField.hpp"

#include "IO/FSPath.hpp"

#include <boost/math/constants/constants.hpp>
using boost::math::constants::pi;

vfps::ElectricField::ElectricField(std::shared_ptr<PhaseSpace> ps
                                  , const std::shared_ptr<Impedance> impedance
                                  , const std::vector<uint32_t> bucketnumber
                                  , const meshindex_t spacing_bins
                                  , oclhptr_t oclh
                                  , const double f_rev
                                  , const meshaxis_t revolutionpart
                                  , const meshaxis_t wakescalining
                                  )
  : volts(ps->getAxis(1)->delta()*ps->getScale(1,"ElectronVolt")/revolutionpart)
  , _nbunches(PhaseSpace::nb)
  , _bucket(bucketnumber)
  , _nmax(impedance->nFreqs())
  , _spacing_bins(spacing_bins)
  , _axis_freq(Ruler<frequency_t>( _nmax,0
                                 , 1/(ps->getDelta(0))
                                 , {{ "Hertz"
                                    , physcons::c/ps->getScale(0,"Meter")}}))
  // _axis_wake[PhaseSpace::nx] will be at position 0
  , _axis_wake(Ruler<meshaxis_t>(2*PhaseSpace::nx
                                , -ps->getDelta(0)*PhaseSpace::nx
                                , ps->getDelta(0)*(PhaseSpace::nx-1)
                                , {{"Meter", ps->getScale(0,"Meter")}}))
  , _phasespace(ps)
  , _formfactorrenorm(ps->getDelta(0)*ps->getDelta(0))
  , factor4WattPerHertz(2*impedance->factor4Ohms*ps->current*ps->current/f_rev)
  , factor4Watts(factor4WattPerHertz*_axis_freq.scale("Hertz"))
  , _csrintensity(Array::array1<csrpower_t>(_nbunches))
  , _csrspectrum(Array::array2<csrpower_t>(_nbunches,_nmax))
  , _isrspectrum(Array::array2<csrpower_t>(_nbunches,_nmax))
  , _impedance(impedance)
  , _oclh(oclh)
  , _wakefunction(nullptr)
  , _wakelosses(nullptr)
  , _wakelosses_fft(nullptr)
  , _wakepotential_padded(nullptr)
  #if INOVESA_USE_OPENCL == 1 and INOVESA_USE_OPENGL == 1
  , wakepotential_glbuf(0)
  #endif // INOVESA_USE_OPENCL and INOVESA_USE_OPENGL
  , _wakepotential(Array::array2<meshaxis_t>(PhaseSpace::nb,PhaseSpace::nx))
  , _fft_wakelosses(nullptr)
  #if INOVESA_USE_CLFFT == 1
  , _wakescaling(_oclh ? wakescalining : wakescalining/_nmax )
  #else // INOVESA_USE_CLFFT
  , _wakescaling(wakescalining/_nmax)
  #endif // INOVESA_USE_CLFFT
{
    #if INOVESA_USE_CLFFT == 1
    if (_oclh) {
        try {
            _bp_padded = new integral_t[_nmax];
            std::fill_n(_bp_padded,_nmax,0);
            _bp_padded_buf = cl::Buffer(_oclh->context,
                                          CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                          sizeof(*_bp_padded)*_nmax,_bp_padded);
            _formfactor = new impedance_t[_nmax];
            _formfactor_buf = cl::Buffer(_oclh->context,
                                           CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                           sizeof(*_formfactor)*_nmax,_formfactor);
            clfftCreateDefaultPlan(&_clfft_bunchprofile,
                                   _oclh->context(),CLFFT_1D,&_nmax);
            clfftSetPlanPrecision(_clfft_bunchprofile,CLFFT_SINGLE);
            clfftSetLayout(_clfft_bunchprofile, CLFFT_REAL, CLFFT_HERMITIAN_INTERLEAVED);
            clfftSetResultLocation(_clfft_bunchprofile, CLFFT_OUTOFPLACE);
            _oclh->bakeClfftPlan(_clfft_bunchprofile);
        } catch (cl::Error &e) {
            std::cerr << "Error: " << e.what() << std::endl
                      << "Shutting down OpenCL." << std::endl;
            _oclh.reset();
        }
        /* If setup using OpenCL was succesfull,
         * the code below is not needed.
         */
        return;
    }
    #endif // INOVESA_USE_CLFFT
    /* You might want to see this is an else block that also
     * is used if an error is thrown in the if statement. */
    {
        _bp_padded_fft = fft_alloc_real(_nmax);
        _bp_padded = reinterpret_cast<meshdata_t*>(_bp_padded_fft);

        _formfactor_fft = fft_alloc_complex(_nmax);
        _formfactor = reinterpret_cast<impedance_t*>(_formfactor_fft);

        _fft_bunchprofile = prepareFFT(_nmax,_bp_padded,_formfactor);
    }
}

vfps::ElectricField::ElectricField( std::shared_ptr<PhaseSpace> ps
                                  , const std::shared_ptr<Impedance> impedance
                                  , const std::vector<uint32_t> bucketnumber
                                  , const meshindex_t spacing_bins
                                  , oclhptr_t oclh
                                  , const double f_rev
                                  , const double revolutionpart
                                  , const double Ib, const double E0
                                  , const double sigmaE, const double dt
                                  )
  : ElectricField( ps,impedance
                 , bucketnumber, spacing_bins
                 , oclh
                 , f_rev,revolutionpart
                 , Ib*dt*physcons::c/ps->getScale(0,"Meter")/(ps->getDelta(1)*sigmaE*E0)
                 )
{
    _wakepotential = new meshaxis_t[PhaseSpace::nx];
    #if INOVESA_USE_OPENCL == 1
    if (_oclh) {
        #if INOVESA_USE_OPENGL == 1
        if (_oclh->OpenGLSharing()) {
            glGenBuffers(1, &wakepotential_glbuf);
            glBindBuffer(GL_ARRAY_BUFFER,wakepotential_glbuf);
            glBufferData( GL_ARRAY_BUFFER, PhaseSpace::nx*sizeof(*_wakepotential)
                        , 0, GL_DYNAMIC_DRAW);
            wakepotential_clbuf = cl::BufferGL( _oclh->context,CL_MEM_READ_WRITE
                                             , wakepotential_glbuf);
        } else
        #endif // INOVESA_USE_OPENGL
        {
            wakepotential_clbuf = cl::Buffer( _oclh->context, CL_MEM_READ_WRITE
                                           , sizeof(*_wakepotential)*PhaseSpace::nx);
        }
    #ifndef INOVESA_USE_CLFFT
    }
    #else // defined INOVESA_USE_CLFFT
        _wakelosses = new impedance_t[_nmax];

        // second half is initialized because it is not touched elsewhere
        std::fill_n(_wakelosses+_nmax/2,_nmax/2,0);


        _wakelosses_buf = cl::Buffer(_oclh->context, CL_MEM_READ_WRITE,
                                     sizeof(impedance_t)*_nmax);
        _wakepotential_padded = new meshaxis_t[_nmax];
        _wakepotential_padded_buf = cl::Buffer(_oclh->context,CL_MEM_READ_WRITE,
                                        sizeof(*_wakepotential_padded)*_nmax);
        const size_t nmax = _nmax;
        clfftCreateDefaultPlan(&_clfft_wakelosses,
                               _oclh->context(),CLFFT_1D,&nmax);
        clfftSetPlanPrecision(_clfft_wakelosses,CLFFT_SINGLE);
        clfftSetLayout(_clfft_wakelosses,
                       CLFFT_HERMITIAN_INTERLEAVED, CLFFT_REAL);
        clfftSetResultLocation(_clfft_wakelosses, CLFFT_OUTOFPLACE);
        _oclh->bakeClfftPlan(_clfft_wakelosses);

        std::string cl_code_wakelosses = R"(
            __kernel void wakeloss(__global impedance_t* wakelosses,
                                   const __global impedance_t* impedance,
                                   const __global impedance_t* formfactor)
            {
                const uint n = get_global_id(0);
                wakelosses[n] = cmult(impedance[n],formfactor[n]);
            }
            )";

        _clProgWakelosses = _oclh->prepareCLProg(cl_code_wakelosses);
        _clKernWakelosses = cl::Kernel(_clProgWakelosses, "wakeloss");
        _clKernWakelosses.setArg(0, _wakelosses_buf);
        _clKernWakelosses.setArg(1, _impedance->data_buf);
        _clKernWakelosses.setArg(2, _formfactor_buf);

        std::string cl_code_wakepotential = R"(
            __kernel void scalewp(__global data_t* wakepot,
                                  const data_t scaling,
                                  const __global data_t* wakepot_padded)
            {
                const uint g = get_global_id(0);
                wakepot[g] = scaling*wakepot_padded[g];
            }
            )";

        _clProgScaleWP = _oclh->prepareCLProg(cl_code_wakepotential);
        _clKernScaleWP = cl::Kernel(_clProgScaleWP, "scalewp");
        _clKernScaleWP.setArg(0, wakepotential_clbuf);
        _clKernScaleWP.setArg(1, _wakescaling);
        _clKernScaleWP.setArg(2, _wakepotential_padded_buf);
    } else
    #endif // INOVESA_USE_CLFFT
    #endif // INOVESA_USE_OPENCL
    {
        _wakelosses_fft = fft_alloc_complex(_nmax);
        _wakelosses=reinterpret_cast<impedance_t*>(_wakelosses_fft);

        _wakepotential_padded = fft_alloc_real(_nmax);

        _fft_wakelosses = prepareFFT(_nmax,_wakelosses,
                                     _wakepotential_padded);
    }
}

// (unmaintained) constructor for use of wake function
vfps::ElectricField::ElectricField( std::shared_ptr<PhaseSpace> ps
                                  , const std::shared_ptr<Impedance> impedance
                                  , const std::vector<uint32_t> bucketnumber
                                  , const meshindex_t spacing_bins
                                  , oclhptr_t oclh
                                  , const double f_rev
                                  , const double Ib, const double E0
                                  , const double sigmaE, const double dt
                                  , const double rbend, const double fs
                                  , const size_t nmax)
  : ElectricField( ps,impedance, bucketnumber, spacing_bins
                 , oclh
                 , f_rev,dt*physcons::c/(2*pi<double>()*rbend))
{
    _wakefunction = new meshaxis_t[2*PhaseSpace::nx];
    fftw_complex* z_fftw = fftw_alloc_complex(nmax);
    fftw_complex* zcsrf_fftw = fftw_alloc_complex(nmax);
    fftw_complex* zcsrb_fftw = fftw_alloc_complex(nmax); //for wake
    impedance_t* z = reinterpret_cast<impedance_t*>(z_fftw);
    impedance_t* zcsrf = reinterpret_cast<impedance_t*>(zcsrf_fftw);
    impedance_t* zcsrb = reinterpret_cast<impedance_t*>(zcsrb_fftw);

     const double g = - Ib*physcons::c*ps->getDelta(1)*dt
                    / (2*pi<double>()*fs*sigmaE*E0)/(pi<double>()*rbend);


    std::copy_n(_impedance->data(),std::min(nmax,_impedance->nFreqs()),z);
    if (_impedance->nFreqs() < nmax) {
        std::stringstream wavenumbers;
        wavenumbers << "(Known: n=" <<_impedance->nFreqs()
                    << ", needed: N=" << nmax << ")";
        Display::printText("Warning: Unknown impedance for high wavenumbers. "
                           +wavenumbers.str());
        std::fill_n(&z[_impedance->nFreqs()],nmax-_impedance->nFreqs(),
                    impedance_t(0));
    }

    fft_plan p3 = prepareFFT( nmax, z, zcsrf, fft_direction::forward );
    fft_plan p4 = prepareFFT( nmax, z, zcsrb, fft_direction::backward);

    fft_execute(p3);
    fft_destroy_plan(p3);
    fft_execute(p4);
    fft_destroy_plan(p4);

    /* This method works like a DFT of Z with Z(-n) = Z*(n).
     *
     * the element _wakefunction[PhaseSpace::nx] represents the self interaction
     * set this element (q==0) to zero to make the function anti-semetric
     */
    _wakefunction[0] = 0;
    for (size_t i=0; i< PhaseSpace::nx; i++) {
        // zcsrf[0].real() == zcsrb[0].real(), see comment above
        _wakefunction[PhaseSpace::nx-i] = g * zcsrf[i].real();
        _wakefunction[PhaseSpace::nx+i] = g * zcsrb[i].real();
    }
    fftw_free(z_fftw);
    fftw_free(zcsrf_fftw);
    fftw_free(zcsrb_fftw);
}

vfps::ElectricField::~ElectricField() noexcept
{
    delete [] _wakefunction;

    #if INOVESA_USE_CLFFT == 1
    if (_oclh) {
        delete [] _bp_padded;
        delete [] _formfactor;
        delete [] _wakepotential_padded;
        clfftDestroyPlan(&_clfft_bunchprofile);
        clfftDestroyPlan(&_clfft_wakelosses);
    } else
    #endif // INOVESA_USE_CLFFT
    {
        fft_free(_bp_padded_fft);
        fft_free(_formfactor_fft);
        if(_wakelosses_fft != nullptr) {
            fft_free(_wakelosses_fft);
        }
        if(_wakepotential_padded != nullptr) {
            fft_free(_wakepotential_padded);
        }
        fft_destroy_plan(_fft_bunchprofile);
        if (_fft_wakelosses != nullptr) {
            fft_destroy_plan(_fft_wakelosses);
        }
        fft_cleanup();
    }
}

vfps::csrpower_t* vfps::ElectricField::updateCSR(const frequency_t cutoff)
{
    #if INOVESA_USE_CLFFT == 1
    if (_oclh) {
        _oclh->enqueueCopyBuffer(_phasespace->projectionX_clbuf,_bp_padded_buf,
                                0,0,sizeof(_bp_padded[0])*PhaseSpace::nx);
        _oclh->enqueueBarrier();
        _oclh->enqueueDFT(_clfft_bunchprofile,CLFFT_FORWARD,
                          _bp_padded_buf,_formfactor_buf);
        _oclh->enqueueBarrier();

        _oclh->enqueueReadBuffer(_formfactor_buf,CL_TRUE,0,
                                _nmax*sizeof(*_formfactor),_formfactor);
    }
    #elif INOVESA_USE_OPENCL == 1
    if (_oclh) {
        _phasespace->syncCLMem(OCLH::clCopyDirection::dev2cpu);
    }
    #endif // INOVESA_USE_CLTTT
        for (uint32_t n = 0; n < _nbunches; n++) {
        #if INOVESA_USE_OPENCL == 1
        if (!_oclh)
        #endif // INOVESA_USE_OPENCL
        {
            // copy bunch profile to be padded
            const vfps::projection_t* bp = _phasespace->getProjection(0)[n];
            std::copy_n(bp,PhaseSpace::nx,_bp_padded);

            //FFT charge density
            fft_execute(_fft_bunchprofile);
        }
        _csrintensity[n] = 0;

        for (unsigned int i=0; i<_nmax; i++) {
            frequency_t renorm(_formfactorrenorm);
            if (cutoff > 0) {
                renorm *= (1-std::exp(-std::pow((_axis_freq.scale("Hertz")*_axis_freq[i]/cutoff),2)));
            }

            // norm = squared magnitude
            _csrspectrum[n][i] = renorm * ((*_impedance)[i]).real()
                               * std::norm(_formfactor[i]);

            _csrintensity[n] += _axis_freq.delta()*_csrspectrum[n][i];
        }
    }

    return _csrspectrum.data();
}

vfps::meshaxis_t *vfps::ElectricField::wakePotential()
{
    #if INOVESA_USE_CLFFT == 1
    if (_oclh){
        _oclh->enqueueCopyBuffer(_phasespace->projectionX_clbuf,_bp_padded_buf,
                                0,0,sizeof(*_bp_padded)*PhaseSpace::nx);
        _oclh->enqueueBarrier();
        _oclh->enqueueDFT(_clfft_bunchprofile,CLFFT_FORWARD,
                         _bp_padded_buf,_formfactor_buf);
        _oclh->enqueueBarrier();

        _oclh->enqueueNDRangeKernel( _clKernWakelosses,cl::NullRange,
                                          cl::NDRange(_nmax));
        _oclh->enqueueBarrier();
        _oclh->enqueueDFT(_clfft_wakelosses,CLFFT_BACKWARD,
                         _wakelosses_buf,_wakepotential_padded_buf);
        _oclh->enqueueBarrier();
        _oclh->enqueueNDRangeKernel( _clKernScaleWP,cl::NullRange,
                                          cl::NDRange(_nmax));
        _oclh->enqueueBarrier();
        #if INOVESA_SYNC_CL == 1
        syncCLMem(OCLH::clCopyDirection::dev2cpu);
        #endif // INOVESA_SYNC_CL
    } else
    #elif INOVESA_USE_OPENCL == 1
    if (_oclh) {
        _phasespace->syncCLMem(OCLH::clCopyDirection::dev2cpu);
    }
    #endif // INOVESA_USE_OPENCL
    {
        // copy bunch profiles to have correct padding
        const vfps::projection_t* bp= _phasespace->getProjection(0);
        for (uint32_t b=0; b<PhaseSpace::nb; b++) {
            std::copy_n( bp+b*PhaseSpace::nx
                       , PhaseSpace::nx,_bp_padded+_bucket[b]*_spacing_bins);
        }

        /* Fourier transorm bunch profile (_bp_padded),
         * result will be saved to _formfactor.
         *
         * FFTW R2C only computes elements 0...n/2, and
         * sets second half of output array to 0.
         * This is because
         *   Re(Y[n-i]) = Re(Y[i]), and
         *   Im(Y[n-i]) = -Im(Y[i]).
         */
        fft_execute(_fft_bunchprofile);

        for (unsigned int i=0; i<_nmax/2; i++) {
            _wakelosses[i]= (*_impedance)[i] *_formfactor[i];
        }

        //Fourier transorm wakelosses
        fft_execute(_fft_wakelosses);

        for (size_t b=0; b<PhaseSpace::nb; b++) {
            for (size_t x=0; x<PhaseSpace::nx; x++) {
                _wakepotential[b][x] = _wakescaling
                            * _wakepotential_padded[_bucket[b]*_spacing_bins+x];
            }
        }
        #if INOVESA_USE_OPENCL == 1
        #ifndef INOVESA_USE_CLFFT
        if (_oclh) {
            _oclh->enqueueWriteBuffer(wakepotential_clbuf,CL_TRUE,0,
                                     sizeof(*_wakepotential)*PhaseSpace::nx,
                                     _wakepotential);
        }
        #endif // INOVESA_USE_CLFFT
        #endif // INOVESA_USE_OPENCL
    }
    return _wakepotential;
}

#if INOVESA_USE_OPENCL == 1
void vfps::ElectricField::syncCLMem(OCLH::clCopyDirection dir)
{
    if (_oclh) {
    switch (dir) {
    case OCLH::clCopyDirection::cpu2dev:
        _oclh->enqueueWriteBuffer(_bp_padded_buf,CL_TRUE,0,
                                       sizeof(*_bp_padded)*_nmax,_bp_padded);
        _oclh->enqueueWriteBuffer(_formfactor_buf,CL_TRUE,0,
                                       sizeof(*_formfactor)*_nmax,_formfactor);
        #if INOVESA_USE_CLFFT == 1
        _oclh->enqueueWriteBuffer(_wakelosses_buf,CL_TRUE,0,
                                       sizeof(*_wakelosses)*_nmax,_wakelosses);
        #endif // INOVESA_USE_CLFFT
        _oclh->enqueueWriteBuffer(_wakepotential_padded_buf,CL_TRUE,0,
                                       sizeof(*_wakepotential_padded)*_nmax,
                                       _wakepotential_padded);
        _oclh->enqueueWriteBuffer(wakepotential_clbuf,CL_TRUE,0,
                                       sizeof(*_wakepotential)*PhaseSpace::nx,
                                       _wakepotential);
        break;
    case OCLH::clCopyDirection::dev2cpu:
        _oclh->enqueueReadBuffer(_bp_padded_buf,CL_TRUE,0,
                                      sizeof(*_bp_padded)*_nmax,_bp_padded);
        _oclh->enqueueReadBuffer(_formfactor_buf,CL_TRUE,0,
                                      sizeof(*_formfactor)*_nmax,_formfactor);
        #if INOVESA_USE_CLFFT == 1
        _oclh->enqueueReadBuffer(_wakelosses_buf,CL_TRUE,0,
                                      sizeof(*_wakelosses)*_nmax,_wakelosses);
        #endif // INOVESA_USE_CLFFT
        _oclh->enqueueReadBuffer(_wakepotential_padded_buf,CL_TRUE,0,
                                      sizeof(*_wakepotential_padded)*_nmax,
                                      _wakepotential_padded);
        _oclh->enqueueReadBuffer(wakepotential_clbuf,CL_TRUE,0,
                                      sizeof(*_wakepotential)*PhaseSpace::nx,
                                      _wakepotential);
        break;
    }
    }
}
#endif // INOVESA_USE_OPENCL

vfps::fft_complex* vfps::ElectricField::fft_alloc_complex(size_t n)
{
    fft_complex* rv;
    if (std::is_same<vfps::csrpower_t,float>::value) {
        rv = reinterpret_cast<fft_complex*>(fftwf_alloc_complex(n));
    } else if (std::is_same<vfps::csrpower_t,double>::value) {
        rv = reinterpret_cast<fft_complex*>(fftw_alloc_complex(n));
    }
    // initialize as 2*n long real array
    std::fill_n(reinterpret_cast<csrpower_t*>(rv),2*n,0);
    return rv;
}

vfps::integral_t* vfps::ElectricField::fft_alloc_real(size_t n)
{
    integral_t* rv;
    if (std::is_same<vfps::csrpower_t,float>::value) {
        rv = reinterpret_cast<integral_t*>(fftwf_alloc_real(n));
    } else if (std::is_same<vfps::csrpower_t,double>::value) {
        rv = reinterpret_cast<integral_t*>(fftw_alloc_real(n));
    }
    std::fill_n(rv,n,integral_t(0));
    return rv;
}

fftw_plan vfps::ElectricField::prepareFFT(size_t n, double* in,
                                          fftw_complex* out)
{
    fftw_plan plan = nullptr;

    std::stringstream wisdomfname;
    wisdomfname << "wisdom_r2c64_" << n << ".fftw";

    FSPath wisdompath(FSPath::datapath());
    wisdompath.append("fftwisdom/"+wisdomfname.str());

    // use wisdomfile, if it exists
    if (fftw_import_wisdom_from_filename(wisdompath.c_str()) != 0) {
        plan = fftw_plan_dft_r2c_1d(n,in,out,FFTW_WISDOM_ONLY|FFTW_PATIENT);
    }
    // if there was no wisdom (no or bad file), create some
    if (plan == nullptr) {
        plan = fftw_plan_dft_r2c_1d(n,in,out,FFTW_PATIENT);
        fftw_export_wisdom_to_filename(wisdompath.c_str());
        Display::printText("Created some wisdom at "+wisdompath.str());
    }
    return plan;
}


fftwf_plan vfps::ElectricField::prepareFFT(size_t n, float *in,
                                           fftwf_complex*out)
{
    fftwf_plan plan = nullptr;

    std::stringstream wisdomfname;
    wisdomfname << "wisdom_r2c32_" << n << ".fftw";

    FSPath wisdompath(FSPath::datapath());
    wisdompath.append("fftwisdom/"+wisdomfname.str());

    // use wisdomfile, if it exists
    if (fftwf_import_wisdom_from_filename(wisdompath.c_str()) != 0) {
        plan = fftwf_plan_dft_r2c_1d(n,in,out,FFTW_WISDOM_ONLY|FFTW_PATIENT);
    }
    // if there was no wisdom (no or bad file), create some
    if (plan == nullptr) {
        plan = fftwf_plan_dft_r2c_1d(n,in,out,FFTW_PATIENT);
        fftwf_export_wisdom_to_filename(wisdompath.c_str());
        Display::printText("Created some wisdom at "+wisdompath.str());
    }
    return plan;
}

fftwf_plan vfps::ElectricField::prepareFFT(size_t n, fftwf_complex *in,
                                           float *out)
{
    fftwf_plan plan = nullptr;

    std::stringstream wisdomfname;
    wisdomfname << "wisdom_c2r32_" << n << ".fftw";

    FSPath wisdompath(FSPath::datapath());
    wisdompath.append("fftwisdom/"+wisdomfname.str());

    // use wisdomfile, if it exists
    if (fftwf_import_wisdom_from_filename(wisdompath.c_str()) != 0) {
        plan = fftwf_plan_dft_c2r_1d(n,in,out,FFTW_WISDOM_ONLY|FFTW_PATIENT);
    }
    // if there was no wisdom (no or bad file), create some
    if (plan == nullptr) {
        plan = fftwf_plan_dft_c2r_1d(n,in,out,FFTW_PATIENT);
        fftwf_export_wisdom_to_filename(wisdompath.c_str());
        Display::printText("Created some wisdom at "+wisdompath.str());
    }
    return plan;
}

fftw_plan vfps::ElectricField::prepareFFT(size_t n,
                                          fftw_complex* in,
                                          fftw_complex* out,
                                          fft_direction direction)
{
    fftw_plan plan = nullptr;

    char dir;
    int_fast8_t sign;
    if (direction == fft_direction::backward) {
        dir = 'b';
        sign = +1;
    } else {
        dir = 'f';
        sign = -1;
    }

    std::stringstream wisdomfname;
    // find filename for wisdom
    wisdomfname << "wisdom_c" << dir << "c64_" << n << ".fftw";

    FSPath wisdompath(FSPath::datapath());
    wisdompath.append("fftwisdom/"+wisdomfname.str());

    // use wisdomfile, if it exists
    if (fftw_import_wisdom_from_filename(wisdompath.c_str()) != 0) {
        plan = fftw_plan_dft_1d(n,in,out,sign,FFTW_WISDOM_ONLY|FFTW_PATIENT);
    }
    // if there was no wisdom (no or bad file), create some
    if (plan == nullptr) {
        plan = fftw_plan_dft_1d(n,in,out,sign,FFTW_PATIENT);
        fftw_export_wisdom_to_filename(wisdompath.c_str());
        Display::printText("Created some wisdom at "+wisdompath.str());
    }
    return plan;
}


fftwf_plan vfps::ElectricField::prepareFFT(size_t n,
                                           fftwf_complex* in,
                                           fftwf_complex* out,
                                           fft_direction direction)
{
    fftwf_plan plan = nullptr;

    char dir;
    int_fast8_t sign;
    if (direction == fft_direction::backward) {
        dir = 'b';
        sign = +1;
    } else {
        dir = 'f';
        sign = -1;
    }

    std::stringstream wisdomfname;
    // find filename for wisdom
    wisdomfname << "wisdom_c" << dir << "c32_" << n << ".fftw";

    FSPath wisdompath(FSPath::datapath());
    wisdompath.append("fftwisdom/"+wisdomfname.str());

    // use wisdomfile, if it exists
    if (fftwf_import_wisdom_from_filename(wisdompath.c_str()) != 0) {
        plan = fftwf_plan_dft_1d(n,in,out,sign,FFTW_WISDOM_ONLY|FFTW_PATIENT);
    }
    // if there was no wisdom (no or bad file), create some
    if (plan == nullptr) {
        plan = fftwf_plan_dft_1d(n,in,out,sign,FFTW_PATIENT);
        fftwf_export_wisdom_to_filename(wisdompath.c_str());
        Display::printText("Created some wisdom at "+wisdompath.str());
    }
    return plan;
}
