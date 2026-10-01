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
        c.degraded_heading_sigma_rad,c.unsafe_heading_sigma_rad,c.unsafe_without_gnss_s,c.unobserved_stop_deceleration_mps2};
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
    wheel_check_gnss_position_={};wheel_check_gnss_time_s_=-1;wheel_check_odom_distance_m_=0;wheel_check_heading_rad_=0;
    wheel_check_bad_windows_=wheel_check_good_windows_=0;have_wheel_check_anchor_=false;
    reacquisition_candidates_={};reacquisition_candidate_count_=reacquisition_candidate_next_=reacquisition_correction_steps_=0;
    reacquisition_center_={};reacquisition_active_=false;
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
    if(reacquisition_active_||state_.wheel_health==WheelHealth::Degraded||
        (state_.wheel_health==WheelHealth::Suspect&&state_.wheel_inconsistency_count>=3)||
        state_.position_uncertainty_m>=config_.unsafe_position_sigma_m||state_.heading_uncertainty_rad>=config_.unsafe_heading_sigma_rad||
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
        const bool unobserved_stop=state_.time_since_gnss_s>=config_.unsafe_without_gnss_s&&
            (!f.odometry||stale(f.odometry->timestamp_s)||state_.wheel_health==WheelHealth::Degraded);
        double prediction_speed=speed_before;
        if(unobserved_stop) {
            const double stop_sign=speed_before<0.0?-1.0:1.0;
            x[3]=stop_sign*std::max(0.0,std::abs(speed_before)-config_.unobserved_stop_deceleration_mps2*dt);
            prediction_speed=0.5*(speed_before+x[3]);
        }
        x[0]+=prediction_speed*std::cos(yaw_before)*dt;
        x[1]+=prediction_speed*std::sin(yaw_before)*dt;
        at(f_jacobian,0,2)=-speed_before*std::sin(yaw_before)*dt;
        at(f_jacobian,1,2)= speed_before*std::cos(yaw_before)*dt;
        at(f_jacobian,0,3)=std::cos(yaw_before)*dt*(unobserved_stop?0.5:1.0);
        at(f_jacobian,1,3)=std::sin(yaw_before)*dt*(unobserved_stop?0.5:1.0);
        if(valid_imu) x[2]=wrap(x[2]+f.imu->yaw_rate_radps*dt);
        if(new_odom) {
            if(stale(f.odometry->timestamp_s)) {++state_.stale_rejected;last_odom_stamp_s_=f.odometry->timestamp_s;}
            else {
                const auto& o=*f.odometry;
                if(!valid_imu&&have_odom_heading_origin_)
                    scalar_update(x,p,2,wrap(odometry_heading_origin_rad_+o.heading_change_rad),
                        std::max(1e-12,config_.odometry_heading_sigma_rad*config_.odometry_heading_sigma_rad),true);
                const double wheel_variance=std::max(1e-12,config_.odometry_speed_sigma_mps*config_.odometry_speed_sigma_mps)*
                    (state_.wheel_health==WheelHealth::Degraded?10000.0:state_.wheel_health==WheelHealth::Suspect?9.0:1.0);
                if(state_.wheel_health!=WheelHealth::Nominal)++state_.wheel_downweighted;
                scalar_update(x,p,3,o.speed_mps,wheel_variance);
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
                state_.speed_mps=f.odometry->speed_mps;
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
            if(finite(nis))state_.maximum_gnss_nis=std::max(state_.maximum_gnss_nis,nis);
            // Compare independent GNSS displacement with wheel distance over a multi-second window.
            // Requiring meaningful travel and repeated disagreement avoids reacting to one noisy fix.
            const double odom_distance=f.odometry&&!stale(f.odometry->timestamp_s)?f.odometry->distance_m:previous_odom_distance_m_;
            const auto set_wheel_health=[&](WheelHealth health){if(state_.wheel_health!=health){state_.wheel_health=health;++state_.wheel_health_transitions;}};
            if(have_wheel_check_anchor_&&g.timestamp_s-wheel_check_gnss_time_s_>5.0) {
                wheel_check_gnss_position_=g.position;wheel_check_gnss_time_s_=g.timestamp_s;wheel_check_odom_distance_m_=odom_distance;wheel_check_heading_rad_=state_.heading_rad;
            } else if(have_wheel_check_anchor_&&g.timestamp_s-wheel_check_gnss_time_s_>=4.0) {
                const double wheel_delta=std::abs(odom_distance-wheel_check_odom_distance_m_);
                const double gnss_delta=std::hypot(g.position.x_m-wheel_check_gnss_position_.x_m,g.position.y_m-wheel_check_gnss_position_.y_m);
                if(wheel_delta>=2.0&&std::abs(wrap(state_.heading_rad-wheel_check_heading_rad_))<=0.65) {
                    const double ratio=gnss_delta/wheel_delta;
                    if(ratio<0.76||ratio>1.28) {
                        wheel_check_bad_windows_+=1.0;wheel_check_good_windows_=0.0;++state_.wheel_inconsistency_count;
                        set_wheel_health(wheel_check_bad_windows_>=2.0?WheelHealth::Degraded:WheelHealth::Suspect);
                    } else {
                        wheel_check_good_windows_+=1.0;wheel_check_bad_windows_=std::max(0.0,wheel_check_bad_windows_-0.25);
                        if(wheel_check_good_windows_>=3.0){wheel_check_bad_windows_=0.0;set_wheel_health(WheelHealth::Nominal);}
                        else if(state_.wheel_health==WheelHealth::Degraded)set_wheel_health(WheelHealth::Suspect);
                    }
                }
                wheel_check_gnss_position_=g.position;wheel_check_gnss_time_s_=g.timestamp_s;wheel_check_odom_distance_m_=odom_distance;wheel_check_heading_rad_=state_.heading_rad;
            } else if(!have_wheel_check_anchor_) {
                wheel_check_gnss_position_=g.position;wheel_check_gnss_time_s_=g.timestamp_s;wheel_check_odom_distance_m_=odom_distance;wheel_check_heading_rad_=state_.heading_rad;have_wheel_check_anchor_=true;
            }
            last_gnss_stamp_s_=g.timestamp_s;
            if(reacquisition_active_||!finite(nis)||nis>config_.gnss_nis_gate||std::hypot(dx,dy)>2.0){++state_.gnss_rejected;++state_.gate_activations;++state_.gnss_reject_streak;}
            else {
                State x{state_.position.x_m,state_.position.y_m,state_.heading_rad,state_.speed_mps};Matrix p=state_.covariance;
                const double prior_x=x[0],prior_y=x[1];
                scalar_update(x,p,0,g.position.x_m,r);scalar_update(x,p,1,g.position.y_m,r);
                const double correction=std::hypot(x[0]-prior_x,x[1]-prior_y);
                if(correction>0.75){x[0]=prior_x+(x[0]-prior_x)*0.75/correction;x[1]=prior_y+(x[1]-prior_y)*0.75/correction;}
                state_.position={x[0],x[1]};state_.heading_rad=x[2];state_.speed_mps=x[3];state_.covariance=p;
                ++state_.gnss_accepted;last_gnss_time_s_=g.timestamp_s;
                state_.gnss_reject_streak=0;
            }
            if((nis>config_.gnss_nis_gate||std::hypot(dx,dy)>2.0||reacquisition_active_)&&finite(nis)) {
                state_.gnss_recovery=GnssRecoveryState::Inconsistent;
                if(reacquisition_candidate_count_>0) {
                    const auto last=reacquisition_candidates_[(reacquisition_candidate_next_+reacquisition_candidates_.size()-1)%reacquisition_candidates_.size()];
                    if(std::hypot(g.position.x_m-last.x_m,g.position.y_m-last.y_m)>5.0) {
                        reacquisition_candidate_count_=0;++state_.reacquisition_candidates_rejected;
                    }
                }
                reacquisition_candidates_[reacquisition_candidate_next_]=g.position;
                reacquisition_candidate_next_=(reacquisition_candidate_next_+1)%reacquisition_candidates_.size();
                reacquisition_candidate_count_=std::min(reacquisition_candidate_count_+1,reacquisition_candidates_.size());
                if(reacquisition_candidate_count_==reacquisition_candidates_.size()) {
                    Vec2 center{};for(const auto& candidate:reacquisition_candidates_){center.x_m+=candidate.x_m;center.y_m+=candidate.y_m;}
                    center.x_m/=static_cast<double>(reacquisition_candidates_.size());center.y_m/=static_cast<double>(reacquisition_candidates_.size());
                    double spread=0.0;for(const auto& candidate:reacquisition_candidates_)spread=std::max(spread,std::hypot(candidate.x_m-center.x_m,candidate.y_m-center.y_m));
                    if(spread<=3.0) {
                        if(!reacquisition_active_)++state_.reacquisition_attempts;
                        reacquisition_active_=true;state_.gnss_recovery=GnssRecoveryState::Reacquiring;reacquisition_center_=center;
                        const double rx=center.x_m-state_.position.x_m,ry=center.y_m-state_.position.y_m,range=std::hypot(rx,ry);
                        const double step=std::min(0.75,range);
                        if(range>1e-12){state_.position.x_m+=rx/range*step;state_.position.y_m+=ry/range*step;}
                        if(step>0.0)++reacquisition_correction_steps_;
                        state_.covariance[0]=std::max(state_.covariance[0],0.25);state_.covariance[5]=std::max(state_.covariance[5],0.25);
                        if(std::hypot(center.x_m-state_.position.x_m,center.y_m-state_.position.y_m)<=1.0&&reacquisition_correction_steps_>=3) {
                            reacquisition_active_=false;state_.gnss_recovery=GnssRecoveryState::Recovered;++state_.reacquisition_successes;
                            last_gnss_time_s_=g.timestamp_s;
                            state_.gnss_reject_streak=0;
                            state_.covariance[0]=state_.covariance[5]=0.25;state_.covariance[1]=state_.covariance[4]=0.0;
                        }
                    } else {++state_.reacquisition_candidates_rejected;state_.gnss_recovery=GnssRecoveryState::Inconsistent;}
                }
            } else if(state_.gnss_recovery!=GnssRecoveryState::Recovered) {
                state_.gnss_recovery=GnssRecoveryState::Tracking;
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
        if(dt>0.0&&(state_.wheel_health!=WheelHealth::Nominal||state_.gnss_recovery==GnssRecoveryState::Inconsistent||state_.gnss_recovery==GnssRecoveryState::Reacquiring))
            state_.localization_degraded_time_s+=dt;
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
