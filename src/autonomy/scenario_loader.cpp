#include "airside/autonomy/scenario_loader.hpp"

#include "airside/scenario/scenario_loader.hpp"

#include <yaml-cpp/yaml.h>

#include <stdexcept>

namespace airside::autonomy {
namespace {
template<class T> T value(const YAML::Node& n,const char* key,T fallback){return n[key]?n[key].as<T>():fallback;}
}
AutonomyScenario load_scenario(const std::filesystem::path& path){
    try {
        const auto root=YAML::LoadFile(path.string());
        AutonomyScenario s;
        s.name=value<std::string>(root,"name","autonomy_tug");
        s.default_seed=value<std::uint64_t>(root,"default_seed",42);
        const auto map_path=path.parent_path()/value<std::string>(root,"map_scenario","baseline.yaml");
        s.airport=airside::load_scenario(map_path);
        const auto mission=root["mission"];
        s.start_node=value<std::string>(mission,"start_node","Service Depot");
        s.goal_node=value<std::string>(mission,"goal_node","Gate A2");
        const auto vehicle=root["vehicle"];
        s.initial_state.position={value<double>(vehicle,"x_m",0.0),value<double>(vehicle,"y_m",0.0)};
        s.initial_state.heading_rad=value<double>(vehicle,"heading_rad",0.0);
        s.limits.maximum_speed_mps=value<double>(vehicle,"maximum_speed_mps",5.0);
        s.limits.maximum_acceleration_mps2=value<double>(vehicle,"maximum_acceleration_mps2",1.0);
        s.limits.maximum_deceleration_mps2=value<double>(vehicle,"maximum_deceleration_mps2",1.5);
        s.limits.maximum_yaw_rate_radps=value<double>(vehicle,"maximum_yaw_rate_radps",0.8);
        s.limits.radius_m=value<double>(vehicle,"radius_m",1.0);
        const auto simulation=root["simulation"];
        s.timestep_s=value<double>(simulation,"timestep_s",0.02);
        s.timeout_s=value<double>(simulation,"timeout_s",240.0);
        s.goal_tolerance_m=value<double>(simulation,"goal_tolerance_m",2.0);
        s.stopped_speed_mps=value<double>(simulation,"stopped_speed_mps",0.15);
        s.safety_stop_range_m=value<double>(simulation,"safety_stop_range_m",2.2);
        const auto sensors=root["sensors"];
        s.sensors.gnss_hz=value<double>(sensors,"gnss_hz",5.0);
        s.sensors.gnss_sigma_m=value<double>(sensors,"gnss_sigma_m",0.5);
        s.sensors.gnss_bias_m={value<double>(sensors,"gnss_bias_x_m",0.0),value<double>(sensors,"gnss_bias_y_m",0.0)};
        s.sensors.imu_hz=value<double>(sensors,"imu_hz",50.0);
        s.sensors.imu_yaw_sigma_radps=value<double>(sensors,"imu_yaw_sigma_radps",0.005);
        s.sensors.imu_accel_sigma_mps2=value<double>(sensors,"imu_accel_sigma_mps2",0.03);
        s.sensors.odometry_hz=value<double>(sensors,"odometry_hz",20.0);
        s.sensors.odometry_sigma_mps=value<double>(sensors,"odometry_sigma_mps",0.02);
        s.sensors.lidar_hz=value<double>(sensors,"lidar_hz",10.0);
        s.sensors.lidar_fov_rad=value<double>(sensors,"lidar_fov_deg",180.0)*3.14159265358979323846/180.0;
        s.sensors.lidar_beams=value<std::size_t>(sensors,"lidar_beams",181);
        s.sensors.lidar_min_range_m=value<double>(sensors,"lidar_min_range_m",0.1);
        s.sensors.lidar_max_range_m=value<double>(sensors,"lidar_max_range_m",30.0);
        s.sensors.lidar_sigma_m=value<double>(sensors,"lidar_sigma_m",0.01);
        if(const auto obstacles=root["obstacles"])for(const auto& o:obstacles)s.obstacles.push_back({o["id"].as<std::string>(),{o["x_m"].as<double>(),o["y_m"].as<double>()},o["radius_m"].as<double>()});
        return s;
    } catch(const std::exception& e){throw std::runtime_error("autonomy scenario '"+path.string()+"': "+e.what());}
}
}
