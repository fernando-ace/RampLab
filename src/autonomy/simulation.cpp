#include "airside/autonomy/simulation.hpp"

#include "airside/routing/astar.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
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
    std::mt19937_64 random;
    std::normal_distribution<double> normal{0.0, 1.0};
    MissionState mission;
    VehicleState state;
    double time{0.0}, previous_accel{0.0}, initial_heading{0.0};
    SensorFrame frame;
    double next_gnss{0.0}, next_imu{0.0}, next_odom{0.0}, next_lidar{0.0};
    bool complete{false}, safety_was_active{false};
    MissionMetrics metrics;
    std::size_t emergency_stops{0}, route_error_count{0};
    double route_error_sum{0.0};
    std::uint64_t digest{14695981039346656037ULL};

    Impl(AutonomyScenario input, std::uint64_t seed) : scenario(std::move(input)), random(seed), state(scenario.initial_state) {
        const auto& sensor=scenario.sensors;
        if (!(scenario.timestep_s>0.0) || !(scenario.timeout_s>0.0) || !(scenario.limits.maximum_speed_mps>0.0) ||
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
        if(time+1e-9>=next_imu){frame.imu=ImuMeasurement{time,wrap_angle(state.heading_rad+noise(c.imu_heading_sigma_rad)),state.yaw_rate_radps+noise(c.imu_yaw_rate_sigma_radps),previous_accel+noise(c.imu_accel_sigma_mps2)};next_imu+=1.0/c.imu_hz;++metrics.imu_samples;}
        if(time+1e-9>=next_odom){frame.odometry=OdometryMeasurement{time,state.distance_m+noise(c.odometry_sigma_m),std::max(0.0,state.speed_mps+noise(c.odometry_sigma_mps)),wrap_angle(state.heading_rad-initial_heading)};next_odom+=1.0/c.odometry_hz;++metrics.odometry_samples;}
        if(time+1e-9>=next_lidar){frame.lidar=make_lidar();next_lidar+=1.0/c.lidar_hz;++metrics.lidar_scans;}
        if(time+1e-9>=next_gnss){frame.gnss=GnssMeasurement{time,{state.position.x_m+c.gnss_bias_m.x_m+noise(c.gnss_sigma_m),state.position.y_m+c.gnss_bias_m.y_m+noise(c.gnss_sigma_m)},c.gnss_sigma_m*1.96};next_gnss+=1.0/c.gnss_hz;++metrics.gnss_samples;}
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

ReferenceController::ReferenceController(double stopping):safety_stop_range_m_(stopping){}
VehicleCommand ReferenceController::update(const SensorFrame& f,const MissionState&m){
    if(f.odometry && f.odometry->timestamp_s != last_odometry_timestamp_s_){
        if(have_position_){const double delta=f.odometry->distance_m-last_odometry_distance_m_;estimated_position_.x_m+=delta*std::cos(estimated_heading_rad_);estimated_position_.y_m+=delta*std::sin(estimated_heading_rad_);}
        last_odometry_distance_m_=f.odometry->distance_m;last_odometry_timestamp_s_=f.odometry->timestamp_s;
    }
    if(f.gnss && f.gnss->timestamp_s != last_gnss_timestamp_s_){
        if(!have_position_)estimated_position_=f.gnss->position;
        else{estimated_position_.x_m=.85*estimated_position_.x_m+.15*f.gnss->position.x_m;estimated_position_.y_m=.85*estimated_position_.y_m+.15*f.gnss->position.y_m;}
        have_position_=true;last_gnss_timestamp_s_=f.gnss->timestamp_s;
    }
    if(f.imu)estimated_heading_rad_=f.imu->heading_rad;
    const double goal=distance(estimated_position_,m.goal);
    if(f.lidar){bool blocked=false;for(std::size_t i=0;i<f.lidar->ranges_m.size();++i){const double a=f.lidar->angle_min_rad+static_cast<double>(i)*f.lidar->angle_increment_rad;if(std::abs(a)<=.20&&f.lidar->ranges_m[i]<safety_stop_range_m_)blocked=true;}
        if(blocked){if(!safety_was_active_)++emergency_stops_;safety_was_active_=true;return{};}safety_was_active_=false;}
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

AutonomySimulation::AutonomySimulation(AutonomyScenario scenario,std::uint64_t seed):impl_(std::make_unique<Impl>(std::move(scenario),seed)){}
AutonomySimulation::~AutonomySimulation()=default;
AutonomySimulation::AutonomySimulation(AutonomySimulation&&) noexcept=default;
AutonomySimulation&AutonomySimulation::operator=(AutonomySimulation&&) noexcept=default;
const MissionState&AutonomySimulation::mission()const noexcept{return impl_->mission;}
const AirportGraph&AutonomySimulation::graph()const noexcept{return impl_->scenario.airport.graph;}
const std::vector<CircleObstacle>&AutonomySimulation::obstacles()const noexcept{return impl_->scenario.obstacles;}
const VehicleState&AutonomySimulation::state()const noexcept{return impl_->state;}
double AutonomySimulation::time_s()const noexcept{return impl_->time;}
bool AutonomySimulation::finished()const noexcept{return impl_->complete;}
SensorFrame AutonomySimulation::observe()const{return impl_->frame;}
AutonomySnapshot AutonomySimulation::snapshot()const{return{impl_->time,impl_->state,impl_->frame,impl_->mission,impl_->scenario.obstacles,impl_->metrics,impl_->complete};}
bool AutonomySimulation::advance(IAutonomyController& controller){
    if(impl_->complete)return false;
    VehicleCommand command;
    try{command=controller.update(impl_->frame,impl_->mission);}catch(...){impl_->metrics.result=MissionResult::ControllerFailure;impl_->complete=true;return false;}
    impl_->step(command);
    if(const auto* ref=dynamic_cast<const ReferenceController*>(&controller))impl_->metrics.emergency_stops=ref->emergency_stops();
    if(!impl_->complete&&distance(impl_->state.position,impl_->mission.goal)<=impl_->scenario.goal_tolerance_m&&impl_->state.speed_mps<=impl_->scenario.stopped_speed_mps){impl_->metrics.result=MissionResult::Success;impl_->complete=true;}
    if(!impl_->complete&&impl_->time+1e-9>=impl_->scenario.timeout_s){impl_->metrics.result=MissionResult::Timeout;impl_->complete=true;}
    return !impl_->complete;
}
AutonomyRun AutonomySimulation::result()const{
    auto m=impl_->metrics;m.completion_time_s=impl_->time;m.distance_traveled_m=impl_->state.distance_m;
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
        impl_->step(command);
        if(csv){const auto est=impl_->frame.gnss?impl_->frame.gnss->position:impl_->state.position;double lidar=impl_->scenario.sensors.lidar_max_range_m;if(impl_->frame.lidar)for(double r:impl_->frame.lidar->ranges_m)lidar=std::min(lidar,r);*csv<<impl_->time<<','<<impl_->state.position.x_m<<','<<impl_->state.position.y_m<<','<<est.x_m<<','<<est.y_m<<','<<impl_->state.heading_rad<<','<<impl_->state.speed_mps<<','<<command.target_speed_mps<<','<<command.target_yaw_rate_radps<<','<<route_error(impl_->state.position,impl_->mission.waypoints)<<','<<lidar<<'\n';}
        if(!impl_->complete&&distance(impl_->state.position,impl_->mission.goal)<=impl_->scenario.goal_tolerance_m&&impl_->state.speed_mps<=impl_->scenario.stopped_speed_mps){impl_->metrics.result=MissionResult::Success;impl_->complete=true;}
        if(!impl_->complete&&impl_->time+1e-9>=impl_->scenario.timeout_s){impl_->metrics.result=MissionResult::Timeout;impl_->complete=true;}
        if(const auto* ref=dynamic_cast<const ReferenceController*>(&controller))impl_->metrics.emergency_stops=ref->emergency_stops();
    }
    return result();
}
std::string to_string(MissionResult r){switch(r){case MissionResult::Success:return"SUCCESS";case MissionResult::Collision:return"COLLISION";case MissionResult::Timeout:return"TIMEOUT";case MissionResult::ControllerFailure:return"CONTROLLER_FAILURE";}return"UNKNOWN";}
} // namespace airside::autonomy
