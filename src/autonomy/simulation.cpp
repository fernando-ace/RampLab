#include "airside/autonomy/simulation.hpp"
#include "airside/autonomy/state_estimator.hpp"

#include "airside/routing/astar.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>

namespace airside::autonomy {
namespace {
constexpr double kPi = 3.14159265358979323846;
double wrap_angle(double angle) { return std::remainder(angle, 2.0 * kPi); }
double distance(Vec2 a, Vec2 b) { return std::hypot(a.x_m - b.x_m, a.y_m - b.y_m); }
void hash_value(std::uint64_t& hash, double value) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    for (unsigned shift = 0; shift < 64; shift += 8) { hash ^= static_cast<std::uint8_t>(bits >> shift); hash *= 1099511628211ULL; }
}
NodeId find_node(const AirportGraph& graph, const std::string& key) {
    for (const auto& node : graph.nodes()) if (node.name == key) return node.id;
    throw std::invalid_argument("autonomy route references unknown road node '" + key + "'");
}
double ray_circle_distance(Vec2 origin, double angle, const CircleObstacle& obstacle) {
    const double dx = std::cos(angle), dy = std::sin(angle);
    const double ox = origin.x_m - obstacle.center.x_m, oy = origin.y_m - obstacle.center.y_m;
    const double b = ox * dx + oy * dy, c = ox * ox + oy * oy - obstacle.radius_m * obstacle.radius_m;
    const double discriminant = b * b - c;
    if (discriminant < 0.0) return std::numeric_limits<double>::infinity();
    const double root = std::sqrt(discriminant), near_hit = -b - root, far_hit = -b + root;
    return near_hit >= 0.0 ? near_hit : (far_hit >= 0.0 ? far_hit : std::numeric_limits<double>::infinity());
}
double point_segment_distance(Vec2 p, Vec2 a, Vec2 b) {
    const double dx = b.x_m - a.x_m, dy = b.y_m - a.y_m, length2 = dx * dx + dy * dy;
    if (length2 == 0.0) return distance(p, a);
    const double t = std::clamp(((p.x_m-a.x_m)*dx + (p.y_m-a.y_m)*dy) / length2, 0.0, 1.0);
    return distance(p, {a.x_m+t*dx, a.y_m+t*dy});
}
double route_error(Vec2 point, const std::vector<Vec2>& waypoints) {
    if (waypoints.size() < 2) return waypoints.empty() ? 0.0 : distance(point, waypoints.front());
    double result = std::numeric_limits<double>::infinity();
    for (std::size_t i=1; i<waypoints.size(); ++i) result=std::min(result,point_segment_distance(point,waypoints[i-1],waypoints[i]));
    return result;
}
}

struct AutonomySimulation::Impl {
    AutonomyScenario scenario;
    StateEstimator2D estimator;
    std::mt19937_64 random;
    std::mt19937_64 fault_random;
    std::normal_distribution<double> normal{0.0, 1.0};
    std::normal_distribution<double> fault_normal{0.0, 1.0};
    MissionState mission;
    VehicleState state;
    double time{0.0}, previous_accel{0.0}, initial_heading{0.0};
    SensorFrame frame;
    double next_gnss{0.0}, next_imu{0.0}, next_odom{0.0}, next_lidar{0.0};
    bool complete{false}, safety_was_active{false};
    MissionMetrics metrics;
    std::size_t emergency_stops{0}, route_error_count{0};
    std::size_t estimate_count{};
    double position_error_sum{}, position_error_squared_sum{}, heading_error_sum{};
    Vec2 last_estimated_position{};
    double last_estimated_heading{};
    bool estimator_stop_recorded{};
    bool gnss_gate_active{}, stale_event_active{};
    double previous_time_eval{};
    double route_error_sum{0.0};
    std::uint64_t digest{14695981039346656037ULL};
    std::vector<bool> fault_active;
    std::vector<bool> fault_drop_recorded, fault_delay_recorded;
    std::deque<std::pair<double,GnssMeasurement>> delayed_gnss;
    std::deque<std::pair<double,ImuMeasurement>> delayed_imu;
    std::deque<std::pair<double,OdometryMeasurement>> delayed_odometry;
    std::deque<std::pair<double,LidarScan>> delayed_lidar;
    double odometry_scale{1.0}, odometry_anchor_truth{}, odometry_anchor_reported{};
    double unavailable_since{-1.0};
    void evaluate_estimated_pose(Vec2 position,double heading) {
        const double pe=distance(position,state.position), he=std::abs(wrap_angle(heading-state.heading_rad));
        position_error_sum+=pe;position_error_squared_sum+=pe*pe;heading_error_sum+=he;++estimate_count;
        metrics.maximum_position_error_m=std::max(metrics.maximum_position_error_m,pe);
        metrics.maximum_heading_error_rad=std::max(metrics.maximum_heading_error_rad,he);
        last_estimated_position=position;last_estimated_heading=heading;
    }

