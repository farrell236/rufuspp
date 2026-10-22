/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QMainWindow>
#include <QElapsedTimer>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "rufus/core/media.hpp"
#include "rufus/core/macos_installer.hpp"
#include "rufus/core/deployment_quality.hpp"
#include "rufus/core/secure_boot_analyzer.hpp"
#include "rufus/core/windows_to_go.hpp"

class QComboBox;
class QCheckBox;
class QCloseEvent;
class QLineEdit;
class QLabel;
class QProgressBar;
class QProcess;
class QPushButton;
class QSlider;
class QSpinBox;
class QTextEdit;
class QThread;
class QToolButton;
class QWidget;

namespace rufus::backend {
class BlockDeviceBackend;
class NtfsIsoImageStager;
class StandaloneFilesystemStager;
class WindowsToGoImageStager;
struct WindowsToGoProgress;
}

namespace rufus::core {
enum class IsoDeploymentFilesystem;
class ImageAnalyzer;
struct IsoDeploymentProgress;
struct IsoDeploymentOptions;
struct ImageAnalysisResult;
struct BadBlockTestProgress;
struct MediaCaptureProgress;
struct StandaloneMediaProgress;
struct RawWriteProgress;
struct RawWriteResult;
struct RuntimeUefiValidationAssets;
}

namespace rufus::qt {

class MainWindow final : public QMainWindow {
 Q_OBJECT

  friend class ProgressDisplayProbe;
  friend class PreflightSizingProbe;

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  // Renders the real Qt widgets in deterministic, non-destructive demo states
  // for the checked-in user guide. No device is opened and no source is read.
  [[nodiscard]] bool generateDocumentationScreenshots(
      const QString& outputDirectory, QString& error);

 protected:
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void chooseImage();
  void calculateChecksums();
  void captureDevice();
  void formatDevice();
  void closeOrCancel();
  void configureWindowsExperience();
  void pollVolumes();
  void refreshVolumes();
  void showDiagnostics();
  void inspectSelectedDevice();
  void analyzeSelectedImageTrust();
  void runVirtualBootTest();
  void startWrite();
  void toggleDriveAdvanced(bool expanded);
  void toggleFormatAdvanced(bool expanded);
  void toggleLog();

 private:
  static void configureIsoPreflightSizing(
      core::DeploymentPreflightInput& input,
      core::IsoDeploymentFilesystem fileSystem);
  void buildUi();
  [[nodiscard]] bool collectWindowsToGoOptions();
  [[nodiscard]] bool collectWindowsInstallationOptions();
  void applyImageProfile();
  void appendLog(const QString& message);
  void handleIsoDeploymentProgress(const core::IsoDeploymentProgress& progress);
  void handleImageAnalysisFinished(const core::ImageAnalysisResult& result,
                                   const QString& path);
  void handleMacOsInstallerAnalysisFinished(
      const core::MacOsInstallerAnalysisResult& result, const QString& path);
  void handleMacOsInstallerProgress(
      const core::MacOsInstallerProgress& progress);
  void handleBadBlockProgress(const core::BadBlockTestProgress& progress);
  void handleCaptureProgress(const core::MediaCaptureProgress& progress);
  void handleStandaloneMediaProgress(
      const core::StandaloneMediaProgress& progress);
  void handleWindowsToGoProgress(const backend::WindowsToGoProgress& progress);
  void handleWriteFinished(const core::RawWriteResult& result);
  void handleWriteProgress(const core::RawWriteProgress& progress);
  [[nodiscard]] QString selectedOperation() const;
  void updateRuntimeValidationAvailability();
  [[nodiscard]] bool updatePrivilegeIndicator();
  void updateDeploymentOptionControls();
  void updatePersistenceRange();
  void updateWriteReadiness();
  [[nodiscard]] core::IsoDeploymentOptions isoDeploymentOptions() const;
  [[nodiscard]] core::VerificationProfile selectedVerificationProfile() const;
  void beginOperationGuard();
  void endOperationGuard();
  void noteOperationProgress();
  void setOperationProgress(const QString& stage, std::uint64_t completed = 0,
                            std::uint64_t total = 0);
  void refreshOperationProgress();
  void fitWindowToContents();
  void scheduleWindowFitToContents();
  void saveDeploymentReceipt(const core::RawWriteResult& result);

