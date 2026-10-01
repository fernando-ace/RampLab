#include "airside/autonomy/scenario_loader.hpp"
#include "airside/autonomy/fleet.hpp"

#include "airside/scenario/scenario_loader.hpp"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <stdexcept>

namespace airside::autonomy {
namespace {
template<class T> T value(const YAML::Node& n,const char* key,T fallback){return n[key]?n[key].as<T>():fallback;}
SensorKind sensor_kind(const std::string& v){if(v=="gnss")return SensorKind::Gnss;if(v=="imu")return SensorKind::Imu;if(v=="odometry")return SensorKind::Odometry;if(v=="lidar")return SensorKind::Lidar;throw std::invalid_argument("unknown fault sensor '"+v+"'");}
SensorFaultKind fault_kind(const std::string& v){if(v=="dropout")return SensorFaultKind::Dropout;if(v=="noise")return SensorFaultKind::Noise;if(v=="bias")return SensorFaultKind::Bias;if(v=="range_limit")return SensorFaultKind::RangeLimit;if(v=="obstruction")return SensorFaultKind::Obstruction;if(v=="scale")return SensorFaultKind::Scale;if(v=="drift")return SensorFaultKind::Drift;if(v=="delay")return SensorFaultKind::Delay;if(v=="packet_loss")return SensorFaultKind::PacketLoss;if(v=="burst_loss")return SensorFaultKind::BurstLoss;throw std::invalid_argument("unknown sensor fault type '"+v+"'");}
}
AutonomyScenario load_scenario(const std::filesystem::path& path){
    try {
        const auto root=YAML::LoadFile(path.string());
        AutonomyScenario s;
        s.name=value<std::string>(root,"name","autonomy_tug");
        if(const auto estimator=root["estimator"]) {
            s.estimator_enabled=value<bool>(estimator,"enabled",true);
            s.estimator.initial_position_variance_m2=value<double>(estimator,"initial_position_variance_m2",s.estimator.initial_position_variance_m2);
            s.estimator.initial_heading_variance_rad2=value<double>(estimator,"initial_heading_variance_rad2",s.estimator.initial_heading_variance_rad2);
            s.estimator.initial_speed_variance_m2ps2=value<double>(estimator,"initial_speed_variance_m2ps2",s.estimator.initial_speed_variance_m2ps2);
            s.estimator.position_process_noise_m2ps=value<double>(estimator,"position_process_noise_m2ps",s.estimator.position_process_noise_m2ps);
            s.estimator.heading_process_noise_rad2ps=value<double>(estimator,"heading_process_noise_rad2ps",s.estimator.heading_process_noise_rad2ps);
            s.estimator.speed_process_noise_m2ps3=value<double>(estimator,"speed_process_noise_m2ps3",s.estimator.speed_process_noise_m2ps3);
            s.estimator.gnss_sigma_m=value<double>(estimator,"gnss_sigma_m",s.estimator.gnss_sigma_m);
            s.estimator.imu_heading_sigma_rad=value<double>(estimator,"imu_heading_sigma_rad",s.estimator.imu_heading_sigma_rad);
            s.estimator.imu_yaw_rate_sigma_radps=value<double>(estimator,"imu_yaw_rate_sigma_radps",s.estimator.imu_yaw_rate_sigma_radps);
            s.estimator.odometry_speed_sigma_mps=value<double>(estimator,"odometry_speed_sigma_mps",s.estimator.odometry_speed_sigma_mps);
            s.estimator.odometry_heading_sigma_rad=value<double>(estimator,"odometry_heading_sigma_rad",s.estimator.odometry_heading_sigma_rad);
            s.estimator.gnss_nis_gate=value<double>(estimator,"gnss_nis_gate",s.estimator.gnss_nis_gate);
            s.estimator.maximum_measurement_age_s=value<double>(estimator,"maximum_measurement_age_s",s.estimator.maximum_measurement_age_s);
            s.estimator.degraded_position_sigma_m=value<double>(estimator,"degraded_position_sigma_m",s.estimator.degraded_position_sigma_m);
            s.estimator.unsafe_position_sigma_m=value<double>(estimator,"unsafe_position_sigma_m",s.estimator.unsafe_position_sigma_m);
            s.estimator.degraded_heading_sigma_rad=value<double>(estimator,"degraded_heading_sigma_rad",s.estimator.degraded_heading_sigma_rad);
            s.estimator.unsafe_heading_sigma_rad=value<double>(estimator,"unsafe_heading_sigma_rad",s.estimator.unsafe_heading_sigma_rad);
            s.estimator.unsafe_without_gnss_s=value<double>(estimator,"unsafe_without_gnss_s",s.estimator.unsafe_without_gnss_s);
            s.estimator.unobserved_stop_deceleration_mps2=value<double>(estimator,"unobserved_stop_deceleration_mps2",s.estimator.unobserved_stop_deceleration_mps2);
        }
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
        s.localization_timeout_s=value<double>(simulation,"localization_timeout_s",3.0);
        s.perception_timeout_s=value<double>(simulation,"perception_timeout_s",0.5);
        const auto sensors=root["sensors"];
        s.sensors.gnss_hz=value<double>(sensors,"gnss_hz",10.0);
        s.sensors.gnss_sigma_m=value<double>(sensors,"gnss_sigma_m",0.5);
        s.sensors.gnss_bias_m={value<double>(sensors,"gnss_bias_x_m",0.0),value<double>(sensors,"gnss_bias_y_m",0.0)};
        s.sensors.imu_hz=value<double>(sensors,"imu_hz",50.0);
        s.sensors.imu_heading_sigma_rad=value<double>(sensors,"imu_heading_sigma_rad",0.005);
        s.sensors.imu_yaw_rate_sigma_radps=value<double>(sensors,"imu_yaw_rate_sigma_radps",0.005);
        s.sensors.imu_accel_sigma_mps2=value<double>(sensors,"imu_accel_sigma_mps2",0.03);
        s.sensors.odometry_hz=value<double>(sensors,"odometry_hz",50.0);
        s.sensors.odometry_sigma_mps=value<double>(sensors,"odometry_sigma_mps",0.02);
        s.sensors.odometry_sigma_m=value<double>(sensors,"odometry_sigma_m",0.01);
        s.sensors.lidar_hz=value<double>(sensors,"lidar_hz",10.0);
        s.sensors.camera_hz=value<double>(sensors,"camera_hz",20.0);
        s.sensors.lidar_fov_rad=value<double>(sensors,"lidar_fov_deg",180.0)*3.14159265358979323846/180.0;
        s.sensors.lidar_beams=value<std::size_t>(sensors,"lidar_beams",181);
        s.sensors.lidar_min_range_m=value<double>(sensors,"lidar_min_range_m",0.1);
        s.sensors.lidar_max_range_m=value<double>(sensors,"lidar_max_range_m",30.0);
        s.sensors.lidar_sigma_m=value<double>(sensors,"lidar_sigma_m",0.01);
        const auto timing = [&](std::string_view prefix, double rate) {
            const std::string key{prefix};
            return SensorTimingConfig{
                rate,
                value<double>(sensors,(key+"_phase_s").c_str(),0.0),
                value<double>(sensors,(key+"_latency_s").c_str(),0.0),
                value<double>(sensors,(key+"_jitter_s").c_str(),0.0),
                value<double>(sensors,(key+"_packet_loss_probability").c_str(),0.0),
                value<double>(sensors,(key+"_stale_after_s").c_str(),0.0)};
        };
        s.sensors.gnss_timing=timing("gnss",s.sensors.gnss_hz);
        s.sensors.imu_timing=timing("imu",s.sensors.imu_hz);
        s.sensors.odometry_timing=timing("odometry",s.sensors.odometry_hz);
        s.sensors.lidar_timing=timing("lidar",s.sensors.lidar_hz);
        s.sensors.camera_timing=timing("camera",s.sensors.camera_hz);
        const auto extrinsics = [&](std::string_view prefix, SensorExtrinsics defaults) {
            const std::string key{prefix};
            return SensorExtrinsics{
                value<double>(sensors,(key+"_extrinsic_x_m").c_str(),defaults.x_m),
                value<double>(sensors,(key+"_extrinsic_y_m").c_str(),defaults.y_m),
                value<double>(sensors,(key+"_extrinsic_z_m").c_str(),defaults.z_m),
                value<double>(sensors,(key+"_extrinsic_yaw_rad").c_str(),defaults.yaw_rad)};
        };
        s.sensors.gnss_extrinsics=extrinsics("gnss",s.sensors.gnss_extrinsics);
        s.sensors.imu_extrinsics=extrinsics("imu",s.sensors.imu_extrinsics);
        s.sensors.odometry_extrinsics=extrinsics("odometry",s.sensors.odometry_extrinsics);
        s.sensors.lidar_extrinsics=extrinsics("lidar",s.sensors.lidar_extrinsics);
        s.sensors.camera_extrinsics=extrinsics("camera",s.sensors.camera_extrinsics);
        if(const auto faults=root["faults"]) for(const auto& f:faults){
            SensorFault fault; fault.sensor=sensor_kind(f["sensor"].as<std::string>()); fault.kind=fault_kind(f["type"].as<std::string>());
            fault.start_s=f["start_s"].as<double>(); fault.duration_s=f["duration_s"].as<double>();
            fault.magnitude=value<double>(f,"magnitude",0.0); fault.probability=value<double>(f,"probability",0.0);
            fault.offset={value<double>(f,"x_m",0.0),value<double>(f,"y_m",0.0)};
            fault.angle_min_rad=value<double>(f,"angle_min_deg",0.0)*3.14159265358979323846/180.0;
            fault.angle_max_rad=value<double>(f,"angle_max_deg",0.0)*3.14159265358979323846/180.0;
            if(!std::isfinite(fault.start_s)||!std::isfinite(fault.duration_s)||!std::isfinite(fault.magnitude)||!std::isfinite(fault.probability)||!std::isfinite(fault.offset.x_m)||!std::isfinite(fault.offset.y_m)||!std::isfinite(fault.angle_min_rad)||!std::isfinite(fault.angle_max_rad)||fault.start_s<0.0||fault.duration_s<=0.0||fault.magnitude<0.0||fault.probability<0.0||fault.probability>1.0||fault.angle_max_rad<fault.angle_min_rad)throw std::invalid_argument("invalid fault interval or parameter");
            if((fault.kind==SensorFaultKind::Noise||fault.kind==SensorFaultKind::RangeLimit||fault.kind==SensorFaultKind::Delay)&&fault.magnitude<=0.0)throw std::invalid_argument("fault magnitude must be positive");
            if((fault.kind==SensorFaultKind::Noise&&fault.sensor!=SensorKind::Gnss&&fault.sensor!=SensorKind::Imu)||(fault.kind==SensorFaultKind::Bias&&fault.sensor!=SensorKind::Gnss&&fault.sensor!=SensorKind::Imu)||(fault.kind==SensorFaultKind::RangeLimit&&fault.sensor!=SensorKind::Lidar)||(fault.kind==SensorFaultKind::Obstruction&&fault.sensor!=SensorKind::Lidar)||(fault.kind==SensorFaultKind::Scale&&fault.sensor!=SensorKind::Odometry)||(fault.kind==SensorFaultKind::Drift&&fault.sensor!=SensorKind::Odometry))throw std::invalid_argument("fault type is incompatible with selected sensor");
            if(fault.kind==SensorFaultKind::Scale&&fault.magnitude>1.0)throw std::invalid_argument("odometry scale magnitude must be at most 1.0");
            if(fault.kind==SensorFaultKind::RangeLimit&&fault.magnitude<=s.sensors.lidar_min_range_m)throw std::invalid_argument("LiDAR range limit must exceed minimum range");
            if(fault.kind==SensorFaultKind::Obstruction&&(fault.angle_min_rad< -s.sensors.lidar_fov_rad*0.5||fault.angle_max_rad>s.sensors.lidar_fov_rad*0.5))throw std::invalid_argument("LiDAR obstruction sector is outside configured field of view");
            s.faults.push_back(fault);
        }
        if(const auto obstacles=root["obstacles"])for(const auto& o:obstacles)s.obstacles.push_back({o["id"].as<std::string>(),{o["x_m"].as<double>(),o["y_m"].as<double>()},o["radius_m"].as<double>()});
        return s;
    } catch(const std::exception& e){throw std::runtime_error("autonomy scenario '"+path.string()+"': "+e.what());}
}

FleetScenario load_fleet_scenario(const std::filesystem::path& path){
    try{
        const auto root=YAML::LoadFile(path.string());FleetScenario out;
        out.name=value<std::string>(root,"name","autonomy_fleet");out.default_seed=value<std::uint64_t>(root,"default_seed",42);
        out.vehicle_scenario=load_scenario(path.parent_path()/root["vehicle_scenario"].as<std::string>());
        out.deadlock_persistence_s=value<double>(root,"deadlock_persistence_s",2.0);
        if(!std::isfinite(out.deadlock_persistence_s)||out.deadlock_persistence_s<0.0)throw std::invalid_argument("deadlock_persistence_s must be finite and nonnegative");
        out.resource_specific_tie_breaks=value<bool>(root,"resource_specific_tie_breaks",false);
        const auto list=root["missions"];if(!list||!list.IsSequence()||list.size()==0)throw std::invalid_argument("fleet scenario requires at least one mission");
        for(const auto& item:list){FleetMission m;m.id.value=item["id"].as<std::string>();m.start_node=item["start_node"].as<std::string>();m.goal_node=item["goal_node"].as<std::string>();m.priority=value<int>(item,"priority",0);if(const auto faults=item["faults"])for(const auto& f:faults){SensorFault x;x.sensor=sensor_kind(f["sensor"].as<std::string>());x.kind=fault_kind(f["type"].as<std::string>());x.start_s=f["start_s"].as<double>();x.duration_s=f["duration_s"].as<double>();x.magnitude=value<double>(f,"magnitude",0.0);x.probability=value<double>(f,"probability",0.0);x.offset={value<double>(f,"x_m",0.0),value<double>(f,"y_m",0.0)};if(x.start_s<0||x.duration_s<=0||x.probability<0||x.probability>1)throw std::invalid_argument("invalid fleet sensor fault window");m.faults.push_back(x);}out.missions.push_back(std::move(m));}
        std::ranges::sort(out.missions,{},&FleetMission::id);for(std::size_t i=0;i<out.missions.size();++i){if(out.missions[i].id.value.empty()||(i&&out.missions[i-1].id==out.missions[i].id))throw std::invalid_argument("fleet vehicle IDs must be nonempty and unique");}
        if(const auto changes=root["road_events"])for(const auto& item:changes){
            const double time=value<double>(item,"time_seconds",-1.0);
            const auto edge_value=item["edge_id"].as<std::uint32_t>();
            const bool available=item["available"].as<bool>();
            if(!std::isfinite(time)||time<0.0||std::trunc(time)!=time)throw std::invalid_argument("fleet road event time_seconds must be a nonnegative whole second");
            const EdgeId edge{edge_value};
            (void)out.vehicle_scenario.airport.graph.edge(edge);
            out.road_events.push_back({std::chrono::seconds{static_cast<std::int64_t>(time)},edge,available});
        }
        std::ranges::stable_sort(out.road_events,{},[](const auto& event){return event.time;});
        return out;
    }catch(const std::exception&e){throw std::runtime_error("fleet scenario '"+path.string()+"': "+e.what());}
}
}
