/*
 This file is part of CapMiner.

 CapMiner is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 CapMiner is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with CapMiner.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <libethcore/Farm.h>
#include <iostream> 
#include <fstream>
#include <iomanip> 
#include <string.h>
#include <vector>
#include <random>
#include <numeric>
#include "gnuplot-iostream.h"
#include <ethash/global_context.hpp>

#if ETH_ETHASHCL
#include <libethash-cl/CLMiner.h>
#endif

#if ETH_ETHASHCUDA
#include <libethash-cuda/CUDAMiner.h>
#endif

#if ETH_ETHASHCPU
#include <libethash-cpu/CPUMiner.h>
#endif

int counter = 0;
int magnitude;
int overclock = 0;
double avgHr = 0;
double avgPower = 0;
double avgProfit = 0;
double bestHash = 0;
double bestProfit = 0;
std::string clockChoice = "1";
static int ocConversion[] = {0, 200, 300, 400, 500};
std::string powerChoice = "100";
std::string powerOut = "100";

double stockHashrate = 8.87 * 1000000;
double stockRVNperHour = 0.215599555;
double RVNvalue = 0.065;
double kWhCost = 0.11175;
// Gnuplot gp("\"C:\\Program Files\\gnuplot\\bin\\gnuplot.exe\"");

// FILE *file = fopen("plot.dat", "a");
ofstream plotData;
bool append = false;
bool optimizeHash = true;

namespace dev
{
namespace eth
{



Farm* Farm::m_this = nullptr;

Farm::Farm(std::map<std::string, DeviceDescriptor>& _DevicesCollection,
    FarmSettings _settings, CUSettings _CUSettings, CLSettings _CLSettings, CPSettings _CPSettings)
  : m_Settings(std::move(_settings)),
    m_CUSettings(std::move(_CUSettings)),
    m_CLSettings(std::move(_CLSettings)),
    m_CPSettings(std::move(_CPSettings)),
    m_io_strand(g_io_service),
    m_collectTimer(g_io_service),
    m_DevicesCollection(_DevicesCollection)
{
    m_this = this;

    // Init HWMON if needed
    if (m_Settings.hwMon)
    {
        m_telemetry.hwmon = true;

#if defined(__linux)
        bool need_sysfsh = false;
#else
        bool need_adlh = false;
#endif
        bool need_nvmlh = false;

        // Scan devices collection to identify which hw monitors to initialize
        for (auto it = m_DevicesCollection.begin(); it != m_DevicesCollection.end(); it++)
        {
            if (it->second.subscriptionType == DeviceSubscriptionTypeEnum::Cuda)
            {
                need_nvmlh = true;
                continue;
            }
            if (it->second.subscriptionType == DeviceSubscriptionTypeEnum::OpenCL)
            {
                if (it->second.clPlatformType == ClPlatformTypeEnum::Nvidia)
                {
                    need_nvmlh = true;
                    continue;
                }
                if (it->second.clPlatformType == ClPlatformTypeEnum::Amd)
                {
#if defined(__linux)
                    need_sysfsh = true;
#else
                    need_adlh = true;
#endif
                    continue;
                }
            }
        }

#if defined(__linux)
        if (need_sysfsh)
            sysfsh = wrap_amdsysfs_create();
        if (sysfsh)
        {
            // Build Pci identification mapping as done in miners.
            for (int i = 0; i < sysfsh->sysfs_gpucount; i++)
            {
                std::ostringstream oss;
                std::string uniqueId;
                oss << std::setfill('0') << std::setw(2) << std::hex
                    << (unsigned int)sysfsh->sysfs_pci_bus_id[i] << ":" << std::setw(2)
                    << (unsigned int)(sysfsh->sysfs_pci_device_id[i]) << ".0";
                uniqueId = oss.str();
                map_amdsysfs_handle[uniqueId] = i;
            }
        }

#else
        if (need_adlh)
            adlh = wrap_adl_create();
        if (adlh)
        {
            // Build Pci identification as done in miners.
            for (int i = 0; i < adlh->adl_gpucount; i++)
            {
                std::ostringstream oss;
                std::string uniqueId;
                oss << std::setfill('0') << std::setw(2) << std::hex
                    << (unsigned int)adlh->devs[adlh->phys_logi_device_id[i]].iBusNumber << ":"
                    << std::setw(2)
                    << (unsigned int)(adlh->devs[adlh->phys_logi_device_id[i]].iDeviceNumber)
                    << ".0";
                uniqueId = oss.str();
                map_adl_handle[uniqueId] = i;
            }
        }

#endif
        if (need_nvmlh)
            nvmlh = wrap_nvml_create();
        if (nvmlh)
        {
            // Build Pci identification as done in miners.
            for (int i = 0; i < nvmlh->nvml_gpucount; i++)
            {
                std::ostringstream oss;
                std::string uniqueId;
                oss << std::setfill('0') << std::setw(2) << std::hex
                    << (unsigned int)nvmlh->nvml_pci_bus_id[i] << ":" << std::setw(2)
                    << (unsigned int)(nvmlh->nvml_pci_device_id[i] >> 3) << ".0";
                uniqueId = oss.str();
                map_nvml_handle[uniqueId] = i;
            }
        }
    }

    // Initialize nonce_scrambler
    shuffle();

    // Start data collector timer
    // It should work for the whole lifetime of Farm
    // regardless it's mining state
    m_collectTimer.expires_from_now(boost::posix_time::milliseconds(m_collectInterval));
    m_collectTimer.async_wait(
        m_io_strand.wrap(boost::bind(&Farm::collectData, this, boost::asio::placeholders::error)));
}

Farm::~Farm()
{
    // Stop data collector (before monitors !!!)
    m_collectTimer.cancel();

    // Deinit HWMON
#if defined(__linux)
    if (sysfsh)
        wrap_amdsysfs_destroy(sysfsh);
#else
    if (adlh)
        wrap_adl_destroy(adlh);
#endif
    if (nvmlh)
        wrap_nvml_destroy(nvmlh);

    // Stop mining (if needed)
    if (m_isMining.load(std::memory_order_relaxed))
        stop();
}

/**
 * @brief Randomizes the nonce scrambler
 */