    Impl(AutonomyScenario input, std::uint64_t seed, std::uint64_t fault_seed) : scenario(std::move(input)), estimator(scenario.estimator), random(seed), fault_random(fault_seed==0?seed^0x9e3779b97f4a7c15ULL:fault_seed), state(scenario.initial_state), fault_active(scenario.faults.size(), false), fault_drop_recorded(scenario.faults.size(),false), fault_delay_recorded(scenario.faults.size(),false) {
        for(std::size_t i=0;i<scenario.faults.size();++i)metrics.fault_events.push_back({i,0.0,"scheduled"});
        const auto& sensor=scenario.sensors;
        if (!(scenario.timestep_s>0.0) || !(scenario.timeout_s>0.0) || !(scenario.localization_timeout_s>0.0) || !(scenario.perception_timeout_s>0.0) || !(scenario.limits.maximum_speed_mps>0.0) ||
            !(scenario.limits.maximum_acceleration_mps2>0.0) || !(scenario.limits.maximum_deceleration_mps2>0.0) ||
            !(scenario.limits.maximum_yaw_rate_radps>0.0) || !(sensor.gnss_hz>0.0) || !(sensor.imu_hz>0.0) ||
            !(sensor.odometry_hz>0.0) || !(sensor.lidar_hz>0.0) || sensor.lidar_beams<2 ||
            !(sensor.lidar_max_range_m>sensor.lidar_min_range_m) || !(sensor.lidar_fov_rad>0.0) ||
            sensor.gnss_sigma_m<0.0 || sensor.imu_heading_sigma_rad<0.0 || sensor.imu_yaw_rate_sigma_radps<0.0 ||
            sensor.imu_accel_sigma_mps2<0.0 || sensor.odometry_sigma_mps<0.0 || sensor.odometry_sigma_m<0.0 || sensor.lidar_sigma_m<0.0)
            throw std::invalid_argument("invalid autonomy simulation configuration");
        const auto start=find_node(scenario.airport.graph,scenario.start_node), goal=find_node(scenario.airport.graph,scenario.goal_node);
        const auto route=find_route(scenario.airport.graph,start,goal);
        if (!route) throw std::invalid_argument("no airport road route exists for autonomy mission");
        for (const auto node:route->nodes) mission.waypoints.push_back(scenario.airport.graph.node(node).position);
        mission.goal=mission.waypoints.back(); mission.limits=scenario.limits;
        if (state.position==Vec2{}) state.position=mission.waypoints.front();
        initial_heading=state.heading_rad;
        metrics.minimum_obstacle_clearance_m=std::numeric_limits<double>::infinity();
        produce_sensors();
    }
    double noise(double sigma) { return sigma==0.0 ? 0.0 : normal(random)*sigma; }
    double fault_noise(double sigma) { return sigma==0.0 ? 0.0 : fault_normal(fault_random)*sigma; }
    LidarScan make_lidar() {
        const auto& c=scenario.sensors; LidarScan scan;
        scan.timestamp_s=time; scan.angle_min_rad=-c.lidar_fov_rad*0.5;
        scan.angle_increment_rad=c.lidar_fov_rad/static_cast<double>(c.lidar_beams-1);
        scan.range_min_m=c.lidar_min_range_m; scan.range_max_m=c.lidar_max_range_m; scan.ranges_m.reserve(c.lidar_beams);
        for(std::size_t i=0;i<c.lidar_beams;++i){
            const double angle=state.heading_rad+scan.angle_min_rad+static_cast<double>(i)*scan.angle_increment_rad;
            double range=c.lidar_max_range_m;
            for(const auto& o:scenario.obstacles){const double hit=ray_circle_distance(state.position,angle,o);if(hit>=c.lidar_min_range_m&&hit<=c.lidar_max_range_m)range=std::min(range,hit);}
            scan.ranges_m.push_back(std::clamp(range+noise(c.lidar_sigma_m),c.lidar_min_range_m,c.lidar_max_range_m));
        }
        return scan;
    }
    void produce_sensors() {
        frame.timestamp_s=time; const auto& c=scenario.sensors;
        double desired_odom_scale=1.0;for(const auto& fault:scenario.faults)if(fault.sensor==SensorKind::Odometry&&fault.kind==SensorFaultKind::Scale&&time+1e-9>=fault.start_s&&time<fault.start_s+fault.duration_s-1e-9)desired_odom_scale*=1.0-fault.magnitude;
        if(std::abs(desired_odom_scale-odometry_scale)>1e-12){odometry_anchor_reported=odometry_anchor_reported+(state.distance_m-odometry_anchor_truth)*odometry_scale;odometry_anchor_truth=state.distance_m;odometry_scale=desired_odom_scale;}
        const double reported_distance=odometry_anchor_reported+(state.distance_m-odometry_anchor_truth)*odometry_scale;
        if(time+1e-9>=next_imu){frame.imu=ImuMeasurement{time,wrap_angle(state.heading_rad+noise(c.imu_heading_sigma_rad)),state.yaw_rate_radps+noise(c.imu_yaw_rate_sigma_radps),previous_accel+noise(c.imu_accel_sigma_mps2)};next_imu+=1.0/c.imu_hz;++metrics.imu_samples;}
        if(time+1e-9>=next_odom){frame.odometry=OdometryMeasurement{time,reported_distance+noise(c.odometry_sigma_m),std::max(0.0,state.speed_mps*odometry_scale+noise(c.odometry_sigma_mps)),wrap_angle(state.heading_rad-initial_heading)};next_odom+=1.0/c.odometry_hz;++metrics.odometry_samples;}
        if(time+1e-9>=next_lidar){frame.lidar=make_lidar();next_lidar+=1.0/c.lidar_hz;++metrics.lidar_scans;}
        if(time+1e-9>=next_gnss){frame.gnss=GnssMeasurement{time,{state.position.x_m+c.gnss_bias_m.x_m+noise(c.gnss_sigma_m),state.position.y_m+c.gnss_bias_m.y_m+noise(c.gnss_sigma_m)},c.gnss_sigma_m*1.96};next_gnss+=1.0/c.gnss_hz;++metrics.gnss_samples;}
        bool any_unavailable=false;double gnss_delay=0.0,imu_delay=0.0,odometry_delay=0.0,lidar_delay=0.0;
        for(std::size_t i=0;i<scenario.faults.size();++i){
            const auto& fault=scenario.faults[i]; const bool active=time+1e-9>=fault.start_s&&time<fault.start_s+fault.duration_s-1e-9;
            if(active!=fault_active[i]){fault_active[i]=active;(active?metrics.fault_activation_times_s:metrics.fault_deactivation_times_s).push_back(time);metrics.fault_events.push_back({i,time,active?"activated":"deactivated"});}
            if(!active)continue;
            bool dropped=false;
            if(fault.kind==SensorFaultKind::Dropout||fault.kind==SensorFaultKind::BurstLoss){
                std::size_t queued=0;
                if(fault.sensor==SensorKind::Gnss){queued=delayed_gnss.size();delayed_gnss.clear();if(frame.gnss){frame.gnss.reset();++queued;}metrics.gnss_dropped+=queued;}
                else if(fault.sensor==SensorKind::Imu){queued=delayed_imu.size();delayed_imu.clear();if(frame.imu){frame.imu.reset();++queued;}metrics.imu_dropped+=queued;}
                else if(fault.sensor==SensorKind::Odometry){queued=delayed_odometry.size();delayed_odometry.clear();if(frame.odometry){frame.odometry.reset();++queued;}metrics.odometry_dropped+=queued;}
                else {queued=delayed_lidar.size();delayed_lidar.clear();if(frame.lidar){frame.lidar.reset();++queued;}metrics.lidar_dropped+=queued;}
                metrics.messages_dropped+=queued;if(queued>0&&!fault_drop_recorded[i]){metrics.fault_events.push_back({i,time,"measurement_dropped"});fault_drop_recorded[i]=true;}any_unavailable=true;continue;
            }
            auto loss=[&](bool fresh){return fault.kind==SensorFaultKind::Dropout||fault.kind==SensorFaultKind::BurstLoss||(fault.kind==SensorFaultKind::PacketLoss&&fresh&&std::generate_canonical<double,53>(fault_random)<fault.probability);};
            if(fault.sensor==SensorKind::Gnss&&frame.gnss){auto& m=*frame.gnss;
                const bool fresh=std::abs(m.timestamp_s-time)<1e-9;
                if(fresh&&fault.kind==SensorFaultKind::Noise){m.position.x_m+=fault_noise(fault.magnitude);m.position.y_m+=fault_noise(fault.magnitude);m.accuracy_m=std::hypot(m.accuracy_m,fault.magnitude*1.96);}
                else if(fresh&&fault.kind==SensorFaultKind::Bias){m.position.x_m+=fault.offset.x_m;m.position.y_m+=fault.offset.y_m;}
                else if(fresh&&fault.kind==SensorFaultKind::Delay)gnss_delay=std::max(gnss_delay,fault.magnitude);
                if(loss(fresh)){frame.gnss.reset();++metrics.messages_dropped;++metrics.gnss_dropped;dropped=true;}
            } else if(fault.sensor==SensorKind::Imu&&frame.imu){auto& m=*frame.imu;
                const bool fresh=std::abs(m.timestamp_s-time)<1e-9;
                if(fresh&&fault.kind==SensorFaultKind::Noise){m.heading_rad=wrap_angle(m.heading_rad+fault_noise(fault.magnitude));m.yaw_rate_radps+=fault_noise(fault.magnitude);}
                else if(fresh&&fault.kind==SensorFaultKind::Bias){m.heading_rad=wrap_angle(m.heading_rad+fault.magnitude);m.yaw_rate_radps+=fault.magnitude;}
                else if(fresh&&fault.kind==SensorFaultKind::Delay)imu_delay=std::max(imu_delay,fault.magnitude);
                if(loss(fresh)){frame.imu.reset();++metrics.messages_dropped;++metrics.imu_dropped;dropped=true;}
            } else if(fault.sensor==SensorKind::Odometry&&frame.odometry){auto& m=*frame.odometry;
                const bool fresh=std::abs(m.timestamp_s-time)<1e-9;
                if(fresh&&fault.kind==SensorFaultKind::Drift)m.distance_m+=fault.magnitude*std::max(0.0,time-fault.start_s);
                else if(fresh&&fault.kind==SensorFaultKind::Delay)odometry_delay=std::max(odometry_delay,fault.magnitude);
                if(loss(fresh)){frame.odometry.reset();++metrics.messages_dropped;++metrics.odometry_dropped;dropped=true;}
            } else if(fault.sensor==SensorKind::Lidar&&frame.lidar){auto& scan=*frame.lidar;
                const bool fresh=std::abs(scan.timestamp_s-time)<1e-9;
                if(fresh&&fault.kind==SensorFaultKind::RangeLimit){scan.range_max_m=std::min(scan.range_max_m,fault.magnitude);for(auto& r:scan.ranges_m)r=std::min(r,scan.range_max_m);}
                else if(fresh&&fault.kind==SensorFaultKind::Obstruction){for(std::size_t b=0;b<scan.ranges_m.size();++b){const double angle=scan.angle_min_rad+static_cast<double>(b)*scan.angle_increment_rad;if(angle>=fault.angle_min_rad&&angle<=fault.angle_max_rad)scan.ranges_m[b]=scan.range_max_m;}}
                else if(fresh&&fault.kind==SensorFaultKind::Delay)lidar_delay=std::max(lidar_delay,fault.magnitude);
                if(loss(fresh)){frame.lidar.reset();++metrics.messages_dropped;++metrics.lidar_dropped;dropped=true;}
            }
            if(dropped&&!fault_drop_recorded[i]){metrics.fault_events.push_back({i,time,"measurement_dropped"});fault_drop_recorded[i]=true;}
            if((fault.sensor==SensorKind::Gnss?gnss_delay:fault.sensor==SensorKind::Imu?imu_delay:fault.sensor==SensorKind::Odometry?odometry_delay:lidar_delay)>0.0&&!fault_delay_recorded[i]){metrics.fault_events.push_back({i,time,"measurement_delayed"});fault_delay_recorded[i]=true;}
            any_unavailable=any_unavailable||dropped||fault.kind==SensorFaultKind::Dropout||fault.kind==SensorFaultKind::BurstLoss;
        }
        const auto enqueue_delay=[&](auto& value,auto& queue,double delay,std::size_t& sensor_delayed){if(value&&delay>0.0&&std::abs(value->timestamp_s-time)<1e-9){queue.emplace_back(time+delay,*value);value.reset();++metrics.messages_delayed;++sensor_delayed;metrics.maximum_delay_s=std::max(metrics.maximum_delay_s,delay);}};
        enqueue_delay(frame.gnss,delayed_gnss,gnss_delay,metrics.gnss_delayed);enqueue_delay(frame.imu,delayed_imu,imu_delay,metrics.imu_delayed);enqueue_delay(frame.odometry,delayed_odometry,odometry_delay,metrics.odometry_delayed);enqueue_delay(frame.lidar,delayed_lidar,lidar_delay,metrics.lidar_delayed);
        const auto release=[&](auto& queue,auto& value){while(!queue.empty()&&queue.front().first<=time+1e-9){value=std::move(queue.front().second);queue.pop_front();}};
        release(delayed_gnss,frame.gnss);release(delayed_imu,frame.imu);release(delayed_odometry,frame.odometry);release(delayed_lidar,frame.lidar);
        if(scenario.estimator_enabled) {
            const auto before=estimator.state().health;
            const auto rejected_before=estimator.state().gnss_rejected;
            const auto accepted_before=estimator.state().gnss_accepted;
            const auto stale_before=estimator.state().stale_rejected;
            frame.estimate=estimator.update(frame);
            const auto& e=*frame.estimate;
            if(before!=e.health) {
                const char* event=e.health==EstimatorHealth::Healthy?"healthy":e.health==EstimatorHealth::Degraded?"degraded":
                    e.health==EstimatorHealth::Unsafe?"unsafe":e.health==EstimatorHealth::Invalid?"invalid":"uninitialized";
                metrics.estimator_events.push_back({time,std::string("estimator_")+event});
                if(before==EstimatorHealth::Uninitialized&&e.initialized) {
                    metrics.estimator_initialization_time_s=time;
                    metrics.estimator_events.push_back({time,"estimator_initialized"});
                }
            }
            const double dt_eval=std::max(0.0,time-previous_time_eval);
            if(e.health==EstimatorHealth::Healthy)metrics.estimator_healthy_time_s+=dt_eval;
            else if(e.health==EstimatorHealth::Degraded)metrics.estimator_degraded_time_s+=dt_eval;
            else if(e.health==EstimatorHealth::Unsafe||e.health==EstimatorHealth::Invalid)metrics.estimator_unsafe_time_s+=dt_eval;
            previous_time_eval=time;
            if(e.initialized) {
                // Evaluation truth is read only here; it never enters StateEstimator2D::update.
                evaluate_estimated_pose(e.position,e.heading_rad);
                metrics.maximum_position_uncertainty_m=std::max(metrics.maximum_position_uncertainty_m,e.position_uncertainty_m);
                metrics.maximum_heading_uncertainty_rad=std::max(metrics.maximum_heading_uncertainty_rad,e.heading_uncertainty_rad);
            }
            if(e.gnss_rejected>rejected_before&&!gnss_gate_active)metrics.estimator_events.push_back({time,"gnss_gate_activated"});
            if(e.gnss_rejected>rejected_before)gnss_gate_active=true;
            else if(e.gnss_accepted>accepted_before)gnss_gate_active=false;
            if(e.stale_rejected>stale_before&&!stale_event_active)metrics.estimator_events.push_back({time,"stale_measurement_rejected"});
            if(e.stale_rejected>stale_before)stale_event_active=true;
            else if(e.stale_rejected==stale_before)stale_event_active=false;
            if((e.health==EstimatorHealth::Unsafe||e.health==EstimatorHealth::Invalid)&&!estimator_stop_recorded) {
                metrics.estimator_uncertainty_safety_stops++;estimator_stop_recorded=true;
            }
            metrics.gnss_updates_accepted=e.gnss_accepted;metrics.gnss_updates_rejected=e.gnss_rejected;
            metrics.stale_measurements_rejected=e.stale_rejected;metrics.gnss_gate_activations=e.gate_activations;
        } else frame.estimate.reset();
        if(any_unavailable){if(unavailable_since<0.0){unavailable_since=time;metrics.fault_events.push_back({std::numeric_limits<std::size_t>::max(),time,"sensor_unavailable"});}}else if(unavailable_since>=0.0){metrics.unavailable_duration_s+=time-unavailable_since;metrics.fault_events.push_back({std::numeric_limits<std::size_t>::max(),time,"sensor_recovered"});unavailable_since=-1.0;}
    }
    bool collided(std::string& id)const{for(const auto&o:scenario.obstacles)if(distance(state.position,o.center)<=scenario.limits.radius_m+o.radius_m){id=o.id;return true;}return false;}
    void step(VehicleCommand command){
        const double dt=scenario.timestep_s;
        command.target_speed_mps=std::clamp(command.target_speed_mps,0.0,scenario.limits.maximum_speed_mps);
        command.target_yaw_rate_radps=std::clamp(command.target_yaw_rate_radps,-scenario.limits.maximum_yaw_rate_radps,scenario.limits.maximum_yaw_rate_radps);
        const double old=state.speed_mps, delta=command.target_speed_mps-state.speed_mps;
        const double limit=delta>=0?scenario.limits.maximum_acceleration_mps2:scenario.limits.maximum_deceleration_mps2;
        state.speed_mps=std::clamp(state.speed_mps+std::clamp(delta,-limit*dt,limit*dt),0.0,scenario.limits.maximum_speed_mps);
        previous_accel=(state.speed_mps-old)/dt;state.yaw_rate_radps=command.target_yaw_rate_radps;
        const double middle=state.heading_rad+state.yaw_rate_radps*dt*0.5;
        state.position.x_m+=state.speed_mps*std::cos(middle)*dt;state.position.y_m+=state.speed_mps*std::sin(middle)*dt;
        state.heading_rad=wrap_angle(state.heading_rad+state.yaw_rate_radps*dt);state.distance_m+=state.speed_mps*dt;time+=dt;
        for(const auto&o:scenario.obstacles)metrics.minimum_obstacle_clearance_m=std::min(metrics.minimum_obstacle_clearance_m,distance(state.position,o.center)-scenario.limits.radius_m-o.radius_m);
        const double error=route_error(state.position,mission.waypoints);route_error_sum+=error;++route_error_count;metrics.maximum_route_error_m=std::max(metrics.maximum_route_error_m,error);
        hash_value(digest,state.position.x_m);hash_value(digest,state.position.y_m);hash_value(digest,state.heading_rad);hash_value(digest,state.speed_mps);hash_value(digest,command.target_speed_mps);hash_value(digest,command.target_yaw_rate_radps);
        std::string obstacle;if(collided(obstacle)){++metrics.collision_count;if(!metrics.first_collision_time_s){metrics.first_collision_time_s=time;metrics.collided_obstacle=obstacle;}metrics.result=MissionResult::Collision;complete=true;}
        produce_sensors();
    }
};

