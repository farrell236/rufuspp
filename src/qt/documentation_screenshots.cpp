/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "main_window.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "rufus/core/application_info.hpp"
#include "rufus/core/image_analyzer.hpp"
#include "rufus/core/iso_deployment.hpp"
#include "rufus/core/media.hpp"
#include "rufus/core/raw_image_writer.hpp"
#include "rufus/core/windows_to_go.hpp"
#include "windows_user_experience_dialog.hpp"

namespace rufus::qt {
namespace {

constexpr std::uint64_t kGibibyte = 1024ULL * 1024ULL * 1024ULL;

bool saveWidgetScreenshot(QWidget& widget, const QDir& outputDirectory,
                          const QString& filename, QString& error) {
  widget.ensurePolished();
  widget.show();
  widget.raise();
  widget.activateWindow();
  QApplication::processEvents();
  QApplication::processEvents();

  const QPixmap screenshot = widget.grab();
  const QString path = outputDirectory.filePath(filename);
  if (screenshot.isNull() || !screenshot.save(path, "PNG")) {
    error = "Unable to save documentation screenshot: " + path;
    return false;
  }
  return true;
}

void expandMessageDetails(QMessageBox& box) {
  QApplication::processEvents();
  const auto buttons = box.findChildren<QPushButton*>();
  for (QPushButton* button : buttons) {
    if (button->text().contains("Details", Qt::CaseInsensitive)) {
      button->click();
      QApplication::processEvents();
      return;
    }
  }
}

QInputDialog* makeChoiceDialog(QWidget* parent, const QString& title,
                               const QString& label,
                               const QStringList& choices,
                               const int current = 0) {
  auto* dialog = new QInputDialog(parent);
  dialog->setWindowTitle(title);
  dialog->setLabelText(label);
  dialog->setOption(QInputDialog::UseListViewForComboBoxItems, true);
  dialog->setComboBoxItems(choices);
  dialog->setTextValue(choices.value(current));
  dialog->setComboBoxEditable(false);
  dialog->setOkButtonText("OK");
  dialog->setCancelButtonText("Cancel");
  dialog->adjustSize();
  const int documentationHeight =
      qMin(440, qMax(230, static_cast<int>(choices.size()) * 25 + 105));
  dialog->resize(qMax(470, dialog->width()),
                 documentationHeight);
  return dialog;
}

QDialog* makeTextReport(QWidget* parent, const QString& title,
                        const QString& text, const QStringList& actions = {}) {
  auto* dialog = new QDialog(parent);
  dialog->setWindowTitle(title);
  dialog->resize(700, 560);
  auto* layout = new QVBoxLayout(dialog);
  auto* view = new QTextEdit(dialog);
  view->setReadOnly(true);
  view->setLineWrapMode(QTextEdit::NoWrap);
  view->setPlainText(text);
  layout->addWidget(view, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  for (const QString& action : actions) {
    buttons->addButton(action, QDialogButtonBox::ActionRole);
  }
  layout->addWidget(buttons);
  return dialog;
}

core::BlockDeviceInfo documentationDevice() {
  core::BlockDeviceInfo device;
  device.stableId = "documentation-demo-device";
  device.devicePath = "DEMO-REMOVABLE-DEVICE";
  device.displayName = "Example USB Drive";
  device.vendor = "Example";
  device.model = "USB Drive";
  device.serialNumber = "DEMO-0001";
  device.capacityBytes = 64ULL * kGibibyte;
  device.logicalSectorSize = 512;
  device.bus = core::DeviceBus::Usb;
  device.removable = true;
  device.ejectable = true;
  device.writable = true;
  device.wholeDevice = true;
  return device;
}

core::ImageInfo documentationWindowsImage() {
  core::ImageInfo image;
  image.path = "/examples/Win11_24H2_English_x64.iso";
  image.displayName = "Win11_24H2_English_x64.iso";
  image.volumeLabel = "CCCOMA_X64FRE_EN-US_DV9";
  image.sizeBytes = 6ULL * kGibibyte;
  image.expandedSizeBytes = 7ULL * kGibibyte;
  image.windowsImageCount = 2;
  image.windowsBootIndex = 1;
  image.windowsVersionMajor = 10;
  image.windowsVersionMinor = 0;
  image.windowsBuild = 26100;
  image.format = core::ImageFormat::Iso;
  image.partitionScheme = core::PartitionScheme::Mbr;
  image.family = core::ImageFamily::WindowsInstaller;
  image.architecture = core::ImageArchitecture::X64;
  image.bootable = true;
  image.capabilities.isoExtraction = true;
  image.capabilities.rawWrite = true;
  image.capabilities.standardWindowsInstallation = true;
  image.capabilities.windowsToGo = true;
  image.capabilities.windowsCustomization = true;
  image.capabilities.biosBootable = true;
  image.capabilities.uefiBootable = true;
  image.capabilities.requiresNtfs = true;
  image.capabilities.containsLargeFile = true;
  image.capabilities.iso9660 = true;
  image.capabilities.joliet = true;
  image.capabilities.udf = true;
  image.capabilities.validBootCatalog = true;
  image.capabilities.windowsImageMetadata = true;

  core::WindowsEditionInfo professional;
  professional.index = 6;
  professional.name = "Windows 11 Pro";
  professional.description = "Windows 11 Pro";
  professional.totalBytes = 16ULL * kGibibyte;
  professional.versionMajor = 10;
  professional.build = 26100;
  professional.architecture = core::ImageArchitecture::X64;
  image.windowsEditions.push_back(professional);

  core::WindowsEditionInfo home = professional;
  home.index = 1;
  home.name = "Windows 11 Home";
  home.description = "Windows 11 Home";
  image.windowsEditions.insert(image.windowsEditions.begin(), home);
  return image;
}

core::ImageInfo documentationLinuxImage() {
  core::ImageInfo image;
  image.path = "/examples/ubuntu-24.04.3-desktop-amd64.iso";
  image.displayName = "ubuntu-24.04.3-desktop-amd64.iso";
  image.volumeLabel = "Ubuntu 24.04.3 LTS amd64";
  image.sizeBytes = 5ULL * kGibibyte;
  image.expandedSizeBytes = 6ULL * kGibibyte;
  image.format = core::ImageFormat::Iso;
  image.partitionScheme = core::PartitionScheme::Mbr;
  image.family = core::ImageFamily::LinuxLive;
  image.architecture = core::ImageArchitecture::X64;
  image.bootable = true;
  image.capabilities.isoExtraction = true;
  image.capabilities.rawWrite = true;
  image.capabilities.linuxPersistence = true;
  image.capabilities.linuxPersistenceStyle =
      core::LinuxPersistenceStyle::Casper;
  image.capabilities.biosBootable = true;
  image.capabilities.uefiBootable = true;
  image.capabilities.usesGrub = true;
  image.capabilities.iso9660 = true;
  image.capabilities.joliet = true;
  image.capabilities.validBootCatalog = true;
  return image;
}

}  // namespace

bool MainWindow::generateDocumentationScreenshots(
    const QString& outputDirectoryPath, QString& error) {
  QDir outputDirectory(outputDirectoryPath);
  if (!outputDirectory.exists() && !QDir().mkpath(outputDirectoryPath)) {
    error = "Unable to create documentation asset directory: " +
            outputDirectoryPath;
    return false;
  }
  outputDirectory = QDir(outputDirectoryPath);

  // Device polling would make checked-in images host-dependent. This mode is
  // intentionally read-only and exits immediately after rendering.
  const auto timers = findChildren<QTimer*>();
  for (QTimer* timer : timers) {
    timer->stop();
  }

  const core::BlockDeviceInfo demoDevice = documentationDevice();
  const auto clearTarget = [this] {
    const QSignalBlocker blocker(volumeBox_);
    volumeBox_->clear();
    volumeBox_->addItem("No eligible removable devices detected");
    volumeBox_->setEnabled(false);
    visibleDevices_.clear();
  };
  const auto installTarget = [this, &demoDevice] {
    const QSignalBlocker blocker(volumeBox_);
    visibleDevices_ = {demoDevice};
    volumeBox_->clear();
    volumeBox_->addItem("Example USB Drive [64 GiB]",
                        QString::fromStdString(demoDevice.stableId));
    volumeBox_->setItemData(0, true, Qt::UserRole + 1);
    volumeBox_->setItemData(0, static_cast<qulonglong>(0), Qt::UserRole + 2);
    volumeBox_->setItemData(
        0,
        "Documentation example only\nBus: USB\nCapacity: 64 GiB\nEligibility: eligible",
        Qt::ToolTipRole);
    volumeBox_->setEnabled(true);
  };
  const auto prepareMainWindow = [this] {
    driveAdvancedButton_->setChecked(false);
    formatAdvancedButton_->setChecked(true);
    logView_->hide();
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setFormat("ANALYZED — READY");
    operationProgressLabel_->hide();
    progressStage_.clear();
    statusBar()->showMessage("Example media analyzed — review settings before starting");
    checksumButton_->setEnabled(true);
    captureButton_->setEnabled(true);
    formatButton_->setEnabled(true);
    startButton_->setText("START");
    startButton_->setEnabled(true);
    closeButton_->setText("CLOSE");
    fitWindowToContents();
    QApplication::processEvents();
    fitWindowToContents();
  };
  const auto selectImage = [this, &clearTarget](core::ImageInfo image,
                                                const QString& displayPath) {
    clearTarget();
    selectedMacOsInstaller_.reset();
    selectedImage_ = std::move(image);
    bootSelectionBox_->clear();
    bootSelectionBox_->addItem(
        QString::fromStdString(selectedImage_->displayName), displayPath);
    bootSelectionBox_->setToolTip(displayPath);
    volumeLabel_->setText(QString::fromStdString(selectedImage_->volumeLabel));
    partitionSchemeBox_->setCurrentText("MBR");
    targetSystemBox_->setCurrentText("BIOS + UEFI");
    fileSystemBox_->setCurrentText(
        selectedImage_->capabilities.requiresNtfs ? "NTFS" : "FAT32");
    clusterSizeBox_->setCurrentIndex(0);
    quickFormat_->setChecked(true);
    verificationProfileBox_->setCurrentIndex(
        verificationProfileBox_->findData("standard"));
    applyImageProfile();
  };

  clearTarget();
  selectedImage_.reset();
  selectedMacOsInstaller_.reset();
  bootSelectionBox_->clear();
#if defined(Q_OS_MACOS)
  bootSelectionBox_->addItem("Disk image or macOS installer (Please select)");
#else
  bootSelectionBox_->addItem("Disk or ISO image (Please select)");
#endif
  volumeLabel_->clear();
  progressBar_->setFormat("READY");
  statusBar()->showMessage("Ready — select an image and eligible target");
  applyImageProfile();
  formatAdvancedButton_->setChecked(true);
  fitWindowToContents();
  if (!saveWidgetScreenshot(*this, outputDirectory, "main-window.png", error)) {
    return false;
  }

  selectImage(documentationWindowsImage(),
              "/examples/Win11_24H2_English_x64.iso");
  installTarget();
  partitionSchemeBox_->setEnabled(true);
  targetSystemBox_->setEnabled(true);
  fileSystemBox_->setEnabled(true);
  clusterSizeBox_->setEnabled(true);
  quickFormat_->setEnabled(true);
  prepareMainWindow();
  if (!saveWidgetScreenshot(*this, outputDirectory,
                            "windows-installation-mode.png", error)) {
    return false;
  }

  // Fictional display state only: no worker, source file or device is opened.
  isoDeploymentRunning_ = true;
  progressElapsedMs_ = 83 * 1000;
  handleWriteProgress({core::RawWriteStage::Writing,
                       4ULL * kGibibyte, 8ULL * kGibibyte});
  startButton_->setEnabled(false);
  closeButton_->setText("CANCEL");
  fitWindowToContents();
  if (!saveWidgetScreenshot(*this, outputDirectory,
                            "write-progress.png", error)) {
    return false;
  }
  isoDeploymentRunning_ = false;
  progressElapsedMs_ = 0;

  imageOptionBox_->setCurrentIndex(
      imageOptionBox_->findData("windows-to-go"));
  partitionSchemeBox_->setCurrentText("GPT");
  targetSystemBox_->setCurrentText("UEFI (non CSM)");
  fileSystemBox_->setCurrentText("NTFS");
  prepareMainWindow();
  if (!saveWidgetScreenshot(*this, outputDirectory,
                            "windows-to-go-mode.png", error)) {
    return false;
  }

  core::WindowsUserExperienceOptions setupOptions;
  setupOptions.bypassHardwareRequirements = true;
  setupOptions.bypassOnlineAccountRequirement = true;
  setupOptions.createLocalAccount = true;
  setupOptions.localAccountName = "alex";
  setupOptions.useRegionalOptions = true;
  setupOptions.localeName = "en-US";
  setupOptions.disableDataCollection = true;
  setupOptions.disableAutomaticDeviceEncryption = true;
  setupOptions.applyQualityOfLifeOptions = true;
  WindowsUserExperienceDialog setupDialog(
      core::ImageArchitecture::X64, 26100, setupOptions,
      core::WindowsDeploymentMode::StandardInstallation, this);
  setupDialog.resize(560, setupDialog.sizeHint().height());
  if (!saveWidgetScreenshot(setupDialog, outputDirectory,
                            "windows-installation-options.png", error)) {
    return false;
  }
  setupDialog.hide();

  core::WindowsUserExperienceOptions toGoOptions = setupOptions;
  toGoOptions.preventInternalDiskAccess = true;
  toGoOptions.bypassHardwareRequirements = false;
  WindowsUserExperienceDialog toGoDialog(
      core::ImageArchitecture::X64, 26100, toGoOptions,
      core::WindowsDeploymentMode::WindowsToGo, this);
  toGoDialog.resize(590, toGoDialog.sizeHint().height());
  if (!saveWidgetScreenshot(toGoDialog, outputDirectory,
                            "windows-to-go-options.png", error)) {
    return false;
  }
  toGoDialog.hide();

  std::unique_ptr<QInputDialog> editionDialog(makeChoiceDialog(
      this, "Select Windows edition", "Select the Windows edition to install:",
      {"[1] Windows 11 Home", "[6] Windows 11 Pro"}, 1));
  if (!saveWidgetScreenshot(*editionDialog, outputDirectory,
                            "windows-edition-selection.png", error)) {
    return false;
  }
  editionDialog->hide();

  selectImage(documentationLinuxImage(),
              "/examples/ubuntu-24.04.3-desktop-amd64.iso");
  installTarget();
  updatePersistenceRange();
  persistenceSizeBox_->setValue(8192);
  partitionSchemeBox_->setEnabled(true);
  targetSystemBox_->setEnabled(true);
  fileSystemBox_->setEnabled(true);
  clusterSizeBox_->setEnabled(true);
  quickFormat_->setEnabled(true);
  prepareMainWindow();
  if (!saveWidgetScreenshot(*this, outputDirectory,
                            "linux-persistence-mode.png", error)) {
    return false;
  }

  core::MacOsInstallerInfo installer;
  installer.applicationPath =
      "/Applications/Install macOS Sequoia.app";
  installer.createInstallMediaPath =
      "/Applications/Install macOS Sequoia.app/Contents/Resources/createinstallmedia";
  installer.displayName = "Install macOS Sequoia";
  installer.version = "15.7";
  installer.build = "24G222";
  installer.payloadSizeBytes = 14ULL * kGibibyte;
  core::MacOsInstallerAnalysisResult macAnalysis;
  macAnalysis.installer = installer;
  clearTarget();
  handleMacOsInstallerAnalysisFinished(
      macAnalysis, "/Applications/Install macOS Sequoia.app");
  installTarget();
  prepareMainWindow();
  quickFormat_->setEnabled(true);
  if (!saveWidgetScreenshot(*this, outputDirectory,
                            "macos-installer-mode.png", error)) {
    return false;
  }

  selectImage(documentationWindowsImage(),
              "/examples/Win11_24H2_English_x64.iso");
  installTarget();
  driveAdvancedButton_->setChecked(true);
  formatAdvancedButton_->setChecked(true);
  logView_->setPlainText(
      "Rufus++ user interface initialized.\n"
      "Example removable device detected and validated.\n"
      "Windows 11 installation media analyzed.\n"
      "Ready for preflight review.");
  logView_->show();
  startButton_->setEnabled(true);
  fitWindowToContents();
  if (!saveWidgetScreenshot(*this, outputDirectory,
                            "advanced-options-and-log.png", error)) {
    return false;
  }

  std::unique_ptr<QInputDialog> captureDialog(makeChoiceDialog(
      this, "Save device to image", "Image format:",
      {"Raw disk image (DD)", "Fixed VHD", "Dynamic VHD", "Dynamic VHDX",
       "FFU", "UDF ISO"}));
  if (!saveWidgetScreenshot(*captureDialog, outputDirectory,
                            "capture-format-selection.png", error)) {
    return false;
  }
  captureDialog->hide();

  std::unique_ptr<QInputDialog> formatDialog(makeChoiceDialog(
      this, "Standalone formatting", "Media type:",
      {"FAT32 (non bootable)", "FreeDOS (FAT32)",
       "MS-DOS 7/8 (user-supplied FAT32 system files)",
       "GRUB2 BIOS prompt (FAT32)",
       "Grub4DOS (user-supplied GRLDR, FAT32)",
       "ReactOS (user-supplied FREELDR.SYS, FAT32)",
       "Syslinux 4 BIOS prompt (FAT32)",
       "UEFI Shell 2.2 (built-in, FAT32)",
       "UEFI Shell 2.2 (built-in multi-architecture, FAT32)",
       "UEFI application (user-supplied, FAT32)",
       "ext2 (portable Linux filesystem)"},
      1));
  formatDialog->resize(590, formatDialog->height());
  if (!saveWidgetScreenshot(*formatDialog, outputDirectory,
                            "standalone-format-selection.png", error)) {
    return false;
  }
  formatDialog->hide();

  std::unique_ptr<QInputDialog> shellDialog(makeChoiceDialog(
      this, "Built-in UEFI Shell architecture", "Target firmware:",
      {"UEFI Shell 2.2 — x86-64", "UEFI Shell 2.2 — x86-32",
       "UEFI Shell 2.2 — ARM64", "UEFI Shell 2.2 — ARM32",
       "UEFI Shell 2.2 — RISC-V 64", "UEFI Shell 2.2 — LoongArch64"}));
  shellDialog->resize(540, shellDialog->height());
  if (!saveWidgetScreenshot(*shellDialog, outputDirectory,
                            "uefi-shell-architecture.png", error)) {
    return false;
  }
  shellDialog->hide();

  const QString applicationVersion =
      QString::fromUtf8(core::ApplicationInfo::displayName.data(),
                        static_cast<qsizetype>(
                            core::ApplicationInfo::displayName.size())) +
      " v" +
      QString::fromUtf8(core::ApplicationInfo::version.data(),
                        static_cast<qsizetype>(
                            core::ApplicationInfo::version.size()));
  std::unique_ptr<QDialog> healthDialog(makeTextReport(
      this, "Capability health",
      applicationVersion + "\nCapability and dependency health\n\n" +
      "Device backend: native platform backend\n"
      "  Physical discovery: available\n"
      "  Read-only inspection: available\n"
      "  Raw-device writing: authorization required\n"
      "  Identity revalidation: available\n"
      "  Exclusive access: available\n"
      "  Write verification: available\n"
      "  Media capture: available\n"
      "  Bad-block testing: available\n\n"
      "Image/deployment providers\n"
      "  Runtime UEFI validation assets: available\n"
      "  QEMU boot smoke test: optional dependency\n\n"
      "Secure Boot revocation data\n"
      "  Baseline integrity: verified official data\n\n"
      "Privilege boundary\n"
      "  UNPRIVILEGED: authorize before physical-device operations",
      {"Inspect target", "Analyze image trust", "Virtual boot test",
       "Check DBX updates", "Import trust data"}));
  if (!saveWidgetScreenshot(*healthDialog, outputDirectory,
                            "capability-health.png", error)) {
    return false;
  }
  healthDialog->hide();

  QMessageBox checksumBox(
      QMessageBox::Information, "Image checksums",
      "All image checksums were computed successfully.", QMessageBox::Ok,
      this);
  checksumBox.setDetailedText(
      "MD5\n3c77b6f2d48a19583d474b4c92d2a9ac\n\n"
      "SHA-1\n5703c37f6f4e8e20fbcb22c8bca2c0b7bc9a9ca0\n\n"
      "SHA-256\n42d01edb95f6fbd25036a6b46eb3f5bc403bc691ea59cd29cfd4d75b93e66730\n\n"
      "SHA-512\n0f8c924cf0eaa4787be0d7c37a51a64c1e213986ca59da9d2d61d768c4a7a9ec"
      "7a7282dc88667af5ff7bd3d01a12cfe8d460b8d21195d267f0a36cbb0189c657");
  checksumBox.show();
  expandMessageDetails(checksumBox);
  if (!saveWidgetScreenshot(checksumBox, outputDirectory,
                            "checksum-results.png", error)) {
    return false;
  }
  checksumBox.hide();

  QDialog virtualBootDialog(this);
  virtualBootDialog.setWindowTitle("Virtual boot smoke test");
  virtualBootDialog.resize(540, 150);
  auto* virtualBootLayout = new QVBoxLayout(&virtualBootDialog);
  auto* virtualBootLabel = new QLabel(
      "Launching the image read-only. Passing means QEMU remained alive for "
      "eight seconds; it is not a substitute for hardware boot testing.",
      &virtualBootDialog);
  virtualBootLabel->setWordWrap(true);
  virtualBootLayout->addWidget(virtualBootLabel);
  virtualBootLayout->addWidget(
      new QDialogButtonBox(QDialogButtonBox::Cancel, &virtualBootDialog));
  if (!saveWidgetScreenshot(virtualBootDialog, outputDirectory,
                            "virtual-boot-test.png", error)) {
    return false;
  }
  virtualBootDialog.hide();

  QMessageBox confirmationBox(this);
  confirmationBox.setIcon(QMessageBox::Warning);
  confirmationBox.setWindowTitle("Confirm destructive write");
  confirmationBox.setText(
      "This operation will permanently overwrite Example USB Drive.");
  confirmationBox.setInformativeText(
      "The preflight report is ready. Expand Show Details to review the exact "
      "layout, transformations, dependencies and verification policy.\n\n"
      "All existing data on the target will be lost.");
  core::DeploymentPreflightInput preflight;
  preflight.operation = "Windows installation media";
  preflight.image = documentationWindowsImage();
  preflight.target = demoDevice;
  preflight.partitionScheme = "MBR";
  preflight.targetSystem = "BIOS + UEFI";
  preflight.fileSystem = "NTFS + UEFI:NTFS";
  preflight.quickFormat = true;
  preflight.bytesToWrite = preflight.temporaryBytes = demoDevice.capacityBytes;
  configureIsoPreflightSizing(preflight, core::IsoDeploymentFilesystem::Ntfs);
  preflight.transformations = {
      "Extract and byte-verify the optical-image file tree",
      "Install the offline UEFI:NTFS helper partition",
      "Install Windows legacy BIOS boot code and its backup boot sector"};
  preflight.dependencies.push_back(
      {"NTFS staging provider", true, true, "Documentation example"});
  confirmationBox.setDetailedText(
      QString::fromStdString(core::buildDeploymentPreflight(preflight).toText()));
  confirmationBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
  confirmationBox.setDefaultButton(QMessageBox::Cancel);
  confirmationBox.show();
  expandMessageDetails(confirmationBox);
  if (!saveWidgetScreenshot(confirmationBox, outputDirectory,
                            "destructive-write-confirmation.png", error)) {
    return false;
  }
  confirmationBox.hide();

  hide();
  return true;
}

}  // namespace rufus::qt
