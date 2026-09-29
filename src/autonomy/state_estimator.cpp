#include "airside/autonomy/state_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace airside::autonomy {
namespace {
constexpr double kPi = 3.14159265358979323846;
using Matrix = std::array<double, 16>;
using State = std::array<double, 4>;
double wrap(double a) noexcept { return std::remainder(a, 2.0 * kPi); }
bool finite(double x) noexcept { return std::isfinite(x); }
double& at(Matrix& p, std::size_t r, std::size_t c) { return p[r * 4 + c]; }
double at(const Matrix& p, std::size_t r, std::size_t c) { return p[r * 4 + c]; }
void scalar_update(State& x, Matrix& p, std::size_t index, double measurement, double variance, bool angular=false) {
    const double innovation = angular ? wrap(measurement - x[index]) : measurement - x[index];
    const double denominator = at(p,index,index) + variance;
    if (!(denominator > 1e-15) || !finite(denominator)) throw std::runtime_error("singular estimator measurement covariance");
    std::array<double,4> k{};
    for (std::size_t r=0;r<4;++r) k[r]=at(p,r,index)/denominator;
    for (std::size_t r=0;r<4;++r) x[r]+=k[r]*innovation;
    x[2]=wrap(x[2]);
    Matrix old=p;
    // Joseph form for scalar observation H=e_index^T.
    for (std::size_t r=0;r<4;++r) for (std::size_t c=0;c<4;++c)
        at(p,r,c)=at(old,r,c)-k[r]*at(old,index,c)-at(old,r,index)*k[c]+k[r]*denominator*k[c];
}
bool valid_config(const EstimatorConfig& c) noexcept {
    const std::array values{c.initial_position_variance_m2,c.initial_heading_variance_rad2,c.initial_speed_variance_m2ps2,
        c.position_process_noise_m2ps,c.heading_process_noise_rad2ps,c.speed_process_noise_m2ps3,c.gnss_sigma_m,
        c.imu_heading_sigma_rad,c.imu_yaw_rate_sigma_radps,c.odometry_speed_sigma_mps,c.odometry_heading_sigma_rad,
        c.gnss_nis_gate,c.maximum_measurement_age_s,c.degraded_position_sigma_m,c.unsafe_position_sigma_m,
        c.degraded_heading_sigma_rad,c.unsafe_heading_sigma_rad,c.unsafe_without_gnss_s};
    return std::ranges::all_of(values, [](double x){return finite(x)&&x>=0.0;}) &&
        c.initial_position_variance_m2>0&&c.initial_heading_variance_rad2>0&&c.initial_speed_variance_m2ps2>0&&
        c.gnss_nis_gate>0&&c.maximum_measurement_age_s>0&&c.unsafe_position_sigma_m>c.degraded_position_sigma_m&&
        c.unsafe_heading_sigma_rad>c.degraded_heading_sigma_rad;
}
}

StateEstimator2D::StateEstimator2D(EstimatorConfig config) : config_(config) {
    if (!valid_config(config_)) throw std::invalid_argument("invalid 2D estimator configuration");
}
void StateEstimator2D::reset() noexcept {
    state_={}; previous_time_s_=-1; previous_odom_distance_m_=previous_odom_heading_change_rad_=0;
    last_imu_stamp_s_=last_odom_stamp_s_=last_gnss_stamp_s_=last_gnss_time_s_=-1;
    last_odom_heading_estimate_rad_=odometry_heading_origin_rad_=0;
    have_odom_=have_imu_=have_odom_heading_origin_=false;
}
void StateEstimator2D::refresh_health(double now) noexcept {
    state_.timestamp_s=now;
    state_.time_since_gnss_s=last_gnss_time_s_<0?std::max(0.0,now):std::max(0.0,now-last_gnss_time_s_);
    const double px=state_.covariance[0], py=state_.covariance[5], ph=state_.covariance[10];
    state_.position_uncertainty_m=std::sqrt(std::max(0.0,px+py));
    state_.heading_uncertainty_rad=std::sqrt(std::max(0.0,ph));
    if (!state_.initialized) { state_.health=EstimatorHealth::Uninitialized; return; }
    if (!finite(state_.position.x_m)||!finite(state_.position.y_m)||!finite(state_.heading_rad)||!finite(state_.speed_mps)||
        !finite(state_.position_uncertainty_m)||!finite(state_.heading_uncertainty_rad)||
        !std::ranges::all_of(state_.covariance,[](double x){return finite(x)&&std::abs(x)<=1e12;})) { state_.health=EstimatorHealth::Invalid; return; }
    if(state_.position_uncertainty_m>=config_.unsafe_position_sigma_m||state_.heading_uncertainty_rad>=config_.unsafe_heading_sigma_rad||
        state_.time_since_gnss_s>=config_.unsafe_without_gnss_s) state_.health=EstimatorHealth::Unsafe;
    else if(state_.position_uncertainty_m>=config_.degraded_position_sigma_m||state_.heading_uncertainty_rad>=config_.degraded_heading_sigma_rad||
        state_.time_since_gnss_s>=0.25||!have_imu_||!have_odom_) state_.health=EstimatorHealth::Degraded;
    else state_.health=EstimatorHealth::Healthy;
}

