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
// $Maintainer: Timo Sachsenberg $
// $Authors: Timo Sachsenberg $
// --------------------------------------------------------------------------

#include <OpenMS/KERNEL/StandardTypes.h>
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/CONCEPT/Constants.h>
#include <OpenMS/APPLICATIONS/TOPPBase.h>
#include <OpenMS/CHEMISTRY/ModificationsDB.h>
#include <OpenMS/DATASTRUCTURES/Param.h>
#include <OpenMS/FORMAT/FeatureXMLFile.h>
#include <OpenMS/FORMAT/FASTAFile.h>
#include <OpenMS/CHEMISTRY/Element.h>
#include <OpenMS/CHEMISTRY/ElementDB.h>
#include <OpenMS/MATH/STATISTICS/StatisticFunctions.h>
#include <OpenMS/MATH/MISC/BilinearInterpolation.h>
#include <OpenMS/TRANSFORMATIONS/RAW2PEAK/PeakPickerHiRes.h>
#include <OpenMS/MATH/MISC/NonNegativeLeastSquaresSolver.h>
#include <OpenMS/FORMAT/SVOutStream.h>
#include <OpenMS/FORMAT/TextFile.h>
#include <OpenMS/FILTERING/TRANSFORMERS/Normalizer.h>
#include <OpenMS/TRANSFORMATIONS/FEATUREFINDER/GaussModel.h>
#include <OpenMS/COMPARISON/SPECTRA/SpectrumAlignment.h>
#include <OpenMS/FORMAT/TextFile.h>
#include <OpenMS/FILTERING/TRANSFORMERS/ThresholdMower.h>

//#include "FastEMD/FastEMDWrapper.h"

#include <QtCore/QStringList>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <map>

#include <math.h>
#include <gsl/gsl_bspline.h>
#include <gsl/gsl_multifit.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#include <gsl/gsl_statistics.h>

using namespace OpenMS;
using namespace std;

typedef map< DoubleReal, DoubleReal> MapRateToScoreType;
typedef pair<DoubleReal, vector<DoubleReal> > IsotopePattern;
typedef vector<IsotopePattern> IsotopePatterns;

struct RateScorePair
{
  DoubleReal rate;
  DoubleReal score;
};

/// datastructure for reporting an incorporation event
struct SIPIncorporation
{
  DoubleReal rate; ///< rate
  
  DoubleReal correlation; ///< correlation coefficient
  
  DoubleReal abundance; ///< abundance of isotopologue
  
  PeakSpectrum theoretical; ///< peak spectrum as generated from the theoretical isotopic distribution
};

/// datastructure for reporting a peptide with one or more incorporation rates
struct SIPPeptide
{
  AASequence sequence;   ///< sequence of the peptide

  vector<String> accessions;  ///< protein accessions of the peptide

  bool unique; ///< if the peptide is unique and therefor identifies the protein umambigously 

  DoubleReal mz_theo; ///< theoretical mz

  DoubleReal score; ///< search engine score or q-value if fdr filtering is applied
  
  DoubleReal feature_rt; ///< measurement time of feature apex [s]
  
  DoubleReal feature_mz; ///< mz of feature apex [s]

  Int charge; ///< charge of the peptide feature

  DoubleReal mass_diff;  // 13C or 15N mass difference

  DoubleReal global_LR; ///< labeling ratio for the whole spectrum used to detect global drifts. 13C/(12C+13C) intensities. (15N analogous)

  vector<RateScorePair> correlation_maxima;
  
  MapRateToScoreType decomposition_map; // all rate to decomposition scores for the peptide
  
  MapRateToScoreType correlation_map; // all rate to correlation scores for the peptide

  DoubleReal RR; ///< R squared of NNLS fit
  
  DoubleReal explained_TIC_fraction; ///< fraction of the MS2 TIC that is explained by the maximum correlating decomposition weights

  Size non_zero_decomposition_coefficients; ///< decomposition coefficients significantly larger than 0

  PeakSpectrum reconstruction; ///< signal reconstruction (debugging)
  
  vector<DoubleReal> reconstruction_monoistopic; ///< signal reconstruction of natural peptide (at mono-isotopic peak)

  PeakSpectrum merged;

  PeakSpectrum filtered_merged;

  PeakSpectrum accumulated;

  vector<SIPIncorporation> incorporations;

  IsotopePatterns patterns;

  vector<PeakSpectrum> pattern_spectra;
};

struct SIPSummaryStatistic
{
  DoubleReal median_LR;
  DoubleReal median_RIA;
  DoubleReal stdev_LR;
  DoubleReal stdev_RIA;
};

/// datastructure for reporting a protein group with one or more SIP peptides
struct SIPProteinReport
{  
  String accession;
  String description;
  SIPSummaryStatistic protein_statistic;
  vector<SIPPeptide> peptides;
};

struct SIPGroupReport
{
  vector<SIPProteinReport> grouped_proteins;
  SIPSummaryStatistic group_statistics;
};

// A group of proteins (e.g. different cluster, each corresponding to a distinct RIA)
struct SIPGroupsReport
{
  map<Size, SIPGroupReport> map_id_to_protein_group;  // All proteins associated with a group  
};

class TOPPMetaProSIP : public TOPPBase
{
public:
  TOPPMetaProSIP()
    : ADDITIONAL_ISOTOPES(5),
      TOPPBase("MetaProSIP", "Performs proteinSIP on peptide features for elemental flux analysis.", false)
  {
  }			

protected:
  const Size ADDITIONAL_ISOTOPES; // extend isotopic patterns to collect other element higher isotopes at 100% incorporation
  String tmp_path_;
  String cluster_type_;
  String qc_output_directory_;
  void registerOptionsAndFlags_()
  {
    registerInputFile_("in_mzML","<file>","","Centroided MS1 data");
    setValidFormats_("in_mzML", ListUtils::create<String>("mzML"));

    registerInputFile_("in_fasta","<file>","","Protein sequence database");
    setValidFormats_("in_fasta", ListUtils::create<String>("fasta"));

    registerOutputFile_("out_csv","<file>","","Column separated file with feature fitting result.");
    setValidFormats_("out_csv", ListUtils::create<String>("csv"));

    registerOutputFile_("out_peptide_centric_csv","<file>","","Column separated file with peptide centric result.");
    setValidFormats_("out_peptide_centric_csv", ListUtils::create<String>("csv"));

    registerOutputFile_("out_protein_centric_csv","<file>","","Column separated file with protein centric result.");
    setValidFormats_("out_protein_centric_csv", ListUtils::create<String>("csv"));

    registerInputFile_("in_featureXML","<file>","","Feature data annotated with identifications (IDMapper)");
    setValidFormats_("in_featureXML", ListUtils::create<String>("featureXML"));

    registerDoubleOption_("mz_tolerance_ppm", "<tol>", 10.0, "Tolerance in ppm", false);
    
    registerDoubleOption_("intensity_threshold", "<tol>", 1000.0, "Intensity threshold to consider a peak.", false);

    registerDoubleOption_("correlation_threshold", "<tol>", 0.5, "Correlation threshold for reporting a pattern", false);

    registerDoubleOption_("weight_merge_window", "<tol>", 5.0, "Decomposition coefficients within +- this rate window will be combined", false);

    registerDoubleOption_("min_correlation_distance_to_averagine", "<tol>", 0.0, "Minimum difference in correlation between incorporation pattern and averagine pattern. Positive values filter all RIAs passing the correlation threshold but that also show a better correlation to an averagine peptide.", false);
    
    registerDoubleOption_("pattern_15N_TIC_threshold", "<threshold>", 0.95, "The most intense peaks of the theoretical pattern contributing to at least this TIC fraction are taken into account.", false, true);
    registerDoubleOption_("pattern_13C_TIC_threshold", "<threshold>", 0.95, "The most intense peaks of the theoretical pattern contributing to at least this TIC fraction are taken into account.", false, true);
    registerIntOption_("heatmap_bins", "<threshold>", 20, "Number of RIA bins for heat map generation.", false, true);

    registerOutputFile_("debug_patterns_name", "<file>", "", "mzML file debug spectra and patterns are generated in", false);
    setValidFormats_("debug_patterns_name", ListUtils::create<String>("mzML"));
    
    registerStringOption_("plot_extension", "<extension>", "png", "Extension used for plots (png|svg|pdf).", false);
    StringList valid_extensions;
    valid_extensions << "png" << "svg" << "pdf";
    setValidStrings_("plot_extension", valid_extensions);

    registerStringOption_("cluster_type", "<algorithm>", "emd", "Clustering algorithm used.", false);
    StringList valid_cluster_type;
    valid_cluster_type << "emd" << "profile";
    setValidStrings_("cluster_type", valid_cluster_type);

    registerStringOption_("qc_output_directory", "<directory>", "", "Output directory for the quality report", false);

    registerFlag_("use_15N", "Use 15N instead of 13C", false);
    
    registerFlag_("plot_merged", "Plot merged spectra", false);
    
    registerFlag_("filter_monoisotopic", "Try to filter out mono-isotopic patterns to improve detection of low RIA patterns", false);
    
    registerFlag_("cluster", "Perform grouping", false);        

    registerDoubleOption_("observed_peak_fraction", "<threshold>", 0.5, "Fraction of observed/expected peaks.", false, true);

    registerDoubleOption_("score_plot_yaxis_min", "<threshold>", 0.0, "The minimum value of the score axis. Values smaller than zero usually only make sense if the observed peak fraction is set to 0.", false, true);

    registerStringOption_("collect_method", "<method>", "correlation_maximum", "How RIAs are collected.", false, true);
    StringList valid_collect_method;
    valid_collect_method << "correlation_maximum" << "decomposition_maximum";
    setValidStrings_("collect_method", valid_collect_method);

    registerDoubleOption_("lowRIA_correlation_threshold", "<tol>", -1, "Correlation threshold for reporting low RIA patterns. Disable and take correlation_threshold value for negative values.", false, true);
  }
  
  ///< comparator for vectors of SIPPeptides based on their size. Used to sort by group size.
	struct SizeLess
		: public std::binary_function <vector<SIPPeptide>, vector<SIPPeptide>, bool>
	{
		inline bool operator () (const vector<SIPPeptide>& a, const vector<SIPPeptide>& b) const
		{
			return (a.size() < b.size());
		}
	};

	struct SequenceLess
		: public std::binary_function <pair<SIPPeptide, Size>, pair<SIPPeptide, Size>, bool>
	{
		inline bool operator () (const pair<SIPPeptide, Size>& a, const pair<SIPPeptide, Size>& b) const
		{
      return (a.first.sequence.toString() < b.first.sequence.toString());
		}
	};

  struct RIALess
    : public std::binary_function <SIPIncorporation, SIPIncorporation, bool>
  {
    inline bool operator () (const SIPIncorporation& a, const SIPIncorporation& b) const
    {
      return (a.rate < b.rate);
    }
  };

  

  ///< Determine score maxima from rate to score distribution using derivatives from akima spline interpolation
  vector<RateScorePair> getHighPoints(DoubleReal threshold, MapRateToScoreType& rate2score, Size NBREAK = 10)
  {
    vector<RateScorePair> high_points;
    const size_t n = rate2score.size() + 2;
    vector<double> x, y;

    // set proper boundaries
    x.push_back(-0.1);
    y.push_back(0);
    
    // copy data
    for (MapRateToScoreType::const_iterator it = rate2score.begin(); it != rate2score.end(); ++it)
    {
      x.push_back(it->first);
      y.push_back(it->second);
    }

    x.push_back(100.0);
    y.push_back(0);

    gsl_interp_accel *acc = gsl_interp_accel_alloc();
    gsl_interp_accel *acc_deriv = gsl_interp_accel_alloc();
    gsl_spline *spline = gsl_spline_alloc(gsl_interp_akima, n);     

    gsl_spline_init(spline, &(*x.begin()), &(*y.begin()), n);
    
    double last_dxdy = 0;
    for (double xi = x[0]; xi < x[n-1]; xi += 0.1)
    {    
      double dxdy = gsl_spline_eval_deriv(spline, xi, acc_deriv);
      double y = gsl_spline_eval(spline, xi, acc);
      
      if (last_dxdy > 0.0 && dxdy <= 0 && y > threshold)
      {
        RateScorePair rsp;
        rsp.rate = xi;
        rsp.score = y;
        high_points.push_back(rsp);
      }
      last_dxdy = dxdy;
    }

    gsl_spline_free (spline);
    gsl_interp_accel_free (acc);
    gsl_interp_accel_free (acc_deriv);
    return high_points;
  }

  /// Extracts isotopic intensities from seeds_rt.size() spectra in the given peak map.
  /// To reduce noise the mean intensity at each isotopic position is returned.
  vector<DoubleReal> extractIsotopicIntensitiesConsensus(Size element_count, DoubleReal mass_diff,
                                                         DoubleReal mz_tolerance_ppm,
                                                         const vector<DoubleReal>& seeds_rt,
                                                         DoubleReal seed_mz,
                                                         DoubleReal seed_charge,
                                                         const MSExperiment<Peak1D>& peak_map)
  {
    vector<vector<DoubleReal> > all_intensities;
    // extract intensities auf central spectrum (at time seeds_rt[0])
    vector<DoubleReal> isotopic_intensities = extractIsotopicIntensities(element_count, mass_diff, mz_tolerance_ppm, seeds_rt[0], seed_mz, seed_charge, peak_map);
    all_intensities.push_back(isotopic_intensities);

    // extract intensities auf neighbouring spectra
    for (Size i = 1; i < seeds_rt.size(); ++i)
    {
      vector<DoubleReal> tmp = extractIsotopicIntensities(element_count, mass_diff, mz_tolerance_ppm, seeds_rt[i], seed_mz, seed_charge, peak_map);
      all_intensities.push_back(tmp);
    }

    // calculate mean for each bucket
    for (Size p = 0; p != isotopic_intensities.size(); ++p)
    {
      DoubleReal sum = 0;
      for (Size i = 0; i != all_intensities.size(); ++i)
      {
        sum += all_intensities[i][p];     
      }
      isotopic_intensities[p] = sum / seeds_rt.size();
      
      //cout << isotopic_intensities[p] << endl;
    }

    return isotopic_intensities;
  }

  ///> Extracts peaks at the theoretical position of the isotopic intensities from seeds_rt.size() spectra in the given peak map.
  PeakSpectrum extractPeakSpectrumConsensus(Size element_count, DoubleReal mass_diff,
                                            const vector<DoubleReal>& seeds_rt,
                                            DoubleReal seed_mz,
                                            DoubleReal seed_charge,
                                            const MSExperiment<Peak1D>& peak_map)
  {
    MSExperiment<> to_merge;
    for (Size i = 0; i != seeds_rt.size(); ++i)
    {
      to_merge.addSpectrum(extractPeakSpectrum(element_count, mass_diff, seeds_rt[i], seed_mz, seed_charge, peak_map));
    }
    return mergeSpectra(to_merge);
  }

  ///> 
  PeakSpectrum filterPeakSpectrumForIsotopicPeaks(Size element_count, DoubleReal mass_diff, DoubleReal seed_mz,
                                                  DoubleReal seed_charge, const PeakSpectrum& spectrum, DoubleReal ppm = 10.0)
  {
    PeakSpectrum ret;
    DoubleReal iso_dist = mass_diff / seed_charge;

    for (PeakSpectrum::ConstIterator it = spectrum.begin(); it != spectrum.end(); ++it)
    {
      DoubleReal mz_dist = it->getMZ() - seed_mz;
      if (mz_dist < -seed_mz * ppm * 1e-6)
      {
        continue;
      }
      Size k = (Size)(mz_dist / iso_dist + 0.5);  // determine which k-th isotopic peak we probably are dealing with
      // determine ppm window the peak must lie in
      DoubleReal mz = seed_mz + k * mass_diff / seed_charge;
      DoubleReal min_mz = mz - mz * ppm * 1e-6;
      DoubleReal max_mz = mz + mz * ppm * 1e-6;
      if (it->getMZ() > min_mz && it->getMZ() < max_mz)
      {
        ret.push_back(*it);
      }
    }
    return ret;
  }

