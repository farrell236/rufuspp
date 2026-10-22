/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <iterator>
#include <utility>

#include "rufus/core/raw_image_writer.hpp"

namespace rufus::backend::detail {

// Run both before and after unmounting: a stable ID alone does not establish
// that the newly observed device is still an eligible write target.
inline core::SafetyCheckResult validateRawWriteTarget(
    const core::RawWritePlan& plan, const core::BlockDeviceInfo& observed) {
  const core::SafetyPolicy policy;
  auto result = policy.validateIdentity(plan.target(), observed);
  auto writeSafety = policy.validateWrite(plan.image(), observed);
  result.issues.insert(result.issues.end(),
      std::make_move_iterator(writeSafety.issues.begin()),
      std::make_move_iterator(writeSafety.issues.end()));
  return result;
}

// Native finalization (source revalidation and partition-table refresh) is part
// of the operation. Suppress the copy engine's Complete until it also succeeds.
template <typename Write, typename Finalize>
core::RawWriteResult finishNativeWrite(const core::RawWritePlan& plan,
                                       const Write& write, const Finalize& finalize,
                                       const core::RawWriteProgressCallback& onProgress) {
  auto result = write([&](const core::RawWriteProgress& progress) {
    if (onProgress && progress.stage != core::RawWriteStage::Complete) onProgress(progress);
  });
  if (result.success && !finalize(result.error)) result.success = false;
  if (result.success && onProgress) {
    onProgress({core::RawWriteStage::Complete, plan.bytesToWrite(), plan.bytesToWrite()});
  }
  return result;
}

}  // namespace rufus::backend::detail
