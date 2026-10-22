/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QIcon>
#include <QLabel>
#include <QStringList>
#include <QTimer>
#include <QToolButton>

#include <cstdio>
#include <algorithm>
#include <limits>
#include <memory>

#include "main_window.hpp"
#include "rufus/core/application_info.hpp"
#include "secure_boot_assets.hpp"
#include "uefi_shell_assets.hpp"
#include "rufus/core/iso_deployment.hpp"
#include "rufus/core/raw_image_writer.hpp"
#include "rufus/core/format.hpp"
#include "rufus/backend/ntfs_iso_image_stager.hpp"

namespace {

constexpr int kAdaptiveWindowProbePollMilliseconds = 25;
constexpr int kAdaptiveWindowProbeTimeoutMilliseconds = 3000;

class AdaptiveWindowProbe final
    : public std::enable_shared_from_this<AdaptiveWindowProbe> {
 public:
  AdaptiveWindowProbe(QApplication& application,
                      rufus::qt::MainWindow& window)
      : application_(application), window_(window) {}

  void advance() {
    switch (stage_) {
      case 0:
        if (!waitFor(fixedAtCurrentSize(),
                     "Rufus++ must be fixed to its content-wrapped size")) {
          return;
        }
        if (!findControls()) {
          fail("Unable to find the adaptive-window controls");
          return;
        }
        baselineSize_ = window_.size();
        driveAdvanced_->setChecked(true);
        beginStage(1);
        return;
      case 1:
        if (!waitFor(fixedAtCurrentSize() &&
                         window_.width() == baselineSize_.width() &&
                         window_.height() > baselineSize_.height(),
                     "Expanding drive properties did not grow the window vertically")) {
          return;
        }
        driveAdvanced_->setChecked(false);
        beginStage(2);
        return;
      case 2:
        if (!waitFor(fixedAtCurrentSize() && window_.size() == baselineSize_,
                     "Collapsing drive properties did not restore the wrapped size")) {
          return;
        }
        formatAdvanced_->setChecked(false);
        beginStage(3);
        return;
      case 3:
        if (!waitFor(fixedAtCurrentSize() &&
                         window_.width() == baselineSize_.width() &&
                         window_.height() < baselineSize_.height(),
                     "Collapsing format options did not shrink the window vertically")) {
          return;
        }
        formatAdvanced_->setChecked(true);
        beginStage(4);
        return;
      case 4:
        if (!waitFor(fixedAtCurrentSize() && window_.size() == baselineSize_,
                     "Expanding format options did not restore the wrapped size")) {
          return;
        }
        logButton_->click();
        beginStage(5);
        return;
      case 5:
        if (!waitFor(fixedAtCurrentSize() &&
                         window_.width() == baselineSize_.width() &&
                         window_.height() > baselineSize_.height(),
                     "Showing the operation log did not grow the window vertically")) {
          return;
        }
        application_.exit(0);
        return;
      default:
        fail("Adaptive-window test entered an invalid state");
    }
  }

 private:
  [[nodiscard]] bool fixedAtCurrentSize() const {
    return window_.minimumSize() == window_.maximumSize() &&
           window_.size() == window_.minimumSize();
  }

  [[nodiscard]] bool findControls() {
    const auto buttons = window_.findChildren<QToolButton*>();
    for (QToolButton* button : buttons) {
      if (button->text().contains("advanced drive properties")) {
        driveAdvanced_ = button;
      } else if (button->text().contains("advanced format options")) {
        formatAdvanced_ = button;
      } else if (button->toolTip() == "Show or hide log") {
        logButton_ = button;
      }
    }
    return driveAdvanced_ != nullptr && formatAdvanced_ != nullptr &&
           logButton_ != nullptr;
  }

  void queueProbe() {
    const auto self = shared_from_this();
    QTimer::singleShot(kAdaptiveWindowProbePollMilliseconds, &application_,
                       [self] { self->advance(); });
  }

  void beginStage(const int stage) {
    stage_ = stage;
    stageWaitMilliseconds_ = 0;
    queueProbe();
  }

  [[nodiscard]] bool waitFor(const bool condition, const char* failureMessage) {
    if (condition) {
      return true;
    }
    stageWaitMilliseconds_ += kAdaptiveWindowProbePollMilliseconds;
    if (stageWaitMilliseconds_ >= kAdaptiveWindowProbeTimeoutMilliseconds) {
      fail(failureMessage);
    } else {
      queueProbe();
    }
    return false;
  }

  void fail(const char* message) {
    std::fprintf(
        stderr,
        "%s [stage=%d, current=%dx%d, minimum=%dx%d, maximum=%dx%d, "
        "baseline=%dx%d, drive-panel=%s, format-panel=%s]\n",
        message, stage_, window_.width(), window_.height(),
        window_.minimumWidth(), window_.minimumHeight(),
        window_.maximumWidth(), window_.maximumHeight(), baselineSize_.width(),
        baselineSize_.height(),
        driveAdvanced_ != nullptr && driveAdvanced_->isChecked() ? "open"
                                                                 : "closed",
        formatAdvanced_ != nullptr && formatAdvanced_->isChecked() ? "open"
                                                                   : "closed");
    std::fflush(stderr);
    qCritical().noquote() << message;
    application_.exit(1);
  }

  QApplication& application_;
  rufus::qt::MainWindow& window_;
  QToolButton* driveAdvanced_{};
  QToolButton* formatAdvanced_{};
  QToolButton* logButton_{};
  QSize baselineSize_;
  int stage_{};
  int stageWaitMilliseconds_{};
};

}  // namespace