  ///> filter intensity to remove noise or additional incorporation peaks that otherwise might interfere with correlation calculation
  void filterIsotopicIntensities(vector<DoubleReal>::const_iterator& pattern_begin, vector<DoubleReal>::const_iterator& pattern_end,
                                 vector<DoubleReal>::const_iterator& intensities_begin, vector<DoubleReal>::const_iterator& intensities_end, DoubleReal TIC_threshold = 0.99)
  {
    if (std::distance(pattern_begin, pattern_end) != std::distance(intensities_begin, intensities_end))
    {
      cout << "Error: size of pattern and collected intensities don't match!" << endl;
    }

    if (pattern_begin == pattern_end)
    {
      return;
    }

    // determine order of peaks based on intensities
    vector<DoubleReal>::const_iterator b_it = pattern_begin;
    vector<DoubleReal>::const_iterator e_it = pattern_end;
    // create intensity to offset map for sorting
    vector<std::pair<DoubleReal, Int> > intensity_to_offset;
    for (; b_it != e_it; ++b_it)
    {
      std::pair<DoubleReal, Int> intensity_offset_pair = make_pair(*b_it, std::distance(pattern_begin, b_it));
      intensity_to_offset.push_back(intensity_offset_pair);  // pair: intensity, offset to pattern_begin iterator
    }
    // sort by intensity (highest first)
    std::sort(intensity_to_offset.begin(), intensity_to_offset.end(), std::greater<pair<DoubleReal, Int> >());

    // determine sequence of (neighbouring) peaks needed to achieve threshold * 100 % TIC in the patterns
    DoubleReal TIC = 0.0;
    Int min_offset = std::distance(pattern_begin, pattern_end);
    Int max_offset = 0;

    for (vector<std::pair<DoubleReal, Int> >::const_iterator it = intensity_to_offset.begin(); it != intensity_to_offset.end(); ++it)
    {
      TIC += it->first;
      if (it->second < min_offset)
      {
        min_offset = it->second;
      }

      if (it->second > max_offset)
      {
        max_offset = it->second;
      }

      if (TIC > TIC_threshold)
      {
        break;
      }
    }

    //cout << "before: " << std::distance(pattern_begin, pattern_end) << endl;

    vector<DoubleReal>::const_iterator tmp_pattern_it(pattern_begin);
    vector<DoubleReal>::const_iterator tmp_intensity_it(intensities_begin);
    
    std::advance(pattern_begin, min_offset);
    std::advance(intensities_begin, min_offset);
    std::advance(tmp_pattern_it, max_offset + 1);
    std::advance(tmp_intensity_it, max_offset + 1);

    pattern_end = tmp_pattern_it;
    intensities_end = tmp_intensity_it;

    //cout << "after: " << std::distance(pattern_begin, pattern_end) << " " << min_offset << " " << max_offset << endl;
  }

  ///< Calculates the correlation between measured isotopic_intensities and the theoretical isotopic patterns for all incorporation rates
  void calculateCorrelation(String seq, const vector<DoubleReal>& isotopic_intensities, IsotopePatterns patterns,
                            MapRateToScoreType& map_rate_to_correlation_score, bool use_N15, DoubleReal min_correlation_distance_to_averagine = 0.0)
  {
    DoubleReal observed_peak_fraction = getDoubleOption_("observed_peak_fraction");
 	
    cout << "Calculating " << patterns.size() << " isotope patterns with " << ADDITIONAL_ISOTOPES << " additional isotopes." << endl;
    Size n_element = use_N15 ? AASequence(seq).getFormula().getNumberOf("Nitrogen") : AASequence(seq).getFormula().getNumberOf("Carbon");
    DoubleReal TIC_threshold = use_N15 ? getDoubleOption_("pattern_15N_TIC_threshold") : getDoubleOption_("pattern_13C_TIC_threshold"); // N15 has smaller RIA resolution and multiple RIA peaks tend to overlap more in correlation. This reduces the width of the pattern leading to better distinction

    DoubleReal max_incorporation_rate = 100.0;
    DoubleReal incorporation_step = max_incorporation_rate / (DoubleReal)n_element;

    // calculate correlation with a natural averagine peptide (used to filter out coeluting peptides)
    DoubleReal peptide_weight = AASequence(seq).getMonoWeight(Residue::Full, 0);

    const Size AVERAGINE_CORR_OFFSET = 3;
    std::vector<DoubleReal> averagine_correlation(0.0, AVERAGINE_CORR_OFFSET); // doesn't make sense to correlate the unlabeled peptide with known sequence to the averagine peptide so skip the first 3 peaks
    for (Size ii = AVERAGINE_CORR_OFFSET; ii < isotopic_intensities.size() - ADDITIONAL_ISOTOPES; ++ii)
    {
      // calculate isotope distribution of averagine peptide as this will be used to detect spurious correlations with coeluting peptides
      // Note: actually it would be more accurate to use 15N-14N or 13C-12C distances. This doesn't affect averagine distribution much so this approximation is sufficient. (see TODO)
      DoubleReal current_weight = peptide_weight + ii * 1.0;  // TODO: use 13C-12C or 15N-14N instead of 1.0 as mass distance to be super accurate
      IsotopeDistribution averagine = IsotopeDistribution(20);
      averagine.estimateFromPeptideWeight(current_weight);
      std::vector<std::pair<Size, double> > averagine_intensities_pairs = averagine.getContainer();
      // add zero intensites to the left of distribution as we are doing a sliding correlation and explicitly want to correlate the region before the actual averagine peptide
      // as this often discriminates between the gaussian shape of a peptide with incorporation and a natural peptide
      std::vector<DoubleReal> averagine_intensities(0.0, AVERAGINE_CORR_OFFSET); // add 0 intensity bins
      for (Size i = 0; i != 5; ++i)
      {
        averagine_intensities.push_back(averagine_intensities_pairs[i].second);  // add intensities of actual theoretical isotope pattern
      }

      DoubleReal corr_with_averagine = Math::pearsonCorrelationCoefficient(averagine_intensities.begin(), averagine_intensities.end(), isotopic_intensities.begin() + ii - AVERAGINE_CORR_OFFSET, isotopic_intensities.begin() + ii + ADDITIONAL_ISOTOPES); 
      averagine_correlation.push_back(corr_with_averagine);
    }

    // calculate correlation of RIA peptide with measured data
    for (Size ii = 0; ii != patterns.size(); ++ii)
    {
      DoubleReal rate = (DoubleReal) ii * incorporation_step;

      vector<DoubleReal>::const_iterator pattern_begin = patterns[ii].second.begin();
      vector<DoubleReal>::const_iterator pattern_end = patterns[ii].second.end();
      vector<DoubleReal>::const_iterator intensities_begin = isotopic_intensities.begin();
      vector<DoubleReal>::const_iterator intensities_end = isotopic_intensities.end();

      filterIsotopicIntensities(pattern_begin, pattern_end, intensities_begin, intensities_end, TIC_threshold);
      Size zeros = 0;
      for (vector<DoubleReal>::const_iterator it = intensities_begin; it != intensities_end; ++it)
      {
        if (*it < 1e-8)
        {
          zeros++;
        }
      }
    
      // remove correlations with only very few peaks 
      if ((DoubleReal)zeros / (DoubleReal)std::distance(intensities_begin, intensities_end) > observed_peak_fraction)
      {
        map_rate_to_correlation_score[rate] = 0;
        continue;
      }
     
      DoubleReal correlation_score = Math::pearsonCorrelationCoefficient(pattern_begin, pattern_end, intensities_begin, intensities_end);

      // remove correlations that show higher similarity to an averagine peptide
      if (averagine_correlation[ii] > correlation_score - min_correlation_distance_to_averagine)
      {
        map_rate_to_correlation_score[rate] = 0;
        continue;
      }

      // cout << ii << "\t" << std::distance(intensities_end, intensities_begin) << "\t" << std::distance(intensities_begin, isotopic_intensities.begin()) << "\t" << std::distance(intensities_end, isotopic_intensities.begin()) << endl;
      if (boost::math::isnan(correlation_score))
      {
        correlation_score = 0.0;
      }
      map_rate_to_correlation_score[rate] = correlation_score;
    }
  }

  ///< Returns highest scoring rate and score pair in the map
  void getBestRateScorePair(const MapRateToScoreType& map_rate_to_score, DoubleReal& best_rate, DoubleReal& best_score)
  {
    best_score = -1;
    for (MapRateToScoreType::const_iterator mit = map_rate_to_score.begin(); mit != map_rate_to_score.end(); ++mit)
    {
      if (mit->second > best_score)
      {
        best_score = mit->second;
        best_rate = mit->first;
      }
    }
  }
 
  Int calculateDecompositionWeightsIsotopicPatterns(const String& seq, const vector<DoubleReal>& isotopic_intensities, const IsotopePatterns& patterns, MapRateToScoreType& map_rate_to_decomposition_weight, bool use_N15, SIPPeptide& sip_peptide)
  {
    Size n_bins = use_N15 ? AASequence(seq).getFormula().getNumberOf("Nitrogen") : AASequence(seq).getFormula().getNumberOf("Carbon");
    Matrix<DoubleReal> beta(n_bins, 1);
    Matrix<DoubleReal> intensity_vector(isotopic_intensities.size(), 1);

    for (Size p = 0; p != isotopic_intensities.size(); ++p)
    {
      intensity_vector(p, 0) =  isotopic_intensities[p];
    }

    Matrix<DoubleReal> basis_matrix(isotopic_intensities.size(), n_bins);

    for (Size row = 0; row != isotopic_intensities.size(); ++row)
    {
      for (Size col = 0; col != n_bins; ++col)
      {
        const vector<DoubleReal>& pattern = patterns[col].second;
        if (row <= n_bins)
        {
          basis_matrix(row, col) = pattern[row];
        } else
        {
          basis_matrix(row, col) = 0;
        }
      }
    }

    Int result = NonNegativeLeastSquaresSolver::solve(basis_matrix, intensity_vector, beta);

    for (Size p = 0; p != n_bins; ++p)
    {
      map_rate_to_decomposition_weight[(DoubleReal) p/n_bins * 100.0] = beta(p, 0);
    }

    // calculate R squared
    DoubleReal S_tot = 0;
    DoubleReal mean = accumulate(isotopic_intensities.begin(), isotopic_intensities.end(), 0) / isotopic_intensities.size();
    for (Size row = 0; row != isotopic_intensities.size(); ++row)
    {
      S_tot += pow(isotopic_intensities[row] - mean, 2);
    }

    DoubleReal S_err = 0;
    DoubleReal predicted_sum = 0;
    PeakSpectrum reconstructed;
    PeakSpectrum alphas;
    for (Size row = 0; row != isotopic_intensities.size(); ++row)
    {
      DoubleReal predicted = 0;
      for (Size col = 0; col != n_bins; ++col)
      {
        predicted += basis_matrix(row, col) * beta(col, 0);
      }    
      predicted_sum += predicted;
      Peak1D peak;
      peak.setIntensity(predicted);
      peak.setMZ(sip_peptide.mz_theo + sip_peptide.mass_diff / sip_peptide.charge * row);
      reconstructed.push_back(peak);
      S_err += pow(isotopic_intensities[row] - predicted, 2);
    }

    for (Size row = 0; row != 5; ++row)
    {
      DoubleReal predicted = 0;
      for (Size col = 0; col != 3; ++col)
      {
        predicted += basis_matrix(row, col) * beta(col, 0);
      }
      sip_peptide.reconstruction_monoistopic.push_back(predicted); 
    }

    sip_peptide.RR = 1.0 - (S_err / S_tot);
    sip_peptide.reconstruction = reconstructed;

    /*
    cout << "RR: " << seq << " " << 1.0 - (S_err / S_tot) << " " << S_err << " " << S_tot << endl;
    cout << "absolute TIC error: " << fabs(1.0 - predicted_sum / accumulate(isotopic_intensities.begin(), isotopic_intensities.end(), 0)) << endl;
    */
    return result;
  }


  ///> Given a peptide sequence calculate the theoretical isotopic patterns given all incorporations rate (13C Version)
  IsotopePatterns calculateIsotopePatternsFor13CRange(const AASequence& peptide)
  {
    IsotopePatterns ret;

    const Element * e1 = ElementDB::getInstance()->getElement("Carbon");
    Element * e2 = const_cast<Element *>(e1);


    EmpiricalFormula e = peptide.getFormula();
    UInt MAXISOTOPES = (UInt)e.getNumberOf("Carbon");

    // calculate isotope distribution for a given peptide and varying incoperation rates
    // modification of isotope distribution in static ElementDB
    for (DoubleReal abundance = 0.0; abundance < 100.0 - 1e-8; abundance += 100.0 / (DoubleReal)MAXISOTOPES)
    {
      DoubleReal a = abundance / 100.0;
      IsotopeDistribution isotopes;
      std::vector<std::pair<Size, double> > container;
      container.push_back(make_pair(12, 1.0 - a));
      container.push_back(make_pair(13, a));
      isotopes.set(container);
      e2->setIsotopeDistribution(isotopes);
      IsotopeDistribution dist = peptide.getFormula(Residue::Full, 0).getIsotopeDistribution(MAXISOTOPES + ADDITIONAL_ISOTOPES);
      container = dist.getContainer();
      vector<DoubleReal> intensities;
      for (Size i = 0; i != container.size(); ++i)
      {
        intensities.push_back(container[i].second);
      }
      ret.push_back(make_pair(abundance, intensities));
    }


    // reset to natural occurance
    IsotopeDistribution isotopes;
    std::vector<std::pair<Size, double> > container;
    container.push_back(make_pair(12, 0.9893));
    container.push_back(make_pair(13, 0.0107));
    isotopes.set(container);
    e2->setIsotopeDistribution(isotopes);
    return ret;
  }

  ///> Given a peptide sequence calculate the theoretical isotopic patterns given all incorporations rate (15C Version)
  IsotopePatterns calculateIsotopePatternsFor15NRange(const AASequence& peptide)
  {
    IsotopePatterns ret;

    const Element * e1 = ElementDB::getInstance()->getElement("Nitrogen");
    Element * e2 = const_cast<Element *>(e1);
    
 //   const Element * carbon = ElementDB::getInstance()->getElement("Carbon");

    EmpiricalFormula e = peptide.getFormula();
    UInt MAXISOTOPES = (UInt) e.getNumberOf("Nitrogen");

    // calculate isotope distribution for a given peptide and varying incoperation rates
    // modification of isotope distribution in static ElementDB
    for (DoubleReal abundance = 0; abundance < 100.0 - 1e-8; abundance += 100.0 / (DoubleReal) MAXISOTOPES)
    {
      DoubleReal a = abundance / 100.0;
      IsotopeDistribution isotopes;
      std::vector<std::pair<Size, double> > container;
      container.push_back(make_pair(14, 1.0 - a));
      container.push_back(make_pair(15, a));
      isotopes.set(container);
      e2->setIsotopeDistribution(isotopes);
      IsotopeDistribution dist = peptide.getFormula(Residue::Full, 0).getIsotopeDistribution(MAXISOTOPES + ADDITIONAL_ISOTOPES);
      container = dist.getContainer();
      vector<DoubleReal> intensities;
      for (Size i = 0; i != container.size(); ++i)
      {
        intensities.push_back(container[i].second);
      }
      ret.push_back(make_pair(abundance, intensities));
    }

    // reset to natural occurance
    IsotopeDistribution isotopes;
    std::vector<std::pair<Size, double> > container;
    container.push_back(make_pair(14, 0.99632));
    container.push_back(make_pair(15, 0.368));
    isotopes.set(container);
    e2->setIsotopeDistribution(isotopes);
    return ret;
  }

  PeakSpectrum extractPeakSpectrum(Size element_count, DoubleReal mass_diff, DoubleReal rt, DoubleReal feature_hit_theoretical_mz, Int feature_hit_charge, const MSExperiment<>& peak_map)
  {
    PeakSpectrum spec = *peak_map.RTBegin(rt - 1e-8);
    PeakSpectrum::ConstIterator begin_it = spec.MZBegin(feature_hit_theoretical_mz - 1e-8);
    PeakSpectrum::ConstIterator end_it = spec.MZEnd(feature_hit_theoretical_mz + element_count * mass_diff / feature_hit_charge + 1e-8);

    PeakSpectrum ret;
    for (; begin_it != end_it; ++begin_it)
    {
      if (begin_it->getIntensity() > 1e-8)
      {
        ret.push_back(*begin_it);
      }
    }
    return ret;
  }

  void saveDebugSpectrum(String filename, const PeakSpectrum& ps)
  {
    MSExperiment<> exp;
    exp.addSpectrum(ps);
    MzMLFile mtest;
    mtest.store(filename, exp);  
  }

