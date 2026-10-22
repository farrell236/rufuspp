/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "main_window.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

#include "rufus/backend/block_device_backend.hpp"
#include "rufus/backend/ntfs_iso_image_stager.hpp"
#include "rufus/backend/standalone_filesystem_stager.hpp"
#include "rufus/backend/windows_to_go_image_stager.hpp"
#include "rufus/core/application_info.hpp"
#include "rufus/core/deployment_quality.hpp"
#include "rufus/core/format.hpp"
#include "rufus/core/image_analyzer.hpp"
#include "rufus/core/iso_deployment.hpp"
#include "rufus/core/media.hpp"
#include "rufus/core/platform.hpp"
#include "rufus/core/raw_image_writer.hpp"
#include "rufus/core/secure_boot_analyzer.hpp"
#include "rufus/core/standalone_media.hpp"
#include "rufus/core/write_plan.hpp"
#include "rufus/core/windows_to_go.hpp"
#include "rufus/core/wim_applier.hpp"
#include "rufus/core/wim_splitter.hpp"

#include "secure_boot_assets.hpp"
#include "uefi_shell_assets.hpp"
#include "windows_user_experience_dialog.hpp"

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace rufus::qt {

namespace {

QString fromView(const std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QHBoxLayout* makeSectionHeader(const QString& title, QWidget* parent) {
  auto* row = new QHBoxLayout();
  row->setContentsMargins(0, 1, 0, 1);
  row->setSpacing(9);

  auto* label = new QLabel(title, parent);
  QFont font = label->font();
  font.setPointSize(15);
  font.setWeight(QFont::Medium);
  label->setFont(font);

  auto* line = new QFrame(parent);
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Plain);
  line->setLineWidth(1);

  row->addWidget(label);
  row->addWidget(line, 1);
  return row;
}

QLabel* makeFieldLabel(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  QFont font = label->font();
  font.setPointSize(11);
  label->setFont(font);
  return label;
}

QToolButton* makeToolbarButton(const QString& iconPath,
                               const QString& tooltip,
                               QWidget* parent) {
  auto* button = new QToolButton(parent);
  button->setAutoRaise(true);
  button->setIcon(QIcon(iconPath));
  button->setIconSize({22, 22});
  button->setFixedSize(30, 30);
  button->setToolTip(tooltip);
  return button;
}

std::filesystem::path fileSystemPath(const QString& path) {
#if defined(_WIN32)
  return std::filesystem::path(path.toStdWString());
#else
  return std::filesystem::path(path.toStdString());
#endif
}

QString deviceLabel(const core::BlockDeviceInfo& device) {
  QString label = QString::fromStdString(device.displayName) + " (" +
                  QString::fromStdString(device.devicePath) + ") [" +
                  fromView(core::formatByteSize(device.capacityBytes)) + ']';
  const auto eligibility = core::evaluateDeviceEligibility(device);
  if (eligibility != core::DeviceEligibility::Eligible) {
    label += " — " + fromView(core::deviceEligibilityName(eligibility));
  }
  return label;
}

QString deviceToolTip(const core::BlockDeviceInfo& device) {
  QString tooltip = "Backend device: " + QString::fromStdString(device.devicePath) +
                    "\nBus: " + fromView(core::deviceBusName(device.bus));
  if (!device.serialNumber.empty()) {
    tooltip += "\nSerial: " + QString::fromStdString(device.serialNumber);
  }
  if (!device.mountPoints.empty()) {
    tooltip += "\nMounted at: ";
    for (std::size_t index = 0; index < device.mountPoints.size(); ++index) {
      if (index != 0) {
        tooltip += ", ";
      }
      tooltip += QString::fromStdString(device.mountPoints[index]);
    }
  }
  tooltip += "\nEligibility: " +
             fromView(core::deviceEligibilityName(core::evaluateDeviceEligibility(device)));
  return tooltip;
}

std::string discoverySignature(const backend::DeviceDiscoveryResult& discovery) {
  std::vector<std::string> records;
  records.reserve(discovery.devices.size());
  for (const auto& device : discovery.devices) {
    std::ostringstream record;
    record << device.stableId << '\n' << device.devicePath << '\n'
           << device.capacityBytes << '\n' << device.logicalSectorSize << '\n'
           << device.removable << device.ejectable << device.writable
           << device.systemDevice << device.wholeDevice;
    for (const auto& mount : device.mountPoints) {
      record << '\n' << mount;
    }
    records.push_back(record.str());
  }
  std::sort(records.begin(), records.end());
  std::string signature;
  for (const auto& record : records) {
    signature += record;
    signature.push_back('\0');
  }
  return signature;
}

struct ChecksumResult final {
  bool success{};
  bool cancelled{};
  QString error;
  QString md5;
  QString sha1;
  QString sha256;
  QString sha512;
};

std::shared_ptr<const core::RuntimeUefiValidationAssets>
loadRuntimeValidationAssets() {
  auto assets = std::make_shared<core::RuntimeUefiValidationAssets>();
  constexpr std::array<const char*, 6> names{
      "bootx64.efi", "bootia32.efi", "bootaa64.efi", "bootarm.efi",
      "bootloongarch64.efi", "bootriscv64.efi"};
  for (const char* name : names) {
    QFile resource(":/rufus/uefi-md5/" + QString::fromLatin1(name));
    if (!resource.open(QIODevice::ReadOnly)) {
      return {};
    }
    const QByteArray bytes = resource.readAll();
    if (bytes.isEmpty()) {
      return {};
    }
    assets->bootloaders.push_back(
        {name, std::vector<unsigned char>(bytes.begin(), bytes.end())});
  }
  return assets;
}

void showTextReport(QWidget* parent, const QString& title,
                    const QString& contents) {
  QDialog dialog(parent);
  dialog.setWindowTitle(title);
  dialog.resize(640, 520);
  auto* layout = new QVBoxLayout(&dialog);
  auto* view = new QTextEdit(&dialog);
  view->setReadOnly(true);
  view->setPlainText(contents);
  view->setLineWrapMode(QTextEdit::NoWrap);
  layout->addWidget(view, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save |
                                            QDialogButtonBox::Close,
                                        &dialog);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  QObject::connect(buttons->button(QDialogButtonBox::Close),
                   &QPushButton::clicked, &dialog, &QDialog::accept);
  QObject::connect(buttons->button(QDialogButtonBox::Save),
                   &QPushButton::clicked, &dialog, [&dialog, view, title] {
                     const QString path = QFileDialog::getSaveFileName(
                         &dialog, "Save " + title, QDir::homePath(),
                         "Text report (*.txt);;All files (*)");
                     if (path.isEmpty()) {
                       return;
                     }
                     QFile output(path);
                     if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                         output.write(view->toPlainText().toUtf8()) < 0) {
                       QMessageBox::warning(&dialog, "Unable to save report",
                                            output.errorString());
                     }
                   });
  layout->addWidget(buttons);
  dialog.exec();
}

QString qemuExecutable(const core::ImageArchitecture architecture) {
  QStringList candidates;
  switch (architecture) {
    case core::ImageArchitecture::Arm:
      candidates << "qemu-system-arm";
      break;
    case core::ImageArchitecture::Arm64:
      candidates << "qemu-system-aarch64";
      break;
    case core::ImageArchitecture::RiscV64:
      candidates << "qemu-system-riscv64";
      break;
    case core::ImageArchitecture::X86:
      candidates << "qemu-system-i386" << "qemu-system-x86_64";
      break;
    case core::ImageArchitecture::X64:
    case core::ImageArchitecture::Multiple:
    case core::ImageArchitecture::Unknown:
      candidates << "qemu-system-x86_64" << "qemu-system-i386";
      break;
    case core::ImageArchitecture::Itanium:
    case core::ImageArchitecture::LoongArch64:
      break;
  }
  for (const QString& candidate : candidates) {
    const QString path = QStandardPaths::findExecutable(candidate);
    if (!path.isEmpty()) {
      return path;
    }
  }
  return {};
}

QString ovmfFirmwarePath() {
  const QStringList candidates{
      "/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
      "/usr/local/share/qemu/edk2-x86_64-code.fd",
      "/usr/share/OVMF/OVMF_CODE.fd",
      "/usr/share/edk2/ovmf/OVMF_CODE.fd",
      "/usr/share/qemu/edk2-x86_64-code.fd"};
  for (const QString& candidate : candidates) {
    if (QFileInfo::exists(candidate)) {
      return candidate;
    }
  }
  return {};
}

QString filesystemSafeComponent(QString value) {
  for (qsizetype index = 0; index < value.size(); ++index) {
    const QChar character = value.at(index);
    if (!character.isLetterOrNumber() && character != '-' && character != '_') {
      value[index] = '_';
    }
  }
  return value.left(80);
}

QString privilegeRouteLabel(const backend::PrivilegeRoute route) {
  switch (route) {
    case backend::PrivilegeRoute::Unprivileged:
      return QStringLiteral("UNPRIVILEGED");
    case backend::PrivilegeRoute::Elevated:
      return QStringLiteral("ELEVATED");
    case backend::PrivilegeRoute::PrivilegedHelper:
      return QStringLiteral("PRIVILEGED HELPER");
  }
  return QStringLiteral("UNPRIVILEGED");
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      deviceBackend_(backend::makePlatformBlockDeviceBackend()),
      ntfsIsoStager_(backend::makePlatformNtfsIsoImageStager()),
      filesystemStager_(backend::makePlatformStandaloneFilesystemStager()),
      windowsToGoStager_(backend::makePlatformWindowsToGoImageStager()),
      imageAnalyzer_(std::make_unique<core::ImageAnalyzer>()) {
  runtimeValidationAssets_ = loadRuntimeValidationAssets();
  const SecureBootDataLoadResult secureBootData = loadSecureBootData();
  secureBootDatabase_ = secureBootData.database;
  buildUi();
  if (!secureBootData.warning.isEmpty()) {
    appendLog("Secure Boot data warning: " + secureBootData.warning);
  } else {
    appendLog("Secure Boot DBX " +
              QString::fromStdString(secureBootDatabase_.version) +
              (secureBootData.usedCachedUpdate ? " loaded from verified cache."
                                               : " loaded from the application bundle."));
  }
  refreshVolumes();
  auto* devicePoller = new QTimer(this);
  devicePoller->setInterval(3000);
  connect(devicePoller, &QTimer::timeout, this, &MainWindow::pollVolumes);
  devicePoller->start();
  operationWatchdog_ = new QTimer(this);
  operationWatchdog_->setInterval(15000);
  connect(operationWatchdog_, &QTimer::timeout, this, [this] {
    if (lastOperationProgressMs_ == 0 || stallWarningIssued_) {
      return;
    }
    const auto idleMilliseconds =
        QDateTime::currentMSecsSinceEpoch() - lastOperationProgressMs_;
    if (idleMilliseconds >= 60000) {
      stallWarningIssued_ = true;
      appendLog(
          "Operation watchdog: no progress callback for 60 seconds. The current I/O call is still allowed to finish or fail safely.");
      statusBar()->showMessage(
          "Device operation is taking unusually long — cancellation remains available");
    }
  });
}

MainWindow::~MainWindow() {
  cancelRequested_.store(true);
  analysisCancelRequested_.store(true);
  endOperationGuard();
  if (imageAnalysisThread_ != nullptr) {
    imageAnalysisThread_->wait();
    delete imageAnalysisThread_;
    imageAnalysisThread_ = nullptr;
  }
  if (writeThread_ != nullptr) {
    writeThread_->wait();
    delete writeThread_;
    writeThread_ = nullptr;
  }
  if (checksumThread_ != nullptr) {
    checksumThread_->wait();
    delete checksumThread_;
    checksumThread_ = nullptr;
  }
  if (captureThread_ != nullptr) {
    captureThread_->wait();
    delete captureThread_;
    captureThread_ = nullptr;
  }
  if (analysisToolThread_ != nullptr) {
    analysisToolThread_->wait();
    delete analysisToolThread_;
    analysisToolThread_ = nullptr;
  }
}

void MainWindow::buildUi() {
  const auto platform = core::platformName(core::currentPlatform());
  setWindowTitle(fromView(core::ApplicationInfo::displayName) + " v" +
                 fromView(core::ApplicationInfo::version));
  resize(490, 580);

  auto* central = new QWidget(this);
  central->setObjectName("rufusPage");
  central->setStyleSheet(R"(
    QWidget#rufusPage QComboBox,
    QWidget#rufusPage QLineEdit,
    QWidget#rufusPage QPushButton {
      min-height: 25px;
    }
    QWidget#rufusPage QProgressBar {
      min-height: 23px;
      text-align: center;
    }
  )");

  auto* page = new QVBoxLayout(central);
  page->setContentsMargins(13, 8, 13, 6);
  page->setSpacing(4);

  page->addLayout(makeSectionHeader("Drive Properties", central));

  auto* driveGrid = new QGridLayout();
  driveGrid->setContentsMargins(0, 0, 0, 0);
  driveGrid->setHorizontalSpacing(10);
  driveGrid->setVerticalSpacing(3);
  driveGrid->setColumnStretch(0, 1);
  driveGrid->setColumnStretch(1, 1);

  driveGrid->addWidget(makeFieldLabel("Device", central), 0, 0, 1, 2);
  volumeBox_ = new QComboBox(central);
  volumeBox_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  volumeBox_->setToolTip("Physical devices are discovered read-only; no device is opened for writing.");
  connect(volumeBox_, &QComboBox::currentIndexChanged, this,
          [this](const int) {
            updatePersistenceRange();
            updateWriteReadiness();
          });
  driveGrid->addWidget(volumeBox_, 1, 0, 1, 2);

  driveGrid->addWidget(makeFieldLabel("Boot selection", central), 2, 0, 1, 2);
  auto* bootRow = new QHBoxLayout();
  bootRow->setContentsMargins(0, 0, 0, 0);
  bootRow->setSpacing(7);
  bootSelectionBox_ = new QComboBox(central);
#if defined(Q_OS_MACOS)
  bootSelectionBox_->addItem("Disk image or macOS installer (Please select)");
#else
  bootSelectionBox_->addItem("Disk or ISO image (Please select)");
#endif
  bootSelectionBox_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  selectButton_ = new QPushButton("SELECT", central);
  selectButton_->setMinimumWidth(100);
  connect(selectButton_, &QPushButton::clicked, this, &MainWindow::chooseImage);
  bootRow->addWidget(bootSelectionBox_, 1);
  bootRow->addWidget(selectButton_);
  driveGrid->addLayout(bootRow, 3, 0, 1, 2);

  imageOptionLabel_ = makeFieldLabel("Image option", central);
  driveGrid->addWidget(imageOptionLabel_, 4, 0, 1, 2);
  imageOptionBox_ = new QComboBox(central);
  imageOptionBox_->addItem("Standard installation (select an image)");
  imageOptionBox_->setEnabled(false);
  imageOptionBox_->setToolTip("Image type and boot metadata will appear after selection.");
  auto* imageOptionRow = new QHBoxLayout();
  imageOptionRow->setContentsMargins(0, 0, 0, 0);
  imageOptionRow->setSpacing(7);
  imageOptionRow->addWidget(imageOptionBox_, 1);
  windowsToGoOptionsButton_ = new QToolButton(central);
  windowsToGoOptionsButton_->setIcon(QIcon(":/rufus/icons/settings-24.png"));
  windowsToGoOptionsButton_->setIconSize({20, 20});
  windowsToGoOptionsButton_->setFixedSize(31, 31);
  windowsToGoOptionsButton_->setToolTip("Configure Windows To Go options");
  windowsToGoOptionsButton_->hide();
  connect(windowsToGoOptionsButton_, &QToolButton::clicked,
          this, &MainWindow::configureWindowsExperience);
  connect(imageOptionBox_, &QComboBox::currentIndexChanged, this,
          [this](const int) {
            const bool windowsToGo =
                imageOptionBox_->currentData().toString() == "windows-to-go";
            const bool windowsInstallation =
                imageOptionBox_->currentData().toString() == "windows-install";
            windowsToGoOptionsButton_->setVisible(windowsToGo || windowsInstallation);
            windowsToGoOptionsButton_->setToolTip(
                windowsToGo ? "Configure Windows To Go options"
                            : "Configure Windows installation options");
            if (windowsToGo) {
              partitionSchemeBox_->setCurrentText("GPT");
              targetSystemBox_->setCurrentText("UEFI (non CSM)");
              fileSystemBox_->setCurrentText("NTFS");
            } else if (selectedImage_ &&
                       (imageOptionBox_->currentData().toString() == "windows-install" ||
                        imageOptionBox_->currentData().toString() == "iso-copy")) {
              partitionSchemeBox_->setCurrentText("MBR");
              targetSystemBox_->setCurrentText(
                  selectedImage_->capabilities.biosBootable &&
                          selectedImage_->capabilities.uefiBootable
                      ? "BIOS + UEFI"
                  : selectedImage_->capabilities.biosBootable
                      ? "BIOS (or UEFI-CSM)"
                      : "UEFI (non CSM)");
              fileSystemBox_->setCurrentText(
                  selectedImage_->capabilities.requiresNtfs ? "NTFS" : "FAT32");
            }
            const QString operation = imageOptionBox_->currentData().toString();
            volumeLabel_->setEnabled(operation == "windows-to-go" ||
                                     operation == "windows-install" ||
                                     operation == "iso-copy");
            updateRuntimeValidationAvailability();
            updateDeploymentOptionControls();
            updateWriteReadiness();
          });
  imageOptionRow->addWidget(windowsToGoOptionsButton_);
  driveGrid->addLayout(imageOptionRow, 5, 0, 1, 2);

  persistencePanel_ = new QWidget(central);
  auto* persistenceLayout = new QHBoxLayout(persistencePanel_);
  persistenceLayout->setContentsMargins(0, 0, 0, 0);
  persistenceLayout->setSpacing(7);
  persistenceSlider_ = new QSlider(Qt::Horizontal, persistencePanel_);
  persistenceSlider_->setRange(0, 1000);
  persistenceSlider_->setValue(0);
  persistenceSizeBox_ = new QSpinBox(persistencePanel_);
  persistenceSizeBox_->setRange(0, 0);
  persistenceSizeBox_->setSingleStep(256);
  persistenceSizeBox_->setSpecialValueText("No persistence");
  persistenceSizeBox_->setSuffix(" MiB");
  persistenceSizeBox_->setMinimumWidth(105);
  persistenceLayout->addWidget(persistenceSlider_, 1);
  persistenceLayout->addWidget(persistenceSizeBox_);
  connect(persistenceSlider_, &QSlider::valueChanged, this, [this](const int value) {
    const int maximum = persistenceSizeBox_->maximum();
    int size = maximum == 0 ? 0 : static_cast<int>(
        (static_cast<long long>(maximum) * value + 500LL) / 1000LL);
    if (size != 0 && size < 256) {
      size = std::min(256, maximum);
    }
    const QSignalBlocker blocker(persistenceSizeBox_);
    persistenceSizeBox_->setValue(size);
    updateWriteReadiness();
  });
  connect(persistenceSizeBox_, &QSpinBox::valueChanged, this, [this](const int value) {
    const int maximum = persistenceSizeBox_->maximum();
    const int position = maximum == 0 ? 0 : static_cast<int>(
        (static_cast<long long>(value) * 1000LL + maximum / 2LL) / maximum);
    const QSignalBlocker blocker(persistenceSlider_);
    persistenceSlider_->setValue(position);
    updateWriteReadiness();
  });
  persistencePanel_->hide();
  persistenceLabel_ = makeFieldLabel("Persistent partition size", central);
  persistenceLabel_->hide();
  driveGrid->addWidget(persistenceLabel_, 6, 0, 1, 2);
  driveGrid->addWidget(persistencePanel_, 7, 0, 1, 2);

  driveGrid->addWidget(makeFieldLabel("Partition scheme", central), 8, 0);
  driveGrid->addWidget(makeFieldLabel("Target system", central), 8, 1);
  partitionSchemeBox_ = new QComboBox(central);
  partitionSchemeBox_->addItem("Auto (recommended)", "auto");
  partitionSchemeBox_->addItem("MBR", "mbr");
  partitionSchemeBox_->addItem("GPT", "gpt");
  partitionSchemeBox_->setEnabled(false);
  targetSystemBox_ = new QComboBox(central);
  targetSystemBox_->addItem("Auto (recommended)", "auto");
  targetSystemBox_->addItem("BIOS (or UEFI-CSM)", "bios");
  targetSystemBox_->addItem("UEFI (non CSM)", "uefi");
  targetSystemBox_->addItem("BIOS + UEFI", "dual");
  targetSystemBox_->addItem("Mac firmware", "mac");
  targetSystemBox_->setEnabled(false);
  connect(partitionSchemeBox_, &QComboBox::currentIndexChanged, this,
          [this](const int) {
            updateDeploymentOptionControls();
            updateWriteReadiness();
          });
  connect(targetSystemBox_, &QComboBox::currentIndexChanged, this,
          [this](const int) { updateWriteReadiness(); });
  driveGrid->addWidget(partitionSchemeBox_, 9, 0);
  driveGrid->addWidget(targetSystemBox_, 9, 1);
  page->addLayout(driveGrid);

  driveAdvancedButton_ = new QToolButton(central);
  driveAdvancedButton_->setText("Show advanced drive properties");
  driveAdvancedButton_->setArrowType(Qt::DownArrow);
  driveAdvancedButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  driveAdvancedButton_->setCheckable(true);
  driveAdvancedButton_->setAutoRaise(true);
  connect(driveAdvancedButton_, &QToolButton::toggled,
          this, &MainWindow::toggleDriveAdvanced);
  page->addWidget(driveAdvancedButton_);

  driveAdvancedPanel_ = new QWidget(central);
  auto* driveAdvancedLayout = new QVBoxLayout(driveAdvancedPanel_);
  driveAdvancedLayout->setContentsMargins(22, 0, 0, 0);
  listFixedDisks_ = new QCheckBox("List fixed and system disks", driveAdvancedPanel_);
  listFixedDisks_->setToolTip(
      "Shows protected devices for inspection only; they remain ineligible for writing.");
  connect(listFixedDisks_, &QCheckBox::toggled, this, &MainWindow::refreshVolumes);
  driveAdvancedLayout->addWidget(listFixedDisks_);
  driveAdvancedPanel_->hide();
  page->addWidget(driveAdvancedPanel_);

  page->addLayout(makeSectionHeader("Format Options", central));

  auto* formatGrid = new QGridLayout();
  formatGrid->setContentsMargins(0, 0, 0, 0);
  formatGrid->setHorizontalSpacing(10);
  formatGrid->setVerticalSpacing(3);
  formatGrid->setColumnStretch(0, 1);
  formatGrid->setColumnStretch(1, 1);

  formatGrid->addWidget(makeFieldLabel("Volume label", central), 0, 0, 1, 2);
  volumeLabel_ = new QLineEdit(central);
  volumeLabel_->setPlaceholderText("NO_LABEL");
  volumeLabel_->setEnabled(false);
  volumeLabel_->setMaxLength(32);
  connect(volumeLabel_, &QLineEdit::textChanged, this,
          [this](const QString&) { updateWriteReadiness(); });
  formatGrid->addWidget(volumeLabel_, 1, 0, 1, 2);

  formatGrid->addWidget(makeFieldLabel("File system", central), 2, 0);
  formatGrid->addWidget(makeFieldLabel("Cluster size", central), 2, 1);
  fileSystemBox_ = new QComboBox(central);
  fileSystemBox_->addItems({"FAT32", "NTFS", "exFAT",
                            "Mac OS Extended (Journaled)", "Image-defined"});
  fileSystemBox_->setEnabled(false);
  connect(fileSystemBox_, &QComboBox::currentIndexChanged, this,
          [this](const int) {
            updateDeploymentOptionControls();
            updateWriteReadiness();
          });
  clusterSizeBox_ = new QComboBox(central);
  clusterSizeBox_->addItem("Automatic (Default)", 0U);
  for (const unsigned int bytes : {512U, 1024U, 2048U, 4096U, 8192U,
                                   16384U, 32768U, 65536U}) {
    clusterSizeBox_->addItem(
        bytes < 1024U ? QString::number(bytes) + " bytes"
                      : QString::number(bytes / 1024U) + " KiB",
        bytes);
  }
  clusterSizeBox_->setEnabled(false);
  clusterSizeBox_->setToolTip(
      "Choose an allocation unit supported by the selected filesystem and target sector size.");
  connect(clusterSizeBox_, &QComboBox::currentIndexChanged, this,
          [this](const int) { updateWriteReadiness(); });
  formatGrid->addWidget(fileSystemBox_, 3, 0);
  formatGrid->addWidget(clusterSizeBox_, 3, 1);
  page->addLayout(formatGrid);

  formatAdvancedButton_ = new QToolButton(central);
  formatAdvancedButton_->setText("Hide advanced format options");
  formatAdvancedButton_->setArrowType(Qt::UpArrow);
  formatAdvancedButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  formatAdvancedButton_->setCheckable(true);
  formatAdvancedButton_->setChecked(true);
  formatAdvancedButton_->setAutoRaise(true);
  connect(formatAdvancedButton_, &QToolButton::toggled,
          this, &MainWindow::toggleFormatAdvanced);
  page->addWidget(formatAdvancedButton_);

  formatAdvancedPanel_ = new QWidget(central);
  auto* formatAdvancedLayout = new QGridLayout(formatAdvancedPanel_);
  formatAdvancedLayout->setContentsMargins(22, 0, 0, 0);
  formatAdvancedLayout->setHorizontalSpacing(10);
  formatAdvancedLayout->setVerticalSpacing(3);
  formatAdvancedLayout->setColumnStretch(0, 1);
  formatAdvancedLayout->setColumnStretch(1, 1);

  quickFormat_ = new QCheckBox("Quick format", formatAdvancedPanel_);
  quickFormat_->setChecked(true);
  quickFormat_->setEnabled(false);
  quickFormat_->setToolTip(
      "Clear this to overwrite and verify every otherwise-unused target sector. DD and FFU modes always use the image-defined layout.");
  connect(quickFormat_, &QCheckBox::toggled, this,
          [this](const bool) { updateWriteReadiness(); });
  auto* verificationLabel =
      new QLabel("Post-write verification", formatAdvancedPanel_);
  verificationProfileBox_ = new QComboBox(formatAdvancedPanel_);
  verificationProfileBox_->addItem("Fast (sampled)", "fast");
  verificationProfileBox_->addItem("Standard (all written data)", "standard");
  verificationProfileBox_->addItem("Full device", "full");
  verificationProfileBox_->addItem("Apple createinstallmedia (native)",
                                   "apple-native");
  verificationProfileBox_->addItem("Full pre-wipe + Apple native",
                                   "apple-full");
  verificationProfileBox_->setCurrentIndex(1);
  verificationProfileBox_->setToolTip(
      "Fast compares deterministic samples. Standard compares every written byte. Full device requires an image or full format covering the complete target.");
  connect(verificationProfileBox_, &QComboBox::currentIndexChanged, this,
          [this](const int) {
            updateDeploymentOptionControls();
            updateWriteReadiness();
          });
  auto* extendedLabel = new QCheckBox("Create extended label and icon files", formatAdvancedPanel_);
  extendedLabel->setChecked(false);
  extendedLabel->setEnabled(false);
  extendedLabel->setToolTip(
      "The cross-platform stager preserves image contents and does not add host-specific autorun files.");
  runtimeValidation_ =
      new QCheckBox("Validate UEFI media at boot", formatAdvancedPanel_);
  runtimeValidation_->setEnabled(false);
  runtimeValidation_->setToolTip(
      "Wrap supported UEFI fallback loaders with Rufus++'s offline MD5 validation app and create a complete manifest.");
  connect(runtimeValidation_, &QCheckBox::toggled, this,
          [this](const bool) { updateWriteReadiness(); });
  badBlocks_ = new QCheckBox("Check device for bad blocks", formatAdvancedPanel_);
  badBlocks_->setEnabled(deviceBackend_ && deviceBackend_->capabilities().badBlockTest);
  badBlocks_->setToolTip(
      "Destructively test the complete device with address-dependent patterns before deployment."
      " This also detects fake-capacity media.");
  badBlockPassCount_ = new QComboBox(formatAdvancedPanel_);
  badBlockPassCount_->addItems({"1 pass", "2 passes", "3 passes", "4 passes"});
  badBlockPassCount_->setEnabled(false);
  connect(badBlocks_, &QCheckBox::toggled, badBlockPassCount_,
          &QComboBox::setEnabled);
  formatAdvancedLayout->addWidget(quickFormat_, 0, 0, 1, 2);
  formatAdvancedLayout->addWidget(verificationLabel, 1, 0);
  formatAdvancedLayout->addWidget(verificationProfileBox_, 1, 1);
  formatAdvancedLayout->addWidget(extendedLabel, 2, 0, 1, 2);
  formatAdvancedLayout->addWidget(runtimeValidation_, 3, 0, 1, 2);
  formatAdvancedLayout->addWidget(badBlocks_, 4, 0);
  formatAdvancedLayout->addWidget(badBlockPassCount_, 4, 1);
  page->addWidget(formatAdvancedPanel_);

  page->addLayout(makeSectionHeader("Status", central));
  progressBar_ = new QProgressBar(central);
  progressBar_->setRange(0, 100);
  progressBar_->setValue(0);
  progressBar_->setFormat("READY");
  progressBar_->setToolTip(
      "DD writes require a validated removable target and authorized raw-device access.");
  page->addWidget(progressBar_);

  // Native macOS progress bars do not reliably display their format text.
  // Keep stage and byte counts in an independent, accessible text widget.
  operationProgressLabel_ = new QLabel(central);
  operationProgressLabel_->setObjectName("operationProgress");
  operationProgressLabel_->setTextFormat(Qt::PlainText);
  operationProgressLabel_->setWordWrap(true);
  operationProgressLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  operationProgressLabel_->setMinimumHeight(
      operationProgressLabel_->fontMetrics().height() * 2);
  operationProgressLabel_->hide();
  page->addWidget(operationProgressLabel_);
  progressRefreshTimer_ = new QTimer(this);
  progressRefreshTimer_->setInterval(1000);
  progressRefreshTimer_->setTimerType(Qt::PreciseTimer);
  connect(progressRefreshTimer_, &QTimer::timeout,
          this, &MainWindow::refreshOperationProgress);

  logView_ = new QTextEdit(central);
  logView_->setReadOnly(true);
  logView_->setMinimumHeight(90);
  logView_->setPlaceholderText("Operation log");
  logView_->hide();
  page->addWidget(logView_);

  page->addStretch(1);

  auto* actions = new QHBoxLayout();
  actions->setContentsMargins(0, 3, 0, 0);
  actions->setSpacing(6);
  auto* languageButton =
      makeToolbarButton(":/rufus/icons/lang-24.png", "Language", central);
  connect(languageButton, &QToolButton::clicked, this, [this] {
    QMessageBox::information(
        this, "Language", "English is the only language bundled in this build.");
  });
  actions->addWidget(languageButton);
  auto* aboutButton =
      makeToolbarButton(":/rufus/icons/info-24.png", "About Rufus++", central);
  connect(aboutButton, &QToolButton::clicked, this, [this] {
    QString contents =
        fromView(core::ApplicationInfo::displayName) + " v" +
        fromView(core::ApplicationInfo::version) +
        "\n\nA GPLv3 cross-platform boot-media utility derived from Rufus."
        "\n\nPhysical-media validation is still required before release use.\n\n"
        "Secure Boot DBX: " +
        QString::fromStdString(secureBootDatabase_.version) +
        (secureBootDatabase_.publishedDate.empty()
             ? QString{}
             : " (published " +
                   QString::fromStdString(secureBootDatabase_.publishedDate) +
                   ')') +
        "\nSecure Boot source: " +
        QString::fromStdString(secureBootDatabase_.source) +
        "\n\nBundled UEFI Shell payloads are copyright TianoCore and "
        "contributors and are distributed under BSD-2-Clause-Patent.\n\n";
    QFile shellLicense(":/rufus/uefi-shell/License.txt");
    if (shellLicense.open(QIODevice::ReadOnly)) {
      contents += QString::fromUtf8(shellLicense.readAll());
    } else {
      contents += "The bundled UEFI Shell licence resource is unavailable.";
    }
    contents += "\n\nMicrosoft Secure Boot data license\n\n";
    QFile secureBootLicense(":/rufus/secure-boot/License.txt");
    if (secureBootLicense.open(QIODevice::ReadOnly)) {
      contents += QString::fromUtf8(secureBootLicense.readAll());
    } else {
      contents += "The bundled Secure Boot data licence resource is unavailable.";
    }
    showTextReport(this, "About Rufus++", contents);
  });
  actions->addWidget(aboutButton);
  auto* settingsButton =
      makeToolbarButton(
          ":/rufus/icons/settings-24.png",
          "Settings and capability health\nSecure Boot DBX " +
              QString::fromStdString(secureBootDatabase_.version),
          central);
  connect(settingsButton, &QToolButton::clicked, this,
          &MainWindow::showDiagnostics);
  actions->addWidget(settingsButton);
  formatButton_ =
      makeToolbarButton(":/rufus/icons/format-24.svg", "Format device", central);
  formatButton_->setEnabled(false);
  connect(formatButton_, &QToolButton::clicked,
          this, &MainWindow::formatDevice);
  actions->addWidget(formatButton_);
  captureButton_ =
      makeToolbarButton(":/rufus/icons/save-24.png", "Save device to image", central);
  captureButton_->setEnabled(false);
  connect(captureButton_, &QToolButton::clicked,
          this, &MainWindow::captureDevice);
  actions->addWidget(captureButton_);
  checksumButton_ =
      makeToolbarButton(":/rufus/icons/hash-24.png", "Compute image checksums", central);
  checksumButton_->setEnabled(false);
  connect(checksumButton_, &QToolButton::clicked,
          this, &MainWindow::calculateChecksums);
  actions->addWidget(checksumButton_);
  auto* logButton = makeToolbarButton(":/rufus/icons/log-24.png", "Show or hide log", central);
  connect(logButton, &QToolButton::clicked, this, &MainWindow::toggleLog);
  actions->addWidget(logButton);
  actions->addStretch(1);

  startButton_ = new QPushButton("START", central);
  startButton_->setMinimumWidth(100);
  startButton_->setEnabled(false);
  startButton_->setToolTip("Select a supported image operation and an eligible target.");
  connect(startButton_, &QPushButton::clicked, this, &MainWindow::startWrite);
  closeButton_ = new QPushButton("CLOSE", central);
  closeButton_->setMinimumWidth(100);
  connect(closeButton_, &QPushButton::clicked, this, &MainWindow::closeOrCancel);
  actions->addWidget(startButton_);
  actions->addWidget(closeButton_);
  page->addLayout(actions);

  setCentralWidget(central);
  statusBar()->setSizeGripEnabled(false);
  statusBar()->showMessage("Ready — select an image and eligible target");
  privilegeLabel_ = new QLabel(this);
  privilegeLabel_->setObjectName("privilegeIndicator");
  statusBar()->addPermanentWidget(privilegeLabel_);
  static_cast<void>(updatePrivilegeIndicator());
  appendLog("Qt user interface initialized on " + fromView(platform) + '.');
  scheduleWindowFitToContents();
}

