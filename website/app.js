(() => {
  "use strict";

  const repository = "farrell236/rufuspp";
  const releaseBanner = document.querySelector("#release-banner");
  const releaseName = document.querySelector("#release-name");
  const releaseDate = document.querySelector("#release-date");
  const releaseNotesLink = document.querySelector("#release-notes-link");
  const releaseList = document.querySelector("#release-list");
  const carouselImage = document.querySelector("#carousel-image");
  const carouselCaption = document.querySelector("#screenshot-caption");
  const carouselPosition = document.querySelector("#screenshot-position");

  const screenshots = [
    {
      src: "assets/windows-installation-mode.png",
      alt: "Rufus++ configured to create Windows installation media",
      caption: "Windows installation media",
    },
    {
      src: "assets/windows-to-go-options.png",
      alt: "Rufus++ Windows To Go options dialog",
      caption: "Windows To Go options",
    },
    {
      src: "assets/linux-persistence-mode.png",
      alt: "Rufus++ Linux persistence controls",
      caption: "Linux persistence",
    },
    {
      src: "assets/macos-installer-mode.png",
      alt: "Rufus++ macOS installer creation mode",
      caption: "macOS installer creation",
    },
    {
      src: "assets/destructive-write-confirmation.png",
      alt: "Rufus++ destructive-write confirmation",
      caption: "Write confirmation",
    },
  ];
  let screenshotIndex = 0;

  const assetMatchers = {
    "windows-x64": (name) => /windows.*x86_64.*\.zip$/i.test(name),
    "linux-x64": (name) => /linux.*x86_64.*\.tar\.gz$/i.test(name),
    "macos-arm64": (name) => /macos.*arm64.*\.dmg$/i.test(name),
    "macos-x64": (name) => /macos.*x86_64.*\.dmg$/i.test(name),
  };
  function showScreenshot(index) {
    screenshotIndex = (index + screenshots.length) % screenshots.length;
    const screenshot = screenshots[screenshotIndex];
    carouselImage.src = screenshot.src;
    carouselImage.alt = screenshot.alt;
    carouselCaption.textContent = screenshot.caption;
    carouselPosition.textContent = `${screenshotIndex + 1} of ${screenshots.length}`;
  }

  const formatDate = (value) =>
    new Intl.DateTimeFormat(undefined, {
      year: "numeric",
      month: "short",
      day: "numeric",
    }).format(new Date(value));

  const truncateNotes = (body) => {
    const clean = (body || "")
      .replace(/\\r\\n/g, "\n")
      .replace(/\\n/g, "\n")
      .replace(/\\r/g, "\n")
      .replace(/```[\s\S]*?```/g, "")
      .replace(/!\[[^\]]*\]\([^)]*\)/g, "")
      .replace(/\[([^\]]+)\]\([^)]*\)/g, "$1")
      .replace(/^[#>*+-]+\s*/gm, "")
      .replace(/\r/g, "")
      .trim();
    if (!clean) return "Package details and checksums are available on GitHub.";
    return clean.length > 260 ? `${clean.slice(0, 257).trim()}…` : clean;
  };

  async function fetchPublishedReleases() {
    const response = await fetch(
      `https://api.github.com/repos/${repository}/releases?per_page=6`,
      { headers: { Accept: "application/vnd.github+json" } },
    );
    if (!response.ok) throw new Error(`GitHub returned ${response.status}`);
    return (await response.json()).filter((release) => !release.draft);
  }

  function setUnavailableDownloads(message) {
    document.querySelectorAll(".download-card").forEach((card) => {
      const link = card.querySelector(".download-link");
      const checksumField = card.querySelector(".checksum-field");
      const checksumValue = card.querySelector(".checksum-value");
      const copyButton = card.querySelector(".checksum-copy");
      link.classList.remove("is-loading");
      link.textContent = message;
      link.removeAttribute("href");
      link.setAttribute("aria-disabled", "true");
      checksumField.classList.remove("is-loading");
      checksumValue.textContent = "Not available";
      copyButton.disabled = true;
      copyButton.removeAttribute("data-checksum");
    });
  }

  function populateDownloads(release) {
    document.querySelectorAll(".download-card").forEach((card) => {
      const link = card.querySelector(".download-link");
      const checksumField = card.querySelector(".checksum-field");
      const checksumValue = card.querySelector(".checksum-value");
      const copyButton = card.querySelector(".checksum-copy");
      const matcher = assetMatchers[card.dataset.asset];
      const asset = release.assets.find((candidate) => matcher(candidate.name));
      const checksum = asset?.digest?.match(/^sha256:([a-f0-9]{64})$/i)?.[1];

      checksumField.classList.remove("is-loading");
      checksumValue.textContent = checksum || "Not provided";
      copyButton.disabled = !checksum;
      copyButton.textContent = "Copy";
      if (checksum) {
        copyButton.dataset.checksum = checksum;
        copyButton.setAttribute("aria-label", `Copy SHA-256 for ${asset.name}`);
      } else {
        copyButton.removeAttribute("data-checksum");
        copyButton.removeAttribute("aria-label");
      }

      link.classList.remove("is-loading");
      if (!asset) {
        link.textContent = "Not in this release";
        link.removeAttribute("href");
        link.setAttribute("aria-disabled", "true");
        return;
      }
      link.href = asset.browser_download_url;
      link.textContent = "Download";
      link.removeAttribute("aria-disabled");
      link.setAttribute("aria-label", `Download ${asset.name}`);
    });
  }

  function renderReleaseHistory(releases) {
    releaseList.replaceChildren();
    if (!releases.length) {
      const entry = document.createElement("article");
      entry.className = "empty-release";
      const marker = document.createElement("span");
      marker.className = "status-dot";
      marker.setAttribute("aria-hidden", "true");
      const heading = document.createElement("h3");
      heading.textContent = "No public release has been published yet.";
      const detail = document.createElement("p");
      detail.textContent =
        "Release notes and direct platform downloads will appear here automatically after the first GitHub Release is published.";
      entry.append(marker, heading, detail);
      releaseList.append(entry);
      return;
    }

    releases.slice(0, 4).forEach((release) => {
      const entry = document.createElement("article");
      entry.className = "release-entry";
      const meta = document.createElement("div");
      meta.className = "release-meta";
      meta.textContent = `${release.tag_name} · ${formatDate(release.published_at)}`;
      const content = document.createElement("div");
      const heading = document.createElement("h3");
      heading.textContent = release.name || release.tag_name;
      const detail = document.createElement("p");
      detail.textContent = truncateNotes(release.body);
      content.append(heading, detail);
      const link = document.createElement("a");
      link.href = release.html_url;
      link.textContent = "Release Notes ↗";
      entry.append(meta, content, link);
      releaseList.append(entry);
    });
  }

  async function loadReleases() {
    try {
      const releases = await fetchPublishedReleases();
      renderReleaseHistory(releases);
      if (!releases.length) {
        setUnavailableDownloads("Awaiting release");
        return;
      }

      const latest = releases[0];
      releaseName.textContent = latest.name || latest.tag_name;
      releaseDate.textContent = `Published ${formatDate(latest.published_at)}`;
      releaseNotesLink.href = latest.html_url;
      releaseBanner.hidden = false;
      populateDownloads(latest);
    } catch (error) {
      setUnavailableDownloads("View on GitHub");
      document.querySelectorAll(".download-link").forEach((link) => {
        link.href = `https://github.com/${repository}/releases`;
        link.removeAttribute("aria-disabled");
      });
      renderReleaseHistory([]);
      console.warn("Release lookup failed", error);
    }
  }

  const navigationLinks = [...document.querySelectorAll(".nav-tabs a")];
  const observedSections = navigationLinks
    .map((link) => document.querySelector(link.getAttribute("href")))
    .filter(Boolean);

  const navigationObserver = new IntersectionObserver(
    (entries) => {
      const visible = entries
        .filter((entry) => entry.isIntersecting)
        .sort((left, right) => right.intersectionRatio - left.intersectionRatio)[0];
      if (!visible) return;
      navigationLinks.forEach((link) => {
        if (link.getAttribute("href") === `#${visible.target.id}`) {
          link.setAttribute("aria-current", "page");
        } else {
          link.removeAttribute("aria-current");
        }
      });
    },
    { rootMargin: "-25% 0px -60%", threshold: [0, 0.15, 0.4] },
  );
  observedSections.forEach((section) => navigationObserver.observe(section));

  document.querySelector(".carousel-button.previous").addEventListener("click", () => {
    showScreenshot(screenshotIndex - 1);
  });
  document.querySelector(".carousel-button.next").addEventListener("click", () => {
    showScreenshot(screenshotIndex + 1);
  });

  document.querySelectorAll(".checksum-copy").forEach((button) => {
    button.addEventListener("click", async () => {
      if (button.disabled || !button.dataset.checksum) return;

      try {
        const temporaryInput = document.createElement("textarea");
        temporaryInput.value = button.dataset.checksum;
        temporaryInput.setAttribute("readonly", "");
        temporaryInput.style.position = "fixed";
        temporaryInput.style.opacity = "0";
        document.body.append(temporaryInput);
        temporaryInput.select();
        let copied = document.execCommand("copy");
        temporaryInput.remove();

        if (!copied && navigator.clipboard && window.isSecureContext) {
          await navigator.clipboard.writeText(button.dataset.checksum);
          copied = true;
        }
        if (!copied) throw new Error("Copy command was rejected");
        button.textContent = "Copied";
        window.setTimeout(() => { button.textContent = "Copy"; }, 1600);
      } catch (error) {
        button.textContent = "Copy failed";
        window.setTimeout(() => { button.textContent = "Copy"; }, 1600);
        console.warn("Unable to copy checksum", error);
      }
    });
  });

  document.querySelector("#copyright-year").textContent = new Date().getFullYear();
  loadReleases();
})();