  // collects intensities starting at seed_mz/_rt, if no peak is found at the expected position a 0 is added
  vector<DoubleReal> extractIsotopicIntensities(Size element_count, DoubleReal mass_diff, DoubleReal mz_tolerance_ppm,
                                                DoubleReal seed_rt, DoubleReal seed_mz, DoubleReal seed_charge,
                                                const MSExperiment<Peak1D>& peak_map)
  {
    vector<DoubleReal> isotopic_intensities;
    for (Size k = 0; k != element_count; ++k)
    {
      DoubleReal min_rt = seed_rt - 0.01; // feature rt
      DoubleReal max_rt = seed_rt + 0.01;
      DoubleReal mz = seed_mz + k * mass_diff / seed_charge;

      DoubleReal min_mz;
      DoubleReal max_mz;

      if (k <= 5)
      {
        DoubleReal ppm = std::max(10.0, mz_tolerance_ppm);  // restrict ppm to 10 for low intensity peaks
        min_mz = mz - mz * ppm * 1e-6;
        max_mz = mz + mz * ppm * 1e-6;
      } else
      {
        min_mz = mz - mz * mz_tolerance_ppm * 1e-6;
        max_mz = mz + mz * mz_tolerance_ppm * 1e-6;
      }

      DoubleReal found_peak_int = 0;

      MSExperiment<Peak1D>::ConstAreaIterator aait = peak_map.areaBeginConst(min_rt, max_rt, min_mz, max_mz);

      // find 13C/15N peak in window around theoretical predicted position
      vector<DoubleReal> found_peaks;
      for (; aait != peak_map.areaEndConst(); ++aait)
      {
        DoubleReal peak_int = aait->getIntensity();
        if (peak_int > 1) // we found a valid 13C/15N peak
        {
          found_peaks.push_back(peak_int);
        }
      }

      found_peak_int = std::accumulate(found_peaks.begin(), found_peaks.end(), 0);

      // assign peak intensity to first peak in small area around theoretical predicted position (should be usually only be 1)
      isotopic_intensities.push_back(found_peak_int);
    }
    return isotopic_intensities;
  }

  void writePeakIntensities_(SVOutStream& out_stream, vector<DoubleReal> isotopic_intensities, bool write_13Cpeaks)
  {    
    DoubleReal intensities_sum_12C = 0.0;
    // calculate 12C summed intensity
    for (Size k = 0; k != 5; ++k)
    {
      if (k >= isotopic_intensities.size())
      {
        break;
      }
      intensities_sum_12C += isotopic_intensities[k];
    }

    // determine 13C peaks and summed intensity
    DoubleReal intensities_sum_13C = 0;
    for (Size u = 5; u < isotopic_intensities.size(); ++u)
    {
      intensities_sum_13C += isotopic_intensities[u];
    }

    String int_string;
    // print 12C peaks
    for (Size u = 0; u != 5; ++u)
    {
      if (u == isotopic_intensities.size())
      {
        break;
      }
      int_string += String::number(isotopic_intensities[u], 0);
      int_string += " ";
    }
    int_string += ", ";

    if (write_13Cpeaks)
    {
      // print 13C peaks
      for (Size u = 5; u < isotopic_intensities.size(); ++u)
      {
        int_string += String::number(isotopic_intensities[u], 0);
        if (u < isotopic_intensities.size() - 1)
        {
          int_string += " ";
        }
      }
      out_stream << int_string;

      DoubleReal ratio = 0.0;
      if (intensities_sum_12C + intensities_sum_13C > 0.0000001)
      {
        ratio = intensities_sum_13C / (intensities_sum_12C + intensities_sum_13C);
      }
      out_stream << ratio; // << skewness(intensities_13C.begin(), intensities_13C.end());

    } else // bad correlation, no need to print intensities, ratio etc.
    {
      String int_string;

      int_string += "\t";
      int_string += "\t";
      out_stream << int_string;
    }
  }

  // scores smaller than 0 will be paddde to 0
  MapRateToScoreType normalizeToMax(const MapRateToScoreType& map_rate_to_decomposition_weight)
  {
    // extract heightest weight (best score) and rate
    DoubleReal best_rate, best_score;
    getBestRateScorePair(map_rate_to_decomposition_weight, best_rate, best_score);

    //  cout << "best rate+score: " << best_rate << " " << best_score << endl;

    // normalize weights to max(weights)=1
    MapRateToScoreType map_weights_norm(map_rate_to_decomposition_weight);
    for (MapRateToScoreType::iterator mit = map_weights_norm.begin(); mit != map_weights_norm.end(); ++mit)
    {
      if (best_score > 0)
      {
        mit->second /= best_score;
      } else
      {
        mit->second = 0;
      }
    }

    return map_weights_norm;
  }

  // Extract the mono-isotopic trace and reports the rt of the maximum intensity
  // Used to compensate for slight RT shifts (e.g. important if features of a different map are used)
  // n_scans corresponds to the number of neighboring scan rts that should be extracted
  // n_scan = 2 -> vector size = 1 + 2 + 2
  vector<DoubleReal> findApexRT(const FeatureMap<>::iterator feature_it, DoubleReal hit_rt, const MSExperiment<Peak1D>& peak_map, Size n_scans)
  {
    vector<DoubleReal> seeds_rt;
    // extract elution profile of 12C containing mass trace using a bounding box
    // first convex hull contains the monoisotopic 12C trace
    const DBoundingBox<2>& mono_bb = feature_it->getConvexHulls()[0].getBoundingBox();
    vector<Peak2D> mono_trace;

    //(min_rt, max_rt, min_mz, max_mz)
    MSExperiment<Peak1D>::ConstAreaIterator ait = peak_map.areaBeginConst(mono_bb.minPosition()[0], mono_bb.maxPosition()[0], mono_bb.minPosition()[1], mono_bb.maxPosition()[1]);
    for (; ait != peak_map.areaEndConst(); ++ait)
    {
      Peak2D p2d;
      p2d.setRT(ait.getRT());  // get rt of scan
      p2d.setMZ(ait->getMZ());  // get peak 1D mz
      p2d.setIntensity(ait->getIntensity());
      mono_trace.push_back(p2d);
    }

    // if there is no 12C mono trace generate a valid starting point
    if (mono_trace.empty())
    {
      Peak2D p2d;
      DoubleReal next_valid_scan_rt = peak_map.RTBegin(hit_rt - 0.001)->getRT();
      p2d.setRT(next_valid_scan_rt);
      p2d.setMZ(0);  // actually not needed
      p2d.setIntensity(0);
      mono_trace.push_back(p2d);
    }

    // determine trace peak with highest intensity
    DoubleReal max_trace_int = -1;
    DoubleReal max_trace_int_idx = 0;

    for (Size j = 0; j != mono_trace.size(); ++j)
    {
      if (mono_trace[j].getIntensity() > max_trace_int)
      {
        max_trace_int = mono_trace[j].getIntensity();
        max_trace_int_idx = j;
      }
    }
    DoubleReal max_trace_int_rt = mono_trace[max_trace_int_idx].getRT();
    seeds_rt.push_back(max_trace_int_rt);

    for (Size i = 1; i <= n_scans; ++i)
    {
      DoubleReal rt_after = max_trace_int_rt;
      if (max_trace_int_idx < mono_trace.size() - i)
      {
        rt_after = mono_trace[max_trace_int_idx + i].getRT();
      }

      DoubleReal rt_before = max_trace_int_rt;
      if (max_trace_int_idx >= i)
      {
        rt_before = mono_trace[max_trace_int_idx - i].getRT();
      }

      if (fabs(max_trace_int_rt - rt_after) < 10.0) 
      {
        seeds_rt.push_back(rt_after);
      }
      
      if (fabs(max_trace_int_rt - rt_before) < 10.0)
      {
        seeds_rt.push_back(rt_before);
      }           
    }
    //cout << "Seeds size:" << seeds_rt.size() << endl;
    return seeds_rt;
  }

  PeakSpectrum mergeSpectra(const MSExperiment<>& to_merge) 
  {
    PeakSpectrum merged;
    for (Size i = 0; i != to_merge.size(); ++i)
    {
      std::copy(to_merge[i].begin(), to_merge[i].end(), std::back_inserter(merged));
    }
    merged.sortByPosition();

    return merged;
  }

  ///> converts a vector of isotopic intensities to a peak spectrum starting at mz=mz_start with mass_diff/charge step size
  PeakSpectrum isotopicIntensitiesToSpectrum(DoubleReal mz_start, DoubleReal mass_diff, Int charge, vector<DoubleReal> isotopic_intensities)
  {
    PeakSpectrum ps;
    for (Size i = 0; i != isotopic_intensities.size(); ++i)
    {
      Peak1D peak;
      peak.setMZ(mz_start + i * mass_diff / (DoubleReal)charge);
      peak.setIntensity(isotopic_intensities[i]);
      ps.push_back(peak);
    }
    return ps;
  }

  ///> Collect decomposition coefficients in the merge window around the correlation maximum.
  ///> Final list of RIAs is constructed for the peptide.
  void extractIncorporationsAtCorrelationMaxima(SIPPeptide& sip_peptide,
                                                 const IsotopePatterns& patterns,
                                                 DoubleReal weight_merge_window = 5.0,
                                                 DoubleReal min_corr_threshold = 0.5, 
                                                 DoubleReal min_decomposition_weight = 10.0)
{
    const MapRateToScoreType& map_rate_to_decomposition_weight = sip_peptide.decomposition_map;
    const MapRateToScoreType& map_rate_to_correlation_score = sip_peptide.correlation_map;  
    vector<SIPIncorporation> sip_incorporations;
    const vector<RateScorePair>&  corr_maxima = sip_peptide.correlation_maxima;
   
    DoubleReal explained_TIC_fraction = 0;
    DoubleReal TIC = 0;
    Size non_zero_decomposition_coefficients = 0;
       
    DoubleReal max_corr_TIC = 0;

    for (Size k = 0; k < corr_maxima.size(); ++k)
    {
      const DoubleReal rate = corr_maxima[k].rate;
      const DoubleReal corr = corr_maxima[k].score;
    
      if (corr > min_corr_threshold)
      {
          SIPIncorporation sip_incorporation;
          sip_incorporation.rate = rate;

          // sum up decomposition intensities for quantification in merge window
          DoubleReal int_sum = 0;
          MapRateToScoreType::const_iterator low = map_rate_to_decomposition_weight.lower_bound(rate - weight_merge_window - 1e-4);
          MapRateToScoreType::const_iterator high = map_rate_to_decomposition_weight.lower_bound(rate + weight_merge_window + 1e-4);                    
          for (;low != high; ++low)
          {
            int_sum += low->second;
          }

          if (low != map_rate_to_decomposition_weight.end())
          {
            int_sum += low->second;
          }

          sip_incorporation.abundance = int_sum; // calculate abundance as sum of all decompositions
          sip_incorporation.correlation = min(corr, 1.0); 
          
          max_corr_TIC += int_sum;

          PeakSpectrum theoretical_spectrum;        

          // find closest idx (could be more efficient using binary search)
          Size closest_idx = 0;
          for (Size i = 0; i != patterns.size(); ++i)
          {
            if ( fabs(patterns[i].first - rate) < fabs(patterns[closest_idx].first - rate))
            {
              closest_idx = i;
            }
          }
          sip_incorporation.theoretical = isotopicIntensitiesToSpectrum(sip_peptide.mz_theo, sip_peptide.mass_diff, sip_peptide.charge, patterns[closest_idx].second);
          
          sip_incorporations.push_back(sip_incorporation);
      }
    }

    // find highest non-natural incorporation
    DoubleReal highest_non_natural_abundance = 0;
    DoubleReal highest_non_natural_rate = 0;
    for (vector<SIPIncorporation>::const_iterator it = sip_incorporations.begin(); it != sip_incorporations.end(); ++it)
    {
      if (it->rate < 5.0) // skip natural
      {
        continue;
      }

      if (it->abundance > highest_non_natural_abundance)
      {
        highest_non_natural_rate = it->rate;
        highest_non_natural_abundance = it->abundance;
      }
    }

    bool non_natural;
    if (highest_non_natural_rate > 5.0)
    {
      non_natural = true;
    }


    // used for non-gaussian shape detection
    for (MapRateToScoreType::const_iterator mit = map_rate_to_decomposition_weight.begin(); mit != map_rate_to_decomposition_weight.end(); ++mit)
    {
      DoubleReal decomposition_rate = mit->first;
      DoubleReal decomposition_weight = mit->second;
      TIC += decomposition_weight;

      if (non_natural && decomposition_weight > 0.05 * highest_non_natural_abundance && decomposition_rate > 5.0)
      {
        ++non_zero_decomposition_coefficients;
      }
    }

    if (TIC > 1e-5)
    {
      explained_TIC_fraction = max_corr_TIC / TIC;
    } else
    {
      explained_TIC_fraction = 0;
    }

    // set results
    sip_peptide.incorporations = sip_incorporations;
    sip_peptide.explained_TIC_fraction = explained_TIC_fraction;
    sip_peptide.non_zero_decomposition_coefficients = non_zero_decomposition_coefficients;
}

  ///> Collect decomposition coefficients. Starting at the largest decomposition weights merge smaller weights in the merge window.
  void extractIncorporationsAtHeighestDecompositionWeights(SIPPeptide& sip_peptide,
                             const IsotopePatterns& patterns,
                             DoubleReal weight_merge_window = 5.0,
                             DoubleReal min_corr_threshold = 0.5,
                             DoubleReal min_low_RIA_threshold = -1,
                             DoubleReal min_decomposition_weight = 10.0)
{
  if (min_low_RIA_threshold < 0)
  {
    min_low_RIA_threshold = min_corr_threshold;
  }

  const MapRateToScoreType& map_rate_to_decomposition_weight = sip_peptide.decomposition_map;
  const MapRateToScoreType& map_rate_to_correlation_score = sip_peptide.correlation_map;  

  DoubleReal explained_TIC_fraction = 0;
  DoubleReal TIC = 0;
  Size non_zero_decomposition_coefficients = 0;       
  DoubleReal max_corr_TIC = 0;
  vector<SIPIncorporation> sip_incorporations;

  // find decomposition weights with correlation larger than threshold (seeds)
  MapRateToScoreType::const_iterator md_it = map_rate_to_decomposition_weight.begin();
  MapRateToScoreType::const_iterator mc_it = map_rate_to_correlation_score.begin();  

  set< pair<DoubleReal, DoubleReal> > seeds_weight_rate_pair;
  for (; md_it != map_rate_to_decomposition_weight.end(); ++md_it, ++mc_it)
  {
    if (mc_it->first < 10.0) // lowRIA region
    {
      if (mc_it->second >= min_low_RIA_threshold && md_it->second >= min_decomposition_weight)
      {
        seeds_weight_rate_pair.insert(make_pair(md_it->second, md_it->first));      
      } 
    } else  // non-low RIA region
    {
      if (mc_it->second >= min_corr_threshold && md_it->second >= min_decomposition_weight)
      {
        seeds_weight_rate_pair.insert(make_pair(md_it->second, md_it->first));
        //cout << "Seeds insert: " << md_it->second << " " << md_it->first << endl;
      }
    }
  }

  // cout << "Seeds: " << seeds_weight_rate_pair.size() << endl;
  
  // seeds_weight_rate_pair contains the seeds ordered by their decomposition weight
  while (!seeds_weight_rate_pair.empty())
  {
    // pop last element from set
    set< pair<DoubleReal, DoubleReal> >::iterator last_element = --seeds_weight_rate_pair.end();
    pair<DoubleReal, DoubleReal> current_seed = *last_element;
    
    //cout << current_seed.first << " " << current_seed.second << endl;

    // find weights in window to merge, remove from seed map. maybe also remove from original map depending on whether we want to quantify the weight only 1 time
    const DoubleReal weight = current_seed.first;
    const DoubleReal rate = current_seed.second;

    SIPIncorporation sip_incorporation;
    sip_incorporation.rate = rate;
    
    MapRateToScoreType::const_iterator low = map_rate_to_decomposition_weight.lower_bound(rate - weight_merge_window - 1e-4);
    MapRateToScoreType::const_iterator high = map_rate_to_decomposition_weight.lower_bound(rate + weight_merge_window + 1e-4);
    
    // cout << "Distance: " << std::distance(low, high) << endl;;

    MapRateToScoreType::const_iterator l1 = low;
    MapRateToScoreType::const_iterator h1 = high;

    // iterate over peaks in merge window
    for (; l1 != h1; ++l1)
    {
      // remove from seed map      
      Int erased = seeds_weight_rate_pair.erase(make_pair(l1->second, l1->first));
      //cout << "erasing: " << l1->second << " " << l1->first << " " << erased << endl;
    }

    // Sum up decomposition intensities for quantification in merge window
    DoubleReal int_sum = 0;
    for (;low != high; ++low)
    {
      int_sum += low->second;
    }

    if (low != map_rate_to_decomposition_weight.end())
    {
      int_sum += low->second;
    }

    sip_incorporation.abundance = int_sum; 
    MapRateToScoreType::const_iterator corr_it = map_rate_to_correlation_score.lower_bound(rate - 1e-6);
    sip_incorporation.correlation = min(corr_it->second, 1.0); 
          
    max_corr_TIC += int_sum;

    PeakSpectrum theoretical_spectrum;        

    // find closest idx (could be more efficient using binary search)
    Size closest_idx = 0;
    for (Size i = 0; i != patterns.size(); ++i)
    {
      if ( fabs(patterns[i].first - rate) < fabs(patterns[closest_idx].first - rate))
      {
        closest_idx = i;
      }
    }
    sip_incorporation.theoretical = isotopicIntensitiesToSpectrum(sip_peptide.mz_theo, sip_peptide.mass_diff, sip_peptide.charge, patterns[closest_idx].second);
          
    sip_incorporations.push_back(sip_incorporation);
  }

  // find highest non-natural incorporation
  DoubleReal highest_non_natural_abundance = 0;
  DoubleReal highest_non_natural_rate = 0;
  for (vector<SIPIncorporation>::const_iterator it = sip_incorporations.begin(); it != sip_incorporations.end(); ++it)
  {
    if (it->rate < 5.0) // skip natural
    {
      continue;
    }

    if (it->abundance > highest_non_natural_abundance)
    {
      highest_non_natural_rate = it->rate;
      highest_non_natural_abundance = it->abundance;
    }
  }

  bool non_natural;
  if (highest_non_natural_rate > 5.0)
  {
    non_natural = true;
  }

  // used for non-gaussian shape detection
  for (MapRateToScoreType::const_iterator mit = map_rate_to_decomposition_weight.begin(); mit != map_rate_to_decomposition_weight.end(); ++mit)
  {
    DoubleReal decomposition_rate = mit->first;
    DoubleReal decomposition_weight = mit->second;
    TIC += decomposition_weight;

    if (non_natural && decomposition_weight > 0.05 * highest_non_natural_abundance && decomposition_rate > 5.0)
    {
      ++non_zero_decomposition_coefficients;
    }
  }

  if (TIC > 1e-5)
  {
    explained_TIC_fraction = max_corr_TIC / TIC;
  } else
  {
    explained_TIC_fraction = 0;
  }

  // set results
  std::sort(sip_incorporations.begin(), sip_incorporations.end(), RIALess());
  sip_peptide.incorporations = sip_incorporations;
  sip_peptide.explained_TIC_fraction = explained_TIC_fraction;
  sip_peptide.non_zero_decomposition_coefficients = non_zero_decomposition_coefficients;
}