void Farm::shuffle()
{
    // Given that all nonces are equally likely to solve the problem
    // we could reasonably always start the nonce search ranges
    // at a fixed place, but that would be boring. Provide a once
    // per run randomized start place, without creating much overhead.
    random_device engine;
    m_nonce_scrambler = uniform_int_distribution<uint64_t>()(engine);
}

void Farm::setBenchmark(bool optimize){
    optimizeHash = optimize;
}
   

void Farm::plot(bool isHash)
{
    if (isHash){
        Gnuplot gp("gnuplot -persist");
        gp << "set title \"Hash Benchmark\" font \",12\"\n";
        gp << "set xlabel \"Iteration\" font \",12\"\n";
        gp << "set ylabel \"Hash Rate (h)\" font \",12\"\n";
        gp << "set grid\n";
        gp << "plot for[in=0:4] \"plot.dat\" i in u 1:2 w lp pt 7 t columnheader(1)\n";
        gp << "set terminal pngcairo enhanced font \"Times New Roman,12.0\" size 1500,1100\n";
        gp << "--set output \"hashBenchmark.png\"\n";
        gp << "replot\n";
    }
    else{
        Gnuplot gp("gnuplot -persist");
        gp << "set title \"Profit Benchmark\" font \",12\"\n";
        gp << "set xlabel \"Iteration\" font \",12\"\n";
        gp << "set ylabel \"Profit per Hour (CAD/hr)\" font \",12\"\n";
        gp << "set grid\n";
        gp << "plot for[in=5:9] \"plot.dat\" i in u 1:4 w lp pt 7 t columnheader(1)\n";
        gp << "set terminal pngcairo enhanced font \"Times New Roman,12.0\" size 1500,1100\n";
        gp << "--set output \"profitBenchmark.png\"\n";
        gp << "replot\n";
    }
}



