//# RegionStats.cc: implementation of class for calculating region statistics and histograms

#include "RegionStats.h"
#include "Histogram.h"
#include "MinMax.h"

#include <chrono>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <tbb/blocked_range.h>
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_reduce.h>

#include <casacore/casa/Arrays/ArrayMath.h>
#include <casacore/lattices/LatticeMath/LatticeStatistics.h>

using namespace carta;
using namespace std;

// ***** Histograms *****

bool RegionStats::setHistogramRequirements(const std::vector<CARTA::SetHistogramRequirements_HistogramConfig>& histogramReqs) {
    m_configs = histogramReqs;
    return true;
}

size_t RegionStats::numHistogramConfigs() {
    return m_configs.size();
}

CARTA::SetHistogramRequirements_HistogramConfig RegionStats::getHistogramConfig(int histogramIndex) {
    CARTA::SetHistogramRequirements_HistogramConfig config;
    if (histogramIndex < m_configs.size())
        config = m_configs[histogramIndex];
    return config;
}

void RegionStats::getMinMax(float& minVal, float& maxVal, const std::vector<float>& data) {
    // get min and max values in data
    tbb::blocked_range<size_t> range(0, data.size());
    MinMax<float> mm(data);
    tbb::parallel_reduce(range, mm);
    std::tie(minVal, maxVal) = mm.getMinMax();
}

void RegionStats::fillHistogram(CARTA::Histogram* histogram, const std::vector<float>& data,
        const size_t chanIndex, const size_t stokesIndex, const int nBins, const float minVal,
        const float maxVal) {
    // stored?
    if (m_channelHistograms.count(chanIndex) && m_stokes==stokesIndex && m_bins==nBins) {
        *histogram = m_channelHistograms[chanIndex];
    } else {
        // find histogram for input array
        tbb::blocked_range<size_t> range(0, data.size());
        Histogram hist(nBins, minVal, maxVal, data);
        tbb::parallel_reduce(range, hist);
        std::vector<int> histogramBins = hist.getHistogram();
        float binWidth = hist.getBinWidth();

        // fill histogram
        histogram->set_channel(chanIndex);
        histogram->set_num_bins(nBins);
        histogram->set_bin_width(binWidth);
        histogram->set_first_bin_center(minVal + (binWidth / 2.0));
        *histogram->mutable_bins() = {histogramBins.begin(), histogramBins.end()};

        // save for next time
        m_channelHistograms[chanIndex] = *histogram;
        m_stokes = stokesIndex;
        m_bins = nBins;
    }
}

bool RegionStats::getChannelHistogram(CARTA::Histogram* histogram, int channel, int stokes, int numBins) {
    bool haveHistogram(false);
    if (m_channelHistograms.count(channel) && m_stokes==stokes && m_bins==numBins) {
        *histogram = m_channelHistograms[channel];
        haveHistogram = true;
    } else {
        histogram = nullptr;
    }
    return haveHistogram;
}

// ***** Statistics *****

void RegionStats::setStatsRequirements(const std::vector<int>& statsTypes) {
    m_regionStats = statsTypes;
}

size_t RegionStats::numStats() {
   return m_regionStats.size();
}

void RegionStats::fillStatsData(CARTA::RegionStatsData& statsData, const casacore::SubLattice<float>& subLattice) {
    // fill RegionStatsData with statistics types set in requirements

    if (m_regionStats.empty()) {  // no requirements set
        // add empty StatisticsValue
        auto statsValue = statsData.add_statistics();  // pointer
        statsValue->set_stats_type(CARTA::StatsType::None);
        return;
    }

    std::vector<std::vector<float>> results;
    if (getStatsValues(results, m_regionStats, subLattice)) {
        for (size_t i=0; i<m_regionStats.size(); ++i) {
            auto statType = static_cast<CARTA::StatsType>(m_regionStats[i]);
            std::vector<float> values(results[i]);
            // add StatisticsValue
            auto statsValue = statsData.add_statistics();
            statsValue->set_stats_type(statType);
            statsValue->set_value(values[0]); // only one value allowed
        }
    }
}

