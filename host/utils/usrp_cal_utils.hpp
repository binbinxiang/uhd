//
// Copyright 2010 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <uhd/utils/paths.hpp>
#include <uhd/property_tree.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/usrp/dboard_eeprom.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <vector>
#include <complex>
#include <cmath>
#include <fstream>

namespace fs = boost::filesystem;

struct result_t{double freq, real_corr, imag_corr, best, delta;};

/***********************************************************************
 * Constants
 **********************************************************************/
static const double tau = 6.28318531;
static const double alpha = 0.0001; //very tight iir filter
static const size_t wave_table_len = 8192;
static const size_t num_search_steps = 5;
static const size_t num_search_iters = 7;
static const size_t skip_initial_samps = 20;
static const double default_freq_step = 7.3e6;
static const size_t default_num_samps = 10000;

/***********************************************************************
 * Determine gain settings
 **********************************************************************/
static inline void set_optimum_gain(uhd::usrp::multi_usrp::sptr usrp){
    uhd::property_tree::sptr tree = usrp->get_device()->get_tree();
    const uhd::fs_path tx_fe_path = "/mboards/0/dboards/A/tx_frontends/0";
    const std::string tx_name = tree->access<std::string>(tx_fe_path / "name").get();
    if (tx_name.find("WBX") != std::string::npos or tx_name.find("SBX") != std::string::npos){
        usrp->set_tx_gain(0);
    }
    else{
        throw std::runtime_error("self-calibration is not supported for this hardware");
    }

    const uhd::fs_path rx_fe_path = "/mboards/0/dboards/A/tx_frontends/0";
    const std::string rx_name = tree->access<std::string>(rx_fe_path / "name").get();
    if (rx_name.find("WBX") != std::string::npos or rx_name.find("SBX") != std::string::npos){
        usrp->set_rx_gain(25);
    }
    else{
        throw std::runtime_error("self-calibration is not supported for this hardware");
    }

}

/***********************************************************************
 * Sinusoid wave table
 **********************************************************************/
static inline std::vector<std::complex<float> > gen_table(void){
    std::vector<std::complex<float> > wave_table(wave_table_len);
    for (size_t i = 0; i < wave_table_len; i++){
        wave_table[i] = std::polar<float>(1.0, (tau*i)/wave_table_len);
    }
    return wave_table;
}

static inline std::complex<float> wave_table_lookup(const size_t index){
    static const std::vector<std::complex<float> > wave_table = gen_table();
    return wave_table[index % wave_table_len];
}

/***********************************************************************
 * Compute power of a tone
 **********************************************************************/
static inline double compute_tone_dbrms(
    const std::vector<std::complex<float> > &samples,
    const double freq //freq is fractional
){
    //shift the samples so the tone at freq is down at DC
    std::vector<std::complex<double> > shifted(samples.size() - skip_initial_samps);
    for (size_t i = 0; i < shifted.size(); i++){
        shifted[i] = std::complex<double>(samples[i+skip_initial_samps]) * std::polar<double>(1.0, -freq*tau*i);
    }

    //filter the samples with a narrow low pass
    std::complex<double> iir_output = 0, iir_last = 0;
    double output = 0;
    for (size_t i = 0; i < shifted.size(); i++){
        iir_output = alpha * shifted[i] + (1-alpha)*iir_last;
        iir_last = iir_output;
        output += std::abs(iir_output);
    }

    return 20*std::log10(output/shifted.size());
}

/***********************************************************************
 * Write a dat file
 **********************************************************************/
static inline void write_samples_to_file(
    const std::vector<std::complex<float> > &samples, const std::string &file
){
    std::ofstream outfile(file.c_str(), std::ofstream::binary);
    outfile.write((const char*)&samples.front(), samples.size()*sizeof(std::complex<float>));
    outfile.close();
}

/***********************************************************************
 * Store data to file
 **********************************************************************/
static void store_results(
    uhd::usrp::multi_usrp::sptr usrp,
    const std::vector<result_t> &results,
    const std::string &XX,
    const std::string &xx,
    const std::string &what
){
    //extract eeprom serial
    uhd::property_tree::sptr tree = usrp->get_device()->get_tree();
    const uhd::fs_path db_path = "/mboards/0/dboards/A/" + xx + "_eeprom";
    const uhd::usrp::dboard_eeprom_t db_eeprom = tree->access<uhd::usrp::dboard_eeprom_t>(db_path).get();
    if (db_eeprom.serial.empty()) throw std::runtime_error(XX + " dboard has empty serial!");

    //make the calibration file path
    fs::path cal_data_path = fs::path(uhd::get_app_path()) / ".uhd";
    fs::create_directory(cal_data_path);
    cal_data_path = cal_data_path / "cal";
    fs::create_directory(cal_data_path);
    cal_data_path = cal_data_path / str(boost::format("%s_%s_cal_v0.1_%s.csv") % xx % what % db_eeprom.serial);
    if (fs::exists(cal_data_path)){
        fs::rename(cal_data_path, cal_data_path.string() + str(boost::format(".%d") % time(NULL)));
    }

    //fill the calibration file
    std::ofstream cal_data(cal_data_path.string().c_str());
    cal_data << boost::format("name, %s Frontend Calibration\n") % XX;
    cal_data << boost::format("serial, %s\n") % db_eeprom.serial;
    cal_data << boost::format("timestamp, %d\n") % time(NULL);
    cal_data << boost::format("version, 0, 1\n");
    cal_data << boost::format("DATA STARTS HERE\n");
    cal_data << "lo_frequency, correction_real, correction_imag, measured, delta\n";

    for (size_t i = 0; i < results.size(); i++){
        cal_data
            << results[i].freq << ", "
            << results[i].real_corr << ", "
            << results[i].imag_corr << ", "
            << results[i].best << ", "
            << results[i].delta << "\n"
        ;
    }

    std::cout << "wrote cal data to " << cal_data_path << std::endl;
}