void Farm::setWork(WorkPackage const& _newWp)
{
    // Set work to each miner giving it's own starting nonce
    Guard l(x_minerWork);

    // Retrieve appropriate EpochContext
    if (m_currentWp.epoch != _newWp.epoch)
    {
        ethash::epoch_context _ec = ethash::get_global_epoch_context(_newWp.epoch);
        m_currentEc.epochNumber = _newWp.epoch;
        m_currentEc.lightNumItems = _ec.light_cache_num_items;
        m_currentEc.lightSize = ethash::get_light_cache_size(_ec.light_cache_num_items);
        m_currentEc.dagNumItems = ethash::calculate_full_dataset_num_items(_newWp.epoch);
        m_currentEc.dagSize = ethash::get_full_dataset_size(m_currentEc.dagNumItems);
        m_currentEc.lightCache = _ec.light_cache;

        for (auto const& miner : m_miners)
            miner->setEpoch(m_currentEc);
    }

    m_currentWp = _newWp;

    // Check if we need to shuffle per work (ergodicity == 2)
    if (m_Settings.ergodicity == 2 && m_currentWp.exSizeBytes == 0)
        shuffle();

    uint64_t _startNonce;
    if (m_currentWp.exSizeBytes > 0)
    {
        // Equally divide the residual segment among miners
        _startNonce = m_currentWp.startNonce;
        m_nonce_segment_with =
            (unsigned int)log2(pow(2, 64 - (m_currentWp.exSizeBytes * 4)) / m_miners.size());
    }
    else
    {
        // Get the randomly selected nonce
        _startNonce = m_nonce_scrambler;
    }

    for (unsigned int i = 0; i < m_miners.size(); i++)
    {
        m_currentWp.startNonce = _startNonce + ((uint64_t)i << m_nonce_segment_with);
        m_miners.at(i)->setWork(m_currentWp);
    }
}

/**
 * @brief Start a number of miners.
 */
bool Farm::start()
{
    // Prevent recursion
    if (m_isMining.load(std::memory_order_relaxed))
        return true;

    Guard l(x_minerWork);

    // Start all subscribed miners if none yet
    if (!m_miners.size())
    {
        for (auto it = m_DevicesCollection.begin(); it != m_DevicesCollection.end(); it++)
        {
            TelemetryAccountType minerTelemetry;
#if ETH_ETHASHCUDA
            if (it->second.subscriptionType == DeviceSubscriptionTypeEnum::Cuda)
            {
                minerTelemetry.prefix = "cu";
                m_miners.push_back(std::shared_ptr<Miner>(
                    new CUDAMiner(m_miners.size(), m_CUSettings, it->second)));
            }
#endif
#if ETH_ETHASHCL

            if (it->second.subscriptionType == DeviceSubscriptionTypeEnum::OpenCL)
            {
                minerTelemetry.prefix = "cl";
                m_miners.push_back(std::shared_ptr<Miner>(
                    new CLMiner(m_miners.size(), m_CLSettings, it->second)));
            }
#endif
#if ETH_ETHASHCPU

            if (it->second.subscriptionType == DeviceSubscriptionTypeEnum::Cpu)
            {
                minerTelemetry.prefix = "cp";
                m_miners.push_back(std::shared_ptr<Miner>(
                    new CPUMiner(m_miners.size(), m_CPSettings, it->second)));
            }
#endif
            if (minerTelemetry.prefix.empty())
                continue;
            m_telemetry.miners.push_back(minerTelemetry);
            m_miners.back()->startWorking();
        }

        // Initialize DAG Load mode
        Miner::setDagLoadInfo(m_Settings.dagLoadMode, (unsigned int)m_miners.size());

        m_isMining.store(true, std::memory_order_relaxed);
    }
    else
    {
        for (auto const& miner : m_miners)
            miner->startWorking();
        m_isMining.store(true, std::memory_order_relaxed);
    }

    return m_isMining.load(std::memory_order_relaxed);
}

/**
 * @brief Stop all mining activities.
 */
void Farm::stop()
{
    // Avoid re-entering if not actually mining.
    // This, in fact, is also called by destructor
    if (isMining())
    {
        {
            Guard l(x_minerWork);
            for (auto const& miner : m_miners)
            {
                miner->triggerStopWorking();
                miner->kick_miner();
            }

            m_miners.clear();
            m_isMining.store(false, std::memory_order_relaxed);
        }
    }
}

