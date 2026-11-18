# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/).

## [1.0.0] — 2026-11-18

First public release.

### Added
- Windows Explorer thumbnails for `.step` and `.stp` files (Windows 10/11, x64).
- Software renderer based on Open CASCADE 7.7.2: isometric view, anti-aliasing, edge lines, silhouette outline and transparent background.
- Automatic up-axis detection: Y-up for SolidWorks, Creo and Inventor exports; Z-up for everything else.
- Out-of-process rendering with a 45-second timeout and a job object, so Explorer never hangs.
- Installer that coexists with SolidWorks, Fusion and eDrawings, and backs up and restores any existing STEP thumbnail handler.
- GitHub Actions pipeline: cross-build on Linux, then install and test on Windows. Releases are published automatically from `v*` tags.
