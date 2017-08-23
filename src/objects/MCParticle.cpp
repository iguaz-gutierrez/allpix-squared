/**
 * @file
 * @brief Implementation of Monte-Carlo particle object
 * @copyright MIT License
 */

#include "MCParticle.hpp"

using namespace allpix;

MCParticle::MCParticle(ROOT::Math::XYZPoint local_begin_point,
                       ROOT::Math::XYZPoint global_begin_point,
                       ROOT::Math::XYZPoint local_end_point,
                       ROOT::Math::XYZPoint global_end_point,
                       int particle_id)
    : local_begin_point_(std::move(local_begin_point)), global_begin_point_(std::move(global_begin_point)),
      local_end_point_(std::move(local_end_point)), global_end_point_(std::move(global_end_point)),
      particle_id_(particle_id) {
    setParent(nullptr);
}

ROOT::Math::XYZPoint MCParticle::getLocalBeginPoint() const {
    return local_begin_point_;
}
ROOT::Math::XYZPoint MCParticle::getGlobalBeginPoint() const {
    return global_begin_point_;
}

ROOT::Math::XYZPoint MCParticle::getLocalEndPoint() const {
    return local_end_point_;
}
ROOT::Math::XYZPoint MCParticle::getGlobalEndPoint() const {
    return global_end_point_;
}

int MCParticle::getParticleID() const {
    return particle_id_;
}

/**
 * Object is stored as TRef and can only be accessed if pointed object is in scope
 */
const MCParticle* MCParticle::getParent() const {
    return dynamic_cast<MCParticle*>(parent_.GetObject());
}

void MCParticle::setParent(const MCParticle* mc_particle) {
    parent_ = const_cast<MCParticle*>(mc_particle); // NOLINT
}

ClassImp(MCParticle)
