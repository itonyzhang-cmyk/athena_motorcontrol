# Temporary validation area

This directory contains generated build trees and historical bench artifacts
that are useful for audit but are not source code or release baselines.

- `artifacts/`: dated experimental images, captures, and result reports.
- `builds/`: dated or profile-specific generated firmware build directories.

The source tests in `tests/`, production firmware in `Core/`, tools, docs, and
the formal release under `artifacts/athena_mainline_release_197f5cd_20260905/`
remain outside this temporary area. Do not flash an image from here as normal
firmware unless it receives a new disposition and exact SHA-locked review.