bool MainWindow::updatePrivilegeIndicator() {
  if (privilegeLabel_ == nullptr) {
    return false;
  }
  const backend::PrivilegeStatus status =
      deviceBackend_ != nullptr
          ? deviceBackend_->privilegeStatus()
          : backend::PrivilegeStatus{
                backend::PrivilegeRoute::Unprivileged,
                "No platform device backend is available"};
  const QString label = privilegeRouteLabel(status.route);
  const bool labelChanged = privilegeLabel_->text() != label;
  privilegeLabel_->setText(label);
  privilegeLabel_->setToolTip(QString::fromStdString(status.detail));
  privilegeLabel_->setAccessibleName("Privilege status: " + label);
  switch (status.route) {
    case backend::PrivilegeRoute::Unprivileged:
      privilegeLabel_->setStyleSheet(
          "QLabel { background: #9b2c2c; color: white; font-weight: 700; padding: 2px 7px; border-radius: 3px; }");
      break;
    case backend::PrivilegeRoute::Elevated:
      privilegeLabel_->setStyleSheet(
          "QLabel { background: #1a7f37; color: white; font-weight: 700; padding: 2px 7px; border-radius: 3px; }");
      break;
    case backend::PrivilegeRoute::PrivilegedHelper:
      privilegeLabel_->setStyleSheet(
          "QLabel { background: #1f6feb; color: white; font-weight: 700; padding: 2px 7px; border-radius: 3px; }");
      break;
  }
  if (labelChanged) {
    appendLog("Privilege route: " + label + ". " +
              QString::fromStdString(status.detail));
    scheduleWindowFitToContents();
  }
  return labelChanged;
}

void MainWindow::calculateChecksums() {
  if (!selectedImage_ || checksumThread_ != nullptr ||
      imageAnalysisThread_ != nullptr || writeThread_ != nullptr ||
      captureThread_ != nullptr) {
    return;
  }
  const QString path = bootSelectionBox_->currentData().toString();
  if (path.isEmpty()) {
    return;
  }

  auto result = std::make_shared<ChecksumResult>();
  cancelRequested_.store(false);
  selectButton_->setEnabled(false);
  checksumButton_->setEnabled(false);
  captureButton_->setEnabled(false);
  formatButton_->setEnabled(false);
  startButton_->setEnabled(false);
  closeButton_->setText("CANCEL");
  progressBar_->setRange(0, 100);
  progressBar_->setValue(0);
  progressBar_->setFormat("CHECKSUMS — %p%");
  statusBar()->showMessage("Computing MD5, SHA-1, SHA-256 and SHA-512");
  appendLog("Computing checksums for " + path + '.');
  beginOperationGuard();

  QThread* const thread = QThread::create([this, path, result] {
    QFile input(path);
    const QFileInfo before(path);
    const qint64 expectedSize = before.size();
    const QDateTime expectedTimestamp = before.lastModified();
    if (!before.isFile() || expectedSize < 0 || !input.open(QIODevice::ReadOnly)) {
      result->error = "Unable to open the selected image for checksum calculation.";
      return;
    }
    QCryptographicHash md5(QCryptographicHash::Md5);
    QCryptographicHash sha1(QCryptographicHash::Sha1);
    QCryptographicHash sha256(QCryptographicHash::Sha256);
    QCryptographicHash sha512(QCryptographicHash::Sha512);
    constexpr qint64 chunkBytes = 8LL * 1024LL * 1024LL;
    qint64 completed = 0;
    while (!input.atEnd()) {
      if (cancelRequested_.load()) {
        result->cancelled = true;
        result->error = "Checksum calculation cancelled.";
        return;
      }
      const QByteArray chunk = input.read(chunkBytes);
      if (chunk.isEmpty() && input.error() != QFileDevice::NoError) {
        result->error = "Unable to read the selected image: " + input.errorString();
        return;
      }
      md5.addData(chunk);
      sha1.addData(chunk);
      sha256.addData(chunk);
      sha512.addData(chunk);
      completed += chunk.size();
      const int percentage = expectedSize == 0
                                 ? 100
                                 : static_cast<int>(std::min<qint64>(
                                       100, completed * 100 / expectedSize));
      static_cast<void>(QMetaObject::invokeMethod(
          this,
          [this, percentage] {
            noteOperationProgress();
            progressBar_->setValue(percentage);
            statusBar()->showMessage(
                "Computing image checksums — " + QString::number(percentage) + '%');
          },
          Qt::QueuedConnection));
    }
    const QFileInfo after(path);
    if (!after.isFile() || after.size() != expectedSize ||
        after.lastModified() != expectedTimestamp || completed != expectedSize) {
      result->error = "The selected image changed while its checksums were computed.";
      return;
    }
    result->md5 = QString::fromLatin1(md5.result().toHex());
    result->sha1 = QString::fromLatin1(sha1.result().toHex());
    result->sha256 = QString::fromLatin1(sha256.result().toHex());
    result->sha512 = QString::fromLatin1(sha512.result().toHex());
    result->success = true;
  });
  checksumThread_ = thread;
  connect(thread, &QThread::finished, this, [this, thread, result] {
    if (checksumThread_ == thread) {
      checksumThread_ = nullptr;
    }
    thread->deleteLater();
    endOperationGuard();
    selectButton_->setEnabled(true);
    closeButton_->setText("CLOSE");
    closeButton_->setEnabled(true);
    if (result->success) {
      latestImageSha256_ = result->sha256.toStdString();
      latestChecksumImagePath_ =
          bootSelectionBox_->currentData().toString().toStdString();
      progressBar_->setValue(100);
      progressBar_->setFormat("CHECKSUMS COMPLETE");
      const QString summary =
          "MD5\n" + result->md5 + "\n\nSHA-1\n" + result->sha1 +
          "\n\nSHA-256\n" + result->sha256 + "\n\nSHA-512\n" + result->sha512;
      appendLog("MD5: " + result->md5);
      appendLog("SHA-1: " + result->sha1);
      appendLog("SHA-256: " + result->sha256);
      appendLog("SHA-512: " + result->sha512);
      QMessageBox box(QMessageBox::Information, "Image checksums",
                      "All image checksums were computed successfully.",
                      QMessageBox::Ok, this);
      box.setDetailedText(summary);
      box.exec();
      statusBar()->showMessage("Image checksums computed successfully");
    } else {
      progressBar_->setFormat(result->cancelled ? "CANCELLED" : "CHECKSUM ERROR");
      statusBar()->showMessage(result->error);
      if (!result->cancelled) {
        QMessageBox::warning(this, "Checksum error", result->error);
      }
    }
    updateWriteReadiness();
  });
  thread->start();
}

void MainWindow::showDiagnostics() {
  const auto yesNo = [](const bool value) {
    return value ? QStringLiteral("available") : QStringLiteral("unavailable");
  };
  QString report = fromView(core::ApplicationInfo::displayName) + " v" +
                   fromView(core::ApplicationInfo::version) +
                   "\nCapability and dependency health\n\n";
  if (!deviceBackend_) {
    report += "Device backend: unavailable\n";
  } else {
    const auto capabilities = deviceBackend_->capabilities();
    report += "Device backend: " + fromView(deviceBackend_->name()) + "\n";
    report += "  Physical discovery: " + yesNo(capabilities.physicalDeviceDiscovery) + "\n";
    report += "  Read-only inspection: " + yesNo(capabilities.readOnlyInspection) + "\n";
    report += "  Raw-device writing: " + yesNo(capabilities.rawWrite) + "\n";
    report += "  Identity revalidation: " + yesNo(capabilities.identityRevalidation) + "\n";
    report += "  Exclusive access: " + yesNo(capabilities.exclusiveAccess) + "\n";
    report += "  Write verification: " + yesNo(capabilities.rawVerification) + "\n";
    report += "  Media capture: " + yesNo(capabilities.mediaCapture) + "\n";
    report += "  Bad-block testing: " + yesNo(capabilities.badBlockTest) + "\n";
  }

  const core::BlockDeviceInfo* diagnosticTarget = nullptr;
  const QVariant selectedTarget = volumeBox_->currentData(Qt::UserRole + 2);
  if (selectedTarget.isValid()) {
    const auto index =
        static_cast<std::size_t>(selectedTarget.toULongLong());
    if (index < visibleDevices_.size()) {
      diagnosticTarget = &visibleDevices_[index];
      report += "\nSelected target\n  " +
                QString::fromStdString(diagnosticTarget->displayName) + "\n  " +
                QString::fromStdString(diagnosticTarget->devicePath) + "\n";
      if (deviceBackend_) {
        const auto availability =
            deviceBackend_->rawWriteAvailability(*diagnosticTarget);
        report += "  Raw access: " + yesNo(availability.available) +
                  "\n    " + QString::fromStdString(availability.reason) + "\n";
      }
    }
  }

  report += "\nImage/deployment providers\n";
  const auto splitter = core::createSystemWimSplitter();
  report += "  wimlib split/export: " + yesNo(splitter->available()) +
            "\n    " + QString::fromStdString(splitter->availabilityReason()) + "\n";
  const auto applicator = core::createSystemWimApplicator();
  report += "  wimlib apply: " + yesNo(applicator->available()) +
            "\n    " + QString::fromStdString(applicator->availabilityReason()) + "\n";
  if (ntfsIsoStager_) {
    const auto availability = ntfsIsoStager_->availability();
    report += "  NTFS/UEFI:NTFS staging: " + yesNo(availability.available) +
              "\n    " + QString::fromStdString(availability.reason) + "\n";
  }
  if (windowsToGoStager_) {
    const auto availability = windowsToGoStager_->availability();
    report += "  Windows To Go staging: " + yesNo(availability.available) +
              "\n    " + QString::fromStdString(availability.reason) + "\n";
  }
  if (filesystemStager_ && diagnosticTarget != nullptr) {
    const std::array filesystems{
        backend::StandaloneFilesystem::Ntfs,
        backend::StandaloneFilesystem::UefiNtfs,
        backend::StandaloneFilesystem::ExFat,
        backend::StandaloneFilesystem::Udf,
        backend::StandaloneFilesystem::ReFs,
        backend::StandaloneFilesystem::Ext3};
    for (const auto filesystem : filesystems) {
      const auto availability =
          filesystemStager_->availability(filesystem, *diagnosticTarget);
      report += "  " +
                QString::fromUtf8(
                    backend::standaloneFilesystemName(filesystem)) +
                " staging: " + yesNo(availability.available) + "\n    " +
                QString::fromStdString(availability.reason) + "\n";
    }
  } else {
    report += "  Standalone host formatters: select a target for a per-filesystem check\n";
  }

  const QString qemu = qemuExecutable(
      selectedImage_ ? selectedImage_->architecture : core::ImageArchitecture::X64);
  report += "\nOptional validation tools\n";
  report += "  QEMU boot smoke test: " + yesNo(!qemu.isEmpty()) +
            (qemu.isEmpty() ? "\n" : "\n    " + qemu + "\n");
  const QString firmware = ovmfFirmwarePath();
  report += "  OVMF x86 UEFI firmware: " + yesNo(!firmware.isEmpty()) +
            (firmware.isEmpty() ? "\n" : "\n    " + firmware + "\n");
  report += "  Runtime UEFI validation assets: " +
            yesNo(runtimeValidationAssets_ != nullptr) + "\n";
  report += "\nSecure Boot revocation data\n";
  report += "  DBX version: " +
            (secureBootDatabase_.version.empty()
                 ? QStringLiteral("unversioned")
                 : QString::fromStdString(secureBootDatabase_.version)) +
            "\n";
  if (!secureBootDatabase_.publishedDate.empty()) {
    report += "  Published: " +
              QString::fromStdString(secureBootDatabase_.publishedDate) + "\n";
  }
  report += "  Source: " + QString::fromStdString(secureBootDatabase_.source) +
            "\n";
  report += "  Baseline integrity: " +
            QString(secureBootDatabase_.trustedBaseline
                        ? "verified official data"
                        : "custom/unverified overlay") +
            "\n";
  report += "    Exact revoked hashes: " +
            QString::number(secureBootDatabase_.revokedSha256.size()) + "\n";
  report += "    SBAT policy version: " +
            (secureBootDatabase_.sbatVersion.empty()
                 ? QStringLiteral("unversioned")
                 : QString::fromStdString(secureBootDatabase_.sbatVersion)) +
            "\n";
  report += "    SBAT component floors: " +
            QString::number(secureBootDatabase_.minimumSbatGeneration.size()) +
            "\n";

  report += "\nPrivilege boundary\n";
  const backend::PrivilegeStatus privilege =
      deviceBackend_ != nullptr
          ? deviceBackend_->privilegeStatus()
          : backend::PrivilegeStatus{
                backend::PrivilegeRoute::Unprivileged,
                "No platform device backend is available"};
  report += "  " + privilegeRouteLabel(privilege.route) + ": " +
            QString::fromStdString(privilege.detail) + "\n";
  const QString receiptDirectory =
      QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
          .filePath("receipts");
  report += "\nDeployment receipts\n  " + receiptDirectory + "\n";

  QDialog dialog(this);
  dialog.setWindowTitle("Capability health");
  dialog.resize(700, 560);
  auto* layout = new QVBoxLayout(&dialog);
  auto* view = new QTextEdit(&dialog);
  view->setReadOnly(true);
  view->setLineWrapMode(QTextEdit::NoWrap);
  view->setPlainText(report);
  layout->addWidget(view, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  auto* inspect = buttons->addButton("Inspect target", QDialogButtonBox::ActionRole);
  auto* trust = buttons->addButton("Analyze image trust", QDialogButtonBox::ActionRole);
  auto* boot = buttons->addButton("Virtual boot test", QDialogButtonBox::ActionRole);
  auto* update = buttons->addButton("Check DBX updates", QDialogButtonBox::ActionRole);
  auto* import = buttons->addButton("Import trust data", QDialogButtonBox::ActionRole);
  inspect->setEnabled(deviceBackend_ &&
                      deviceBackend_->capabilities().readOnlyInspection);
  trust->setEnabled(selectedImage_.has_value());
  boot->setEnabled(selectedImage_.has_value());
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked,
          &dialog, &QDialog::accept);
  connect(inspect, &QPushButton::clicked, &dialog, [this, &dialog] {
    dialog.accept();
    QTimer::singleShot(0, this, &MainWindow::inspectSelectedDevice);
  });
  connect(trust, &QPushButton::clicked, &dialog, [this, &dialog] {
    dialog.accept();
    QTimer::singleShot(0, this, &MainWindow::analyzeSelectedImageTrust);
  });
  connect(boot, &QPushButton::clicked, &dialog, [this, &dialog] {
    dialog.accept();
    QTimer::singleShot(0, this, &MainWindow::runVirtualBootTest);
  });
  connect(update, &QPushButton::clicked, &dialog,
          [this, view, update, &dialog] {
            update->setEnabled(false);
            const SecureBootDataRefreshResult refreshed =
                refreshSecureBootData(&dialog, secureBootDatabase_);
            update->setEnabled(true);
            if (refreshed.cancelled) {
              return;
            }
            if (!refreshed.success) {
              QMessageBox::warning(&dialog, "Secure Boot update failed",
                                   refreshed.error);
              return;
            }
            secureBootDatabase_ = refreshed.database;
            view->append("\n" + refreshed.message);
            appendLog(refreshed.message);
            if (refreshed.alreadyCurrent) {
              QMessageBox::information(&dialog, "Secure Boot data",
                                       refreshed.message);
            } else {
              QMessageBox::information(
                  &dialog, "Secure Boot data updated",
                  refreshed.message +
                      "\nThe new database is active and will be reused offline.");
            }
          });
  connect(import, &QPushButton::clicked, &dialog, [this, view, &dialog] {
    const QString path = QFileDialog::getOpenFileName(
        &dialog, "Import offline Secure Boot data", QString(),
        "Secure Boot data (*.txt *.db *.esl *.auth *.auth2 *.efiauth2);;All files (*)");
    if (path.isEmpty()) {
      return;
    }
    const auto loaded = core::loadSecureBootDatabase(
        fileSystemPath(path), secureBootDatabase_);
    if (!loaded.success) {
      QMessageBox::warning(&dialog, "Trust data rejected",
                           QString::fromStdString(loaded.error));
      return;
    }
    secureBootDatabase_ = loaded.database;
    view->append("\nImported custom offline trust data: " + path +
                 "\nBaseline status: custom/unverified overlay");
    appendLog("Imported offline Secure Boot trust data from " + path + '.');
  });
  layout->addWidget(buttons);
  dialog.exec();
}

