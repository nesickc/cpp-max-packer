#pragma once

#include "spectrapack/geometry/validation.hpp"

namespace spectrapack::geometry {

struct ValidationContext::Storage {
  std::shared_ptr<const AcceptedSolid> object_;
  Container container_;
  Constraints constraints_;
  Storage(std::shared_ptr<const AcceptedSolid> object, Container container,
          const Constraints& constraints)
      : object_(std::move(object)), container_(std::move(container)), constraints_(constraints) {}
};

struct Candidate::Storage {
  std::shared_ptr<const ValidationContext> context_;
  std::vector<CopyPose> copies_;
  Storage(std::shared_ptr<const ValidationContext> context,
          const std::vector<CopyPose>& copies)
      : context_(std::move(context)), copies_(copies) {}
};

}  // namespace spectrapack::geometry