  ///> calculate the global labeling ration based on all but the first 4 peaks
  DoubleReal calculateGlobalLR(const vector<DoubleReal>& isotopic_intensities)
  {
    if (isotopic_intensities.size() < 5)
    {
      return 0.0;
    }

    DoubleReal sum = accumulate(isotopic_intensities.begin(), isotopic_intensities.end(), 0);    
    DoubleReal sum_incorporated = accumulate(isotopic_intensities.begin() + 4, isotopic_intensities.end(), 0);

    if (sum < 1e-4)
    {
      return 0.0;
    }

    return sum_incorporated / sum;
  }

  void plotHeatMap_(const String& output_dir, const String& tmp_path, const String& file_suffix, const vector<vector<DoubleReal> >& binned_ria, vector<String> class_labels, Size debug_level = 0)
  { 
    String plot_extension = getStringOption_("plot_extension");  
    String filename = String("heatmap") + file_suffix + "." + plot_extension;
    String script_filename = String("heatmap") + file_suffix + String(".R");
    
    TextFile current_script;
    StringList ria_list, col_labels;

    for (Size i = 0; i != binned_ria[0].size(); ++i)
    {
      col_labels << String(i*(100 / binned_ria[0].size())) + "%-" + String((i+1)*(100 / binned_ria[0].size())) + "%";
    }

    for (vector<vector<DoubleReal> >::const_iterator pit = binned_ria.begin(); pit != binned_ria.end(); ++pit)
    {
      for (vector<DoubleReal>::const_iterator rit = pit->begin(); rit != pit->end(); ++rit)
      {
        ria_list << String(*rit);
      }
    }

    // row labels
    StringList row_labels;
    if (!class_labels.empty())
    {
      for (Size i = 0; i != class_labels.size(); ++i)
      {
        row_labels << class_labels[i];
      }      
    }

    // plot heatmap
    current_script.push_back("library(gplots)");
    String ria_list_string;
    ria_list_string.concatenate(ria_list.begin(), ria_list.end(), ",");
    current_script.push_back("mdat <- matrix(c(" + ria_list_string + "), ncol=" + String(binned_ria[0].size()) + ", byrow=TRUE)");

    if (plot_extension == "png")
    {
      current_script.push_back("png('" + tmp_path + "/" + filename + "', width=1000, height=" + String(10*binned_ria.size()) + ")");
    } else if (plot_extension == "svg")
    {
      current_script.push_back("svg('" + tmp_path + "/" + filename + "', width=8, height=4.5)");
    } else if (plot_extension == "pdf")
    {
      current_script.push_back("pdf('" + tmp_path + "/" + filename + "', width=8, height=4.5)");
    }

    String labRowString; 
    if (row_labels.empty())
    {
      labRowString = "FALSE";
    } else
    {
      String row_labels_string;
      row_labels_string.concatenate(row_labels.begin(), row_labels.end(), "\",\"");
      labRowString = String("c(\"") + row_labels_string + "\")";
    }

    String col_labels_string;
    col_labels_string.concatenate(col_labels.begin(), col_labels.end(), "\",\"");

    current_script.push_back("heatmap.2(mdat, dendrogram=\"none\", col=colorRampPalette(c(\"black\",\"red\")), Rowv=FALSE, Colv=FALSE, key=FALSE, labRow=" + labRowString + ",labCol=c(\"" + col_labels_string + "\"),trace=\"none\", density.info=\"none\")");

    current_script.push_back("tmp<-dev.off()");
    current_script.store(tmp_path + "/" + script_filename);
    QStringList qparam;
    qparam << "--vanilla" << "--quiet" << "--slave" << "--file=" + QString(tmp_path.toQString() + "\\" + script_filename.toQString());
    Int status = QProcess::execute("R", qparam);

    // cleanup
    if (status != 0)
    {
      std::cerr << "Error: Process returned with non 0 status." << std::endl;
    } else
    {
      QFile(QString(tmp_path.toQString() + "\\" + filename.toQString())).copy(output_dir.toQString() + "/heatmap" + file_suffix.toQString() + "." + plot_extension.toQString());
      if (debug_level < 1)
      {          
        QFile(QString(tmp_path.toQString() + "\\" + script_filename.toQString())).remove();
        QFile(QString(tmp_path.toQString() + "\\" + filename.toQString())).remove();
      }
    }
  }

  void plotFilteredSpectra_(const String& output_dir, const String& tmp_path, const String& file_suffix, const vector<SIPPeptide>& sip_peptides, Size debug_level = 0)
  { 
    String plot_extension = getStringOption_("plot_extension");
   
    String filename = String("spectrum_plot") + file_suffix + "." + plot_extension;
    String script_filename = String("spectrum_plot") + file_suffix + String(".R");

    for (Size i = 0; i != sip_peptides.size(); ++i)
    {     
      TextFile current_script;
      StringList mz_list;
      StringList intensity_list;

      for (Size j = 0; j != sip_peptides[i].accumulated.size(); ++j)
      {
        const Peak1D& peak = sip_peptides[i].accumulated[j];               
        mz_list << String(peak.getMZ());
        intensity_list << String(peak.getIntensity());
      }
      
      String mz_list_string;
      mz_list_string.concatenate(mz_list.begin(), mz_list.end(), ",");

      String intensity_list_string;
      intensity_list_string.concatenate(intensity_list.begin(), intensity_list.end(), ",");

      current_script.push_back("mz<-c(" + mz_list_string + ")");
      current_script.push_back("int<-c(" + intensity_list_string + ")");
      current_script.push_back("x0=mz; x1=mz; y0=rep(0, length(x0)); y1=int");

      if (plot_extension == "png")
      {
        current_script.push_back("png('" + tmp_path + "/" + filename + "')");
      } else  if (plot_extension == "svg")
      {
        current_script.push_back("svg('" + tmp_path + "/" + filename + "', width=8, height=4.5)");
      } else if (plot_extension == "pdf")
      {
        current_script.push_back("pdf('" + tmp_path + "/" + filename + "', width=8, height=4.5)");
      }

      current_script.push_back("plot.new()");
      current_script.push_back("plot.window(xlim=c(min(mz),max(mz)), ylim=c(0,max(int)))");
      current_script.push_back("axis(1); axis(2)");
      current_script.push_back("title(xlab=\"m/z\")");
      current_script.push_back("title(ylab=\"intensity\")");
      current_script.push_back("box()");
      current_script.push_back("segments(x0,y0,x1,y1)");
      current_script.push_back("tmp<-dev.off()");
      current_script.store(tmp_path + "/" + script_filename);
      QStringList qparam;
      qparam << "--vanilla" << "--quiet" << "--slave" << "--file=" + QString(tmp_path.toQString() + "\\" + script_filename.toQString());
      Int status = QProcess::execute("R", qparam);
      if (status != 0)
      {
        std::cerr << "Error: Process returned with non 0 status." << std::endl;
      } else
      {
        QFile(QString(tmp_path.toQString() + "\\" + filename.toQString())).copy(output_dir.toQString() + "/spectrum" + file_suffix.toQString() + "_rt_" + String(sip_peptides[i].feature_rt).toQString() + "." + plot_extension.toQString());
        if (debug_level < 1)
        {          
          QFile(QString(tmp_path.toQString() + "\\" + script_filename.toQString())).remove();
          QFile(QString(tmp_path.toQString() + "\\" + filename.toQString())).remove();
        }
      }
    }
  }

  void plotMergedSpectra_(const String& output_dir, const String& tmp_path, const String& file_suffix, const vector<SIPPeptide>& sip_peptides, Size debug_level = 0)
  { 
    String plot_extension = getStringOption_("plot_extension");

    String filename = String("merged_spectra") + file_suffix + "." + plot_extension;
    String script_filename = String("merged_spectra") + file_suffix + String(".R");

    for (Size i = 0; i != sip_peptides.size(); ++i)
    {     
      TextFile current_script;
      StringList mz_list;
      StringList intensity_list;

      for (Size j = 0; j != sip_peptides[i].merged.size(); ++j)
      {
        const Peak1D& peak = sip_peptides[i].merged[j];               
        mz_list << String(peak.getMZ());
        intensity_list << String(peak.getIntensity());
      }

      String mz_list_string;
      mz_list_string.concatenate(mz_list.begin(), mz_list.end(), ",");

      String intensity_list_string;
      intensity_list_string.concatenate(intensity_list.begin(), intensity_list.end(), ",");

      current_script.push_back("mz<-c(" + mz_list_string + ")");
      current_script.push_back("int<-c(" + intensity_list_string + ")");
      current_script.push_back("x0=mz; x1=mz; y0=rep(0, length(x0)); y1=int");

      if (plot_extension == "png")
      {
        current_script.push_back("png('" + tmp_path + "/" + filename + "')");
      } else if (plot_extension == "svg")
      {
        current_script.push_back("svg('" + tmp_path + "/" + filename + "', width=8, height=4.5)");
      } else if (plot_extension == "pdf")
      {
        current_script.push_back("pdf('" + tmp_path + "/" + filename + "', width=8, height=4.5)");
      }

      current_script.push_back("plot.new()");
      current_script.push_back("plot.window(xlim=c(min(mz),max(mz)), ylim=c(0,max(int)))");
      current_script.push_back("axis(1); axis(2)");
      current_script.push_back("title(xlab=\"m/z\")");
      current_script.push_back("title(ylab=\"intensity\")");
      current_script.push_back("box()");
      current_script.push_back("segments(x0,y0,x1,y1)");
      current_script.push_back("tmp<-dev.off()");
      current_script.store(tmp_path + "/" + script_filename);
      QStringList qparam;
      qparam << "--vanilla" << "--quiet" << "--slave" << "--file=" + QString(tmp_path.toQString() + "\\" + script_filename.toQString());
      Int status = QProcess::execute("R", qparam);
      if (status != 0)
      {
        std::cerr << "Error: Process returned with non 0 status." << std::endl;
      } else
      {
        QFile(QString(tmp_path.toQString() + "\\" + filename.toQString())).copy(output_dir.toQString() + "/merged_spectra" + file_suffix.toQString() + "_rt_" + String(sip_peptides[i].feature_rt).toQString() + "." + plot_extension.toQString());
        if (debug_level < 1)
        {          
          QFile(QString(tmp_path.toQString() + "\\" + script_filename.toQString())).remove();
          QFile(QString(tmp_path.toQString() + "\\" + filename.toQString())).remove();
        }
      }
    }
  }

   void writeHTML_(const String& qc_output_directory, const String& file_suffix, const vector<SIPPeptide>& sip_peptides)
   {
     TextFile current_script;

     // html header
     current_script.push_back("<!DOCTYPE html>\n<html>\n<body>\n");

     // peptide heat map plot
     current_script.push_back(String("<h1>") + "peptide heat map</h1>");
     String peptide_heatmap_plot_filename = String("heatmap_peptide") + file_suffix + String(".") + getStringOption_("plot_extension");
     current_script.push_back("<p> <img src=\"" + peptide_heatmap_plot_filename + "\" alt=\"graphic\"></p>");

     // cluster plot
     if (cluster_type_ == "profile")
     {
       String cluster_plot_filename = String("cluster_plot") + file_suffix + String(".") + getStringOption_("plot_extension");
       current_script.push_back("<p> <img src=\"" + cluster_plot_filename + "\" alt=\"graphic\"></p>");
     }

     for (Size i = 0; i != sip_peptides.size(); ++i)
     {
       // heading
       current_script.push_back(String("<h1>") + "RT: " + String(sip_peptides[i].feature_rt) + "</h1>");

       current_script.push_back("<table border=\"1\">");
       // sequence table row
       current_script.push_back("<tr>");
       current_script.push_back("<td>sequence</td>");
       current_script.push_back(String("<td>") + sip_peptides[i].sequence.toString() + "</td>");
       current_script.push_back("</tr>");

       current_script.push_back("<tr>");
       current_script.push_back("<td>rt (min.)</td>");
       current_script.push_back(String("<td>" + String::number(sip_peptides[i].feature_rt / 60.0, 2) + "</td>"));
       current_script.push_back("</tr>");

       current_script.push_back("<tr>");
       current_script.push_back("<td>rt (sec.)</td>");
       current_script.push_back(String("<td>" + String::number(sip_peptides[i].feature_rt, 2) + "</td>"));
       current_script.push_back("</tr>");

       current_script.push_back("<tr>");
       current_script.push_back("<td>mz</td>");
       current_script.push_back(String("<td>" + String::number(sip_peptides[i].feature_mz, 4) + "</td>"));
       current_script.push_back("</tr>");

       current_script.push_back("<tr>");
       current_script.push_back("<td>theo. mz</td>");
       current_script.push_back(String("<td>" + String::number(sip_peptides[i].mz_theo, 4) + "</td>"));
       current_script.push_back("</tr>");

       current_script.push_back("<tr>");
       current_script.push_back("<td>charge</td>");
       current_script.push_back(String("<td>" + String(sip_peptides[i].charge) + "</td>"));
       current_script.push_back("</tr>");

       if (!sip_peptides[i].accessions.empty())
       {
         current_script.push_back(String("<tr>"));
         current_script.push_back("<td>accessions</td>");
         current_script.push_back(String("<td>" + sip_peptides[i].accessions[0] + "</td>"));
         current_script.push_back(String("</tr>"));

         current_script.push_back(String("<tr>"));
         current_script.push_back("<td>unique</td>");
         current_script.push_back(String("<td>" + String(sip_peptides[i].unique) + "</td>"));
         current_script.push_back(String("</tr>"));
       }

       current_script.push_back(String("<tr>"));
       current_script.push_back("<td>search score</td>");
       current_script.push_back(String("<td>") + String(sip_peptides[i].score) + "</td>");
       current_script.push_back("</tr>");

       current_script.push_back("<tr>");
       current_script.push_back("<td>global labeling ratio</td>");
       current_script.push_back(String("<td>") + String::number(sip_peptides[i].global_LR, 2) + "</td>");
       current_script.push_back("</tr>");

       current_script.push_back("<tr>");
       current_script.push_back("<td>R squared</td>");
       current_script.push_back(String("<td>") + String::number(sip_peptides[i].RR, 2) + "</td>");
       current_script.push_back("</tr>");

       current_script.push_back("</table>");
      
       // table header of incorporations
       current_script.push_back("<p>");
       current_script.push_back("<table border=\"1\">");
       current_script.push_back("<tr>");
       for (Size k = 0; k != sip_peptides[i].incorporations.size(); ++k)
       {
         current_script.push_back(String("<td>RIA") + String(k + 1) + "</td>");
         current_script.push_back(String("<td>CORR.") + String(k + 1) + "</td>");
         current_script.push_back(String("<td>INT") + String(k + 1) + "</td>");       
       }
       current_script.push_back("</tr>");

       // table of incorporations
       current_script.push_back("<tr>");
       for (Size k = 0; k != sip_peptides[i].incorporations.size(); ++k)
       {
         SIPIncorporation p = sip_peptides[i].incorporations[k];
         current_script.push_back(String("<td>") + String::number(p.rate, 2) + "</td>");
         current_script.push_back(String("<td>") + String::number(p.correlation, 2) + "</td>");
         current_script.push_back(String("<td>") + String::number(p.abundance, 0) + "</td>");  
       }
       current_script.push_back("</tr>");

       current_script.push_back("</table>");

       // spectrum plot
       String spectrum_filename = String("spectrum") + file_suffix + "_rt_" + String(sip_peptides[i].feature_rt) + "." + getStringOption_("plot_extension");
       current_script.push_back("<p> <img src=\"" + spectrum_filename + "\" alt=\"graphic\"></p>");

       // score plot
       String score_filename = String("scores") + file_suffix + "_rt_" + String(sip_peptides[i].feature_rt) + "." + getStringOption_("plot_extension");
       current_script.push_back("<p> <img src=\"" + score_filename + "\" alt=\"graphic\"></p>");
     }
     current_script.push_back("\n</body>\n</html>");
     current_script.store(qc_output_directory.toQString() + "/index" + file_suffix.toQString() + ".html");
   }