void MainWindow::inspectSelectedDevice() {
  if (!deviceBackend_ || writeThread_ != nullptr || captureThread_ != nullptr) {
    return;
  }
  const QVariant selected = volumeBox_->currentData(Qt::UserRole + 2);
  if (!selected.isValid()) {
    QMessageBox::warning(this, "No target", "Select a physical device first.");
    return;
  }
  const auto index = static_cast<std::size_t>(selected.toULongLong());
  if (index >= visibleDevices_.size()) {
    QMessageBox::warning(this, "Stale target",
                         "Refresh and select the physical device again.");
    return;
  }
  const auto& device = visibleDevices_[index];
  appendLog("Reading bounded, read-only partition samples from " +
            QString::fromStdString(device.devicePath) + '.');
  const core::MediaInspectionResult inspection =
      deviceBackend_->inspectReadOnly(device);
  showTextReport(this, "Read-only media inspection",
                 QString::fromStdString(inspection.toText(device)));
}

void MainWindow::analyzeSelectedImageTrust() {
  if (!selectedImage_ || analysisToolThread_ != nullptr ||
      imageAnalysisThread_ != nullptr || checksumThread_ != nullptr ||
      captureThread_ != nullptr || writeThread_ != nullptr) {
    return;
  }
  if (selectedImage_->format != core::ImageFormat::Iso ||
      !selectedImage_->capabilities.isoExtraction) {
    QMessageBox::information(
        this, "Secure Boot analysis unavailable",
        "Offline Secure Boot analysis currently requires a readable ISO-9660, Joliet, or UDF image.");
    return;
  }
  const core::ImageInfo image = *selectedImage_;
  const core::SecureBootDatabase database = secureBootDatabase_;
  auto result = std::make_shared<core::SecureBootAnalysisResult>();
  analysisCancelRequested_.store(false);
  selectButton_->setEnabled(false);
  startButton_->setEnabled(false);
  closeButton_->setText("CANCEL");
  progressBar_->setRange(0, 0);
  progressBar_->setFormat("CHECKING SECURE BOOT");
  statusBar()->showMessage("Inspecting EFI loaders against offline trust data");
  appendLog("Starting offline Secure Boot analysis of " +
            QString::fromStdString(image.path) + '.');
  QThread* const thread = QThread::create([this, image, database, result] {
    *result = core::SecureBootAnalyzer{}.analyze(
        image, database,
        [this] { return analysisCancelRequested_.load(); });
  });
  analysisToolThread_ = thread;
  connect(thread, &QThread::finished, this, [this, thread, result] {
    if (analysisToolThread_ == thread) {
      analysisToolThread_ = nullptr;
    }
    thread->deleteLater();
    progressBar_->setRange(0, 100);
    closeButton_->setText("CLOSE");
    closeButton_->setEnabled(true);
    if (result->success) {
      progressBar_->setValue(100);
      progressBar_->setFormat(result->hasRevokedImage()
                                  ? "REVOCATION FOUND"
                                  : "TRUST CHECK COMPLETE");
      statusBar()->showMessage(result->hasRevokedImage()
                                   ? "A known revoked EFI loader was detected"
                                   : "No known offline hash/SBAT revocation was detected");
      appendLog(result->hasRevokedImage()
                    ? "Offline Secure Boot analysis found a revoked loader."
                    : "Offline Secure Boot analysis found no known revoked loader.");
      showTextReport(this, "Offline Secure Boot analysis",
                     QString::fromStdString(result->toText()));
    } else {
      progressBar_->setValue(0);
      progressBar_->setFormat(result->cancelled ? "CANCELLED" : "TRUST CHECK FAILED");
      statusBar()->showMessage(QString::fromStdString(result->error));
      if (!result->cancelled) {
        QMessageBox::warning(this, "Secure Boot analysis failed",
                             QString::fromStdString(result->error));
      }
    }
    updateWriteReadiness();
    if (closeWhenFinished_) {
      closeWhenFinished_ = false;
      close();
    }
  });
  thread->start();
}

void MainWindow::runVirtualBootTest() {
  if (!selectedImage_ || imageAnalysisThread_ != nullptr ||
      analysisToolThread_ != nullptr || checksumThread_ != nullptr ||
      captureThread_ != nullptr || writeThread_ != nullptr) {
    return;
  }
  if (selectedImage_->compressed ||
      (selectedImage_->format != core::ImageFormat::Iso &&
       selectedImage_->format != core::ImageFormat::Raw)) {
    QMessageBox::information(
        this, "Virtual boot test unavailable",
        "The read-only QEMU smoke test currently accepts ISO and raw images. Decompress or convert this image first.");
    return;
  }
  const QString executable = qemuExecutable(selectedImage_->architecture);
  if (executable.isEmpty()) {
    QMessageBox::information(
        this, "QEMU unavailable",
        "Install the QEMU system emulator for this image architecture to enable the optional virtual boot smoke test.");
    return;
  }
  const bool uefiOnly = selectedImage_->capabilities.uefiBootable &&
                        !selectedImage_->capabilities.biosBootable;
  const bool requestUefi = uefiOnly ||
      targetSystemBox_->currentData().toString() == "uefi";
  const QString firmware = requestUefi ? ovmfFirmwarePath() : QString{};
  if (requestUefi && firmware.isEmpty()) {
    QMessageBox::information(
        this, "UEFI firmware unavailable",
        "The image requires UEFI, but a supported read-only OVMF firmware image was not found in a standard location.");
    return;
  }

  QStringList arguments{"-machine", "accel=tcg", "-m", "512",
                        "-no-reboot", "-display", "none", "-serial",
                        "none", "-monitor", "none"};
  if (!firmware.isEmpty()) {
    arguments << "-drive"
              << "if=pflash,format=raw,readonly=on,file=" + firmware;
  }
  const QString imagePath = QString::fromStdString(selectedImage_->path);
  if (selectedImage_->format == core::ImageFormat::Iso) {
    arguments << "-boot" << "d" << "-cdrom" << imagePath;
  } else {
    arguments << "-drive"
              << "file=" + imagePath + ",format=raw,readonly=on";
  }

  QDialog dialog(this);
  dialog.setWindowTitle("Virtual boot smoke test");
  auto* layout = new QVBoxLayout(&dialog);
  auto* label = new QLabel(
      "Launching the image read-only. Passing means QEMU remained alive for eight seconds; it is not a substitute for hardware boot testing.",
      &dialog);
  label->setWordWrap(true);
  layout->addWidget(label);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  QProcess process(&dialog);
  bool observationCompleted = false;
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(&process,
          qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog,
          [&dialog](int, QProcess::ExitStatus) { dialog.accept(); });
  process.setProcessChannelMode(QProcess::SeparateChannels);
  process.start(executable, arguments, QIODevice::ReadOnly);
  if (!process.waitForStarted(3000)) {
    QMessageBox::warning(this, "QEMU launch failed", process.errorString());
    return;
  }
  QTimer::singleShot(8000, &dialog, [&dialog, &process, &observationCompleted] {
    if (process.state() != QProcess::NotRunning) {
      observationCompleted = true;
      process.terminate();
      dialog.accept();
    }
  });
  dialog.exec();
  if (process.state() != QProcess::NotRunning) {
    process.terminate();
    if (!process.waitForFinished(1000)) {
      process.kill();
      process.waitForFinished(1000);
    }
  }
  const QString diagnostics = QString::fromUtf8(process.readAllStandardError()).trimmed();
  if (observationCompleted) {
    appendLog("QEMU read-only boot smoke test passed its eight-second observation window.");
    QMessageBox result(QMessageBox::Information, "Virtual boot test",
                       "The image passed the QEMU launch smoke test.",
                       QMessageBox::Ok, this);
    result.setInformativeText(
        "QEMU remained active for the observation window. Firmware, USB controllers and physical hardware may still behave differently.");
    if (!diagnostics.isEmpty()) {
      result.setDetailedText(diagnostics);
    }
    result.exec();
  } else if (dialog.result() == QDialog::Rejected) {
    appendLog("QEMU boot smoke test cancelled.");
  } else {
    QMessageBox result(QMessageBox::Warning, "Virtual boot test",
                       "QEMU exited before the observation window completed.",
                       QMessageBox::Ok, this);
    result.setDetailedText(diagnostics.isEmpty()
                               ? "QEMU returned exit code " +
                                     QString::number(process.exitCode())
                               : diagnostics);
    result.exec();
  }
}

void MainWindow::captureDevice() {
  if (captureThread_ != nullptr || imageAnalysisThread_ != nullptr ||
      checksumThread_ != nullptr || writeThread_ != nullptr || !deviceBackend_) {
    return;
  }
  const QVariant selectedDevice = volumeBox_->currentData(Qt::UserRole + 2);
  if (!selectedDevice.isValid()) {
    QMessageBox::warning(this, "No source device",
                         "Select an eligible removable device first.");
    return;
  }
  const auto deviceIndex =
      static_cast<std::size_t>(selectedDevice.toULongLong());
  if (deviceIndex >= visibleDevices_.size()) {
    QMessageBox::warning(this, "Stale source",
                         "Refresh and select the source device again.");
    return;
  }
  const core::BlockDeviceInfo source = visibleDevices_[deviceIndex];
  const std::array formats{
      core::MediaCaptureFormat::Raw,
      core::MediaCaptureFormat::FixedVhd,
      core::MediaCaptureFormat::DynamicVhd,
      core::MediaCaptureFormat::DynamicVhdx,
      core::MediaCaptureFormat::Ffu,
      core::MediaCaptureFormat::UdfIso,
  };
  QStringList formatLabels;
  std::vector<core::MediaCaptureFormat> offeredFormats;
  for (const auto format : formats) {
    const auto availability = deviceBackend_->captureAvailability(source, format);
    if (availability.available || availability.authorizationCanBeRequested) {
      formatLabels.push_back(QString::fromUtf8(core::mediaCaptureFormatName(format)));
      offeredFormats.push_back(format);
    }
  }
  if (offeredFormats.empty()) {
    QMessageBox::warning(
        this, "Capture unavailable",
        "No device-capture provider is available for the selected source on this platform.");
    return;
  }
  bool accepted = false;
  const QString selectedFormat = QInputDialog::getItem(
      this, "Save device to image", "Image format:", formatLabels, 0, false,
      &accepted);
  if (!accepted) {
    return;
  }
  const int formatIndex = formatLabels.indexOf(selectedFormat);
  if (formatIndex < 0 ||
      static_cast<std::size_t>(formatIndex) >= offeredFormats.size()) {
    return;
  }
  const auto format = offeredFormats[static_cast<std::size_t>(formatIndex)];
  auto availability = deviceBackend_->captureAvailability(source, format);
  if (!availability.available && availability.authorizationCanBeRequested) {
    const auto authorization = deviceBackend_->requestRawWriteAuthorization();
    static_cast<void>(updatePrivilegeIndicator());
    appendLog("Administrative disk access: " +
              QString::fromStdString(authorization.reason));
    if (!authorization.available) {
      QMessageBox::information(this, "Administrative access required",
                               QString::fromStdString(authorization.reason));
      updateWriteReadiness();
      return;
    }
    availability = deviceBackend_->captureAvailability(source, format);
  }
  if (!availability.available) {
    QMessageBox::warning(this, "Capture unavailable",
                         QString::fromStdString(availability.reason));
    return;
  }
  QString extension;
  QString filter;
  switch (format) {
    case core::MediaCaptureFormat::Raw:
      extension = ".img";
      filter = "Raw disk images (*.img *.dd *.raw)";
      break;
    case core::MediaCaptureFormat::FixedVhd:
    case core::MediaCaptureFormat::DynamicVhd:
      extension = ".vhd";
      filter = "Virtual hard disks (*.vhd)";
      break;
    case core::MediaCaptureFormat::DynamicVhdx:
      extension = ".vhdx";
      filter = "Virtual hard disks (*.vhdx)";
      break;
    case core::MediaCaptureFormat::Ffu:
      extension = ".ffu";
      filter = "Full Flash Update images (*.ffu)";
      break;
    case core::MediaCaptureFormat::UdfIso:
      extension = ".iso";
      filter = "UDF ISO images (*.iso *.udf)";
      break;
  }
  QString destination = QFileDialog::getSaveFileName(
      this, "Save captured device", "device" + extension,
      filter + ";;All files (*)");
  if (destination.isEmpty()) {
    return;
  }
  if (QFileInfo(destination).suffix().isEmpty()) {
    destination += extension;
  }
  if (QFileInfo::exists(destination)) {
    QMessageBox::warning(
        this, "Destination exists",
        "Choose a new output filename. Capture never overwrites an existing file.");
    return;
  }
  const QString confirmation =
      "Rufus++ will read the complete source device and save a verified image.\n\n"
      "Source: " + QString::fromStdString(source.displayName) + "\n" +
      "Device: " + QString::fromStdString(source.devicePath) + "\n" +
      "Capacity: " + fromView(core::formatByteSize(source.capacityBytes)) + "\n" +
      "Format: " + selectedFormat + "\n" +
      "Destination: " + destination + "\n\nContinue?";
  if (QMessageBox::question(this, "Confirm device capture", confirmation,
                            QMessageBox::Yes | QMessageBox::Cancel,
                            QMessageBox::Cancel) != QMessageBox::Yes) {
    return;
  }

  auto outcome = std::make_shared<core::MediaCaptureResult>();
  const std::filesystem::path destinationPath = fileSystemPath(destination);
  const core::MediaCaptureOptions options{format, 8U * 1024U * 1024U, true};
  cancelRequested_.store(false);
  closeWhenFinished_ = false;
  volumeBox_->setEnabled(false);
  selectButton_->setEnabled(false);
  checksumButton_->setEnabled(false);
  captureButton_->setEnabled(false);
  formatButton_->setEnabled(false);
  startButton_->setEnabled(false);
  closeButton_->setText("CANCEL");
  progressBar_->setRange(0, 100);
  progressBar_->setValue(0);
  progressBar_->setFormat("CAPTURING — %p%");
  statusBar()->showMessage("Capturing " + QString::fromStdString(source.devicePath));
  appendLog("Capturing " + QString::fromStdString(source.devicePath) + " to " +
            destination + " as " + selectedFormat + '.');
  beginOperationGuard();

  QThread* const thread = QThread::create(
      [this, source, destinationPath, options, outcome] {
        *outcome = deviceBackend_->capture(
            source, destinationPath, options,
            [this](const core::MediaCaptureProgress& progress) {
              static_cast<void>(QMetaObject::invokeMethod(
                  this, [this, progress] { handleCaptureProgress(progress); },
                  Qt::QueuedConnection));
            },
            [this] { return cancelRequested_.load(); });
      });
  captureThread_ = thread;
  connect(thread, &QThread::finished, this,
          [this, thread, outcome, destination] {
            if (captureThread_ == thread) {
              captureThread_ = nullptr;
            }
            thread->deleteLater();
            endOperationGuard();
            closeButton_->setText("CLOSE");
            closeButton_->setEnabled(true);
            if (outcome->success) {
              progressBar_->setValue(100);
              progressBar_->setFormat("CAPTURE COMPLETE");
              statusBar()->showMessage("Device capture completed successfully");
              appendLog("Verified device capture saved to " + destination + '.');
              QMessageBox::information(
                  this, "Capture complete",
                  "The device image was captured and verified successfully.");
            } else {
              progressBar_->setFormat(outcome->cancelled ? "CANCELLED"
                                                         : "CAPTURE FAILED");
              statusBar()->showMessage(QString::fromStdString(outcome->error));
              appendLog(QString(outcome->cancelled ? "Capture cancelled: "
                                                   : "Capture failed: ") +
                        QString::fromStdString(outcome->error));
              if (!outcome->cancelled) {
                QMessageBox::critical(this, "Capture failed",
                                      QString::fromStdString(outcome->error));
              }
            }
            refreshVolumes();
            updateWriteReadiness();
            if (closeWhenFinished_) {
              closeWhenFinished_ = false;
              close();
            }
          });
  thread->start();
}

