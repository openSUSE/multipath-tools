# multipath-tools Release Notes

## Backported fixes from upstream 0.9.9 ... 0.10.5

* Updates to the built-in hardware table:
  - add some NVMe storage array (VASTData, Infinidat, HITACHI VSP)
  - add QSAN
  - add EqualLogic PS

* Avoid a possible system hang during shutdown with queueing multipath maps,
  which was introduced in 0.8.8.
* Failed paths should be checked every `polling_interval`. In certain cases,
  this wouldn't happen, because the check interval wasn't reset by multipathd.
* It could happen that multipathd would accidentally release a SCSI persistent
  reservation held by another node. Fix it.
* After manually failing some paths and then reinstating them, sometimes
  the reinstated paths were immediately failed again by multipathd.
* Fix crash in foreign (nvme native multipath) code, present since 0.8.8.
* Fix file descriptor leak in kpartx. This problem existed since 0.4.5.
* Fix memory leak in error code path in libmpathpersist which existed
  since 0.4.9.
* Fix possible out-of-bounds memory access in vector code that existed
  since 0.4.9.
* Fix a possible NULL dereference in the iet prioritizer, existing since
  0.4.9.
* Fix an error check in the nvme foreign library, problem introduced in 0.7.8.

* Updates to the built-in hardware table:
  - Add Quantum devices
  - Enable ALUA for AStor/NeoSapphire
  - Update NFINIDAT/InfiniBox config
  - Fix product blacklist of S/390 devices
  - Add Seagate Lyve
  - Add HITACHI VSP One SDS Block
  - Add SCST (SCSI Target Subsystem for Linux)