  void plotScoresAndWeights_(const String& output_dir, const String& tmp_path, const String& file_suffix, const vector<SIPPeptide>& sip_peptides, Size debug_level = 0)
  {
    String plot_extension = getStringOption_("plot_extension");

    String score_filename = String("score_plot") + file_suffix + plot_extension;
    String script_filename = String("score_plot") + file_suffix + String(".R");

    DoubleReal score_plot_yaxis_min = getDoubleOption_("score_plot_yaxis_min");

    for (Size i = 0; i != sip_peptides.size(); ++i)
    {     
      TextFile current_script;
      StringList rate_dec_list;
      StringList rate_corr_list;
      StringList weights_list;
      StringList corr_list;

      MapRateToScoreType decomposition_map; // all rate to decomposition scores for the peptide
      MapRateToScoreType correlation_map; // all rate to correlation scores for the peptide

      for (MapRateToScoreType::const_iterator mit = sip_peptides[i].decomposition_map.begin(); mit != sip_peptides[i].decomposition_map.end(); ++mit)
      {
        rate_dec_list << String(mit->first);
        weights_list << String(mit->second);
      }

      for (MapRateToScoreType::const_iterator mit = sip_peptides[i].correlation_map.begin(); mit != sip_peptides[i].correlation_map.end(); ++mit)
      {
        rate_corr_list << String(mit->first);
        corr_list << String(mit->second);
      }

      String rate_dec_list_string;
      rate_dec_list_string.concatenate(rate_dec_list.begin(), rate_dec_list.end(), ",");

      String weights_list_string;
      weights_list_string.concatenate(weights_list.begin(), weights_list.end(), ",");

      String rate_corr_list_string;
      rate_corr_list_string.concatenate(rate_corr_list.begin(), rate_corr_list.end(), ",");

      String corr_list_string;
      corr_list_string.concatenate(corr_list.begin(), corr_list.end(), ",");

      current_script.push_back("rate_dec<-c(" + rate_dec_list_string + ")");
      current_script.push_back("dec<-c(" + weights_list_string + ")");
      current_script.push_back("if (max(dec)!=0) {dec<-dec/max(dec)}");
      current_script.push_back("rate_corr<-c(" + rate_corr_list_string + ")");
      current_script.push_back("corr<-c(" + corr_list_string + ")");

      if (score_plot_yaxis_min >= 0)
      {
        current_script.push_back("corr[corr<0]=0"); // truncate at 0 for better drawing
      }

      current_script.push_back("x0=rate_dec; x1=rate_dec; y0=rep(0, length(x0)); y1=dec");  // create R segments for decomposition score (vertical bars)
      if (plot_extension == "png")
      {
        current_script.push_back("png('" + tmp_path + "/" + score_filename + "')");
      } else if (plot_extension == "svg")
      {
        current_script.push_back("svg('" + tmp_path + "/" + score_filename + "', width=8, height=4.5)");
      } else if (plot_extension == "pdf")
      {
        current_script.push_back("pdf('" + tmp_path + "/" + score_filename + "', width=8, height=4.5)");
      }
      current_script.push_back("plot.new()");
      current_script.push_back("plot.window(xlim=c(0,100), ylim=c(" + String(score_plot_yaxis_min) + ",1))");
      current_script.push_back("axis(1); axis(2)");
      current_script.push_back("title(xlab=\"RIA\")");
      current_script.push_back("title(ylab=\"normalized weight / corr.\")");
      current_script.push_back("box()");
      current_script.push_back("segments(x0,y0,x1,y1, col='red')");
      current_script.push_back("lines(x=rate_corr, y=corr, col='blue')");
      current_script.push_back("legend('bottomright', horiz=FALSE, xpd=TRUE, col=c('red', 'blue'), lwd=2, c('weights', 'correlation'))");
      current_script.push_back("tmp<-dev.off()");
      current_script.store(tmp_path + "/" + script_filename);
      QStringList qparam;
      qparam << "--vanilla" << "--quiet" << "--slave" << "--file=" + QString(tmp_path.toQString() + "\\" + script_filename.toQString());
      Int status = QProcess::execute("R", qparam);
      if (status != 0)
      {
        std::cerr << "Error: Process returned with non 0 status." << std::endl;
      } else
      {
        QFile(QString(tmp_path.toQString() + "\\" + score_filename.toQString())).copy(output_dir.toQString() + "/scores" + file_suffix.toQString() + "_rt_" + String(sip_peptides[i].feature_rt).toQString() + "." + plot_extension.toQString());
        if (debug_level < 1)
        {          
          QFile(QString(tmp_path.toQString() + "\\" + script_filename.toQString())).remove();
          QFile(QString(tmp_path.toQString() + "\\" + score_filename.toQString())).remove();
        }
      }
    }
  }

  /*
  void emdClusterData_(String tmp_path, String data_filename, String file_suffix, String qc_output_directory, Int debug_level)
  {
    String script_filename = String("cluster_data") + file_suffix + String(".R");
    String result_filename = String("cluster_result") + file_suffix + String(".dat");

    // add scripts
    TextFile current_script;

    current_script.push_back("library(clValid)");   
    current_script.push_back("d=read.table('" + tmp_path + "/" + data_filename + "', sep='\\t')");
    current_script.push_back("m=as.matrix(d[,-(1:2)])");
    current_script.push_back("m=t(m)");
    current_script.push_back("clusters=rep(1,nrow(t(m)))");

    current_script.push_back("if (nrow(t(m))>=20)");
    current_script.push_back("{");
    current_script.push_back("max_clust<-max(3,nrow(t(m))%/%5)");  // on average there should be at least 5 spectra per group and we want to check at least 2 to 3 cluster
    current_script.push_back("max_clust<-min(max_clust, 10)");  // at most 10 cluster      
    current_script.push_back("result<-clValid(t(m), nClust=2:max_clust, clMethods=c('pam'), validation='internal', maxitem=nrow(t(m)), diss=TRUE)");
    current_script.push_back("k=as.numeric(as.matrix(optimalScores(result)[,\"Clusters\"]))");
    current_script.push_back("k=median(k)");
    current_script.push_back("cl=pam(x=t(m), k=k, diss=TRUE)");
    current_script.push_back("clusters=cl$cluster");
    current_script.push_back("}");
    current_script.push_back("r=cbind(clusters, d[,1])");
    current_script.push_back("r=r[!is.na(r[,1]),]");
    current_script.push_back("write.table(r, '" + tmp_path + "/" + result_filename + "', col.names=FALSE, row.names=FALSE, sep='\t')");

    current_script.store(tmp_path + "/" + script_filename);

    QStringList qparam;
    qparam << "--vanilla" << "--quiet" << "--slave" << "--file=" + QString(tmp_path.toQString() + "\\" + script_filename.toQString());
    Int status = QProcess::execute("R", qparam);
    if (status != 0)
    {
      std::cerr << "Error: Process returned with non 0 status." << std::endl;
    } else
    {              
      if (debug_level < 1)
      {          
        QFile(QString(tmp_path.toQString() + "\\" + data_filename.toQString())).remove();
        QFile(QString(tmp_path.toQString() + "\\" + script_filename.toQString())).remove();
      }
    }
  }
  */

  void emdClusterData_(String tmp_path, String data_filename, String file_suffix, String qc_output_directory, Int debug_level)
  {
    String script_filename = String("cluster_data") + file_suffix + String(".R");
    String result_filename = String("cluster_result") + file_suffix + String(".dat");

    // add scripts
    TextFile current_script;

    current_script.push_back("library(fpc)");   
    current_script.push_back("d=read.table('" + tmp_path + "/" + data_filename + "', sep='\\t')");
    current_script.push_back("m=as.matrix(d[,-(1:2)])");
    current_script.push_back("m=t(m)");
    current_script.push_back("clusters=rep(1,nrow(t(m)))");

    current_script.push_back("if (nrow(t(m))>=20)");
    current_script.push_back("{"); 
    current_script.push_back("result<-dbscan(t(m), eps=9999, MinPts=3, method=\"dist\")");  // 9999 corresponds to an eps of 9.999% RIA
    current_script.push_back("clusters=predict(result)");
    current_script.push_back("}");
    current_script.push_back("r=cbind(clusters, d[,1])");
    current_script.push_back("r=r[!is.na(r[,1]),]");
    current_script.push_back("write.table(r, '" + tmp_path + "/" + result_filename + "', col.names=FALSE, row.names=FALSE, sep='\t')");

    current_script.store(tmp_path + "/" + script_filename);

    QStringList qparam;
    qparam << "--vanilla" << "--quiet" << "--slave" << "--file=" + QString(tmp_path.toQString() + "\\" + script_filename.toQString());
    Int status = QProcess::execute("R", qparam);
    if (status != 0)
    {
      std::cerr << "Error: Process returned with non 0 status." << std::endl;
    } else
    {              
      if (debug_level < 1)
      {          
        QFile(QString(tmp_path.toQString() + "\\" + data_filename.toQString())).remove();
        QFile(QString(tmp_path.toQString() + "\\" + script_filename.toQString())).remove();
      }
    }
  }


  void plotAndClusterCorrelationData_(String tmp_path, String data_filename, Size bins, String file_suffix, String qc_output_directory, Int debug_level)
  {
    String script_filename = String("cluster_data") + file_suffix + String(".R");
    String plot_filename = String("cluster_plot") + file_suffix + String(".") + getStringOption_("plot_extension");
    String result_filename = String("cluster_result") + file_suffix + String(".dat");
      
    // add scripts
    TextFile current_script;

    current_script.push_back("library(clValid)");   
    current_script.push_back("d=read.table('" + tmp_path + "/" + data_filename + "', sep='\\t')");
    current_script.push_back("m=as.matrix(d[,-(1:2)])");
    current_script.push_back("m[m<0.0]=0");
    current_script.push_back("m=t(m)");
    current_script.push_back("clusters=rep(1,nrow(t(m)))");

    cout << "Cluster data..." << endl;
    current_script.push_back("if (nrow(t(m))>=20)");
    current_script.push_back("{");
    current_script.push_back("max_clust<-max(3,nrow(t(m))%/%5)");  // on average there should be at least 5 spectra per group and we want to check at least 2 to 3 cluster
    current_script.push_back("max_clust<-min(max_clust, 10)");  // at most 10 cluster      
    current_script.push_back("result<-clValid(t(m), metric='correlation', nClust=2:max_clust, clMethods=c('pam'), validation='internal', maxitem=nrow(t(m)))");
    current_script.push_back("k = as.numeric(as.matrix(optimalScores(result)[,\"Clusters\"]))");
    current_script.push_back("k = median(k)");
    current_script.push_back("cl = pam(x=t(m),k=k)");
    //current_script.push_back("cl=kmeans(x=t(m),centers=k)");
    current_script.push_back("clusters=cl$cluster");
    current_script.push_back("}");


    if (getStringOption_("plot_extension") == "png")
    {
      current_script.push_back("png('" + tmp_path + "/" + plot_filename + "', width=1280, height=720)");
    } else if (getStringOption_("plot_extension") == "svg")
    {
      current_script.push_back("svg('" + tmp_path + "/" + plot_filename + "', width=16, height=9)");
    } else if (getStringOption_("plot_extension") == "pdf")
    {
      current_script.push_back("pdf('" + tmp_path + "/" + plot_filename + "', width=16, height=9)");
    }

    current_script.push_back("colors<-adjustcolor(clusters, alpha=0.3)");
    current_script.push_back("matplot(x=seq(0,99.9,"+ String(100.0/bins)+ "), y=m, type='l', col=colors, ylim=c(0,1), lty=1, ylab='Correlation', xlab='RIA (%)')");
    current_script.push_back("tmp<-dev.off()");
    current_script.push_back("r=cbind(clusters, d[,1])");
    current_script.push_back("r=r[!is.na(r[,1]),]");
    current_script.push_back("write.table(r, '" + tmp_path + "/" + result_filename + "', col.names=FALSE, row.names=FALSE, sep='\t')");

    current_script.store(tmp_path + "/" + script_filename);

    QStringList qparam;
    qparam << "--vanilla" << "--quiet" << "--slave" << "--file=" + QString(tmp_path.toQString() + "\\" + script_filename.toQString());
    Int status = QProcess::execute("R", qparam);
    if (status != 0)
    {
      std::cerr << "Error: Process returned with non 0 status." << std::endl;
    } else
    {        
      QFile(QString(tmp_path.toQString() + "\\" + plot_filename.toQString())).copy(qc_output_directory.toQString() + "/cluster_plot" + file_suffix.toQString() + String(".").toQString() + getStringOption_("plot_extension").toQString());
      if (debug_level < 1)
      {          
        QFile(QString(tmp_path.toQString() + "\\" + data_filename.toQString())).remove();
        QFile(QString(tmp_path.toQString() + "\\" + script_filename.toQString())).remove();
        QFile(QString(tmp_path.toQString() + "\\" + plot_filename.toQString())).remove();
      }
    }
  }

  void plotCorrelationData_(String tmp_path, String data_filename, Size bins, String file_suffix, String qc_output_directory, Int debug_level)
  {
    String script_filename = String("cluster_data") + file_suffix + String(".R");
    String plot_filename = String("cluster_plot") + file_suffix + String(".") + getStringOption_("plot_extension");

    // add scripts
    TextFile current_script;

    current_script.push_back("d=read.table('" + tmp_path + "/" + data_filename + "', sep='\\t')");
    current_script.push_back("m=as.matrix(d[,-(1:2)])");
    current_script.push_back("m[m<0.0]=0");
    current_script.push_back("m=t(m)");
    current_script.push_back("clusters=rep(1,nrow(t(m)))");

    if (getStringOption_("plot_extension") == "png")
    {
      current_script.push_back("png('" + tmp_path + "/" + plot_filename + "', width=1280, height=720)");
    } else if (getStringOption_("plot_extension") == "svg")
    {
      current_script.push_back("svg('" + tmp_path + "/" + plot_filename + "', width=16, height=9)");
    } else if (getStringOption_("plot_extension") == "pdf")
    {
      current_script.push_back("pdf('" + tmp_path + "/" + plot_filename + "', width=16, height=9)");
    }

    current_script.push_back("colors<-adjustcolor(clusters, alpha=0.3)");
    current_script.push_back("matplot(x=seq(0,99.9,"+ String(100.0/bins)+ "), y=m, type='l', col=colors, ylim=c(0,1), lty=1, ylab='Correlation', xlab='RIA (%)')");
    current_script.push_back("tmp<-dev.off()");
    current_script.push_back("r=cbind(clusters, d[,1])");
    current_script.push_back("r=r[!is.na(r[,1]),]");

    current_script.store(tmp_path + "/" + script_filename);

    QStringList qparam;
    qparam << "--vanilla" << "--quiet" << "--slave" << "--file=" + QString(tmp_path.toQString() + "\\" + script_filename.toQString());
    Int status = QProcess::execute("R", qparam);
    if (status != 0)
    {
      std::cerr << "Error: Process returned with non 0 status." << std::endl;
    } else
    {        
      QFile(QString(tmp_path.toQString() + "\\" + plot_filename.toQString())).copy(qc_output_directory.toQString() + "/cluster_plot" + file_suffix.toQString() + String(".").toQString() + getStringOption_("plot_extension").toQString());
      if (debug_level < 1)
      {          
        QFile(QString(tmp_path.toQString() + "\\" + data_filename.toQString())).remove();
        QFile(QString(tmp_path.toQString() + "\\" + script_filename.toQString())).remove();
        QFile(QString(tmp_path.toQString() + "\\" + plot_filename.toQString())).remove();
      }
    }
  }