void MainWindow::formatDevice() {
  if (writeThread_ != nullptr || captureThread_ != nullptr ||
      imageAnalysisThread_ != nullptr || checksumThread_ != nullptr ||
      !deviceBackend_) {
    return;
  }
  const QVariant selectedDevice = volumeBox_->currentData(Qt::UserRole + 2);
  if (!selectedDevice.isValid()) {
    QMessageBox::warning(this, "No target device",
                         "Select an eligible removable device first.");
    return;
  }
  const auto deviceIndex =
      static_cast<std::size_t>(selectedDevice.toULongLong());
  if (deviceIndex >= visibleDevices_.size()) {
    QMessageBox::warning(this, "Stale target",
                         "Refresh and select the target device again.");
    return;
  }
  const core::BlockDeviceInfo target = visibleDevices_[deviceIndex];
  auto availability = deviceBackend_->rawWriteAvailability(target);
  if (!availability.available && availability.authorizationCanBeRequested) {
    const auto authorization = deviceBackend_->requestRawWriteAuthorization();
    static_cast<void>(updatePrivilegeIndicator());
    appendLog("Administrative disk access: " +
              QString::fromStdString(authorization.reason));
    if (!authorization.available) {
      QMessageBox::information(this, "Administrative access required",
                               QString::fromStdString(authorization.reason));
      updateWriteReadiness();
      return;
    }
    availability = deviceBackend_->rawWriteAvailability(target);
  }
  if (!availability.available) {
    QMessageBox::warning(this, "Formatting unavailable",
                         QString::fromStdString(availability.reason));
    return;
  }

  bool accepted = false;
  QStringList filesystemChoices{
      "FAT32 (non bootable)", "FreeDOS (FAT32)",
      "MS-DOS 7/8 (user-supplied FAT32 system files)",
      "GRUB2 BIOS prompt (FAT32)",
      "Grub4DOS (user-supplied GRLDR, FAT32)",
      "ReactOS (user-supplied FREELDR.SYS, FAT32)",
      "Syslinux 4 BIOS prompt (FAT32)",
      "UEFI Shell 2.2 (built-in, FAT32)",
      "UEFI Shell 2.2 (built-in multi-architecture, FAT32)",
      "UEFI application (user-supplied, FAT32)",
      "ext2 (portable Linux filesystem)"};
  if (target.logicalSectorSize == 512U &&
      target.capacityBytes >= 16ULL * 1024ULL * 1024ULL &&
      target.capacityBytes <= 2ULL * 1024ULL * 1024ULL * 1024ULL) {
    filesystemChoices.insert(0, "FAT16 (non bootable)");
  }
  constexpr std::array providerFilesystems{
      backend::StandaloneFilesystem::Ntfs,
      backend::StandaloneFilesystem::UefiNtfs,
      backend::StandaloneFilesystem::ExFat,
      backend::StandaloneFilesystem::Udf,
      backend::StandaloneFilesystem::ReFs,
      backend::StandaloneFilesystem::Ext3};
  if (filesystemStager_) {
    for (const auto candidate : providerFilesystems) {
      if (filesystemStager_->availability(candidate, target).available) {
        filesystemChoices.push_back(
            QString::fromUtf8(backend::standaloneFilesystemName(candidate)) +
            " (installed host provider)");
      }
    }
  }
  const QString filesystem = QInputDialog::getItem(
      this, "Standalone formatting", "Media type:",
      filesystemChoices, 0, false, &accepted);
  if (!accepted) {
    return;
  }
  const bool freeDos = filesystem.startsWith("FreeDOS");
  const bool fat16 = filesystem.startsWith("FAT16");
  const bool msDos = filesystem.startsWith("MS-DOS");
  const bool grub2 = filesystem.startsWith("GRUB2");
  const bool grub4Dos = filesystem.startsWith("Grub4DOS");
  const bool reactOs = filesystem.startsWith("ReactOS");
  const bool syslinux = filesystem.startsWith("Syslinux");
  const bool bundledUefiShell =
      filesystem == "UEFI Shell 2.2 (built-in, FAT32)";
  const bool multiArchitectureUefiShell =
      filesystem ==
      "UEFI Shell 2.2 (built-in multi-architecture, FAT32)";
  const bool uefiApplication = filesystem.startsWith("UEFI application");
  const bool uefiBootMedia = bundledUefiShell ||
                             multiArchitectureUefiShell || uefiApplication;
  const bool ext2 = filesystem.startsWith("ext2");
  std::optional<backend::StandaloneFilesystem> providerFilesystem;
  for (const auto candidate : providerFilesystems) {
    if (filesystem.startsWith(
            QString::fromUtf8(backend::standaloneFilesystemName(candidate)))) {
      providerFilesystem = candidate;
      break;
    }
  }
  std::vector<core::StandaloneMediaFile> mediaFiles;
  QString msDosSourceDirectory;
  QString uefiShellDescription;
  if (msDos) {
    msDosSourceDirectory = QFileDialog::getExistingDirectory(
        this, "Select the MS-DOS 7/8 system-file directory");
    if (msDosSourceDirectory.isEmpty()) {
      return;
    }
    const QDir sourceDirectory(msDosSourceDirectory);
    const QFileInfoList entries = sourceDirectory.entryInfoList(
        QDir::Files | QDir::Readable | QDir::NoDotAndDotDot);
    constexpr std::array<const char*, 3> requiredNames{
        "IO.SYS", "MSDOS.SYS", "COMMAND.COM"};
    std::uint64_t totalBytes = 0U;
    for (const char* requiredName : requiredNames) {
      const auto entry = std::find_if(
          entries.begin(), entries.end(), [requiredName](const QFileInfo& info) {
            return info.fileName().compare(QString::fromLatin1(requiredName),
                                           Qt::CaseInsensitive) == 0;
          });
      if (entry == entries.end() || !entry->isFile() || entry->size() <= 0 ||
          entry->size() > 16 * 1024 * 1024) {
        QMessageBox::warning(
            this, "MS-DOS system files unavailable",
            QString("The selected directory must contain a readable %1 file no larger than 16 MiB.")
                .arg(QString::fromLatin1(requiredName)));
        return;
      }
      QFile file(entry->absoluteFilePath());
      if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "MS-DOS system files unavailable",
                             "Unable to read " + entry->absoluteFilePath());
        return;
      }
      const QByteArray bytes = file.readAll();
      totalBytes += static_cast<std::uint64_t>(bytes.size());
      if (totalBytes > 32U * 1024U * 1024U) {
        QMessageBox::warning(this, "MS-DOS system files unavailable",
                             "The selected MS-DOS system files exceed the 32 MiB safety limit.");
        return;
      }
      mediaFiles.push_back(
          {requiredName,
           std::vector<unsigned char>(bytes.begin(), bytes.end())});
    }
  } else if (grub4Dos) {
    const QString loaderPath = QFileDialog::getOpenFileName(
        this, "Select the Grub4DOS GRLDR loader", QString(),
        "Grub4DOS loader (grldr GRLDR);;All files (*)");
    const QFileInfo loaderInfo(loaderPath);
    if (loaderPath.isEmpty()) {
      return;
    }
    if (!loaderInfo.isFile() || loaderInfo.size() <= 0 ||
        loaderInfo.size() > 64 * 1024 * 1024) {
      QMessageBox::warning(
          this, "Grub4DOS loader unavailable",
          "Select a non-empty regular GRLDR file no larger than 64 MiB.");
      return;
    }
    QFile loader(loaderPath);
    if (!loader.open(QIODevice::ReadOnly)) {
      QMessageBox::warning(this, "Grub4DOS loader unavailable",
                           "Unable to read " + loaderPath);
      return;
    }
    const QByteArray bytes = loader.readAll();
    mediaFiles.push_back(
        {"GRLDR", std::vector<unsigned char>(bytes.begin(), bytes.end())});
  } else if (reactOs) {
    const QString loaderPath = QFileDialog::getOpenFileName(
        this, "Select the ReactOS FREELDR.SYS loader", QString(),
        "ReactOS loader (freeldr.sys FREELDR.SYS);;All files (*)");
    const QFileInfo loaderInfo(loaderPath);
    if (loaderPath.isEmpty()) {
      return;
    }
    if (!loaderInfo.isFile() || loaderInfo.size() <= 0 ||
        loaderInfo.size() > 64 * 1024 * 1024) {
      QMessageBox::warning(
          this, "ReactOS loader unavailable",
          "Select a non-empty regular FREELDR.SYS file no larger than 64 MiB.");
      return;
    }
    QFile loader(loaderPath);
    if (!loader.open(QIODevice::ReadOnly)) {
      QMessageBox::warning(this, "ReactOS loader unavailable",
                           "Unable to read " + loaderPath);
      return;
    }
    const QByteArray loaderBytes = loader.readAll();
    mediaFiles.push_back(
        {"FREELDR.SYS",
         std::vector<unsigned char>(loaderBytes.begin(), loaderBytes.end())});
    const QString configurationPath =
        loaderInfo.dir().filePath("freeldr.ini");
    QFile configuration(configurationPath);
    if (configuration.exists() && configuration.open(QIODevice::ReadOnly) &&
        configuration.size() <= 1024 * 1024) {
      const QByteArray configurationBytes = configuration.readAll();
      mediaFiles.push_back(
          {"FREELDR.INI",
           std::vector<unsigned char>(configurationBytes.begin(),
                                      configurationBytes.end())});
    }
  } else if (bundledUefiShell || multiArchitectureUefiShell) {
    const auto& assets = bundledUefiShellAssets();
    std::vector<const BundledUefiShellAsset*> selectedAssets;
    if (multiArchitectureUefiShell) {
      selectedAssets.reserve(assets.size());
      for (const auto& asset : assets) {
        selectedAssets.push_back(&asset);
      }
      uefiShellDescription =
          "UEFI Shell 2.2 for x86-64, x86-32, ARM64, ARM32, RISC-V 64 and LoongArch64";
    } else {
      QStringList choices;
      choices.reserve(static_cast<qsizetype>(assets.size()));
      for (const auto& asset : assets) {
        choices.push_back(asset.displayLabel());
      }
      const QString choice = QInputDialog::getItem(
          this, "Built-in UEFI Shell architecture", "Target firmware:",
          choices, 0, false, &accepted);
      if (!accepted) {
        return;
      }
      const qsizetype selectedIndex = choices.indexOf(choice);
      if (selectedIndex < 0 ||
          selectedIndex >= static_cast<qsizetype>(assets.size())) {
        QMessageBox::critical(this, "UEFI Shell unavailable",
                              "The selected UEFI Shell architecture is invalid.");
        return;
      }
      selectedAssets.push_back(
          &assets.at(static_cast<std::size_t>(selectedIndex)));
      uefiShellDescription = selectedAssets.front()->releaseLabel + " for " +
                             selectedAssets.front()->architectureLabel;
    }
    for (const BundledUefiShellAsset* asset : selectedAssets) {
      QByteArray bytes;
      QString error;
      if (!loadBundledUefiShellAsset(*asset, bytes, error)) {
        QMessageBox::critical(this, "UEFI Shell unavailable", error);
        return;
      }
      mediaFiles.push_back(
          {("EFI/BOOT/" + asset->fallbackFileName).toStdString(),
           std::vector<unsigned char>(bytes.begin(), bytes.end())});
    }
  } else if (uefiApplication) {
    const QString applicationPath = QFileDialog::getOpenFileName(
        this, "Select a UEFI application", QString(),
        "UEFI applications (*.efi);;All files (*)");
    if (applicationPath.isEmpty()) {
      return;
    }
    const QFileInfo applicationInfo(applicationPath);
    if (!applicationInfo.isFile() || applicationInfo.size() <= 0 ||
        applicationInfo.size() > 64 * 1024 * 1024) {
      QMessageBox::warning(
          this, "UEFI application unavailable",
          "Select a non-empty regular EFI application no larger than 64 MiB.");
      return;
    }
    const QString architecture = QInputDialog::getItem(
        this, "UEFI fallback architecture", "Architecture:",
        {"x86-64", "x86-32", "ARM64", "ARM32", "LoongArch64",
         "RISC-V 64"},
        0, false, &accepted);
    if (!accepted) {
      return;
    }
    const QMap<QString, QString> fallbackNames{
        {"x86-64", "BOOTX64.EFI"}, {"x86-32", "BOOTIA32.EFI"},
        {"ARM64", "BOOTAA64.EFI"}, {"ARM32", "BOOTARM.EFI"},
        {"LoongArch64", "BOOTLOONGARCH64.EFI"},
        {"RISC-V 64", "BOOTRISCV64.EFI"}};
    QFile application(applicationPath);
    if (!application.open(QIODevice::ReadOnly)) {
      QMessageBox::warning(this, "UEFI application unavailable",
                           "Unable to read " + applicationPath);
      return;
    }
    const QByteArray bytes = application.readAll();
    mediaFiles.push_back(
        {("EFI/BOOT/" + fallbackNames.value(architecture)).toStdString(),
         std::vector<unsigned char>(bytes.begin(), bytes.end())});
  }
  QString label = QInputDialog::getText(
      this, "Standalone formatting", "Volume label:", QLineEdit::Normal,
      volumeLabel_->text().isEmpty()
          ? (freeDos ? "FREEDOS"
                     : fat16 ? "PORTABLE"
                     : msDos ? "MSDOS"
                     : grub2 ? "GRUB2"
                     : grub4Dos ? "GRUB4DOS"
                     : reactOs ? "REACTOS"
                     : syslinux ? "SYSLINUX"
                     : bundledUefiShell || multiArchitectureUefiShell
                         ? "UEFI_SHELL"
                     : uefiApplication ? "UEFI"
                     : ext2 ? "PORTABLE_EXT2"
                                       : "NO_LABEL")
          : volumeLabel_->text(),
      &accepted);
  if (!accepted) {
    return;
  }
  label = label.trimmed();
  if (label.isEmpty()) {
    label = "NO_LABEL";
  }
  const bool extendedLinuxLabel =
      ext2 || (providerFilesystem.has_value() &&
               *providerFilesystem == backend::StandaloneFilesystem::Ext3);
  const bool longProviderLabel =
      providerFilesystem == backend::StandaloneFilesystem::Ntfs ||
      providerFilesystem == backend::StandaloneFilesystem::UefiNtfs ||
      providerFilesystem == backend::StandaloneFilesystem::Udf ||
      providerFilesystem == backend::StandaloneFilesystem::ReFs;
  const qsizetype maximumLabelBytes =
      extendedLinuxLabel ? 16 : longProviderLabel ? 32 : 11;
  if (label.toUtf8().size() > maximumLabelBytes) {
    QMessageBox::warning(this, "Volume label too long",
                         extendedLinuxLabel
                             ? "ext volume labels are limited to 16 UTF-8 bytes."
                         : longProviderLabel
                             ? "This filesystem's volume label is limited to 32 UTF-8 bytes."
                             : "FAT and exFAT volume labels are limited to 11 UTF-8 bytes.");
    return;
  }
  const core::StandaloneFormatOptions formatOptions{
      clusterSizeBox_->currentData().toUInt(), quickFormat_->isChecked()};
  const core::VerificationProfile verificationProfile =
      selectedVerificationProfile();
  if ((fat16 || freeDos || msDos || grub2 || grub4Dos || reactOs || syslinux ||
       uefiBootMedia || (!providerFilesystem.has_value() && !ext2)) &&
      formatOptions.clusterSizeBytes > 32768U) {
    QMessageBox::warning(this, "Cluster size unavailable",
                         "FAT allocation units are limited to 32 KiB.");
    return;
  }
  if (ext2 && formatOptions.clusterSizeBytes != 0U) {
    QMessageBox::warning(
        this, "Cluster size unavailable",
        "The portable ext2 formatter currently requires Automatic cluster size.");
    return;
  }
  if (providerFilesystem == backend::StandaloneFilesystem::Udf &&
      formatOptions.clusterSizeBytes != 0U) {
    QMessageBox::warning(
        this, "Cluster size unavailable",
        "UDF uses the target logical block size; select Automatic cluster size.");
    return;
  }
  if (providerFilesystem == backend::StandaloneFilesystem::Ext3 &&
      formatOptions.clusterSizeBytes != 0U &&
      formatOptions.clusterSizeBytes != 1024U &&
      formatOptions.clusterSizeBytes != 2048U &&
      formatOptions.clusterSizeBytes != 4096U) {
    QMessageBox::warning(this, "Cluster size unavailable",
                         "ext3 block size must be 1, 2, or 4 KiB.");
    return;
  }
  core::DeploymentPreflightInput preflightInput;
  preflightInput.operation = "Standalone " + filesystem.toStdString() +
                             " format";
  preflightInput.image.displayName = "Blank " + filesystem.toStdString() +
                                     " media";
  preflightInput.image.format = core::ImageFormat::Raw;
  preflightInput.image.sizeBytes = target.capacityBytes;
  preflightInput.image.expandedSizeBytes = target.capacityBytes;
  preflightInput.target = target;
  preflightInput.partitionScheme = "MBR";
  preflightInput.targetSystem =
      uefiBootMedia ? "UEFI"
      : freeDos || msDos || grub2 || grub4Dos || reactOs || syslinux
          ? "BIOS"
          : "Non-bootable";
  preflightInput.fileSystem = filesystem.toStdString();
  preflightInput.clusterSizeBytes = formatOptions.clusterSizeBytes;
  preflightInput.quickFormat = formatOptions.quickFormat;
  preflightInput.verificationProfile = verificationProfile;
  preflightInput.bytesToWrite = target.capacityBytes;
  const auto writeAvailability = deviceBackend_->rawWriteAvailability(target);
  preflightInput.dependencies.push_back(
      {"Guarded raw-device writer", true, writeAvailability.available,
       writeAvailability.reason});
  if (providerFilesystem.has_value() && filesystemStager_) {
    const auto provider =
        filesystemStager_->availability(*providerFilesystem, target);
    preflightInput.dependencies.push_back(
        {filesystem.toStdString() + " formatter", true, provider.available,
         provider.reason});
  }
  preflightInput.transformations.push_back(
      "Create volume label " + label.toStdString());
  if (msDos) {
    preflightInput.transformations.push_back(
        "Install user-supplied MS-DOS system files from " +
        msDosSourceDirectory.toStdString());
  }
  if (bundledUefiShell || multiArchitectureUefiShell) {
    preflightInput.transformations.push_back(
        "Install the integrity-checked bundled " +
        uefiShellDescription.toStdString());
    preflightInput.warnings.push_back(
        "The bundled UEFI Shell is not signed for Microsoft Secure Boot; Secure Boot must be disabled on the target system");
  }
  if (badBlocks_->isChecked()) {
    preflightInput.transformations.push_back(
        "Run " + std::to_string(badBlockPassCount_->currentIndex() + 1) +
        " destructive bad-block/fake-capacity pass(es)");
  }
  core::DeploymentPreflightReport preflight =
      core::buildDeploymentPreflight(preflightInput);
  if (!preflight.ready()) {
    showTextReport(this, "Format blocked",
                   QString::fromStdString(preflight.toText()));
    return;
  }
  QMessageBox confirmation(this);
  confirmation.setIcon(QMessageBox::Warning);
  confirmation.setWindowTitle("Confirm standalone format");
  confirmation.setText("This operation will permanently erase " +
                       QString::fromStdString(target.displayName) + ".");
  QString confirmationInformation =
      "Expand Show Details to review the exact format, dependencies and verification policy.\n\nAll existing data will be lost.";
  if (bundledUefiShell || multiArchitectureUefiShell) {
    confirmationInformation +=
        "\n\nSecure Boot must be disabled to start the bundled UEFI Shell.";
  }
  confirmation.setInformativeText(confirmationInformation);
  confirmation.setDetailedText(QString::fromStdString(preflight.toText()));
  confirmation.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
  confirmation.setDefaultButton(QMessageBox::Cancel);
  if (confirmation.exec() != QMessageBox::Yes) {
    return;
  }
  activePreflight_ = std::move(preflight);
  operationStartedAtUtc_ =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();

  const std::filesystem::path stagingDirectory = fileSystemPath(
      QDir(QDir::tempPath()).filePath(
          "rufus-plus-plus-format-stage-" +
          QUuid::createUuid().toString(QUuid::WithoutBraces)));
  std::error_code stagingError;
  if (!std::filesystem::create_directory(stagingDirectory, stagingError)) {
    QMessageBox::critical(
        this, "Staging unavailable",
        "Unable to create the format staging directory: " +
            QString::fromStdString(stagingError.message()));
    activePreflight_.reset();
    operationStartedAtUtc_.clear();
    return;
  }
#if !defined(_WIN32)
  std::filesystem::permissions(stagingDirectory,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace,
                               stagingError);
  if (stagingError) {
    std::error_code ignored;
    std::filesystem::remove(stagingDirectory, ignored);
    QMessageBox::critical(this, "Staging unavailable",
                          "Unable to secure the format staging directory.");
    activePreflight_.reset();
    operationStartedAtUtc_.clear();
    return;
  }
#endif
  QString providerStagingName;
  if (providerFilesystem.has_value()) {
    providerStagingName =
        QString::fromUtf8(
            backend::standaloneFilesystemName(*providerFilesystem))
            .toLower();
    providerStagingName.replace(':', '-');
    providerStagingName += "-format.img";
  }
  const auto stagingPath =
      stagingDirectory /
      (freeDos ? "freedos-fat32.img"
               : fat16 ? "blank-fat16.img"
               : msDos ? "msdos-fat32.img"
               : grub2 ? "grub2-fat32.img"
               : grub4Dos ? "grub4dos-fat32.img"
               : reactOs ? "reactos-fat32.img"
               : syslinux ? "syslinux-fat32.img"
               : bundledUefiShell || multiArchitectureUefiShell
                   ? "uefi-shell-fat32.img"
               : uefiApplication ? "uefi-fat32.img"
               : ext2 ? "blank-ext2.img"
               : providerFilesystem.has_value()
                   ? providerStagingName.toStdString()
                                 : "blank-fat32.img");
  if (freeDos) {
    constexpr std::array<const char*, 9> resourceNames{
        "COMMAND.COM", "KERNEL.SYS", "DISPLAY.EXE", "KEYB.EXE",
        "KEYBOARD.SYS", "KEYBRD2.SYS", "KEYBRD3.SYS", "KEYBRD4.SYS",
        "MODE.COM"};
    for (const char* name : resourceNames) {
      QFile resource(":/rufus/freedos/" + QString::fromLatin1(name));
      if (!resource.open(QIODevice::ReadOnly)) {
        std::error_code ignored;
        std::filesystem::remove_all(stagingDirectory, ignored);
        QMessageBox::critical(
            this, "FreeDOS resources unavailable",
            "A required bundled FreeDOS file could not be opened: " +
                QString::fromLatin1(name));
        return;
      }
      const QByteArray bytes = resource.readAll();
      mediaFiles.push_back(
          {name, std::vector<unsigned char>(bytes.begin(), bytes.end())});
    }
    constexpr std::string_view config =
        "DOS=HIGH\r\nLASTDRIVE=Z\r\nSHELLHIGH=C:\\COMMAND.COM C:\\ /E:1024 /P\r\n";
    constexpr std::string_view autoexec =
        "@ECHO OFF\r\nSET DOSDIR=C:\\\r\nSET PATH=C:\\\r\n"
        "ECHO FreeDOS media created by Rufus++\r\nVER\r\n";
    mediaFiles.push_back(
        {"FDCONFIG.SYS", std::vector<unsigned char>(config.begin(), config.end())});
    mediaFiles.push_back(
        {"AUTOEXEC.BAT", std::vector<unsigned char>(autoexec.begin(), autoexec.end())});
  }
  auto outcome = std::make_shared<core::RawWriteResult>();
  const bool runBadBlockTest = badBlocks_->isChecked();
  const unsigned int badBlockPasses = static_cast<unsigned int>(
      badBlockPassCount_->currentIndex() + 1);
  cancelRequested_.store(false);
  closeWhenFinished_ = false;
  standaloneFormatRunning_ = true;
  freeDosFormatRunning_ = freeDos;
  msDosFormatRunning_ = msDos;
  ext2FormatRunning_ = ext2;
  standaloneFilesystemLabel_ =
      fat16 ? "FAT16"
      : providerFilesystem.has_value()
          ? backend::standaloneFilesystemName(*providerFilesystem)
          : std::string{};
  const core::StandaloneBootMode bootMode =
      freeDos ? core::StandaloneBootMode::FreeDos
      : msDos ? core::StandaloneBootMode::MsDos
      : grub2 ? core::StandaloneBootMode::Grub2
      : grub4Dos ? core::StandaloneBootMode::Grub4Dos
      : reactOs ? core::StandaloneBootMode::ReactOs
      : syslinux ? core::StandaloneBootMode::Syslinux
      : uefiBootMedia ? core::StandaloneBootMode::Uefi
                        : core::StandaloneBootMode::None;
  volumeBox_->setEnabled(false);
  selectButton_->setEnabled(false);
  checksumButton_->setEnabled(false);
  captureButton_->setEnabled(false);
  formatButton_->setEnabled(false);
  imageOptionBox_->setEnabled(false);
  partitionSchemeBox_->setEnabled(false);
  targetSystemBox_->setEnabled(false);
  fileSystemBox_->setEnabled(false);
  clusterSizeBox_->setEnabled(false);
  quickFormat_->setEnabled(false);
  verificationProfileBox_->setEnabled(false);
  badBlocks_->setEnabled(false);
  badBlockPassCount_->setEnabled(false);
  startButton_->setEnabled(false);
  closeButton_->setText("CANCEL");
  progressBar_->setRange(0, 100);
  progressBar_->setValue(0);
  progressBar_->setFormat("FORMATTING — %p%");
  statusBar()->showMessage("Preparing standalone filesystem format");
  appendLog("Confirmed standalone filesystem format of " +
            QString::fromStdString(target.devicePath) + '.');

  QThread* const thread = QThread::create(
      [this, target, label, stagingDirectory, stagingPath, outcome,
       runBadBlockTest, badBlockPasses, bootMode, fat16, ext2,
       providerFilesystem, formatOptions, verificationProfile,
       mediaFiles = std::move(mediaFiles)]() mutable {
        const auto cleanup = [&] {
          std::error_code ignored;
          std::filesystem::remove_all(stagingDirectory, ignored);
        };
        if (runBadBlockTest) {
          const auto test = deviceBackend_->testBadBlocks(
              target, {badBlockPasses, 4U * 1024U * 1024U, 256U},
              [this](const core::BadBlockTestProgress& progress) {
                static_cast<void>(QMetaObject::invokeMethod(
                    this,
                    [this, progress] { handleBadBlockProgress(progress); },
                    Qt::QueuedConnection));
              },
              [this] { return cancelRequested_.load(); });
          if (!test.success) {
            outcome->cancelled = test.cancelled;
            outcome->destructiveWriteStarted = test.destructiveWriteStarted;
            outcome->bytesWritten = test.bytesTested;
            outcome->error = test.error;
            cleanup();
            return;
          }
        }
        const auto stageProgress =
            [this](const core::StandaloneMediaProgress& progress) {
              static_cast<void>(QMetaObject::invokeMethod(
                  this,
                  [this, progress] { handleStandaloneMediaProgress(progress); },
                  Qt::QueuedConnection));
            };
        const auto cancelled = [this] { return cancelRequested_.load(); };
        core::StandaloneMediaResult staged;
        if (fat16) {
          staged = core::stageBlankFat16Media(
              target, stagingPath, label.toStdString(), stageProgress,
              cancelled, formatOptions);
        } else if (ext2) {
          staged = core::stageBlankExt2Media(
              target, stagingPath, label.toStdString(), stageProgress,
              cancelled, formatOptions);
        } else if (providerFilesystem.has_value() && filesystemStager_) {
          staged = filesystemStager_->stage(
              *providerFilesystem, target, stagingPath, label.toStdString(),
              formatOptions, stageProgress, cancelled);
        } else {
          staged = core::stageFat32Media(
              target, stagingPath, label.toStdString(), bootMode,
              std::move(mediaFiles), stageProgress, cancelled, formatOptions);
        }
        if (!staged.success || !staged.stagedImage.has_value()) {
          outcome->cancelled = staged.cancelled;
          outcome->error = staged.error.empty()
                               ? "Standalone FAT32 staging failed"
                               : staged.error;
          cleanup();
          return;
        }
        const core::WritePlanBuilder builder;
        const bool clearTailMetadata =
            staged.stagedImage->sizeBytes < target.capacityBytes;
        auto planning = builder.buildRawWriteWithVerification(
            *staged.stagedImage, target, verificationProfile,
            4U * 1024U * 1024U, clearTailMetadata);
        if (!planning.succeeded()) {
          outcome->error = planning.issues.empty()
                               ? "Standalone format write planning failed"
                               : planning.issues.front().message;
          cleanup();
          return;
        }
        *outcome = deviceBackend_->writeRaw(
            *planning.plan,
            [this](const core::RawWriteProgress& progress) {
              static_cast<void>(QMetaObject::invokeMethod(
                  this, [this, progress] { handleWriteProgress(progress); },
                  Qt::QueuedConnection));
            },
            [this] { return cancelRequested_.load(); });
        cleanup();
      });
  writeThread_ = thread;
  connect(thread, &QThread::finished, this, [this, thread, outcome] {
    if (writeThread_ == thread) {
      writeThread_ = nullptr;
    }
    thread->deleteLater();
    handleWriteFinished(*outcome);
  });
  beginOperationGuard();
  thread->start();
}