/**
 * @brief Pauses the whole collection of miners
 */
void Farm::pause()
{
    // Signal each miner to suspend mining
    Guard l(x_minerWork);
    m_paused.store(true, std::memory_order_relaxed);
    for (auto const& m : m_miners)
        m->pause(MinerPauseEnum::PauseDueToFarmPaused);
}

/**
 * @brief Returns whether or not this farm is paused for any reason
 */
bool Farm::paused()
{
    return m_paused.load(std::memory_order_relaxed);
}

/**
 * @brief Resumes from a pause condition
 */
void Farm::resume()
{
    // Signal each miner to resume mining
    // Note ! Miners may stay suspended if other reasons
    Guard l(x_minerWork);
    m_paused.store(false, std::memory_order_relaxed);
    for (auto const& m : m_miners)
        m->resume(MinerPauseEnum::PauseDueToFarmPaused);
}

/**
 * @brief Stop all mining activities and Starts them again
 */
void Farm::restart()
{
    if (m_onMinerRestart)
        m_onMinerRestart();
}

/**
 * @brief Stop all mining activities and Starts them again (async post)
 */
void Farm::restart_async()
{
    g_io_service.post(m_io_strand.wrap(boost::bind(&Farm::restart, this)));
}

/**
 * @brief Spawn a reboot script (reboot.bat/reboot.sh)
 * @return false if no matching file was found
 */
bool Farm::reboot(const std::vector<std::string>& args)
{
#if defined(_WIN32)
    const char* filename = "reboot.bat";
#else
    const char* filename = "reboot.sh";
#endif

    return spawn_file_in_bin_dir(filename, args);
}

/**
 * @brief Account solutions for miner and for farm
 */
void Farm::accountSolution(unsigned _minerIdx, SolutionAccountingEnum _accounting)
{
    if (_accounting == SolutionAccountingEnum::Accepted)
    {
        m_telemetry.farm.solutions.accepted++;
        m_telemetry.farm.solutions.tstamp = std::chrono::steady_clock::now();
        m_telemetry.miners.at(_minerIdx).solutions.accepted++;
        m_telemetry.miners.at(_minerIdx).solutions.tstamp = std::chrono::steady_clock::now();
        return;
    }
    if (_accounting == SolutionAccountingEnum::Wasted)
    {
        m_telemetry.farm.solutions.wasted++;
        m_telemetry.farm.solutions.tstamp = std::chrono::steady_clock::now();
        m_telemetry.miners.at(_minerIdx).solutions.wasted++;
        m_telemetry.miners.at(_minerIdx).solutions.tstamp = std::chrono::steady_clock::now();
        return;
    }
    if (_accounting == SolutionAccountingEnum::Rejected)
    {
        m_telemetry.farm.solutions.rejected++;
        m_telemetry.farm.solutions.tstamp = std::chrono::steady_clock::now();
        m_telemetry.miners.at(_minerIdx).solutions.rejected++;
        m_telemetry.miners.at(_minerIdx).solutions.tstamp = std::chrono::steady_clock::now();
        return;
    }
    if (_accounting == SolutionAccountingEnum::Failed)
    {
        m_telemetry.farm.solutions.failed++;
        m_telemetry.farm.solutions.tstamp = std::chrono::steady_clock::now();
        m_telemetry.miners.at(_minerIdx).solutions.failed++;
        m_telemetry.miners.at(_minerIdx).solutions.tstamp = std::chrono::steady_clock::now();
        return;
    }
}

/**
 * @brief Gets the solutions account for the whole farm
 */

SolutionAccountType Farm::getSolutions()
{
    return m_telemetry.farm.solutions;
}

/**
 * @brief Gets the solutions account for single miner
 */
SolutionAccountType Farm::getSolutions(unsigned _minerIdx)
{
    try
    {
        return m_telemetry.miners.at(_minerIdx).solutions;
    }
    catch (const std::exception&)
    {
        return SolutionAccountType();
    }
}

/**
 * @brief Provides the description of segments each miner is working on
 * @return a JsonObject
 */