  ExitCodes main_(int, const char**)
  {
    String plot_extension_ = getStringOption_("plot_extension");  
    Int debug_level = getIntOption_("debug");
    String in_mzml = getStringOption_("in_mzML");
    String in_features = getStringOption_("in_featureXML");
    DoubleReal mz_tolerance_ppm_ = getDoubleOption_("mz_tolerance_ppm");
    DoubleReal weight_merge_window_ = getDoubleOption_("weight_merge_window");
    DoubleReal intensity_threshold_ = getDoubleOption_("intensity_threshold");
    qc_output_directory_ = getStringOption_("qc_output_directory");
    cluster_type_ = getStringOption_("cluster_type");

    // trying to create qc_output_directory if not present
    QDir qc_dir(qc_output_directory_.toQString());
    if (!qc_dir.exists()) 
    {
      qc_dir.mkpath(qc_output_directory_.toQString());
    }

    String out_csv = getStringOption_("out_csv");
    bool use_N15 = getFlag_("use_15N");
    bool cluster_flag = getFlag_("cluster");
    String debug_patterns_name = getStringOption_("debug_patterns_name");

    ofstream out_csv_stream(out_csv.c_str());
    out_csv_stream << fixed << setprecision(4);
    SVOutStream out_stream(out_csv_stream, "\t", "_", String::NONE);
    DoubleReal correlation_threshold = getDoubleOption_("correlation_threshold");

    DoubleReal min_correlation_distance_to_averagine = getDoubleOption_("min_correlation_distance_to_averagine");
    
    tmp_path_ = File::getTempDirectory();
    tmp_path_.substitute('\\', '/');
    


    // read descriptions from FASTA and create map for fast annotation
    cout << "loading sequences..." << endl;
    String in_fasta = getStringOption_("in_fasta");
    vector<FASTAFile::FASTAEntry> fasta_entries;
    FASTAFile().load(in_fasta, fasta_entries);
    map<String, String> proteinid_to_description;
    for (vector<FASTAFile::FASTAEntry>::const_iterator it = fasta_entries.begin(); it != fasta_entries.end(); ++it)
    {
      if (!it->identifier.empty() && !it->description.empty())
      {
        String s = it->identifier;
        proteinid_to_description[s.trim().toUpper()] = it->description;    
      }
    }
    
    cout << "loading feature map..." << endl;
    FeatureXMLFile fh;
    FeatureMap<> feature_map;
    fh.load(in_features, feature_map);

    cout << "loading experiment..." << endl;
    MSExperiment<Peak1D> peak_map;
    MzMLFile mh;
    std::vector<Int> ms_level(1, 1);
    mh.getOptions().setMSLevels(ms_level);
    mh.load(in_mzml, peak_map);
    peak_map.updateRanges();
    ThresholdMower tm;
    Param tm_parameters;
    tm_parameters.setValue("threshold", intensity_threshold_);
    tm.setParameters(tm_parameters);
    tm.filterPeakMap(peak_map);
    peak_map.sortSpectra();

    // used to generate plots
    vector<String> titles;
    vector<MapRateToScoreType> weight_maps;
    vector<MapRateToScoreType> normalized_weight_maps;
    vector<MapRateToScoreType> correlation_maps;

    String file_suffix = "_" +  String(QFileInfo(in_mzml.toQString()).baseName()) + "_" + String::random(4);

    vector<SIPPeptide> sip_peptides;

    Size nPSMs = 0; ///< number of PSMs. If 0 IDMapper has not been called.

    for (FeatureMap<>::iterator feature_it = feature_map.begin(); feature_it != feature_map.end(); ++feature_it) // for each peptide feature
    {
      const DoubleReal feature_hit_center_rt = feature_it->getRT();

      // check if out of experiment bounds
      if (feature_hit_center_rt > peak_map.getMaxRT() || feature_hit_center_rt < peak_map.getMinRT())
      {
        continue;
      }

      // Extract 1 or more MS/MS with identifications assigned to the feature by IDMapper
      vector<PeptideIdentification> pep_ids = feature_it->getPeptideIdentifications();
   
      nPSMs += pep_ids.size();

      // Skip features without peptide identifications
      if (pep_ids.empty())
      {
        continue;
      }

      // add best scoring PeptideHit of all PeptideIdentifications mapping to the current feature to tmp_pepid
      PeptideIdentification tmp_pepid;
      tmp_pepid.setHigherScoreBetter(pep_ids[0].isHigherScoreBetter());
      for (Size i = 0; i != pep_ids.size(); ++i)
      {
        pep_ids[i].assignRanks();
        const vector<PeptideHit>& hits = pep_ids[i].getHits();        
        tmp_pepid.insertHit(hits[0]);        
      }
      tmp_pepid.assignRanks();

     
      SIPPeptide sip_peptide;

      // retrieve identification information
      const PeptideHit& feature_hit = tmp_pepid.getHits()[0];
      const DoubleReal feature_hit_score = feature_hit.getScore();
      const DoubleReal feature_hit_center_mz = feature_it->getMZ();
      const DoubleReal feature_hit_charge = feature_hit.getCharge();
      const AASequence feature_hit_aaseq = feature_hit.getSequence();
      const String feature_hit_seq = feature_hit.getSequence().toString();
      const DoubleReal feature_hit_theoretical_mz = feature_hit.getSequence().getMonoWeight(Residue::Full, feature_hit.getCharge()) / feature_hit.getCharge();

      sip_peptide.accessions = feature_hit.getProteinAccessions();
      sip_peptide.sequence = feature_hit_aaseq;
      sip_peptide.mz_theo = feature_hit_theoretical_mz;
      sip_peptide.charge = feature_hit_charge;
      sip_peptide.score = feature_hit_score;
      sip_peptide.feature_rt = feature_hit_center_rt;
      sip_peptide.feature_mz = feature_hit_center_mz;
      sip_peptide.unique = sip_peptide.accessions.size() == 1 ? true : false;                   

      // determine retention time of scans next to the central scan
      vector<DoubleReal> seeds_rt = findApexRT(feature_it, feature_hit_center_rt, peak_map, 2); // 1 scan at maximum, 2+2 above and below
      DoubleReal max_trace_int_rt = seeds_rt[0];

      // determine maximum number of peaks and mass difference
      EmpiricalFormula e = feature_hit_aaseq.getFormula();
      //cout << "Empirical formula: " << e.getString() << endl;

      DoubleReal mass_diff;
      Size element_count;
      if (!use_N15)
      {
        mass_diff = 1.003354837810;
        element_count = e.getNumberOf("Carbon");      
        //cout << "Carbon count: " << element_count << endl;
      } else
      {
        mass_diff = 0.9970349;
        element_count = e.getNumberOf("Nitrogen");
        //cout << "Nitrogen count: " << element_count << endl;
      }
      sip_peptide.mass_diff = mass_diff;

      // collect 13C / 15N peaks
      vector<DoubleReal> isotopic_intensities = extractIsotopicIntensitiesConsensus(element_count + ADDITIONAL_ISOTOPES, mass_diff, mz_tolerance_ppm_, seeds_rt, feature_hit_theoretical_mz, feature_hit_charge, peak_map);
      DoubleReal TIC = accumulate(isotopic_intensities.begin(), isotopic_intensities.end(), 0.0);

      // no Peaks collected
      if (TIC < 1e-4)
      {
        if (debug_level > 0)
        {
          cout << "no isotopic peaks in spectrum" << endl;
        }        
        continue;
      }

      // FOR VALIDATION: extract the merged peak spectra for later visualization during validation
      sip_peptide.merged = extractPeakSpectrumConsensus(element_count  + ADDITIONAL_ISOTOPES, mass_diff, seeds_rt, feature_hit_theoretical_mz, feature_hit_charge, peak_map);
      // FOR VALIDATION: filter peaks outside of x ppm window around expected isotopic peak
      sip_peptide.filtered_merged = filterPeakSpectrumForIsotopicPeaks(element_count + ADDITIONAL_ISOTOPES, mass_diff, feature_hit_theoretical_mz, feature_hit_charge, sip_peptide.merged, mz_tolerance_ppm_);
      // store accumulated intensities at theoretical positions 
      sip_peptide.accumulated = isotopicIntensitiesToSpectrum(feature_hit_theoretical_mz, mass_diff, feature_hit_charge, isotopic_intensities);

      sip_peptide.global_LR = calculateGlobalLR(isotopic_intensities);

      cout << "isotopic intensities collected: " << isotopic_intensities.size() << endl;

      cout << feature_hit.getSequence().toString() << "\trt: " << max_trace_int_rt << endl;

      // correlation filtering
      MapRateToScoreType map_rate_to_correlation_score;

      IsotopePatterns patterns;

      // calculate isotopic patterns for the given sequence, incoroporation interval/steps
      // pair<incoperation rate, isotopic peaks>

      if (!use_N15)
      {
        patterns = calculateIsotopePatternsFor13CRange(AASequence(feature_hit_seq));
      } else
      {
        patterns = calculateIsotopePatternsFor15NRange(AASequence(feature_hit_seq));
      }

      // store theoretical patterns for visualization
      sip_peptide.patterns = patterns;
      for (IsotopePatterns::const_iterator pit = sip_peptide.patterns.begin(); pit != sip_peptide.patterns.end(); ++pit)
      {
        PeakSpectrum p = isotopicIntensitiesToSpectrum(feature_hit_theoretical_mz, mass_diff, feature_hit_charge, pit->second);
        p.setMetaValue("rate", (DoubleReal)pit->first);
        p.setMSLevel(2);
        sip_peptide.pattern_spectra.push_back(p);
      }

      // calculate decomposition into isotopic patterns
      MapRateToScoreType map_rate_to_decomposition_weight;
      Int result = calculateDecompositionWeightsIsotopicPatterns(feature_hit_seq, isotopic_intensities, patterns, map_rate_to_decomposition_weight, use_N15, sip_peptide);
      
      // set first intensity to zero and remove first 2 possible RIAs (0% and e.g. 1.07% for carbon)
      MapRateToScoreType tmp_map_rate_to_correlation_score;
      if (getFlag_("filter_monoisotopic"))
      {
        // calculate correlation of natural RIAs (for later reporting) before we subtract the intensities. This is somewhat redundant but no speed bottleneck.
        calculateCorrelation(feature_hit_seq, isotopic_intensities, patterns, tmp_map_rate_to_correlation_score, use_N15);
        for (Size i = 0; i != sip_peptide.reconstruction_monoistopic.size(); ++i)
        {  

          if (i == 0)
          {
            isotopic_intensities[0] = 0;
          }

          isotopic_intensities[i] -= sip_peptide.reconstruction_monoistopic[i];
          if (isotopic_intensities[i] < 0)
          {
            isotopic_intensities[i] = 0;
          }
        }
      }

      sip_peptide.decomposition_map = map_rate_to_decomposition_weight;

      // calculate Pearson correlation coefficients
      calculateCorrelation(feature_hit_seq, isotopic_intensities, patterns, map_rate_to_correlation_score, use_N15);  
      
      // restore original correlation of natural RIAs (take maximum of observed correlations)
      if (getFlag_("filter_monoisotopic"))
      {
        MapRateToScoreType::iterator dc_it = map_rate_to_correlation_score.begin();
        MapRateToScoreType::const_iterator tmp_dc_it = tmp_map_rate_to_correlation_score.begin();        
        dc_it->second = max(tmp_dc_it->second, dc_it->second);
        ++dc_it; 
        ++tmp_dc_it;
        dc_it->second = max(tmp_dc_it->second, dc_it->second);
      }
      
      sip_peptide.correlation_map = map_rate_to_correlation_score;

      // determine maximum correlations
      sip_peptide.correlation_maxima = getHighPoints(correlation_threshold, map_rate_to_correlation_score);

      vector<String> feature_protein_accessions = feature_hit.getProteinAccessions();

      // FOR REPORTING: store incorporation information like e.g. theoretical spectrum for best correlations
      if (getStringOption_("collect_method") == "correlation_maximum")
      {
        extractIncorporationsAtCorrelationMaxima(sip_peptide, patterns, weight_merge_window_, correlation_threshold);
      } else if (getStringOption_("collect_method") == "decomposition_maximum")
      {
        extractIncorporationsAtHeighestDecompositionWeights(sip_peptide, patterns, weight_merge_window_, correlation_threshold, getDoubleOption_("lowRIA_correlation_threshold"));
      }

      // store sip peptide
      if (sip_peptide.incorporations.size() != 0)
      {
        if (debug_level > 0)
        {
          cout << "SIP peptides: " << sip_peptide.incorporations.size() << endl;
        }        
        sip_peptides.push_back(sip_peptide);
      }

      MapRateToScoreType map_rate_to_normalized_weight = normalizeToMax(map_rate_to_decomposition_weight);

      // store for plotting
      titles.push_back(feature_hit_seq + " " + String(feature_hit_center_rt));
      weight_maps.push_back(map_rate_to_decomposition_weight);
      normalized_weight_maps.push_back(map_rate_to_normalized_weight);
      correlation_maps.push_back(map_rate_to_correlation_score);
    }

    if (nPSMs == 0)
    {
      cerr << "No assigned identifications found in featureXML. Did you forget to run IDMapper?" << endl;
      return INCOMPATIBLE_INPUT_DATA;
    }
      
    if (sip_peptides.size() == 0)
    {
      cerr << "No peptides passing the incorporation threshold found." << endl;
      return INCOMPATIBLE_INPUT_DATA;
    }

    // copy meta information
    MSExperiment<Peak1D> debug_exp = peak_map;
    debug_exp.clear(false);

    unsigned int bins = 1000;

    for (Size i = 0; i != sip_peptides.size(); ++i)
    {
      const SIPPeptide& sip_peptide = sip_peptides[i];
      addDebugSpectra(debug_exp, sip_peptide);      
    }  
    
    vector<vector<SIPPeptide> > sippeptide_clusters;  // vector of cluster
    if (cluster_flag)  // data has been clustered so read back the result from R
    {
      if (cluster_type_ == "emd")
      {
        cout << "Writing emd data..." << endl;
        String emd_data_filename = String("emd_data_filename") + file_suffix + String(".dat");
        createEMDDataFile_(tmp_path_, emd_data_filename, file_suffix, sip_peptides, bins);

        cout << "Cluster data..." << endl;
        emdClusterData_(tmp_path_, emd_data_filename, file_suffix, qc_output_directory_, debug_level);
      } else if (cluster_type_ == "profile")
      {
        cout << "Writing correlation data..." << endl;
        String correlation_data_filename = String("cluster_data") + file_suffix + String(".dat");
        createCorrelationDataFile_(correlation_data_filename, sip_peptides, bins);

        cout << "Cluster data..." << endl;
        plotAndClusterCorrelationData_(tmp_path_, correlation_data_filename, bins, file_suffix, qc_output_directory_, debug_level);
      }

      String result_filename = String("cluster_result") + file_suffix + String(".dat");
      map<Size, vector<SIPPeptide> > map_cluster2sippeptide;
      cout << "Reading cluster results..." << endl;

      // read R output 
      TextFile csv_file(tmp_path_ + "/" + result_filename);
      for (Size i = 0; i != csv_file.size(); ++i)
      {
        std::vector<String> fields;
        csv_file[i].split('\t', fields);
        if (fields.size() != 2)
        {
          continue;
        }
        Size cluster = (Size)fields[0].toInt();
        Size index = (Size)fields[1].toInt();
        map_cluster2sippeptide[cluster].push_back(sip_peptides[index]);
      }

      // remove cluster results file
      if (debug_level < 1)
      {
        QFile(QString(tmp_path_.toQString() + "/" + result_filename.toQString())).remove();
      }

      // drop R internal cluster number and copy to simpler structure
      sip_peptides.clear();
      for (map<Size, vector<SIPPeptide> >::const_iterator mit = map_cluster2sippeptide.begin(); mit != map_cluster2sippeptide.end(); ++mit)
      {
        sippeptide_clusters.push_back(mit->second);
        sip_peptides.insert(sip_peptides.end(), mit->second.begin(), mit->second.end());  // also reorder sip_peptides to reflect new order
      }
    } else // data hasn't been clustered so just add all SIP peptides as cluster zero
    { 
      sippeptide_clusters.push_back(sip_peptides);
    }

    createCSVReport_(sippeptide_clusters, out_stream, proteinid_to_description, out_csv_stream);

    // plot debug spectra 
    if (!debug_patterns_name.empty())
    {
      MzMLFile mtest;
      mtest.store(debug_patterns_name, debug_exp);  
    }

    // quality report
    if (!qc_output_directory_.empty())
    {
      createQualityReport_(qc_output_directory_, file_suffix, sip_peptides, sippeptide_clusters);
    }

    return EXECUTION_OK;
  }