namespace rufus::qt {

// Exercise the same sizing path as START preflight, without preparing an image
// or starting a device operation. Check rendered reports and receipt contents.
class PreflightSizingProbe final {
 public:
  static bool verify() {
    constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    for (const auto fileSystem : {core::IsoDeploymentFilesystem::Fat32,
                                  core::IsoDeploymentFilesystem::Ntfs}) {
      for (const bool quick : {true, false}) {
        core::DeploymentPreflightInput input;
        input.target.stableId = "qt-test:no-device-opened";
        input.target.writable = input.target.wholeDevice = true;
        input.target.capacityBytes = 32U * gib;
        input.image.expandedSizeBytes = 10U * gib;
        input.bytesToWrite = input.temporaryBytes = input.target.capacityBytes;
        input.quickFormat = quick;
        MainWindow::configureIsoPreflightSizing(input, fileSystem);
        bool pending = quick;
        std::uint64_t temporary = input.target.capacityBytes;
        if (fileSystem == core::IsoDeploymentFilesystem::Ntfs) {
          const auto requirements = backend::estimateNtfsIsoStagingRequirements(
              input.image.expandedSizeBytes, input.target.capacityBytes, quick);
          if (!requirements.success) return false;
          temporary = requirements.temporaryBytes;
#if defined(_WIN32)
          pending = false;
#endif
        }
        const auto report = core::buildDeploymentPreflight(input);
        const auto value = [&report](const std::string& label) {
          const auto found = std::find_if(report.entries.begin(), report.entries.end(),
              [&label](const auto& entry) { return entry.label == label; });
          return found == report.entries.end() ? std::string{} : found->value;
        };
        const std::string writeSize = pending ? "Calculated during preparation"
            : core::formatByteSize(input.target.capacityBytes);
        const auto temporarySize = core::formatByteSize(temporary);
        core::DeploymentReceipt receipt;
        receipt.preflight = report;
        if (!report.ready() || input.writeSizePending != pending ||
            input.temporaryBytes != temporary || value("Write size") != writeSize ||
            value("Temporary space") != temporarySize ||
            receipt.toJson().find(temporarySize) == std::string::npos ||
            receipt.toJson().find(writeSize) == std::string::npos) {
          qCritical() << "ISO preflight and receipt must report backend space and actual write-size policy";
          qCritical().noquote() << QString::fromStdString(report.toText());
          return false;
        }
      }
    }
    core::DeploymentPreflightInput overflow;
    overflow.target.stableId = "qt-test:no-device-opened";
    overflow.target.writable = overflow.target.wholeDevice = true;
    overflow.target.capacityBytes = 32U * gib;
    overflow.image.expandedSizeBytes = std::numeric_limits<std::uint64_t>::max();
    overflow.quickFormat = true;
    MainWindow::configureIsoPreflightSizing(overflow, core::IsoDeploymentFilesystem::Ntfs);
    if (core::buildDeploymentPreflight(overflow).ready() || overflow.blockers.empty() ||
        overflow.temporaryBytes != 0) {
      qCritical() << "Unrepresentable NTFS space requirements must block preflight";
      return false;
    }
    return true;
  }
};

// Exercise real progress callbacks/widgets without starting a device operation.
class ProgressDisplayProbe final {
 public:
  static bool verify(MainWindow& window, const QString& screenshot) {
    const auto check = [](const bool condition, const char* message) {
      if (!condition) qCritical() << message;
      return condition;
    };
    auto* label = window.findChild<QLabel*>("operationProgress");
    if (!check(label != nullptr, "Progress must have a separate text widget")) return false;
    window.operationElapsed_.start();
    window.progressRefreshTimer_->start();
    constexpr std::uint64_t mib = 1024U * 1024U;
    window.handleWriteProgress({core::RawWriteStage::Writing, mib, 4U * mib});
    QApplication::processEvents();
    if (!check(label->isVisible() && label->text().contains("Writing image — 25%") &&
                   label->text().contains("1.0 MiB of 4.0 MiB"),
               "Writing bytes and phase percentage must be visible outside the bar")) return false;
    window.handleWriteProgress({core::RawWriteStage::Verifying, 0, 4U * mib});
    if (!check(label->text().contains("Verifying image — 0%") &&
                   label->text().contains("0 bytes of 4.0 MiB"),
               "Verification must reset its byte counter independently of writing")) return false;
    window.handleWriteProgress({core::RawWriteStage::Flushing, 4U * mib, 4U * mib});
    const auto callbackTime = window.lastOperationProgressMs_;
    if (!check(label->text().contains("Flushing device") && !label->text().contains("100%"),
               "Flushing must not claim the operation is complete")) return false;
    QEventLoop wait;
    QTimer changed;
    changed.setInterval(25);
    QObject::connect(&changed, &QTimer::timeout, &wait, [&] {
      if (!label->text().contains("Elapsed 00:00")) wait.quit();
    });
    changed.start();
    QTimer::singleShot(4000, &wait, &QEventLoop::quit);
    wait.exec();
    if (!check(!label->text().contains("Elapsed 00:00") &&
                   label->text().contains("Flushing device") &&
                   window.lastOperationProgressMs_ == callbackTime,
               "Elapsed time must advance without inventing a device progress callback")) return false;
    window.handleIsoDeploymentProgress({core::IsoDeploymentStage::Extracting, mib, 4U * mib, "casper/filesystem.squashfs"});
    if (!check(label->text().contains("25%") && label->text().contains("1.0 MiB of 4.0 MiB"),
               "ISO preparation must expose its byte counts as well")) return false;
    window.handleIsoDeploymentProgress({core::IsoDeploymentStage::Complete, 4U * mib, 4U * mib, {}});
    if (!check(label->text().startsWith("Image prepared"),
               "Staging completion must not be presented as completed USB writing")) return false;
    window.handleMacOsInstallerProgress({core::MacOsInstallerStage::CreatingInstaller, 50, 100, {}});
    if (!check(label->text().contains("50%") && !label->text().contains("bytes"),
               "Apple's installer percentage must not be mislabeled as byte counts")) return false;
    window.endOperationGuard();
    if (!check(!window.progressRefreshTimer_->isActive() && !window.operationElapsed_.isValid(),
               "The elapsed display must stop with the operation")) return false;
    if (!screenshot.isEmpty()) {
      window.isoDeploymentRunning_ = true;
      window.handleWriteProgress({core::RawWriteStage::Writing, 6ULL * 1024U * mib, 16ULL * 1024U * mib});
      QApplication::processEvents();
      window.fitWindowToContents();
      if (!window.grab().save(screenshot)) return false;
    }
    return true;
  }
};

}  // namespace rufus::qt

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(
      QString::fromUtf8(rufus::core::ApplicationInfo::name.data()));
  application.setApplicationDisplayName(
      QString::fromUtf8(rufus::core::ApplicationInfo::displayName.data()));
  application.setApplicationVersion(
      QString::fromUtf8(rufus::core::ApplicationInfo::version.data()));
  application.setOrganizationName(
      QString::fromUtf8(rufus::core::ApplicationInfo::organization.data()));
  application.setWindowIcon(QIcon(":/rufus/icons/rufus-128.png"));

  if (application.arguments().contains("--verify-embedded-assets")) {
    QString shellReport;
    QString secureBootReport;
    if (!rufus::qt::verifyBundledUefiShellAssets(shellReport)) {
      qCritical().noquote() << shellReport;
      return 1;
    }
    if (!rufus::qt::verifyBundledSecureBootAssets(secureBootReport)) {
      qCritical().noquote() << secureBootReport;
      return 1;
    }
    qInfo().noquote() << shellReport;
    qInfo().noquote() << secureBootReport;
    return 0;
  }

  rufus::qt::MainWindow window;
  window.show();

  const auto progressArgument = application.arguments().indexOf("--verify-progress-display");
  if (progressArgument >= 0) {
    const QString screenshot = progressArgument + 1 < application.arguments().size()
                                   ? application.arguments().at(progressArgument + 1) : QString{};
    return rufus::qt::ProgressDisplayProbe::verify(window, screenshot) ? 0 : 1;
  }

  if (application.arguments().contains("--verify-preflight-sizing")) {
    return rufus::qt::PreflightSizingProbe::verify() ? 0 : 1;
  }

  const qsizetype documentationArgument =
      application.arguments().indexOf("--generate-documentation-screenshots");
  if (documentationArgument >= 0) {
    const QStringList arguments = application.arguments();
    if (documentationArgument + 1 >= arguments.size()) {
      qCritical() << "--generate-documentation-screenshots requires an output directory";
      return 2;
    }
    QString error;
    if (!window.generateDocumentationScreenshots(
            arguments.at(documentationArgument + 1), error)) {
      qCritical().noquote() << error;
      return 1;
    }
    qInfo().noquote()
        << "Documentation screenshots written to"
        << arguments.at(documentationArgument + 1);
    return 0;
  }

  if (application.arguments().contains("--verify-privilege-indicator")) {
    const auto* indicator = window.findChild<QLabel*>("privilegeIndicator");
    const QStringList validLabels{
        "UNPRIVILEGED", "ELEVATED", "PRIVILEGED HELPER"};
    if (indicator == nullptr || !validLabels.contains(indicator->text()) ||
        indicator->toolTip().isEmpty()) {
      qCritical() << "The bottom-right privilege indicator is missing or invalid";
      return 1;
    }
    for (const QString& marker : validLabels) {
      if (window.windowTitle().contains(marker)) {
        qCritical() << "Privilege state must not appear in the window title";
        return 1;
      }
    }
    if (window.windowTitle().contains("UNSIGNED ROOT BUILD") ||
        window.windowTitle().contains("RESTRICTED BUILD")) {
      qCritical() << "A legacy build-state marker remains in the window title";
      return 1;
    }
    QTimer::singleShot(0, &application, &QApplication::quit);
  }

  std::shared_ptr<AdaptiveWindowProbe> sizingProbe;
  if (application.arguments().contains("--verify-window-sizing")) {
    sizingProbe = std::make_shared<AdaptiveWindowProbe>(application, window);
    QTimer::singleShot(kAdaptiveWindowProbePollMilliseconds, &application,
                       [sizingProbe] { sizingProbe->advance(); });
  }

  if (application.arguments().contains("--smoke-test")) {
    QTimer::singleShot(0, &application, &QApplication::quit);
  }

  return application.exec();
}
