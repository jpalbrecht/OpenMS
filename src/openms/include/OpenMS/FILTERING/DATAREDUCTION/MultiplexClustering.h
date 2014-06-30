// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2013.
//
// This software is released under a three-clause BSD license:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of any author or any participating institution
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// For a full list of authors, refer to the file AUTHORS.
// --------------------------------------------------------------------------
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL ANY OF THE AUTHORS OR THE CONTRIBUTING
// INSTITUTIONS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// --------------------------------------------------------------------------
// $Maintainer: Lars Nilse $
// $Authors: Lars Nilse $
// --------------------------------------------------------------------------

#ifndef OPENMS_FILTERING_DATAREDUCTION_MULTIPLEXCLUSTERING_H
#define OPENMS_FILTERING_DATAREDUCTION_MULTIPLEXCLUSTERING_H

#include <OpenMS/KERNEL/StandardTypes.h>
#include <OpenMS/TRANSFORMATIONS/RAW2PEAK/PeakPickerHiRes.h>
#include <OpenMS/FILTERING/DATAREDUCTION/PeakPattern.h>
#include <OpenMS/FILTERING/DATAREDUCTION/FilterResult.h>
#include <OpenMS/MATH/MISC/CubicSpline2d.h>
#include <OpenMS/FILTERING/DATAREDUCTION/SplineSpectrum.h>
#include <OpenMS/FILTERING/DATAREDUCTION/MultiplexFiltering.h>

#include <vector>
#include <algorithm>
#include <iostream>

namespace OpenMS
{
    /**
     * @brief clusters results from multiplex filtering
     * 
     * The multiplex filtering algorithm identified regions in the picked and
     * profile data that correspond to peptide features. This clustering algorithm
     * takes these filter results as input and groups data points belong to
     * the same peptide features. It makes use of the general purpose hierarchical
     * clustering implementation LocalClustering. 
     * 
     * @see MultiplexFiltering
     * @see LocalClustering
     */
    class OPENMS_DLLAPI MultiplexClustering
    {        
        public:
        /**
         * @brief constructor
         */
        MultiplexClustering(std::vector<std::vector<PeakPickerHiRes::PeakBoundary> > boundaries, double x);
        
        /**
         * @brief cluster
         */
        void cluster();
        
        /**
         * @brief rough estimation of the peak width at m/z
         */
        class OPENMS_DLLAPI PeakWidthEstimator
        {
            public:
            /**
            * @brief constructor
            */
            PeakWidthEstimator(int a);
        
            /**
            * @brief returns a
            */
            int getA() const;
        
            private:        
            /// hide default Ctor
            PeakWidthEstimator();
        
            /**
            * @brief a
            */
            int a_;
        
        };

        private:
        /**
         * @brief x
         */
        double x_;
        
   };
  
}

#endif /* MULTIPLEXCLUSTERING_H_ */