* Fix multipathd crash because of invalid path group index value, for example
  if an invalid path device was removed from a map.
  Fixes [#105](https://github.com/opensvc/multipath-tools/issues/105).
  This issue existed since 0.4.5.
* Fix the problem that `group_by_tpg` might be disabled if one or more
  paths were offline during initial configuration.
  This problem exists since 0.9.6.

* Fixed the problem that devices with `no_path_retry fail` and no setting
  for `dev_loss_tmo` might get the `dev_loss_tmo` set to 0, causing the
  device to be deleted immediately in the event of a transport disruption.
  This bug was introduced in 0.9.6.
* Fixed the problem that, if there were multiple maps with deferred failback
  (`failback` value > 0 in `multipath.conf`), some maps might fail back later
  than configured. The problem existed since 0.9.6.
* Removed a warning message that multipathd would print if systemd's
  `WATCHDOG_USEC` environment variable had the value "0", which means that the
  watchdog is simply disabled. This (minor) problem existed since 0.4.9.
* Fixed a memory leak in the nvme foreign library. The bug existed since
  0.7.8.
* Fixed a problem in the marginal path detection algorithm that could cause
  the io error check for a recently failed path to be delayed. This bug
  existed since 0.7.4.

* Fixed bug that caused queueing to be always disabled if flushing a map failed
  (bug introduced in 0.9.8).
* Fixed old mpathpersist bug leading to the error message "configured reservation
  key doesn't match: 0x0" when `reservation_key` was configured in the
  multipaths section of `multipath.conf`
  (Fixes [#92](https://github.com/opensvc/multipath-tools/issues/92)).
* Fixed output of `multipath -t` and `multipath -T` for the options
  `force_sync` and `retrigger_tries`.
  (Fixes [#88](https://github.com/opensvc/multipath-tools/pull/88))
* Added hardware defaults for Huawei storage arrays and XSG1 vendors.
* Fixed minor issues detected by coverity.
* Fixed a minor bug in the config file parser
  (Fixes [#93](https://github.com/opensvc/multipath-tools/pull/93)).

* Fixed map failure counting for `no_path_retry > 0`.

## multipath-tools 0.9.8, 2024/02

### User-Visible Changes

* Socket activation via multipathd.socket has been disabled by default because
  it has undesirable side effects (fixes
  [#76](https://github.com/opensvc/multipath-tools/issues/76), at least partially).
* The restorequeueing CLI command now only enables queueing if disablequeueing
  had been sent before.
* Error messages sent from multipathd to the command line client have been
  improved. The user will now see messages like "map or partition in use" or
  "device not found" instead of just "fail". 
  
### Other Major Changes

* multipathd now tracks the queueing mode of maps in its internal features
  string. This is helpful to ensure that maps have the desired queuing
  status. Without this, it could happen that a map remains in queueing state
  even after the `no_path_retry` timeout has expired.
* multipathd's map flushing code has been reworked to avoid hangs if there are
  no paths but outstanding IO. Thus, if multipathd is running, `multipath -F`
  can now retry map flushing using the daemon, rather than locally.

### Bug Fixes

* A segmentation fault in the 0.9.7 autoresize code has been fixed.
* Fixed a bug introduced in 0.9.6 that had caused map reloads being omitted
  when path priorities changed. 
* Fixed compilation with gcc 14. (Fixes [#80](https://github.com/opensvc/multipath-tools/issues/80))
* Minor fixes for issues detected by coverity.
* Spelling fixes and other minor fixes.

### CI

* Enabled `-D_FILE_OFFSET_BITS=64` to fix issues with emulated 32-bit
  environments in the GitHub CI, so that we can now run our CI in arm/v7.
* Added the check-spelling GitHub action.
* Various improvements and updates for the GitHub CI workflows.

## multipath-tools 0.9.7, 2023/11

### User-Visible Changes

* The options `bindings_file`, `wwids_file` and `prkeys_file`, which were
  deprecated since 0.8.8, have been removed. The path to these files is now
  hard-coded to `$(statedir)` (see below).
* Added `max_retries` config option to limit SCSI retries.
* Added `auto_resize` config option to enable resizing multipath maps automatically.
* Added support for handling FPIN-Li events for FC-NVMe.
  
### Other Major Changes

* Rework of alias selection code:
  - strictly avoid using an alias that is already taken.
  - cache bindings table in memory.
  - write bindings file only if changes have been applied, and watch it with inotify.
  - sort aliases in "alias order" by using length+alphabetical sort, allowing
    more efficient allocation of new aliases

### Bug Fixes

* Avoid that `multipath -d` changes sysfs settings.
* Fix memory and error handling of code using aio in the marginal paths code.
and the directio checker (fixes
[#73](https://github.com/opensvc/multipath-tools/issues/73)).
* Insert compile time settings for paths in man pages correctly.

### Other

* Add new compile-time variable `statedir` which defaults to `/etc/multipath`.
* Add new compile-time variable `etc_prefix` as prefix for config file and config dir.
* Compile-time variable `usr_prefix` now defaults to `/usr` if `prefix` is empty.
* Remove check whether multipath is enabled in systemd `.wants` directories.
* README improvements.

## multipath-tools 0.9.6, 2023/09

### User-Visible Changes

* Added new path grouping policy `group_by_tpg` to group paths by their ALUA
  target port group (TPG).
* Added new configuration parameters `detect_pgpolicy` (default: yes) and
  `detect_pgpolicy_use_tpg` (default: no).
* Add new wildcard `%A` to print target port group in `list paths format` command.
* NVMe devices are now ignored if NVMe native multipath is enabled in the
  kernel.

### Other Major Changes

* Prioritizers now use the same timeout logic as path checkers.
* Reload maps if the path groups aren't properly ordered by priority.
* Improve logic for updating path priorities.
* Avoid paths with unknown priority affecting the priority of their path
  group.

### Bug Fixes

* Fix `max_sectors_kb` for cases where a path is deleted and re-added
  (Fixes [#66](https://github.com/opensvc/multipath-tools/pull/66)).
* Fix handling of `dev_loss_tmo` in cases where it wasn't explicitly
  configured.
* Syntax fixes in udev rules (Fixes [#69](https://github.com/opensvc/multipath-tools/pull/69)).
  
### Other

* Adapt HITACHI/OPEN- config to work with alua and multibus.
* Build system fixes.

## multipath-tools 0.9.5, 2023/04

### User-Visible Changes
  
* Always use directio path checker for Linux TCM (LIO) targets
  (Fixes [#54](https://github.com/opensvc/multipath-tools/issues/54).
* `multipath -u` now checks if path devices are already in use 
  (e.g. mounted), and if so, makes them available to systemd immediately.

### Other Major Changes

* Persistent reservations are now handled consistently. Previously, whether a
  PR key was registered for a path depended on the situation in which the
  path had been first detected by multipathd.

### Bug Fixes

* Make sure that if a map device must be renamed and reloaded, both
  actions actually take place (previously, the map would only be renamed).
* Make sure to always flush IO if a map is resized.
* Avoid incorrectly claiming path devices in `find_multipaths smart` case
  for paths that have no valid WWID or for which `multipath -u` failed.
* Avoid paths failures for ALUA transitioning state
  (fixes [#60]( https://github.com/opensvc/multipath-tools/pull/60).
* Handle persistent reservations correctly even if there are no active paths
  during map creation.
* Make sure all paths are orphaned if maps are removed.
* Avoid error messages for unsupported device designators in VPD pages.
* Fix a memory leak.
* Honor the global option `uxsock_timeout` in libmpathpersist
  (fixes [#45](https://github.com/opensvc/multipath-tools/issues/45)).
* Don't fail for devices lacking INQUIRY properties such as "vendor"
  (fixes [#56](https://github.com/opensvc/multipath-tools/issues/56)).
* Remove `Also=` in `multipathd.socket`
  (fixes [#65](https://github.com/opensvc/multipath-tools/issues/65)).

### CI

* Use Ubuntu 22.04 instead of 18.04.

## multipath-tools 0.9.4, 2022/12

### Bug Fixes

* Verify device-mapper table configuration strings before passing them
  to the kernel.
* Fix failure of `setprstatus`, `unsetprstatus` and `unsetprkey` commands
  sent from libmpathpersist introduced in 0.9.2.
* Fix a memory leak.
* Compilation fixes for some architectures, older compilers, and MUSL libc.
* Fix `show paths format %c` failure for orphan paths
  (fixes [#49](https://github.com/opensvc/multipath-tools/pull/49))

### Build system changes

* Added a simple `autoconf`-like mechanism.
* Use "quiet build" by default, verbose build can be enabled using `make V=1`.
* Reworked the Makefile variables for configuring paths.
* Don't require perl just for installation of man pages.

### CI

* True "multi-architecture" workflows are now possible on GitHub workflows, to
  test compilation and run unit tests on different architectures.
* Containers for test builds are now pulled from ghcr.io rather than from
  docker hub.

### Other

* Updates for the hardware table: PowerMax NVMe, Alletra 5000, FAS/AFF and
  E/EF.
* Documentation fixes.

## multipath-tools 0.9.3, 2022/10

### Bug fixes

* Fix segmentation violation caused by different symbol version numbers in
  libmultipath and libmpathutil 
  (fixes [47](https://github.com/opensvc/multipath-tools/issues/47).

## multipath-tools 0.9.2, 2022/10

### User-Visible Changes
  
* Fix handling of device-mapper `queue_mode` parameter.
* Enforce `queue_mode bio` for NVMe/TCP paths.
  
### Other Major Changes

* Rework the command parsing logic in multipathd (CVE-2022-41974).
* Use `/run` rather than `/dev/shm` (CVE-2022-41973).
* Check transport protocol for NVMe devices.

### Bug Fixes

* Rework feature string handling, fixing bugs for corner cases.
* Fix a race in kpartx partition device creation.
* Fix memory leak in the unix socket listener code.
* Fix a read past end of buffer in the unix socket listener code.
* Fix compilation error with clang 15.

## multipath-tools 0.9.1, 2022/09

### User-Visible Changes

* multipathd doesn't use libreadline any more due to licensing
  conflicts, because readline has changed its license to GPL 3.0,
  which is incompatible with the GPL-2.0-only license of parts of the
  multipath-tools code base.
  Therefore the command line editing feature in multipathd is
  disabled by default. libedit can be used instead of libreadline by
  setting `READLINE=libedit` during compilation. 
  `READLINE=libreadline` can also still be set. Only the new helper program
  *multipathc*, which does not contain GPL-2.0 code, is linked with
  libreadline or libedit. `multipathd -k` now executes `multipathc`.
  Fixes [36](https://github.com/opensvc/multipath-tools/issues/36).
* As part of the work separating code of conflicting licenses, the multipath
  library has been split into `libmultipath` and `libmpathutil`. The latter
  can be linked with GPL-3.0 code without licensing conflicts.
* Speed up start of `multipath -u` and `multipath -U`.
* Speed up seeking for aliases in systems with lots of alias entries.
* Always use the `emc_clariion` checker for Clariion/Unity storage arrays.
  
### Bug Fixes

* Avoid checker thread blocking uevents or other requests for an extended
  amount of time with a huge amount of path devices, by occasionally
  interrupting the checker loop.
* Fix handling the case where a map ended up with no paths while being
  updated.
* Fix a segmentation violation in `list map format` code.
* Fix use-after-free in code handling path WWID changes by sorting the
  alias table.
* Fix timeout handling in unix socket listener code.
* Fix systemd timers in the initramfs.
* Fix `find_multipaths_timeout` for unknown hardware.
* Fix `multipath -ll` output for native NVMe.

### Other

* Cleanup code for sysfs access, and sanitize error handling.
* Separation of public and internal APIs in libmpathpersist.
* Build system fixes.
* Spelling fixes.

## multipath-tools 0.9.0, 2022/06

### User-Visible Changes

 * The properties `dev_loss_tmo`, `eh_deadline`, and `fast_io_fail_tmo` can
   now be set *by protocol*, in the `overrides` → `protocol` section of
   `multipath.conf`.
 * The `config_dir` and `multipath_dir` run-time options, marked deprecated
   since 0.8.8, have been replaced by the build-time options `configdir=` and
   `plugindir=`, respectively.
 * `getuid_callout` is not supported any more.
   
### Other Major Changes
   
 * The uevent filtering and merging code has been re-written to avoid
   artificial delays in uevent processing.
   
### Bug fixes

 * The `delayed_reconfigure` logic has been fixed.

### Other

* hardware table updates.
