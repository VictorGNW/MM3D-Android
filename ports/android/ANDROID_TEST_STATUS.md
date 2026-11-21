# Moonfall Curse Android test status

Updated November 21, 2026. This record covers the installer, Android renderer,
touch controls, and experimental Adreno compatibility path.

## Forge installation and Xclipse 540

The Samsung SM-A566E report ended in `SIGABRT` on `forge-install` while the
Samsung Vulkan driver was allocating memory. It also recorded exits under
memory pressure. The Forge can create Vulkan pipelines in the installer process
before the game starts, making optional prewarming a plausible contributor.
The report alone does not establish the cause of the mapping failure.

Prewarming now defaults to off and preserves saved choices. Diagnostics record
whether it was requested, the last observed Forge stage, and a native heap
sample. The public alpha was built and audited, including verification of both
the Android signature and the Forge package signature. A clean install and
first frame on Xclipse 540 still need device validation. The mitigation applies
to every GPU.

## Menus, presentation and touch

- Android uses the graphics presentation queue by default, recognizes SDL2
  when selecting it, avoids redundant swapchain rebuilds, and preserves copy
  surfaces and the original renderer error during cleanup. Optional VRS
  defaults to off. Frame pacing at 30 Hz recovers after a missed deadline.
- The native pause pages and host in-game menu fill spare borders with a
  lightly blurred, dimmed, olive-gold copy of the game image. The center keeps
  its presentation bounds. The shader was validated for both Vulkan scanout
  paths. The test APK was installed on the Edge 60 and the installer opened.
  Menu appearance and gameplay still need a device check.
- The Edge 60 log recorded 14 writes of a roughly 190–205 MB Vulkan pipeline
  cache while pipelines were being created in gameplay. Android now waits at
  least 60 seconds between snapshots instead of 20, and logs snapshot and
  write times. This reduces cache activity. The cause of individual first-use
  stalls remains open.
- Touch changes include the revised default layout, automatic Zora art,
  customizable shortcuts, inactivity and controller hiding, and menu-only
  controls while the host menu is open. The viewport-relative layout uses the
  short edge for sizes and remaps saved positions when the view resizes. The
  Android and controls unit suites passed, including a resize regression test.
  That last layout change has yet to be installed on the Edge 60.

## Adreno 619 and Turnip experiment

The tested Adreno 619 system driver reported Vulkan 1.1. The game requires
Vulkan 1.2 or later, so the system path stopped before rendering. The launcher
now probes the system driver and distinguishes the standard, compatibility,
and unsupported cases.

The experimental path accepts an optional libadrenotools source checkout with
its submodules through `TRIAEVUM_ADRENOTOOLS_SOURCE`. The checkout tested was
`8fae8ce254dfc1344527e05301e43f37dea2df80`. A developer APK with the matching
Turnip library can request it via the `org.triaevum.android.USE_TURNIP` boolean
intent extra. SDL and NRI then use the same selected loader. Standard launches
use the system driver.

A developer launch selected Turnip on Adreno 619, reported Vulkan 1.3.363
(Mesa 26.3.0-devel, `a995eaeda4`), and reached
`first_frame_presented` at 1605×720. This verifies startup on that setup.
Gameplay stability and automatic compatibility setup remain unverified.