ReferenceController::ReferenceController(double stopping,double perception_timeout,double localization_timeout):safety_stop_range_m_(stopping),perception_timeout_s_(perception_timeout),localization_timeout_s_(localization_timeout){}
VehicleCommand ReferenceController::update(const SensorFrame& f,const MissionState&m){
    if(last_update_timestamp_s_>=0.0&&safety_was_active_&&safety_due_to_degraded_sensing_)
        degraded_stop_time_s_+=std::max(0.0,f.timestamp_s-last_update_timestamp_s_);
    last_update_timestamp_s_=f.timestamp_s;
    if(f.lidar)last_lidar_timestamp_s_=std::max(last_lidar_timestamp_s_,f.lidar->timestamp_s);
    if(f.imu)last_imu_timestamp_s_=std::max(last_imu_timestamp_s_,f.imu->timestamp_s);
    if(f.estimate) {
        if(f.estimate->initialized)last_gnss_timestamp_s_=f.timestamp_s-f.estimate->time_since_gnss_s;
        if(f.odometry)last_odometry_timestamp_s_=std::max(last_odometry_timestamp_s_,f.odometry->timestamp_s);
    }
    if(f.estimate && f.estimate->initialized) {
        estimated_position_=f.estimate->position;
        estimated_heading_rad_=f.estimate->heading_rad;
        have_position_=true;
    } else if(!f.estimate && f.odometry && f.odometry->timestamp_s != last_odometry_timestamp_s_){
        if(have_position_){const double delta=f.odometry->distance_m-last_odometry_distance_m_;estimated_position_.x_m+=delta*std::cos(estimated_heading_rad_);estimated_position_.y_m+=delta*std::sin(estimated_heading_rad_);}
        last_odometry_distance_m_=f.odometry->distance_m;last_odometry_timestamp_s_=f.odometry->timestamp_s;
    }
    if(!f.estimate && f.gnss && f.gnss->timestamp_s != last_gnss_timestamp_s_){
        if(!have_position_)estimated_position_=f.gnss->position;
        else{estimated_position_.x_m=.85*estimated_position_.x_m+.15*f.gnss->position.x_m;estimated_position_.y_m=.85*estimated_position_.y_m+.15*f.gnss->position.y_m;}
        have_position_=true;last_gnss_timestamp_s_=f.gnss->timestamp_s;
    }
    if(!f.estimate && f.imu)estimated_heading_rad_=f.imu->heading_rad;
    const double gnss_age=last_gnss_timestamp_s_<0.0?f.timestamp_s:f.timestamp_s-last_gnss_timestamp_s_;
    const double odom_age=last_odometry_timestamp_s_<0.0?f.timestamp_s:f.timestamp_s-last_odometry_timestamp_s_;
    const double imu_age=last_imu_timestamp_s_<0.0?std::numeric_limits<double>::infinity():f.timestamp_s-last_imu_timestamp_s_;
    const double lidar_age=last_lidar_timestamp_s_<0.0?std::numeric_limits<double>::infinity():f.timestamp_s-last_lidar_timestamp_s_;
    const bool estimator_degraded=f.estimate&&f.estimate->health!=EstimatorHealth::Healthy;
    const bool degraded=f.estimate?(estimator_degraded||lidar_age>0.20):
        (gnss_age>0.25||imu_age>0.10||odom_age>0.10||lidar_age>0.20);
    if(degraded&&!degraded_active_){++degraded_mode_entries_;degraded_since_s_=f.timestamp_s;}
    if(!degraded&&degraded_active_&&degraded_since_s_>=0.0)degraded_since_s_=-1.0;
    degraded_active_=degraded;
    const bool localization_lost=(f.estimate&&(f.estimate->health==EstimatorHealth::Uninitialized||
        f.estimate->health==EstimatorHealth::Unsafe||f.estimate->health==EstimatorHealth::Invalid))||
        (!f.estimate&&(gnss_age>localization_timeout_s_||imu_age>localization_timeout_s_||odom_age>localization_timeout_s_));
    const bool perception_lost=lidar_age>perception_timeout_s_;
    if(localization_lost||perception_lost){if(!safety_was_active_){++safety_stop_entries_;safety_stop_since_s_=f.timestamp_s;}safety_was_active_=true;safety_due_to_degraded_sensing_=true;return{};}
    const double goal=distance(estimated_position_,m.goal);
    if(f.lidar){bool blocked=false;for(std::size_t i=0;i<f.lidar->ranges_m.size();++i){const double a=f.lidar->angle_min_rad+static_cast<double>(i)*f.lidar->angle_increment_rad;if(std::abs(a)<=.20&&f.lidar->ranges_m[i]<safety_stop_range_m_)blocked=true;}
        if(blocked){if(!safety_was_active_){++emergency_stops_;++safety_stop_entries_;safety_stop_since_s_=f.timestamp_s;}safety_was_active_=true;safety_due_to_degraded_sensing_=false;return{};}}
    if(safety_was_active_){safety_stop_since_s_=-1.0;safety_was_active_=false;safety_due_to_degraded_sensing_=false;}
    if(goal<=1.5)return{};
    target_waypoint_index_=std::min(target_waypoint_index_,m.waypoints.size()-1);
    while(target_waypoint_index_+1<m.waypoints.size()&&distance(estimated_position_,m.waypoints[target_waypoint_index_])<=8.0)++target_waypoint_index_;
    const auto target=m.waypoints[target_waypoint_index_];const double desired=std::atan2(target.y_m-estimated_position_.y_m,target.x_m-estimated_position_.x_m);
    const double heading_error=wrap_angle(desired-estimated_heading_rad_);
    const double yaw=std::clamp(1.8*heading_error,-m.limits.maximum_yaw_rate_radps,m.limits.maximum_yaw_rate_radps);
    const double turn_speed=std::max(1.0,m.limits.maximum_speed_mps*(1.0-std::min(.8,std::abs(heading_error)/kPi)));
    const double stop_speed=std::sqrt(std::max(0.0,2.0*m.limits.maximum_deceleration_mps2*(goal-.25)));
    return{std::min({m.limits.maximum_speed_mps,turn_speed,stop_speed}),yaw};
}