const EstimatedState& StateEstimator2D::update(const SensorFrame& f) {
    const double now=f.timestamp_s;
    if(!finite(now)||now<0.0||(previous_time_s_>=0.0&&now<previous_time_s_)) { state_.health=EstimatorHealth::Invalid; return state_; }
    const double dt=previous_time_s_<0?0:now-previous_time_s_;
    auto stale=[&](double stamp){return !finite(stamp)||stamp>now+1e-9||now-stamp>config_.maximum_measurement_age_s;};
    if(state_.initialized) {
        State x{state_.position.x_m,state_.position.y_m,state_.heading_rad,state_.speed_mps};
        Matrix p=state_.covariance;
        const bool new_odom=f.odometry&&f.odometry->timestamp_s>last_odom_stamp_s_+1e-9;
        const bool new_imu=f.imu&&f.imu->timestamp_s>last_imu_stamp_s_+1e-9;
        const bool valid_imu=new_imu&&!stale(f.imu->timestamp_s);
        Matrix f_jacobian{};for(std::size_t i=0;i<4;++i)at(f_jacobian,i,i)=1.0;
        // Forward speed is the last wheel-odometry observation. Propagating it each
        // simulation interval provides deliberate dead reckoning during wheel-message loss.
        const double yaw_before=x[2], speed_before=x[3];
        x[0]+=speed_before*std::cos(yaw_before)*dt;
        x[1]+=speed_before*std::sin(yaw_before)*dt;
        at(f_jacobian,0,2)=-speed_before*std::sin(yaw_before)*dt;
        at(f_jacobian,1,2)= speed_before*std::cos(yaw_before)*dt;
        at(f_jacobian,0,3)=std::cos(yaw_before)*dt;
        at(f_jacobian,1,3)=std::sin(yaw_before)*dt;
        if(valid_imu) x[2]=wrap(x[2]+f.imu->yaw_rate_radps*dt);
        if(new_odom) {
            if(stale(f.odometry->timestamp_s)) {++state_.stale_rejected;last_odom_stamp_s_=f.odometry->timestamp_s;}
            else {
                const auto& o=*f.odometry;
                if(!valid_imu&&have_odom_heading_origin_)
                    scalar_update(x,p,2,wrap(odometry_heading_origin_rad_+o.heading_change_rad),
                        std::max(1e-12,config_.odometry_heading_sigma_rad*config_.odometry_heading_sigma_rad),true);
                scalar_update(x,p,3,std::max(0.0,o.speed_mps),
                    std::max(1e-12,config_.odometry_speed_sigma_mps*config_.odometry_speed_sigma_mps));
                previous_odom_distance_m_=o.distance_m;previous_odom_heading_change_rad_=o.heading_change_rad;
                last_odom_stamp_s_=o.timestamp_s;have_odom_=true;last_odom_heading_estimate_rad_=x[2];
            }
        }
        Matrix propagated{};
        for(std::size_t r=0;r<4;++r)for(std::size_t c=0;c<4;++c)
            for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j)
                at(propagated,r,c)+=at(f_jacobian,r,i)*at(p,i,j)*at(f_jacobian,c,j);
        p=propagated;
        at(p,0,0)+=config_.position_process_noise_m2ps*dt;
        at(p,1,1)+=config_.position_process_noise_m2ps*dt;
        at(p,2,2)+=config_.heading_process_noise_rad2ps*dt+
            (valid_imu?config_.imu_yaw_rate_sigma_radps*config_.imu_yaw_rate_sigma_radps*dt*dt:0.0);
        at(p,3,3)+=config_.speed_process_noise_m2ps3*dt;
        if(new_imu) {
            const auto& imu=*f.imu;
            if(stale(imu.timestamp_s)) ++state_.stale_rejected;
            else {
                scalar_update(x,p,2,imu.heading_rad, std::max(1e-12,config_.imu_heading_sigma_rad*config_.imu_heading_sigma_rad),true);
                last_imu_stamp_s_=imu.timestamp_s;have_imu_=true;
            }
        }
        state_.position={x[0],x[1]};state_.heading_rad=x[2];state_.speed_mps=x[3];state_.covariance=p;
    }
    if(!state_.initialized&&f.imu&& !stale(f.imu->timestamp_s)) {
        state_.heading_rad=wrap(f.imu->heading_rad);last_imu_stamp_s_=f.imu->timestamp_s;have_imu_=true;
    }
    if(f.gnss&&f.gnss->timestamp_s>last_gnss_stamp_s_+1e-9) {
        const auto& g=*f.gnss;
        if(stale(g.timestamp_s)) ++state_.stale_rejected;
        else if(!finite(g.position.x_m)||!finite(g.position.y_m)||!finite(g.accuracy_m)||g.accuracy_m<0) ++state_.gnss_rejected;
        else if(!state_.initialized&&f.imu&&!stale(f.imu->timestamp_s)) {
            state_.position=g.position;state_.initialized=true;
            if(f.odometry&&!stale(f.odometry->timestamp_s)) {
                state_.speed_mps=std::max(0.0,f.odometry->speed_mps);
                last_odom_stamp_s_=f.odometry->timestamp_s;
                previous_odom_distance_m_=f.odometry->distance_m;
                previous_odom_heading_change_rad_=f.odometry->heading_change_rad;
                have_odom_=true;
                odometry_heading_origin_rad_=wrap(state_.heading_rad-f.odometry->heading_change_rad);
                have_odom_heading_origin_=true;
            }
            state_.covariance.fill(0.0);state_.covariance[0]=state_.covariance[5]=std::max(1e-6,config_.gnss_sigma_m*config_.gnss_sigma_m);
            state_.covariance[10]=config_.initial_heading_variance_rad2;state_.covariance[15]=config_.initial_speed_variance_m2ps2;
            ++state_.gnss_accepted;last_gnss_stamp_s_=g.timestamp_s;last_gnss_time_s_=g.timestamp_s;
            last_imu_stamp_s_=f.imu->timestamp_s;have_imu_=true;
        } else if(state_.initialized) {
            const double r=std::max(config_.gnss_sigma_m*config_.gnss_sigma_m,(g.accuracy_m/1.96)*(g.accuracy_m/1.96));
            const double s00=state_.covariance[0]+r,s01=state_.covariance[1],s10=state_.covariance[4],s11=state_.covariance[5]+r;
            const double det=s00*s11-s01*s10;
            const double dx=g.position.x_m-state_.position.x_m,dy=g.position.y_m-state_.position.y_m;
            const double nis=det>1e-15?(dx*(s11*dx-s01*dy)+dy*(-s10*dx+s00*dy))/det:std::numeric_limits<double>::infinity();
            state_.last_gnss_nis=nis;state_.maximum_gnss_innovation_m=std::max(state_.maximum_gnss_innovation_m,std::hypot(dx,dy));
            last_gnss_stamp_s_=g.timestamp_s;
            if(!finite(nis)||nis>config_.gnss_nis_gate){++state_.gnss_rejected;++state_.gate_activations;}
            else {
                State x{state_.position.x_m,state_.position.y_m,state_.heading_rad,state_.speed_mps};Matrix p=state_.covariance;
                scalar_update(x,p,0,g.position.x_m,r);scalar_update(x,p,1,g.position.y_m,r);
                state_.position={x[0],x[1]};state_.heading_rad=x[2];state_.speed_mps=x[3];state_.covariance=p;
                ++state_.gnss_accepted;last_gnss_time_s_=g.timestamp_s;
            }
        }
    }
    if(state_.initialized) {
        if(f.odometry&&f.odometry->timestamp_s>last_odom_stamp_s_+1e-9) {
            if(stale(f.odometry->timestamp_s)) ++state_.stale_rejected;
            else {
                previous_odom_distance_m_=f.odometry->distance_m;previous_odom_heading_change_rad_=f.odometry->heading_change_rad;
                have_odom_=true;
                if(!have_odom_heading_origin_) {
                    odometry_heading_origin_rad_=wrap(state_.heading_rad-f.odometry->heading_change_rad);
                    have_odom_heading_origin_=true;
                }
                last_odom_heading_estimate_rad_=state_.heading_rad;
            }
            last_odom_stamp_s_=f.odometry->timestamp_s;
        }
        for(std::size_t r=0;r<4;++r) {
            at(state_.covariance,r,r)=std::max(1e-12,at(state_.covariance,r,r));
            for(std::size_t c=r+1;c<4;++c) {
                const double symmetric=0.5*(at(state_.covariance,r,c)+at(state_.covariance,c,r));
                at(state_.covariance,r,c)=at(state_.covariance,c,r)=symmetric;
            }
        }
        refresh_health(now);
    } else {state_.timestamp_s=now;state_.health=EstimatorHealth::Uninitialized;}
    previous_time_s_=now;
    return state_;
}

const char* to_string(EstimatorHealth h) noexcept {
    switch(h){case EstimatorHealth::Uninitialized:return "uninitialized";case EstimatorHealth::Healthy:return "healthy";
    case EstimatorHealth::Degraded:return "degraded";case EstimatorHealth::Unsafe:return "unsafe";case EstimatorHealth::Invalid:return "invalid";}
    return "invalid";
}
} // namespace airside::autonomy