Json::Value Farm::get_nonce_scrambler_json()
{
    Json::Value jRes;
    jRes["start_nonce"] = toHex(m_nonce_scrambler, HexPrefix::Add);
    jRes["device_width"] = m_nonce_segment_with;
    jRes["device_count"] = (uint64_t)m_miners.size();

    return jRes;
}

void Farm::setTStartTStop(unsigned tstart, unsigned tstop)
{
    m_Settings.tempStart = tstart;
    m_Settings.tempStop = tstop;
}

void Farm::submitProof(Solution const& _s)
{
    g_io_service.post(m_io_strand.wrap(boost::bind(&Farm::submitProofAsync, this, _s)));
}

void Farm::submitProofAsync(Solution const& _s)
{
#ifdef DEV_BUILD
    const bool dbuild = true;
#else
    const bool dbuild = false;
#endif
    if (!m_Settings.noEval || dbuild)
    {
        Result r = EthashAux::eval(_s.work.epoch, _s.work.block, _s.work.header, _s.nonce);
        if (r.value > _s.work.get_boundary())
        {
            accountSolution(_s.midx, SolutionAccountingEnum::Failed);
            cwarn << "GPU " << _s.midx
                  << " gave incorrect result. Lower overclocking values if it happens frequently.";
            return;
        }
        if (dbuild && (_s.mixHash != r.mixHash))
            cwarn << "GPU " << _s.midx << " mix missmatch";
        m_onSolutionFound(Solution{_s.nonce, r.mixHash, _s.work, _s.tstamp, _s.midx});
    }
    else
        m_onSolutionFound(_s);

#ifdef DEV_BUILD
    if (g_logOptions & LOG_SUBMIT)
        cnote << "Submit time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - _s.tstamp)
                     .count()
              << " us.";
#endif
}