AutonomySimulation::AutonomySimulation(AutonomyScenario scenario,std::uint64_t seed,std::uint64_t fault_seed):impl_(std::make_unique<Impl>(std::move(scenario),seed,fault_seed)){}
AutonomySimulation::~AutonomySimulation()=default;
AutonomySimulation::AutonomySimulation(AutonomySimulation&&) noexcept=default;
AutonomySimulation&AutonomySimulation::operator=(AutonomySimulation&&) noexcept=default;
const MissionState&AutonomySimulation::mission()const noexcept{return impl_->mission;}
const AirportGraph&AutonomySimulation::graph()const noexcept{return impl_->scenario.airport.graph;}
const std::vector<CircleObstacle>&AutonomySimulation::obstacles()const noexcept{return impl_->scenario.obstacles;}
const VehicleState&AutonomySimulation::state()const noexcept{return impl_->state;}
const EstimatedState&AutonomySimulation::estimated_state()const noexcept{return impl_->estimator.state();}
double AutonomySimulation::time_s()const noexcept{return impl_->time;}
bool AutonomySimulation::finished()const noexcept{return impl_->complete;}
SensorFrame AutonomySimulation::observe()const{return impl_->frame;}
AutonomySnapshot AutonomySimulation::snapshot()const{return{impl_->time,impl_->state,impl_->frame,impl_->mission,impl_->scenario.obstacles,impl_->metrics,impl_->complete};}
bool AutonomySimulation::advance(IAutonomyController& controller){
    if(impl_->complete)return false;
    VehicleCommand command;
    try{command=controller.update(impl_->frame,impl_->mission);}catch(...){impl_->metrics.result=MissionResult::ControllerFailure;impl_->complete=true;return false;}
    if(!impl_->scenario.estimator_enabled)if(const auto* ref=dynamic_cast<const ReferenceController*>(&controller);ref&&ref->has_estimated_position())impl_->evaluate_estimated_pose(ref->estimated_position(),ref->estimated_heading_rad());
    impl_->step(command);
    if(const auto* ref=dynamic_cast<const ReferenceController*>(&controller)){impl_->metrics.emergency_stops=ref->emergency_stops();impl_->metrics.degraded_mode_entries=ref->degraded_mode_entries();impl_->metrics.safety_stop_entries=ref->safety_stop_entries();impl_->metrics.time_stopped_degraded_s=ref->degraded_stop_time_s();}
    if(!impl_->complete&&distance(impl_->state.position,impl_->mission.goal)<=impl_->scenario.goal_tolerance_m&&impl_->state.speed_mps<=impl_->scenario.stopped_speed_mps){impl_->metrics.result=MissionResult::Success;impl_->complete=true;}
    if(!impl_->complete&&impl_->time+1e-9>=impl_->scenario.timeout_s){impl_->metrics.result=MissionResult::Timeout;impl_->complete=true;}
    return !impl_->complete;
}
AutonomyRun AutonomySimulation::result()const{
    auto m=impl_->metrics;m.completion_time_s=impl_->time;m.distance_traveled_m=impl_->state.distance_m;
    if(impl_->estimate_count>0) {
        const double n=static_cast<double>(impl_->estimate_count);
        m.mean_position_error_m=impl_->position_error_sum/n;
        m.rms_position_error_m=std::sqrt(impl_->position_error_squared_sum/n);
        m.mean_heading_error_rad=impl_->heading_error_sum/n;
        m.final_position_error_m=distance(impl_->last_estimated_position,impl_->state.position);
        m.final_heading_error_rad=std::abs(wrap_angle(impl_->last_estimated_heading-impl_->state.heading_rad));
    }
    if(impl_->unavailable_since>=0.0)m.unavailable_duration_s+=std::max(0.0,impl_->time-impl_->unavailable_since);
    m.gnss_delivered=m.gnss_samples-m.gnss_dropped-impl_->delayed_gnss.size();m.imu_delivered=m.imu_samples-m.imu_dropped-impl_->delayed_imu.size();m.odometry_delivered=m.odometry_samples-m.odometry_dropped-impl_->delayed_odometry.size();m.lidar_delivered=m.lidar_scans-m.lidar_dropped-impl_->delayed_lidar.size();
    m.final_x_m=impl_->state.position.x_m;m.final_y_m=impl_->state.position.y_m;m.final_heading_rad=impl_->state.heading_rad;m.final_speed_mps=impl_->state.speed_mps;
    double route=0;for(std::size_t i=1;i<impl_->mission.waypoints.size();++i)route+=distance(impl_->mission.waypoints[i-1],impl_->mission.waypoints[i]);
    m.path_efficiency=m.distance_traveled_m>0?std::min(1.0,route/m.distance_traveled_m):0.0;m.mean_route_error_m=impl_->route_error_count?impl_->route_error_sum/static_cast<double>(impl_->route_error_count):0.0;
    if(!std::isfinite(m.minimum_obstacle_clearance_m))m.minimum_obstacle_clearance_m=0.0;
    m.trajectory_digest=impl_->digest;return{impl_->state,m};
}
AutonomyRun AutonomySimulation::run(IAutonomyController& controller,std::ostream* csv){
    if(csv)*csv<<"time_s,x_m,y_m,estimated_x_m,estimated_y_m,heading_rad,speed_mps,command_speed_mps,command_yaw_rate_radps,route_error_m,lidar_min_m\n";
    while(!impl_->complete){
        VehicleCommand command;
        try{command=controller.update(impl_->frame,impl_->mission);}catch(...){impl_->metrics.result=MissionResult::ControllerFailure;impl_->complete=true;break;}
        if(!impl_->scenario.estimator_enabled)if(const auto* ref=dynamic_cast<const ReferenceController*>(&controller);ref&&ref->has_estimated_position())impl_->evaluate_estimated_pose(ref->estimated_position(),ref->estimated_heading_rad());
        impl_->step(command);
        if(csv){const auto est=impl_->frame.gnss?impl_->frame.gnss->position:impl_->state.position;double lidar=impl_->scenario.sensors.lidar_max_range_m;if(impl_->frame.lidar)for(double r:impl_->frame.lidar->ranges_m)lidar=std::min(lidar,r);*csv<<impl_->time<<','<<impl_->state.position.x_m<<','<<impl_->state.position.y_m<<','<<est.x_m<<','<<est.y_m<<','<<impl_->state.heading_rad<<','<<impl_->state.speed_mps<<','<<command.target_speed_mps<<','<<command.target_yaw_rate_radps<<','<<route_error(impl_->state.position,impl_->mission.waypoints)<<','<<lidar<<'\n';}
        if(!impl_->complete&&distance(impl_->state.position,impl_->mission.goal)<=impl_->scenario.goal_tolerance_m&&impl_->state.speed_mps<=impl_->scenario.stopped_speed_mps){impl_->metrics.result=MissionResult::Success;impl_->complete=true;}
        if(!impl_->complete&&impl_->time+1e-9>=impl_->scenario.timeout_s){impl_->metrics.result=MissionResult::Timeout;impl_->complete=true;}
        if(const auto* ref=dynamic_cast<const ReferenceController*>(&controller)){impl_->metrics.emergency_stops=ref->emergency_stops();impl_->metrics.degraded_mode_entries=ref->degraded_mode_entries();impl_->metrics.safety_stop_entries=ref->safety_stop_entries();impl_->metrics.time_stopped_degraded_s=ref->degraded_stop_time_s();}
    }
    return result();
}
std::string to_string(MissionResult r){switch(r){case MissionResult::Success:return"SUCCESS";case MissionResult::Collision:return"COLLISION";case MissionResult::Timeout:return"TIMEOUT";case MissionResult::ControllerFailure:return"CONTROLLER_FAILURE";}return"UNKNOWN";}
} // namespace airside::autonomy
