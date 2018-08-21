// This is to evaluate the odometry sequences from kitti. Unlike the
// raw scans, they come pre-corrected for motion distortion

#include "../include/kitti_utility_methods.hpp"

int main(int argc, char **argv) {
    if (argc != 5) {
        throw std::runtime_error(
                "Must be run with only 3 arguments: \n 1. Full path to data \n 2. Full path to configuration files \n 3. 1 "
                "or 0 to indicate whether a visualizer should be run \n 4. Sequence to run.");
    }
    std::string data_path(argv[1]);
    std::string config_path(argv[2]);
    bool run_viz = std::stoi(argv[3]) == 1;
    std::string sequence(argv[4]);
    if (data_path.back() != '/') {
        data_path = data_path + '/';
    }
    if (config_path.back() != '/') {
        config_path = config_path + '/';
    }

    wave::ConfigParser parser;
    wave::MatX cutoff_angles;
    parser.addParam("cutoff_angles", &cutoff_angles);
    parser.load(config_path + "kitti_angles.yaml");

    wave::LaserOdomParams params;
    wave::FeatureExtractorParams feature_params;
    loadFeatureParams(config_path, "features.yaml", feature_params);
    setupFeatureParameters(feature_params);
    LoadParameters(config_path, "odom.yaml", params);
    params.n_ring = 64;
    wave::TransformerParams transformer_params;
    transformer_params.traj_resolution = params.num_trajectory_states;
    transformer_params.delRTol = 100.0f;
    transformer_params.delVTol = 100.0f;
    transformer_params.delWTol = 100.0f;

    wave::LaserOdom odom(params, feature_params, transformer_params);
    wave::PointCloudDisplay *display;

    if (run_viz) {
        display = new wave::PointCloudDisplay("Kitti Eval", 0.2, 3, 2);
        display->startSpin();
    } else {
        display = nullptr;
    }

    std::string gtpath = data_path + "poses/" + sequence + ".txt";
    data_path = data_path + "sequences/" + sequence + "/";
    std::string calibration_path = data_path + "calib.txt";

    // set up pointcloud iterators
    boost::filesystem::path p(data_path + "velodyne");
    std::vector<boost::filesystem::path> v;
    std::copy(boost::filesystem::directory_iterator(p), boost::filesystem::directory_iterator(), std::back_inserter(v));
    std::sort(v.begin(), v.end());

    // timesteps
    fstream timestamps(data_path + "times.txt", ios::in);

    if (!timestamps.good()) {
        throw std::runtime_error("file structure not as expected for timestamps");
    }

    std::vector<wave::TimeType> times;
    std::string cur_stamp;
    while (std::getline(timestamps, cur_stamp)) {
        std::stringstream ss(cur_stamp);
        double seconds;
        ss >> seconds;
        wave::TimeType new_stamp;
        auto micros = static_cast<uint64_t>(seconds * 1e6);
        new_stamp = new_stamp + std::chrono::microseconds(micros);
        times.emplace_back(new_stamp);
    }

    fstream ground_truth_file(gtpath, ios::in);
    fstream calibration_file(calibration_path, ios::in);

    if (!calibration_file.good()) {
        throw std::runtime_error("Unable to find calibration");
    }

    wave::Transformation<> T_CAM_LIDAR;

    {
        std::string data_input;
        for (uint32_t i = 0; i < 5; ++i) {
            std::getline(calibration_file, data_input);
        }
        std::stringstream ss(data_input);
        std::string junk;
        ss >> junk;
        std::vector<double> vals;
        while (ss.rdbuf()->in_avail() > 0) {
            double new_val;
            ss >> new_val;
            vals.emplace_back(new_val);
        }
        Eigen::Map<Eigen::Matrix<double, 3, 4, Eigen::RowMajor>> raw_mat(vals.data());
        T_CAM_LIDAR = wave::Transformation<decltype(raw_mat), false>(raw_mat);
    }

    wave::VecE<wave::PoseStamped> T_L1_Lx_trajectory;

    wave::Transformation<> T_L1_O;

    if (!ground_truth_file.good()) {
        LOG_INFO("Ground truth not available");
    } else {
        std::string line_string;
        T_L1_Lx_trajectory.resize(times.size());
        for (uint32_t frame_id = 0; frame_id < times.size(); ++frame_id) {
            std::getline(ground_truth_file, line_string);
            std::stringstream ss(line_string);
            std::vector<double> vals;
            while (ss.rdbuf()->in_avail() > 0) {
                double new_val;
                ss >> new_val;
                vals.emplace_back(new_val);
            }
            Eigen::Map<Eigen::Matrix<double, 3, 4, Eigen::RowMajor>> raw_mat(vals.data());
            wave::Transformation<> T_O_cam(raw_mat);
            wave::Transformation<> T_O_Lx = T_O_cam * T_CAM_LIDAR;
            if (frame_id == 0) {
                T_L1_O = T_O_Lx.transformInverse();
            }
            T_L1_Lx_trajectory.at(frame_id).pose = T_L1_O * T_O_Lx;
            T_L1_Lx_trajectory.at(frame_id).stamp = times.at(frame_id);
        }
    }

    wave::VecE<wave::PoseVelStamped> odom_trajectory;

    auto func = [&]() { updateVisualizer(&odom, display, &odom_trajectory); };
    odom.registerOutputFunction(func);

    unsigned long counter = 0;
    bool binary_format = false;
    uint16_t ring_index = 0;

    uint32_t scan_index = 0;
    for (auto iter = v.begin(); iter != v.end(); ++iter) {
        fstream cloud_file;
        if (iter->string().substr(iter->string().find_last_of('.') + 1) == "bin") {
            binary_format = true;
            cloud_file.open(iter->string(), ios::in | ios::binary);
        } else {
            cloud_file.open(iter->string(), ios::in);
        }
        const auto &time_t = times.at(counter);

        ++counter;

        ring_index = 0;
        double prev_azimuth = 0;
        bool first_point = true;
        while (cloud_file.good() && !cloud_file.eof()) {
            std::vector<wave::PointXYZIR> pt_vec(1);
            if (binary_format) {
                cloud_file.read((char *) pt_vec.front().data, 3 * sizeof(float));
                cloud_file.read((char *) &pt_vec.front().intensity, sizeof(float));
            } else {
                std::string line;
                std::getline(cloud_file, line);
                std::stringstream ss(line);
                ss >> pt_vec.front().x;
                ss >> pt_vec.front().y;
                ss >> pt_vec.front().z;
                ss >> pt_vec.front().intensity;
            }

            auto azimuth = (std::atan2(pt_vec.front().y, pt_vec.front().x));
            azimuth < 0 ? azimuth = (float) (azimuth + 2.0 * M_PI) : azimuth;

            wave::TimeType stamp = time_t;

            if (prev_azimuth > azimuth + 0.2) {
                ++ring_index;
            }
            prev_azimuth = azimuth;

            pt_vec.front().ring = ring_index;

            if (ring_index < 64) {
                if (first_point) {
                    odom.addPoints(pt_vec, 0, stamp);
                    first_point = false;
                } else {
                    odom.addPoints(pt_vec, 3000, stamp);;
                }
            }
        }
        cloud_file.close();
        ++scan_index;
        if (scan_index % 10 == 0 || scan_index == times.size()) {
            std::cout << "\rFinished with scan " << std::to_string(scan_index) << "/"
                      << std::to_string(times.size()) << std::flush;
        }
    }

    if (T_L1_Lx_trajectory.empty()) {
        plotResults(odom_trajectory);
    } else {
        plotResults(T_L1_Lx_trajectory, odom_trajectory);
    }

    ofstream output_file(sequence + ".txt");
    wave::Transformation<> T_O_L1 = T_L1_O.transformInverse();

    const static Eigen::IOFormat Format(
            Eigen::FullPrecision, Eigen::DontAlignCols, " ", " ");

    for (const auto &T_L1_Lx : odom_trajectory) {
        wave::Transformation<> T_O_CAM;
        T_O_CAM = T_O_L1 * T_L1_Lx.pose * T_CAM_LIDAR.transformInverse();
        output_file << T_O_CAM.storage.format(Format) << "\n";
    }

    if (run_viz) {
        display->stopSpin();
        delete display;
    }

    return 0;
}
