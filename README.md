# waze-hud

Waze on a BMW's instrument cluster and its head-up display, for the iDrive 7
head unit (MGU), installed from the
[mgu Store](https://github.com/robertnisipeanu/mgu-store).

Waze sends CarPlay no route guidance, so BMW's HUD shows no arrows for it.
This project reads Waze's next turn from the CarPlay picture and sends it to
the car's own navigation interface (SOME/IP `ProjectedModeGuiding`), the way
CarPlay sends Apple Maps' and Google Maps' guidance: **BMW's own HUD arrows
and distance, for Waze**. The CarPlay (or Android Auto) map is shown on the
instrument cluster as well.

| Path | |
|---|---|
| [`native/jap-waze-on-cluster`](native/jap-waze-on-cluster) | the tool: the carplay and aapd patches, the map capture for the cluster, the HUD arrows (`hudguide`) |
| [`apps/jap_waze_cluster`](apps/jap_waze_cluster) | **Waze HUD**, the iDrive app: status, Enable and Disable, Android Auto, Logs, and the choice when another Waze on cluster is installed |
| [`native/cluster-render`](native/cluster-render) | the shared cluster renderer, kept here to build against; installed from its own source |
| [`release`](release) | builds the Store catalog |

## Requirements

- A BMW with an iDrive 7 head unit (MGU, ID7) and CarPlay (Android Auto for
  the cluster map only, where the car supports it).
- The mgu Store with `mgu-deploy` 0.7.11 or later (the Store brings it).
- Two Store sources: this repository and `robertnisipeanu/idrive-apps`,
  which publishes `cluster-render` 0.3.7, the renderer the tool requires.

Tested on a BMW with iDrive 7 (MGU), with an iPhone running Waze over CarPlay.

## Install

1. In the Store, add the GitHub source `robertnisipeanu/idrive-apps` on
   Releases (`robertnisipeanu/idrive-apps:releases`), if it is not there.
2. Add the GitHub source `JustAnotherPU/waze-hud` on Releases
   (`JustAnotherPU/waze-hud:releases`).
3. Install **Waze HUD**. The Store installs what it requires first:
   `cluster-render`, then the tool `jap-waze-on-cluster`.
4. Restart the head unit when the Store says so: the CarPlay patch takes
   effect at the next start.
5. Set **Instrument cluster > Central display area** to **Map view** and
   start a route in Waze.

The HUD shows Waze's arrow (read from the CarPlay card on the centre screen,
else from the arrow on Waze's cluster map). The distance under it needs
glyph templates you make from your own copy of SF Pro
(`native/jap-waze-on-cluster/tools/hud_font_templates.py`; no font ships
here). Without them, the arrow is expected to show without the distance. The drawn-image backend
(`hud_backend = image`) is an optional fallback;
[the tool's README](native/jap-waze-on-cluster/README.md#hud-arrows) has
both.

## If robertnisipeanu's Waze on cluster is installed

Both versions patch CarPlay, so only one can run. This one uses its own ids,
paths and units, so installing it overwrites nothing of the other; while the
other is installed it stays dormant: no patch, no capture, no HUD guidance.
Open **Waze HUD** and a pop-up says the other version is installed and both
patch CarPlay:

- **Remove it and use Waze HUD**: the tool removes the other app and tool
  through the deploy tool (as the Store does, its own removal reverting its
  patch), then turns this one on. Restart the head unit to finish. This
  removal has not been tested on a car yet; removing the other version in
  the Store yourself, then installing this one, is the tested path.
- **Cancel**: nothing changes; this one stays off. The pop-up asks again the
  next time the app opens; the page has the same choice as a button.

`cluster-render` is shared and stays installed.

**Who can ask for it.** The tool's root helper listens on
`/dev/shm/jap-waze-on-cluster/patch.sock` (root:tm_oap_app, 0660), the same
model as the Store's mgu-updater socket: any installed OAP app can ask it to
apply or revert the patch, remove the other Waze on cluster (`takeover`) or
restart the head unit. Install apps only from sources you trust.

## Remove or roll back

In the Store, remove **Waze HUD**, then `jap-waze-on-cluster`; its removal
reverts the CarPlay and Android Auto patches, and the next head-unit start
runs the original programs. `cluster-render` and `mgu-deploy` stay. An update
that does not work can be undone in the Store (Undo update) while the
previous version is kept.

## Not tested yet

- Handing over between Waze and Apple Maps (or Google Maps) during one drive.
- A route in BMW's own navigation while Waze guides on the phone (the HUD
  guidance is meant to give way to it).
- Removing the other Waze on cluster from the pop-up, on the car (tested
  against a fake deploy tool).
- The pop-up's second button on the car (the page button is the fallback).

## Build and test

```sh
make -C native/jap-waze-on-cluster test        # host tests (Node, C++)
make -C native/jap-waze-on-cluster target      # the unit's binary, in Docker
node apps/jap_waze_cluster/tools/test_control.js
node apps/jap_waze_cluster/tools/test_page.js
node release/test/release_test.js
node release/release.js build                  # artifacts and catalog.json in release/out/
```

## License

GPL-3.0-or-later ([`LICENSE`](LICENSE)), unless a file's SPDX header says
otherwise, derived from
[robertnisipeanu/idrive-apps](https://github.com/robertnisipeanu/idrive-apps)
([`NOTICE`](NOTICE) lists the changes). cluster-render's `include/`,
`client/`, `kit/`, `js/cluster_render_client.js` and `PROTOCOL.md` are
MPL-2.0 ([`LICENSES/MPL-2.0.txt`](LICENSES/MPL-2.0.txt)); third-party files
keep their own licenses (the RAMSES and vsomeip interfaces, MPL-2.0; the
Intel Media SDK headers, MIT; Project Nayuki's QR code generator, MIT).