bool RegionStats::getStatsValues(std::vector<std::vector<float>>& statsValues,
    const std::vector<int>& requestedStats, const casacore::SubLattice<float>& subLattice) {
    // Fill statsValues vector for requested stats; one vector<float> per stat

    // use LatticeStatistics to fill statistics values according to type
    casacore::LatticeStatistics<float> latticeStats = casacore::LatticeStatistics<float>(subLattice,
            /*showProgress*/ false, /*forceDisk*/ false, /*clone*/ false);

    for (size_t i=0; i<requestedStats.size(); ++i) {
        // get requested statistics values
        std::vector<float> values;
        casacore::LatticeStatsBase::StatisticsTypes lattStatsType(casacore::LatticeStatsBase::NSTATS);
        auto statType = static_cast<CARTA::StatsType>(requestedStats[i]);

        switch (statType) {
            case CARTA::StatsType::None:
                break;
            case CARTA::StatsType::Sum:
                lattStatsType = casacore::LatticeStatsBase::SUM;
                break;
            case CARTA::StatsType::FluxDensity:
                lattStatsType = casacore::LatticeStatsBase::FLUX;
                break;
            case CARTA::StatsType::Mean:
                lattStatsType = casacore::LatticeStatsBase::MEAN;
                break;
            case CARTA::StatsType::RMS:
                lattStatsType = casacore::LatticeStatsBase::RMS;
                break;
            case CARTA::StatsType::Sigma:
                lattStatsType = casacore::LatticeStatsBase::SIGMA;
                break;
            case CARTA::StatsType::SumSq:
                lattStatsType = casacore::LatticeStatsBase::SUMSQ;
                break;
            case CARTA::StatsType::Min:
                lattStatsType = casacore::LatticeStatsBase::MIN;
                break;
            case CARTA::StatsType::Max:
                lattStatsType = casacore::LatticeStatsBase::MAX;
                break;
            case CARTA::StatsType::Blc:
            case CARTA::StatsType::Trc:
            case CARTA::StatsType::MinPos:
            case CARTA::StatsType::MaxPos: {
                // use LatticeRegion for positional stats
                const casacore::LatticeRegion* lregion = subLattice.getRegionPtr();
                casacore::Slicer lrSlicer = lregion->slicer();
                std::vector<int> result;
                if (statType==CARTA::StatsType::Blc) {
                    result = lrSlicer.start().asStdVector();
                } else if (statType==CARTA::StatsType::Trc) {
                    result = lrSlicer.end().asStdVector();
                } else {
                    std::vector<int> blc = lrSlicer.start().asStdVector();
                    casacore::IPosition minPos, maxPos;
                    latticeStats.getMinMaxPos(minPos, maxPos);
                    if (statType==CARTA::StatsType::MinPos) {
                        result = (blc + minPos).asStdVector();
                    } else { // MaxPos
                        result = (blc + maxPos).asStdVector();
                    }
                }
                values.reserve(result.size());
                for (unsigned int i=0; i<result.size(); ++i)  // convert to float
                    values.push_back(static_cast<float>(result[i]));
                }
                break;
            default:
                break;
        }

        if (lattStatsType < casacore::LatticeStatsBase::NSTATS) {
            casacore::Array<casacore::Double> result;  // has to be a Double
            latticeStats.getStatistic(result, lattStatsType);
            if (!result.empty()) {
                std::vector<double> dblResult(result.tovector());
                values.reserve(dblResult.size());
                for (unsigned int i=0; i<dblResult.size(); ++i)  // convert to float
                    values.push_back(static_cast<float>(dblResult[i]));
            }
        }
        statsValues.push_back(values);
    }
    return true;
}

