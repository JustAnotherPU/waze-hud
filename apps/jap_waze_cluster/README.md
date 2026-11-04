# jap_waze_cluster

**Waze HUD**, the HMI app for [jap-waze-on-cluster](../../native/jap-waze-on-cluster), which shows
the CarPlay cluster map (Waze, Apple Maps or Google Maps), or Android Auto's,
on the instrument cluster of an iDrive 7 (MGU) head unit, and Waze's next
turn on BMW's own HUD arrows. Everything is done from the HMI; no
shell is needed.

Install it from this repository's catalog (Store source
`JustAnotherPU/waze-hud:releases`) through the
[mgu Store](https://github.com/robertnisipeanu/mgu-store). It requires
jap-waze-on-cluster 1.0.0, which requires the cluster renderer and writes the
CarPlay patch, and the Android Auto patch where the car supports it, as it
installs; one head-unit start activates them. After that
the map is on the cluster whenever CarPlay streams it, whether or not this
app is open.

Set **Instrument cluster > Central display area** to **Map view**: the Waze
map replaces the stock map while CarPlay's cluster stream shows a map, and
the stock map is back when the stream ends or shows none. Until a navigation
app draws on the cluster (in Waze or Google Maps, once a route starts), the
stock map stays and the page shows `Waiting for the phone's map — start
navigation`. **Off** works too, with an empty centre
instead of the stock map. With Route preview or Assisted Driving View the
cluster's own content takes the centre. After changing the setting, CarPlay
may need reconnecting before the map returns: the change can freeze the
phone's cluster stream, and the page then shows `Map frozen — reconnect
CarPlay` while the stock map (in Off, an empty centre) shows.

To remove, remove the app in the Store, then the Waze tool, which reverts
the patch.

**robertnisipeanu's Waze on cluster installed.** Both patch CarPlay, so the
tool stays off while it is installed (jap-waze-on-cluster README, "Beside
robertnisipeanu's Waze on cluster"). The status says
`Off — Robert Nisipeanu's Waze on cluster is installed`, Enable, Disable and
Android Auto's buttons are gone, and a pop-up (`ST_OnlineApps_JapWazeClusterConflict`,
once each time the app shows) explains it with **Remove it and use Waze HUD**
and **Cancel**. The page's `Remove Robert Nisipeanu's Waze on cluster`
button does the same on a second press within 8 s. Then the status shows
`Removing Robert Nisipeanu's Waze on cluster…`, `Turning Waze HUD on…`, and
`Patch pending — reboot to activate` with `Restart head unit`, or
`Could not remove the other version — <why>`.

## The page

One `LT_State_Wide` page:

| Row | What it does |
|---|---|
| status | the patch and map state: `Patch pending — reboot to activate`, `Removal pending — reboot to finish`, `Original — enable the map`, `Not patched — <reason>`, `Applying patch…`, `Patched — waiting for CarPlay`, `Waiting for the phone's map — start navigation`, `Stream lost — reconnect CarPlay`, `Starting map…`, `Showing: Waze map — 16 frames/s …`, `Map frozen — reconnect CarPlay`, `Map error: …`, `Patch service unavailable (reboot may be needed)` |
| setup | `Set Instrument cluster > Central display area to Map view`, always shown |
| Enable map | applies the patch; shown while it is off, unknown or out of date and no change is pending |
| Disable map | reverts the patch, which turns the map off; shown while patched and no change is pending |
| Android Auto | `Android Auto: patched, 180 dpi[, turn card hidden]`, `off`, `patch pending — reboot to activate`, `removal pending — reboot to finish`, `not supported on this car`, `untested aapd build`, `not on this head unit`, `partly patched`, `applying…`, `another change running`, `try again in a few seconds`, `status unavailable`, `not patched`, `failed` |
| Enable Android Auto | applies its patch; shown while it is off or partly applied, supported and nothing is pending |
| Disable Android Auto | reverts it; shown while patched and nothing is pending |
| Zoom out / Zoom in | the density the phone is told, 10 dpi a press (150-400): out shows more map; shown while patched |
| Hide turn card / Show turn card | crops Google Maps' turn card off the map, or shows it; shown while patched |
| Restart head unit | a normal system reboot; shown only while a written change, CarPlay's or Android Auto's, waits for the next head-unit start |
| Hide BMW turn box | a checkbox: ticked, BMW's turn-by-turn box on the cluster stays hidden while the map shows (the default); unticked, it shows. Shown once the map service reports the setting; a press changes it at once and the service keeps it. `Hide BMW turn box — unavailable`, greyed and unticked, while the cluster renderer keeps the box shown |
| Capture logs (5 min) / Stop capture | a bounded DLT capture for diagnostics; start it, then reconnect CarPlay |
| Logs | opens the Logs page |
| 30 rows | both tools' log lines, newest first, tagged `map:` / `patch:` |

The **Logs** page (`ST_OnlineApps_JapWazeClusterLogs`, a child of the main
page):

