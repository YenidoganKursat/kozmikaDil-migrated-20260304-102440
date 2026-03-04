#pragma once

#include "spark/core/dto/platform_support_dto.h"
#include "spark/core/driver/platform_support_driver.h"
#include "spark/core/manager/platform_support_manager.h"
#include "spark/core/servis/platform_support_servis.h"

namespace spark::core::logic {

class PlatformSupportLogic {
 public:
  PlatformSupportLogic(driver::PlatformSupportDriver& driver, manager::PlatformSupportManager& manager,
                       servis::PlatformSupportServis& servis)
      : driver_(driver), manager_(manager), servis_(servis) {}

  bool execute(const dto::PlatformSupportRequest& request, dto::PlatformSupportResponse& out_response) const;

 private:
  driver::PlatformSupportDriver& driver_;
  manager::PlatformSupportManager& manager_;
  servis::PlatformSupportServis& servis_;
};

}  // namespace spark::core::logic