  QComboBox* volumeBox_{};
  QComboBox* bootSelectionBox_{};
  QPushButton* selectButton_{};
  QLabel* imageOptionLabel_{};
  QComboBox* imageOptionBox_{};
  QToolButton* windowsToGoOptionsButton_{};
  QLabel* persistenceLabel_{};
  QWidget* persistencePanel_{};
  QSlider* persistenceSlider_{};
  QSpinBox* persistenceSizeBox_{};
  QComboBox* partitionSchemeBox_{};
  QComboBox* targetSystemBox_{};
  QLineEdit* volumeLabel_{};
  QComboBox* fileSystemBox_{};
  QComboBox* clusterSizeBox_{};
  QToolButton* driveAdvancedButton_{};
  QWidget* driveAdvancedPanel_{};
  QCheckBox* listFixedDisks_{};
  QToolButton* formatAdvancedButton_{};
  QWidget* formatAdvancedPanel_{};
  QCheckBox* quickFormat_{};
  QComboBox* verificationProfileBox_{};
  QCheckBox* badBlocks_{};
  QCheckBox* runtimeValidation_{};
  QComboBox* badBlockPassCount_{};
  QToolButton* checksumButton_{};
  QToolButton* captureButton_{};
  QToolButton* formatButton_{};
  QPushButton* startButton_{};
  QPushButton* closeButton_{};
  QProgressBar* progressBar_{};
  QLabel* operationProgressLabel_{};
  QTimer* progressRefreshTimer_{};
  QElapsedTimer operationElapsed_;
  qint64 progressElapsedMs_{};
  QString progressStage_;
  std::uint64_t progressCompleted_{};
  std::uint64_t progressTotal_{};
  QTextEdit* logView_{};
  QLabel* privilegeLabel_{};
  std::unique_ptr<backend::BlockDeviceBackend> deviceBackend_;
  std::unique_ptr<backend::NtfsIsoImageStager> ntfsIsoStager_;
  std::unique_ptr<backend::StandaloneFilesystemStager> filesystemStager_;
  std::unique_ptr<backend::WindowsToGoImageStager> windowsToGoStager_;
  std::unique_ptr<core::ImageAnalyzer> imageAnalyzer_;
  std::shared_ptr<const core::RuntimeUefiValidationAssets>
      runtimeValidationAssets_;
  std::vector<core::BlockDeviceInfo> visibleDevices_;
  std::string discoverySignature_;
  std::optional<core::ImageInfo> selectedImage_;
  std::optional<core::MacOsInstallerInfo> selectedMacOsInstaller_;
  core::WindowsToGoOptions windowsToGoOptions_;
  core::WindowsUserExperienceOptions windowsInstallationOptions_;
  bool windowsToGoOptionsConfigured_{};
  bool windowsInstallationOptionsConfigured_{};
  QThread* writeThread_{};
  QThread* imageAnalysisThread_{};
  QThread* checksumThread_{};
  QThread* captureThread_{};
  QThread* analysisToolThread_{};
  QProcess* sleepInhibitor_{};
  QTimer* operationWatchdog_{};
  std::atomic_bool cancelRequested_{false};
  std::atomic_bool analysisCancelRequested_{false};
  std::int64_t lastOperationProgressMs_{};
  bool stallWarningIssued_{};
  bool closeWhenFinished_{};
  bool windowFitPending_{};
  bool isoDeploymentRunning_{};
  bool ntfsIsoDeploymentRunning_{};
  bool windowsToGoDeploymentRunning_{};
  bool ffuDeploymentRunning_{};
  bool macOsInstallerDeploymentRunning_{};
  bool standaloneFormatRunning_{};
  bool freeDosFormatRunning_{};
  bool msDosFormatRunning_{};
  bool ext2FormatRunning_{};
  std::string standaloneFilesystemLabel_;
  core::SecureBootDatabase secureBootDatabase_{
      core::packagedSecureBootBaseline()};
  std::optional<core::DeploymentPreflightReport> activePreflight_;
  std::string operationStartedAtUtc_;
  std::string latestImageSha256_;
  std::string latestChecksumImagePath_;
};

}  // namespace rufus::qt