// Collects data about hashing and hardware status
void Farm::collectData(const boost::system::error_code& ec)
{
    if (ec)
        return;

    // Reset hashrate (it will accumulate from miners)
    float farm_hr = 0.0f;

    // Process miners
    for (auto const& miner : m_miners)
    {
        int minerIdx = miner->Index();
        float hr = (miner->paused() ? 0.0f : miner->RetrieveHashRate());
        farm_hr += hr;
        m_telemetry.miners.at(minerIdx).hashrate = hr;
        m_telemetry.miners.at(minerIdx).paused = miner->paused();


        if (m_Settings.hwMon)
        {
            HwMonitorInfo hwInfo = miner->hwmonInfo();

            unsigned int tempC = 0, fanpcnt = 0, powerW = 0;

            if (hwInfo.deviceType == HwMonitorInfoType::NVIDIA && nvmlh)
            {
                int devIdx = hwInfo.deviceIndex;
                if (devIdx == -1 && !hwInfo.devicePciId.empty())
                {
                    if (map_nvml_handle.find(hwInfo.devicePciId) != map_nvml_handle.end())
                    {
                        devIdx = map_nvml_handle[hwInfo.devicePciId];
                        miner->setHwmonDeviceIndex(devIdx);
                    }
                    else
                    {
                        // This will prevent further tries to map
                        miner->setHwmonDeviceIndex(-2);
                    }
                }

                if (devIdx >= 0)
                {
                    wrap_nvml_get_tempC(nvmlh, devIdx, &tempC);
                    wrap_nvml_get_fanpcnt(nvmlh, devIdx, &fanpcnt);

                    if (m_Settings.hwMon == 2)
                        wrap_nvml_get_power_usage(nvmlh, devIdx, &powerW);
                }
            }
            else if (hwInfo.deviceType == HwMonitorInfoType::AMD)
            {
#if defined(__linux)
                if (sysfsh)
                {
                    int devIdx = hwInfo.deviceIndex;
                    if (devIdx == -1 && !hwInfo.devicePciId.empty())
                    {
                        if (map_amdsysfs_handle.find(hwInfo.devicePciId) !=
                            map_amdsysfs_handle.end())
                        {
                            devIdx = map_amdsysfs_handle[hwInfo.devicePciId];
                            miner->setHwmonDeviceIndex(devIdx);
                        }
                        else
                        {
                            // This will prevent further tries to map
                            miner->setHwmonDeviceIndex(-2);
                        }
                    }

                    if (devIdx >= 0)
                    {
                        wrap_amdsysfs_get_tempC(sysfsh, devIdx, &tempC);
                        wrap_amdsysfs_get_fanpcnt(sysfsh, devIdx, &fanpcnt);

                        if (m_Settings.hwMon == 2)
                            wrap_amdsysfs_get_power_usage(sysfsh, devIdx, &powerW);
                    }
                }
#else
                if (adlh)  // Windows only for AMD
                {
                    int devIdx = hwInfo.deviceIndex;
                    if (devIdx == -1 && !hwInfo.devicePciId.empty())
                    {
                        if (map_adl_handle.find(hwInfo.devicePciId) != map_adl_handle.end())
                        {
                            devIdx = map_adl_handle[hwInfo.devicePciId];
                            miner->setHwmonDeviceIndex(devIdx);
                        }
                        else
                        {
                            // This will prevent further tries to map
                            miner->setHwmonDeviceIndex(-2);
                        }
                    }

                    if (devIdx >= 0)
                    {
                        wrap_adl_get_tempC(adlh, devIdx, &tempC);
                        wrap_adl_get_fanpcnt(adlh, devIdx, &fanpcnt);

                        if (m_Settings.hwMon == 2)
                            wrap_adl_get_power_usage(adlh, devIdx, &powerW);
                    }
                }
#endif
            }


            // If temperature control has been enabled call
            // check threshold
            if (m_Settings.tempStop)
            {
                bool paused = miner->pauseTest(MinerPauseEnum::PauseDueToOverHeating);
                if (!paused && (tempC >= m_Settings.tempStop))
                    miner->pause(MinerPauseEnum::PauseDueToOverHeating);
                if (paused && (tempC <= m_Settings.tempStart))
                    miner->resume(MinerPauseEnum::PauseDueToOverHeating);
            }

            m_telemetry.miners.at(minerIdx).sensors.tempC = tempC;
            m_telemetry.miners.at(minerIdx).sensors.fanP = fanpcnt;
            m_telemetry.miners.at(minerIdx).sensors.powerW = powerW / ((double)1000.0);
        }
        static string suffixes[] = {"h", "Kh", "Mh", "Gh"};
        m_telemetry.farm.hashrate = farm_hr;
        if(optimizeHash){if ((hr != 0.0f))
            {
                counter += 1;
                ofstream myfile;
                switch(counter) {
                    case 1:
                        // append = false;
                        // clock +0
                        system("..\\..\\..\\..\\ps\\msiProfile1.bat");
                        overclock = 0;
                        break;
                    case 11:
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << "\"0MHz OC\"" << "\n";
                        plotData.close();
                        append = true;
                        avgHr = 0;
                        avgPower = 0;
                        break;
                    case 21:
                        append = false;
                        // Compare
                        if (avgHr > bestHash){
                            bestHash = avgHr;
                            clockChoice = "1";
                        }
                        avgHr = 0;
                        avgPower = 0;
                        // clock +200
                        system("..\\..\\..\\..\\ps\\msiProfile2.bat");
                        overclock = 200;
                        break;
                    case 31:
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << "\n" << "\n" <<  "\"200MHz OC\"" << "\n";
                        plotData.close();
                        append = true;
                        avgHr = 0;
                        avgPower = 0;
                        break;
                    case 41:
                        append = false;
                        if (avgHr > bestHash){
                            bestHash = avgHr;
                            clockChoice = "2";
                        }
                        avgHr = 0;
                        avgPower = 0;
                        // clock +300
                        system("..\\..\\..\\..\\ps\\msiProfile3.bat");
                        overclock = 300;
                        break;
                    case 51:
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << "\n" << "\n" <<  "\"300MHz OC\"" << "\n";
                        plotData.close();
                        append = true;
                        avgHr = 0;
                        avgPower = 0;
                        break;
                    case 61:
                        append = false;
                        if (avgHr > bestHash){
                            bestHash = avgHr;
                            clockChoice = "3";
                        }
                        avgHr = 0;
                        avgPower = 0;
                        // clock +400
                        system("..\\..\\..\\..\\ps\\msiProfile4.bat");
                        overclock = 400;
                        break;
                    case 71:
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << "\n" << "\n" <<  "\"400MHz OC\"" << "\n";
                        plotData.close();
                        append = true;
                        avgHr = 0;
                        avgPower = 0;
                        break;
                    case 81:
                        append = false;
                        if (avgHr > bestHash){
                            bestHash = avgHr;
                            clockChoice = "4";
                        }
                        avgHr = 0;
                        avgPower = 0;
                        // clock +500
                        system("..\\..\\..\\..\\ps\\msiProfile5.bat");
                        overclock = 500;
                        break;
                    case 91:
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << "\n" << "\n" <<  "\"500MHz OC\"" << "\n";
                        plotData.close();
                        append = true;
                        avgHr = 0;
                        avgPower = 0;
                        break;
                    case 101:
                        append = false;
                        if (avgHr > bestHash){
                            bestHash = avgHr;
                            clockChoice = "5";
                        }
                        // set clock perm "msiProfile"+str(clockChoice)+".bat"
                        overclock = ocConversion[stoi(clockChoice)-1];
                        system(("..\\..\\..\\..\\ps\\msiProfile"+clockChoice+".bat").c_str());
                        avgHr = 0;
                        avgPower = 0;
                        // Power 90%
                        system("..\\..\\..\\..\\ps\\pl90.bat");
                        powerOut = "90";
                        break;
                    case 111:
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << "\n" << "\n\"" << overclock << " MHz" << "  @ 90% Power\""  << "\n";
                        plotData.close();
                        append = true;
                        avgHr = 0;
                        avgPower = 0;
                        break;
                    case 121:
                        append = false;
                        if (avgProfit > bestProfit){
                            bestProfit = avgProfit;
                            powerChoice = "90";
                        }
                        avgHr = 0;
                        avgPower = 0;
                        // Power 80%
                        system("..\\..\\..\\..\\ps\\pl80.bat");
                        powerOut = "80";
                        break;
                    case 131:
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << "\n" << "\n\"" << overclock << " MHz" << "  @ 80% Power\""  << "\n";
                        plotData.close();
                        append = true;
                        avgHr = 0;
                        avgPower = 0;
                        break;
                    case 141:
                        append = false;
                        if (avgProfit > bestProfit){
                            bestProfit = avgProfit;
                            powerChoice = "80";
                        }
                        avgHr = 0;
                        avgPower = 0;
                        // Power 70%
                        system("..\\..\\..\\..\\ps\\pl70.bat");
                        powerOut = "70";
                        break;
                    case 151:
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << "\n" << "\n\"" << overclock << " MHz" << "  @ 70% Power\""  << "\n";
                        plotData.close();
                        append = true;
                        avgHr = 0;
                        avgPower = 0;
                        break;
                    case 161:
                        append = false;
                        if (avgProfit > bestProfit){
                            bestProfit = avgProfit;
                            powerChoice = "70";
                        }
                        avgHr = 0;
                        // Power 60%
                        system("..\\..\\..\\..\\ps\\pl60.bat");
                        powerOut = "60";
                        break;
                    case 171:
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << "\n" << "\n\"" << overclock << " MHz" << "  @ 60% Power\""  << "\n";
                        plotData.close();
                        append = true;
                        avgHr = 0;
                        avgPower = 0;
                        break;
                    case 181:
                        append = false;
                        if (avgProfit > bestProfit){
                            bestProfit = avgProfit;
                            powerChoice = "60";
                        }
                        avgHr = 0;
                        avgPower = 0;
                        // Power 50%
                        system("..\\..\\..\\..\\ps\\pl50.bat");
                        powerOut = "50";
                        break;
                    case 191:
                        
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << "\n" << "\n\"" << overclock << " MHz" << "  @ 50% Power\""  << "\n";
                        plotData.close();
                        append = true;
                        avgHr = 0;
                        avgPower = 0;
                        break;
                    case 201:
                        append = false;
                        if (avgProfit > bestProfit){
                            bestProfit = avgProfit;
                            powerChoice = "50";
                        }
                        avgHr = 0;
                        avgPower = 0;                        
                        system(("..\\..\\..\\..\\ps\\pl"+powerChoice+".bat").c_str());
                        
                       
                        char cChoice;
                        char pChoice;

                        myfile.open("benchmark.txt");
                        for(int i = 0; i < clockChoice.length(); i++){
                            cChoice = clockChoice[i] + 100;
                            myfile << cChoice;
                        }
                        myfile << "\n";
                        for(int i = 0; i < powerChoice.length(); i++){
                            pChoice = powerChoice[i] + 100;
                            myfile << pChoice;
                        }
                        
                        //myfile << clockChoice + "\n" + powerChoice;
                        //myfile << cChoice + "\n" + pChoice;
                        myfile.close();
                        plot(true);
                        plot(false);
                        setBenchmark(false);
                        break;
                    }

                if (counter % 10 != 0)
                {   
                    avgHr = (avgHr*(counter % 10 - 1) + m_telemetry.miners.at(minerIdx).hashrate) / (counter % 10);
                    avgPower = (avgPower*(counter % 10 - 1) + m_telemetry.miners.at(minerIdx).sensors.powerW) / (counter % 10);
                    avgProfit = (avgHr/stockHashrate)*(stockRVNperHour)*RVNvalue - (avgPower/1000*kWhCost);
                    if (append == true){
                        plotData.open("plot.dat", std::ofstream::out | std::ofstream::app);
                        plotData << ((counter % 10)) << "\t" << avgHr << "\t"  << avgPower << "\t"  << avgProfit << "\n";
                        plotData.close();
                    }
                }

                magnitude = 0;
                double hrCopy = avgHr;
                while (hrCopy > 1000.0f && magnitude <= 3)
                {
                    hrCopy /= 1000.0f;
                    magnitude++;
                }
                std::cout << std::setfill(' ') << std::setw(15) << EthGray << std::fixed << "Memory Overclock: "
                << EthReset << EthBlueBold << overclock << " MHz " << EthReset << EthGray << "Power Limit: " 
                << EthReset << EthYellowBold << powerOut << "% " << std::endl;
                std::cout << std::setfill(' ') << std::setw(14) << EthReset << EthGray << std::fixed << "Average Hash Rate: " 
                << EthReset << EthCyanBold << std::setprecision(2) << hrCopy
                << suffixes[magnitude] << EthReset << EthGray << "  Average Power: " << EthReset << EthRedBold 
                << std::setfill(' ') << std::setw(6) << avgPower << "W " << EthReset << EthGray << "  Average Profit: " 
                << EthReset << EthGreenBold << std::setprecision(6) << avgProfit << "CAD/hour" << EthReset << std::endl;
                
            }
        }        
        
        miner->TriggerHashRateUpdate();
    }

    // Resubmit timer for another loop
    m_collectTimer.expires_from_now(boost::posix_time::milliseconds(m_collectInterval));
    m_collectTimer.async_wait(
        m_io_strand.wrap(boost::bind(&Farm::collectData, this, boost::asio::placeholders::error)));
}

bool Farm::spawn_file_in_bin_dir(const char* filename, const std::vector<std::string>& args)
{
    std::string fn = boost::dll::program_location().parent_path().string() +
                     "/" +  // boost::filesystem::path::preferred_separator
                     filename;
    try
    {
        if (!boost::filesystem::exists(fn))
            return false;

        /* anything in the file */
        if (!boost::filesystem::file_size(fn))
            return false;

#if defined(__linux)
        struct stat sb;
        if (stat(fn.c_str(), &sb) != 0)
            return false;
        /* just check if any exec flag is set.
           still execution can fail (not the uid, not in the group, selinux, ...)
         */
        if ((sb.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
            return false;
#endif
        /* spawn it (no wait,...) - fire and forget! */
        boost::process::spawn(fn, args);
        return true;
    }
    catch (...)
    {
    }
    return false;
}


}  // namespace eth
}  // namespace dev