void MainWindow::chooseImage() {
  if (imageAnalysisThread_ != nullptr || checksumThread_ != nullptr ||
      writeThread_ != nullptr || captureThread_ != nullptr ||
      analysisToolThread_ != nullptr) {
    return;
  }
  const QString path = QFileDialog::getOpenFileName(
      this,
      "Select boot image or macOS installer",
      QString(),
#if defined(Q_OS_MACOS)
      "Deployable images and macOS installers (*.app *.iso *.udf *.img *.raw *.dd *.vhd *.vhdx *.ffu *.gz *.gzip *.bz2 *.bzip2 "
#else
      "Deployable images (*.iso *.udf *.img *.raw *.dd *.vhd *.vhdx *.ffu *.gz *.gzip *.bz2 *.bzip2 "
#endif
      "*.zip *.lzma *.xz "
      "*.zst *.zstd);;All files (*)");
  if (path.isEmpty()) {
    return;
  }

  // Once a new path has been accepted, never leave the previously analyzed
  // image armed while the replacement is being checked (or if that check
  // fails). The visible filename and the executable plan must always refer to
  // the same source.
  operationProgressLabel_->hide();
  selectedImage_.reset();
  selectedMacOsInstaller_.reset();
  latestImageSha256_.clear();
  latestChecksumImagePath_.clear();
  checksumButton_->setEnabled(false);
  captureButton_->setEnabled(false);
  formatButton_->setEnabled(false);
  windowsToGoOptions_ = {};
  windowsInstallationOptions_ = {};
  windowsToGoOptionsConfigured_ = false;
  windowsInstallationOptionsConfigured_ = false;
  bootSelectionBox_->clear();
  bootSelectionBox_->addItem(QFileInfo(path).fileName(), path);
  bootSelectionBox_->setToolTip(path);
  volumeLabel_->clear();
  partitionSchemeBox_->setCurrentText("Auto (recommended)");
  targetSystemBox_->setCurrentText("Auto (recommended)");
  fileSystemBox_->setCurrentText("Image-defined");
  applyImageProfile();

  analysisCancelRequested_.store(false);
  selectButton_->setEnabled(false);
  imageOptionBox_->setEnabled(false);
  windowsToGoOptionsButton_->setEnabled(false);
  startButton_->setEnabled(false);
  closeButton_->setText("CANCEL");
  progressBar_->setRange(0, 0);
  progressBar_->setFormat("ANALYZING");
  statusBar()->showMessage("Analyzing and validating " + QFileInfo(path).fileName());
  appendLog("Analyzing " + path + '.');

#if defined(Q_OS_MACOS)
  const bool macOsInstallerApplication =
      QFileInfo(path).isDir() &&
      QFileInfo(path).suffix().compare("app", Qt::CaseInsensitive) == 0;
  if (macOsInstallerApplication) {
    auto result =
        std::make_shared<core::MacOsInstallerAnalysisResult>();
    const std::filesystem::path sourcePath = fileSystemPath(path);
    QThread* const thread = QThread::create(
        [this, result, sourcePath] {
          *result =
              deviceBackend_->analyzeMacOsInstallerApplication(sourcePath);
        });
    imageAnalysisThread_ = thread;
    connect(thread, &QThread::finished, this,
            [this, thread, result, path] {
              if (imageAnalysisThread_ == thread) {
                imageAnalysisThread_ = nullptr;
              }
              thread->deleteLater();
              progressBar_->setRange(0, 100);
              selectButton_->setEnabled(true);
              windowsToGoOptionsButton_->setEnabled(true);
              closeButton_->setText("CLOSE");
              closeButton_->setEnabled(true);
              handleMacOsInstallerAnalysisFinished(*result, path);
              if (closeWhenFinished_) {
                closeWhenFinished_ = false;
                close();
              }
            });
    thread->start();
    return;
  }
#endif

  auto result = std::make_shared<core::ImageAnalysisResult>();
  const std::filesystem::path sourcePath = fileSystemPath(path);
  const std::string filenameHint = QFileInfo(path).fileName().toStdString();
  QThread* const thread = QThread::create(
      [this, result, sourcePath, filenameHint] {
        *result = imageAnalyzer_->analyze(
            sourcePath, filenameHint,
            [this] { return analysisCancelRequested_.load(); });
      });
  imageAnalysisThread_ = thread;
  connect(thread, &QThread::finished, this, [this, thread, result, path] {
    if (imageAnalysisThread_ == thread) {
      imageAnalysisThread_ = nullptr;
    }
    thread->deleteLater();
    progressBar_->setRange(0, 100);
    selectButton_->setEnabled(true);
    windowsToGoOptionsButton_->setEnabled(true);
    closeButton_->setText("CLOSE");
    closeButton_->setEnabled(true);
    handleImageAnalysisFinished(*result, path);
    if (closeWhenFinished_) {
      closeWhenFinished_ = false;
      close();
    }
  });
  thread->start();
}

void MainWindow::handleImageAnalysisFinished(
    const core::ImageAnalysisResult& analysis, const QString& path) {
  if (!analysis.succeeded()) {
    progressBar_->setValue(0);
    progressBar_->setFormat(analysis.cancelled ? "CANCELLED" : "IMAGE ERROR");
    statusBar()->showMessage(QString::fromStdString(analysis.error));
    appendLog(QString(analysis.cancelled ? "Image analysis cancelled: "
                                         : "Image analysis failed: ") +
              QString::fromStdString(analysis.error));
    updateWriteReadiness();
    return;
  }

  const core::ImageInfo& image = *analysis.image;
  selectedMacOsInstaller_.reset();
  selectedImage_ = image;
  checksumButton_->setEnabled(true);
  windowsToGoOptions_ = {};
  windowsInstallationOptions_ = {};
  windowsToGoOptionsConfigured_ = false;
  windowsInstallationOptionsConfigured_ = false;
  if (!image.windowsEditions.empty()) {
    windowsToGoOptions_.editionIndex = image.windowsEditions.front().index;
  }
  bootSelectionBox_->clear();
  bootSelectionBox_->addItem(QString::fromStdString(image.displayName), path);
  bootSelectionBox_->setToolTip(path);
  volumeLabel_->setText(QString::fromStdString(image.volumeLabel));

  const auto scheme = core::partitionSchemeName(image.partitionScheme);
  partitionSchemeBox_->setCurrentText(
      image.partitionScheme == core::PartitionScheme::Unknown ? "Auto (recommended)"
                                                               : fromView(scheme));
  if (image.capabilities.uefiBootable && !image.capabilities.biosBootable) {
    targetSystemBox_->setCurrentText("UEFI (non CSM)");
  } else if (image.capabilities.biosBootable && image.capabilities.uefiBootable) {
    targetSystemBox_->setCurrentText("BIOS + UEFI");
  } else if (image.capabilities.biosBootable) {
    targetSystemBox_->setCurrentText("BIOS (or UEFI-CSM)");
  } else {
    targetSystemBox_->setCurrentText("Auto (recommended)");
  }
  fileSystemBox_->setCurrentText(image.capabilities.requiresNtfs ? "NTFS" : "FAT32");
  applyImageProfile();

  const QString format = fromView(core::imageFormatName(image.format));
  progressBar_->setFormat("ANALYZED — READY");
  statusBar()->showMessage(format + " analyzed — checking operation availability");
  appendLog("Analyzed " + QString::fromStdString(image.displayName) + " as " + format + " (" +
            fromView(core::formatByteSize(image.sizeBytes)) + ")");
  appendLog("Image family: " + fromView(core::imageFamilyName(image.family)) + '.');
  if (image.capabilities.linuxPersistence) {
    appendLog("Linux persistence: " +
              QString::fromUtf8(core::linuxPersistenceStyleName(
                  image.capabilities.linuxPersistenceStyle)) +
              " boot entries validated.");
  }
  if (image.architecture != core::ImageArchitecture::Unknown) {
    appendLog("Image architecture: " + fromView(core::imageArchitectureName(image.architecture)) +
              '.');
  }
  QStringList opticalFormats;
  if (image.capabilities.iso9660) {
    opticalFormats.append("ISO-9660");
  }
  if (image.capabilities.joliet) {
    opticalFormats.append("Joliet");
  }
  if (image.capabilities.udf) {
    opticalFormats.append("UDF");
  }
  if (!opticalFormats.empty()) {
    appendLog("Optical filesystems: " + opticalFormats.join(", ") + '.');
  }
  if (image.capabilities.windowsImageMetadata) {
    appendLog("Windows images: " + QString::number(image.windowsImageCount) +
              "; boot index: " + QString::number(image.windowsBootIndex) + '.');
    appendLog("Windows version: " + QString::number(image.windowsVersionMajor) + '.' +
              QString::number(image.windowsVersionMinor) + " (build " +
              QString::number(image.windowsBuild) + ").");
  }
  if (image.capabilities.compressedSizeKnown ||
      (image.capabilities.validContainerMetadata && image.expandedSizeBytes != 0)) {
    appendLog("Expanded/virtual size: " +
              fromView(core::formatByteSize(image.expandedSizeBytes)) + '.');
  }
  for (const auto& warning : analysis.warnings) {
    appendLog("Image warning: " + QString::fromStdString(warning));
  }
  updateWriteReadiness();
}

void MainWindow::handleMacOsInstallerAnalysisFinished(
    const core::MacOsInstallerAnalysisResult& analysis,
    const QString& path) {
  if (!analysis.succeeded()) {
    progressBar_->setValue(0);
    progressBar_->setFormat("INSTALLER ERROR");
    statusBar()->showMessage(QString::fromStdString(analysis.error));
    appendLog("macOS installer analysis failed: " +
              QString::fromStdString(analysis.error));
    updateWriteReadiness();
    return;
  }

  selectedMacOsInstaller_ = *analysis.installer;
  core::ImageInfo source;
  source.path = analysis.installer->applicationPath;
  source.displayName = analysis.installer->displayName;
  source.volumeLabel = analysis.installer->displayName;
  source.sizeBytes = analysis.installer->payloadSizeBytes;
  source.expandedSizeBytes = analysis.installer->payloadSizeBytes;
  source.format = core::ImageFormat::MacOsInstallerApplication;
  source.partitionScheme = core::PartitionScheme::Gpt;
  source.family = core::ImageFamily::MacOsInstaller;
  source.capabilities.uefiBootable = true;
  source.capabilities.macOsInstallerCreation = true;
  source.bootable = true;
  selectedImage_ = std::move(source);

  bootSelectionBox_->clear();
  bootSelectionBox_->addItem(
      QString::fromStdString(analysis.installer->displayName), path);
  bootSelectionBox_->setToolTip(path);
  volumeLabel_->setText(
      QString::fromStdString(analysis.installer->displayName));
  partitionSchemeBox_->setCurrentText("GPT");
  targetSystemBox_->setCurrentText("Mac firmware");
  fileSystemBox_->setCurrentText("Mac OS Extended (Journaled)");
  quickFormat_->setChecked(true);
  badBlocks_->setChecked(false);
  checksumButton_->setEnabled(false);
  applyImageProfile();

  const QString version = analysis.installer->version.empty()
                              ? QStringLiteral("unknown version")
                              : QString::fromStdString(
                                    analysis.installer->version);
  progressBar_->setFormat("ANALYZED — READY");
  statusBar()->showMessage("Apple-signed macOS installer analyzed — checking target availability");
  appendLog("Validated Apple-signed macOS installer application: " +
            QString::fromStdString(analysis.installer->displayName) +
            " (" + version + ").");
  appendLog("Native tool: " +
            QString::fromStdString(
                analysis.installer->createInstallMediaPath));
  for (const auto& warning : analysis.warnings) {
    appendLog("Installer warning: " + QString::fromStdString(warning));
  }
  updateWriteReadiness();
}

void MainWindow::applyImageProfile() {
  imageOptionBox_->clear();
  imageOptionBox_->show();
  persistencePanel_->hide();
  persistenceLabel_->hide();
  imageOptionLabel_->setText("Image option");

  if (!selectedImage_) {
    imageOptionBox_->addItem("Standard installation (select an image)");
    imageOptionBox_->setEnabled(false);
    windowsToGoOptionsButton_->hide();
    volumeLabel_->setEnabled(false);
    updateRuntimeValidationAvailability();
    scheduleWindowFitToContents();
    return;
  }

  const core::ImageInfo& image = *selectedImage_;
  const core::ImageCapabilities& capabilities = image.capabilities;
  const bool executableIsoMode =
      capabilities.isoExtraction &&
      (capabilities.uefiBootable || capabilities.linuxPersistence);
  if (selectedMacOsInstaller_ && capabilities.macOsInstallerCreation) {
    imageOptionLabel_->setText("Installer option");
    imageOptionBox_->addItem("Create bootable macOS installer",
                             "macos-installer");
    imageOptionBox_->setToolTip(
        "Use Apple's createinstallmedia tool from the selected installer application.");
  } else if (capabilities.standardWindowsInstallation) {
    imageOptionBox_->addItem("Standard Windows installation", "windows-install");
    if (capabilities.windowsToGo) {
      imageOptionBox_->addItem("Windows To Go", "windows-to-go");
    }
    imageOptionBox_->setToolTip(
        "Windows media was detected from its install image and boot files. "
        "Windows To Go options use the selected install image edition.");
  } else if (capabilities.linuxPersistence) {
    imageOptionBox_->addItem("ISO image mode (file copy)", "iso-copy");
    if (capabilities.rawWrite) {
      imageOptionBox_->addItem("DD image mode", "raw-write");
    }
    imageOptionBox_->setToolTip(
        "Choose UEFI/FAT32 file extraction or an exact sector-for-sector write.");
    persistenceLabel_->show();
    persistencePanel_->show();
    const QString persistenceStyle = QString::fromUtf8(
        core::linuxPersistenceStyleName(capabilities.linuxPersistenceStyle));
    persistencePanel_->setToolTip(
        "Validated " + persistenceStyle +
        " kernel boot entries were found. A zero size performs ordinary "
        "ISO deployment; a nonzero size adds the matching ext2 persistence "
        "partition and patches only those recognized entries.");
    updatePersistenceRange();
  } else if (executableIsoMode && capabilities.rawWrite) {
    imageOptionBox_->addItem("ISO image mode (file copy)", "iso-copy");
    imageOptionBox_->addItem("DD image mode", "raw-write");
    imageOptionBox_->setToolTip(
        "This hybrid ISO supports an extracted-file layout or a sector-for-sector write.");
  } else if (executableIsoMode) {
    imageOptionBox_->addItem("ISO image mode (file copy)", "iso-copy");
    imageOptionBox_->setToolTip("The image contains a readable ISO-9660, Joliet, or UDF file tree.");
  } else if (image.format == core::ImageFormat::Ffu &&
             capabilities.validContainerMetadata) {
    imageOptionBox_->addItem("Apply FFU image", "ffu-apply");
    imageOptionBox_->setToolTip(
        deviceBackend_->capabilities().ffuApply
            ? "Apply the validated FFU through the Windows DISM provider."
            : "FFU application requires the Windows DISM provider; this host can inspect the image only.");
  } else if (image.compressed && capabilities.rawWrite) {
    imageOptionBox_->addItem("DD image mode (streaming decompression)", "raw-write");
    imageOptionBox_->setToolTip(
        "The payload was fully validated and will be decompressed directly while writing and verifying.");
  } else if (capabilities.rawWrite) {
    imageOptionBox_->addItem("DD image mode", "raw-write");
    imageOptionBox_->setToolTip("This image can only be written sector-for-sector.");
  } else {
    imageOptionBox_->addItem("Unsupported image operation");
    imageOptionBox_->setToolTip("No safe operation was identified for this image.");
  }
  imageOptionBox_->setEnabled(imageOptionBox_->count() > 1);
  const QString operation = imageOptionBox_->currentData().toString();
  volumeLabel_->setEnabled(operation == "windows-to-go" ||
                           operation == "windows-install" ||
                           operation == "iso-copy");
  windowsToGoOptionsButton_->setVisible(
      operation == "windows-to-go" || operation == "windows-install");
  windowsToGoOptionsButton_->setToolTip(
      operation == "windows-to-go" ? "Configure Windows To Go options"
                                    : "Configure Windows installation options");
  updateRuntimeValidationAvailability();
  scheduleWindowFitToContents();
}

void MainWindow::configureWindowsExperience() {
  if (imageOptionBox_->currentData().toString() == "windows-to-go") {
    static_cast<void>(collectWindowsToGoOptions());
  } else {
    static_cast<void>(collectWindowsInstallationOptions());
  }
}

bool MainWindow::collectWindowsToGoOptions() {
  if (!selectedImage_ || !selectedImage_->capabilities.windowsToGo ||
      selectedImage_->windowsEditions.empty()) {
    QMessageBox::warning(this, "Windows To Go unavailable",
                         "The selected ISO does not contain validated Windows editions.");
    return false;
  }

  const auto& editions = selectedImage_->windowsEditions;
  if (editions.size() > 1U) {
    QStringList labels;
    int current = 0;
    for (std::size_t index = 0; index < editions.size(); ++index) {
      QString label = "[" + QString::number(editions[index].index) + "] " +
                      QString::fromStdString(editions[index].name);
      if (!editions[index].description.empty() &&
          editions[index].description != editions[index].name) {
        label += " — " + QString::fromStdString(editions[index].description);
      }
      labels.append(label);
      if (editions[index].index == windowsToGoOptions_.editionIndex) {
        current = static_cast<int>(index);
      }
    }
    bool accepted = false;
    const QString selected = QInputDialog::getItem(
        this, "Select Windows edition",
        "Select the Windows edition to install:", labels, current, false, &accepted);
    if (!accepted) {
      return false;
    }
    const int selectedIndex = labels.indexOf(selected);
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(editions.size())) {
      return false;
    }
    windowsToGoOptions_.editionIndex =
        editions[static_cast<std::size_t>(selectedIndex)].index;
  } else {
    windowsToGoOptions_.editionIndex = editions.front().index;
  }

  const auto edition = std::find_if(
      editions.begin(), editions.end(), [this](const core::WindowsEditionInfo& candidate) {
        return candidate.index == windowsToGoOptions_.editionIndex;
      });
  const core::ImageArchitecture architecture =
      edition == editions.end() || edition->architecture == core::ImageArchitecture::Unknown
          ? selectedImage_->architecture
          : edition->architecture;
  const std::uint32_t windowsBuild =
      edition == editions.end() || edition->build == 0
          ? selectedImage_->windowsBuild
          : edition->build;
  WindowsUserExperienceDialog dialog(
      architecture, windowsBuild, windowsToGoOptions_.userExperience,
      core::WindowsDeploymentMode::WindowsToGo, this);
  if (dialog.exec() != QDialog::Accepted) {
    return false;
  }
  windowsToGoOptions_.userExperience = dialog.options();
  windowsToGoOptionsConfigured_ = true;
  appendLog("Configured Windows To Go edition " +
            QString::number(windowsToGoOptions_.editionIndex) +
            "; unattended options are ready.");
  updateWriteReadiness();
  return true;
}

bool MainWindow::collectWindowsInstallationOptions() {
  if (!selectedImage_ ||
      !selectedImage_->capabilities.standardWindowsInstallation) {
    QMessageBox::warning(this, "Windows options unavailable",
                         "The selected ISO is not validated Windows installation media.");
    return false;
  }
  WindowsUserExperienceDialog dialog(
      selectedImage_->architecture, selectedImage_->windowsBuild,
      windowsInstallationOptions_, core::WindowsDeploymentMode::StandardInstallation,
      this);
  if (dialog.exec() != QDialog::Accepted) {
    return false;
  }
  windowsInstallationOptions_ = dialog.options();
  windowsInstallationOptionsConfigured_ = true;
  appendLog("Configured Windows Setup unattended installation options.");
  updateWriteReadiness();
  return true;
}

void MainWindow::refreshVolumes() {
  const QString previousStableId = volumeBox_->currentData().toString();
  volumeBox_->clear();
  visibleDevices_.clear();
  if (!deviceBackend_) {
    volumeBox_->addItem("No device backend is available");
    volumeBox_->setEnabled(false);
    appendLog("Physical-device discovery is unsupported on this platform.");
    return;
  }

  const backend::DeviceDiscoveryResult discovery = deviceBackend_->discover();
  discoverySignature_ = discoverySignature(discovery);
  const bool showProtected = listFixedDisks_ != nullptr && listFixedDisks_->isChecked();
  std::size_t eligibleCount = 0;
  std::size_t displayedCount = 0;
  for (const auto& device : discovery.devices) {
    const bool eligible =
        core::evaluateDeviceEligibility(device) == core::DeviceEligibility::Eligible;
    if (eligible) {
      ++eligibleCount;
    }
    if (!eligible && !showProtected) {
      continue;
    }

    const int index = volumeBox_->count();
    volumeBox_->addItem(deviceLabel(device), QString::fromStdString(device.stableId));
    volumeBox_->setItemData(index, deviceToolTip(device), Qt::ToolTipRole);
    volumeBox_->setItemData(index, eligible, Qt::UserRole + 1);
    volumeBox_->setItemData(index, static_cast<qulonglong>(visibleDevices_.size()),
                            Qt::UserRole + 2);
    visibleDevices_.push_back(device);
    ++displayedCount;
  }

  if (displayedCount == 0) {
    volumeBox_->addItem(showProtected ? "No physical devices detected"
                                     : "No eligible removable devices detected");
    volumeBox_->setEnabled(false);
  } else {
    volumeBox_->setEnabled(true);
    const int previousIndex = volumeBox_->findData(previousStableId);
    if (previousIndex >= 0) {
      volumeBox_->setCurrentIndex(previousIndex);
    }
  }

  appendLog(QString("%1 discovered %2 physical device(s); %3 currently eligible.")
                .arg(fromView(deviceBackend_->name()))
                .arg(static_cast<qulonglong>(discovery.devices.size()))
                .arg(static_cast<qulonglong>(eligibleCount)));
  for (const auto& warning : discovery.warnings) {
    appendLog("Discovery warning: " + QString::fromStdString(warning));
  }
  updatePersistenceRange();
  updateWriteReadiness();
}

void MainWindow::pollVolumes() {
  if (updatePrivilegeIndicator()) {
    updateWriteReadiness();
  }
  if (writeThread_ != nullptr || captureThread_ != nullptr ||
      imageAnalysisThread_ != nullptr || !deviceBackend_) {
    return;
  }
  const auto discovery = deviceBackend_->discover();
  if (discoverySignature(discovery) != discoverySignature_) {
    appendLog("Physical-device change detected; refreshing target list.");
    refreshVolumes();
  }
}