| Row | What it does |
|---|---|
| Save logs | saves an archive of the tools' logs, kept journal and status (jap-waze-on-cluster README, "Logs") |
| status | `Saving…`, `Saved <name>`, `<n> saved`, `No logs saved`, `Not enough space`, `Logs too large`, `Save failed`, `Logs unavailable — restart head unit`, `Connecting…` |
| QR code | the download address, for a phone on the car's Wi-Fi hotspot; blank without one |
| address | `http://172.16.222.97:8475/<token>/`, or `Wi-Fi hotspot off` |
| 5 rows | the saved archives, newest first, with their size |

The app holds the logs socket while the Logs page is visible; the logs
service serves the archives while it is held and for 15 minutes after a
save. The QR code is a PNG in `/dev/shm/jap_waze_cluster_qr_<uid>/`
(`src/qr.js`, with Project Nayuki's QR code generator, `src/qrcodegen.js`),
replaced when the address changes and removed when the app shuts down.

`Starting map…` shows while the cluster renderer starts; a map error shows
only when the renderer stays unreachable for 15 s. Before the patch daemon
runs (right after the install), the pending patch is read from
`/dev/shm/jap-waze-on-cluster/activation-pending`.

## The sockets

Unix stream sockets, newline-delimited JSON, group `tm_oap_app`. The app
holds the first two while one of its pages is visible and the third while
the Logs page is, and retries each once a second; the tools keep running
when it closes them.

- **Render** `/dev/shm/jap-waze-on-cluster/ctl.sock`. The app sends
  `{"cmd":"guide","hide":0|1}` (Hide BMW turn box). The tool sends
  `{"ev":"status","state","target","page","reason","upload_cap_kb_per_s","upload_reason","hide_bmw_guidance","guide_disabled"}`
  (`state` idle, starting, showing, frozen or error; `hide_bmw_guidance` 0
  or 1; `guide_disabled` 1 while the cluster renderer keeps the box shown),
  `{"ev":"log","line"}`.
- **Patch daemon** `/dev/shm/jap-waze-on-cluster/patch.sock` (root). The app sends
  `{"cmd":"apply","panel":"high","mode":"avb"}`, `{"cmd":"revert"}`,
  `{"cmd":"aa-apply"}`, `{"cmd":"aa-revert"}`, `{"cmd":"aa-density","value"}`, `{"cmd":"aa-card","value"}`,
  `{"cmd":"reboot"}`, `{"cmd":"capture-start"|"capture-stop"}`. The daemon sends
  `{"ev":"status","state","panel","mode","res","safe","carplay","reason","pending","conflict","aa"}`
  (`aa`: `{"state","reason","conflict","pending","aapd","dpi","diskCard","density","card","densities"}`)
  (`state` original, patched, unknown, busy or error; `carplay: "deferred"`
  while a change waits for the next start), `{"ev":"capture",…}`,
  `{"ev":"reboot-reply","ok","reason"}` and `{"ev":"log","line"}`.
- **Logs** `/dev/shm/jap-waze-on-cluster/logs.sock`; a connection starts the
  logs service. The app sends `{"cmd":"save"}`. The service sends
  `{"ev":"state","url","saving","error","saved","archives":[{"name","size","mtime"}]}`
  on connect and on each change (`url` null without the hotspot's address;
  `error` `no-space`, `too-large` or `failed`).

A patch with another sink, panel, size (not `1088x480`) or safe area (not
`504x368+286+72`) shows `Patched — Enable map to update the patch`.

## Build and test

```sh
node tools/test_control.js           # the socket client against fake sockets
node tools/test_page.js              # the page against a fake SDK and HMI
node tools/build.js                  # jap_waze_cluster@1.0.0.src.tgz
node tools/build.js --sdk <donor>    # also jap_waze_cluster@1.0.0.tgz
```

The app ships as a source package (`mgu-app.json`; `"build": "unit"` in the
catalog). The repository carries no oap-sdk, which is BMW's: the head unit's
deploy tool links the app against the unit's own oap-sdk and takes the
package's entry point from the unit's own packages. To build the OAP tgz
locally, pass a donor bundle with `--sdk` (or `MGU_SDK_DONOR`), for example
the one `mgu-deploy.sh sdk-donor` takes from the unit. The tests need no
donor.

`node tools/fakes.js <render.sock> <patch.sock>` runs both fakes by hand;
Enter starts or stops a fake CarPlay stream.

| Path | |
|---|---|
| `src/jap_waze_cluster.js` | the app |
| `src/waze_cluster_control.js` | the render and patch socket clients and the status logic |
| `src/logs_control.js` | the logs socket client |
| `src/qr.js`, `src/qrcodegen.js` | the QR code PNG; Project Nayuki's generator (MIT, `LICENSE-qrcodegen.txt`) |
| `tools/` | build, fakes, tests, icon |
| `jap_waze_cluster@1.0.0/` | the package tree (manifest, RHMI UI, config) |

## License

GPL-3.0-or-later (`LICENSE`). Third-party files keep their own licenses.