  void createCorrelationDataFile_(String correlation_data_filename, vector<SIPPeptide> &sip_peptides, unsigned int bins )
  {    
    ofstream correlation_out_stream(String(tmp_path_ + "/" + correlation_data_filename).c_str());
    SVOutStream correlation_out(correlation_out_stream);

    for (Size i = 0; i != sip_peptides.size(); ++i)
    {
      const SIPPeptide& sip_peptide = sip_peptides[i];

      // output correlation results
      correlation_out << i << sip_peptide.feature_rt;

      for (Size l = 0; l != bins; ++l)
      {  
        MapRateToScoreType::const_iterator k = sip_peptide.correlation_map.lower_bound((DoubleReal)l / (bins/100.0) - 1e-4);
        if (k == sip_peptide.correlation_map.end())
        {
          correlation_out << 0.0;
        } else
        {
          if (k != sip_peptide.correlation_map.begin())
          {
            k--;
          }
          MapRateToScoreType::const_iterator kk = k;
          ++kk;
          if (k->second <= 1.0 + 1e-4)
          {
            if (kk == sip_peptides[i].correlation_map.end())
            {
              correlation_out << k->second;
            } else
            {
              DoubleReal a = ((DoubleReal)l/(bins/100.0)- k->first) / (kk->first - k->first);
              //cout << (DoubleReal)l/10.0 << "\t" << k->first << "\t" << kk->first << "\t" << a << endl;
              correlation_out << (1-a) * k->second + a * kk->second;
            }              
          } else
          {
            correlation_out << 0.0;
          }
        }
      }
      correlation_out << endl;  
    }

    correlation_out_stream.close();
  }

  /*
  SIPGroupReport createSIPGroupsReport(vector<vector<SIPPeptide> >& sippeptide_cluster)
  {
    SIPGroupsReport groups;

    // sort clusters by non increasing size
    sort(sippeptide_cluster.rbegin(), sippeptide_cluster.rend(), SizeLess());

    for(Size i = 0; i != sippeptide_cluster.size(); ++i)
    {
      const vector<SIPPeptide>& current_cluster = sippeptide_cluster[i];
      
      // perform protein inference
      map<String, vector<SIPPeptide> > all_peptides;  // map sequence to SIPPeptide  
      map<String, vector<SIPPeptide> > ambigous_peptides;  // map sequence to SIPPeptide
      map<String, map<String, vector<SIPPeptide> > > unambigous_proteins;  // map Accession to unmodified String to SIPPeptides

      for (Size k = 0; k != current_cluster.size(); ++k)
      {
        const SIPPeptide& current_SIPpeptide = current_cluster[k];
        String seq = current_SIPpeptide.sequence.toUnmodifiedString();
        if (current_SIPpeptide.unique)
        {            
          unambigous_proteins[current_SIPpeptide.accessions[0]][seq].push_back(current_SIPpeptide);
        } else
        {
          ambigous_peptides[current_SIPpeptide.sequence.toUnmodifiedString()].push_back(current_SIPpeptide);
        }
        all_peptides[seq].push_back(current_SIPpeptide);
      }

      Size n_all_peptides = all_peptides.size();  // # of different (on sequence level) unique and non-unique peptides 
      Size n_ambigous_peptides = ambigous_peptides.size();
      Size n_unambigous_proteins = unambigous_proteins.size();
      
      // determine median and stdev of global LR of all peptides (unique and ambigous) for the whole group 
      {
        vector<DoubleReal> group_global_LRs;
        vector<DoubleReal> group_number_RIAs;
        for (map<String, vector<SIPPeptide> >::const_iterator all_it = all_peptides.begin(); all_it != all_peptides.end(); ++all_it)
        {
          for (vector<SIPPeptide>::const_iterator v_it = all_it->second.begin(); v_it != all_it->second.end(); ++v_it)
          {
            group_global_LRs.push_back(v_it->global_LR);
            group_number_RIAs.push_back(v_it->incorporations.size());
          }
        }

        // determine median number of RIAs (TODO: maybe use maximum here)
        Size group_number_RIA = (Size) (Math::median(group_number_RIAs.begin(), group_number_RIAs.end(), false) + 0.5); // median number of RIAs

        // calculate median LR
        SIPSummaryStatistic group_summary_statistic;      
        group_summary_statistic.median_LR = Math::median(group_global_LRs.begin(), group_global_LRs.end(), false);        
      
        // calculate std. deviation
        DoubleReal sum = std::accumulate(group_global_LRs.begin(), group_global_LRs.end(), 0.0);
        DoubleReal mean = sum / group_global_LRs.size();
        DoubleReal sq_sum = std::inner_product(group_global_LRs.begin(), group_global_LRs.end(), group_global_LRs.begin(), 0.0);
        group_summary_statistic.stdev_LR = std::sqrt(sq_sum / group_global_LRs.size() - mean * mean);
      }

      SIPProteinReport protein_report;
      protein_report.accession =
      protein_report.description =
      protein_report.peptides =
      protein_report.protein_statistic =;
      groups.map_id_to_protein_group[i].grouped_proteins.push_back(protein_report);
      groups.map_id_to_protein_group[i].group_statistics = group_summary_statistics;
    }
  }
  */
  void createCSVReport_( vector<vector<SIPPeptide> >& sippeptide_cluster, SVOutStream& out_stream, map<String, String> &proteinid_to_description, ofstream &out_csv_stream)
  {
    // sort clusters by non increasing size
    sort(sippeptide_cluster.rbegin(), sippeptide_cluster.rend(), SizeLess());

    for(Size i = 0; i != sippeptide_cluster.size(); ++i)
    {
      const vector<SIPPeptide>& current_cluster = sippeptide_cluster[i];

      // Group
      map<String, vector<SIPPeptide> > all_peptides;  // map sequence to SIPPeptide  
      map<String, vector<SIPPeptide> > ambigous_peptides;  // map sequence to SIPPeptide
      map<String, map<String, vector<SIPPeptide> > > unambigous_proteins;  // map Accession to unmodified String to SIPPeptides

      for (Size k = 0; k != current_cluster.size(); ++k)
      {
        const SIPPeptide& current_SIPpeptide = current_cluster[k];
        String seq = current_SIPpeptide.sequence.toUnmodifiedString();
        if (current_SIPpeptide.unique)
        {            
          unambigous_proteins[current_SIPpeptide.accessions[0]][seq].push_back(current_SIPpeptide);
        } else
        {
          ambigous_peptides[current_SIPpeptide.sequence.toUnmodifiedString()].push_back(current_SIPpeptide);
        }
        all_peptides[seq].push_back(current_SIPpeptide);
      }

      Size n_all_peptides = all_peptides.size();  // # of different (on sequence level) unique and non-unique peptides 
      Size n_ambigous_peptides = ambigous_peptides.size();
      Size n_unambigous_proteins = unambigous_proteins.size();

      // determine median global LR of whole group
      vector<DoubleReal> group_global_LRs;
      vector<DoubleReal> group_number_RIAs;
      for (map<String, vector<SIPPeptide> >::const_iterator all_it = all_peptides.begin(); all_it != all_peptides.end(); ++all_it)
      {
        for (vector<SIPPeptide>::const_iterator v_it = all_it->second.begin(); v_it != all_it->second.end(); ++v_it)
        {
          group_global_LRs.push_back(v_it->global_LR);
          group_number_RIAs.push_back(v_it->incorporations.size());
        }
      }
      DoubleReal group_global_LR = Math::median(group_global_LRs.begin(), group_global_LRs.end(), false);        

      Size group_number_RIA = (Size) (Math::median(group_number_RIAs.begin(), group_number_RIAs.end(), false) + 0.5); // median number of RIAs
      // Group header
      // Distinct peptides := different (on sequence level) unique and non-unique peptides 
      out_stream << String("Group ") + String(i+1) << "# Distinct Peptides" << "# Unambigous Proteins" << "Median Global LR";
      for (Size i = 0; i != group_number_RIA; ++i)
      {
        out_stream << "median RIA " + String(i + 1);
      }
      out_stream << endl;

      out_stream << "" << n_all_peptides << n_unambigous_proteins << group_global_LR;

      // collect 1th, 2nd, ... RIA of the group based on the peptide RIAs
      vector< vector<DoubleReal> > group_RIAs(group_number_RIA, vector<DoubleReal>());
      vector< DoubleReal > group_RIA_medians(group_number_RIA, 0);

      for (map<String, vector<SIPPeptide> >::const_iterator all_it = all_peptides.begin(); all_it != all_peptides.end(); ++all_it)
      {
        for (vector<SIPPeptide>::const_iterator v_it = all_it->second.begin(); v_it != all_it->second.end(); ++v_it)
        {
          for (Size i = 0; i != group_number_RIA; ++i)
          {
            if (i == v_it->incorporations.size())
            {
              break;
            }                  
            group_RIAs[i].push_back(v_it->incorporations[i].rate);
          }
        }
      }  

      for (Size i = 0; i != group_number_RIA; ++i)
      {
        group_RIA_medians[i] = Math::median(group_RIAs[i].begin(), group_RIAs[i].end(), false);
      }

      for (Size i = 0; i != group_number_RIA; ++i)
      {
        out_stream << String(group_RIA_medians[i]);
      }
      out_stream << endl;

      // unambiguous protein level
      for (map<String, map<String, vector<SIPPeptide> > >::const_iterator prot_it = unambigous_proteins.begin(); prot_it != unambigous_proteins.end(); ++prot_it)
      {
        // determine median global LR of protein
        vector<DoubleReal> protein_global_LRs;
        vector<DoubleReal> protein_number_RIAs;
        for (map<String, vector<SIPPeptide> >::const_iterator pept_it = prot_it->second.begin(); pept_it != prot_it->second.end(); ++pept_it)
        {
          for (vector<SIPPeptide>::const_iterator v_it = pept_it->second.begin(); v_it != pept_it->second.end(); ++v_it)
          {
            protein_global_LRs.push_back(v_it->global_LR);
            protein_number_RIAs.push_back(v_it->incorporations.size());
          }
        }          
        DoubleReal protein_global_LR = Math::median(protein_global_LRs.begin(), protein_global_LRs.end(), false);
        Size protein_number_RIA = (Size) (Math::median(protein_number_RIAs.begin(), protein_number_RIAs.end(), false) + 0.5); // median number of RIAs

        out_stream << "" << "Protein Accession" << "Description" << "# Unique Peptides" << "Median Global LR";
        for (Size i = 0; i != protein_number_RIA; ++i)
        {
          out_stream << "median RIA " + String(i + 1);
        }
        out_stream << endl;

        String protein_accession = prot_it->first;
        String protein_description = "none";
        if (proteinid_to_description.find(protein_accession.trim().toUpper()) != proteinid_to_description.end())
        {
          protein_description = proteinid_to_description.at(protein_accession.trim().toUpper());
        }

        out_stream << "" << protein_accession << protein_description <<  prot_it->second.size() << protein_global_LR;

        vector< vector<DoubleReal> > protein_RIAs(protein_number_RIA, vector<DoubleReal>());
        vector< DoubleReal > protein_RIA_medians(protein_number_RIA, 0);

        // ratio to natural decomposition
        vector< vector<DoubleReal> > protein_ratio(protein_number_RIA, vector<DoubleReal>());
        vector< DoubleReal > protein_ratio_medians(protein_number_RIA, 0); 

        // collect 1th, 2nd, ... RIA of the protein based on the peptide RIAs
        for (map<String, vector<SIPPeptide> >::const_iterator pept_it = prot_it->second.begin(); pept_it != prot_it->second.end(); ++pept_it)
        {
          for (vector<SIPPeptide>::const_iterator v_it = pept_it->second.begin(); v_it != pept_it->second.end(); ++v_it)
          {
            for (Size i = 0; i != protein_number_RIA; ++i)
            {
              if (i == v_it->incorporations.size())
              {
                break;
              }                  
              protein_RIAs[i].push_back(v_it->incorporations[i].rate);
              protein_ratio[i].push_back(v_it->incorporations[i].abundance);
            }
          }
        }  

        for (Size i = 0; i != protein_number_RIA; ++i)
        {
          protein_RIA_medians[i] = Math::median(protein_RIAs[i].begin(), protein_RIAs[i].end(), false);
          protein_ratio_medians[i] = Math::median(protein_ratio[i].begin(), protein_ratio[i].end(), false);
        }

        for (Size i = 0; i != protein_number_RIA; ++i)
        {
          out_stream << String(protein_RIA_medians[i]);
        }

        out_stream << endl;

        // print header of unique peptides
        out_stream << "" << "" << "Peptide Sequence" << "RT" << "Exp. m/z" << "Theo. m/z" << "Charge" << "Score" << "TIC fraction" << "#non-natural weights" << "";
        Size max_incorporations = 0;
        for (map<String, vector<SIPPeptide> >::const_iterator pept_it = prot_it->second.begin(); pept_it != prot_it->second.end(); ++pept_it)
        {     
          for (vector<SIPPeptide>::const_iterator v_it = pept_it->second.begin(); v_it != pept_it->second.end(); ++v_it)
          {
            max_incorporations = std::max(v_it->incorporations.size(), max_incorporations);
          }
        }

        for (Size i = 0; i != max_incorporations; ++i)
        {
          out_stream << "RIA " + String(i + 1) << "INT " + String(i + 1) << "Cor. " + String(i + 1);
        }
        out_stream << "Peak intensities" << "Global LR" << endl;

        // print data of unique peptides
        for (map<String, vector<SIPPeptide> >::const_iterator pept_it = prot_it->second.begin(); pept_it != prot_it->second.end(); ++pept_it)
        {
          for (vector<SIPPeptide>::const_iterator v_it = pept_it->second.begin(); v_it != pept_it->second.end(); ++v_it)
          {
            out_stream << "" << "" << v_it->sequence.toString() << String::number(v_it->feature_rt / 60.0, 2)  << String::number(v_it->feature_mz, 4) << v_it->mz_theo << v_it->charge << v_it->score << v_it->explained_TIC_fraction << v_it->non_zero_decomposition_coefficients << "";
            for (vector<SIPIncorporation>::const_iterator incorps = v_it->incorporations.begin(); incorps != v_it->incorporations.end(); ++incorps)
            {
              out_stream << String::number(incorps->rate,1) << String::number(incorps->abundance, 0) << String::number(incorps->correlation, 2);
            }

            // blank entries for nicer formatting
            for (Int q = 0; q < max_incorporations - v_it->incorporations.size(); ++q)
            {
              out_stream << "" << "" << "";
            }

            // output peak intensities
            String peak_intensities;
            for (PeakSpectrum::const_iterator p = v_it->accumulated.begin(); p != v_it->accumulated.end(); ++p)
            {
              peak_intensities += String::number(p->getIntensity(),0) + " ";
            }
            out_stream << peak_intensities;
            out_stream << v_it->global_LR;

            out_stream << endl; 
          }
        }
      }

      // print header of non-unique peptides below the protein section
      Size max_incorporations = 0;
      for (map<String, vector<SIPPeptide> >::const_iterator pept_it = ambigous_peptides.begin(); pept_it != ambigous_peptides.end(); ++pept_it)
      {     
        for (vector<SIPPeptide>::const_iterator v_it = pept_it->second.begin(); v_it != pept_it->second.end(); ++v_it)
        {
          max_incorporations = std::max(v_it->incorporations.size(), max_incorporations);
        }
      }

      out_stream << "Non-Unique Peptides" << "Accessions" << "Peptide Sequence" << "Descriptions" << "Score" << "RT" << "Exp. m/z" << "Theo. m/z" << "Charge" << "#non-natural weights" << "";

      for (Size i = 0; i != max_incorporations; ++i)
      {
        out_stream << "RIA " + String(i + 1) << "INT " + String(i + 1) << "Cor. " + String(i + 1);
      }
      out_stream << "Peak intensities" << "Global LR" << endl;

      // print data of non-unique peptides below the protein section
      for (map<String, vector<SIPPeptide> >::const_iterator pept_it = ambigous_peptides.begin(); pept_it != ambigous_peptides.end(); ++pept_it)
      {     
        // build up the protein accession string for non-unique peptides. Only the first 3 accessions are added.
        for (vector<SIPPeptide>::const_iterator v_it = pept_it->second.begin(); v_it != pept_it->second.end(); ++v_it)
        {
          String accessions_string;
          String description_string = "none";

          for (Size ac = 0; ac != v_it->accessions.size(); ++ac)
          {
            if (ac >= 3) // only print at most 3 accessions as these can be quite numorous
            {
              accessions_string += "...";
              break;
            }
            String protein_accession = v_it->accessions[ac];
            accessions_string += protein_accession;

            if (proteinid_to_description.find(protein_accession.trim().toUpper()) != proteinid_to_description.end())
            {
              if (description_string == "none")
              {
                description_string = "";
              }
              description_string += proteinid_to_description.at(protein_accession.trim().toUpper());
            }

            if (ac < v_it->accessions.size() - 1) 
            {
              accessions_string += ", ";
              if (description_string != "none")
              {
                description_string += ", ";
              }
            }
          }

          out_stream << "" << accessions_string << v_it->sequence.toString() << description_string << v_it->score << String::number(v_it->feature_rt / 60.0, 2) << String::number(v_it->feature_mz, 4) << v_it->mz_theo << v_it->charge << v_it->non_zero_decomposition_coefficients << "";

          // output variable sized RIA part
          for (vector<SIPIncorporation>::const_iterator incorps = v_it->incorporations.begin(); incorps != v_it->incorporations.end(); ++incorps)
          {
            out_stream << String::number(incorps->rate,1) << String::number(incorps->abundance, 0) << String::number(incorps->correlation, 2);
          }

          // blank entries for nicer formatting
          for (Int q = 0; q < max_incorporations - v_it->incorporations.size(); ++q)
          {
            out_stream << "" << "" << "";
          }

          // output peak intensities
          String peak_intensities;
          for (PeakSpectrum::const_iterator p = v_it->accumulated.begin(); p != v_it->accumulated.end(); ++p)
          {
            peak_intensities += String::number(p->getIntensity(),0) + " ";
          }
          out_stream << peak_intensities;
          out_stream << v_it->global_LR;
          out_stream << endl; 
        }
      }      
    }
    out_csv_stream.close();
  }