void MainWindow::startWrite() {
  if (writeThread_ != nullptr || captureThread_ != nullptr || !selectedImage_) {
    return;
  }

  const QVariant selectedDevice = volumeBox_->currentData(Qt::UserRole + 2);
  if (!selectedDevice.isValid()) {
    QMessageBox::warning(this, "No target device", "Select an eligible removable device first.");
    return;
  }
  const auto deviceIndex = static_cast<std::size_t>(selectedDevice.toULongLong());
  if (deviceIndex >= visibleDevices_.size()) {
    QMessageBox::warning(this, "Stale target", "Refresh and select the target device again.");
    return;
  }

  const QString operation = selectedOperation();
  const core::VerificationProfile verificationProfile =
      selectedVerificationProfile();
  const bool isoMode = operation == "iso-copy" || operation == "windows-install" ||
                       operation == "linux-persistence";
  const bool windowsToGoMode = operation == "windows-to-go";
  const bool ffuMode = operation == "ffu-apply";
  const bool macOsInstallerMode = operation == "macos-installer";
  if (!isoMode && !windowsToGoMode && !ffuMode && !macOsInstallerMode &&
      operation != "raw-write") {
    QMessageBox::warning(this, "Operation unavailable",
                         "No executable provider is available for the selected deployment strategy.");
    return;
  }

  const auto availability =
      macOsInstallerMode && selectedMacOsInstaller_
          ? deviceBackend_->macOsInstallerAvailability(
                *selectedMacOsInstaller_, visibleDevices_[deviceIndex])
      : ffuMode
          ? deviceBackend_->ffuApplyAvailability(*selectedImage_,
                                                 visibleDevices_[deviceIndex])
          : deviceBackend_->rawWriteAvailability(visibleDevices_[deviceIndex]);
  if (!availability.available) {
    if (!availability.authorizationCanBeRequested) {
      QMessageBox::warning(this,
                           macOsInstallerMode
                               ? "macOS installer creation unavailable"
                               : "Raw writing unavailable",
                           QString::fromStdString(availability.reason));
      return;
    }
    const auto authorization = deviceBackend_->requestRawWriteAuthorization();
    static_cast<void>(updatePrivilegeIndicator());
    appendLog("Administrative disk access: " +
              QString::fromStdString(authorization.reason.empty()
                                         ? "authorized"
                                         : authorization.reason));
    if (authorization.available) {
      QMessageBox::information(
          this, "Administrative access enabled",
          macOsInstallerMode
              ? "The authorized macOS installer helper is ready. Review the target, then select START again."
              : "The authorized raw-device writer is ready. Review the target, then select START again to write.");
    } else {
      QMessageBox::information(this, "Administrative access required",
                               QString::fromStdString(authorization.reason));
    }
    updateWriteReadiness();
    return;
  }
  if (badBlocks_->isChecked()) {
    const auto testAvailability =
        deviceBackend_->badBlockTestAvailability(visibleDevices_[deviceIndex]);
    if (!testAvailability.available) {
      QMessageBox::warning(this, "Bad-block testing unavailable",
                           QString::fromStdString(testAvailability.reason));
      return;
    }
  }

  if (windowsToGoMode && !windowsToGoOptionsConfigured_ &&
      !collectWindowsToGoOptions()) {
    return;
  }
  if (operation == "windows-install" &&
      !windowsInstallationOptionsConfigured_ &&
      !collectWindowsInstallationOptions()) {
    return;
  }

  std::shared_ptr<core::RawWritePlan> rawPlan;
  std::shared_ptr<core::IsoDeploymentPlan> isoPlan;
  std::shared_ptr<core::WindowsToGoPlan> windowsToGoPlan;
  std::vector<core::SafetyIssue> planningIssues;
  std::vector<std::string> planningWarnings;
  if (macOsInstallerMode) {
    if (!selectedMacOsInstaller_ ||
        !selectedImage_->capabilities.macOsInstallerCreation) {
      planningIssues.push_back(
          {core::SafetyIssueCode::ImageUnsupported,
           "A validated Apple macOS installer application is required"});
    }
  } else if (ffuMode) {
    if (selectedImage_->format != core::ImageFormat::Ffu ||
        !selectedImage_->capabilities.validContainerMetadata) {
      planningIssues.push_back(
          {core::SafetyIssueCode::ImageUnsupported,
           "FFU deployment requires a structurally validated FFU image"});
    }
  } else if (windowsToGoMode) {
    const core::WindowsToGoPlanner planner;
    auto planning = planner.build(*selectedImage_, visibleDevices_[deviceIndex],
                                  windowsToGoOptions_, volumeLabel_->text().toStdString());
    planningIssues = planning.issues;
    planningWarnings = planning.warnings;
    if (planning.succeeded()) {
      windowsToGoPlan =
          std::make_shared<core::WindowsToGoPlan>(std::move(*planning.plan));
    }
  } else if (isoMode) {
    core::IsoDeploymentOptions deploymentOptions = isoDeploymentOptions();
    const core::IsoDeploymentPlanner planner(core::createSystemWimSplitter(),
                                             deploymentOptions);
    const core::LinuxPersistenceOptions persistence{
        operation == "linux-persistence"
            ? static_cast<std::uint64_t>(persistenceSizeBox_->value()) *
                  1024ULL * 1024ULL
            : 0U};
    const core::WindowsInstallationOptions windows{
        windowsInstallationOptions_};
    auto planning = planner.build(*selectedImage_, visibleDevices_[deviceIndex],
                                  volumeLabel_->text().toStdString(), persistence,
                                  operation == "windows-install"
                                      ? windows
                                      : core::WindowsInstallationOptions{});
    planningIssues = planning.issues;
    planningWarnings = planning.warnings;
    if (planning.succeeded()) {
      isoPlan = std::make_shared<core::IsoDeploymentPlan>(std::move(*planning.plan));
    }
  } else {
    const core::WritePlanBuilder builder;
    auto planning = builder.buildRawWriteWithVerification(
        *selectedImage_, visibleDevices_[deviceIndex], verificationProfile);
    planningIssues = planning.issues;
    if (planning.succeeded()) {
      rawPlan = std::make_shared<core::RawWritePlan>(std::move(*planning.plan));
    }
  }
  const bool planMissing = macOsInstallerMode || ffuMode
                               ? false
                               : windowsToGoMode
                               ? windowsToGoPlan == nullptr
                               : isoMode ? isoPlan == nullptr : rawPlan == nullptr;
  if (!planningIssues.empty() || planMissing) {
    QString details;
    for (const auto& issue : planningIssues) {
      if (!details.isEmpty()) {
        details += '\n';
      }
      details += QString::fromStdString(issue.message);
    }
    QMessageBox::critical(this, "Write plan rejected", details);
    return;
  }
  for (const auto& warning : planningWarnings) {
    appendLog(QString(windowsToGoMode ? "Windows To Go warning: "
                                      : "ISO deployment warning: ") +
              QString::fromStdString(warning));
  }
  if (isoPlan != nullptr &&
      isoPlan->fileSystem() == core::IsoDeploymentFilesystem::Ntfs) {
    if (ntfsIsoStager_ == nullptr) {
      QMessageBox::critical(this, "NTFS staging unavailable",
                            "No platform NTFS ISO staging provider is installed.");
      return;
    }
    const auto staging = ntfsIsoStager_->availability();
    if (!staging.available) {
      QMessageBox::critical(this, "NTFS staging unavailable",
                            QString::fromStdString(staging.reason));
      return;
    }
  }
  if (windowsToGoMode) {
    if (windowsToGoStager_ == nullptr) {
      QMessageBox::critical(this, "Windows To Go unavailable",
                            "No Windows To Go staging provider is installed.");
      return;
    }
    const auto staging = windowsToGoStager_->availability();
    if (!staging.available) {
      QMessageBox::critical(this, "Windows To Go unavailable",
                            QString::fromStdString(staging.reason));
      return;
    }
  }

  const core::BlockDeviceInfo target =
      (macOsInstallerMode || ffuMode) ? visibleDevices_[deviceIndex]
              : windowsToGoMode ? windowsToGoPlan->target()
                                : isoMode ? isoPlan->target() : rawPlan->target();
  QString isoFirmware;
  if (isoPlan != nullptr) {
    switch (isoPlan->targetSystem()) {
      case core::IsoTargetSystem::Bios:
        isoFirmware = "BIOS";
        break;
      case core::IsoTargetSystem::Uefi:
        isoFirmware = "UEFI";
        break;
      case core::IsoTargetSystem::BiosAndUefi:
        isoFirmware = "BIOS + UEFI";
        break;
      case core::IsoTargetSystem::Automatic:
        isoFirmware = "Automatic";
        break;
    }
  }
  core::DeploymentPreflightInput preflightInput;
  preflightInput.operation =
      macOsInstallerMode ? "Create bootable macOS installer"
      : ffuMode ? "Windows FFU deployment"
      : windowsToGoMode ? "Windows To Go deployment"
      : operation == "linux-persistence" ? "Persistent Linux ISO deployment"
      : isoMode ? "ISO-mode deployment"
                : "Raw image deployment";
  preflightInput.image = *selectedImage_;
  preflightInput.target = target;
  preflightInput.verificationProfile = verificationProfile;
  if (macOsInstallerMode) {
    preflightInput.verificationProfile =
        quickFormat_->isChecked() ? core::VerificationProfile::None
                                  : core::VerificationProfile::Full;
  }
  preflightInput.bytesToWrite =
      rawPlan != nullptr ? rawPlan->bytesToWrite() : target.capacityBytes;
  preflightInput.partitionScheme =
      macOsInstallerMode ? "GPT (Apple-managed)"
      : isoPlan != nullptr
          ? std::string(core::partitionSchemeName(isoPlan->partitionScheme()))
      : windowsToGoMode ? "GPT"
      : selectedImage_->partitionScheme == core::PartitionScheme::Unknown
          ? "Image-defined"
          : std::string(
                core::partitionSchemeName(selectedImage_->partitionScheme));
  preflightInput.targetSystem =
      macOsInstallerMode ? "Mac firmware"
      : isoPlan != nullptr ? isoFirmware.toStdString()
      : windowsToGoMode ? "UEFI"
      : selectedImage_->capabilities.biosBootable &&
                selectedImage_->capabilities.uefiBootable
          ? "BIOS + UEFI (image-defined)"
      : selectedImage_->capabilities.uefiBootable ? "UEFI (image-defined)"
      : selectedImage_->capabilities.biosBootable ? "BIOS (image-defined)"
                                                  : "Image-defined";
  preflightInput.fileSystem =
      macOsInstallerMode ? "Mac OS Extended (Journaled), then Apple-managed"
      : isoPlan != nullptr
          ? isoPlan->fileSystem() == core::IsoDeploymentFilesystem::Ntfs
                ? (isoPlan->targetSystem() == core::IsoTargetSystem::Bios
                       ? "NTFS" : "NTFS + UEFI:NTFS")
                : "FAT32"
      : windowsToGoMode ? "EFI FAT32 + NTFS"
                        : "Image-defined";
  preflightInput.clusterSizeBytes =
      isoPlan != nullptr ? isoPlan->clusterSizeBytes() : 0U;
  preflightInput.quickFormat = macOsInstallerMode
                                   ? quickFormat_->isChecked()
                                   : isoPlan != nullptr && isoPlan->quickFormat();
  preflightInput.temporaryBytes =
      isoMode || windowsToGoMode ? target.capacityBytes : 0U;
  if (isoPlan != nullptr) {
    configureIsoPreflightSizing(preflightInput, isoPlan->fileSystem());
  }
  preflightInput.warnings = planningWarnings;
  const auto rawAvailability = macOsInstallerMode && selectedMacOsInstaller_
                                   ? deviceBackend_->macOsInstallerAvailability(
                                         *selectedMacOsInstaller_, target)
                               : ffuMode
                                   ? deviceBackend_->ffuApplyAvailability(
                                         *selectedImage_, target)
                                   : deviceBackend_->rawWriteAvailability(target);
  preflightInput.dependencies.push_back(
      {macOsInstallerMode
#if defined(Q_OS_MACOS) && RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
           ? "Apple createinstallmedia + unsigned root process"
#else
           ? "Apple createinstallmedia + privileged helper"
#endif
       : ffuMode ? "Windows DISM FFU provider"
                 : "Guarded raw-device writer",
       true, rawAvailability.available, rawAvailability.reason});
  if (macOsInstallerMode) {
    preflightInput.transformations.emplace_back(
        "Create a temporary GPT Mac OS Extended (Journaled) target volume");
    preflightInput.transformations.emplace_back(
        "Run the selected Apple-signed createinstallmedia tool without a shell");
    if (!quickFormat_->isChecked()) {
      preflightInput.transformations.emplace_back(
          "Zero-fill and read-verify every target byte before installer creation");
    }
    if (target.capacityBytes <
        selectedMacOsInstaller_->recommendedTargetBytes) {
      preflightInput.warnings.emplace_back(
          "Apple recommends 32 GB for current macOS installers; this target meets only the 16 GiB compatibility minimum");
    }
  }
  if (isoPlan != nullptr) {
    preflightInput.transformations.emplace_back(
        "Extract and byte-verify the optical-image file tree");
    if (isoPlan->fileSystem() == core::IsoDeploymentFilesystem::Ntfs) {
      const auto ntfs = ntfsIsoStager_->availability();
      preflightInput.dependencies.push_back(
          {"NTFS staging provider", true, ntfs.available,
           ntfs.reason});
      if (isoPlan->targetSystem() != core::IsoTargetSystem::Bios) {
        preflightInput.transformations.emplace_back(
            "Install the offline UEFI:NTFS helper partition");
      }
      if (isoPlan->legacyBiosBootstrap() == core::LegacyBiosBootstrap::WindowsBootManager) {
        preflightInput.transformations.emplace_back(
            "Install Windows legacy BIOS boot code and its backup boot sector");
      }
    }
    if (isoPlan->splitsWindowsImage()) {
      const auto splitter = core::createSystemWimSplitter();
      preflightInput.dependencies.push_back(
          {"wimlib Windows-image transformer", true,
           splitter->available(), splitter->availabilityReason()});
      preflightInput.transformations.emplace_back(
          "Convert or split the Windows install image for FAT32");
    }
    if (isoPlan->hasLinuxPersistence()) {
      preflightInput.transformations.push_back(
          "Create and verify a " +
          std::string(isoPlan->linuxPersistenceStyle() ==
                              core::LinuxPersistenceStyle::Casper
                          ? "casper-rw"
                          : "Debian persistence") +
          " partition");
    }
    if (isoPlan->runtimeUefiValidation()) {
      preflightInput.transformations.emplace_back(
          "Wrap fallback loaders with offline runtime validation");
    }
  }
  if (windowsToGoMode) {
    const auto staging = windowsToGoStager_->availability();
    preflightInput.dependencies.push_back(
        {"Windows To Go staging toolchain", true, staging.available,
         staging.reason});
    preflightInput.transformations.push_back(
        "Apply Windows edition " +
        std::to_string(windowsToGoPlan->edition().index) +
        " and construct EFI/BCD boot data");
  }
  if (selectedImage_->compressed) {
    preflightInput.transformations.emplace_back(
        "Stream-decompress and verify the expanded image");
  }
  if (badBlocks_->isChecked()) {
    preflightInput.transformations.push_back(
        "Destructively test every target address with " +
        std::to_string(badBlockPassCount_->currentIndex() + 1) +
        " pattern pass(es) before deployment");
  }
  core::DeploymentPreflightReport preflight =
      core::buildDeploymentPreflight(preflightInput);
  if (!preflight.ready()) {
    showTextReport(this, "Deployment blocked",
                   QString::fromStdString(preflight.toText()));
    return;
  }

  QMessageBox confirmation(this);
  confirmation.setIcon(QMessageBox::Warning);
  confirmation.setWindowTitle("Confirm destructive write");
  confirmation.setText(
      "This operation will permanently overwrite " +
      QString::fromStdString(target.displayName) + ".");
  confirmation.setInformativeText(
      "The preflight report is ready. Expand Show Details to review the exact layout, transformations, dependencies and verification policy.\n\nAll existing data on the target will be lost.");
  confirmation.setDetailedText(QString::fromStdString(preflight.toText()));
  confirmation.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
  confirmation.setDefaultButton(QMessageBox::Cancel);
  if (confirmation.exec() != QMessageBox::Yes) {
    return;
  }
  activePreflight_ = std::move(preflight);
  operationStartedAtUtc_ =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();

  auto outcome = std::make_shared<core::RawWriteResult>();
  const bool runBadBlockTest = badBlocks_->isChecked();
  const unsigned int badBlockPasses = static_cast<unsigned int>(
      badBlockPassCount_->currentIndex() + 1);
  std::filesystem::path stagingDirectory;
  std::filesystem::path stagingPath;
  if (isoMode || windowsToGoMode) {
    stagingDirectory = fileSystemPath(QDir(QDir::tempPath()).filePath(
        QString(windowsToGoMode ? "rufus-plus-plus-wtg-stage-" : "rufus-plus-plus-iso-stage-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces)));
    std::error_code stagingError;
    if (!std::filesystem::create_directory(stagingDirectory, stagingError)) {
      QMessageBox::critical(
          this, "Staging unavailable",
          "Unable to create a private deployment staging directory: " +
              QString::fromStdString(stagingError.message()));
      activePreflight_.reset();
      operationStartedAtUtc_.clear();
      return;
    }
#if !defined(_WIN32)
    std::filesystem::permissions(stagingDirectory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, stagingError);
    if (stagingError) {
      std::error_code ignored;
      std::filesystem::remove(stagingDirectory, ignored);
      QMessageBox::critical(
          this, "Staging unavailable",
          "Unable to secure the deployment staging directory: " +
              QString::fromStdString(stagingError.message()));
      activePreflight_.reset();
      operationStartedAtUtc_.clear();
      return;
    }
#endif
    stagingPath = stagingDirectory /
#if defined(_WIN32)
        (windowsToGoMode ||
                 (isoPlan != nullptr &&
                  isoPlan->fileSystem() == core::IsoDeploymentFilesystem::Ntfs)
             ? "deployment.vhd"
             : "deployment.img");
#else
        "deployment.img";
#endif
  }
  cancelRequested_.store(false);
  closeWhenFinished_ = false;
  isoDeploymentRunning_ = isoMode;
  ntfsIsoDeploymentRunning_ =
      isoPlan != nullptr &&
      isoPlan->fileSystem() == core::IsoDeploymentFilesystem::Ntfs;
  windowsToGoDeploymentRunning_ = windowsToGoMode;
  ffuDeploymentRunning_ = ffuMode;
  macOsInstallerDeploymentRunning_ = macOsInstallerMode;
  volumeBox_->setEnabled(false);
  selectButton_->setEnabled(false);
  checksumButton_->setEnabled(false);
  captureButton_->setEnabled(false);
  formatButton_->setEnabled(false);
  imageOptionBox_->setEnabled(false);
  partitionSchemeBox_->setEnabled(false);
  targetSystemBox_->setEnabled(false);
  fileSystemBox_->setEnabled(false);
  clusterSizeBox_->setEnabled(false);
  quickFormat_->setEnabled(false);
  verificationProfileBox_->setEnabled(false);
  runtimeValidation_->setEnabled(false);
  windowsToGoOptionsButton_->setEnabled(false);
  persistencePanel_->setEnabled(false);
  badBlocks_->setEnabled(false);
  badBlockPassCount_->setEnabled(false);
  driveAdvancedButton_->setEnabled(false);
  listFixedDisks_->setEnabled(false);
  startButton_->setEnabled(false);
  closeButton_->setText("CANCEL");
  closeButton_->setEnabled(true);
  progressBar_->setValue(0);
  progressBar_->setFormat("PREPARING");
  appendLog(QString(macOsInstallerMode
                        ? "Confirmed bootable macOS installer creation on "
                        : ffuMode ? "Confirmed FFU deployment to "
                                    : windowsToGoMode ? "Confirmed Windows To Go deployment to "
                                    : isoMode ? "Confirmed ISO-mode deployment to "
                                              : "Confirmed raw write to ") +
            QString::fromStdString(target.devicePath) + '.');

  const std::optional<core::ImageInfo> ffuImage =
      ffuMode ? selectedImage_ : std::optional<core::ImageInfo>{};
  const std::optional<core::MacOsInstallerInfo> macOsInstaller =
      macOsInstallerMode ? selectedMacOsInstaller_
                         : std::optional<core::MacOsInstallerInfo>{};
  const bool fullMacOsWipe = macOsInstallerMode && !quickFormat_->isChecked();
  QThread* const thread = QThread::create(
      [this, rawPlan, isoPlan, windowsToGoPlan, stagingDirectory, stagingPath,
       target, ffuImage, macOsInstaller, fullMacOsWipe, outcome,
       runBadBlockTest, badBlockPasses, verificationProfile] {
    const auto cleanupStaging = [&] {
      if (isoPlan == nullptr && windowsToGoPlan == nullptr) {
        return;
      }
      std::error_code ignored;
      std::filesystem::remove_all(stagingDirectory, ignored);
    };
    if (runBadBlockTest) {
      const auto test = deviceBackend_->testBadBlocks(
          target, {badBlockPasses, 4U * 1024U * 1024U, 256U},
          [this](const core::BadBlockTestProgress& progress) {
            static_cast<void>(QMetaObject::invokeMethod(
                this, [this, progress] { handleBadBlockProgress(progress); },
                Qt::QueuedConnection));
          },
          [this] { return cancelRequested_.load(); });
      if (!test.success) {
        outcome->cancelled = test.cancelled;
        outcome->destructiveWriteStarted = test.destructiveWriteStarted;
        outcome->bytesWritten = test.bytesTested;
        outcome->error = test.error.empty()
                             ? "Bad-block or fake-capacity testing failed"
                             : test.error;
        if (test.badSectorCount != 0U) {
          outcome->error += "; mismatched logical sectors: " +
                            std::to_string(test.badSectorCount);
        }
        cleanupStaging();
        return;
      }
      static_cast<void>(QMetaObject::invokeMethod(
          this,
          [this] {
            appendLog("Bad-block and fake-capacity test completed without errors.");
          },
          Qt::QueuedConnection));
    }
    if (macOsInstaller.has_value()) {
      *outcome = deviceBackend_->createMacOsInstaller(
          *macOsInstaller, target, fullMacOsWipe,
          [this](const core::MacOsInstallerProgress& progress) {
            static_cast<void>(QMetaObject::invokeMethod(
                this,
                [this, progress] {
                  handleMacOsInstallerProgress(progress);
                },
                Qt::QueuedConnection));
          },
          [this] { return cancelRequested_.load(); });
      cleanupStaging();
      return;
    }
    std::shared_ptr<core::RawWritePlan> committedPlan = rawPlan;
    if (windowsToGoPlan != nullptr) {
      const backend::WindowsToGoStageResult staging = windowsToGoStager_->stage(
          *windowsToGoPlan, stagingPath,
          [this](const backend::WindowsToGoProgress& progress) {
            static_cast<void>(QMetaObject::invokeMethod(
                this, [this, progress] { handleWindowsToGoProgress(progress); },
                Qt::QueuedConnection));
          },
          [this] { return cancelRequested_.load(); });
      if (!staging.success || !staging.stagedImage) {
        outcome->cancelled = staging.cancelled;
        outcome->error = staging.error.empty()
                             ? "Windows To Go staging failed"
                             : staging.error;
        cleanupStaging();
        return;
      }
      const core::WritePlanBuilder builder;
      auto rawPlanning = builder.buildRawWriteWithVerification(
          *staging.stagedImage, target, verificationProfile,
          4U * 1024U * 1024U, false);
      if (!rawPlanning.succeeded()) {
        for (const auto& issue : rawPlanning.issues) {
          if (!outcome->error.empty()) {
            outcome->error += "; ";
          }
          outcome->error += issue.message;
        }
        cleanupStaging();
        return;
      }
      committedPlan =
          std::make_shared<core::RawWritePlan>(std::move(*rawPlanning.plan));
    } else if (isoPlan != nullptr) {
      const auto progress = [this](const core::IsoDeploymentProgress& value) {
        static_cast<void>(QMetaObject::invokeMethod(
            this, [this, value] { handleIsoDeploymentProgress(value); },
            Qt::QueuedConnection));
      };
      const core::IsoDeploymentResult staging =
          isoPlan->fileSystem() == core::IsoDeploymentFilesystem::Ntfs
              ? ntfsIsoStager_->stage(
                    *isoPlan, stagingPath, progress,
                    [this] { return cancelRequested_.load(); })
              : core::IsoImageStager{}.stage(
                    *isoPlan, stagingPath, progress,
                    [this] { return cancelRequested_.load(); });
      if (!staging.success || !staging.stagedImage) {
        outcome->cancelled = staging.cancelled;
        outcome->error = staging.error.empty() ? "ISO staging failed" : staging.error;
        cleanupStaging();
        return;
      }
      const core::WritePlanBuilder builder;
      auto rawPlanning = builder.buildRawWriteWithVerification(
          *staging.stagedImage, target, verificationProfile,
          4U * 1024U * 1024U,
          isoPlan->requiresTrailingMetadataClear(), staging.writeRanges);
      if (!rawPlanning.succeeded()) {
        for (const auto& issue : rawPlanning.issues) {
          if (!outcome->error.empty()) {
            outcome->error += "; ";
          }
          outcome->error += issue.message;
        }
        cleanupStaging();
        return;
      }
      committedPlan =
          std::make_shared<core::RawWritePlan>(std::move(*rawPlanning.plan));
    }

    const auto progress = [this](const core::RawWriteProgress& value) {
      static_cast<void>(QMetaObject::invokeMethod(
          this, [this, value] { handleWriteProgress(value); },
          Qt::QueuedConnection));
    };
    if (ffuImage.has_value()) {
      *outcome = deviceBackend_->applyFfu(
          *ffuImage, target, progress,
          [this] { return cancelRequested_.load(); });
    } else {
      *outcome = deviceBackend_->writeRaw(
          *committedPlan, progress,
          [this] { return cancelRequested_.load(); });
    }
    cleanupStaging();
  });
  writeThread_ = thread;
  connect(thread, &QThread::finished, this, [this, thread, outcome] {
    if (writeThread_ == thread) {
      writeThread_ = nullptr;
    }
    thread->deleteLater();
    handleWriteFinished(*outcome);
  });
  beginOperationGuard();
  thread->start();
}

void MainWindow::closeOrCancel() {
  if (analysisToolThread_ != nullptr) {
    analysisCancelRequested_.store(true);
    closeButton_->setEnabled(false);
    closeButton_->setText("CANCELLING…");
    statusBar()->showMessage(
        "Cancellation requested — stopping offline image analysis");
    return;
  }
  if (imageAnalysisThread_ != nullptr) {
    analysisCancelRequested_.store(true);
    closeButton_->setEnabled(false);
    closeButton_->setText("CANCELLING…");
    statusBar()->showMessage("Cancellation requested — stopping image analysis");
    return;
  }
  if (checksumThread_ != nullptr) {
    cancelRequested_.store(true);
    closeButton_->setEnabled(false);
    closeButton_->setText("CANCELLING…");
    statusBar()->showMessage("Cancellation requested — stopping checksum calculation");
    return;
  }
  if (captureThread_ != nullptr) {
    cancelRequested_.store(true);
    closeButton_->setEnabled(false);
    closeButton_->setText("CANCELLING…");
    statusBar()->showMessage("Cancellation requested — stopping device capture");
    return;
  }
  if (writeThread_ != nullptr) {
    cancelRequested_.store(true);
    closeButton_->setEnabled(false);
    closeButton_->setText("CANCELLING…");
    statusBar()->showMessage("Cancellation requested — waiting for the current device operation");
    return;
  }
  close();
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (imageAnalysisThread_ != nullptr || checksumThread_ != nullptr ||
      captureThread_ != nullptr || writeThread_ != nullptr ||
      analysisToolThread_ != nullptr) {
    closeWhenFinished_ = true;
    closeOrCancel();
    event->ignore();
    return;
  }
  QMainWindow::closeEvent(event);
}

void MainWindow::handleIsoDeploymentProgress(const core::IsoDeploymentProgress& progress) {
  noteOperationProgress();
  const QString stage = progress.stage == core::IsoDeploymentStage::Complete
                            ? "Image prepared"
                            : QString::fromUtf8(core::isoDeploymentStageName(progress.stage));
  int phasePercentage = 0;
  if (progress.totalBytes != 0) {
    phasePercentage = static_cast<int>(
        (static_cast<long double>(progress.bytesProcessed) * 100.0L) /
        static_cast<long double>(progress.totalBytes));
  }
  int overall = 0;
  switch (progress.stage) {
    case core::IsoDeploymentStage::Planning:
      overall = 1;
      break;
    case core::IsoDeploymentStage::PreparingWindowsImage:
      overall = 2 + std::clamp(phasePercentage, 0, 100) * 3 / 100;
      break;
    case core::IsoDeploymentStage::Formatting:
      overall = 5;
      break;
    case core::IsoDeploymentStage::CreatingPersistence:
      overall = 35 + std::clamp(phasePercentage, 0, 100) * 3 / 100;
      break;
    case core::IsoDeploymentStage::Extracting:
      overall = 8 + std::clamp(phasePercentage, 0, 100) * 27 / 100;
      break;
    case core::IsoDeploymentStage::Verifying:
      overall = 38 + std::clamp(phasePercentage, 0, 100) * 12 / 100;
      break;
    case core::IsoDeploymentStage::Complete:
      overall = 50;
      break;
  }
  progressBar_->setValue(overall);
  progressBar_->setFormat(stage.toUpper() +
                          (progress.totalBytes == 0 ? QString{} : " — %p% overall"));
  setOperationProgress(stage, progress.bytesProcessed, progress.totalBytes);
  QString status = stage;
  if (!progress.currentPath.empty()) {
    status += ": " + QString::fromStdString(progress.currentPath);
  }
  statusBar()->showMessage(status);
}

void MainWindow::handleWindowsToGoProgress(
    const backend::WindowsToGoProgress& progress) {
  noteOperationProgress();
  const QString stage =
      progress.stage == backend::WindowsToGoStage::Complete
          ? "Windows image prepared"
          : QString::fromUtf8(backend::windowsToGoStageName(progress.stage));
  int phasePercentage = 0;
  if (progress.totalBytes != 0) {
    phasePercentage = static_cast<int>(
        (static_cast<long double>(progress.bytesProcessed) * 100.0L) /
        static_cast<long double>(progress.totalBytes));
  }
  int overall = 0;
  switch (progress.stage) {
    case backend::WindowsToGoStage::ExtractingInstallImage:
      overall = std::clamp(phasePercentage, 0, 100) * 5 / 100;
      break;
    case backend::WindowsToGoStage::CreatingDiskLayout:
      overall = 5;
      break;
    case backend::WindowsToGoStage::FormattingFilesystems:
      overall = 7;
      break;
    case backend::WindowsToGoStage::ApplyingWindows:
      overall = 8 + std::clamp(phasePercentage, 0, 100) * 37 / 100;
      break;
    case backend::WindowsToGoStage::ConfiguringBoot:
      overall = 46;
      break;
    case backend::WindowsToGoStage::Finalizing:
      overall = 49;
      break;
    case backend::WindowsToGoStage::Complete:
      overall = 50;
      break;
  }
  progressBar_->setValue(overall);
  progressBar_->setFormat(stage.toUpper() +
                          (progress.totalBytes == 0 ? QString{} : " — %p% overall"));
  setOperationProgress(stage, progress.bytesProcessed, progress.totalBytes);
  statusBar()->showMessage(progress.detail.empty()
                               ? stage
                               : QString::fromStdString(progress.detail));
}

void MainWindow::handleCaptureProgress(
    const core::MediaCaptureProgress& progress) {
  noteOperationProgress();
  const QString stage =
      QString::fromUtf8(core::mediaCaptureStageName(progress.stage));
  int percentage = 0;
  if (progress.totalBytes != 0U) {
    percentage = static_cast<int>(
        (static_cast<long double>(progress.bytesProcessed) * 100.0L) /
        static_cast<long double>(progress.totalBytes));
  }
  percentage = std::clamp(percentage, 0, 100);
  int overall = percentage;
  if (progress.stage == core::MediaCaptureStage::Capturing) {
    overall = percentage * 70 / 100;
  } else if (progress.stage == core::MediaCaptureStage::Finalizing) {
    overall = 70;
  } else if (progress.stage == core::MediaCaptureStage::Verifying) {
    overall = 70 + percentage * 30 / 100;
  } else if (progress.stage == core::MediaCaptureStage::Complete) {
    overall = 100;
  }
  progressBar_->setValue(overall);
  progressBar_->setFormat(stage.toUpper() +
                          (progress.totalBytes == 0U ? QString{} : " — %p%"));
  statusBar()->showMessage(stage);
}

void MainWindow::handleBadBlockProgress(
    const core::BadBlockTestProgress& progress) {
  noteOperationProgress();
  const QString stage =
      QString::fromUtf8(core::badBlockTestStageName(progress.stage));
  int percentage = 0;
  if (progress.totalBytes != 0U) {
    percentage = static_cast<int>(
        (static_cast<long double>(progress.bytesProcessed) * 100.0L) /
        static_cast<long double>(progress.totalBytes));
  }
  progressBar_->setValue(std::clamp(percentage, 0, 100));
  progressBar_->setFormat(stage.toUpper() + " — %p%");
  setOperationProgress(stage, progress.bytesProcessed, progress.totalBytes);
  statusBar()->showMessage(
      QString("%1 — pass %2 of %3")
          .arg(stage)
          .arg(progress.pass)
          .arg(progress.passCount));
}

void MainWindow::handleStandaloneMediaProgress(
    const core::StandaloneMediaProgress& progress) {
  noteOperationProgress();
  const QString stage =
      progress.stage == core::StandaloneMediaStage::Complete
          ? "Filesystem prepared"
          : QString::fromUtf8(core::standaloneMediaStageName(progress.stage));
  int percentage = 0;
  if (progress.totalBytes != 0U) {
    percentage = static_cast<int>(
        (static_cast<long double>(progress.bytesProcessed) * 100.0L) /
        static_cast<long double>(progress.totalBytes));
  }
  progressBar_->setValue(std::clamp(percentage, 0, 100));
  progressBar_->setFormat(stage.toUpper() +
                          (progress.totalBytes == 0U ? QString{} : " — %p%"));
  setOperationProgress(stage, progress.bytesProcessed, progress.totalBytes);
  statusBar()->showMessage(stage);
}

void MainWindow::handleMacOsInstallerProgress(
    const core::MacOsInstallerProgress& progress) {
  noteOperationProgress();
  const QString stage =
      QString::fromUtf8(core::macOsInstallerStageName(progress.stage));
  int phase = 0;
  if (progress.totalBytes != 0U) {
    phase = static_cast<int>(
        (static_cast<long double>(progress.bytesProcessed) * 100.0L) /
        static_cast<long double>(progress.totalBytes));
  }
  phase = std::clamp(phase, 0, 100);
  int overall = 0;
  switch (progress.stage) {
    case core::MacOsInstallerStage::Revalidating:
      overall = 1;
      break;
    case core::MacOsInstallerStage::Wiping:
      overall = phase * 35 / 100;
      break;
    case core::MacOsInstallerStage::VerifyingWipe:
      overall = 35 + phase * 25 / 100;
      break;
    case core::MacOsInstallerStage::PreparingTarget:
      overall = quickFormat_->isChecked() ? 5 : 62;
      break;
    case core::MacOsInstallerStage::CreatingInstaller:
      overall = (quickFormat_->isChecked() ? 8 : 65) +
                phase * (quickFormat_->isChecked() ? 91 : 34) / 100;
      break;
    case core::MacOsInstallerStage::Complete:
      overall = 100;
      break;
  }
  progressBar_->setValue(overall);
  if (progress.stage == core::MacOsInstallerStage::CreatingInstaller) {
    // createinstallmedia reports percentage units, not transferred bytes.
    setOperationProgress(stage + QString(" — %1%").arg(phase));
  } else if (progress.stage == core::MacOsInstallerStage::Complete) {
    setOperationProgress(stage);
  } else {
    setOperationProgress(stage, progress.bytesProcessed, progress.totalBytes);
  }
  progressBar_->setFormat(
      stage.toUpper() +
      (progress.totalBytes == 0U ? QString{} : " — %p%"));
  statusBar()->showMessage(
      progress.detail.empty()
          ? stage
          : QString::fromStdString(progress.detail));
}

void MainWindow::handleWriteProgress(const core::RawWriteProgress& progress) {
  noteOperationProgress();
  QString stage = QString::fromUtf8(core::rawWriteStageName(progress.stage));
  if (ffuDeploymentRunning_ && progress.stage == core::RawWriteStage::Writing) {
    stage = "Applying FFU image";
  } else if (selectedImage_ && selectedImage_->compressed) {
    if (progress.stage == core::RawWriteStage::Writing) {
      stage = "Decompressing and writing image";
    } else if (progress.stage == core::RawWriteStage::Verifying) {
      stage = "Decompressing and verifying image";
    }
  }
  int percentage = 0;
  if (progress.totalBytes != 0) {
    percentage = static_cast<int>(
        (static_cast<long double>(progress.bytesProcessed) * 100.0L) /
        static_cast<long double>(progress.totalBytes));
  }
  const bool stagedDeployment =
      isoDeploymentRunning_ || windowsToGoDeploymentRunning_;
  const int base = stagedDeployment ? 50 : 0;
  const int span = stagedDeployment ? 50 : 100;
  const int phase = std::clamp(percentage, 0, 100);
  int withinCommit = 0;
  switch (progress.stage) {
    case core::RawWriteStage::Revalidating:
    case core::RawWriteStage::Claiming:
    case core::RawWriteStage::Unmounting:
    case core::RawWriteStage::Opening:
      withinCommit = 0;
      break;
    case core::RawWriteStage::Writing:
      withinCommit = phase * 45 / 100;
      break;
    case core::RawWriteStage::ClearingTargetMetadata:
      withinCommit = 45 + phase * 5 / 100;
      break;
    case core::RawWriteStage::Flushing:
      withinCommit = 50;
      break;
    case core::RawWriteStage::Verifying:
      withinCommit = 50 + phase * 45 / 100;
      break;
    case core::RawWriteStage::VerifyingTargetMetadata:
      withinCommit = 95 + phase * 4 / 100;
      break;
    case core::RawWriteStage::Complete:
      withinCommit = 100;
      break;
  }
  const int overall = base + withinCommit * span / 100;
  progressBar_->setValue(overall);
  if (progress.stage == core::RawWriteStage::Writing ||
      progress.stage == core::RawWriteStage::ClearingTargetMetadata ||
      progress.stage == core::RawWriteStage::Verifying ||
      progress.stage == core::RawWriteStage::VerifyingTargetMetadata) {
    progressBar_->setFormat(stage.toUpper() + " — %p% overall");
    setOperationProgress(stage, progress.bytesProcessed, progress.totalBytes);
  } else {
    progressBar_->setFormat(stage.toUpper());
    // Flushing can report all bytes written while the device is still busy.
    // Do not label waiting stages as 100% complete.
    setOperationProgress(stage);
  }
  statusBar()->showMessage(stage);
}

void MainWindow::handleWriteFinished(const core::RawWriteResult& result) {
  endOperationGuard();
  saveDeploymentReceipt(result);
  const bool wasIsoDeployment = isoDeploymentRunning_;
  const bool wasNtfsIsoDeployment = ntfsIsoDeploymentRunning_;
  const bool wasWindowsToGo = windowsToGoDeploymentRunning_;
  const bool wasFfuDeployment = ffuDeploymentRunning_;
  const bool wasMacOsInstaller = macOsInstallerDeploymentRunning_;
  const bool wasStandaloneFormat = standaloneFormatRunning_;
  const bool wasFreeDosFormat = freeDosFormatRunning_;
  const bool wasMsDosFormat = msDosFormatRunning_;
  const bool wasExt2Format = ext2FormatRunning_;
  const std::string providerFilesystem = standaloneFilesystemLabel_;
  isoDeploymentRunning_ = false;
  ntfsIsoDeploymentRunning_ = false;
  windowsToGoDeploymentRunning_ = false;
  ffuDeploymentRunning_ = false;
  macOsInstallerDeploymentRunning_ = false;
  standaloneFormatRunning_ = false;
  freeDosFormatRunning_ = false;
  msDosFormatRunning_ = false;
  ext2FormatRunning_ = false;
  standaloneFilesystemLabel_.clear();
  selectButton_->setEnabled(true);
  checksumButton_->setEnabled(selectedImage_.has_value() &&
                              !selectedMacOsInstaller_.has_value());
  windowsToGoOptionsButton_->setEnabled(true);
  driveAdvancedButton_->setEnabled(true);
  listFixedDisks_->setEnabled(true);
  persistencePanel_->setEnabled(true);
  badBlocks_->setEnabled(deviceBackend_ &&
                         deviceBackend_->capabilities().badBlockTest);
  badBlockPassCount_->setEnabled(badBlocks_->isEnabled() &&
                                 badBlocks_->isChecked());
  closeButton_->setText("CLOSE");
  closeButton_->setEnabled(true);

  if (result.success) {
    progressBar_->setValue(100);
    progressBar_->setFormat("COMPLETE");
    setOperationProgress("Complete", result.bytesWritten, result.bytesWritten);
    appendLog(wasMacOsInstaller
                  ? "Apple createinstallmedia completed successfully; the target is a bootable macOS installer."
                  : wasFfuDeployment
                  ? "The FFU image was applied successfully by Windows DISM."
                  : wasStandaloneFormat
                  ? (wasFreeDosFormat
                         ? "FreeDOS boot media was created and verified successfully."
                     : wasMsDosFormat
                         ? "MS-DOS boot media was created from the supplied system files and verified successfully."
                     : wasExt2Format
                         ? "The standalone ext2 filesystem was created and verified successfully."
                     : !providerFilesystem.empty()
                         ? QString::fromStdString(providerFilesystem) +
                               " was formatted and verified successfully."
                         : "The standalone FAT32 filesystem was created and verified successfully.")
                  : wasWindowsToGo
                  ? "Windows To Go was applied, boot configured, written, and verified successfully."
                  : wasIsoDeployment
                        ? QString("ISO files were staged to %1, written, and verified successfully.")
                              .arg(wasNtfsIsoDeployment ? "NTFS"
                                                       : "FAT32")
                        : "Raw image write and verification completed successfully.");
    QMessageBox::information(this, "Write complete",
                             wasMacOsInstaller
                                 ? "The bootable macOS installer was created successfully."
                             : wasFfuDeployment
                                 ? "FFU deployment completed successfully."
                             : wasStandaloneFormat
                                 ? (wasFreeDosFormat
                                        ? "FreeDOS boot media was created successfully."
                                   : wasMsDosFormat
                                        ? "MS-DOS boot media was created successfully from the supplied files."
                                   : wasExt2Format
                                        ? "Standalone ext2 formatting completed successfully."
                                   : !providerFilesystem.empty()
                                        ? QString::fromStdString(providerFilesystem) +
                                              " formatting completed successfully."
                                        : "Standalone FAT32 formatting completed successfully.")
                             : wasWindowsToGo
                                 ? "Windows To Go deployment and verification completed successfully."
                                 : wasIsoDeployment
                                 ? QString("ISO-mode %1 deployment and verification completed successfully.")
                                       .arg(wasNtfsIsoDeployment
                                                ? "NTFS"
                                                : "FAT32")
                                 : "The image was written and verified successfully.");
  } else {
    progressBar_->setFormat(result.cancelled ? "CANCELLED" : "WRITE FAILED");
    setOperationProgress(result.cancelled ? "Cancelled" : "Write failed");
    QString message = QString::fromStdString(result.error);
    if (result.destructiveWriteStarted) {
      message += "\n\nThe target was already modified and may contain an incomplete image.";
    }
    const QString prefix = wasMacOsInstaller
                               ? "macOS installer creation "
                               : wasFfuDeployment
                               ? "FFU deployment "
                               : wasStandaloneFormat
                               ? "Standalone format "
                               : wasWindowsToGo
                               ? "Windows To Go deployment "
                               : wasIsoDeployment ? "ISO deployment " : "Raw write ";
    appendLog(prefix + (result.cancelled ? "cancelled: " : "failed: ") + message);
    QMessageBox::critical(this, result.cancelled ? "Write cancelled" : "Write failed", message);
  }

  refreshVolumes();
  applyImageProfile();
  updateWriteReadiness();
  if (closeWhenFinished_) {
    closeWhenFinished_ = false;
    close();
  }
}

void MainWindow::updatePersistenceRange() {
  if (persistenceSizeBox_ == nullptr || persistenceSlider_ == nullptr) {
    return;
  }

  std::uint64_t availableBytes = 0;
  if (selectedImage_) {
    const QVariant selectedDevice = volumeBox_->currentData(Qt::UserRole + 2);
    if (selectedDevice.isValid()) {
      const auto index = static_cast<std::size_t>(selectedDevice.toULongLong());
      if (index < visibleDevices_.size() &&
          selectedImage_->expandedSizeBytes <=
              std::numeric_limits<std::uint64_t>::max() / 11U) {
        const std::uint64_t projectedBytes =
            (selectedImage_->expandedSizeBytes * 11U + 9U) / 10U;
        if (visibleDevices_[index].capacityBytes > projectedBytes) {
          availableBytes = visibleDevices_[index].capacityBytes - projectedBytes;
        }
      }
    }
  }

  constexpr std::uint64_t bytesPerMebibyte = 1024ULL * 1024ULL;
  const std::uint64_t availableMebibytes = availableBytes / bytesPerMebibyte;
  const int maximum = availableMebibytes < 256U
                          ? 0
                          : static_cast<int>(std::min<std::uint64_t>(
                                availableMebibytes,
                                static_cast<std::uint64_t>(
                                    std::numeric_limits<int>::max())));
  int previous = std::min(persistenceSizeBox_->value(), maximum);
  if (previous != 0 && previous < 256 && maximum >= 256) {
    previous = 256;
  }
  {
    const QSignalBlocker sizeBlocker(persistenceSizeBox_);
    const QSignalBlocker sliderBlocker(persistenceSlider_);
    persistenceSizeBox_->setRange(0, maximum);
    persistenceSizeBox_->setValue(previous);
    persistenceSlider_->setValue(maximum == 0 ? 0 : static_cast<int>(
        (static_cast<long long>(previous) * 1000LL + maximum / 2LL) / maximum));
  }
  persistenceSizeBox_->setEnabled(maximum > 0);
  persistenceSlider_->setEnabled(maximum > 0);
}

void MainWindow::toggleDriveAdvanced(const bool expanded) {
  driveAdvancedPanel_->setVisible(expanded);
  driveAdvancedButton_->setArrowType(expanded ? Qt::UpArrow : Qt::DownArrow);
  driveAdvancedButton_->setText(expanded ? "Hide advanced drive properties"
                                         : "Show advanced drive properties");
  scheduleWindowFitToContents();
}

void MainWindow::toggleFormatAdvanced(const bool expanded) {
  formatAdvancedPanel_->setVisible(expanded);
  formatAdvancedButton_->setArrowType(expanded ? Qt::UpArrow : Qt::DownArrow);
  formatAdvancedButton_->setText(expanded ? "Hide advanced format options"
                                          : "Show advanced format options");
  scheduleWindowFitToContents();
}

void MainWindow::toggleLog() {
  logView_->setVisible(!logView_->isVisible());
  if (logView_->isVisible()) {
    logView_->setFocus();
  }
  scheduleWindowFitToContents();
}

void MainWindow::fitWindowToContents() {
  QWidget* content = centralWidget();
  if (content == nullptr || content->layout() == nullptr) {
    return;
  }

  const int windowChromeWidth = std::max(0, width() - content->width());
  const int windowChromeHeight = std::max(0, height() - content->height());

  // A previous setFixedSize() can feed the old top-level constraint back into
  // nested size hints on Windows. Release it before asking the layout for its
  // new minimum after a panel is shown or hidden.
  setMinimumSize(0, 0);
  setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  content->updateGeometry();
  content->layout()->invalidate();
  content->layout()->activate();

  const QSize minimumContent = content->layout()->totalMinimumSize();
  constexpr int minimumWindowWidth = 490;
  const QSize fittedSize{
      std::max({minimumWindowWidth, width(),
                minimumContent.width() + windowChromeWidth}),
      minimumContent.height() + windowChromeHeight};
  setFixedSize(fittedSize);
}

void MainWindow::scheduleWindowFitToContents() {
  if (windowFitPending_) {
    return;
  }
  windowFitPending_ = true;
  QTimer::singleShot(0, this, [this] {
    fitWindowToContents();

    // Some platform styles deliver a nested LayoutRequest only after the
    // first queued fit. A second coalesced pass observes that settled layout.
    QTimer::singleShot(0, this, [this] {
      fitWindowToContents();
      windowFitPending_ = false;
    });
  });
}

void MainWindow::appendLog(const QString& message) {
  logView_->append(message.toHtmlEscaped());
}

QString MainWindow::selectedOperation() const {
  if (selectedImage_ && selectedImage_->capabilities.linuxPersistence &&
      persistencePanel_->isVisible() &&
      imageOptionBox_->currentData().toString() == "iso-copy" &&
      persistenceSizeBox_->value() != 0) {
    return QStringLiteral("linux-persistence");
  }
  return imageOptionBox_->currentData().toString();
}

core::VerificationProfile MainWindow::selectedVerificationProfile() const {
  if (verificationProfileBox_ == nullptr) {
    return core::VerificationProfile::Standard;
  }
  const QString selected = verificationProfileBox_->currentData().toString();
  if (selected == "fast") {
    return core::VerificationProfile::Fast;
  }
  if (selected == "full") {
    return core::VerificationProfile::Full;
  }
  return core::VerificationProfile::Standard;
}

void MainWindow::beginOperationGuard() {
  operationElapsed_.start();
  progressElapsedMs_ = 0;
  progressStage_.clear();
  operationProgressLabel_->hide();
  progressRefreshTimer_->start();
  if (writeThread_ != nullptr) {
    setOperationProgress("Preparing");
  }
  noteOperationProgress();
  stallWarningIssued_ = false;
  if (operationWatchdog_ != nullptr) {
    operationWatchdog_->start();
  }
#if defined(Q_OS_WIN)
  if (SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                              ES_AWAYMODE_REQUIRED) == 0) {
    appendLog("Power-management guard could not be enabled for this operation.");
  }
#elif defined(Q_OS_MACOS)
  const QString executable = QStandardPaths::findExecutable("caffeinate");
  if (!executable.isEmpty() && sleepInhibitor_ == nullptr) {
    sleepInhibitor_ = new QProcess(this);
    sleepInhibitor_->start(executable, {"-dimsu"}, QIODevice::ReadOnly);
    if (!sleepInhibitor_->waitForStarted(500)) {
      appendLog("Power-management guard could not start caffeinate: " +
                sleepInhibitor_->errorString());
      sleepInhibitor_->deleteLater();
      sleepInhibitor_ = nullptr;
    }
  }
#elif defined(Q_OS_LINUX)
  const QString inhibitor = QStandardPaths::findExecutable("systemd-inhibit");
  const QString sleeper = QStandardPaths::findExecutable("sleep");
  if (!inhibitor.isEmpty() && !sleeper.isEmpty() && sleepInhibitor_ == nullptr) {
    sleepInhibitor_ = new QProcess(this);
    sleepInhibitor_->start(
        inhibitor,
        {"--what=sleep:shutdown", "--mode=block",
         "--why=Rufus++ device operation", sleeper, "2147483647"},
        QIODevice::ReadOnly);
    if (!sleepInhibitor_->waitForStarted(500)) {
      appendLog("Power-management guard could not start systemd-inhibit: " +
                sleepInhibitor_->errorString());
      sleepInhibitor_->deleteLater();
      sleepInhibitor_ = nullptr;
    }
  }
#endif
}

void MainWindow::endOperationGuard() {
  if (operationElapsed_.isValid()) {
    progressElapsedMs_ = operationElapsed_.elapsed();
    operationElapsed_.invalidate();
  }
  if (progressRefreshTimer_ != nullptr) {
    progressRefreshTimer_->stop();
  }
  refreshOperationProgress();
  if (operationWatchdog_ != nullptr) {
    operationWatchdog_->stop();
  }
#if defined(Q_OS_WIN)
  static_cast<void>(SetThreadExecutionState(ES_CONTINUOUS));
#else
  if (sleepInhibitor_ != nullptr) {
    sleepInhibitor_->terminate();
    if (!sleepInhibitor_->waitForFinished(500)) {
      sleepInhibitor_->kill();
      static_cast<void>(sleepInhibitor_->waitForFinished(500));
    }
    sleepInhibitor_->deleteLater();
    sleepInhibitor_ = nullptr;
  }
#endif
  lastOperationProgressMs_ = 0;
  stallWarningIssued_ = false;
}

void MainWindow::noteOperationProgress() {
  if (stallWarningIssued_) {
    appendLog("Operation watchdog: progress callbacks resumed.");
    stallWarningIssued_ = false;
  }
  lastOperationProgressMs_ = QDateTime::currentMSecsSinceEpoch();
}

void MainWindow::setOperationProgress(const QString& stage,
                                      const std::uint64_t completed,
                                      const std::uint64_t total) {
  progressStage_ = stage;
  progressCompleted_ = std::min(completed, total);
  progressTotal_ = total;
  refreshOperationProgress();
  if (operationProgressLabel_->isHidden()) {
    operationProgressLabel_->show();
    scheduleWindowFitToContents();
  }
}

void MainWindow::refreshOperationProgress() {
  if (operationProgressLabel_ == nullptr || progressStage_.isEmpty()) {
    return;
  }
  QString title = progressStage_;
  QStringList details;
  if (progressTotal_ != 0U) {
    const auto percentage = static_cast<int>(
        static_cast<long double>(progressCompleted_) * 100.0L /
        static_cast<long double>(progressTotal_));
    title += QString(" — %1%").arg(percentage);
    details << QString("%1 of %2")
                   .arg(QString::fromStdString(core::formatByteSize(progressCompleted_)),
                        QString::fromStdString(core::formatByteSize(progressTotal_)));
  }
  const qint64 seconds = (operationElapsed_.isValid()
                             ? operationElapsed_.elapsed() : progressElapsedMs_) / 1000;
  details << QString("Elapsed %1:%2")
                 .arg(seconds / 60, 2, 10, QChar('0'))
                 .arg(seconds % 60, 2, 10, QChar('0'));
  operationProgressLabel_->setText(title + '\n' + details.join(" · "));
}

void MainWindow::configureIsoPreflightSizing(
    core::DeploymentPreflightInput& input,
    const core::IsoDeploymentFilesystem fileSystem) {
  input.writeSizePending = input.quickFormat;
  if (fileSystem == core::IsoDeploymentFilesystem::Ntfs) {
    const auto requirements = backend::estimateNtfsIsoStagingRequirements(
        input.image.expandedSizeBytes, input.target.capacityBytes,
        input.quickFormat);
    input.temporaryBytes = requirements.temporaryBytes;
    input.writeSizePending = requirements.writeSizePending;
    if (!requirements.success) {
      input.blockers.push_back(requirements.error);
    }
  }
}

void MainWindow::saveDeploymentReceipt(const core::RawWriteResult& result) {
  if (!activePreflight_.has_value()) {
    return;
  }
  core::DeploymentReceipt receipt;
  receipt.application = std::string(core::ApplicationInfo::name);
  receipt.applicationVersion = std::string(core::ApplicationInfo::version);
  receipt.startedAtUtc = operationStartedAtUtc_;
  receipt.finishedAtUtc =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
  receipt.preflight = *activePreflight_;
  receipt.success = result.success;
  receipt.cancelled = result.cancelled;
  receipt.destructiveWriteStarted = result.destructiveWriteStarted;
  receipt.bytesWritten = result.bytesWritten;
  receipt.bytesVerified = result.bytesVerified;
  receipt.verificationCompleted = result.verificationCompleted;
  if (selectedImage_ && latestChecksumImagePath_ == selectedImage_->path) {
    receipt.sourceSha256 = latestImageSha256_;
  }
  receipt.error = result.error;

  QString baseDirectory =
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (baseDirectory.isEmpty()) {
    baseDirectory = QDir::tempPath();
  }
  const QString timestamp =
      QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmsszzz");
  QString operation = filesystemSafeComponent(
      QString::fromStdString(activePreflight_->operation).toLower());
  if (operation.isEmpty()) {
    operation = "deployment";
  }
  const QString receiptPath =
      QDir(baseDirectory).filePath("receipts/" + timestamp + '-' + operation +
                                  ".json");
  const auto written =
      core::writeDeploymentReceipt(fileSystemPath(receiptPath), receipt);
  if (written.success) {
    appendLog("Deployment receipt saved to " + receiptPath + '.');
  } else {
    appendLog("Deployment receipt could not be saved: " +
              QString::fromStdString(written.error));
  }
  activePreflight_.reset();
  operationStartedAtUtc_.clear();
}

core::IsoDeploymentOptions MainWindow::isoDeploymentOptions() const {
  core::IsoDeploymentOptions options;
  options.ntfsAvailable = ntfsIsoStager_ != nullptr;
  const QString scheme = partitionSchemeBox_->currentData().toString();
  options.partitionScheme = scheme == "gpt"   ? core::PartitionScheme::Gpt
                            : scheme == "mbr" ? core::PartitionScheme::Mbr
                                              : core::PartitionScheme::Unknown;
  const QString target = targetSystemBox_->currentData().toString();
  options.targetSystem =
      target == "bios" ? core::IsoTargetSystem::Bios
      : target == "uefi" ? core::IsoTargetSystem::Uefi
      : target == "dual" ? core::IsoTargetSystem::BiosAndUefi
                           : core::IsoTargetSystem::Automatic;
  options.fileSystem =
      fileSystemBox_->currentText() == "NTFS"
          ? core::IsoFilesystemPreference::Ntfs
      : fileSystemBox_->currentText() == "FAT32"
          ? core::IsoFilesystemPreference::Fat32
          : core::IsoFilesystemPreference::Automatic;
  options.clusterSizeBytes = clusterSizeBox_->currentData().toUInt();
  options.quickFormat = quickFormat_->isChecked();
  if (runtimeValidation_->isChecked()) {
    options.runtimeUefiValidation = runtimeValidationAssets_;
  }
  return options;
}

void MainWindow::updateDeploymentOptionControls() {
  if (partitionSchemeBox_ == nullptr || targetSystemBox_ == nullptr ||
      fileSystemBox_ == nullptr || clusterSizeBox_ == nullptr ||
      quickFormat_ == nullptr || verificationProfileBox_ == nullptr) {
    return;
  }
  const bool busy = imageAnalysisThread_ != nullptr || checksumThread_ != nullptr ||
                    captureThread_ != nullptr || writeThread_ != nullptr ||
                    analysisToolThread_ != nullptr;
  const QString operation = selectedOperation();
  const bool isoMode = operation == "iso-copy" || operation == "windows-install" ||
                       operation == "linux-persistence";
  const bool windowsToGo = operation == "windows-to-go";
  const bool macOsInstaller = operation == "macos-installer";
  const bool hasFormatTarget =
      volumeBox_ != nullptr && volumeBox_->currentData(Qt::UserRole + 2).isValid();

  if (macOsInstaller) {
    const QSignalBlocker schemeBlocker(partitionSchemeBox_);
    const QSignalBlocker targetBlocker(targetSystemBox_);
    const QSignalBlocker filesystemBlocker(fileSystemBox_);
    partitionSchemeBox_->setCurrentText("GPT");
    targetSystemBox_->setCurrentText("Mac firmware");
    fileSystemBox_->setCurrentText("Mac OS Extended (Journaled)");
  } else if (windowsToGo) {
    const QSignalBlocker schemeBlocker(partitionSchemeBox_);
    const QSignalBlocker targetBlocker(targetSystemBox_);
    const QSignalBlocker filesystemBlocker(fileSystemBox_);
    partitionSchemeBox_->setCurrentText("GPT");
    targetSystemBox_->setCurrentText("UEFI (non CSM)");
    fileSystemBox_->setCurrentText("NTFS");
  } else if (!isoMode && selectedImage_) {
    const QSignalBlocker filesystemBlocker(fileSystemBox_);
    fileSystemBox_->setCurrentText("Image-defined");
  }

  if (isoMode && fileSystemBox_->currentText() != "FAT32" &&
      fileSystemBox_->currentText() != "NTFS") {
    const QSignalBlocker blocker(fileSystemBox_);
    fileSystemBox_->setCurrentText("FAT32");
  }

  partitionSchemeBox_->setEnabled(isoMode && !busy);
  fileSystemBox_->setEnabled(isoMode && !busy);
  clusterSizeBox_->setEnabled((isoMode || (!selectedImage_ && hasFormatTarget)) &&
                              !busy);
  quickFormat_->setEnabled((isoMode || macOsInstaller ||
                            (!selectedImage_ && hasFormatTarget)) &&
                           !busy);
  verificationProfileBox_->setEnabled(!busy && !macOsInstaller);
  if (macOsInstaller) {
    const QSignalBlocker verificationBlocker(verificationProfileBox_);
    verificationProfileBox_->setCurrentIndex(
        verificationProfileBox_->findData(
            quickFormat_->isChecked() ? "apple-native" : "apple-full"));
    verificationProfileBox_->setToolTip(
        quickFormat_->isChecked()
            ? "Apple createinstallmedia owns target erasure, copying, and boot-media finalization."
            : "Rufus++ zero-fills and read-verifies the whole target before Apple createinstallmedia runs.");
    if (badBlocks_->isChecked()) {
      const QSignalBlocker badBlocksBlocker(badBlocks_);
      badBlocks_->setChecked(false);
    }
    badBlocks_->setEnabled(false);
    badBlockPassCount_->setEnabled(false);
  } else {
    if (verificationProfileBox_->currentData().toString().startsWith("apple-")) {
      const QSignalBlocker blocker(verificationProfileBox_);
      verificationProfileBox_->setCurrentIndex(
          verificationProfileBox_->findData("standard"));
    }
    verificationProfileBox_->setToolTip(
        "Fast compares deterministic samples. Standard compares every written byte. Full device requires an image or full format covering the complete target.");
    badBlocks_->setEnabled(!busy && deviceBackend_ &&
                           deviceBackend_->capabilities().badBlockTest);
    badBlockPassCount_->setEnabled(badBlocks_->isEnabled() &&
                                   badBlocks_->isChecked());
  }

  const bool fullDeviceVerification =
      selectedVerificationProfile() == core::VerificationProfile::Full;
  if (!macOsInstaller && fullDeviceVerification && quickFormat_->isChecked() &&
      (isoMode || (!selectedImage_ && hasFormatTarget))) {
    const QSignalBlocker blocker(quickFormat_);
    quickFormat_->setChecked(false);
  }
  if (!macOsInstaller && fullDeviceVerification &&
      (isoMode || (!selectedImage_ && hasFormatTarget))) {
    quickFormat_->setEnabled(false);
    quickFormat_->setToolTip(
        "Full-device verification requires a full format that covers every target sector.");
  } else if (macOsInstaller) {
    quickFormat_->setToolTip(
        "Checked: let createinstallmedia erase and prepare the target. Clear it to zero-fill and verify the complete device first.");
  } else {
    quickFormat_->setToolTip(
        "Clear this to overwrite and verify every otherwise-unused target sector. DD and FFU modes always use the image-defined layout.");
  }

  const bool gpt = partitionSchemeBox_->currentData().toString() == "gpt";
  if (macOsInstaller) {
    targetSystemBox_->setEnabled(false);
    targetSystemBox_->setToolTip(
        "Apple createinstallmedia defines the boot layout for Mac firmware.");
  } else if (isoMode && gpt) {
    const QSignalBlocker blocker(targetSystemBox_);
    targetSystemBox_->setCurrentText("UEFI (non CSM)");
    targetSystemBox_->setEnabled(false);
    targetSystemBox_->setToolTip(
        "GPT ISO layouts use UEFI (non-CSM). Select MBR for legacy BIOS boot.");
  } else {
    targetSystemBox_->setEnabled(isoMode && !busy);
    targetSystemBox_->setToolTip(
        isoMode ? "Choose the firmware boot path required from the selected image."
                : "The selected operation defines its own target firmware.");
  }

  const bool fat32 = fileSystemBox_->currentText() == "FAT32";
  if (fat32 && clusterSizeBox_->currentData().toUInt() > 32768U) {
    const QSignalBlocker blocker(clusterSizeBox_);
    clusterSizeBox_->setCurrentIndex(0);
  }
  clusterSizeBox_->setToolTip(
      macOsInstaller
          ? "Allocation-unit selection is owned by Apple createinstallmedia."
      : fat32
          ? "FAT32 allocation units may be selected from the target sector size through 32 KiB."
          : "NTFS allocation units may be selected from the target sector size through 64 KiB.");
}

void MainWindow::updateRuntimeValidationAvailability() {
  if (runtimeValidation_ == nullptr) {
    return;
  }
  const QString operation = imageOptionBox_ != nullptr
                                ? imageOptionBox_->currentData().toString()
                                : QString{};
  const bool isoOperation = operation == "iso-copy" ||
                            operation == "windows-install" ||
                            operation == "linux-persistence";
  const bool busy = imageAnalysisThread_ != nullptr || checksumThread_ != nullptr ||
                    captureThread_ != nullptr || writeThread_ != nullptr;
  const bool available = !busy && runtimeValidationAssets_ != nullptr &&
                         selectedImage_.has_value() && isoOperation &&
                         selectedImage_->capabilities.isoExtraction &&
                         selectedImage_->capabilities.uefiBootable &&
                         fileSystemBox_->currentText() == "FAT32" &&
                         targetSystemBox_->currentData().toString() != "bios";
  if (!available && runtimeValidation_->isChecked()) {
    runtimeValidation_->setChecked(false);
  }
  runtimeValidation_->setEnabled(available);
  if (runtimeValidationAssets_ == nullptr) {
    runtimeValidation_->setToolTip(
        "The packaged offline UEFI validation applications are unavailable.");
  } else if (!available) {
    runtimeValidation_->setToolTip(
        "Boot-time validation is available for UEFI-bootable FAT32 ISO deployments.");
  } else {
    runtimeValidation_->setToolTip(
        "Wrap supported UEFI fallback loaders with Rufus++'s offline MD5 validation app and create a complete manifest.");
  }
}

void MainWindow::updateWriteReadiness() {
  updateDeploymentOptionControls();
  updateRuntimeValidationAvailability();
  startButton_->setText("START");
  startButton_->setEnabled(false);
  captureButton_->setEnabled(false);
  formatButton_->setEnabled(false);
  if (imageAnalysisThread_ != nullptr) {
    startButton_->setToolTip("Image analysis is still running.");
    return;
  }
  if (analysisToolThread_ != nullptr) {
    startButton_->setToolTip("Offline image analysis is still running.");
    return;
  }
  if (checksumThread_ != nullptr) {
    startButton_->setToolTip("Image checksums are still being computed.");
    return;
  }
  if (captureThread_ != nullptr) {
    startButton_->setToolTip("A device capture is already running.");
    return;
  }
  if (writeThread_ != nullptr) {
    startButton_->setToolTip("A device operation is already running.");
    return;
  }
  const QVariant selectedDevice = volumeBox_->currentData(Qt::UserRole + 2);
  if (!selectedDevice.isValid()) {
    startButton_->setToolTip("Select an eligible removable target device.");
    return;
  }
  const auto index = static_cast<std::size_t>(selectedDevice.toULongLong());
  if (index >= visibleDevices_.size()) {
    startButton_->setToolTip("The selected target is stale; refresh device discovery.");
    return;
  }

  if (deviceBackend_) {
    const auto capabilities = deviceBackend_->capabilities();
    const auto availability =
        deviceBackend_->rawWriteAvailability(visibleDevices_[index]);
    const bool formatAvailable =
        capabilities.rawWrite && capabilities.unmountVolumes &&
        capabilities.exclusiveAccess && capabilities.flush &&
        capabilities.identityRevalidation && capabilities.rawVerification &&
        (availability.available || availability.authorizationCanBeRequested);
    formatButton_->setEnabled(formatAvailable);
    formatButton_->setToolTip(
        formatAvailable
            ? "Create a standalone filesystem or bootloader on the selected device"
            : "Standalone formatting is unavailable for the selected device");
  }

  if (deviceBackend_ && deviceBackend_->capabilities().mediaCapture) {
    const std::array formats{
        core::MediaCaptureFormat::Raw,
        core::MediaCaptureFormat::FixedVhd,
        core::MediaCaptureFormat::DynamicVhd,
        core::MediaCaptureFormat::DynamicVhdx,
        core::MediaCaptureFormat::Ffu,
        core::MediaCaptureFormat::UdfIso,
    };
    const bool captureAvailable = std::any_of(
        formats.begin(), formats.end(), [this, index](const auto format) {
          const auto availability = deviceBackend_->captureAvailability(
              visibleDevices_[index], format);
          return availability.available || availability.authorizationCanBeRequested;
        });
    captureButton_->setEnabled(captureAvailable);
    captureButton_->setToolTip(
        captureAvailable
            ? "Save the selected physical device to a verified image"
            : "No capture provider is available for the selected device");
  }
  if (!selectedImage_) {
    startButton_->setToolTip("Select and analyze a boot image first.");
    return;
  }

  const QString operation = selectedOperation();
  const bool isoMode = operation == "iso-copy" || operation == "windows-install" ||
                       operation == "linux-persistence";
  const bool windowsToGoMode = operation == "windows-to-go";
  const bool ffuMode = operation == "ffu-apply";
  const bool macOsInstallerMode = operation == "macos-installer";
  bool isoNeedsNtfs = false;
  if (selectedImage_->capabilities.linuxPersistence && persistencePanel_->isVisible()) {
    const bool persistenceApplies = imageOptionBox_->currentData().toString() == "iso-copy";
    persistencePanel_->setEnabled(persistenceApplies);
    persistenceLabel_->setEnabled(persistenceApplies);
  }
  if (!isoMode && !windowsToGoMode && !ffuMode && !macOsInstallerMode &&
      operation != "raw-write") {
    startButton_->setToolTip("No executable deployment strategy is selected.");
    return;
  }

  if (macOsInstallerMode) {
    if (!selectedMacOsInstaller_) {
      startButton_->setToolTip(
          "Select and validate an Apple macOS installer application first.");
      return;
    }
    const auto availability = deviceBackend_->macOsInstallerAvailability(
        *selectedMacOsInstaller_, visibleDevices_[index]);
    if (!availability.available &&
        !availability.authorizationCanBeRequested) {
      startButton_->setToolTip(QString::fromStdString(availability.reason));
      return;
    }
  } else if (ffuMode) {
    const QSignalBlocker schemeBlocker(partitionSchemeBox_);
    const QSignalBlocker filesystemBlocker(fileSystemBox_);
    const QSignalBlocker targetBlocker(targetSystemBox_);
    partitionSchemeBox_->setCurrentText("Auto (recommended)");
    fileSystemBox_->setCurrentText("Image-defined");
    targetSystemBox_->setCurrentText("Auto (recommended)");
  } else if (windowsToGoMode) {
    const core::WindowsToGoPlanner planner;
    const auto planning = planner.build(*selectedImage_, visibleDevices_[index],
                                        windowsToGoOptions_,
                                        volumeLabel_->text().toStdString());
    if (!planning.succeeded()) {
      startButton_->setToolTip(QString::fromStdString(planning.issues.front().message));
      return;
    }
  } else if (isoMode) {
    core::IsoDeploymentOptions deploymentOptions = isoDeploymentOptions();
    const core::IsoDeploymentPlanner planner(core::createSystemWimSplitter(),
                                             deploymentOptions);
    const core::LinuxPersistenceOptions persistence{
        operation == "linux-persistence"
            ? static_cast<std::uint64_t>(persistenceSizeBox_->value()) *
                  1024ULL * 1024ULL
            : 0U};
    const core::WindowsInstallationOptions windows{
        windowsInstallationOptions_};
    const auto planning = planner.build(
        *selectedImage_, visibleDevices_[index], volumeLabel_->text().toStdString(),
        persistence, operation == "windows-install"
                         ? windows
                         : core::WindowsInstallationOptions{});
    if (!planning.succeeded()) {
      startButton_->setToolTip(QString::fromStdString(planning.issues.front().message));
      return;
    }
    isoNeedsNtfs =
        planning.plan->fileSystem() == core::IsoDeploymentFilesystem::Ntfs;
  } else {
    const auto scheme = core::partitionSchemeName(selectedImage_->partitionScheme);
    const QSignalBlocker schemeBlocker(partitionSchemeBox_);
    const QSignalBlocker targetBlocker(targetSystemBox_);
    partitionSchemeBox_->setCurrentText(
        selectedImage_->partitionScheme == core::PartitionScheme::Unknown
            ? "Auto (recommended)"
            : fromView(scheme));
    targetSystemBox_->setCurrentText(
        selectedImage_->capabilities.uefiBootable &&
                selectedImage_->capabilities.biosBootable
            ? "BIOS + UEFI"
        : selectedImage_->capabilities.uefiBootable
            ? "UEFI (non CSM)"
        : selectedImage_->capabilities.biosBootable
            ? "BIOS (or UEFI-CSM)"
            : "Auto (recommended)");
    const core::WritePlanBuilder builder;
    const auto planning = builder.buildRawWriteWithVerification(
        *selectedImage_, visibleDevices_[index], selectedVerificationProfile());
    if (!planning.succeeded()) {
      startButton_->setToolTip(QString::fromStdString(planning.issues.front().message));
      return;
    }
  }

  const auto capabilities = deviceBackend_->capabilities();
  const bool rawOperationsReady =
      macOsInstallerMode
          ? capabilities.macOsInstallerCreation &&
                capabilities.identityRevalidation && capabilities.unmountVolumes
      : ffuMode
          ? capabilities.ffuApply && capabilities.unmountVolumes &&
                capabilities.exclusiveAccess && capabilities.identityRevalidation
          : capabilities.rawWrite && capabilities.unmountVolumes &&
                capabilities.exclusiveAccess && capabilities.flush &&
                capabilities.identityRevalidation && capabilities.rawVerification;
  if (!rawOperationsReady) {
    startButton_->setToolTip(
        "Image and target pass portable validation, but this backend does not expose the complete raw-device contract.");
    return;
  }

  const auto availability =
      macOsInstallerMode && selectedMacOsInstaller_
          ? deviceBackend_->macOsInstallerAvailability(
                *selectedMacOsInstaller_, visibleDevices_[index])
      : ffuMode
          ? deviceBackend_->ffuApplyAvailability(*selectedImage_,
                                                 visibleDevices_[index])
          : deviceBackend_->rawWriteAvailability(visibleDevices_[index]);
  if (!availability.available) {
    startButton_->setToolTip(QString::fromStdString(availability.reason));
    if (availability.authorizationCanBeRequested) {
      startButton_->setText("AUTHORIZE");
      startButton_->setEnabled(true);
    }
    return;
  }
  if (badBlocks_->isChecked()) {
    if (!capabilities.badBlockTest) {
      startButton_->setToolTip(
          "This platform backend does not provide bad-block testing.");
      return;
    }
    const auto testAvailability =
        deviceBackend_->badBlockTestAvailability(visibleDevices_[index]);
    if (!testAvailability.available) {
      startButton_->setToolTip(QString::fromStdString(testAvailability.reason));
      return;
    }
  }

  if (windowsToGoMode) {
    if (windowsToGoStager_ == nullptr) {
      startButton_->setToolTip("No Windows To Go staging provider is available.");
      return;
    }
    const auto staging = windowsToGoStager_->availability();
    if (!staging.available) {
      startButton_->setToolTip(QString::fromStdString(staging.reason));
      return;
    }
  }
  if (isoMode && isoNeedsNtfs) {
    const auto staging = ntfsIsoStager_->availability();
    if (!staging.available) {
      startButton_->setToolTip(QString::fromStdString(staging.reason));
      return;
    }
  }

  startButton_->setEnabled(true);
  startButton_->setToolTip(
      macOsInstallerMode
          ? "Use the selected Apple createinstallmedia tool to erase this device and create a bootable macOS installer. Clear Quick format for a full zero-fill and verification pass first."
      : ffuMode
          ? "Apply the validated Full Flash Update image through Windows DISM after target identity revalidation."
      : windowsToGoMode
          ? "Select a Windows edition and user-experience options, apply it to a GPT/NTFS "
            "portable installation, configure UEFI boot files, then write and verify it."
          : isoMode
                ? "Create a bootable FAT32 or NTFS layout for the selected BIOS/UEFI target, extract and verify the ISO files, "
                  "then write and verify it through this platform's native raw-device backend."
                : "Write and verify the image after an explicit destructive-action confirmation. "
                  "Administrative raw-device access may still be required.");
}

}  // namespace rufus::qt