  void createPeptideCentricCSVReport_( vector<vector<SIPPeptide> >& sippeptide_cluster, SVOutStream& out_stream, map<String, String> &proteinid_to_description, ofstream &out_csv_stream, String qc_output_directory="", String file_suffix="" )
  {
    // sort clusters by non increasing size
    sort(sippeptide_cluster.rbegin(), sippeptide_cluster.rend(), SizeLess());

    // store SIP peptide with cluster index for peptide centric view on data
    vector<pair<SIPPeptide, Size> > peptide_to_cluster_index;
    for(Size i = 0; i != sippeptide_cluster.size(); ++i)
    {
      const vector<SIPPeptide>& current_cluster = sippeptide_cluster[i];
      for (Size k = 0; k != current_cluster.size(); ++k)
      {
        peptide_to_cluster_index.push_back(make_pair(current_cluster[k], i));
      }
    }

    // sort by sequence
    sort(peptide_to_cluster_index.begin(), peptide_to_cluster_index.end(), SequenceLess());

    out_stream << "Peptide Sequence" << "Quality Report Spectrum" << "Quality report scores" << "Sample Name" << "Protein Accessions" << "Description" << "Unique" "#Ambiguity members" 
      << "Score" << "RT" << "Exp. m/z" << "Theo. m/z" << "Charge" << "TIC fraction" << "#non-natural weights" << "Peak intensities" << "Group" << "Global Peptide LR";

    for (Size i = 1; i <= 10; ++i)
    {
      out_stream << "RIA " + String(i) <<  "LR of RIA " + String(i) <<  "INT " + String(i)  <<  "Cor. " + String(i);
    }
    out_stream << std::endl;

    for(Size i = 0; i != peptide_to_cluster_index.size(); ++i)
    {
      const SIPPeptide& current_SIPpeptide = peptide_to_cluster_index[i].first;
      const Size& current_cluster_index = peptide_to_cluster_index[i].second;

      // output peptide sequence
      out_stream << current_SIPpeptide.sequence.toString();

      // output quality report links if available
      if (qc_output_directory_.empty() && file_suffix.empty())
      {
        out_stream << "" << "";
      } else
      {
        String plot_extension = getStringOption_("plot_extension");
        String qr_spectrum_filename = String("file://") + qc_output_directory + String("spectrum") + file_suffix + "_rt_" + String(current_SIPpeptide.feature_rt) + "." + plot_extension;
        String qr_scores_filename = String("file://") + qc_output_directory + String("scores")  + file_suffix + "_rt_" + String(current_SIPpeptide.feature_rt) + "." + plot_extension;
        out_stream << qr_spectrum_filename << qr_scores_filename << getStringOption_("in_mzML");
      }

      // output protein accessions and descriptions
      String accession_string;
      String protein_descriptions = "none";
      for (Size j = 0; j != current_SIPpeptide.accessions.size(); ++j)
      {
        String current_accession = current_SIPpeptide.accessions[j];
        current_accession.trim().toUpper();
        accession_string += current_accession;

        if (proteinid_to_description.find(current_accession) != proteinid_to_description.end())
        {
          if (protein_descriptions == "none")
          {
            protein_descriptions = proteinid_to_description.at(current_accession);
          } else
          {
            protein_descriptions += proteinid_to_description.at(current_accession);
          }
        }

        // add "," between accessions
        if (j != current_SIPpeptide.accessions.size()-1)
        {
          accession_string += ",";
          protein_descriptions += ",";
        }
      }      
     
      out_stream << accession_string << protein_descriptions << current_SIPpeptide.unique << current_SIPpeptide.accessions.size() << current_SIPpeptide.score << String::number(current_SIPpeptide.feature_rt / 60.0, 2)
        << String::number(current_SIPpeptide.feature_mz, 4) << String::number(current_SIPpeptide.mz_theo, 4) << current_SIPpeptide.charge << current_SIPpeptide.explained_TIC_fraction << current_SIPpeptide.non_zero_decomposition_coefficients;

      // output peak intensities
      String peak_intensities;
      for (PeakSpectrum::const_iterator p = current_SIPpeptide.accumulated.begin(); p != current_SIPpeptide.accumulated.end(); ++p)
      {
        peak_intensities += String::number(p->getIntensity(), 0) + " ";
      }
      out_stream << peak_intensities;
      
      out_stream << current_cluster_index << current_SIPpeptide.global_LR;

      for (Size j = 0; j != current_SIPpeptide.incorporations.size(); ++j)
      {
        const DoubleReal ria = current_SIPpeptide.incorporations[j].rate;
        const DoubleReal abundance = current_SIPpeptide.incorporations[j].abundance;
        const DoubleReal corr = current_SIPpeptide.incorporations[j].correlation;
        
        DoubleReal LR_of_RIA = 0;
        if (ria < 1.5) // first RIA hast natural abundance
        {
          LR_of_RIA = abundance / current_SIPpeptide.incorporations[0].abundance;
        }
        out_stream << String::number(ria, 1) << String::number(LR_of_RIA, 1) << String::number(abundance, 1) << String::number(corr, 1);
      }      
      out_stream << endl; 
    }

    out_stream << endl; 
    out_csv_stream.close();
  }

  /*
  void createProteinCentricCSVReport_( vector<vector<SIPPeptide> >& sippeptide_cluster, SVOutStream& out_stream, map<String, String> &proteinid_to_description, ofstream &out_csv_stream, String qc_output_directory="", String file_suffix="" )
  {
    TODO how should this look like afjklasdjflksadjflkasdjflk 
    out_stream << endl; 
    out_csv_stream.close();
  }
  */
  void createQualityReport_( String qc_output_directory, String file_suffix, vector<SIPPeptide> sip_peptides, const vector< vector<SIPPeptide> >& sip_peptide_cluster)
  {
    // heat map based on peptide RIAs
    cout << "Plotting peptide heat map" << endl;    
    vector< vector<DoubleReal> > binned_peptide_ria;
    vector<String> class_labels;
    createBinnedPeptideRIAData_(sip_peptide_cluster, binned_peptide_ria, class_labels);

    plotHeatMap_(qc_output_directory, tmp_path_, "_peptide" + file_suffix, binned_peptide_ria, class_labels);

    cout << "Plotting filtered spectra for quality report" << endl;
    plotFilteredSpectra_(qc_output_directory, tmp_path_, file_suffix, sip_peptides);
    if (getFlag_("plot_merged"))
    {
      cout << "Plotting merged spectra for quality report" << endl;
      plotMergedSpectra_(qc_output_directory, tmp_path_, file_suffix, sip_peptides);
    }

    cout << "Plotting correlation score and weight distribution" << endl;
    plotScoresAndWeights_(qc_output_directory, tmp_path_, file_suffix, sip_peptides);

    String plot_extension = getStringOption_("plot_extension");
    if (plot_extension != "pdf")  // html doesn't support pdf as image
    {
      writeHTML_(qc_output_directory, file_suffix, sip_peptides);
    }
  }

  void createBinnedPeptideRIAData_(const vector<vector<SIPPeptide>>& sip_clusters, vector< vector<DoubleReal> >& binned_peptide_ria, vector<String>& cluster_labels)
  {
    cluster_labels.clear();
    binned_peptide_ria.clear();
    const Size n_heatmap_bins = (Size)getIntOption_("heatmap_bins");
    for (vector<vector<SIPPeptide>>::const_iterator cit = sip_clusters.begin(); cit != sip_clusters.end(); ++cit)
    {
      const vector<SIPPeptide>& sip_peptides = *cit;
      for (vector<SIPPeptide>::const_iterator pit = sip_peptides.begin(); pit != sip_peptides.end(); ++pit)
      {
        vector<DoubleReal> binned(n_heatmap_bins, 0.0); 
        for (vector<SIPIncorporation>::const_iterator iit = pit->incorporations.begin(); iit != pit->incorporations.end(); ++iit)
        {
          Int bin = iit->rate / 100.0 * n_heatmap_bins;
          bin = bin > binned.size() - 1 ? binned.size() - 1 : bin;
          bin = bin < 0 ? 0 : bin;
          binned[bin] = log(1.0 + iit->abundance);
        }
        binned_peptide_ria.push_back(binned);
        cluster_labels.push_back((String)(cit - sip_clusters.begin()));
      }
    }
  }

  void addDebugSpectra(MSExperiment<Peak1D>& debug_exp, const SIPPeptide& sip_peptide)
  {
    debug_exp.addSpectrum(sip_peptide.merged);
    debug_exp[debug_exp.size() - 1].setRT(sip_peptide.feature_rt);
    debug_exp.addSpectrum(sip_peptide.filtered_merged);
    debug_exp[debug_exp.size() - 1].setRT(sip_peptide.feature_rt + 1e-6);
    debug_exp.addSpectrum(sip_peptide.accumulated);
    debug_exp[debug_exp.size() - 1].setRT(sip_peptide.feature_rt + 2e-6);
    debug_exp.addSpectrum(sip_peptide.reconstruction);
    debug_exp[debug_exp.size() - 1].setRT(sip_peptide.feature_rt + 3e-6);

    // theoretical spectra that have been chosen by correlation and decomposition
    for (Size j = 0; j != sip_peptide.incorporations.size(); ++j)
    {
      const SIPIncorporation& si = sip_peptide.incorporations[j];
      PeakSpectrum ps = si.theoretical;
      ps.setMSLevel(2);
      ps.setRT(sip_peptide.feature_rt + 4e-6 + j *  1e-6 );
      Precursor pc;
      pc.setMZ(si.rate);
      vector<Precursor> pcs;
      pcs.push_back(pc);
      ps.setPrecursors(pcs);
      debug_exp.addSpectrum(ps);
    }

    // push empty spectrum
    debug_exp.addSpectrum(PeakSpectrum());
    debug_exp[debug_exp.size() - 1].setRT(sip_peptide.feature_rt + 5e-6);

    // push all theoretical patterns
    for (Size j = 0; j != sip_peptide.pattern_spectra.size(); ++j)
    {
      PeakSpectrum ps = sip_peptide.pattern_spectra[j];
      ps.setRT(sip_peptide.feature_rt + 5e-6 + (j+1) *  1e-6 );
      Precursor pc;                
      DoubleReal rate = (DoubleReal)ps.getMetaValue("rate");
      pc.setMZ(rate);
      vector<Precursor> pcs;
      pcs.push_back(pc);
      ps.setPrecursors(pcs);
      //cout << "adding MS" << ps.getMSLevel() << " with " << ps.getPrecursors().size() << " precursors " << ps.getPrecursors()[0].getMZ() << endl; 
      debug_exp.addSpectrum(ps);
    }
  }

  void createEMDDataFile_(String tmp_path, String emd_data_filename, String file_suffix, vector<SIPPeptide> sip_peptides, unsigned int n_bins)
  {
    n_bins = 100;
    Size n_peptides = sip_peptides.size();    

    ofstream out_stream(String(tmp_path + "/" + emd_data_filename).c_str());
    //std::cout << "Writing emd data to: " << String(tmp_path + "/" + emd_data_filename) << endl;

    SVOutStream svout_stream(out_stream);

     cout << "EMD: binning data" << endl;
    // generate binned, integral version of decomposition coefficients for fast EMD calculation
    vector<vector<int> > binned_decompositions;
    for(Size i = 0; i != n_peptides; ++i)
    {
      const SIPPeptide& sip_peptide = sip_peptides[i];
      const vector<SIPIncorporation>& incs = sip_peptide.incorporations;
      std::vector<int> a(n_bins, 0);

      // determine max abundance of incorporations
      DoubleReal max_abundance = 0;
      for (vector<SIPIncorporation>::const_iterator it = incs.begin(); it != incs.end(); ++it)
      {
        if (it->abundance > max_abundance)
        {
          max_abundance = it->abundance;
        }
      }

      for (vector<SIPIncorporation>::const_iterator it = incs.begin(); it != incs.end(); ++it)
      {       
        DoubleReal rate = it->rate;
        Size bin_index = rate / 100.0 * n_bins;
        bin_index = bin_index > n_bins - 1 ? n_bins - 1 : bin_index;
        //a[bin_index] = it->abundance / max_abundance * 1000.0; // normalize peptide wise
        //a[bin_index] = log(1+it->abundance); // don't let mass influence clustering too much
        a[bin_index] = 1;
      }
      binned_decompositions.push_back(a);
    }
    
    cout << "EMD: create ground distance matrix" << endl;

    // generate ground distance cost matrix (distances between bins)
    std::vector<std::vector<int> > emd_cost_matrix;
    //FastEMDWrapper::generateCostMatrix(n_bins, 1000, emd_cost_matrix, 10); // adapt mass penalty if non-binary feature used

    cout << "EMD: calculate emd distances" << endl;
    // calculate EMD distance matrix
    std::vector<std::vector<int> > distance_matrix;
    distance_matrix.swap(std::vector< std::vector<int> >(n_peptides, std::vector<int>(n_peptides, 0)));

    for (Size i = 0; i != n_peptides; ++i)
    {
      for (Size j = 0; j != n_peptides; ++j)
      {
//        distance_matrix[i][j] = FastEMDWrapper::calcEMD(binned_decompositions[i], binned_decompositions[j], emd_cost_matrix, 1000*10);
      }
    }

    cout << "EMD: write matrix to file" << endl;
    // write matrix to file
    for (Size i = 0; i != n_peptides; ++i)
    {
      const SIPPeptide& sip_peptide = sip_peptides[i];

      svout_stream << i << sip_peptide.feature_rt;

      for (Size j = 0; j != n_peptides; ++j)
      {
        svout_stream << distance_matrix[i][j];
      }
      svout_stream << endl;  
    }
    out_stream.close();
  }
};


int main( int argc, const char** argv )
{
  TOPPMetaProSIP tool;
  return tool.main(argc, argv);
}

